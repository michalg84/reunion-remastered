// Improvement (game/improvements.h, raids; needs adaptive_threat): between the
// story's waves a hostile race sends small fleets to the player's colonies
// outside New Earth: a scout that looks and leaves, a raid that shoots from
// orbit, a blockade that stays and stops the loading and unloading of ore.
//
// One schedule, 800-1200 h apart, kept 200 h away from every story launch.
// The race is the hostile one whose next wave is nearest and the fleet is at
// most 30% of that wave's Hunters and Starfighters (a race with no wave
// ahead: its home garrison): its weight is the target's defence times
// 1.0-1.5 (1.2-1.8 later), between half the ceiling and the ceiling; a body
// with no defence at all gets 3-6 Hunters. The target is drawn from the
// colonies, the richer and the weaker defended the likelier.
//
// The fleet has a slot of its own (the races' seven are the story's), flies
// from its race's home like a wave, waits at the target as one does, and
// then: a player at least 1.5 times as strong there and it turns back with
// no fight (the next raid later); an undefended raid target is shot at (up to
// 15% of the buildings, 30% of the ore, more the weaker the defence); else a
// space battle, which the raid leaves when it has lost half its weight.
// A fleet that has done its part goes home and is gone.
//
// A raid's battle skips the story's hooks. The state is a port record: a save
// with the improvement on is in the port's format.
#pragma once

#include <stdint.h>
#include <stdbool.h>

enum : int {
    RAID_NONE,
    RAID_SCOUT,
    RAID_RAID,
    RAID_BLOCKADE,
};

enum : int {
    RAID_PHASE_NONE,
    RAID_PHASE_FLYING,   // on its way: clock = hours to arrive
    RAID_PHASE_WAITING,  // at the target before it acts: clock = hours to it
    RAID_PHASE_STAYING,  // a blockade, or a scout looking around (clock: hours it stays)
};

enum : int {
    RAID_FLEETS = 2,     // [0] a raid or blockade, [1] a scout
    RAID_SHIP_ID = 240,  // the id of its ships in a battle's list
};

// Bytes only (uint16 all through), so the record is its own file layout.
typedef struct RaidFleet {
    uint16_t phase;
    uint16_t mission;
    uint16_t race;
    uint16_t system, planet, moon;  // the target
    uint16_t clock;
    uint16_t units[2];  // Hunters, Starfighters
    uint16_t start[2];  // at launch
} RaidFleet;

typedef struct RaidState {
    uint16_t timer;       // hours to the next launch; 0 = nothing planned
    uint16_t quiet;       // hours still to wait after the story's last fleet
    uint16_t scout_timer; // hours to the scout that goes ahead; 0 none
    uint16_t warned;      // 1 = the advance warning of the planned one was given
    uint16_t lead;        // hours before the launch the warning comes
    uint16_t plan_mission, plan_race, plan_system, plan_planet, plan_moon;
    uint16_t in_battle;   // 1 + the fleet whose space battle this is; 0 none
    uint16_t fled;        // 1 = it turned back in the battle
    uint16_t defence;     // the target's defence when the battle began (tenths of a Hunter)
    RaidFleet fleet[RAID_FLEETS];
} RaidState;
static_assert(sizeof(RaidFleet) == 2 * 11);
static_assert(sizeof(RaidState) == 2 * 13 + RAID_FLEETS * sizeof(RaidFleet));

extern RaidState Raid_State;

// Cleared (a new game, a DOS save or one without the record loaded).
void Raid_Reset(void);

// One game hour (sim.c's step, after the story's). With the improvement off,
// or without adaptive_threat, nothing happens.
void Raid_Hour(void);

// A fleet of the race is blockading this body: nothing is loaded or unloaded there.
bool Raid_Blockaded(int16_t system, int16_t planet, int16_t moon);

// Any fleet is out (the record must reach the save).
bool Raid_Any(void);

// --- On the map -----------------------------------------------------------------------------
// A fleet at its target is an entry of the planet's fleet list (the race's icon) with
// Index RAID_ENTRY + slot; the player can attack it from there.

enum : int { RAID_ENTRY = 100 };

const RaidFleet *Raid_Fleet(int slot);
// The fleet of the slot waits or stays at this planet (or a moon of it): it can be seen and fought.
bool Raid_FleetAtPlanet(int slot, int16_t system, int16_t planet);
// The player's fleet attacks it (battle screen follows, set up here). False if it cannot be now
// (another battle is at hand, or it is not there).
bool Raid_PlayerAttack(int slot);
// Some fleet is out for a system to show on the galaxy map: 1 = on its way, 2 = at its target.
int Raid_SystemMark(int16_t system);
// The race of that fleet (for its flag); 0 if none.
int16_t Raid_MarkRace(int16_t system);

// The battle in hand is a raid's.
bool Raid_BattleActive(void);
// The fleet in that battle.
const RaidFleet *Raid_BattleFleet(void);

// After a step: the raid has lost half its weight, it turns back. `alive`
// are its ships of each class left in the battle's list; true if it turns.
bool Raid_RetreatCheck(const int32_t alive[2]);
// The battle's losses are in: the fleet is what is left. Adds the ships
// lost to `losses` (by class).
void Raid_ApplyLosses(const int32_t alive[2], int32_t losses[4]);
// The battle is over: the fleet goes home or stays, the raid shoots if the
// player lost. True if it was a raid's battle (the story's hooks are not run).
bool Raid_BattleOver(bool playerWon);

// --- Planning (pure, for the tests) ------------------------------------------

// The fleet of a raid on a target defended with `defence` (tenths of a
// Hunter) from a ceiling of `hunters` and `fighters` of the nearest wave:
// the Hunters and Starfighters that fly. `early`: the first stages (a 1.0-1.5
// times the defence rather than 1.2-1.8). Draws chance.
void Raid_PlanFleet(int32_t defence, int32_t hunters, int32_t fighters, bool early, int32_t units[2]);

// The raid shoots at a body its fleet of `raid` weight finds defended with `defence`:
// how the ceilings of the losses scale (percent of them, 10-100).
int32_t Raid_Advantage(int32_t raid, int32_t defence);

// The combat weight (tenths of a Hunter) of units[Hunters, Starfighters].
int32_t Raid_Weight(const uint16_t units[2]);
