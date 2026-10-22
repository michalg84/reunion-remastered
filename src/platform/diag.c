#include "platform/diag.h"

#include <SDL3/SDL.h>

#include "platform/config.h"
#include "platform/playerdir.h"

#include <stdio.h>
#if defined(_WIN32)
#include <share.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <signal.h>
#elif !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#if defined(__GLIBC__) || defined(__APPLE__)
#include <execinfo.h>
#define DIAG_BACKTRACE 1
#endif
#endif

enum : int {
    MAX_FRAMES = 64,
    MAX_HANGS = 5,   // reports a run
    REPORT_SIZE = 16384,
    PATH_SIZE = 1024,
};

static bool started;
static char *logs_dir;  // created when something is written
static char stamp[32];  // the run's start: 20261022-171100
static SDL_AtomicInt beat;  // SDL_GetTicks() of the last heartbeat, in ms
static void (*describe_state)(char *out, size_t size);

// --- Files --------------------------------------------------------------------

const char *diag_logs_dir(void)
{
    if (!logs_dir) {
        logs_dir = playerdir_path("logs/");
        if (logs_dir && !SDL_CreateDirectory(logs_dir)) {
            SDL_free(logs_dir);
            logs_dir = nullptr;
        }
    }
    return logs_dir;
}

// logs/<prefix>-STAMP<suffix>; false without a folder.
static bool log_path(char *out, size_t size, const char *prefix, const char *suffix)
{
    const char *dir = diag_logs_dir();
    if (!dir)
        return false;
    SDL_snprintf(out, size, "%s%s-%s%s", dir, prefix, stamp, suffix);
    return true;
}

static void set_stamp(void)
{
    SDL_Time now;
    SDL_DateTime dt;
    if (SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &dt, true))
        SDL_snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d", dt.year, dt.month, dt.day,
                     dt.hour, dt.minute, dt.second);
    else
        SDL_strlcpy(stamp, "run", sizeof stamp);
}

// "2026-10-22 17:11:03.250"
static void time_text(char *out, size_t size)
{
    SDL_Time now;
    SDL_DateTime dt;
    if (SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &dt, true))
        SDL_snprintf(out, size, "%04d-%02d-%02d %02d:%02d:%02d.%03d", dt.year, dt.month, dt.day,
                     dt.hour, dt.minute, dt.second, dt.nanosecond / 1000000);
    else
        SDL_strlcpy(out, "?", size);
}

// Opens a file for appending that others may read while the game runs
// (SDL's files are locked on Windows). The path is UTF-8.
static FILE *open_append(const char *path)
{
#if defined(_WIN32)
    wchar_t wide[PATH_SIZE];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, PATH_SIZE))
        return nullptr;
    return _wfsopen(wide, L"ab", _SH_DENYNO);
#else
    return fopen(path, "a");
#endif
}

// --- Reports ------------------------------------------------------------------
// Crash and hang reports are put together in a static buffer without the
// heap (which a crash may have broken, or the frozen thread may hold).

static char report[REPORT_SIZE];
static size_t report_len;

static void report_start(void)
{
    report_len = 0;
    report[0] = '\0';
}

