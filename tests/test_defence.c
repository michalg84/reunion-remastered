// Improvement (game/improvements.h, planetary_defence): game/defence.h. The
// Twin laser battery, the port's 26th building type; one Ion cannon and one
// Energy shield to a body, up to ten batteries; the batteries and the Ion
// cannon as ships of the space battle with rules of their own; the ones
// that fall demolished; the weight in the player's strength; a save with a
// battery in the port's format. With the switch off none of it is there.
// Needs the game data.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "game/adaptive.h"
#include "game/defence.h"
#include "game/dseg.h"
#include "game/gfx.h"
#include "game/groups.h"
#include "game/improvements.h"
#include "game/main_program.h"
#include "game/message.h"
#include "game/planetinfo.h"
#include "game/saveload.h"
#include "game/spwar.h"
#include "game/system.h"
#include "platform/config.h"
#include "platform/fs.h"
#include "platform/video.h"

static bool ok = true;
#define CHECK(c) do { if (!(c)) { printf("FAILED line %d: %s\n", __LINE__, #c); ok = false; } } while (0)

static bool wait_retrace(void) { return true; }

static char dir[128];

static void reset(bool on)
{
    dseg_clear();
    vid_clear();
    dseg_init();
    system_init();
    Game_InitGameData();
    Message_Init();
    SaveLoad_LoadInit();
    Adaptive_Reset();
    config_reset();
    ds.Gfx_LineStride = 320;
    ds.Gfx_LineStrideCopy = 320;
    gfx_wait_retrace = wait_retrace;
    ds.SpWar_View = game_alloc(0x5E60);
    ds.SpWar_Background = game_alloc(0x5E60);
    ds.SpWar_Sprites = game_alloc(0xF00);
    ds.SpWar_Face = game_alloc(0x1180);
    ds.SpWar_AttackerList = game_alloc(0x177C);
    ds.SpWar_DefenderList = game_alloc(0x177C);
    ds.SpWar_StepNo = 0;
    ds.SpWar_BattleOver = 0;
    ds.SpWar_AttackerShipCount = ds.SpWar_DefenderShipCount = 0;
    ds.Game_GroupCount = 0;
    ds.Game_PlanetForceCount = 0;
    ds.Game_BuildingCount = 0;
    ds.Game_Credits = 0;
    for (int r = 0; r < 11; r++)
        ds.Races_Races[r].ForceCount = 0;
    ds.Game_CurSystem = 1;
    ds.SpWar_BattleSystem = 1;
    ds.SpWar_BattlePlanet = 5;
    ds.SpWar_BattleMoon = 0;
    improvements.planetary_defence = on;
}

static void add_building(int16_t kind, int16_t system, int16_t planet, int16_t moon, bool built,
                         bool powered)
{
    TBuilding *b = &ds.Game_Buildings[ds.Game_BuildingCount++];
    memset(b, 0, sizeof *b);
    b->Kind = (uint8_t)kind;
    b->System = (uint8_t)system;
    b->Planet = (uint8_t)planet;
    b->Moon = (uint8_t)moon;
    b->BuildLeft = built ? 0 : 100;
    b->Powered = powered;
}

static SpShip *ship(uint8_t *list, int i) { return (SpShip *)list + i - 1; }

static void set_ship(SpShip *s, uint8_t id, uint8_t cls, uint16_t dmg, int16_t hp)
{
    memset(s, 0, sizeof *s);
    s->id = id;
    s->cls = cls;
    s->dmg = dmg;
    s->hp = hp;
    s->x = 20;
    s->y = 30;
    s->len = 2;
    s->path = 1;
    s->pos = 1;
    s->cool = 5;  // no movement: only the target and hit rolls use the generator
}

