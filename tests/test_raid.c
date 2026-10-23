// Improvement (game/improvements.h, raids): game/raid.h. The size of a raid
// (a share of the nearest wave, from the defence of the target); the
// schedule (800-1200 h, kept off the story's waves and fleets); the fleet's
// way (flight, waiting, the player's strength, a body with no defence shot
// at, a chased-off raid, a blockade that stops the ore); the battle (the raid
// in the defenders' list, its retreat at half its weight, what is left of
// it); the state in a save. With the switch off nothing is there. Needs the
// game data.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "game/adaptive.h"
#include "game/cargo.h"
#include "game/defence.h"
#include "game/dseg.h"
#include "game/gfx.h"
#include "game/groups.h"
#include "game/improvements.h"
#include "game/intel.h"
#include "game/main_program.h"
#include "game/message.h"
#include "game/planetinfo.h"
#include "game/raid.h"
#include "game/saveload.h"
#include "game/spwar.h"
#include "game/system.h"
#include "game/textfile.h"
#include "platform/config.h"
#include "platform/fs.h"
#include "platform/video.h"

static bool ok = true;
#define CHECK(c) do { if (!(c)) { printf("FAILED line %d: %s\n", __LINE__, #c); ok = false; } } while (0)

enum { MORGRULS = 3 };

static bool wait_retrace(void) { return true; }
static bool key_pressed(void) { return true; }
static char read_key(void) { return '\r'; }

static char dir[128];

static void setup(bool on, bool zero_story)
{
    dseg_clear();
    vid_clear();
    dseg_init();
    system_init();
    Game_InitGameData();
    Message_Init();
    SaveLoad_LoadInit();
    Adaptive_Reset();
    Raid_Reset();
    config_reset();
    ds.Gfx_LineStride = 320;
    ds.Gfx_LineStrideCopy = 320;
    gfx_wait_retrace = wait_retrace;
    TextFile_KeyPressed = key_pressed;
    TextFile_ReadKey = read_key;
    ds.SpWar_View = game_alloc(0x5E60);
    ds.SpWar_Background = game_alloc(0x5E60);
    ds.SpWar_Sprites = game_alloc(0xF00);
    ds.SpWar_Face = game_alloc(0x1180);
    ds.SpWar_AttackerList = game_alloc(0x177C);
    ds.SpWar_DefenderList = game_alloc(0x177C);
    ds.SpWar_StepNo = 0;
    ds.SpWar_BattleOver = 0;
    ds.SpWar_AttackerShipCount = ds.SpWar_DefenderShipCount = 0;
    ds.Message_LogCount = 0;
    ds.Game_GroupCount = 0;
    ds.Game_PlanetForceCount = 0;
    ds.Game_BuildingCount = 0;
    ds.Game_Credits = 0;
    ds.Game_CurrentScreen = 1;
    ds.Game_ScreenSwitch = 0;
    ds.Game_Hour = 5;
    ds.System_RandSeed = 4321;
    improvements.adaptive_threat = on;
    improvements.raids = on;
    config.adaptive.warn_on_launch = false;
    if (!zero_story)
        return;
    // The story's timers all quiet; Morgruls hostile, with a wave far ahead.
    ds.Intel_MorgrulAttack1Timer = ds.Intel_MorgrulAttack2Timer = ds.Intel_MorgrulAttack3Timer = 0;
    ds.Intel_JointAttackTimer = ds.Intel_Race7to10Timer = ds.Intel_LaunchTimer = 0;
    ds.Intel_MorgrulAttack2Done = 0;
    ds.Intel_MorgrulAttack2Timer = 9000;
    for (int r = 0; r < 11; r++) {
        ds.Races_Races[r].ForceCount = 7;
        for (int f = 0; f < 7; f++) {
            TRaceForce *fc = &ds.Races_Races[r].Forces[f];
            fc->Active = 0;
            fc->NextMission = 0;
        }
    }
    Intel_Race(MORGRULS)->Status = 2;
    Intel_Race(MORGRULS)->Alive = 1;
    Intel_Force(MORGRULS, 2)->Units[0] = 25;  // wave 2: 25 Hunters
    Intel_Force(MORGRULS, 2)->Units[1] = 0;
}