SDL_PRINTF_VARARG_FUNC(1) static void add(SDL_PRINTF_FORMAT_STRING const char *fmt, ...)
{
    if (report_len >= sizeof report - 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = SDL_vsnprintf(report + report_len, sizeof report - report_len, fmt, ap);
    va_end(ap);
    if (n > 0)
        report_len = SDL_min(report_len + (size_t)n, sizeof report - 1);
}

static void add_header(const char *what)
{
    char when[40];
    time_text(when, sizeof when);
    add("Reunion %s at %s\n", what, when);
    if (describe_state) {
        char state[256];
        state[0] = '\0';
        describe_state(state, sizeof state);
        add("Game: %s\n", state);
    }
    add("Ticks: %llu ms since start, last heartbeat %d ms ago\n",
        (unsigned long long)SDL_GetTicks(), (int)((uint32_t)SDL_GetTicks() - (uint32_t)SDL_GetAtomicInt(&beat)));
}

#if defined(_WIN32)

static HANDLE main_thread;
static LPTOP_LEVEL_EXCEPTION_FILTER previous_filter;
static bool crash_installed;
typedef BOOL(WINAPI *MiniDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                 PMINIDUMP_EXCEPTION_INFORMATION,
                                 PMINIDUMP_USER_STREAM_INFORMATION,
                                 PMINIDUMP_CALLBACK_INFORMATION);
static MiniDumpFn minidump;
static char crash_txt[PATH_SIZE], crash_dmp[PATH_SIZE];

static void write_report(const char *path, bool append)
{
    HANDLE f = CreateFileA(path, append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD written;
    WriteFile(f, report, (DWORD)report_len, &written, nullptr);
    CloseHandle(f);
}

// The return addresses of the stack in `ctx`, from the x64 unwind data
// (GCC emits it for every function). Uses no heap and no loader lock.
static int walk(const CONTEXT *ctx, DWORD64 *out, int max)
{
    int n = 0;
#if defined(__x86_64__) || defined(_M_X64)
    CONTEXT c = *ctx;
    while (n < max && c.Rip) {
        out[n++] = c.Rip;
        DWORD64 base;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (fn) {
            PVOID handler_data;
            DWORD64 frame;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &handler_data, &frame, nullptr);
        } else {
            // A leaf function: the return address is on top of the stack.
            if (!c.Rsp || (c.Rsp & 7))
                break;
            c.Rip = *(DWORD64 *)c.Rsp;
            c.Rsp += 8;
        }
    }
#else
    (void)max;
    (void)ctx;
    (void)out;
#endif
    return n;
}

// "  #3 0x00007ff6a1b2c3d4 reunion.exe+0x2c3d4"
static void add_frames(const DWORD64 *frames, int n)
{
    for (int i = 0; i < n; i++) {
        HMODULE m = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                   | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)frames[i], &m)
            && m && GetModuleFileNameA(m, name, sizeof name)) {
            const char *base = name;
            for (const char *p = name; *p; p++)
                if (*p == '\\' || *p == '/')
                    base = p + 1;
            add("  #%-2d 0x%016llx %s+0x%llx\n", i, (unsigned long long)frames[i], base,
                (unsigned long long)(frames[i] - (DWORD64)(uintptr_t)m));
        } else {
            add("  #%-2d 0x%016llx ?\n", i, (unsigned long long)frames[i]);
        }
    }
    add("Symbols: python tools/symbolize.py <this file>\n");
}

static void write_minidump(const char *path, EXCEPTION_POINTERS *ep)
{
    if (!minidump || !path[0])
        return;
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    MINIDUMP_EXCEPTION_INFORMATION info = {GetCurrentThreadId(), ep, FALSE};
    minidump(GetCurrentProcess(), GetCurrentProcessId(), f,
             (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
             ep ? &info : nullptr, nullptr, nullptr);
    CloseHandle(f);
}

static const char *exception_name(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    default: return "exception";
    }
}