static void types(void)
{
    reset(false);
    CHECK(Defence_KindCount() == 25);
    improvements.planetary_defence = true;
    CHECK(Defence_KindCount() == 26);

    const TBuildingType *t = Defence_Type(DEFENCE_KIND_TWIN);
    CHECK(t->Cost == 16000 && t->Energy == 300 && t->Category == 5);
    CHECK(t->Width == 2 && t->Height == 2 && t->NeedProduct == 13);
    CHECK(t->Name.len == 14 && SDL_strncmp(t->Name.s, "Twin laser bat", 14) == 0);
    CHECK(Defence_Type(DEFENCE_KIND_ION) == &ds.Surface_BuildingTypes[DEFENCE_KIND_ION - 1]);
    CHECK(Defence_Type(1) == &ds.Surface_BuildingTypes[0]);
    CHECK(SDL_strncmp(Defence_Type(DEFENCE_KIND_ION)->Name.s, "Ioncannon", 9) == 0);
    CHECK(SDL_strncmp(Defence_Type(DEFENCE_KIND_SHIELD)->Name.s, "Energy shield", 13) == 0);
}

static void placement(void)
{
    // Without the improvement the original's rules: any number.
    reset(false);
    for (int i = 0; i < 12; i++)
        add_building(DEFENCE_KIND_ION, 1, 5, 0, true, true);
    CHECK(Defence_CanPlace(DEFENCE_KIND_ION, 1, 5, 0) && Defence_CanPlace(DEFENCE_KIND_TWIN, 1, 5, 0));

    reset(true);
    CHECK(Defence_CanPlace(DEFENCE_KIND_ION, 1, 5, 0));
    add_building(DEFENCE_KIND_ION, 1, 5, 0, false, false);  // one still going up counts
    CHECK(!Defence_CanPlace(DEFENCE_KIND_ION, 1, 5, 0));
    CHECK(Defence_CanPlace(DEFENCE_KIND_ION, 1, 5, 1));     // another body
    CHECK(Defence_CanPlace(DEFENCE_KIND_ION, 1, 4, 0));
    CHECK(Defence_CanPlace(DEFENCE_KIND_SHIELD, 1, 5, 0));
    add_building(DEFENCE_KIND_SHIELD, 1, 5, 0, true, true);
    CHECK(!Defence_CanPlace(DEFENCE_KIND_SHIELD, 1, 5, 0));

    for (int i = 0; i < DEFENCE_TWIN_LIMIT - 1; i++)
        add_building(DEFENCE_KIND_TWIN, 1, 5, 0, i & 1, true);
    CHECK(Defence_CanPlace(DEFENCE_KIND_TWIN, 1, 5, 0));
    add_building(DEFENCE_KIND_TWIN, 1, 5, 0, true, true);
    CHECK(!Defence_CanPlace(DEFENCE_KIND_TWIN, 1, 5, 0));
    CHECK(Defence_CanPlace(DEFENCE_KIND_TWIN, 1, 5, 1));
    CHECK(Defence_CanPlace(5, 1, 5, 0));  // other kinds are not limited
}

static void weights(void)
{
    reset(true);
    int32_t twin = Defence_Weight(&Defence_Units[0]), ion = Defence_Weight(&Defence_Units[1]);
    CHECK(twin >= 45 && twin <= 51);   // a Starfighter, 39, with 3,000 hit points
    CHECK(ion >= 260 && ion <= 285);   // 600 damage in 40% of the turns: about a Cruiser's weight
    int32_t base = Adaptive_PlayerPower();

    add_building(DEFENCE_KIND_TWIN, 1, 5, 0, true, true);
    add_building(DEFENCE_KIND_TWIN, 1, 5, 2, true, true);  // a moon of the planet counts
    add_building(DEFENCE_KIND_ION, 1, 5, 0, true, true);
    CHECK(Adaptive_PlayerPower() == base + 2 * twin + ion);

    add_building(DEFENCE_KIND_TWIN, 1, 5, 0, false, true);  // not finished
    add_building(DEFENCE_KIND_TWIN, 1, 5, 0, true, false);  // no power
    add_building(DEFENCE_KIND_TWIN, 1, 4, 0, true, true);   // not where the waves go
    CHECK(Adaptive_PlayerPower() == base + 2 * twin + ion);

    improvements.planetary_defence = false;
    CHECK(Adaptive_PlayerPower() == base);
}

static int count_id(uint8_t id)
{
    int n = 0;
    for (int i = 1; i <= ds.SpWar_AttackerShipCount; i++)
        n += ship(ds.SpWar_AttackerList, i)->id == id;
    return n;
}