static void reset(bool on)
{
    setup(on, true);
}

static TBody *colony(int16_t system, int16_t planet, int16_t moon)
{
    TBody *b = PlanetInfo_BodyAt(system, planet, moon);
    b->Owner = 1;
    b->Colony = 1;
    for (int k = 0; k < 6; k++)
        b->Stores[k] = 1000 * (k + 1);
    return b;
}

static void add_building(int16_t kind, int16_t system, int16_t planet, int16_t moon, bool powered)
{
    TBuilding *b = &ds.Game_Buildings[ds.Game_BuildingCount++];
    memset(b, 0, sizeof *b);
    b->Kind = (uint8_t)kind;
    b->System = (uint8_t)system;
    b->Planet = (uint8_t)planet;
    b->Moon = (uint8_t)moon;
    b->Powered = powered;
}

static void hours(int n)
{
    for (int i = 0; i < n; i++) {
        ds.Game_Hour = (uint8_t)((ds.Game_Hour + 1) % 24);
        Raid_Hour();
    }
}

// A player's group of `count` Hunters-worth of ships at the planet.
static void ships_at(int16_t system, int16_t planet, int16_t count)
{
    TGroup *g = Groups_At(0, 1);
    memset(g, 0, sizeof *g);
    g->Kind = 1;
    g->System = (uint8_t)system;
    g->Planet = (uint8_t)planet;
    g->Status = 1;
    g->Units[0][0].Count = count;
    g->Units[0][0].Weapons[0] = (int16_t)(count * 2);
    g->Units[0][0].Weapons[1] = (int16_t)(count * 3);
    ds.Game_GroupCount = 1;
}

static int count_raid_ships(void)
{
    int n = 0;
    for (int i = 1; i <= ds.SpWar_DefenderShipCount; i++)
        n += ((const SpShip *)ds.SpWar_DefenderList)[i - 1].id == RAID_SHIP_ID;
    return n;
}

static void planning(void)
{
    reset(true);
    int32_t u[2];
    int wh = (int)Raid_Weight((const uint16_t[2]){1, 0}), wf = (int)Raid_Weight((const uint16_t[2]){0, 1});
    CHECK(wh == 10 && wf >= 38 && wf <= 40);

    // No defence at all: 3 to 6 Hunters.
    int seen[8] = {};
    for (int i = 0; i < 200; i++) {
        Raid_PlanFleet(0, 25, 0, true, u);
        CHECK(u[0] >= 3 && u[0] <= 6 && u[1] == 0);
        seen[u[0]]++;
    }
    CHECK(seen[3] && seen[4] && seen[5] && seen[6]);

    // 30% of 25 Hunters = 75 tenths of weight: a defence of 40 wants 1.0-1.5 x 40 = 40-60,
    // lifted to the floor of half the ceiling (37), within the ceiling.
    for (int i = 0; i < 200; i++) {
        Raid_PlanFleet(40, 25, 0, true, u);
        int32_t w = u[0] * 10 + u[1] * wf;
        CHECK(w >= 38 && w <= 76 && u[1] == 0);
    }
    // A defence beyond the ceiling is held at the ceiling (about 7-8 Hunters).
    for (int i = 0; i < 50; i++) {
        Raid_PlanFleet(100000, 25, 0, true, u);
        CHECK(u[0] >= 7 && u[0] <= 8);
    }
    // A small defence: the floor, half the ceiling (about 4 Hunters).
    for (int i = 0; i < 50; i++) {
        Raid_PlanFleet(5, 25, 0, false, u);
        CHECK(u[0] >= 3 && u[0] <= 5);
    }
    // Later stages 1.2-1.8: a defence of 50 draws 60-90, the ceiling of 25 H being 75.
    int32_t lo = 100000, hi = 0;
    for (int i = 0; i < 400; i++) {
        Raid_PlanFleet(50, 25, 0, false, u);
        int32_t w = u[0] * 10;
        lo = SDL_min(lo, w);
        hi = SDL_max(hi, w);
    }
    CHECK(lo >= 60 && lo <= 70 && hi >= 70 && hi <= 80);
    // The mix follows the wave: Hunters and Starfighters in the proportion of their weights.
    int sf_total = 0, h_total = 0;
    for (int i = 0; i < 100; i++) {
        Raid_PlanFleet(400, 40, 40, false, u);
        sf_total += u[1];
        h_total += u[0];
    }
    CHECK(sf_total > 0 && h_total > 0 && sf_total * wf > h_total * 10 * 3 / 2);  // SF weigh ~4x a Hunter: ~80% of the weight
    // Always at least a ship.
    Raid_PlanFleet(1, 1, 0, true, u);
    CHECK(u[0] + u[1] >= 1);

    CHECK(Raid_Advantage(100, 0) == 100 && Raid_Advantage(100, 50) == 50);
    CHECK(Raid_Advantage(100, 99) == 10 && Raid_Advantage(100, 500) == 10 && Raid_Advantage(0, 0) == 10);
}

