// Improvement (game/improvements.h, adaptive_threat): the alien waves follow
// the player's strength instead of the clock alone. Settings: [adaptive] of
// the config (platform/config.h).
//
// Strength is counted in units: the player's ships and ground units, their
// credits (credits_per_unit each) and colonies (colony_units each). A wave
// is the units of its race force. The ratio wave / player decides:
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
//    (warn_intel) or an intelligence contract for the race was bought.
//
// A contract is bought at the bar, once per race, for good: its price is
// drawn once between contract_min and contract_max. Its state, with the
// waves', is a port record (AdaptiveState, game/saveload.h).
//
// Only the waves sent by the story timers (sim.c) are covered; with the
// switch off nothing here changes the game.
#pragma once

#include <stdint.h>

// The waves, by where they start in sim.c.
enum : int {
    WAVE_MORGRUL1,  // no waiting: the player has nothing yet
    WAVE_MORGRUL2,
    WAVE_MORGRUL3,
    WAVE_JOINT,
    WAVE_LAUNCH,    // the scheduled launches (Intel_LaunchTimer): scaled, no waiting
    WAVE_COUNT,
};

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

// The player's strength in units.
int32_t Adaptive_PlayerPower(void);

// The story timer of a wave counts down one hour (the place of sim.c's
// tick()): true when it runs out. With the switch off that is all it does.
bool Adaptive_Tick(int wave, int16_t *timer, int16_t race, int16_t force);

// A wave has been sent (Intel_DispatchForce, sim.c): its units are scaled
// and the launch message is shown.
void Adaptive_Dispatched(int wave, int16_t race, int16_t force);

// Intelligence contracts. The race is known (met, or the player is in its
// home system), alive, not an ally, and has no contract yet.
bool Adaptive_ContractOffered(int16_t race);
bool Adaptive_ContractBought(int16_t race);
// The price, drawn on first asking and the same after.
int32_t Adaptive_ContractPrice(int16_t race);
// Pays the price (false if the player can't) and signs.
bool Adaptive_BuyContract(int16_t race);
