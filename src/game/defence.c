#include "game/defence.h"

#include <SDL3/SDL.h>

#include "game/adaptive.h"
#include "game/improvements.h"
#include "game/planetinfo.h"
#include "game/surface.h"
#include "game/system.h"

// The Twin laser battery: the Ion cannon's footprint and surfaces, a Twin
// laser gun to research, 16,000 credits and 300 kwh.
static const TBuildingType twin_battery = {
    .Name = {14, {'T', 'w', 'i', 'n', ' ', 'l', 'a', 's', 'e', 'r', ' ', 'b', 'a', 't'}},
    .NeedProduct = 13,  // Twin laser gun
    .NeedBuilder = 2,
    .Category = 5,
    .Surfaces = {1, 0, 1, 0, 1, 1, 0, 1},
    .Energy = 300,
    .Cost = 16000,
    .PowerPriority = 2,
    .BuildChance = 10,
    .Width = 2,
    .Height = 2,
    .Tiles = {{240, 241}, {242, 243}},  // the Ion cannon's, red lights blue (Defence_MakeTwinTiles)
};

const DefenceUnit Defence_Units[2] = {
    {DEFENCE_KIND_TWIN, DEFENCE_ID_TWIN, 2, 68, 3000, 11, false, 50},
    {DEFENCE_KIND_ION, DEFENCE_ID_ION, 2, 600, 3000, 40, true, 0},
};

int16_t Defence_KindCount(void)
{
    return improvements.planetary_defence ? DEFENCE_KIND_TWIN : DEFENCE_DOS_KINDS;
}

const TBuildingType *Defence_Type(int16_t kind)
{
    if (kind == DEFENCE_KIND_TWIN)
        return &twin_battery;
    return &ds.Surface_BuildingTypes[kind - 1];
}

bool Defence_AnyBattery(void)
{
    for (int16_t i = 0; i < ds.Game_BuildingCount; i++)
        if (ds.Game_Buildings[i].Kind == DEFENCE_KIND_TWIN)
            return true;
    return false;
}

int16_t Defence_Count(int16_t kind, int16_t system, int16_t planet, int16_t moon, bool finishedOnly)
{
    int16_t n = 0;
    for (int16_t i = 0; i < ds.Game_BuildingCount; i++) {
        const TBuilding *b = &ds.Game_Buildings[i];
        if (b->Kind == kind && b->System == system && b->Planet == planet
            && (moon < 0 || b->Moon == moon) && (!finishedOnly || b->BuildLeft == 0))
            n++;
    }
    return n;
}

bool Defence_CanPlace(int16_t kind, int16_t system, int16_t planet, int16_t moon)
{
    if (!improvements.planetary_defence)
        return true;
    int16_t n = Defence_Count(kind, system, planet, moon, false);
    switch (kind) {
    case DEFENCE_KIND_TWIN:
        return n < DEFENCE_TWIN_LIMIT;
    case DEFENCE_KIND_ION:
    case DEFENCE_KIND_SHIELD:
        return n < 1;
    default:
        return true;
    }
}

const DefenceUnit *Defence_UnitOfId(uint8_t id)
{
    for (int i = 0; i < 2; i++)
        if (Defence_Units[i].id == id)
            return &Defence_Units[i];
    return nullptr;
}

// A finished, powered building of the unit on the planet.
static bool fights(const DefenceUnit *u, const TBuilding *b, int16_t system, int16_t planet)
{
    return b->Kind == u->kind && b->System == system && b->Planet == planet && b->BuildLeft == 0
           && b->Powered != 0;
}

int16_t Defence_Fighting(const DefenceUnit *u, int16_t system, int16_t planet)
{
    if (!improvements.planetary_defence)
        return 0;
    int16_t n = 0;
    for (int16_t i = 0; i < ds.Game_BuildingCount; i++)
        n += fights(u, &ds.Game_Buildings[i], system, planet);
    return n;
}

void Defence_Demolish(const DefenceUnit *u, int16_t system, int16_t planet, int16_t lost)
{
    // Which ones fell is chance; every body that lost one balances its power again.
    bool moons[40] = {};
    for (; lost > 0; lost--) {
        int16_t n = Defence_Fighting(u, system, planet);
        if (n == 0)
            break;
        int16_t pick = (int16_t)System_Random((uint16_t)n);
        for (int16_t i = 1; i <= ds.Game_BuildingCount; i++) {
            if (!fights(u, &ds.Game_Buildings[i - 1], system, planet) || pick-- > 0)
                continue;
            uint8_t moon = ds.Game_Buildings[i - 1].Moon;
            if (moon < SDL_arraysize(moons))
                moons[moon] = true;
            Surface_DeleteBuilding(i);
            break;
        }
    }
    for (int16_t m = 0; m < (int16_t)SDL_arraysize(moons); m++)
        if (moons[m])
            PlanetInfo_BalanceBuildingPower(system, planet, m);
}

