// Diagnostics ([diagnostics] in reunion.ini, platform/config.h): reports for
// finding bugs and tuning the game, in logs/ of the player's folder.
//
//   reunion-STAMP.log    SDL's log at log_level and up, with times
//   crash-STAMP.txt      a crash: the exception or signal, the stack, the
//                        game's state; crash-STAMP.dmp a minidump (Windows)
//   hang-STAMP.txt       the game loop stood still for hang_seconds: the main
//                        thread's stack, the game's state, and when it went on
//   events-STAMP.jsonl   game events, one JSON object a line (game/evlog.h)
//
// STAMP is the run's start time. Stacks are addresses (the port is built
// with GCC, whose debug info Windows' tools don't read):
// tools/symbolize.py turns them into functions and lines.
//
// Everything is off by default; diag_apply follows the config.
#pragma once

#include <stddef.h>

// Starts what config.diagnostics asks for. Call once, after the config.
void diag_init(void);
// After a change of config.diagnostics (the port menu).
void diag_apply(void);
// Closes the files and stops the watchdog.
void diag_shutdown(void);

// The game loop is alive: timing_wait_retrace and the loops that wait
// without it (the port menu, the import screen) call it.
void diag_heartbeat(void);

// Writes a line about the game's state ("screen 7, 2263-05-12 14:00") for
// crash and hang reports. Called from the crash handler or the watchdog's
// thread: it must only read memory.
void diag_set_context(void (*describe)(char *out, size_t size));

// The event log is on (config.diagnostics.event_log).
bool diag_events_on(void);
// Appends one line to the event log (no newline in `json`).
void diag_event_line(const char *json);

// logs/ in the player's folder, ending in a separator; nullptr without one.
const char *diag_logs_dir(void);