static void fleets(void)
{
    reset(true);
    add_building(DEFENCE_KIND_TWIN, 1, 5, 0, true, true);
    add_building(DEFENCE_KIND_TWIN, 1, 5, 0, true, true);
    add_building(DEFENCE_KIND_TWIN, 1, 5, 1, true, true);
    add_building(DEFENCE_KIND_TWIN, 1, 5, 0, false, true);  // still going up
    add_building(DEFENCE_KIND_TWIN, 1, 5, 0, true, false);  // no power
    add_building(DEFENCE_KIND_TWIN, 1, 4, 0, true, true);   // another planet
    add_building(DEFENCE_KIND_ION, 1, 5, 0, true, true);
    SpWar_InitSpaceFleets();
    CHECK(ds.SpWar_DefenderShipCount == 0);
    CHECK(ds.SpWar_AttackerShipCount == 4);
    CHECK(count_id(DEFENCE_ID_TWIN) == 3 && count_id(DEFENCE_ID_ION) == 1);
    for (int i = 1; i <= 4; i++) {
        const SpShip *s = ship(ds.SpWar_AttackerList, i);
        bool ion = s->id == DEFENCE_ID_ION;
        CHECK(s->cls == 2 && s->hp == 3000 && s->dmg == (ion ? 600 : 68));
    }

    // Off: only the player's own ships fight, there are none.
    reset(true);
    add_building(DEFENCE_KIND_TWIN, 1, 5, 0, true, true);
    add_building(DEFENCE_KIND_ION, 1, 5, 0, true, true);
    improvements.planetary_defence = false;
    SpWar_InitSpaceFleets();
    CHECK(ds.SpWar_AttackerShipCount == 0);
}

// A step with one attacker and one defender, rolling the target (Random(1)) and
// the hit (Random(100)) for each in turn, then any outright-kill rolls.
static bool seed_where(bool (*rolls)(void), uint32_t *found)
{
    for (uint32_t s = 1; s < 4000000; s++) {
        ds.System_RandSeed = s;
        if (rolls()) {
            *found = s;
            return true;
        }
    }
    return false;
}

static uint16_t roll(uint16_t n) { return System_Random(n); }

// The attacker misses; the defender hits (below 11).
static bool def_hits(void)
{
    roll(1);
    uint16_t a = roll(100);
    roll(1);
    uint16_t b = roll(100);
    return a >= 40 && b < 11;
}

// ... and the hit kills outright (r <= 1, Hunter at a Starfighter), the
// second roll under 50.
static bool def_kills_low(void)
{
    roll(1);
    uint16_t a = roll(100);
    roll(1);
    uint16_t b = roll(100);
    uint16_t r = roll(100), k = roll(100);
    return a >= 40 && b < 11 && r <= 1 && k < 50;
}

static bool def_kills_high(void)
{
    roll(1);
    uint16_t a = roll(100);
    roll(1);
    uint16_t b = roll(100);
    uint16_t r = roll(100), k = roll(100);
    return a >= 40 && b < 11 && r <= 1 && k >= 50;
}

// A plain Starfighter as the target has no second roll.
static bool def_kills_plain(void)
{
    roll(1);
    uint16_t a = roll(100);
    roll(1);
    uint16_t b = roll(100);
    uint16_t r = roll(100);
    return a >= 40 && b < 11 && r <= 1;
}

// The attacker hits (11..39: the Ion cannon does, a plain ship doesn't), the
// defender doesn't.
static bool att_hits_ion_only(void)
{
    roll(1);
    uint16_t a = roll(100);
    roll(1);
    uint16_t b = roll(100);
    return a >= 11 && a < 40 && b >= 11;
}

static bool att_hits_any(void)
{
    roll(1);
    uint16_t a = roll(100);
    roll(1);
    uint16_t b = roll(100);
    return a < 11 && b >= 11;
}

// The defender hits and the shield rolls under 30 / not.
static bool shot_lost(void)
{
    roll(1);
    uint16_t a = roll(100);
    roll(1);
    uint16_t b = roll(100), sh = roll(100);
    return a >= 40 && b < 11 && sh < DEFENCE_SHIELD_MISS;
}

