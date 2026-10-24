#include "platform/config.h"

#include <SDL3/SDL.h>

#include "platform/playerdir.h"

static constexpr Config DEFAULTS = {
    .music = CONFIG_MUSIC_SETUP,
    .effects = true,
    .music_volume = 100,
    .effects_volume = 100,
    .display = CONFIG_WINDOWED,
    .window_width = 1280,  // 320x200 shown 4:3, times 4
    .window_height = 960,
    .colony_stats = true,
    .detoxin_rate = true,
    .language = LANGUAGE_ENGLISH,
    .autosave_minutes = 10,
    .diagnostics = {.hang_seconds = 3, .log_level = CONFIG_LOG_OFF},
    .adaptive = {.level = CONFIG_LEVEL_NORMAL, .band_low = 80, .band_high = 120, .scale_min = 70,
                 .scale_max = 120, .delay_max = 40, .delay_bonus = 4, .credits_per_unit = 5000,
                 .colony_units = 3, .warn_on_launch = true, .warn_intel = true,
                 .contract_min = 50000, .contract_max = 80000, .raid_min = 800, .raid_max = 1200,
                 .raid_gap = 200, .raid_chase_min = 100, .raid_chase_max = 200, .blockade_max = 360},
};

Config config = DEFAULTS;

static char *path;
// dev.ini is in force: reunion.ini's own diagnostics, which config_save writes.
static bool dev_active;
static ConfigDiagnostics file_diagnostics;
// Improvements dev.ini set for this run: their value in reunion.ini (dev_had:
// whether the file had the key), which config_save keeps writing.
static ConfigSwitches dev_prev;
static bool dev_had[CONFIG_MAX_SWITCHES];

static const char *const MUSIC_NAMES[] = {"setup", "none", "soundblaster", "gus"};
static const char *const DISPLAY_NAMES[] = {"off", "on", "desktop"};
static const char *const FILTER_NAMES[] = {"sharp", "smooth", "scale2x", "crt"};
static const char *const LEVEL_NAMES[] = {"easy", "normal", "hard"};
static const char *const LOG_LEVEL_NAMES[] = {"off", "error", "warn", "info", "debug"};

const char *config_log_level_name(ConfigLogLevel level)
{
    return LOG_LEVEL_NAMES[level];
}

const char *config_music_name(ConfigMusic music)
{
    return MUSIC_NAMES[music];
}

void config_reset(void)
{
    config = DEFAULTS;
    SDL_free(path);
    path = nullptr;
    dev_active = false;
    dev_prev.count = 0;
}

char *config_default_path(void)
{
    return playerdir_path("reunion.ini");
}

char *config_dev_path(void)
{
    return playerdir_path("dev.ini");
}

bool config_dev_active(void)
{
    return dev_active;
}

static int switch_index(const ConfigSwitches *sw, const char *key)
{
    for (int i = 0; i < sw->count; i++)
        if (SDL_strcmp(sw->keys[i], key) == 0)
            return i;
    return -1;
}

static bool switch_on(const ConfigSwitches *sw, const char *key)
{
    int i = switch_index(sw, key);
    return i < 0 || sw->on[i];
}

static void set_switch(ConfigSwitches *sw, const char *key, bool on)
{
    int i = switch_index(sw, key);
    if (i < 0) {
        if (sw->count == CONFIG_MAX_SWITCHES || SDL_strlen(key) >= CONFIG_KEY_SIZE)
            return;
        i = sw->count++;
        SDL_strlcpy(sw->keys[i], key, CONFIG_KEY_SIZE);
    }
    sw->on[i] = on;
}

bool config_fix(const char *key)
{
    return switch_on(&config.fixes, key);
}

void config_set_fix(const char *key, bool on)
{
    set_switch(&config.fixes, key, on);
}

bool config_improvement(const char *key)
{
    return switch_on(&config.improvements, key);
}

void config_set_improvement(const char *key, bool on)
{
    set_switch(&config.improvements, key, on);
}

static int binding_index(const char *action)
{
    for (int i = 0; i < config.keys.count; i++)
        if (SDL_strcmp(config.keys.actions[i], action) == 0)
            return i;
    return -1;
}

const char *config_binding(const char *action)
{
    int i = binding_index(action);
    return i < 0 ? nullptr : config.keys.keys[i];
}