static void schedule_and_flight(void)
{
    // Nothing without a hostile race with ships, or a colony: a scout of New Earth.
    reset(true);
    Intel_Race(MORGRULS)->Status = 0;
    ds.Game_Hour = 23;
    hours(1);
    CHECK(Raid_State.timer == 0 && !Raid_Any());

    reset(true);
    ds.Game_Hour = 23;
    hours(1);  // hour 0: a mission is planned
    CHECK(Raid_State.timer >= 800 && Raid_State.timer <= 1200);
    CHECK(Raid_State.plan_mission == RAID_SCOUT && Raid_State.plan_race == MORGRULS);  // only New Earth to look at
    CHECK(Raid_State.plan_system == 1 && Raid_State.plan_planet == 5);

    // A colony: a raid, or a blockade of a body with no defence.
    int blockades = 0, raids = 0;
    for (int i = 0; i < 60; i++) {
        reset(true);
        ds.System_RandSeed = 100 + i;
        colony(1, 3, 0);
        ds.Game_Hour = 23;
        hours(1);
        CHECK(Raid_State.plan_system == 1 && Raid_State.plan_planet == 3);
        blockades += Raid_State.plan_mission == RAID_BLOCKADE;
        raids += Raid_State.plan_mission == RAID_RAID;
    }
    CHECK(blockades > 10 && raids > 20 && blockades + raids == 60);

    // With the switch off, or without adaptive_threat, nothing is planned.
    reset(true);
    colony(1, 3, 0);
    improvements.raids = false;
    ds.Game_Hour = 23;
    hours(1);
    CHECK(Raid_State.timer == 0);
    improvements.raids = true;
    improvements.adaptive_threat = false;
    hours(24);
    CHECK(Raid_State.timer == 0 && !Raid_Any());

    // The flight: it sets off when the timer runs out, a raid on an undefended
    // body of 3-6 Hunters; it is on its way 50-69 h (another system than its home).
    reset(true);
    colony(1, 3, 0);
    ds.Game_Hour = 23;
    hours(1);
    int mission = Raid_State.plan_mission, planned = Raid_State.timer;
    hours(planned - 1);
    CHECK(Raid_State.fleet[0].phase == RAID_PHASE_NONE);
    hours(1);
    const RaidFleet *f = &Raid_State.fleet[0];
    CHECK(f->phase == RAID_PHASE_FLYING && f->mission == (uint16_t)mission && f->race == MORGRULS);
    CHECK(f->units[0] >= 3 && f->units[0] <= 6 && f->units[1] == 0);
    CHECK(f->clock >= 6 && f->clock <= 69);
    CHECK(Raid_State.timer == 0);
}