static bool shot_lands(void)
{
    roll(1);
    uint16_t a = roll(100);
    roll(1);
    uint16_t b = roll(100), sh = roll(100), r = roll(100);
    return a >= 40 && b < 11 && sh >= DEFENCE_SHIELD_MISS && r > 1;
}

static void one_step(uint8_t aid, uint8_t acls, uint16_t admg, int16_t ahp, uint8_t did, uint8_t dcls,
                     uint16_t ddmg, int16_t dhp)
{
    ds.SpWar_AttackerShipCount = 1;
    ds.SpWar_DefenderShipCount = 1;
    ds.SpWar_AttackerStart = 1;
    ds.SpWar_DefenderStart = 1;
    set_ship(ship(ds.SpWar_AttackerList, 1), aid, acls, admg, ahp);
    set_ship(ship(ds.SpWar_DefenderList, 1), did, dcls, ddmg, dhp);
    ds.SpWar_StepNo = 0;
    ds.SpWar_BattleOver = 0;
    SpWar_StepSpaceBattle();
}

static void rules(void)
{
    uint32_t seed = 0;
    reset(true);

    // The Ion cannon fires in 40% of the turns, at a Destroyer for the full 600.
    CHECK(seed_where(att_hits_ion_only, &seed));
    ds.System_RandSeed = seed;
    one_step(DEFENCE_ID_ION, 2, 600, 3000, 21, 3, 1, 5000);
    CHECK(ship(ds.SpWar_DefenderList, 1)->hp == 5000 - 600);
    // A plain Starfighter with the same roll does not fire.
    ds.System_RandSeed = seed;
    one_step(1, 2, 600, 3000, 21, 3, 1, 5000);
    CHECK(ship(ds.SpWar_DefenderList, 1)->hp == 5000);
    // ... and when it does, a third of the damage at a large ship.
    CHECK(seed_where(att_hits_any, &seed));
    ds.System_RandSeed = seed;
    one_step(1, 2, 600, 3000, 21, 3, 1, 5000);
    CHECK(ship(ds.SpWar_DefenderList, 1)->hp == 5000 - 200);
    ds.System_RandSeed = seed;
    one_step(DEFENCE_ID_ION, 2, 600, 3000, 21, 3, 1, 5000);
    CHECK(ship(ds.SpWar_DefenderList, 1)->hp == 5000 - 600);

    // A Hunter's hit that would kill a Starfighter outright (1 in 50 of its
    // hits... r <= 1): the Twin laser battery takes half of those, the Ion
    // cannon none.
    CHECK(seed_where(def_hits, &seed));
    CHECK(seed_where(def_kills_plain, &seed));
    ds.System_RandSeed = seed;
    one_step(1, 2, 68, 3000, 22, 1, 11, 3000);
    CHECK(ds.SpWar_AttackerShipCount == 0);  // the plain Starfighter dies

    CHECK(seed_where(def_kills_low, &seed));
    ds.System_RandSeed = seed;
    one_step(DEFENCE_ID_TWIN, 2, 68, 3000, 22, 1, 11, 3000);
    CHECK(ds.SpWar_AttackerShipCount == 0);  // the battery dies on the roll under half
    ds.System_RandSeed = seed;
    one_step(DEFENCE_ID_ION, 2, 600, 3000, 22, 1, 11, 3000);
    CHECK(ds.SpWar_AttackerShipCount == 1);  // the Ion cannon never

    CHECK(seed_where(def_kills_high, &seed));
    ds.System_RandSeed = seed;
    one_step(DEFENCE_ID_TWIN, 2, 68, 3000, 22, 1, 11, 3000);
    CHECK(ds.SpWar_AttackerShipCount == 1);  // above it the battery stands
    CHECK(ship(ds.SpWar_AttackerList, 1)->hp == 3000 - 11);
}

