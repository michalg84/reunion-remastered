// Improvement (game/improvements.h, autosave), played: a new game with the
// switch on (play_test_setup, so a driver test, not a proof). The
// quick-save key writes on the command centre and is refused on the disk
// screen, which lists the file as a 13th row that SAVE won't write. Then
// meeting 2's own trigger (as test_play_talk) brings the talk: the file
// holds the state of the hour's start, before the trigger. A second new
// game loads the row by clicks, with the trigger still armed, and the talk
// comes again in the same hour. Needs the game data.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "game/dseg.h"
#include "game/improvements.h"
#include "game/le.h"
#include "game/saveload.h"
#include "platform/fs.h"
#include "play/play.h"
#include "play/ui.h"
#include "play/ui_message.h"

static bool ok = true;
#define CHECK(c) do { if (!(c)) { printf("FAILED line %d: %s\n", __LINE__, #c); ok = false; } } while (0)

enum : int {
    SCREEN_COMMAND = 1,
    SCREEN_DISK = 12,
    SCREEN_TALK = 34,
    CMD_DISK = 1,
    CMD_LOAD = 0x2F,
    CMD_SAVE = 0x30,
    CMD_BACK = 24,
    // The port's format: magic and version, then the DOS layout.
    HEADER = 10,
    DATE = HEADER + 0x38A8,  // Game_Year, _Month, _Day, _Hour
};

static char path[2][256];  // the two autosaves'

static struct {
    bool saved, saved_status, refused, refused_status, listed, save_refused;
    bool talk, hour_start;
    char name[20];
    int talk_hour, file_hour;
    bool loaded, talk_again;
} seen;

// Which autosave was written last (0, 1), -1: none.
static int newest(void)
{
    SDL_PathInfo a, b;
    bool has_a = SDL_GetPathInfo(path[0], &a), has_b = SDL_GetPathInfo(path[1], &b);
    if (has_a && has_b)
        return b.modify_time > a.modify_time;
    return has_a ? 0 : has_b ? 1 : -1;
}

// The autosave written last.
static uint8_t *read_autosave(size_t *size)
{
    int n = newest();
    *size = 0;
    return n < 0 ? nullptr : SDL_LoadFile(path[n], size);
}

static void key(SDL_Keycode k)
{
    SDL_Event e = {.key = {.type = SDL_EVENT_KEY_DOWN, .key = k, .down = true}};
    SDL_PushEvent(&e);
    play_wait(2);
    e.key.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    SDL_PushEvent(&e);
    play_wait(4);
}

static bool status_is(const char *s)
{
    char text[64];
    ui_status_text(text, sizeof text);
    return strstr(text, s) != nullptr;
}

static void switch_on(void *unused)
{
    (void)unused;
    improvements.autosave = true;
}

static bool past_midnight(void *unused)
{
    (void)unused;
    return ds.Game_Hour == 1;
}

// Meeting 2's trigger (sim.c ProcessStoryTimers), as test_play_talk sets it
// but an hour later: in two hours MSG 9's box shows, then the talk.
static void arm_talk(void *unused)
{
    (void)unused;
    ds.Intel_Race2Timer2 = 2;
    ds.Intel_Race2Done2 = 0;
    ds.Races_Races[0].Status = 3;       // Jaanosians met
    ds.Research_Products[7].State = 5;  // Communicator done
}

// Closes boxes (MSG 9) until the main loop has entered the talk.
static bool wait_talk(void)
{
    for (int t = 0; t < 3000; t++) {
        if (ds.Game_CurrentScreen == SCREEN_TALK && !ds.Game_ScreenSwitch) {
            play_wait(10);
            return true;
        }
        if (ui_box(nullptr))
            ui_close_box();
        else
            play_wait(1);
    }
    return false;
}

static void open_disk(void)
{
    if (!ui_toolbar_command(CMD_DISK) || !ui_wait_screen(SCREEN_DISK, 2000))
        play_fail("no disk screen");
}

// The row of autosave `n` (0, 1): the 13th or 14th of the disk screen.
static void click_autosave_row(int n)
{
    ui_click(0x40, AUTOSAVE_ROWS_Y + (AUTOSAVE_SLOT + n - 1) * AUTOSAVE_ROW_H + 4);
}