void config_set_binding(const char *action, const char *keys)
{
    int i = binding_index(action);
    if (i < 0) {
        if (config.keys.count == CONFIG_MAX_BINDINGS || SDL_strlen(action) >= CONFIG_KEY_SIZE)
            return;
        i = config.keys.count++;
        SDL_strlcpy(config.keys.actions[i], action, CONFIG_KEY_SIZE);
    }
    SDL_strlcpy(config.keys.keys[i], keys, CONFIG_BINDING_SIZE);
}

// --- Reading ------------------------------------------------------------------

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *end = s + SDL_strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        *--end = '\0';
    return s;
}

static bool parse_bool(const char *v, bool *out)
{
    static const char *const yes[] = {"on", "true", "yes", "1"};
    static const char *const no[] = {"off", "false", "no", "0"};
    for (int i = 0; i < 4; i++) {
        if (SDL_strcasecmp(v, yes[i]) == 0) {
            *out = true;
            return true;
        }
        if (SDL_strcasecmp(v, no[i]) == 0) {
            *out = false;
            return true;
        }
    }
    return false;
}

static void parse_int(const char *v, int *out, int lo, int hi)
{
    char *end;
    long n = SDL_strtol(v, &end, 10);
    if (end != v && !*end && n >= lo && n <= hi)
        *out = (int)n;
}

static void parse_name(const char *v, int *out, const char *const *names, int count)
{
    for (int i = 0; i < count; i++)
        if (SDL_strcasecmp(v, names[i]) == 0)
            *out = i;
}