// The story's waves keep the raids away: near one, or with a fleet of its own out.
static void gaps(void)
{
    reset(true);
    colony(1, 3, 0);
    ds.Game_Hour = 23;
    hours(1);
    int planned = Raid_State.timer;
    ds.Intel_MorgrulAttack2Timer = 150;  // a wave 150 h ahead (<= 200)
    hours(planned + 50);
    CHECK(Raid_State.fleet[0].phase == RAID_PHASE_NONE && Raid_State.timer == 1);  // due, held
    ds.Intel_MorgrulAttack2Timer = 9000;
    hours(1);
    CHECK(Raid_State.fleet[0].phase == RAID_PHASE_FLYING);  // the window is past

    // A story fleet in flight holds it, and 200 h after.
    reset(true);
    colony(1, 3, 0);
    ds.Game_Hour = 23;
    hours(1);
    planned = Raid_State.timer;
    hours(planned - 2);
    TRaceForce *fc = Intel_Force(MORGRULS, 1);
    fc->Active = 2;
    fc->NextMission = 2;
    hours(10);
    CHECK(Raid_State.fleet[0].phase == RAID_PHASE_NONE && Raid_State.quiet == (uint16_t)config.adaptive.raid_gap);
    fc->Active = 0;
    fc->NextMission = 0;
    hours(config.adaptive.raid_gap - 1);
    CHECK(Raid_State.fleet[0].phase == RAID_PHASE_NONE && Raid_State.quiet == 1);
    hours(2);
    CHECK(Raid_State.fleet[0].phase == RAID_PHASE_FLYING);
}

// A launched raid at its target at once.
static RaidFleet *arrive_at(int mission, int16_t system, int16_t planet, int16_t moon, int h, int sf)
{
    RaidFleet *f = &Raid_State.fleet[0];
    memset(f, 0, sizeof *f);
    f->phase = RAID_PHASE_WAITING;
    f->mission = (uint16_t)mission;
    f->race = MORGRULS;
    f->system = (uint16_t)system;
    f->planet = (uint16_t)planet;
    f->moon = (uint16_t)moon;
    f->units[0] = f->start[0] = (uint16_t)h;
    f->units[1] = f->start[1] = (uint16_t)sf;
    f->clock = 1;
    return f;
}

static int count_kind(int16_t kind, int16_t s, int16_t p, int16_t m)
{
    int n = 0;
    for (int i = 0; i < ds.Game_BuildingCount; i++)
        n += ds.Game_Buildings[i].Kind == kind && ds.Game_Buildings[i].System == s && ds.Game_Buildings[i].Planet == p
             && ds.Game_Buildings[i].Moon == m;
    return n;
}

// The story's own state at the start of a game, nothing zeroed by the test: its
// forces hold no raid back, and a hostile race plans one.
static void fresh_story(void)
{
    setup(true, false);  // the original's races, forces and timers as they start
    Intel_Race(MORGRULS)->Status = 2;
    Intel_Race(MORGRULS)->Alive = 1;
    colony(1, 3, 0);
    ds.Game_Hour = 23;
    hours(1);
    CHECK(Raid_State.timer >= 800 && Raid_State.plan_race == MORGRULS);
    int guard = 0;
    while (Raid_State.fleet[0].phase == RAID_PHASE_NONE && guard++ < 1300)
        hours(1);
    CHECK(Raid_State.fleet[0].phase == RAID_PHASE_FLYING && Raid_State.fleet[0].race == MORGRULS);
    CHECK(Raid_State.fleet[0].units[0] >= 3);
}

// The surface screen of the target is open: the raid shoots when it is closed.
static void surface_open(void)
{
    reset(true);
    colony(1, 3, 0);
    for (int i = 0; i < 40; i++)
        add_building(7, 1, 3, 0, true);
    arrive_at(RAID_RAID, 1, 3, 0, 5, 0);
    ds.Game_CurrentScreen = 0x14;
    hours(3);
    CHECK(Raid_State.fleet[0].phase == RAID_PHASE_WAITING && count_kind(7, 1, 3, 0) == 40);
    ds.Game_CurrentScreen = 1;
    hours(1);
    CHECK(Raid_State.fleet[0].phase == RAID_PHASE_NONE);
}

