#include "game/raid.h"

#include <SDL3/SDL.h>

#include "game/adaptive.h"
#include "game/defence.h"
#include "game/dseg.h"
#include "game/evlog.h"
#include "game/improvements.h"
#include "game/intel.h"
#include "game/message.h"
#include "game/planetinfo.h"
#include "game/surface.h"
#include "game/system.h"
#include "platform/config.h"

RaidState Raid_State;

enum : int {
    WARN_LEAD_MIN = 100,    // the warning comes this many hours before the launch...
    WARN_LEAD_SPAN = 101,   // ... 100..200
    SCOUT_LEAD_MIN = 100,   // a scout goes ahead of the raid by 100..300 h
    SCOUT_LEAD_SPAN = 201,
    SCOUT_PERCENT = 50,     // the chance of a scout
    SCOUT_STAY = 24,        // hours it looks around
    BLOCKADE_REMINDER = 72, // hours between the reminders of a blockade in force
    BLOCKADE_LEAVES_BEFORE = 48, // a blockade goes this many hours before a story wave sets off
    BLOCKADE_PERCENT = 40,  // of the raids on a body with no defence
    CEILING_PERCENT = 30,   // of the nearest wave's Hunters and Starfighters
    FLOOR_PERCENT = 50,     // of the ceiling
    EARLY_MIN = 100,        // x the defence: first stages 1.0..1.5, later 1.2..1.8
    EARLY_SPAN = 51,
    LATE_MIN = 120,
    LATE_SPAN = 61,
    OUTMATCH_PERCENT = 150, // a player this strong turns a raid back
    UNDEFENDED_MIN = 3,     // Hunters of a raid on a body with no defence: 3..6
    UNDEFENDED_SPAN = 4,
    BUILDINGS_PERCENT = 15, // the most of a colony's buildings a raid destroys
    ORE_PERCENT = 30,       // ... and of its ore
    ADVANTAGE_MIN = 10,
    EARLY_STAGE = 3,        // Game_StoryStage up to this: the early stages
    COMMAND_CENTRE = 1,
    MINER_STATION = 0x19,
    MINE = 4,
};

void Raid_Reset(void)
{
    SDL_zero(Raid_State);
}

static bool active(void)
{
    return improvements.raids && improvements.adaptive_threat;
}

// --- Weights -------------------------------------------------------------------

// A Hunter's or a Starfighter's combat weight (tenths of a Hunter); class 0, 1.
static int32_t class_weight(int cls)
{
    return Adaptive_ShipWeight(ds.SpWar_ClassB[cls], ds.SpWar_ClassA[cls]);
}

int32_t Raid_Weight(const uint16_t units[2])
{
    return units[0] * class_weight(0) + units[1] * class_weight(1);
}

int32_t Raid_Advantage(int32_t raid, int32_t defence)
{
    if (raid <= 0)
        return ADVANTAGE_MIN;
    int64_t a = (int64_t)(raid - defence) * 100 / raid;
    return (int32_t)SDL_clamp(a, ADVANTAGE_MIN, 100);
}

// A fleet of about `weight`, in the proportion of the weights of `hunters`
// and `fighters`; at least one ship.
static void fleet_of_weight(int32_t weight, int32_t hunters, int32_t fighters, int32_t units[2])
{
    int64_t wh = (int64_t)hunters * class_weight(0), wf = (int64_t)fighters * class_weight(1);
    int64_t share = wh + wf > 0 ? wf * 1000 / (wh + wf) : 0;  // the Starfighters' per mille
    units[1] = (int32_t)((int64_t)weight * share / 1000 + class_weight(1) / 2) / class_weight(1);
    units[0] = (int32_t)(((int64_t)weight * (1000 - share) / 1000) + class_weight(0) / 2) / class_weight(0);
    if (units[0] + units[1] < 1)
        units[0] = 1;
}

void Raid_PlanFleet(int32_t defence, int32_t hunters, int32_t fighters, bool early, int32_t units[2])
{
    units[0] = units[1] = 0;
    int32_t ceiling = (int32_t)(((int64_t)hunters * class_weight(0) + (int64_t)fighters * class_weight(1))
                                * CEILING_PERCENT / 100);
    ceiling = SDL_max(ceiling, 2 * class_weight(0));
    if (defence <= 0) {
        // No defence at all: the lightest ships, a few.
        units[0] = UNDEFENDED_MIN + System_Random(UNDEFENDED_SPAN);
        return;
    }
    int32_t factor = early ? EARLY_MIN + System_Random(EARLY_SPAN) : LATE_MIN + System_Random(LATE_SPAN);
    int64_t weight = (int64_t)defence * factor / 100;
    weight = SDL_clamp(weight, (int64_t)ceiling * FLOOR_PERCENT / 100, ceiling);
    fleet_of_weight((int32_t)weight, hunters, fighters, units);
}

