// Improvement (game/improvements.h, planetary_defence): buildings that fight
// in the space battles of their planet. In the original no building does:
// the Ion cannon and the Energy shield only stand there.
//
// The Ion cannon (kind 18) fires 600 damage in 40% of the turns at any
// target, a large ship included, and is hit like a small ship but can't be
// killed outright. The Twin laser battery (kind 26, this improvement's own
// building, after the 25 of the original) is a Starfighter that doesn't fly:
// 68 damage, 3,000 hit points, half the chance of an outright kill.
// Batteries count in the player's strength (game/adaptive.h). One Ion cannon
// and one Energy shield stand on a body, up to 10 batteries.
//
// A battery is a ship of the player's fleet in the battle (the ids below);
// the ones that fall are demolished when it ends, none comes back.
//
// A save with a battery is in the port's format (the table of building
// types has 25 entries in DOS).
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "game/dseg.h"

enum : int {
    DEFENCE_KIND_ION = 18,
    DEFENCE_KIND_SHIELD = 20,
    DEFENCE_KIND_TWIN = 26,
    DEFENCE_DOS_KINDS = 25,
    DEFENCE_TWIN_LIMIT = 10,   // per body
    DEFENCE_ID_TWIN = 230,     // ids of the ships in a fleet list
    DEFENCE_ID_ION = 231,
    DEFENCE_SHIELD_MISS = 30,  // % of the shots at a shielded building
};

// One kind of fighting building.
typedef struct DefenceUnit {
    uint8_t kind;   // building kind
    uint8_t id;     // ship id in the battle
    uint8_t cls;    // ship class: its sprite, and the rules of a small ship
    uint16_t dmg;
    uint16_t hp;
    uint8_t hit;    // % of the turns it fires
    bool large;     // fires like a large ship: full damage at large ones, no outright kills
    uint8_t kill;   // % of the usual chance that a small ship's hit kills it outright
} DefenceUnit;

extern const DefenceUnit Defence_Units[2];

// The number of building kinds the player can pick: 25, 26 with the improvement.
int16_t Defence_KindCount(void);

// The type of building kind 1..26. The 26th is the port's own.
const TBuildingType *Defence_Type(int16_t kind);

// The Twin laser battery's tiles (240..243, row 12 of the sheet BufC holds
// past the game's own 12 rows): the Ion cannon's (228..231) with its red lights
// blue. `sheet` is that buffer, `palette` the game's (256 x 3, 6-bit) and the
// `used` buffers the picture indices (the sheets' own, palette offset 0xB0
// added) in use: the blue gets colours none of them uses. Returns the number
// of pixels made blue.
enum { DEFENCE_TWIN_FIRST_TILE = 240 };
int Defence_MakeTwinTiles(uint8_t *sheet, uint8_t *palette, const uint8_t *const used[], const size_t used_size[],
                          int used_count);
// The colour the minimap (the radar's 2x2 cell) gives the building tile
// `tile` of `sheet`: the strongest light in it (its most saturated pixel) or,
// without `light`, its commonest colour that is not near black.
uint8_t Defence_RadarColour(const uint8_t *sheet, const uint8_t *palette, int tile, bool light);

// Any Twin laser battery stands (the save is in the port's format).
bool Defence_AnyBattery(void);

// Buildings of `kind` on the body, finished or not; moon -1 any of the planet's.
int16_t Defence_Count(int16_t kind, int16_t system, int16_t planet, int16_t moon, bool finishedOnly);

// May one more of `kind` be placed on the body? Always yes without the improvement.
bool Defence_CanPlace(int16_t kind, int16_t system, int16_t planet, int16_t moon);

// The unit of a ship id in a battle, or nullptr.
const DefenceUnit *Defence_UnitOfId(uint8_t id);

// The finished, powered buildings of the unit on the planet (every moon
// too): the ships it brings to a battle there. 0 without the improvement.
int16_t Defence_Fighting(const DefenceUnit *u, int16_t system, int16_t planet);

// `lost` of those are demolished.
void Defence_Demolish(const DefenceUnit *u, int16_t system, int16_t planet, int16_t lost);

// A battery's combat weight, in tenths of a Hunter (game/adaptive.h).
int32_t Defence_Weight(const DefenceUnit *u);

// An Energy shield (kind 20) stands finished and powered on the planet (any
// body of it): the enemy's shots at its batteries and Ion cannon miss
// DEFENCE_SHIELD_MISS % of the time and are lost. One covers the planet;
// no more come of more.
bool Defence_Shielded(int16_t system, int16_t planet);

// The weight of the batteries on a planet (0 without the improvement).
int32_t Defence_PlanetWeight(int16_t system, int16_t planet);