static void set_value(const char *section, const char *key, const char *v)
{
    bool b;
    if (SDL_strcasecmp(section, "fixes") == 0) {
        if (parse_bool(v, &b))
            config_set_fix(key, b);
        return;
    }
    if (SDL_strcasecmp(section, "improvements") == 0) {
        if (parse_bool(v, &b))
            config_set_improvement(key, b);
        return;
    }
    if (SDL_strcasecmp(section, "keys") == 0) {
        config_set_binding(key, v);
        return;
    }
#define IS(s, k) (SDL_strcasecmp(section, s) == 0 && SDL_strcasecmp(key, k) == 0)
    if (IS("cheats", "freeze_money") && parse_bool(v, &b))
        config.freeze_money = b;
    else if (IS("cheats", "freeze_ore") && parse_bool(v, &b))
        config.freeze_ore = b;
    else if (IS("sound", "music")) {
        parse_name(v, (int *)&config.music, MUSIC_NAMES, (int)SDL_arraysize(MUSIC_NAMES));
        if (SDL_strcasecmp(v, "adlib") == 0)  // the setting's earlier name
            config.music = CONFIG_MUSIC_SOUND_BLASTER;
    } else if (IS("sound", "effects") && parse_bool(v, &b))
        config.effects = b;
    else if (IS("sound", "music_volume"))
        parse_int(v, &config.music_volume, 0, 100);
    else if (IS("sound", "effects_volume"))
        parse_int(v, &config.effects_volume, 0, 100);
    else if (IS("video", "fullscreen"))
        parse_name(v, (int *)&config.display, DISPLAY_NAMES, (int)SDL_arraysize(DISPLAY_NAMES));
    else if (IS("video", "filter"))
        parse_name(v, (int *)&config.filter, FILTER_NAMES, (int)SDL_arraysize(FILTER_NAMES));
    else if (IS("video", "window_width"))
        parse_int(v, &config.window_width, 320, 16384);
    else if (IS("video", "window_height"))
        parse_int(v, &config.window_height, 240, 16384);
    else if (IS("video", "fullscreen_width"))
        parse_int(v, &config.fullscreen_width, 0, 16384);
    else if (IS("video", "fullscreen_height"))
        parse_int(v, &config.fullscreen_height, 0, 16384);
    else if (IS("video", "fullscreen_refresh"))
        config.fullscreen_refresh = SDL_clamp((float)SDL_atof(v), 0.0f, 1000.0f);
    else if (IS("game", "skip_intro") && parse_bool(v, &b))
        config.skip_intro = b;
    else if (IS("game", "colony_stats") && parse_bool(v, &b))
        config.colony_stats = b;
    else if (IS("game", "detoxin_rate") && parse_bool(v, &b))
        config.detoxin_rate = b;
    else if (IS("game", "language") && gamedata_language_from_name(v) < LANGUAGE_COUNT)
        config.language = gamedata_language_from_name(v);
    else if (IS("game", "autosave_minutes"))
        parse_int(v, &config.autosave_minutes, 0, 120);
    else if (IS("diagnostics", "crash_dumps") && parse_bool(v, &b))
        config.diagnostics.crash_dumps = b;
    else if (IS("diagnostics", "hang_watchdog") && parse_bool(v, &b))
        config.diagnostics.hang_watchdog = b;
    else if (IS("diagnostics", "hang_seconds"))
        parse_int(v, &config.diagnostics.hang_seconds, 1, 600);
    else if (IS("diagnostics", "log_level"))
        parse_name(v, (int *)&config.diagnostics.log_level, LOG_LEVEL_NAMES,
                   (int)SDL_arraysize(LOG_LEVEL_NAMES));
    else if (IS("diagnostics", "event_log") && parse_bool(v, &b))
        config.diagnostics.event_log = b;
    else if (IS("adaptive", "level"))
        parse_name(v, (int *)&config.adaptive.level, LEVEL_NAMES, (int)SDL_arraysize(LEVEL_NAMES));
    else if (IS("adaptive", "band_low"))
        parse_int(v, &config.adaptive.band_low, 10, 1000);
    else if (IS("adaptive", "band_high"))
        parse_int(v, &config.adaptive.band_high, 10, 1000);
    else if (IS("adaptive", "scale_min"))
        parse_int(v, &config.adaptive.scale_min, 10, 300);
    else if (IS("adaptive", "scale_max"))
        parse_int(v, &config.adaptive.scale_max, 10, 300);
    else if (IS("adaptive", "delay_max"))
        parse_int(v, &config.adaptive.delay_max, 0, 200);
    else if (IS("adaptive", "delay_bonus"))
        parse_int(v, &config.adaptive.delay_bonus, 0, 50);
    else if (IS("adaptive", "credits_per_unit"))
        parse_int(v, &config.adaptive.credits_per_unit, 100, 1000000);
    else if (IS("adaptive", "colony_units"))
        parse_int(v, &config.adaptive.colony_units, 0, 100);
    else if (IS("adaptive", "warn_on_launch") && parse_bool(v, &b))
        config.adaptive.warn_on_launch = b;
    else if (IS("adaptive", "warn_intel") && parse_bool(v, &b))
        config.adaptive.warn_intel = b;
    else if (IS("adaptive", "contract_min"))
        parse_int(v, &config.adaptive.contract_min, 0, 10000000);
    else if (IS("adaptive", "contract_max"))
        parse_int(v, &config.adaptive.contract_max, 0, 10000000);
    else if (IS("adaptive", "raid_min"))
        parse_int(v, &config.adaptive.raid_min, 24, 100000);
    else if (IS("adaptive", "raid_max"))
        parse_int(v, &config.adaptive.raid_max, 24, 100000);
    else if (IS("adaptive", "raid_gap"))
        parse_int(v, &config.adaptive.raid_gap, 0, 5000);
    else if (IS("adaptive", "raid_chase_min"))
        parse_int(v, &config.adaptive.raid_chase_min, 0, 5000);
    else if (IS("adaptive", "raid_chase_max"))
        parse_int(v, &config.adaptive.raid_chase_max, 0, 5000);
    else if (IS("adaptive", "blockade_max"))
        parse_int(v, &config.adaptive.blockade_max, 1, 20000);
#undef IS
}

// Reads an ini file's entries into config; with `only`, just that
// section's. False if the file can't be read.
static bool read_file(const char *file, const char *only)
{
    size_t size;
    char *text = file ? SDL_LoadFile(file, &size) : nullptr;
    if (!text)
        return false;
    char section[32] = "";
    char *save = nullptr;
    for (char *line = SDL_strtok_r(text, "\n", &save); line; line = SDL_strtok_r(nullptr, "\n", &save)) {
        char *semi = SDL_strchr(line, ';');
        if (semi)
            *semi = '\0';
        char *s = trim(line);
        if (*s == '#' || !*s)
            continue;
        if (*s == '[') {
            char *end = SDL_strchr(s, ']');
            if (end) {
                *end = '\0';
                SDL_strlcpy(section, trim(s + 1), sizeof section);
            }
            continue;
        }
        char *eq = SDL_strchr(s, '=');
        if (!eq || (only && SDL_strcasecmp(section, only) != 0))
            continue;
        *eq = '\0';
        set_value(section, trim(s), trim(eq + 1));
    }
    SDL_free(text);
    return true;
}