// --- The story's waves ---------------------------------------------------------------

typedef struct WaveRef {
    int16_t race;
    int32_t hours;
    int32_t hunters, fighters;
} WaveRef;

// The story's waves with a timer running (as sim.c arms them): their race,
// force and hours to launch. `out` holds 8.
static int armed_waves(WaveRef out[8])
{
    int n = 0;
    struct {
        bool armed;
        int16_t race, force;
        int32_t hours;
    } w[] = {
        {!ds.Intel_MorgrulAttack1Done && ds.Intel_MorgrulAttack1Timer > 0, 3, 1, ds.Intel_MorgrulAttack1Timer},
        {!ds.Intel_MorgrulAttack2Done && ds.Intel_MorgrulAttack2Timer > 0, 3, 2, ds.Intel_MorgrulAttack2Timer},
        {!ds.Intel_MorgrulAttack3Done && ds.Intel_MorgrulAttack3Timer > 0, 3, 3, ds.Intel_MorgrulAttack3Timer},
        {!ds.Intel_JointAttackDone && ds.Intel_JointAttackTimer > 0, 3, 4, ds.Intel_JointAttackTimer},
        {!ds.Intel_Race7to10Done && ds.Intel_Race7to10Timer > 0, 7, 1, ds.Intel_Race7to10Timer},
        {ds.Intel_LaunchTimer > 0, ds.Intel_LaunchRace, ds.Intel_LaunchForce, ds.Intel_LaunchTimer},
    };
    for (size_t i = 0; i < SDL_arraysize(w); i++) {
        if (!w[i].armed || w[i].race < 2 || w[i].race > 12 || w[i].force < 1 || w[i].force > 7)
            continue;
        const TRaceForce *f = Intel_Force(w[i].race, w[i].force);
        out[n++] = (WaveRef){w[i].race, w[i].hours, f->Units[0], f->Units[1]};
    }
    return n;
}

static bool hostile(int16_t race)
{
    const TRace *r = Intel_Race(race);
    return r->Alive && r->Status == 2;
}

// The hours to the story's nearest wave, of any race; -1 for none.
static int32_t hours_to_wave(void)
{
    WaveRef w[8];
    int n = armed_waves(w);
    int32_t best = -1;
    for (int i = 0; i < n; i++)
        if (best < 0 || w[i].hours < best)
            best = w[i].hours;
    return best;
}

// The ships (Hunters and Starfighters) in a race's garrison at home.
static int32_t garrison_ships(int16_t race, int32_t *hunters, int32_t *fighters)
{
    const TRace *rc = Intel_Race(race);
    const TBody *home = PlanetInfo_BodyAt(rc->HomeSystem, rc->HomeBody, 0);
    *hunters = home ? home->Stores[0] : 0;
    *fighters = home ? home->Stores[1] : 0;
    return *hunters + *fighters;
}

// The hostile race whose wave is nearest, and its Hunters and Starfighters;
// without a wave ahead, the one with the largest garrison at home. With a
// `prefer` (the race that planned the mission) that race, if it is hostile
// and has a wave or a garrison: its own ships set the size. False if no race
// is hostile (or none has ships).
static bool nearest_wave_of(int16_t prefer, int16_t *race, int32_t *hunters, int32_t *fighters)
{
    WaveRef w[8];
    int n = armed_waves(w), best = -1;
    for (int i = 0; i < n; i++)
        if (hostile(w[i].race) && (prefer == 0 || w[i].race == prefer) && w[i].hunters + w[i].fighters > 0
            && (best < 0 || w[i].hours < w[best].hours))
            best = i;
    if (best < 0 && prefer > 0 && hostile(prefer) && garrison_ships(prefer, hunters, fighters) > 0) {
        *race = prefer;
        return true;
    }
    if (best < 0 && prefer > 0)
        return nearest_wave_of(0, race, hunters, fighters);
    if (best >= 0) {
        *race = w[best].race;
        *hunters = w[best].hunters;
        *fighters = w[best].fighters;
        return true;
    }
    int32_t most = 0;
    for (int16_t r = 2; r <= 12; r++) {
        if (!hostile(r))
            continue;
        int32_t h, f;
        if (garrison_ships(r, &h, &f) > most) {
            most = h + f;
            *race = r;
            *hunters = h;
            *fighters = f;
        }
    }
    return most > 0;
}