static void bombardment(void)
{
    // An undefended colony is shot at: at most 15% of its buildings (not the
    // command centre), 30% of the ore; the fleet is gone.
    for (int run = 0; run < 40; run++) {
        reset(true);
        ds.System_RandSeed = 7 + run;
        TBody *b = colony(1, 3, 0);
        add_building(1, 1, 3, 0, true);  // the command centre
        for (int i = 0; i < 40; i++) {
            TBuilding *bl = &ds.Game_Buildings[ds.Game_BuildingCount];
            add_building(7, 1, 3, 0, true);
            bl->BuildLeft = 0;
        }
        arrive_at(RAID_RAID, 1, 3, 0, 5, 0);
        hours(1);
        CHECK(Raid_State.fleet[0].phase == RAID_PHASE_NONE);
        int left = count_kind(7, 1, 3, 0);
        CHECK(left >= 34 && left <= 40);          // <= 15% of 40
        CHECK(count_kind(1, 1, 3, 0) == 1);       // the command centre stays
        for (int k = 0; k < 6; k++)
            CHECK(b->Stores[k] >= 1000 * (k + 1) * 70 / 100 && b->Stores[k] <= 1000 * (k + 1));
        CHECK(ds.Message_LogCount > 0);
    }
    // ... on average a good part of the cap: it is a draw from nothing to it.
    int lost_sum = 0;
    for (int run = 0; run < 100; run++) {
        reset(true);
        ds.System_RandSeed = 900 + run;
        colony(1, 3, 0);
        for (int i = 0; i < 100; i++)
            add_building(7, 1, 3, 0, true);
        arrive_at(RAID_RAID, 1, 3, 0, 5, 0);
        hours(1);
        lost_sum += 100 - count_kind(7, 1, 3, 0);
    }
    CHECK(lost_sum > 100 * 4 && lost_sum < 100 * 11);  // about 7.5 of 100 on average

    // Batteries (the defences) are not shot at from orbit (they fight, or are not there).
    reset(true);
    colony(1, 3, 0);
    add_building(DEFENCE_KIND_ION, 1, 3, 0, false);  // unpowered: not defending
    ds.Game_Buildings[0].BuildLeft = 0;
    arrive_at(RAID_RAID, 1, 3, 0, 6, 0);
    hours(1);
    CHECK(count_kind(DEFENCE_KIND_ION, 1, 3, 0) == 1);
}

static void chase_and_battle(void)
{
    // A player 1.5 x as strong at the target: the raid turns back, no fight,
    // the next one later; nothing planned any more.
    reset(true);
    colony(1, 3, 0);
    ships_at(1, 3, 30);
    CHECK(Adaptive_DefenceAt(1, 3) > 100);
    RaidFleet *f = arrive_at(RAID_RAID, 1, 3, 0, 6, 0);
    Raid_State.plan_mission = RAID_RAID;
    (void)f;
    hours(1);
    CHECK(Raid_State.fleet[0].phase == RAID_PHASE_NONE && !Raid_BattleActive() && ds.Game_CurrentScreen == 1);
    CHECK(Raid_State.timer >= config.adaptive.raid_chase_min && Raid_State.timer <= config.adaptive.raid_chase_max);
    CHECK(Raid_State.plan_mission == RAID_NONE);
    CHECK(ds.Message_LogCount > 0);

    // A weaker player than that: a space battle, on the battle screen.
    reset(true);
    colony(1, 3, 0);
    ships_at(1, 3, 3);  // 3 Hunters of ships: about 30+ tenths
    int32_t defence = Adaptive_DefenceAt(1, 3);
    CHECK(defence > 0 && defence < 100);
    arrive_at(RAID_RAID, 1, 3, 0, 8, 0);
    CHECK(Raid_Weight(Raid_State.fleet[0].units) > defence);
    hours(1);
    CHECK(Raid_BattleActive() && ds.Game_CurrentScreen == 0x1D && ds.Game_ScreenSwitch);
    CHECK(ds.SpWar_BattleSystem == 1 && ds.SpWar_BattlePlanet == 3 && ds.SpWar_GroundBattle == 0);
    CHECK(ds.SpWar_EnemyRace == MORGRULS);
    CHECK(Raid_State.defence == defence);

    // The fleets: the raid among the defenders, the player's ships against it.
    SpWar_InitSpaceFleets();
    CHECK(ds.SpWar_DefenderShipCount == 8 && ds.SpWar_AttackerShipCount == 3);
    CHECK(count_raid_ships() == 8);

    // The retreat: at half its weight (80 tenths of 8 Hunters) it turns back.
    int32_t alive[2] = {5, 0};
    CHECK(!Raid_RetreatCheck(alive));  // 50 left
    alive[0] = 3;
    CHECK(Raid_RetreatCheck(alive) && Raid_State.fled && Raid_State.fleet[0].units[0] == 3);
    CHECK(!Raid_RetreatCheck(alive));  // once
    int32_t losses[4] = {0, 0, 0, 0};
    alive[0] = 0;  // the list is empty by then
    Raid_ApplyLosses(alive, losses);
    CHECK(losses[0] == 5 && losses[1] == 0 && Raid_State.fleet[0].units[0] == 3);
    ds.Message_LogCount = 0;
    CHECK(Raid_BattleOver(true));
    CHECK(Raid_State.fleet[0].phase == RAID_PHASE_NONE && !Raid_BattleActive() && ds.Message_LogCount > 0);
    CHECK(!Raid_BattleOver(true));  // no raid's battle any more
}