// An Energy shield on the planet: 30% of the shots at the batteries are lost,
// the ones at ships are not; the shield counts in the weight.
static void shield(void)
{
    uint32_t seed = 0;
    reset(true);
    int32_t base = Adaptive_PlayerPower();
    add_building(DEFENCE_KIND_TWIN, 1, 5, 0, true, true);
    int32_t with_battery = Adaptive_PlayerPower();
    CHECK(!Defence_Shielded(1, 5));
    add_building(DEFENCE_KIND_SHIELD, 1, 5, 2, true, false);  // no power
    CHECK(!Defence_Shielded(1, 5));
    add_building(DEFENCE_KIND_SHIELD, 1, 5, 2, false, true);  // not finished
    CHECK(!Defence_Shielded(1, 5));
    add_building(DEFENCE_KIND_SHIELD, 1, 4, 0, true, true);   // another planet
    CHECK(!Defence_Shielded(1, 5));
    add_building(DEFENCE_KIND_SHIELD, 1, 5, 2, true, true);   // a moon of the planet
    CHECK(Defence_Shielded(1, 5));
    int32_t w = Defence_Weight(&Defence_Units[0]);
    CHECK(with_battery == base + w);
    CHECK(Adaptive_PlayerPower() == base + w * 100 / (100 - DEFENCE_SHIELD_MISS));
    add_building(DEFENCE_KIND_SHIELD, 1, 5, 0, true, true);   // a second adds nothing
    CHECK(Adaptive_PlayerPower() == base + w * 100 / (100 - DEFENCE_SHIELD_MISS));

    SpWar_InitSpaceFleets();  // the battery joins the battle
    CHECK(count_id(DEFENCE_ID_TWIN) == 1);
    CHECK(seed_where(shot_lost, &seed));
    ds.System_RandSeed = seed;
    one_step(DEFENCE_ID_TWIN, 2, 68, 3000, 22, 1, 11, 3000);
    CHECK(ship(ds.SpWar_AttackerList, 1)->hp == 3000);
    CHECK(seed_where(shot_lands, &seed));
    ds.System_RandSeed = seed;
    one_step(DEFENCE_ID_TWIN, 2, 68, 3000, 22, 1, 11, 3000);
    CHECK(ship(ds.SpWar_AttackerList, 1)->hp == 3000 - 11);
    // A ship of the player's own is not covered.
    CHECK(seed_where(shot_lost, &seed));
    ds.System_RandSeed = seed;
    one_step(1, 2, 68, 3000, 22, 1, 11, 3000);
    CHECK(ship(ds.SpWar_AttackerList, 1)->hp == 3000 - 11);

    // Without the improvement the shield is nothing.
    improvements.planetary_defence = false;
    CHECK(!Defence_Shielded(1, 5));
}

// Whole battles of the real step against the simulator's (tools/spacebattle_sim.py,
// the figures the stage's plan is built on): how often `twin` batteries and
// `ion` Ion cannons hold against a raid of `hunters` and `fighters`, in percent.
// A ship as the fleets are built (SpWar_InitSpaceFleets): in its start area, on a route.
static void start_ship(SpShip *s, uint8_t id, uint8_t cls, uint16_t dmg, int16_t hp, bool mirror)
{
    set_ship(s, id, cls, dmg, hp);
    uint8_t x = (uint8_t)(ds.SpWar_StartX[cls - 1] + System_Random(ds.SpWar_StartXRange[cls - 1]));
    s->x = mirror ? (uint8_t)(0xA0 - x) : x;
    s->y = (uint8_t)(ds.SpWar_StartY[cls - 1] + System_Random(ds.SpWar_StartYRange[cls - 1]));
    s->len = (uint8_t)(System_Random(6) + 2);
    s->path = ds.SpWar_ZonePaths[3][System_Random(8)];
    s->pos = 0;
    s->cool = 1;
}