static bool nearest_wave(int16_t *race, int32_t *hunters, int32_t *fighters)
{
    return nearest_wave_of(0, race, hunters, fighters);
}

// A story fleet is on its way or waits at its target.
static bool story_busy(void)
{
    for (int16_t r = 2; r <= 12; r++) {
        const TRace *rc = Intel_Race(r);
        if (rc->Status != 2)
            continue;
        for (int16_t f = 1; f <= rc->ForceCount && f <= 7; f++) {
            const TRaceForce *fc = Intel_Force(r, f);
            if (fc->NextMission != 0 && (fc->Active == 2 || fc->Active == 1))
                return true;
        }
    }
    return false;
}

// --- Targets -------------------------------------------------------------------

typedef struct Target {
    int16_t system, planet, moon;
    int32_t weight;
} Target;

static int32_t buildings_on(int16_t system, int16_t planet, int16_t moon)
{
    int32_t n = 0;
    for (int16_t i = 0; i < ds.Game_BuildingCount; i++) {
        const TBuilding *b = &ds.Game_Buildings[i];
        n += b->System == system && b->Planet == planet && b->Moon == moon;
    }
    return n;
}

// A colony outside New Earth, drawn by its worth (the ore in store, the
// buildings) and the less its planet is defended the likelier. False if
// the player has none.
static bool draw_target(Target *out)
{
    int64_t total = 0;
    Target found[256];
    int n = 0;
    for (int16_t s = 1; s <= 8 && n < 256; s++) {
        for (int16_t p = 1; p <= ds.Galaxy_PlanetCount[s - 1] && n < 256; p++) {
            int16_t moons = ds.Galaxy_MoonLists[s - 1][p - 1].Count;
            for (int16_t m = 0; m <= moons && n < 256; m++) {
                const TBody *b = PlanetInfo_BodyAt(s, p, m);
                if (!b || b->Owner != 1 || (s == 1 && p == 5 && m == 0))
                    continue;
                int64_t ore = 0;
                for (int k = 0; k < 6; k++)
                    ore += SDL_max(b->Stores[k], 0);
                int64_t worth = 100 + SDL_min(ore / 20, 600) + 8 * buildings_on(s, p, m);
                int32_t defence = Adaptive_DefenceAt(s, p);
                int64_t weight = worth * 100 / (100 + defence);
                found[n++] = (Target){s, p, m, (int32_t)SDL_max(weight, 1)};
                total += found[n - 1].weight;
            }
        }
    }
    if (n == 0)
        return false;
    int64_t pick = System_Random((uint16_t)SDL_min(total, 60000));
    for (int i = 0; i < n; i++) {
        pick -= found[i].weight;
        if (pick < 0 || i == n - 1) {
            *out = found[i];
            return true;
        }
    }
    return false;
}

// --- Planning, the warning, the launch ------------------------------------------------

static const char *mission_name(int mission)
{
    return mission == RAID_SCOUT ? "scout" : mission == RAID_BLOCKADE ? "blockade" : "raid";
}

static void where_is(char *buf, size_t size, int s, int p, int m)
{
    PlanetInfo_GetBodyLongName(buf, size, (int16_t)s, (int16_t)p, (int16_t)m);
}

static bool is_early(void)
{
    return ds.Game_StoryStage <= EARLY_STAGE;
}

// The next mission: its race, kind and target, and when.
static void plan(void)
{
    RaidState *st = &Raid_State;
    int16_t race = 0;
    int32_t hunters = 0, fighters = 0;
    if (!nearest_wave(&race, &hunters, &fighters))
        return;  // no hostile race: nothing to plan, look again tomorrow
    Target t;
    int mission = RAID_RAID;
    if (!draw_target(&t)) {
        // No colony but New Earth: a look at it instead.
        t = (Target){1, 5, 0, 1};
        mission = RAID_SCOUT;
    } else if (Adaptive_DefenceAt(t.system, t.planet) == 0 && System_Random(100) < BLOCKADE_PERCENT) {
        mission = RAID_BLOCKADE;
    }
    st->plan_race = (uint16_t)race;
    st->plan_mission = (uint16_t)mission;
    st->plan_system = (uint16_t)t.system;
    st->plan_planet = (uint16_t)t.planet;
    st->plan_moon = (uint16_t)t.moon;
    int span = config.adaptive.raid_max - config.adaptive.raid_min;
    st->timer = (uint16_t)SDL_clamp(config.adaptive.raid_min + System_Random((uint16_t)SDL_max(span + 1, 1)), 24, 60000);
    st->lead = (uint16_t)(WARN_LEAD_MIN + System_Random(WARN_LEAD_SPAN));
    st->warned = 0;
    st->scout_timer = 0;
    if (mission != RAID_SCOUT && System_Random(100) < SCOUT_PERCENT) {
        int32_t ahead = SCOUT_LEAD_MIN + System_Random(SCOUT_LEAD_SPAN);
        if (st->timer > ahead + 24)
            st->scout_timer = (uint16_t)(st->timer - ahead);
    }
    EvLog_Raid(mission_name(mission), "plan", race, t.system, t.planet, t.moon, 0, 0, st->timer, st->scout_timer);
}