// A real battle to its end, as screen_space_battle does it.
static void fight(void)
{
    ds.SpWar_StepNo = 0;
    for (int n = 0; n < 400000 && ds.SpWar_AttackerShipCount > 0 && ds.SpWar_DefenderShipCount > 0; n++) {
        ds.SpWar_BattleOver = 1;  // the step that ends it draws a result screen
        SpWar_StepSpaceBattle();
    }
}

static void battles(void)
{
    int fled = 0, destroyed = 0, lost = 0, holds = 0;
    for (int run = 0; run < 60; run++) {
        reset(true);
        ds.System_RandSeed = 31 + run * 7;
        colony(1, 3, 0);
        for (int i = 0; i < 30; i++)
            add_building(7, 1, 3, 0, true);
        ships_at(1, 3, 7);
        arrive_at(RAID_RAID, 1, 3, 0, 8, 0);
        hours(1);
        CHECK(Raid_BattleActive());
        SpWar_InitSpaceFleets();
        fight();
        int32_t start_weight = Raid_Weight(Raid_State.fleet[0].start);
        bool player_alive = ds.SpWar_AttackerShipCount > 0, raid_fled = Raid_State.fled != 0;
        SpWar_ApplySpaceLosses();
        int survivors = Raid_State.fleet[0].units[0];
        if (raid_fled) {
            fled++;
            CHECK(player_alive && ds.SpWar_DefenderShipCount == 0);
            CHECK(survivors >= 1 && survivors * 10 * 2 < start_weight);  // under half its weight
            CHECK(ds.SpWar_DefenderLosses[0] == 8 - survivors);
        } else if (survivors == 0) {
            destroyed++;
            CHECK(player_alive && ds.SpWar_DefenderLosses[0] == 8);
        } else {
            lost++;
            CHECK(!player_alive);
        }
        ds.Message_LogCount = 0;
        int buildings = count_kind(7, 1, 3, 0);
        CHECK(Raid_BattleOver(player_alive));
        CHECK(!Raid_BattleActive() && Raid_State.fleet[0].phase == RAID_PHASE_NONE);
        if (raid_fled || survivors == 0) {
            CHECK(count_kind(7, 1, 3, 0) == buildings);  // no shooting at the colony
        } else {
            CHECK(ds.Message_LogCount > 0);              // the raiders have the sky: they shoot
            CHECK(count_kind(7, 1, 3, 0) >= buildings * 85 / 100 - 1);
        }
        holds += player_alive;
    }
    // The raid leaves at half its weight, so it is seldom destroyed; the player wins or loses about evenly here.
    CHECK(fled > 5 && lost > 5 && holds == fled + destroyed);

    // The story's hooks are not run for a raid: the battle-over handler reads Raid_BattleOver.
    reset(true);
    CHECK(!Raid_BattleOver(true));
}