static void first_game(void *unused)
{
    (void)unused;
    ui_new_game();
    if (!play_wait_until(past_midnight, nullptr, 1000))
        play_fail("the clock does not run");
    play_test_setup(switch_on, nullptr);
    ui_settle();

    // The key on the command centre: the game as it is.
    key(SDLK_F11);
    size_t size = 0;
    uint8_t *f = read_autosave(&size);
    seen.saved = f && size > DATE + 8 && memcmp(f, "REUNION\x1A", 8) == 0
                 && rd16le(f + DATE + 6) == ds.Game_Hour;
    if (f)
        memcpy(seen.name, f + HEADER + 1, SDL_min(f[HEADER], 19));
    seen.saved_status = status_is("Game saved");

    // The disk screen: the key is refused, the row listed, SAVE refused.
    open_disk();
    key(SDLK_F11);
    size_t size2 = 0;
    uint8_t *f2 = read_autosave(&size2);
    seen.refused = f && f2 && size2 == size && memcmp(f, f2, size) == 0;
    seen.refused_status = status_is("Can't save now");
    SDL_free(f2);
    const uint8_t *row = ds.SaveLoad_SlotTable + (AUTOSAVE_SLOT - 1) * 0x16;
    seen.listed = row[0] == 1 && memcmp(row + 2, seen.name, strlen(seen.name)) == 0;
    click_autosave_row(0);
    ui_settle();
    play_dump("disk");
    uint64_t since = play_frame();
    ui_toolbar_command(CMD_SAVE);
    for (int t = 0; t < 200 && !ui_box(nullptr); t++)
        play_wait(1);
    if (ui_box(nullptr))
        ui_close_box();
    seen.save_refused = ui_box_seen("can only be loaded", since) != nullptr;
    f2 = read_autosave(&size2);
    seen.save_refused &= f2 && size2 == size && memcmp(f, f2, size) == 0;
    SDL_free(f2);
    SDL_free(f);
    ui_toolbar_command(CMD_BACK);
    ui_wait_screen(SCREEN_COMMAND, 2000);

    // The talk: the file holds the hour's start, before the trigger.
    play_test_setup(arm_talk, nullptr);
    seen.talk = wait_talk();
    seen.talk_hour = ds.Game_Hour;
    f = read_autosave(&size);
    seen.file_hour = f && size > DATE + 8 ? rd16le(f + DATE + 6) : -1;
    seen.hour_start = seen.file_hour == (seen.talk_hour + 23) % 24;
    SDL_free(f);
    play_quit();
}

static void second_game(void *unused)
{
    (void)unused;
    ui_new_game();
    if (!play_wait_until(past_midnight, nullptr, 1000))
        play_fail("the clock does not run");
    play_test_setup(switch_on, nullptr);
    // The talk's file, the one written last.
    int n = newest();
    open_disk();
    click_autosave_row(n);
    ui_settle();
    seen.loaded = n >= 0 && ds.SaveLoad_SelectedSlot == AUTOSAVE_SLOT + n;
    ui_toolbar_command(CMD_LOAD);
    seen.loaded &= ui_wait_screen(SCREEN_COMMAND, 2000) && ds.Game_Hour == seen.file_hour;
    // The hour's start: the trigger still armed.
    seen.loaded &= ds.Intel_Race2Timer2 == 1 && ds.Intel_Race2Done2 == 0;
    seen.talk_again = wait_talk() && ds.Game_Hour == seen.talk_hour;
    play_quit();
}

int main(int argc, char *argv[])
{
    if (argc < 2 || !fs_init(argv[1])) {
        printf("no game data, skipping\n");
        return 77;
    }
    // Never touch the real save directory.
    char dir[128];
    SDL_snprintf(dir, sizeof dir, "/tmp/reunion_test_play_autosave_%d",
                 (int)SDL_GetCurrentTime(nullptr) & 0xFFFF);
    SaveLoad_SetSaveDir(dir);
    SDL_snprintf(path[0], sizeof path[0], "%s/reunion.auto", dir);
    SDL_snprintf(path[1], sizeof path[1], "%s/reunion.auto2", dir);

    PlayResult r = play_run(12345, first_game, nullptr, nullptr);
    printf("first: %s; saved %d status %d \"%s\"; refused %d status %d; listed %d; save refused %d; "
           "talk %d at %d, file %d\n",
           r.failed ? r.failure : "ok", seen.saved, seen.saved_status, seen.name, seen.refused,
           seen.refused_status, seen.listed, seen.save_refused, seen.talk, seen.talk_hour,
           seen.file_hour);
    CHECK(!r.failed);
    CHECK(seen.saved && seen.saved_status && strncmp(seen.name, "Autosave 2927-8-14", 18) == 0);
    CHECK(seen.refused && seen.refused_status);
    CHECK(seen.listed && seen.save_refused);
    CHECK(seen.talk && seen.hour_start);

    r = play_run(12345, second_game, nullptr, nullptr);
    printf("second: %s; loaded %d; talk again %d\n", r.failed ? r.failure : "ok", seen.loaded,
           seen.talk_again);
    CHECK(!r.failed);
    CHECK(seen.loaded && seen.talk_again);

    SDL_RemovePath(path[0]);
    SDL_RemovePath(path[1]);
    SDL_RemovePath(dir);
    printf(ok ? "ok\n" : "FAILED\n");
    return ok ? 0 : 1;
}