// Another fleet of the race (the planned one) in the plan's words: "<Race>
// fleet is gathering: raid on <where>", the hours and the ships.
static void warn_early(void)
{
    RaidState *st = &Raid_State;
    if (!Adaptive_WarnsOf((int16_t)st->plan_race))
        return;
    int16_t race = 0;
    int32_t hunters = 0, fighters = 0, units[2];
    if (!nearest_wave_of((int16_t)st->plan_race, &race, &hunters, &fighters))
        return;
    int32_t defence = Adaptive_DefenceAt((int16_t)st->plan_system, (int16_t)st->plan_planet);
    Raid_PlanFleet(defence, hunters, fighters, is_early(), units);
    int32_t ships = units[0] + units[1];
    char label[32], where[96], text[360];
    Adaptive_RaceLabel((int16_t)st->plan_race, label, sizeof label);
    where_is(where, sizeof where, st->plan_system, st->plan_planet, st->plan_moon);
    int from = SDL_max((int)st->timer - 24, 1), to = (int)st->timer + 24;
    const char *what = st->plan_mission == RAID_BLOCKADE ? "will blockade it" : "will shell it, not stay";
    SDL_snprintf(text, sizeof text, "     INTELLIGENCE     | %s fleet is gathering |  It %s | %s | Sets off in %d-%d h |"
                 " Ships %d-%d |",
                 label, what, where, from, to, (int)(ships * 7 / 10), (int)((ships * 13 + 9) / 10));
    Message_ShowMessage(text);
}

static int16_t travel_hours(int16_t race, int s, int p)
{
    const TRace *rc = Intel_Race(race);
    if (rc->HomeSystem == s)
        return rc->HomeBody == p ? (int16_t)(System_Random(5) + 6) : (int16_t)(System_Random(10) + 20);
    return (int16_t)(System_Random(20) + 50);
}

static void announce(const char *kind_text, const RaidFleet *f, const char *tail)
{
    char label[32], where[96], text[300];
    Adaptive_RaceLabel((int16_t)f->race, label, sizeof label);
    where_is(where, sizeof where, f->system, f->planet, f->moon);
    SDL_snprintf(text, sizeof text, "   %s | %s fleet | %s | %s |", kind_text, label, where, tail);
    Message_ShowMessage(text);
}

// A fleet leaves its slot.
static void vanish(RaidFleet *f)
{
    SDL_zerop(f);
}

static void launch(int slot, int mission, int race, int s, int p, int m, const int32_t units[2])
{
    RaidFleet *f = &Raid_State.fleet[slot];
    SDL_zerop(f);
    f->phase = RAID_PHASE_FLYING;
    f->mission = (uint16_t)mission;
    f->race = (uint16_t)race;
    f->system = (uint16_t)s;
    f->planet = (uint16_t)p;
    f->moon = (uint16_t)m;
    f->units[0] = f->start[0] = (uint16_t)units[0];
    f->units[1] = f->start[1] = (uint16_t)units[1];
    f->clock = (uint16_t)travel_hours((int16_t)race, s, p);
    EvLog_Raid(mission_name(mission), "launch", race, s, p, m, units[0], units[1], f->clock, 0);
    if (config.adaptive.warn_on_launch) {
        char tail[48];
        SDL_snprintf(tail, sizeof tail, "Arrives in about %d h", (int)f->clock);
        announce(mission == RAID_SCOUT ? "ALIEN SCOUTS" : mission == RAID_BLOCKADE ? "BLOCKADE FLEET" : "ALIEN RAIDERS", f,
                 tail);
    }
}