static void blockade(void)
{
    reset(true);
    TBody *b = colony(1, 3, 0);
    int32_t ore[6];
    for (int k = 0; k < 6; k++)
        ore[k] = b->Stores[k];
    arrive_at(RAID_BLOCKADE, 1, 3, 0, 5, 0);
    hours(1);
    RaidFleet *f = &Raid_State.fleet[0];
    CHECK(f->phase == RAID_PHASE_STAYING && Raid_Blockaded(1, 3, 0));
    CHECK(!Raid_Blockaded(1, 3, 1) && !Raid_Blockaded(1, 4, 0) && !Raid_Blockaded(2, 3, 0));

    // Nothing is loaded or unloaded there (the ore stays, whatever the ship).
    TGroup g;
    memset(&g, 0, sizeof g);
    g.Kind = 4;
    g.System = 1;
    g.Planet = 3;
    g.Status = 1;
    CHECK(Cargo_MoveAllOre(&g, true) == 0);
    for (int k = 0; k < 6; k++)
        CHECK(b->Stores[k] == ore[k]);
    improvements.raids = false;
    CHECK(!Raid_Blockaded(1, 3, 0));
    improvements.raids = true;

    // Nobody comes: after blockade_max hours it is lifted.
    {
        CHECK(f->clock == config.adaptive.blockade_max);
        f->clock = 300;
        hours(299);
        CHECK(Raid_Blockaded(1, 3, 0));
        hours(2);
        CHECK(!Raid_Blockaded(1, 3, 0) && Raid_State.fleet[0].phase == RAID_PHASE_NONE);
    }
    reset(true);
    b = colony(1, 3, 0);
    arrive_at(RAID_BLOCKADE, 1, 3, 0, 5, 0);
    hours(1);

    // It stays for days; a player with ships there and enough of them chases it.
    hours(200);
    CHECK(Raid_State.fleet[0].phase == RAID_PHASE_STAYING);
    ships_at(1, 3, 30);
    hours(1);
    CHECK(Raid_State.fleet[0].phase == RAID_PHASE_NONE && !Raid_Blockaded(1, 3, 0));
    CHECK(Raid_State.timer >= config.adaptive.raid_chase_min);

    // The colony is gone (taken by an enemy): the blockade lifts.
    reset(true);
    b = colony(1, 3, 0);
    arrive_at(RAID_BLOCKADE, 1, 3, 0, 5, 0);
    hours(1);
    CHECK(Raid_Blockaded(1, 3, 0));
    b->Owner = 0;
    hours(1);
    CHECK(!Raid_Blockaded(1, 3, 0) && Raid_State.fleet[0].phase == RAID_PHASE_NONE);

    // A weak defence meets the blockade in a battle.
    reset(true);
    colony(1, 3, 0);
    arrive_at(RAID_BLOCKADE, 1, 3, 0, 5, 0);
    hours(1);
    ships_at(1, 3, 3);
    hours(1);
    CHECK(Raid_BattleActive() && ds.Game_CurrentScreen == 0x1D);
    // The blockade wins when the player is gone: it stays.
    SpWar_InitSpaceFleets();
    ds.SpWar_AttackerShipCount = 0;
    SpWar_ApplySpaceLosses();
    CHECK(Raid_BattleOver(false));
    CHECK(Raid_State.fleet[0].phase == RAID_PHASE_STAYING && Raid_Blockaded(1, 3, 0));
}

static void scouts(void)
{
    reset(true);
    colony(1, 3, 0);
    RaidFleet *f = &Raid_State.fleet[1];
    memset(f, 0, sizeof *f);
    f->phase = RAID_PHASE_FLYING;
    f->mission = RAID_SCOUT;
    f->race = MORGRULS;
    f->system = 1;
    f->planet = 3;
    f->units[0] = f->start[0] = 3;
    f->clock = 1;
    hours(1);
    CHECK(f->phase == RAID_PHASE_STAYING && f->clock == 24);
    hours(23);
    CHECK(f->phase == RAID_PHASE_STAYING);
    hours(1);
    CHECK(f->phase == RAID_PHASE_NONE);

    // Seen by a ship it leaves at once, and never fights.
    reset(true);
    colony(1, 3, 0);
    f = &Raid_State.fleet[1];
    memset(f, 0, sizeof *f);
    f->phase = RAID_PHASE_STAYING;
    f->mission = RAID_SCOUT;
    f->race = MORGRULS;
    f->system = 1;
    f->planet = 3;
    f->units[0] = 3;
    f->clock = 20;
    ships_at(1, 3, 1);
    hours(1);
    CHECK(f->phase == RAID_PHASE_NONE && !Raid_BattleActive() && ds.Game_CurrentScreen == 1);
}

