#include "game/evlog.h"

#include <SDL3/SDL.h>
#include <stdarg.h>

#include "game/dseg.h"
#include "game/fixes.h"
#include "game/groups.h"
#include "game/improvements.h"
#include "game/intel.h"
#include "game/system.h"
#include "platform/diag.h"
#include "platform/timing.h"

enum : int {
    SYSTEMS = 8,
    MAX_BODIES = 32,
    RACES = 11,  // 2..12
    FORCES = 7,
    PRODUCTS = 35,
};

// --- JSON lines -----------------------------------------------------------------

typedef struct Json {
    char buf[12288];
    size_t len;
    bool first;  // no comma before the next member or element
} Json;

static Json json;

SDL_PRINTF_VARARG_FUNC(2) static void raw(Json *j, SDL_PRINTF_FORMAT_STRING const char *fmt, ...)
{
    if (j->len >= sizeof j->buf - 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = SDL_vsnprintf(j->buf + j->len, sizeof j->buf - j->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        j->len = SDL_min(j->len + (size_t)n, sizeof j->buf - 1);
}

// A member's key, or for nullptr the separator of an array's element.
static void key(Json *j, const char *k)
{
    if (!j->first)
        raw(j, ",");
    j->first = false;
    if (k)
        raw(j, "\"%s\":", k);
}

static void j_int(Json *j, const char *k, long long v)
{
    key(j, k);
    raw(j, "%lld", v);
}

static void j_bool(Json *j, const char *k, bool v)
{
    key(j, k);
    raw(j, v ? "true" : "false");
}

static void j_str(Json *j, const char *k, const char *s)
{
    key(j, k);
    raw(j, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            raw(j, "\\%c", c);
        else if (c < 0x20 || c >= 0x7F)
            raw(j, "\\u%04x", c);  // DOS code page bytes: kept as their numbers
        else
            raw(j, "%c", c);
    }
    raw(j, "\"");
}

static void j_ints(Json *j, const char *k, const int32_t *v, int n)
{
    key(j, k);
    raw(j, "[");
    for (int i = 0; i < n; i++)
        raw(j, i ? ",%ld" : "%ld", (long)v[i]);
    raw(j, "]");
}

static void j_open(Json *j, const char *k, char bracket)
{
    key(j, k);
    raw(j, "%c", bracket);
    j->first = true;
}

static void j_close(Json *j, char bracket)
{
    raw(j, "%c", bracket);
    j->first = false;
}

static void date_text(char *out, size_t size)
{
    SDL_snprintf(out, size, "%04u-%02u-%02u %02u", ds.Game_Year, ds.Game_Month, ds.Game_Day,
                 ds.Game_Hour);
}

static bool session_written;
static void write_session(void);

// Starts a line: {"type":..,"ts":..,"date":..
static Json *begin(const char *type)
{
    if (!session_written)
        write_session();
    Json *j = &json;
    j->len = 0;
    j->first = true;
    raw(j, "{");
    j_str(j, "type", type);
    char when[40] = "?";
    SDL_Time now;
    SDL_DateTime dt;
    if (SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &dt, true))
        SDL_snprintf(when, sizeof when, "%04d-%02d-%02dT%02d:%02d:%02d", dt.year, dt.month, dt.day,
                     dt.hour, dt.minute, dt.second);
    j_str(j, "ts", when);
    char date[24];
    date_text(date, sizeof date);
    j_str(j, "date", date);
    return j;
}

static void end(Json *j)
{
    raw(j, "}");
    diag_event_line(j->buf);
}

static void write_session(void)
{
    session_written = true;
    Json *j = begin("session");
    j_str(j, "build", __DATE__ " " __TIME__);
    j_open(j, "improvements", '[');
    for (int i = 0; i < Improvements_Count; i++)
        if (*Improvements_List[i].value)
            j_str(j, nullptr, Improvements_List[i].key);
    j_close(j, ']');
    j_open(j, "fixes_off", '[');
    for (int i = 0; i < Fixes_Count; i++)
        if (!*Fixes_List[i].value)
            j_str(j, nullptr, Fixes_List[i].key);
    j_close(j, ']');
    end(j);
}

// --- The game's state ------------------------------------------------------------

static int body_count(int s)
{
    return SDL_min(ds.Galaxy_PlanetCount[s] + ds.Galaxy_MoonCount[s], MAX_BODIES);
}

static void race_name(char *out, size_t size, int16_t r)
{
    pstr_get(out, size, &Intel_Race(r)->Name);
}

static void body_name(char *out, size_t size, int s, int i)
{
    pstr_get(out, size, &ds.Galaxy_Names[s][i].Name);
}

// The player's armed groups (fleets, secret forces, planet forces). With a
// site, only those there (ground units only on that moon).
typedef struct Strength {
    int32_t ships[4];
    int32_t ground[4];
} Strength;

static Strength player_strength(bool at_site, int system, int planet, int moon)
{
    Strength st = {0};
    int16_t lists[2] = {ds.Game_GroupCount, ds.Game_PlanetForceCount};
    for (int16_t li = 0; li < 2; li++) {
        for (int16_t g = 1; g <= lists[li]; g++) {
            const TGroup *grp = Groups_At(li, g);
            if (!(grp->Kind == 1 || grp->Kind == 3 || grp->Kind == 5))
                continue;
            if (at_site
                && !(grp->System == system && grp->Planet == planet
                     && (grp->Status == 1 || grp->Status == 2 || grp->Status == 7)))
                continue;
            for (int c = 0; c < 4; c++) {
                st.ships[c] += grp->Units[0][c].Count;
                if (!at_site || grp->Moon == moon)
                    st.ground[c] += grp->Units[1][c].Count;
            }
        }
    }
    return st;
}

static void force_units(int32_t out[8], const TRaceForce *f)
{
    for (int u = 0; u < 8; u++)
        out[u] = f->Units[u];
}

static void j_site(Json *j, int system, int planet, int moon)
{
    j_int(j, "system", system);
    j_int(j, "planet", planet);
    j_int(j, "moon", moon);
}

// --- Comparisons from hour to hour ---------------------------------------------

typedef struct StoryTimer {
    const char *name;
    int16_t *timer;  // nullptr: a flag only
    uint8_t *done;   // nullptr: a timer only
} StoryTimer;

static const StoryTimer STORY[] = {
    {"race2_1", &ds.Intel_Race2Timer1, &ds.Intel_Race2Done1},
    {"race2_2", &ds.Intel_Race2Timer2, &ds.Intel_Race2Done2},
    {"race2_3", &ds.Intel_Race2Timer3, &ds.Intel_Race2Done3},
    {"race2_4", &ds.Intel_Race2Timer4, &ds.Intel_Race2Done4},
    {"morgrul_attack1", &ds.Intel_MorgrulAttack1Timer, &ds.Intel_MorgrulAttack1Done},
    {"morgrul_attack2", &ds.Intel_MorgrulAttack2Timer, &ds.Intel_MorgrulAttack2Done},
    {"morgrul_attack3", &ds.Intel_MorgrulAttack3Timer, &ds.Intel_MorgrulAttack3Done},
    {"phoenix1", &ds.Intel_Phoenix1Timer, &ds.Intel_Phoenix1Done},
    {"race4", &ds.Intel_Race4Timer, &ds.Intel_Race4Done},
    {"kall_turn", &ds.Intel_KallTurnTimer, &ds.Intel_KallTurnDone},
    {"joint_attack", &ds.Intel_JointAttackTimer, &ds.Intel_JointAttackDone},
    {"race5", &ds.Intel_Race5Timer, &ds.Intel_Race5Done},
    {"msg14", &ds.Intel_Msg14Timer, &ds.Intel_Msg14Done},
    {"race7to10", &ds.Intel_Race7to10Timer, &ds.Intel_Race7to10Done},
    {"satellite_theft", &ds.Intel_SatTheftTimer, &ds.Intel_SatTheftDone},
    {"invention14", &ds.Intel_Invention14Timer, &ds.Intel_Invention14Revealed},
    {"dev_away", &ds.Intel_DevAwayTimer, nullptr},
    {"system3", &ds.Intel_System3Timer, nullptr},
    {"system4_destroy", &ds.Intel_System4DestroyTimer, nullptr},
    {"system4_warn", &ds.Intel_System4WarnTimer, nullptr},
    {"force_launch", &ds.Intel_LaunchTimer, nullptr},
    {"system4_warned", nullptr, &ds.Intel_System4Warned},
    {"system4_destroyed", nullptr, &ds.Intel_System4Destroyed},
    {"pub7", nullptr, &ds.Intel_Pub7Done},
    {"pub10", nullptr, &ds.Intel_Pub10Done},
};
enum : int { STORY_COUNT = (int)(sizeof STORY / sizeof STORY[0]) };

static struct {
    bool valid;
    int64_t hour;  // hours since year 0 in the game's calendar (30-day months)
    uint8_t owner[SYSTEMS][MAX_BODIES];
    int8_t status[RACES];
    uint8_t active[RACES][FORCES];
    int16_t timer[STORY_COUNT];
    uint8_t done[STORY_COUNT];
    int16_t product[PRODUCTS];
} prev;

static int64_t game_hour(void)
{
    return (((int64_t)ds.Game_Year * 12 + ds.Game_Month - 1) * 30 + ds.Game_Day - 1) * 24
           + ds.Game_Hour;
}

static void take_snapshot(void)
{
    prev.valid = true;
    prev.hour = game_hour();
    for (int s = 0; s < SYSTEMS; s++)
        for (int i = 0; i < body_count(s); i++)
            prev.owner[s][i] = ds.Galaxy_Bodies[s][i].Owner;
    for (int r = 0; r < RACES; r++) {
        const TRace *rc = Intel_Race((int16_t)(r + 2));
        prev.status[r] = rc->Status;
        for (int f = 0; f < FORCES; f++)
            prev.active[r][f] = f < rc->ForceCount ? rc->Forces[f].Active : 0;
    }
    for (int k = 0; k < STORY_COUNT; k++) {
        prev.timer[k] = STORY[k].timer ? *STORY[k].timer : 0;
        prev.done[k] = STORY[k].done ? *STORY[k].done : 0;
    }
    for (int p = 0; p < PRODUCTS; p++)
        prev.product[p] = ds.Research_Products[p].State;
}

static void compare_story(void)
{
    for (int k = 0; k < STORY_COUNT; k++) {
        const StoryTimer *t = &STORY[k];
        int16_t timer = t->timer ? *t->timer : 0;
        uint8_t done = t->done ? *t->done : 0;
        const char *what = nullptr;
        if (done && !prev.done[k])
            what = "done";
        else if (!t->done && prev.timer[k] > 0 && timer <= 0)
            what = "timer_out";
        else if (timer > 0 && timer > prev.timer[k])
            what = "armed";
        if (!what)
            continue;
        Json *j = begin("story");
        j_str(j, "name", t->name);
        j_str(j, "what", what);
        j_int(j, "timer", timer);
        end(j);
    }
}

static void compare_bodies(void)
{
    char name[32];
    for (int s = 0; s < SYSTEMS; s++) {
        for (int i = 0; i < body_count(s); i++) {
            const TBody *b = &ds.Galaxy_Bodies[s][i];
            if (b->Owner == prev.owner[s][i])
                continue;
            Json *j = begin("body_owner");
            j_int(j, "system", s + 1);
            j_int(j, "body", i + 1);
            body_name(name, sizeof name, s, i);
            j_str(j, "name", name);
            j_int(j, "from", prev.owner[s][i]);
            j_int(j, "to", b->Owner);
            j_int(j, "population", b->Population);
            end(j);
        }
    }
}

static void compare_races(void)
{
    char name[32];
    for (int16_t r = 2; r <= 12; r++) {
        const TRace *rc = Intel_Race(r);
        race_name(name, sizeof name, r);
        if (rc->Status != prev.status[r - 2]) {
            Json *j = begin("race_status");
            j_int(j, "race", r);
            j_str(j, "name", name);
            j_int(j, "from", prev.status[r - 2]);
            j_int(j, "to", rc->Status);
            end(j);
        }
        for (int16_t f = 1; f <= rc->ForceCount && f <= FORCES; f++) {
            const TRaceForce *fc = &rc->Forces[f - 1];
            uint8_t was = prev.active[r - 2][f - 1];
            const char *type = fc->Active == 2 && was != 2   ? "force_dispatch"
                               : fc->Active == 1 && was == 2 ? "force_arrival"
                                                             : nullptr;
            if (!type)
                continue;
            int32_t units[8];
            force_units(units, fc);
            Json *j = begin(type);
            j_int(j, "race", r);
            j_str(j, "name", name);
            j_int(j, "force", f);
            j_site(j, fc->System, fc->Body, fc->Moon);
            j_int(j, "mission", fc->Mission);
            j_int(j, "next_mission", fc->NextMission);
            j_int(j, "travel", fc->TravelTime);
            j_int(j, "timer", fc->Timer);
            j_ints(j, "units", units, 8);
            end(j);
        }
    }
}

static void compare_research(void)
{
    char name[32];
    for (int p = 0; p < PRODUCTS; p++) {
        const TProduct *pr = &ds.Research_Products[p];
        if (pr->State == 5 && prev.product[p] != 5) {
            Json *j = begin("research_done");
            j_int(j, "invention", p + 1);
            pstr_get(name, sizeof name, &pr->Name);
            j_str(j, "name", name);
            end(j);
        }
    }
}

static void write_day(void)
{
    Json *j = begin("day");
    j_int(j, "credits", ds.Game_Credits);
    int32_t ore[6] = {0}, population = 0, colonies = 0;
    for (int s = 0; s < SYSTEMS; s++) {
        for (int i = 0; i < body_count(s); i++) {
            const TBody *b = &ds.Galaxy_Bodies[s][i];
            if (b->Owner != 1)
                continue;
            colonies++;
            population += b->Population;
            for (int o = 0; o < 6; o++)
                ore[o] += b->Stores[o];
        }
    }
    j_int(j, "colonies", colonies);
    j_int(j, "population", population);
    j_ints(j, "ore", ore, 6);
    int32_t done = 0;
    for (int p = 0; p < PRODUCTS; p++)
        done += ds.Research_Products[p].State == 5;
    j_int(j, "inventions", done);
    j_int(j, "researching", ds.Game_CurResearch);
    Strength st = player_strength(false, 0, 0, 0);
    j_ints(j, "ships", st.ships, 4);
    j_ints(j, "ground", st.ground, 4);
    j_int(j, "fleets", ds.Game_GroupCount);

    j_open(j, "races", '[');
    char name[32];
    for (int16_t r = 2; r <= 12; r++) {
        const TRace *rc = Intel_Race(r);
        int32_t units[8] = {0}, garrison[8] = {0};
        int forces = 0, travelling = 0, bodies = 0;
        for (int f = 0; f < rc->ForceCount && f < FORCES; f++) {
            const TRaceForce *fc = &rc->Forces[f];
            if (fc->Active == 0)
                continue;
            forces++;
            travelling += fc->Active == 2;
            for (int u = 0; u < 8; u++)
                units[u] += fc->Units[u];
        }
        for (int s = 0; s < SYSTEMS; s++) {
            for (int i = 0; i < body_count(s); i++) {
                const TBody *b = &ds.Galaxy_Bodies[s][i];
                if (b->Owner != r)
                    continue;
                bodies++;
                for (int u = 0; u < 8; u++)
                    garrison[u] += b->Stores[u];
            }
        }
        j_open(j, nullptr, '{');
        j_int(j, "race", r);
        race_name(name, sizeof name, r);
        j_str(j, "name", name);
        j_int(j, "status", rc->Status);
        j_bool(j, "alive", rc->Alive != 0);
        j_int(j, "credits", rc->Credits);
        j_int(j, "bodies", bodies);
        j_int(j, "forces", forces);
        j_int(j, "travelling", travelling);
        j_ints(j, "units", units, 8);
        j_ints(j, "garrison", garrison, 8);
        j_close(j, '}');
    }
    j_close(j, ']');
    end(j);
}

// --- Hooks -----------------------------------------------------------------------

bool EvLog_On(void)
{
    return diag_events_on();
}

void EvLog_Hour(void)
{
    if (!EvLog_On()) {
        prev.valid = false;
        return;
    }
    if (!prev.valid || game_hour() != prev.hour + 1) {
        char from[24] = "";
        if (prev.valid)
            SDL_snprintf(from, sizeof from, "%lld h earlier", (long long)(game_hour() - prev.hour));
        Json *j = begin("time_jump");
        j_str(j, "from", from);
        j_int(j, "credits", ds.Game_Credits);
        end(j);
        take_snapshot();
        if (ds.Game_Hour == 0)
            write_day();
        return;
    }
    compare_story();
    compare_races();
    compare_bodies();
    compare_research();
    take_snapshot();
    if (ds.Game_Hour == 0)
        write_day();
}

void EvLog_Attack(int16_t race, int16_t force, bool defended, bool battle)
{
    if (!EvLog_On())
        return;
    const TRaceForce *fc = Intel_Force(race, force);
    char name[32];
    race_name(name, sizeof name, race);
    int32_t units[8];
    force_units(units, fc);
    Strength st = player_strength(true, fc->System, fc->Body, fc->Moon);
    Json *j = begin("attack");
    j_int(j, "race", race);
    j_str(j, "name", name);
    j_int(j, "force", force);
    j_site(j, fc->System, fc->Body, fc->Moon);
    j_int(j, "mission", fc->NextMission);
    j_bool(j, "ground", ds.SpWar_GroundBattle != 0);
    j_bool(j, "defended", defended);
    j_bool(j, "battle", battle);
    j_ints(j, "units", units, 8);
    j_ints(j, "player_ships", st.ships, 4);
    j_ints(j, "player_ground", st.ground, 4);
    end(j);
}

static void battle_head(Json *j)
{
    char name[32] = "";
    if (ds.SpWar_EnemyRace >= 2 && ds.SpWar_EnemyRace <= 12)
        race_name(name, sizeof name, ds.SpWar_EnemyRace);
    j_site(j, ds.SpWar_BattleSystem, ds.SpWar_BattlePlanet, ds.SpWar_BattleMoon);
    j_int(j, "race", ds.SpWar_EnemyRace);
    j_str(j, "name", name);
    j_bool(j, "player_attacks", ds.Sim_PlayerAttacks != 0);
}

void EvLog_SpaceBattleStart(void)
{
    if (!EvLog_On())
        return;
    Json *j = begin("space_battle_start");
    battle_head(j);
    j_int(j, "player_ships", ds.SpWar_AttackerShipCount);
    j_int(j, "enemy_ships", ds.SpWar_DefenderShipCount);
    end(j);
}

void EvLog_SpaceBattleEnd(void)
{
    if (!EvLog_On())
        return;
    Json *j = begin("space_battle_end");
    battle_head(j);
    j_bool(j, "player_won", ds.SpWar_PlayerWon != 0);
    j_bool(j, "retreated", ds.SpWar_Retreated != 0);
    j_int(j, "player_ships_start", ds.SpWar_AttackerStart);
    j_int(j, "enemy_ships_start", ds.SpWar_DefenderStart);
    j_int(j, "player_ships_left", ds.SpWar_AttackerShipCount);
    j_int(j, "enemy_ships_left", ds.SpWar_DefenderShipCount);
    j_ints(j, "player_losses", ds.SpWar_AttackerLosses, 4);
    j_ints(j, "enemy_losses", ds.SpWar_DefenderLosses, 4);
    end(j);
}

void EvLog_GroundBattleStart(void)
{
    if (!EvLog_On())
        return;
    Json *j = begin("ground_battle_start");
    battle_head(j);
    j_ints(j, "player_units", ds.GrWarUnits_Totals[0], 4);
    j_ints(j, "enemy_units", ds.GrWarUnits_Totals[1], 4);
    end(j);
}

void EvLog_GroundBattleEnd(void)
{
    if (!EvLog_On())
        return;
    Json *j = begin("ground_battle_end");
    battle_head(j);
    j_bool(j, "player_won", ds.GrWar_PlayerWon != 0);
    j_bool(j, "retreated", ds.SpWar_Retreated != 0);
    j_ints(j, "player_units", ds.GrWarUnits_Totals[0], 4);
    j_ints(j, "enemy_units", ds.GrWarUnits_Totals[1], 4);
    j_ints(j, "player_losses", ds.GrWar_Losses[0], 4);
    j_ints(j, "enemy_losses", ds.GrWar_Losses[1], 4);
    end(j);
}

// --- Crash and hang reports ------------------------------------------------------

static void describe(char *out, size_t size)
{
    SDL_snprintf(out, size, "screen %d, date %04u-%02u-%02u %02u:00, credits %ld, frame %llu",
                 ds.Game_CurrentScreen, ds.Game_Year, ds.Game_Month, ds.Game_Day, ds.Game_Hour,
                 (long)ds.Game_Credits, (unsigned long long)timing_frames());
}

void EvLog_Init(void)
{
    diag_set_context(describe);
    session_written = false;
    prev.valid = false;
}

void EvLog_Adaptive(const char *what, int wave, int16_t race, int a, int b, int c)
{
    if (!EvLog_On())
        return;
    Json *j = begin("adaptive");
    j_str(j, "what", what);
    j_int(j, "wave", wave);
    j_int(j, "race", race);
    char name[32];
    race_name(name, sizeof name, race);
    j_str(j, "name", name);
    j_int(j, "a", a);
    j_int(j, "b", b);
    j_int(j, "c", c);
    end(j);
}