static bool target_is_ours(int s, int p, int m)
{
    const TBody *b = PlanetInfo_BodyAt((int16_t)s, (int16_t)p, (int16_t)m);
    return b && b->Owner == 1;
}

// The planned raid or blockade sets off, its size from the defence of the target now.
static void launch_planned(void)
{
    RaidState *st = &Raid_State;
    int16_t race = 0;
    int32_t hunters = 0, fighters = 0, units[2];
    int s = st->plan_system, p = st->plan_planet, m = st->plan_moon;
    bool ok = st->plan_mission != RAID_NONE && hostile((int16_t)st->plan_race)
              && target_is_ours(s, p, m) && nearest_wave_of((int16_t)st->plan_race, &race, &hunters, &fighters);
    if (ok) {
        // The race that planned it, if it is still the nearest; else the nearest.
        int32_t defence = Adaptive_DefenceAt((int16_t)s, (int16_t)p);
        Raid_PlanFleet(defence, hunters, fighters, is_early(), units);
        int mission = st->plan_mission == RAID_SCOUT ? RAID_SCOUT : (defence == 0 ? (int)st->plan_mission : RAID_RAID);
        if (mission == RAID_SCOUT) {
            units[0] = UNDEFENDED_MIN;  // a few Hunters look
            units[1] = 0;
        }
        launch(0, mission, st->plan_race, s, p, m, units);
    }
    st->timer = 0;
    st->warned = 0;
    st->plan_mission = RAID_NONE;
}

static void launch_scout(void)
{
    RaidState *st = &Raid_State;
    int32_t units[2] = {UNDEFENDED_MIN, 0};
    if (st->fleet[1].phase == RAID_PHASE_NONE && st->plan_mission != RAID_NONE && hostile((int16_t)st->plan_race)
        && target_is_ours(st->plan_system, st->plan_planet, st->plan_moon))
        launch(1, RAID_SCOUT, st->plan_race, st->plan_system, st->plan_planet, st->plan_moon, units);
}

// --- On the way, at the target -----------------------------------------------------------

// The player's defence of the fleet's target planet, tenths of a Hunter.
static int32_t defence_of(const RaidFleet *f)
{
    return Adaptive_DefenceAt((int16_t)f->system, (int16_t)f->planet);
}

// The fleet has done what it came for, or turns back: it goes home and is gone.
static void leave(int slot, const char *what)
{
    RaidFleet *f = &Raid_State.fleet[slot];
    EvLog_Raid(mission_name(f->mission), what, f->race, f->system, f->planet, f->moon, f->units[0], f->units[1], 0, 0);
    vanish(f);
}

// Delays the next mission: a raid that was chased away without a fight.
static void chased(int slot)
{
    RaidState *st = &Raid_State;
    RaidFleet *f = &st->fleet[slot];
    char label[32], where[96], text[260];
    Adaptive_RaceLabel((int16_t)f->race, label, sizeof label);
    where_is(where, sizeof where, f->system, f->planet, f->moon);
    SDL_snprintf(text, sizeof text, "   The %s fleet turned back | seeing your defence at | %s |", label, where);
    Message_ShowMessage(text);
    int span = config.adaptive.raid_chase_max - config.adaptive.raid_chase_min;
    int delay = config.adaptive.raid_chase_min + System_Random((uint16_t)SDL_max(span + 1, 1));
    if (slot == 0) {
        // Nothing is planned (the raid was): the next is planned after this wait too.
        st->timer = (uint16_t)SDL_clamp(delay, 1, 60000);
        st->plan_mission = RAID_NONE;
        st->warned = 1;
    }
    leave(slot, "chased");
}

// A building a raid can destroy: finished, on the body, not the command centre or a defence.
static bool bombardable(const TBuilding *bl, int16_t s, int16_t p, int16_t m)
{
    return bl->System == s && bl->Planet == p && bl->Moon == m && bl->BuildLeft == 0
           && bl->Kind != COMMAND_CENTRE && bl->Kind != DEFENCE_KIND_ION
           && bl->Kind != DEFENCE_KIND_SHIELD && bl->Kind != DEFENCE_KIND_TWIN;
}

