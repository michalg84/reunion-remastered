// The event log (config.diagnostics.event_log): what happens in a game, one
// JSON object a line in logs/events-*.jsonl (platform/diag.h), for finding
// out how the aliens' attacks go and tuning them (tools/analyze_events.py).
//
// It only reads the game's state: with it on or off, the game plays the
// same. Every line has "type", "ts" (the real time) and "date" (the game's,
// "2263-05-12 14"). Types:
//
//   session         the log starts: version, improvements and fixes on
//   time_jump       the game's date didn't go on by an hour (a new game, a
//                   load): the comparisons below start again from here
//   day             midnight: credits, colonies, population, ore, the
//                   player's units and every race's status, forces and units
//   story           a story timer ran out or its flag was set (sim.c)
//   force_dispatch  a race force set off (Active 2): target, travel, units
//   force_arrival   ... arrived (Active 1)
//   attack          a force at its target attacked (Sim_CheckEnemyAttacks):
//                   mission, units, the player's defence there, battle or not
//   space_battle_start / space_battle_end     ships, losses, who won
//   ground_battle_start / ground_battle_end   units, losses, who won
//   body_owner      a body changed hands (colony founded, taken, lost)
//   race_status     a race's status changed (met, hostile, allied, eliminated)
//   research_done   an invention was finished
//
// Units: the 8 of a race force (ship classes 1..4, ground units 1..4); the
// player's as "ships" and "ground", 4 each.
#pragma once

#include <stdint.h>

// Registers the game's state for crash and hang reports and writes the
// session line if the log is on.
void EvLog_Init(void);
// The log is on (cheap: callers may skip their work).
bool EvLog_On(void);

// After a game hour (main_program.c): compares with the hour before and
// writes what changed; at midnight the day line.
void EvLog_Hour(void);
// A race force at its target attacks (sim.c), before NextMission is cleared.
void EvLog_Attack(int16_t race, int16_t force, bool defended, bool battle);
// Battles (spwar.c, grwar.c): after the fleets and units are set up and
// after the result is known.
void EvLog_SpaceBattleStart(void);
void EvLog_SpaceBattleEnd(void);
void EvLog_GroundBattleStart(void);
void EvLog_GroundBattleEnd(void);
