#include "game/adaptive.h"

#include <SDL3/SDL.h>

#include "game/dseg.h"
#include "game/evlog.h"
#include "game/groups.h"
#include "game/improvements.h"
#include "game/intel.h"
#include "game/message.h"
#include "game/planetinfo.h"
#include "game/system.h"
#include "platform/config.h"

enum : int {
    MAX_DELAYED_WAVES = 3,  // waves 1..3 (MORGRUL2 .. JOINT) may wait
    LEVEL_STEP = 20,        // points the band moves at Easy and Hard
    WARN_LEAD_MIN = 100,
    WARN_LEAD_SPAN = 101,   // 100..200 h
};

AdaptiveState Adaptive_State;

// The bodies of system s (0..7): planets and moons.
static int bodies(int s)
{
    return SDL_min(ds.Galaxy_PlanetCount[s] + ds.Galaxy_MoonCount[s], 32);
}

void Adaptive_Reset(void)
{
    SDL_zero(Adaptive_State);
}

// --- Strength -------------------------------------------------------------------

int32_t Adaptive_PlayerPower(void)
{
    int32_t units = 0;
    int16_t lists[2] = {ds.Game_GroupCount, ds.Game_PlanetForceCount};
    for (int16_t li = 0; li < 2; li++) {
        for (int16_t g = 1; g <= lists[li]; g++) {
            const TGroup *grp = Groups_At(li, g);
            if (!(grp->Kind == 1 || grp->Kind == 3 || grp->Kind == 5))
                continue;
            for (int t = 0; t < 2; t++)
                for (int c = 0; c < 4; c++)
                    units += grp->Units[t][c].Count;
        }
    }
    int32_t colonies = 0;
    for (int s = 0; s < 8; s++)
        for (int i = 0; i < bodies(s); i++)
            colonies += ds.Galaxy_Bodies[s][i].Owner == 1;
    const ConfigAdaptive *c = &config.adaptive;
    if (ds.Game_Credits > 0)
        units += ds.Game_Credits / c->credits_per_unit;
    return units + colonies * c->colony_units;
}

static int32_t force_power(const TRaceForce *f)
{
    int32_t n = 0;
    for (int u = 0; u < 8; u++)
        n += f->Units[u] > 0 ? f->Units[u] : 0;
    return n;
}

// The band the wave / player ratio (%) is aimed for.
static void band(int *low, int *high)
{
    int off = (config.adaptive.level - CONFIG_LEVEL_NORMAL) * LEVEL_STEP;
    *low = SDL_max(config.adaptive.band_low + off, 1);
    *high = SDL_max(config.adaptive.band_high + off, *low);
}

// The % of the original wave that brings it into the band, within the
// limits (before the bonus for waiting).
static int scale_for(int32_t wave, int32_t player)
{
    int low, high;
    band(&low, &high);
    if (wave <= 0)
        return 100;
    int64_t ratio = (int64_t)wave * 100 / SDL_max(player, 1);
    int64_t f = 100;
    if (ratio > high)
        f = (int64_t)high * 100 / ratio;
    else if (ratio < low)
        f = (int64_t)low * 100 / ratio;
    return (int)SDL_clamp(f, config.adaptive.scale_min, config.adaptive.scale_max);
}

static bool wave_can_wait(int wave)
{
    return wave >= WAVE_MORGRUL2 && wave < WAVE_MORGRUL2 + MAX_DELAYED_WAVES;
}

// --- Warnings -------------------------------------------------------------------

static const char *race_name(int16_t race, char *buf, size_t size)
{
    pstr_get(buf, size, &Intel_Race(race)->Name);
    for (char *p = buf + SDL_strlen(buf); p > buf && p[-1] == ' '; )
        *--p = '\0';
    return buf;
}

// "Target: <body>" for the force's destination.
static void target_line(char *out, size_t size, const TRaceForce *f)
{
    char where[64];
    PlanetInfo_GetBodyLongName(where, sizeof where, f->System, f->Body, f->Moon);
    SDL_snprintf(out, size, "%s", where);
}