int32_t Defence_Weight(const DefenceUnit *u)
{
    // An Ion cannon fires 40% of the turns, not 11%: the damage per turn at
    // the rate of every other ship.
    return Adaptive_ShipWeight((int64_t)u->dmg * u->hit / 11, u->hp);
}

bool Defence_Shielded(int16_t system, int16_t planet)
{
    if (!improvements.planetary_defence)
        return false;
    for (int16_t i = 0; i < ds.Game_BuildingCount; i++) {
        const TBuilding *b = &ds.Game_Buildings[i];
        if (b->Kind == DEFENCE_KIND_SHIELD && b->System == system && b->Planet == planet
            && b->BuildLeft == 0 && b->Powered != 0)
            return true;
    }
    return false;
}

int32_t Defence_PlanetWeight(int16_t system, int16_t planet)
{
    int64_t w = 0;
    for (int i = 0; i < 2; i++)
        w += (int64_t)Defence_Fighting(&Defence_Units[i], system, planet) * Defence_Weight(&Defence_Units[i]);
    // Shots lost are as good as hit points: 1 / (1 - 30%) of them.
    if (w > 0 && Defence_Shielded(system, planet))
        w = w * 100 / (100 - DEFENCE_SHIELD_MISS);
    return (int32_t)SDL_min(w, INT32_MAX);
}

enum {
    SHEET_COLOUR_BASE = 0xB0,  // the surface sheets' palette offset
    SHEET_COLOURS = 64,
    ION_FIRST_TILE = 228,
    TILE_PER_ROW = 20,
    TILE_SIZE = 16,
    SHEET_STRIDE = 0x140,
};

static size_t tile_offset(int tile)
{
    return (size_t)(tile % TILE_PER_ROW) * TILE_SIZE + (size_t)(tile / TILE_PER_ROW) * (TILE_SIZE * SHEET_STRIDE);
}

// A red light: a strong red with no green or blue (6-bit values).
static bool is_red(const uint8_t *palette, int colour)
{
    const uint8_t *c = palette + colour * 3;
    return c[0] >= 28 && c[1] <= 10 && c[2] <= 10;
}

int Defence_MakeTwinTiles(uint8_t *sheet, uint8_t *palette, const uint8_t *const used[], const size_t used_size[],
                          int used_count)
{
    bool in_use[256] = {false};
    for (int i = 0; i < used_count; i++)
        for (size_t k = 0; k < used_size[i]; k++)
            in_use[used[i][k]] = true;

    int map[256];
    for (int c = 0; c < 256; c++)
        map[c] = -1;
    int free_colour = SHEET_COLOUR_BASE + SHEET_COLOURS - 1;
    int made = 0;
    for (int t = 0; t < 4; t++) {
        const uint8_t *from = sheet + tile_offset(ION_FIRST_TILE + t);
        uint8_t *to = sheet + tile_offset(DEFENCE_TWIN_FIRST_TILE + t);
        for (int y = 0; y < TILE_SIZE; y++) {
            for (int x = 0; x < TILE_SIZE; x++) {
                uint8_t c = from[y * SHEET_STRIDE + x];
                if (map[c] < 0) {
                    map[c] = c;
                    if (c >= SHEET_COLOUR_BASE && c < SHEET_COLOUR_BASE + SHEET_COLOURS && is_red(palette, c)) {
                        while (free_colour >= SHEET_COLOUR_BASE && in_use[free_colour])
                            free_colour--;
                        if (free_colour >= SHEET_COLOUR_BASE) {
                            const uint8_t *r = palette + c * 3;
                            uint8_t *b = palette + free_colour * 3;
                            b[0] = 0;
                            b[1] = (uint8_t)(r[0] * 3 / 10);
                            b[2] = r[0];
                            map[c] = free_colour--;
                        }
                    }
                }
                to[y * SHEET_STRIDE + x] = (uint8_t)map[c];
                made += map[c] != c;
            }
        }
    }
    return made;
}

uint8_t Defence_RadarColour(const uint8_t *sheet, const uint8_t *palette, int tile)
{
    const uint8_t *t = sheet + tile_offset(tile);
    uint8_t best = t[0];
    int best_sat = -1;
    for (int y = 0; y < TILE_SIZE; y++) {
        for (int x = 0; x < TILE_SIZE; x++) {
            uint8_t c = t[y * SHEET_STRIDE + x];
            const uint8_t *rgb = palette + c * 3;
            int sat = SDL_max(rgb[0], SDL_max(rgb[1], rgb[2])) - SDL_min(rgb[0], SDL_min(rgb[1], rgb[2]));
            if (sat > best_sat) {
                best_sat = sat;
                best = c;
            }
        }
    }
    return best;
}
