// Improvement (game/improvements.h, adaptive_threat): the alien waves follow
// the player's strength instead of the clock alone. Settings: [adaptive] of
// the config (platform/config.h).
//
// Strength is a combat weight, in tenths of a Hunter: a ship counts by the
// square root of its average damage per turn times its hit points (Hunter 1,
// Starfighter 3.9, Destroyer 14.6, Cruiser 33.9; the player's by their real
// weapons), a ground unit 1; the player's credits (credits_per_unit each)
// and colonies (colony_units each) count as that many Hunters more. A wave
// is the weight of its race force (the joint attack: of both its forces).
// The ratio wave / player decides:
//
//  - Scale. When it is outside the band the level sets (band_low..band_high
//    at Normal, 20 points down at Easy, up at Hard), the wave's units are
//    scaled toward the band, never below scale_min or above scale_max
//    (percent of the original's). Waiting adds delay_bonus per 100 h.
//  - Wait. When even the smallest wave would be over the band, the wave's
//    timer runs at half speed, for at most delay_max percent of the
//    interval it started with. Then it comes as it is.
//  - Warn. When a wave sets off, a message says who, where, when and how
//    many (warn_on_launch). 100-200 h before it (drawn per wave) a message
//    says the same, roughly, if the race's home system has an observatory
//    or a satellite watches its home body (warn_intel) or an intelligence
//    contract for the race was bought. A wave that may wait names the
//    later end of the window too.
//
// A contract is bought at the bar, once per race, for good: its price is
// drawn once between contract_min and contract_max. Its state, with the
// waves', is a port record (AdaptiveState, game/saveload.h).
//
// Every launch of the story timers (sim.c) is covered: the Morgrul waves,
// the joint attack, the scheduled launches (Intel_LaunchTimer) and the
// League's first (Intel_Race7to10Timer). With the switch off nothing here
// changes the game.
#pragma once

#include <stddef.h>
#include <stdint.h>

// The waves, by where they start in sim.c.
enum : int {
    WAVE_MORGRUL1,  // no waiting: the player has nothing yet
    WAVE_MORGRUL2,
    WAVE_MORGRUL3,
    WAVE_JOINT,
    WAVE_LAUNCH,    // the scheduled launches (Intel_LaunchTimer): scaled, no waiting
    WAVE_RACE7,     // the League's first (Intel_Race7to10Timer): scaled, no waiting
    WAVE_COUNT,
};

// A ground unit's weight, and so a Hunter's tenth: strength is in tenths.
enum : int { WEIGHT_GROUND = 10 };

// Bytes only (uint16 all through), so the record is its own file layout.
typedef struct AdaptiveState {
    uint16_t contract[13];  // by race (2..12): 1 = bought
    uint16_t price10[13];   // ... its price / 10; 0 = not drawn yet
    uint16_t start[WAVE_COUNT];   // the timer when first seen
    uint16_t waited[WAVE_COUNT];  // hours it has waited
    uint16_t warned[WAVE_COUNT];  // 1 = the advance warning was given
    uint16_t lead[WAVE_COUNT];    // hours before it the warning comes (100..200)
} AdaptiveState;
static_assert(sizeof(AdaptiveState) == 2 * (13 + 13 + 4 * WAVE_COUNT));

extern AdaptiveState Adaptive_State;

// Cleared (a new game, a DOS save or one without the record loaded).
void Adaptive_Reset(void);

// The player's strength, in tenths of a Hunter.
int32_t Adaptive_PlayerPower(void);
// A ship's combat weight in tenths of a Hunter, from its damage per turn
// (at the 11% chance to hit) and its hit points.
int32_t Adaptive_ShipWeight(int64_t damage, int32_t hp);

// The story timer of a wave counts down one hour (the place of sim.c's
// tick()): true when it runs out. With the switch off that is all it does.
bool Adaptive_Tick(int wave, int16_t *timer, int16_t race, int16_t force);

// A wave has been sent (Intel_DispatchForce, sim.c; the joint attack once,
// after both its forces): its units are scaled and the launch message is shown.
void Adaptive_Dispatched(int wave, int16_t race, int16_t force);

// Intelligence contracts. The race is known (met, or the player is in its
// home system), alive, not an ally, and has no contract yet.
bool Adaptive_ContractOffered(int16_t race);
bool Adaptive_ContractBought(int16_t race);
// Some contract is signed: its record must reach the save (a port save).
bool Adaptive_AnyContract(void);
// The pub person (Pub_Person, game/pub.h) who sells the contract for the
// race: the Informer (4) the early races, the Spy-Hunter (1) the League's
// and, until the spy is hired, the last two races', then the hired spy (2)
// (Pub_StoryFlags[1]); 0 for none.
int16_t Adaptive_Seller(int16_t race);
// The race's name for a sentence: "Morgruls", without the article.
const char *Adaptive_RaceLabel(int16_t race, char *buf, size_t size);
// The price, drawn on first asking and the same after.
int32_t Adaptive_ContractPrice(int16_t race);
// Pays the price (false if the player can't) and signs.
bool Adaptive_BuyContract(int16_t race);
