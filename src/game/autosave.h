// Improvement (game/improvements.h, autosave): two rolling autosaves,
// save/reunion.auto and reunion.auto2 in the port's format
// (game/saveload.h), outside the player's 12 slots; each write goes over
// the older one. The disk screen lists them as a 13th and 14th row that
// only load.
//
// - At the start of every game hour, before Sim_AdvanceHour, and after a new
//   game or a load, the game is copied to memory as a save. The copy alone
//   is never written.
// - A battle or an alien talk starting (the switch to unit assignment 19,
//   space battle 29, ground battle 32 or alien talk 34) writes the hour's
//   copy, once per copy. It is the hour's start, not the screen's state:
//   the triggers clear their own flags in the hour they fire, so the file
//   loads into the same event again. The story's scenes (33) don't count.
// - The seventh midnight since the file was last written, by any of these,
//   writes that hour's copy.
// - Every [game] autosave_minutes minutes of real time since the last write
//   (platform/config.h; 0: never), the game as it is is written, while the
//   main loop waits for a click with the clock running, outside a battle,
//   a talk or a scene. The status line says so.
// - The quick-save key (game/keyplay.h, quick_save) writes the game as it
//   is, while the main loop waits for a click with the clock running; it is
//   refused elsewhere. The status line says which.
//
// The file's slot name is "Autosave" and the date of the state it holds.
#pragma once

#include <stdint.h>

// A new game or a load: the copy is taken now and the week starts again.
void Autosave_Reset(void);
// The start of a game hour (before Sim_AdvanceHour).
void Autosave_HourStart(void);
// The main loop enters `screen`.
void Autosave_ScreenEntered(int16_t screen);
// The quick-save key, pressed while the main loop waits for a click
// (`waiting`) or not.
void Autosave_QuickSaveKey(bool waiting);
// Each frame the main loop waits for a click: a quick save asked for, or
// the timed one due.
void Autosave_Frame(void);
// Port addition (test hook): the clock of the timed save, in milliseconds
// (nullptr: SDL_GetTicks).
void Autosave_SetTicks(uint64_t (*ticks)(void));