// The race's home system has an observatory, or a satellite watches its home body.
static bool has_intel(int16_t race)
{
    const TRace *r = Intel_Race(race);
    if (r->HomeSystem >= 1 && r->HomeSystem <= 8 && ds.Sim_Observatories[r->HomeSystem - 1] > 0)
        return true;
    const TBody *b = PlanetInfo_BodyAt(r->HomeSystem, r->HomeBody, 0);
    return b && b->Satellite != 0;
}

// How many units (ships, ground) the message names: the wave's, scaled, as
// a range of +-30%.
static void strength_range(const TRaceForce *f, int percent, int32_t ships[2], int32_t ground[2])
{
    int32_t s = 0, g = 0;
    for (int u = 0; u < 4; u++) {
        s += f->Units[u] > 0 ? f->Units[u] : 0;
        g += f->Units[4 + u] > 0 ? f->Units[4 + u] : 0;
    }
    s = (int32_t)((int64_t)s * percent / 100);
    g = (int32_t)((int64_t)g * percent / 100);
    ships[0] = s * 7 / 10;
    ships[1] = (s * 13 + 9) / 10;
    ground[0] = g * 7 / 10;
    ground[1] = (g * 13 + 9) / 10;
}

// The message of a warning. `hours` is when the wave arrives or sets off
// (a window of +-24 h around it); `percent` the scale the wave will have.
static void warn(bool launched, int16_t race, int16_t force, int hours, int percent)
{
    const TRaceForce *f = Intel_Force(race, force);
    char name[32], where[96], head[96], when[96], count[96];
    race_name(race, name, sizeof name);
    target_line(where, sizeof where, f);
    int32_t ships[2], ground[2];
    strength_range(f, percent, ships, ground);
    int from = SDL_max(hours - 24, 1), to = hours + 24;
    SDL_snprintf(head, sizeof head, launched ? " %s fleet has set off " : " %s fleet is gathering ", name);
    SDL_snprintf(when, sizeof when, launched ? " Arrival in %d-%d h " : " It sets off in %d-%d h ", from, to);
    if (ships[1] > 0 && ground[1] > 0)
        SDL_snprintf(count, sizeof count, " Ships %d-%d, ground %d-%d ", (int)ships[0], (int)ships[1],
                     (int)ground[0], (int)ground[1]);
    else
        SDL_snprintf(count, sizeof count, " Ships %d-%d ", (int)ships[0], (int)ships[1]);
    char text[320];
    SDL_snprintf(text, sizeof text, "     INTELLIGENCE     |%s|%s| Target: %s |%s|", head, when,
                 where, count);
    Message_ShowMessage(text);
}

// --- The waves ------------------------------------------------------------------

bool Adaptive_Tick(int wave, int16_t *timer, int16_t race, int16_t force)
{
    if (!improvements.adaptive_threat) {
        return *timer > 0 && --*timer == 0;
    }
    AdaptiveState *st = &Adaptive_State;
    if (*timer <= 0)
        return false;
    if (st->start[wave] == 0) {
        st->start[wave] = (uint16_t)*timer;
        st->lead[wave] = (uint16_t)(System_Random(WARN_LEAD_SPAN) + WARN_LEAD_MIN);
    }
    const TRaceForce *f = Intel_Force(race, force);
    int32_t player = Adaptive_PlayerPower(), power = force_power(f);

    // The advance warning: an observatory or a contract.
    bool intel = (config.adaptive.warn_intel && has_intel(race)) || st->contract[race];
    if (intel && !st->warned[wave] && *timer <= st->lead[wave]) {
        st->warned[wave] = 1;
        int percent = scale_for(power, player);
        EvLog_Adaptive("warn_early", wave, race, *timer, percent, (int)player);
        warn(false, race, force, *timer, percent);
    }

    // Waiting for a player the smallest wave would still overwhelm: half speed.
    if (wave_can_wait(wave)) {
        uint16_t cap = (uint16_t)((uint32_t)st->start[wave] * config.adaptive.delay_max / 100);
        int low, high;
        band(&low, &high);
        bool overwhelmed = power * config.adaptive.scale_min > (int64_t)player * high;
        if (overwhelmed && st->waited[wave] < cap && (ds.Game_Hour & 1)) {
            if (st->waited[wave] == 0)
                EvLog_Adaptive("wait", wave, race, *timer, (int)power, (int)player);
            st->waited[wave]++;
            return false;
        }
    }
    return --*timer == 0;
}