static int holds(int twin, int ion, int hunters, int fighters, int runs)
{
    int won = 0;
    for (int run = 0; run < runs; run++) {
        ds.SpWar_AttackerShipCount = ds.SpWar_DefenderShipCount = 0;
        ds.SpWar_StepNo = 0;
        ds.SpWar_BattleOver = 0;
        for (int i = 0; i < twin + ion; i++) {
            bool is_ion = i >= twin;
            const DefenceUnit *u = &Defence_Units[is_ion];
            start_ship(ship(ds.SpWar_AttackerList, i + 1), u->id, u->cls, u->dmg, (int16_t)u->hp, false);
            ds.SpWar_AttackerShipCount++;
        }
        for (int i = 0; i < hunters + fighters; i++) {
            uint8_t cls = i < hunters ? 1 : 2;
            start_ship(ship(ds.SpWar_DefenderList, i + 1), 60, cls, (uint16_t)ds.SpWar_ClassB[cls - 1],
                       (int16_t)ds.SpWar_ClassA[cls - 1], true);
            ds.SpWar_DefenderShipCount++;
        }
        ds.SpWar_AttackerStart = ds.SpWar_AttackerShipCount;
        ds.SpWar_DefenderStart = ds.SpWar_DefenderShipCount;
        // The step that ends a battle draws the result screen; "over" already
        // set, it does not.
        for (int n = 0; n < 400000 && ds.SpWar_AttackerShipCount > 0 && ds.SpWar_DefenderShipCount > 0; n++) {
            ds.SpWar_BattleOver = 1;
            SpWar_StepSpaceBattle();
        }
        won += ds.SpWar_DefenderShipCount == 0;
    }
    return won * 100 / runs;
}

static void statistics(void)
{
    reset(true);
    ds.System_RandSeed = 777;
    int a = holds(10, 0, 6, 8, 150);   // the simulator: 91%
    int b = holds(5, 0, 6, 3, 150);    // 78%
    int c = holds(10, 1, 6, 8, 150);   // 100%
    int d = holds(10, 1, 27, 27, 100); // 0%
    CHECK(a >= 80 && a <= 99);
    CHECK(b >= 64 && b <= 92);
    CHECK(c >= 96);
    CHECK(d <= 8);
}

// The battery that fell is demolished at the end of the battle, the others stand.
static void losses(void)
{
    reset(true);
    for (int i = 0; i < 4; i++)
        add_building(DEFENCE_KIND_TWIN, 1, 5, 0, true, true);
    add_building(DEFENCE_KIND_TWIN, 1, 4, 0, true, true);
    add_building(DEFENCE_KIND_ION, 1, 5, 0, true, true);
    add_building(5, 1, 5, 0, true, true);  // something else: untouched
    SpWar_InitSpaceFleets();
    CHECK(ds.SpWar_AttackerShipCount == 5);

    // Three twin batteries fell: they are the dead of the list's tail.
    int dead = 0;
    for (int i = ds.SpWar_AttackerShipCount; i >= 1 && dead < 3; i--)
        if (ship(ds.SpWar_AttackerList, i)->id == DEFENCE_ID_TWIN) {
            SpShip tmp = *ship(ds.SpWar_AttackerList, ds.SpWar_AttackerShipCount - dead);
            *ship(ds.SpWar_AttackerList, ds.SpWar_AttackerShipCount - dead) = *ship(ds.SpWar_AttackerList, i);
            *ship(ds.SpWar_AttackerList, i) = tmp;
            dead++;
        }
    ds.SpWar_AttackerShipCount -= 3;
    CHECK(count_id(DEFENCE_ID_TWIN) == 1 && count_id(DEFENCE_ID_ION) == 1);
    SpWar_ApplySpaceLosses();
    CHECK(Defence_Count(DEFENCE_KIND_TWIN, 1, 5, -1, true) == 1);
    CHECK(Defence_Count(DEFENCE_KIND_TWIN, 1, 4, 0, true) == 1);
    CHECK(Defence_Count(DEFENCE_KIND_ION, 1, 5, 0, true) == 1);
    CHECK(Defence_Count(5, 1, 5, 0, true) == 1);
    CHECK(ds.Game_BuildingCount == 4);
    CHECK(ds.SpWar_AttackerLosses[1] == 3);

    // A fifth of the destroyed ships live, a building does not: the same
    // three would be three with any survivors rule.
    reset(true);
    for (int i = 0; i < 10; i++)
        add_building(DEFENCE_KIND_TWIN, 1, 5, 0, true, true);
    SpWar_InitSpaceFleets();
    ds.SpWar_AttackerShipCount = 0;
    SpWar_ApplySpaceLosses();
    CHECK(Defence_Count(DEFENCE_KIND_TWIN, 1, 5, -1, false) == 0);
    CHECK(ds.SpWar_AttackerLosses[1] == 10);

    // A fleet of 500 takes no battery; none of them is lost with it.
    reset(true);
    TGroup *g = Groups_At(0, 1);
    memset(g, 0, sizeof *g);
    g->Kind = 1;
    g->System = 1;
    g->Planet = 5;
    g->Status = 1;
    g->Units[0][0].Count = 500;
    g->Units[0][0].Weapons[0] = 500;
    ds.Game_GroupCount = 1;
    add_building(DEFENCE_KIND_TWIN, 1, 5, 0, true, true);
    SpWar_InitSpaceFleets();
    CHECK(ds.SpWar_AttackerShipCount == 500 && count_id(DEFENCE_ID_TWIN) == 0);
    ds.SpWar_AttackerShipCount = 0;
    SpWar_ApplySpaceLosses();
    CHECK(Defence_Count(DEFENCE_KIND_TWIN, 1, 5, -1, false) == 1);

    // The switch off: no battery in the battle, none lost.
    reset(true);
    add_building(DEFENCE_KIND_TWIN, 1, 5, 0, true, true);
    SpWar_InitSpaceFleets();
    improvements.planetary_defence = false;
    ds.SpWar_AttackerShipCount = 0;
    SpWar_ApplySpaceLosses();
    CHECK(Defence_Count(DEFENCE_KIND_TWIN, 1, 5, -1, false) == 1);
}