void config_load(const char *file)
{
    SDL_free(path);
    path = file ? SDL_strdup(file) : nullptr;
    // With a config file the port picks the music; Sound Blaster is
    // what the shipped SETUP.SPD gives.
    if (config.music == CONFIG_MUSIC_SETUP)
        config.music = CONFIG_MUSIC_SOUND_BLASTER;
    read_file(path, nullptr);
    // The menu doesn't offer "as the setup file says".
    if (config.music == CONFIG_MUSIC_SETUP)
        config.music = CONFIG_MUSIC_SOUND_BLASTER;
}

bool config_load_dev(const char *file, bool all_on)
{
    ConfigDiagnostics own = dev_active ? file_diagnostics : config.diagnostics;
    if (all_on)
        config.diagnostics = (ConfigDiagnostics){
            .crash_dumps = true,
            .hang_watchdog = true,
            .hang_seconds = config.diagnostics.hang_seconds,
            .log_level = CONFIG_LOG_DEBUG,
            .event_log = true,
        };
    bool read = read_file(file, "diagnostics");
    ConfigSwitches before = config.improvements;
    read = read_file(file, "improvements") || read;
    for (int i = 0; i < config.improvements.count; i++) {
        int j = switch_index(&before, config.improvements.keys[i]);
        if (j >= 0 && before.on[j] == config.improvements.on[i])
            continue;
        if (dev_prev.count == CONFIG_MAX_SWITCHES)
            break;
        int n = dev_prev.count++;
        SDL_strlcpy(dev_prev.keys[n], config.improvements.keys[i], CONFIG_KEY_SIZE);
        dev_prev.on[n] = j >= 0 && before.on[j];
        dev_had[n] = j >= 0;
        dev_active = true;
    }
    if (read || all_on) {
        file_diagnostics = own;
        dev_active = true;
    }
    return read || all_on;
}

// --- Writing ------------------------------------------------------------------

static const char *on_off(bool b)
{
    return b ? "on" : "off";
}