// Shoots at the body from orbit: up to 15% of its buildings (the command
// centre and the defences aside) and 30% of its ore are lost, the more the
// weaker the defence was against the raid.
static void bombard(const RaidFleet *f, int32_t defence)
{
    int32_t adv = Raid_Advantage(Raid_Weight(f->units), defence);
    int16_t s = (int16_t)f->system, p = (int16_t)f->planet, m = (int16_t)f->moon;
    TBody *b = PlanetInfo_BodyAt(s, p, m);
    if (!b || b->Owner != 1)
        return;
    int32_t pm_buildings = System_Random((uint16_t)(BUILDINGS_PERCENT * adv / 10 + 1));  // per mille
    int32_t pm_ore = System_Random((uint16_t)(ORE_PERCENT * adv / 10 + 1));
    int16_t candidates = 0;
    for (int16_t i = 0; i < ds.Game_BuildingCount; i++)
        candidates += bombardable(&ds.Game_Buildings[i], s, p, m);
    int16_t lose = (int16_t)((int32_t)candidates * pm_buildings / 1000), lost = 0;
    for (; lose > 0; lose--) {
        int16_t left = 0;
        for (int16_t i = 0; i < ds.Game_BuildingCount; i++)
            left += bombardable(&ds.Game_Buildings[i], s, p, m);
        if (left == 0)
            break;
        int16_t pick = (int16_t)System_Random((uint16_t)left);
        for (int16_t i = 1; i <= ds.Game_BuildingCount; i++) {
            if (!bombardable(&ds.Game_Buildings[i - 1], s, p, m) || pick-- > 0)
                continue;
            Surface_DeleteBuilding(i);
            lost++;
            break;
        }
    }
    if (lost > 0) {
        // A droid with no mine or Miner station to work at goes with them.
        int16_t places = (int16_t)(Surface_CountBuildings(MINER_STATION, s, p, m, false)
                                   + Surface_CountBuildings(MINE, s, p, m, false));
        if (b->Droids > places)
            b->Droids = (uint8_t)SDL_max(places, 0);
        PlanetInfo_BalanceBuildingPower(s, p, m);
    }
    for (int k = 0; k < 6; k++)
        if (b->Stores[k] > 0)
            b->Stores[k] -= (int32_t)((int64_t)b->Stores[k] * pm_ore / 1000);
    EvLog_Raid("raid", "bombard", f->race, s, p, m, f->units[0], f->units[1], lost, pm_ore);
    char label[32], where[96], text[300];
    Adaptive_RaceLabel((int16_t)f->race, label, sizeof label);
    where_is(where, sizeof where, s, p, m);
    SDL_snprintf(text, sizeof text, "   RAID | The %s shot at | %s | %d buildings lost, ore %d.%d%% |", label, where, (int)lost,
                 (int)(pm_ore / 10), (int)(pm_ore % 10));
    Message_ShowMessage(text);
}

// A battle with the raid at its target.
static bool start_battle(int slot, int32_t defence)
{
    RaidState *st = &Raid_State;
    RaidFleet *f = &st->fleet[slot];
    if (ds.Game_CurrentScreen == 0x1D || ds.Game_ScreenSwitch)
        return false;  // another battle is at hand: try again
    st->in_battle = (uint16_t)(slot + 1);
    st->fled = 0;
    st->defence = (uint16_t)SDL_min(defence, 65535);
    ds.SpWar_BattleSystem = (int16_t)f->system;
    ds.SpWar_BattlePlanet = (int16_t)f->planet;
    ds.SpWar_BattleMoon = (int16_t)f->moon;
    ds.SpWar_RaceMissionHigh = 0;
    ds.SpWar_GroundBattle = 0;
    ds.Sim_PlayerAttacks = 0;
    ds.SpWar_EnemyRace = (int16_t)f->race;
    ds.Game_CurrentScreen = 0x1D;
    ds.Game_ScreenSwitch = 1;
    EvLog_Raid(mission_name(f->mission), "battle", f->race, f->system, f->planet, f->moon, f->units[0], f->units[1],
               (int)defence, Raid_Weight(f->units));
    return true;
}

// The raid or blockade meets whatever the player has at its target. True if
// it is settled for now (it left, a battle began, it shot at the body).
static bool confront(int slot)
{
    RaidState *st = &Raid_State;
    RaidFleet *f = &st->fleet[slot];
    int32_t defence = defence_of(f), weight = Raid_Weight(f->units);
    if (!target_is_ours(f->system, f->planet, f->moon)) {
        leave(slot, "leave");  // the colony is gone
        return true;
    }
    if ((int64_t)defence * 100 >= (int64_t)weight * OUTMATCH_PERCENT) {
        chased(slot);
        return true;
    }
    if (defence == 0) {
        if (f->mission == RAID_BLOCKADE) {
            if (f->phase != RAID_PHASE_STAYING) {
                f->phase = RAID_PHASE_STAYING;
                f->clock = (uint16_t)SDL_clamp(config.adaptive.blockade_max, 1, 60000);
                announce("BLOCKADE", f, "No ore will move there");
                EvLog_Raid("blockade", "hold", f->race, f->system, f->planet, f->moon, f->units[0], f->units[1], 0, 0);
            }
        } else {
            if (ds.Game_CurrentScreen == 0x14)
                return false;  // the surface is on screen: not while its map is drawn (as a meteorite waits)
            bombard(f, 0);
            leave(slot, "leave");
        }
        return true;
    }
    return start_battle(slot, defence);
}

