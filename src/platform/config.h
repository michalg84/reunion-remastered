// The port's settings (port menu, F1): reunion.ini in the player's folder
// (platform/playerdir.h), read at start-up and rewritten on every change. Nothing is
// read or written until config_set_path, so tests and scripted runs keep the
// built-in defaults, which are the behaviour without a config file.
#pragma once

#include <stdint.h>

#include "platform/gamedata.h"

typedef enum : int {
    CONFIG_MUSIC_SETUP,  // as SAVE\SETUP.SPD says (the default without a file)
    CONFIG_MUSIC_NONE,
    CONFIG_MUSIC_SOUND_BLASTER,  // the AdLib (FM) tunes; cut-scenes play the modules
    CONFIG_MUSIC_GUS,    // Gravis UltraSound: the ProTracker modules
} ConfigMusic;

typedef enum : int {
    CONFIG_WINDOWED,
    CONFIG_FULLSCREEN,  // exclusive, at fullscreen_width x _height
    CONFIG_DESKTOP,     // borderless at the desktop resolution
} ConfigDisplay;

// [diagnostics] log_level: what goes to logs/reunion-*.log (platform/diag.h).
typedef enum : int {
    CONFIG_LOG_OFF,  // no log file
    CONFIG_LOG_ERROR,
    CONFIG_LOG_WARN,
    CONFIG_LOG_INFO,
    CONFIG_LOG_DEBUG,
} ConfigLogLevel;

// [diagnostics]: reports for finding bugs and tuning the game, written to
// logs/ in the player's folder (platform/diag.h). All off by default: a
// player's game writes nothing. dev.ini (config_load_dev) turns them on for
// development without touching reunion.ini.
typedef struct ConfigDiagnostics {
    bool crash_dumps;    // a report (stack, minidump on Windows) when the game crashes
    bool hang_watchdog;  // a report when the game loop stands still
    int hang_seconds;    // ... for this long
    ConfigLogLevel log_level;
    bool event_log;      // logs/events-*.jsonl: battles, story, daily state (game/evlog.h)
} ConfigDiagnostics;

// [adaptive] level: how hard the adaptive threat plays (game/adaptive.h).
typedef enum : int {
    CONFIG_LEVEL_EASY,
    CONFIG_LEVEL_NORMAL,
    CONFIG_LEVEL_HARD,
} ConfigLevel;

// [adaptive]: the adaptive threat improvement's numbers (game/adaptive.h).
// Percentages are of the original's; the level moves the band of the
// wave-to-player ratio the aliens aim for.
typedef struct ConfigAdaptive {
    ConfigLevel level;
    int band_low;          // % wave / player at Normal: a weaker wave is raised to this
    int band_high;         // ... a stronger one lowered to this (Easy -20, Hard +20 points)
    int scale_min;         // % of the original wave: never below
    int scale_max;         // ... never above (before the bonus for waiting)
    int delay_max;         // % of the wave's own interval it may wait for a weak player
    int delay_bonus;       // % of strength a wave gains per 100 h it waited
    int credits_per_unit;  // credits that count as one Hunter of strength
    int colony_units;      // Hunters of strength a colony counts as
    bool warn_on_launch;   // a message when a wave sets off (free)
    bool warn_intel;       // a message 100-200 h before, with an observatory or satellite at its home
    int contract_min;      // an intelligence contract costs between these, drawn per race
    int contract_max;
} ConfigAdaptive;

enum : int {
    CONFIG_MAX_SWITCHES = 32,
    CONFIG_KEY_SIZE = 40,
    CONFIG_MAX_BINDINGS = 48,
    CONFIG_BINDING_SIZE = 64,
};

// A section of on/off switches by key, in the order first set.
typedef struct ConfigSwitches {
    int count;
    char keys[CONFIG_MAX_SWITCHES][CONFIG_KEY_SIZE];
    bool on[CONFIG_MAX_SWITCHES];
} ConfigSwitches;

// [keys]: an action's keys as written ("Shift+Tab", "Return, Keypad
// Enter"; game/keyplay.h reads them), in the order first set.
typedef struct ConfigBindings {
    int count;
    char actions[CONFIG_MAX_BINDINGS][CONFIG_KEY_SIZE];
    char keys[CONFIG_MAX_BINDINGS][CONFIG_BINDING_SIZE];
} ConfigBindings;

typedef struct Config {
    // [cheats]
    bool freeze_money;
    bool freeze_ore;
    // [sound]
    ConfigMusic music;
    bool effects;
    int music_volume;    // percent
    int effects_volume;  // percent
    // [video]
    ConfigDisplay display;
    int window_width, window_height;
    int fullscreen_width, fullscreen_height;  // 0: the desktop's mode
    float fullscreen_refresh;                 // Hz, 0: any
    // [game]
    bool skip_intro;
    bool colony_stats;  // planet info: the POP row opens the colony's stats
    bool detoxin_rate;  // droids screen and Derrick info: the Derricks' Detoxin rate
    Language language;  // of the CD's games imported, from the next restart (platform/import.h)
    // [fixes] and [improvements]: a switch not listed is on.
    ConfigSwitches fixes;
    ConfigSwitches improvements;
    ConfigBindings keys;
    ConfigDiagnostics diagnostics;
    ConfigAdaptive adaptive;
} Config;

extern Config config;

// Sets the file and loads it (a missing or unreadable file leaves the
// defaults). From then on config_save writes there. Settings that only the
// file can change from SETUP.SPD's choice (the music) get the app's default.
void config_load(const char *path);
// The default path: reunion.ini in the player's folder.
// Free with SDL_free; nullptr if SDL has no pref path.
char *config_default_path(void);
// Writes the file if config_load set one. False on a write error.
bool config_save(void);
// Back to the built-in defaults; forgets the path and dev.ini (tests).
void config_reset(void);

// The development overlay: dev.ini in the player's folder, read after
// reunion.ini and never written. Only its [diagnostics] count; they replace
// reunion.ini's for this run, and config_save keeps writing reunion.ini's.
// `all_on` (--dev, REUNION_DEV=1) first turns every diagnostic on, as if
// the file said so. False if neither the file nor all_on applied.
bool config_load_dev(const char *path, bool all_on);
// The default path: dev.ini in the player's folder. Free with SDL_free.
char *config_dev_path(void);
// dev.ini (or --dev) set the diagnostics: the port menu shows them but
// doesn't change them.
bool config_dev_active(void);

// A fix's or an improvement's switch: on unless the file says off.
bool config_fix(const char *key);
void config_set_fix(const char *key, bool on);
bool config_improvement(const char *key);
void config_set_improvement(const char *key, bool on);

// An action's keys as the file says, or nullptr if it doesn't list it.
const char *config_binding(const char *action);
void config_set_binding(const char *action, const char *keys);

// Names as written in the file.
const char *config_music_name(ConfigMusic music);
const char *config_log_level_name(ConfigLogLevel level);