static bool save_slot(int16_t slot)
{
    SaveLoad_ReadSlotNames();
    uint8_t *e = ds.SaveLoad_SlotTable + (slot - 1) * 0x16;
    memcpy(e + 1, "\x07" "defence", 8);
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
    // A battery makes the save the port's, and it comes back whole, with the
    // switch off too: it stands, without effect.
    reset(true);
    CHECK(!SaveLoad_PortFormat() && !Defence_AnyBattery());
    add_building(DEFENCE_KIND_ION, 1, 5, 0, true, true);
    CHECK(!SaveLoad_PortFormat());  // the Ion cannon is the original's
    add_building(DEFENCE_KIND_TWIN, 2, 3, 0, true, true);
    add_building(DEFENCE_KIND_TWIN, 2, 3, 0, false, false);
    CHECK(Defence_AnyBattery() && SaveLoad_PortFormat());
    improvements.planetary_defence = false;
    CHECK(SaveLoad_PortFormat() && save_slot(3));

    ds.Game_BuildingCount = 0;
    memset(ds.Game_Buildings, 0, sizeof(TBuilding) * 4);
    CHECK(load_slot(3));
    CHECK(ds.Game_BuildingCount == 3);
    CHECK(ds.Game_Buildings[0].Kind == DEFENCE_KIND_ION);
    CHECK(ds.Game_Buildings[1].Kind == DEFENCE_KIND_TWIN && ds.Game_Buildings[1].Planet == 3);
    CHECK(ds.Game_Buildings[2].Kind == DEFENCE_KIND_TWIN && ds.Game_Buildings[2].BuildLeft == 100);
    CHECK(Defence_Count(DEFENCE_KIND_TWIN, 2, 3, 0, false) == 2);
    CHECK(Defence_Fighting(&Defence_Units[0], 2, 3) == 0);  // the switch is off

    // Without a battery a save stays in DOS's format.
    reset(true);
    add_building(DEFENCE_KIND_ION, 1, 5, 0, true, true);
    CHECK(!SaveLoad_PortFormat() && save_slot(4));
}

int main(int argc, char *argv[])
{
    if (argc < 2 || !fs_init(argv[1])) {
        printf("no game data, skipping\n");
        return 77;
    }
    // Never touch the real save directory.
    SDL_snprintf(dir, sizeof dir, "/tmp/reunion_test_defence_%d",
                 (int)SDL_GetCurrentTime(nullptr) & 0xFFFF);
    SaveLoad_SetSaveDir(dir);

    types();
    placement();
    weights();
    fleets();
    rules();
    shield();
    statistics();
    losses();
    saves();

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