// A blockade nobody broke ends: the fleet goes home, the next mission waits a while.
static void lift(int slot)
{
    RaidState *st = &Raid_State;
    int span = config.adaptive.raid_chase_max - config.adaptive.raid_chase_min;
    int delay = config.adaptive.raid_chase_min + System_Random((uint16_t)SDL_max(span + 1, 1));
    if (slot == 0) {
        st->timer = (uint16_t)SDL_clamp(delay, 1, 60000);
        st->plan_mission = RAID_NONE;
        st->warned = 1;
    }
    announce("BLOCKADE", &st->fleet[slot], "Lifted");
    leave(slot, "lift");
}

static void scout_hour(int slot)
{
    RaidFleet *f = &Raid_State.fleet[slot];
    if (defence_of(f) > 0 || !target_is_ours(f->system, f->planet, f->moon)) {
        leave(slot, "leave");  // seen: it never fights
        return;
    }
    if (f->clock > 0)
        f->clock--;
    if (f->clock == 0)
        leave(slot, "leave");
}

static void arrive(int slot)
{
    RaidFleet *f = &Raid_State.fleet[slot];
    EvLog_Raid(mission_name(f->mission), "arrive", f->race, f->system, f->planet, f->moon, f->units[0], f->units[1], 0, 0);
    if (f->mission == RAID_SCOUT) {
        f->phase = RAID_PHASE_STAYING;
        f->clock = SCOUT_STAY;
        announce("ALIEN SCOUTS", f, "A raid will follow");
        return;
    }
    f->phase = RAID_PHASE_WAITING;
    int speed = Intel_Race((int16_t)f->race)->Speed;
    f->clock = (uint16_t)(System_Random((uint16_t)SDL_max(100 - speed, 1)) + 5);
    announce(f->mission == RAID_BLOCKADE ? "BLOCKADE" : "ATTENTION", f, f->mission == RAID_BLOCKADE ? "Arrived" : "Raiders have arrived");
}

static void fleet_hour(int slot)
{
    RaidFleet *f = &Raid_State.fleet[slot];
    switch (f->phase) {
    case RAID_PHASE_FLYING:
        if (f->clock > 0)
            f->clock--;
        if (f->clock == 0)
            arrive(slot);
        break;
    case RAID_PHASE_WAITING:
        if (f->clock > 1) {
            f->clock--;
            break;
        }
        if (!confront(slot))
            f->clock = 1;  // another battle is at hand: the next hour
        break;
    case RAID_PHASE_STAYING:
        if (f->mission == RAID_SCOUT)
            scout_hour(slot);
        else if (defence_of(f) > 0 || !target_is_ours(f->system, f->planet, f->moon))
            confront(slot);
        else if (f->clock > 0) {
            // Nobody came for it. It goes when its time is up or the story's next
            // wave is about to set off; every few days the player is reminded.
            int32_t wave = hours_to_wave();
            if (--f->clock == 0 || (wave >= 0 && wave <= BLOCKADE_LEAVES_BEFORE))
                lift(slot);
            else if (f->clock % BLOCKADE_REMINDER == 0)
                announce("BLOCKADE", f, "Still in force");
        }
        break;
    default:
        break;
    }
}

void Raid_Hour(void)
{
    if (!active())
        return;
    RaidState *st = &Raid_State;
    if (story_busy())
        st->quiet = (uint16_t)SDL_clamp(config.adaptive.raid_gap, 0, 60000);
    else if (st->quiet > 0)
        st->quiet--;

    if (st->in_battle == 0)
        for (int i = 0; i < RAID_FLEETS; i++)
            fleet_hour(i);

    if (st->scout_timer > 0 && --st->scout_timer == 0)
        launch_scout();
    if (st->timer == 0) {
        if (ds.Game_Hour == 0 && Raid_State.fleet[0].phase == RAID_PHASE_NONE)
            plan();  // once a day, while nothing is planned
        return;
    }
    if (!st->warned && st->timer <= st->lead && st->plan_mission != RAID_NONE) {
        st->warned = 1;
        warn_early();
    }
    if (st->timer > 1) {
        st->timer--;
        return;
    }
    // Due: but not near the story's waves, and not with a fleet of the kind out.
    int32_t wave = hours_to_wave();
    bool near_wave = wave >= 0 && wave <= config.adaptive.raid_gap;
    if (st->quiet > 0 || near_wave || st->fleet[0].phase != RAID_PHASE_NONE)
        return;
    launch_planned();
}