static void crash_report(const char *what, const CONTEXT *ctx, EXCEPTION_POINTERS *ep)
{
    report_start();
    add_header("crash");
    add("Cause: %s\n", what);
    if (ep && ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
        && ep->ExceptionRecord->NumberParameters >= 2)
        add("Access: %s 0x%llx\n", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
            (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    DWORD64 frames[MAX_FRAMES];
    add("Stack of the crashed thread:\n");
    add_frames(frames, walk(ctx, frames, MAX_FRAMES));
    write_report(crash_txt, false);
    write_minidump(crash_dmp, ep);
}

static LONG WINAPI on_exception(EXCEPTION_POINTERS *ep)
{
    static volatile LONG entered;
    if (InterlockedExchange(&entered, 1) == 0) {
        char what[96];
        SDL_snprintf(what, sizeof what, "%s (0x%08lx) at 0x%p",
                     exception_name(ep->ExceptionRecord->ExceptionCode),
                     (unsigned long)ep->ExceptionRecord->ExceptionCode,
                     ep->ExceptionRecord->ExceptionAddress);
        crash_report(what, ep->ContextRecord, ep);
    }
    return previous_filter ? previous_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

static void on_abort(int sig)
{
    (void)sig;
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    crash_report("abort()", &ctx, nullptr);
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}

static void crash_install(bool on)
{
    if (on == crash_installed)
        return;
    if (on) {
        if (!log_path(crash_txt, sizeof crash_txt, "crash", ".txt"))
            return;
        log_path(crash_dmp, sizeof crash_dmp, "crash", ".dmp");
        if (!minidump) {
            HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
            if (dbghelp)
                minidump = (MiniDumpFn)(void (*)(void))GetProcAddress(dbghelp, "MiniDumpWriteDump");
        }
        previous_filter = SetUnhandledExceptionFilter(on_exception);
        signal(SIGABRT, on_abort);
    } else {
        SetUnhandledExceptionFilter(previous_filter);
        signal(SIGABRT, SIG_DFL);
    }
    crash_installed = on;
}

static void remember_main_thread(void)
{
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
}

// The main thread's stack, taken while it is suspended (nothing that may
// lock between SuspendThread and ResumeThread).
static void add_main_stack(void)
{
    DWORD64 frames[MAX_FRAMES];
    int n = 0;
    if (main_thread && SuspendThread(main_thread) != (DWORD)-1) {
        CONTEXT ctx = {.ContextFlags = CONTEXT_FULL};
        if (GetThreadContext(main_thread, &ctx))
            n = walk(&ctx, frames, MAX_FRAMES);
        ResumeThread(main_thread);
    }
    add("Stack of the main thread:\n");
    add_frames(frames, n);
}

static void write_hang(const char *path, const char *dmp)
{
    write_report(path, false);
    if (config.diagnostics.crash_dumps)
        write_minidump(dmp, nullptr);
}

#elif !defined(__EMSCRIPTEN__)

static pthread_t main_thread;
static bool crash_installed;
static char crash_txt[PATH_SIZE];
static const int CRASH_SIGNALS[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};

static void write_report_fd(int fd)
{
    size_t done = 0;
    while (done < report_len) {
        ssize_t n = write(fd, report + done, report_len - done);
        if (n <= 0)
            break;
        done += (size_t)n;
    }
}

static void on_signal(int sig)
{
    static volatile sig_atomic_t entered;
    if (!entered) {
        entered = 1;
        report_start();
        add_header("crash");
        add("Cause: signal %d\nStack of the crashed thread:\n", sig);
        int fd = open(crash_txt, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write_report_fd(fd);
#ifdef DIAG_BACKTRACE
            void *frames[MAX_FRAMES];
            backtrace_symbols_fd(frames, backtrace(frames, MAX_FRAMES), fd);
#endif
            close(fd);
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void crash_install(bool on)
{
    if (on == crash_installed)
        return;
    if (on && !log_path(crash_txt, sizeof crash_txt, "crash", ".txt"))
        return;
    for (size_t i = 0; i < SDL_arraysize(CRASH_SIGNALS); i++)
        signal(CRASH_SIGNALS[i], on ? on_signal : SIG_DFL);
    crash_installed = on;
}

static void remember_main_thread(void)
{
    main_thread = pthread_self();
}

#ifdef DIAG_BACKTRACE
// The watchdog asks the main thread for its stack with SIGUSR2.
static void *hang_frames[MAX_FRAMES];
static volatile sig_atomic_t hang_frame_count = -1;

static void on_stack_request(int sig)
{
    (void)sig;
    hang_frame_count = backtrace(hang_frames, MAX_FRAMES);
}
#endif

static void add_main_stack(void)
{
    add("Stack of the main thread:\n");
}

static void write_hang(const char *path, const char *dmp)
{
    (void)dmp;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    write_report_fd(fd);
#ifdef DIAG_BACKTRACE
    hang_frame_count = -1;
    signal(SIGUSR2, on_stack_request);
    if (pthread_kill(main_thread, SIGUSR2) == 0) {
        for (int i = 0; i < 100 && hang_frame_count < 0; i++)
            SDL_Delay(10);
        if (hang_frame_count > 0)
            backtrace_symbols_fd(hang_frames, hang_frame_count, fd);
    }
#endif
    close(fd);
}

#endif

// --- The watchdog -------------------------------------------------------------

#ifndef __EMSCRIPTEN__
static SDL_Thread *watchdog;
static SDL_AtomicInt watchdog_stop;

static void append_line(const char *path, const char *text)
{
    FILE *f = open_append(path);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static int SDLCALL watch(void *unused)
{
    (void)unused;
    int hangs = 0;
    bool hung = false;
    uint32_t hung_since = 0;
    char path[PATH_SIZE], dmp[PATH_SIZE];
    while (!SDL_GetAtomicInt(&watchdog_stop)) {
        SDL_Delay(250);
        uint32_t last = (uint32_t)SDL_GetAtomicInt(&beat);
        uint32_t idle = (uint32_t)SDL_GetTicks() - last;
        if (!hung && idle > (uint32_t)config.diagnostics.hang_seconds * 1000u) {
            hung = true;
            hung_since = last;
            if (++hangs > MAX_HANGS)
                continue;
            char txt_suffix[16], dmp_suffix[16];
            SDL_snprintf(txt_suffix, sizeof txt_suffix, "-%d.txt", hangs);
            SDL_snprintf(dmp_suffix, sizeof dmp_suffix, "-%d.dmp", hangs);
            if (!log_path(path, sizeof path, "hang", txt_suffix)
                || !log_path(dmp, sizeof dmp, "hang", dmp_suffix))
                continue;
            report_start();
            add_header("hang: the game loop stopped");
            add_main_stack();
            write_hang(path, dmp);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Watchdog: no frame for %u ms, report in %s",
                        idle, path);
        } else if (hung && last != hung_since) {
            hung = false;
            char line[128];
            SDL_snprintf(line, sizeof line, "\nResumed after %u ms.\n", last - hung_since);
            if (hangs <= MAX_HANGS)
                append_line(path, line);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Watchdog: the game went on after %u ms",
                        last - hung_since);
        }
    }
    return 0;
}

static void watchdog_run(bool on)
{
    if (on && !watchdog) {
        diag_heartbeat();
        SDL_SetAtomicInt(&watchdog_stop, 0);
        watchdog = SDL_CreateThread(watch, "watchdog", nullptr);
    } else if (!on && watchdog) {
        SDL_SetAtomicInt(&watchdog_stop, 1);
        SDL_WaitThread(watchdog, nullptr);
        watchdog = nullptr;
    }
}
#endif

void diag_heartbeat(void)
{
    SDL_SetAtomicInt(&beat, (int)(uint32_t)SDL_GetTicks());
}

void diag_set_context(void (*describe)(char *out, size_t size))
{
    describe_state = describe;
}

// --- The log file -------------------------------------------------------------

static FILE *log_file;
static SDL_Mutex *log_lock;
static SDL_LogOutputFunction previous_output;
static void *previous_userdata;

static void SDLCALL log_output(void *userdata, int category, SDL_LogPriority priority,
                               const char *message)
{
    static const char *const names[] = {"", "TRACE", "VERBOSE", "DEBUG", "INFO",
                                        "WARN", "ERROR", "CRITICAL"};
    (void)userdata;
    SDL_LockMutex(log_lock);
    if (log_file) {
        char when[40];
        time_text(when, sizeof when);
        const char *name = priority < (int)SDL_arraysize(names) ? names[priority] : "?";
        fprintf(log_file, "%s %-5s [%d] %s\n", when, name, category, message);
        fflush(log_file);
    }
    SDL_UnlockMutex(log_lock);
    if (previous_output)
        previous_output(previous_userdata, category, priority, message);
}

static void log_apply(ConfigLogLevel level)
{
    static const SDL_LogPriority priorities[] = {
        [CONFIG_LOG_ERROR] = SDL_LOG_PRIORITY_ERROR,
        [CONFIG_LOG_WARN] = SDL_LOG_PRIORITY_WARN,
        [CONFIG_LOG_INFO] = SDL_LOG_PRIORITY_INFO,
        [CONFIG_LOG_DEBUG] = SDL_LOG_PRIORITY_DEBUG,
    };
    if (level == CONFIG_LOG_OFF) {
        if (log_file) {
            SDL_SetLogOutputFunction(previous_output, previous_userdata);
            SDL_ResetLogPriorities();
            SDL_LockMutex(log_lock);
            fclose(log_file);
            log_file = nullptr;
            SDL_UnlockMutex(log_lock);
        }
        return;
    }
    if (!log_file) {
        char path[PATH_SIZE];
        if (!log_path(path, sizeof path, "reunion", ".log"))
            return;
        if (!log_lock)
            log_lock = SDL_CreateMutex();
        log_file = open_append(path);
        if (!log_file)
            return;
        SDL_GetLogOutputFunction(&previous_output, &previous_userdata);
        SDL_SetLogOutputFunction(log_output, nullptr);
    }
    SDL_SetLogPriorities(priorities[level]);
}

// --- The event log ------------------------------------------------------------

static FILE *events;

static void events_apply(bool on)
{
    if (on && !events) {
        char path[PATH_SIZE];
        if (log_path(path, sizeof path, "events", ".jsonl"))
            events = open_append(path);
    } else if (!on && events) {
        fclose(events);
        events = nullptr;
    }
}

bool diag_events_on(void)
{
    return events != nullptr;
}

void diag_event_line(const char *json)
{
    if (!events)
        return;
    fputs(json, events);
    fputc('\n', events);
    fflush(events);
}

// --- Start and changes --------------------------------------------------------

void diag_apply(void)
{
    if (!started)
        return;
    const ConfigDiagnostics *d = &config.diagnostics;
    log_apply(d->log_level);
#ifndef __EMSCRIPTEN__
    crash_install(d->crash_dumps);
    watchdog_run(d->hang_watchdog);
#endif
    events_apply(d->event_log);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Diagnostics: crash reports %s, watchdog %s (%d s), log %s, events %s%s",
                d->crash_dumps ? "on" : "off", d->hang_watchdog ? "on" : "off", d->hang_seconds,
                config_log_level_name(d->log_level), d->event_log ? "on" : "off",
                config_dev_active() ? " (dev.ini)" : "");
}

void diag_init(void)
{
    if (started)
        return;
    started = true;
    set_stamp();
    diag_heartbeat();
#ifndef __EMSCRIPTEN__
    remember_main_thread();
#endif
    diag_apply();
}

void diag_shutdown(void)
{
    if (!started)
        return;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Diagnostics: the game ends");
#ifndef __EMSCRIPTEN__
    watchdog_run(false);
#endif
    events_apply(false);
    log_apply(CONFIG_LOG_OFF);
    started = false;
}