static bool save_slot(int16_t slot)
{
    SaveLoad_ReadSlotNames();
    uint8_t *e = ds.SaveLoad_SlotTable + (slot - 1) * 0x16;
    memcpy(e + 1, "\x04" "raid", 5);
    ds.SaveLoad_SelectedSlot = slot;
    return SaveLoad_Transfer(true);
}

static bool load_slot(int16_t slot)
{
    SaveLoad_ReadSlotNames();
    ds.SaveLoad_SelectedSlot = slot;
    return SaveLoad_Transfer(false);
}

static void saves(void)
{
    reset(true);
    colony(1, 3, 0);
    arrive_at(RAID_BLOCKADE, 1, 3, 0, 6, 2);
    Raid_State.timer = 777;
    Raid_State.plan_mission = RAID_RAID;
    Raid_State.plan_race = MORGRULS;
    CHECK(SaveLoad_PortFormat() && save_slot(3));
    Raid_Reset();
    CHECK(!Raid_Any());
    CHECK(load_slot(3));
    CHECK(Raid_State.timer == 777 && Raid_State.plan_mission == RAID_RAID);
    CHECK(Raid_State.fleet[0].phase == RAID_PHASE_WAITING && Raid_State.fleet[0].units[0] == 6
          && Raid_State.fleet[0].units[1] == 2 && Raid_State.fleet[0].mission == RAID_BLOCKADE);

    // A fleet out keeps the saves in the port's format when the switch goes off.
    improvements.raids = false;
    improvements.adaptive_threat = false;
    CHECK(Raid_Any() && SaveLoad_PortFormat());
    Raid_Reset();
    CHECK(!Raid_Any() && !SaveLoad_PortFormat());

    // A save of version 4 (before the raids) loads without them.
    char path[256];
    SDL_snprintf(path, sizeof path, "%s/reunion.3", dir);
    size_t size;
    uint8_t *file = SDL_LoadFile(path, &size);
    CHECK(file && size > 12 && file[8] == 5 && file[9] == 0);
    if (file) {
        file[8] = 4;
        CHECK(SDL_SaveFile(path, file, size - sizeof(RaidState)));
    }
    SDL_free(file);
    Raid_State.timer = 5;
    CHECK(load_slot(3) && !Raid_Any());

    // A DOS save has none.
    reset(false);
    CHECK(!SaveLoad_PortFormat() && save_slot(4));
    Raid_State.timer = 9;
    CHECK(load_slot(4) && Raid_State.timer == 0);
}

static void switch_off(void)
{
    reset(false);
    colony(1, 3, 0);
    hours(48);
    CHECK(!Raid_Any());
    Raid_State.in_battle = 1;
    CHECK(!Raid_BattleActive() && Raid_BattleFleet() == nullptr);
    CHECK(!Raid_Blockaded(1, 3, 0));

    reset(true);
    improvements.adaptive_threat = false;
    colony(1, 3, 0);
    hours(48);
    CHECK(!Raid_Any());
}

int main(int argc, char *argv[])
{
    if (argc < 2 || !fs_init(argv[1])) {
        printf("no game data, skipping\n");
        return 77;
    }
    // Never touch the real save directory.
    SDL_snprintf(dir, sizeof dir, "/tmp/reunion_test_raid_%d", (int)SDL_GetCurrentTime(nullptr) & 0xFFFF);
    SaveLoad_SetSaveDir(dir);

    planning();
    schedule_and_flight();
    gaps();
    fresh_story();
    surface_open();
    bombardment();
    chase_and_battle();
    battles();
    blockade();
    scouts();
    saves();
    switch_off();

    for (int n = 3; n <= 4; n++) {
        char p[256];
        SDL_snprintf(p, sizeof p, "%s/reunion.%d", dir, n);
        SDL_RemovePath(p);
        SDL_snprintf(p, sizeof p, "%s/spidysav.%d", dir, n);
        SDL_RemovePath(p);
    }
    SDL_RemovePath(dir);
    return ok ? 0 : 1;
}