void Adaptive_Dispatched(int wave, int16_t race, int16_t force)
{
    if (!improvements.adaptive_threat)
        return;
    AdaptiveState *st = &Adaptive_State;
    TRaceForce *f = Intel_Force(race, force);
    int32_t player = Adaptive_PlayerPower(), before = force_power(f);
    int percent = scale_for(before, player)
                  + (int)((int64_t)st->waited[wave] * config.adaptive.delay_bonus / 100);
    for (int u = 0; u < 8; u++) {
        int32_t n = f->Units[u];
        if (n <= 0)
            continue;
        n = (int32_t)(((int64_t)n * percent + 50) / 100);
        f->Units[u] = (int16_t)SDL_clamp(n, 1, 32000);
    }
    EvLog_Adaptive("scale", wave, race, percent, (int)before, (int)player);
    if (config.adaptive.warn_on_launch) {
        // Arrival: the flight and the wait before the attack, +-24 h.
        warn(true, race, force, f->TravelTime + f->Timer, 100);
    }
    st->start[wave] = 0;
    st->waited[wave] = 0;
    st->warned[wave] = 0;
}

// --- Contracts ------------------------------------------------------------------

static bool race_valid(int16_t race)
{
    return race >= 2 && race <= 12;
}

bool Adaptive_ContractBought(int16_t race)
{
    return race_valid(race) && Adaptive_State.contract[race] != 0;
}

// The player has a colony or a group in the race's home system.
static bool player_there(int16_t system)
{
    if (system < 1 || system > 8)
        return false;
    for (int i = 0; i < bodies(system - 1); i++)
        if (ds.Galaxy_Bodies[system - 1][i].Owner == 1)
            return true;
    for (int16_t g = 1; g <= ds.Game_GroupCount; g++)
        if (Groups_At(0, g)->System == system)
            return true;
    return false;
}

bool Adaptive_ContractOffered(int16_t race)
{
    if (!improvements.adaptive_threat || !race_valid(race) || Adaptive_ContractBought(race))
        return false;
    const TRace *r = Intel_Race(race);
    if (!r->Alive || r->Status < 0 || r->Status == 6)
        return false;
    // Amnesty is the player's own system: only the race's ships tell of it.
    return r->Status != 0 || (r->HomeSystem > 1 && player_there(r->HomeSystem));
}

int16_t Adaptive_Seller(int16_t race)
{
    switch (race) {
    case 3: case 4: case 5:  // Morgruls, Kalls, Phelonians
        return 4;
    case 6: case 7: case 8: case 9: case 10:  // Erans, Lisonians, Undorlings, Hirachi, Druedians
        return 1;
    case 11: case 12:  // Syonians, Earthlings
        return ds.Pub_StoryFlags[1] ? 2 : 0;
    default:
        return 0;
    }
}

const char *Adaptive_RaceLabel(int16_t race, char *buf, size_t size)
{
    race_name(race, buf, size);
    if (SDL_strncmp(buf, "The ", 4) == 0)
        SDL_memmove(buf, buf + 4, SDL_strlen(buf + 4) + 1);
    return buf;
}

int32_t Adaptive_ContractPrice(int16_t race)
{
    AdaptiveState *st = &Adaptive_State;
    if (!race_valid(race))
        return 0;
    if (st->price10[race] == 0) {
        int lo = SDL_max(config.adaptive.contract_min, 10);
        int hi = SDL_max(config.adaptive.contract_max, lo);
        int span = (hi - lo) / 10 + 1;
        st->price10[race] = (uint16_t)SDL_min(lo / 10 + System_Random((uint16_t)SDL_min(span, 60000)), 65535);
    }
    return (int32_t)st->price10[race] * 10;
}

bool Adaptive_BuyContract(int16_t race)
{
    int32_t price = Adaptive_ContractPrice(race);
    if (!race_valid(race) || Adaptive_ContractBought(race) || ds.Game_Credits < price)
        return false;
    ds.Game_Credits -= price;
    Adaptive_State.contract[race] = 1;
    EvLog_Adaptive("contract", 0, race, price, 0, 0);
    return true;
}