bool config_save(void)
{
    if (!path)
        return true;
    // dev.ini's diagnostics are for this run only.
    const ConfigDiagnostics *diag = dev_active ? &file_diagnostics : &config.diagnostics;
    char *tmp = nullptr;
    SDL_asprintf(&tmp, "%s.tmp", path);
    SDL_IOStream *io = tmp ? SDL_IOFromFile(tmp, "w") : nullptr;
    if (!io) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Config %s: %s", path, SDL_GetError());
        SDL_free(tmp);
        return false;
    }
    bool ok = SDL_IOprintf(io,
                           "; Reunion port settings (F1 in the game). Rewritten by the game.\n"
                           "\n[cheats]\n"
                           "freeze_money = %s\n"
                           "freeze_ore = %s\n"
                           "\n[sound]\n"
                           "music = %s ; none | soundblaster | gus\n"
                           "effects = %s\n"
                           "music_volume = %d\n"
                           "effects_volume = %d\n"
                           "\n[video]\n"
                           "fullscreen = %s ; off | on | desktop\n"
                           "window_width = %d\n"
                           "window_height = %d\n"
                           "fullscreen_width = %d ; 0: the desktop's resolution\n"
                           "fullscreen_height = %d\n"
                           "fullscreen_refresh = %g\n"
                           "filter = %s ; sharp | smooth | scale2x | crt\n"
                           "\n[game]\n"
                           "skip_intro = %s\n"
                           "colony_stats = %s\n"
                           "detoxin_rate = %s\n"
                           "language = %s ; english | german | french | italian (the CD's)\n"
                           "autosave_minutes = %d ; the Autosave improvement saves this often, 0: never\n"
                           "\n[diagnostics] ; reports in logs/ for bug hunting (dev.ini overrides them)\n"
                           "crash_dumps = %s\n"
                           "hang_watchdog = %s\n"
                           "hang_seconds = %d\n"
                           "log_level = %s ; off | error | warn | info | debug\n"
                           "event_log = %s\n"
                           "\n[adaptive] ; the adaptive threat improvement (percentages of the original's)\n"
                           "level = %s ; easy | normal | hard\n"
                           "band_low = %d ; wave / player strength aimed for at Normal, in %%\n"
                           "band_high = %d\n"
                           "scale_min = %d ; a wave's strength never below this %% of the original's\n"
                           "scale_max = %d ; ... never above (before the bonus for waiting)\n"
                           "delay_max = %d ; a wave may wait this %% of its interval for a weak player\n"
                           "delay_bonus = %d ; %% of strength it gains per 100 h waited\n"
                           "credits_per_unit = %d ; credits that count as one Hunter of strength\n"
                           "colony_units = %d ; Hunters of strength one colony counts as\n"
                           "warn_on_launch = %s ; a message when a wave sets off\n"
                           "warn_intel = %s ; a message 100-200 h before it, with an observatory or satellite\n"
                           "contract_min = %d ; an intelligence contract costs between these,\n"
                           "contract_max = %d ; drawn once per race\n"
                           "raid_min = %d ; hours between the raids (the Raids improvement),\n"
                           "raid_max = %d ; drawn between these\n"
                           "raid_gap = %d ; no raid this near a story wave, hours\n"
                           "raid_chase_min = %d ; a raid chased off without a fight delays the next\n"
                           "raid_chase_max = %d ; by this many hours\n"
                           "blockade_max = %d ; a blockade nobody breaks is lifted after this many hours\n"
                           "\n[improvements]\n",
                           on_off(config.freeze_money), on_off(config.freeze_ore),
                           MUSIC_NAMES[config.music], on_off(config.effects), config.music_volume,
                           config.effects_volume, DISPLAY_NAMES[config.display],
                           config.window_width, config.window_height, config.fullscreen_width,
                           config.fullscreen_height, (double)config.fullscreen_refresh, FILTER_NAMES[config.filter],
                           on_off(config.skip_intro), on_off(config.colony_stats),
                           on_off(config.detoxin_rate),
                           gamedata_language_name(config.language), config.autosave_minutes,
                           on_off(diag->crash_dumps),
                           on_off(diag->hang_watchdog), diag->hang_seconds,
                           LOG_LEVEL_NAMES[diag->log_level], on_off(diag->event_log), LEVEL_NAMES[config.adaptive.level],
                           config.adaptive.band_low, config.adaptive.band_high,
                           config.adaptive.scale_min, config.adaptive.scale_max,
                           config.adaptive.delay_max, config.adaptive.delay_bonus,
                           config.adaptive.credits_per_unit, config.adaptive.colony_units,
                           on_off(config.adaptive.warn_on_launch), on_off(config.adaptive.warn_intel),
                           config.adaptive.contract_min, config.adaptive.contract_max,
                           config.adaptive.raid_min, config.adaptive.raid_max, config.adaptive.raid_gap,
                           config.adaptive.raid_chase_min, config.adaptive.raid_chase_max, config.adaptive.blockade_max) > 0;
    for (int i = 0; ok && i < config.improvements.count; i++) {
        bool on = config.improvements.on[i];
        int j = switch_index(&dev_prev, config.improvements.keys[i]);
        if (j >= 0) {
            if (!dev_had[j])
                continue;  // dev.ini added it: reunion.ini never had it
            on = dev_prev.on[j];
        }
        ok = SDL_IOprintf(io, "%s = %s\n", config.improvements.keys[i], on_off(on)) > 0;
    }
    ok = ok && SDL_IOprintf(io, "\n[fixes]\n") > 0;
    for (int i = 0; ok && i < config.fixes.count; i++)
        ok = SDL_IOprintf(io, "%s = %s\n", config.fixes.keys[i], on_off(config.fixes.on[i])) > 0;
    ok = ok && SDL_IOprintf(io, "\n[keys] ; keyboard play: SDL key names, Shift+ Ctrl+ Alt+, a comma between two\n") > 0;
    for (int i = 0; ok && i < config.keys.count; i++)
        ok = SDL_IOprintf(io, "%s = %s\n", config.keys.actions[i], config.keys.keys[i]) > 0;
    ok = SDL_CloseIO(io) && ok;
    ok = ok && SDL_RenamePath(tmp, path);
    if (!ok) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Config %s: %s", path, SDL_GetError());
        SDL_RemovePath(tmp);
    }
    SDL_free(tmp);
    return ok;
}