bool Raid_Blockaded(int16_t system, int16_t planet, int16_t moon)
{
    if (!improvements.raids)
        return false;
    for (int i = 0; i < RAID_FLEETS; i++) {
        const RaidFleet *f = &Raid_State.fleet[i];
        if (f->phase == RAID_PHASE_STAYING && f->mission == RAID_BLOCKADE && f->system == system
            && f->planet == planet && f->moon == moon)
            return true;
    }
    return false;
}

bool Raid_Any(void)
{
    const RaidState *st = &Raid_State;
    if (st->timer != 0 || st->scout_timer != 0)
        return true;
    for (int i = 0; i < RAID_FLEETS; i++)
        if (st->fleet[i].phase != RAID_PHASE_NONE)
            return true;
    return false;
}

// --- The battle ----------------------------------------------------------------------

bool Raid_BattleActive(void)
{
    return improvements.raids && Raid_State.in_battle >= 1 && Raid_State.in_battle <= RAID_FLEETS;
}

const RaidFleet *Raid_BattleFleet(void)
{
    return Raid_BattleActive() ? &Raid_State.fleet[Raid_State.in_battle - 1] : nullptr;
}

bool Raid_RetreatCheck(const int32_t alive[2])
{
    if (!Raid_BattleActive() || Raid_State.fled)
        return false;
    RaidFleet *f = &Raid_State.fleet[Raid_State.in_battle - 1];
    int32_t start = Raid_Weight(f->start), left = alive[0] * class_weight(0) + alive[1] * class_weight(1);
    if (start <= 0 || left * 2 >= start || left <= 0)
        return false;
    // Half its weight lost: it turns back with what it has.
    Raid_State.fled = 1;
    f->units[0] = (uint16_t)SDL_max(alive[0], 0);
    f->units[1] = (uint16_t)SDL_max(alive[1], 0);
    return true;
}

void Raid_ApplyLosses(const int32_t alive[2], int32_t losses[4])
{
    if (!Raid_BattleActive())
        return;
    RaidFleet *f = &Raid_State.fleet[Raid_State.in_battle - 1];
    int32_t now[2] = {alive[0], alive[1]};
    if (Raid_State.fled) {
        now[0] = f->units[0];  // the ones that turned back are not dead
        now[1] = f->units[1];
        // The ships that left the battle were not counted as lost.
        for (int c = 0; c < 2; c++)
            losses[c] += SDL_max((int32_t)f->start[c] - (int32_t)f->units[c], 0);
    } else {
        for (int c = 0; c < 2; c++)
            losses[c] += SDL_max((int32_t)f->units[c] - now[c], 0);
        f->units[0] = (uint16_t)SDL_max(now[0], 0);
        f->units[1] = (uint16_t)SDL_max(now[1], 0);
    }
}

bool Raid_BattleOver(bool playerWon)
{
    RaidState *st = &Raid_State;
    if (!Raid_BattleActive())
        return false;
    int slot = st->in_battle - 1;
    RaidFleet *f = &st->fleet[slot];
    bool fled = st->fled != 0;
    st->in_battle = 0;
    st->fled = 0;
    if (fled) {
        char label[32], text[200];
        Adaptive_RaceLabel((int16_t)f->race, label, sizeof label);
        SDL_snprintf(text, sizeof text, "   The %s fleet withdrew |", label);
        Message_ShowMessage(text);
        leave(slot, "withdrew");
        return true;
    }
    if (f->units[0] + f->units[1] == 0) {
        leave(slot, "destroyed");
        return true;
    }
    // The player lost what he had there: the raiders have the sky.
    if (f->mission == RAID_BLOCKADE) {
        f->phase = RAID_PHASE_STAYING;
        announce("BLOCKADE", f, "Your ships are gone");
    } else {
        bombard(f, st->defence);
        leave(slot, "leave");
    }
    (void)playerWon;
    return true;
}
