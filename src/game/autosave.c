#include "game/autosave.h"

#include <SDL3/SDL.h>

#include "game/dseg.h"
#include "game/gfx.h"
#include "game/improvements.h"
#include "game/lang.h"
#include "game/saveload.h"
#include "platform/config.h"

enum : int { WEEK = 7, STATUS_FRAMES = 150 };  // midnights; frames the status line keeps its word

static struct {
    void *data;  // the copy
    size_t size;
    uint32_t id, written;  // the copy's number; the last one written
    int midnights;         // since the file was last written
    uint64_t written_ms;   // when a file was last written, or the game began
    bool quick_save;       // asked for
    const char *status;    // the status line's word, redrawn while status_frames last
    int status_frames;
} autosave;

static uint64_t (*ticks)(void) = SDL_GetTicks;

void Autosave_SetTicks(uint64_t (*t)(void))
{
    ticks = t ? t : SDL_GetTicks;
}

static void drop_copy(void)
{
    SDL_free(autosave.data);
    autosave.data = nullptr;
}

// The game as it is now, named for the slot.
static void *save_now(size_t *size)
{
    char text[32];
    SDL_snprintf(text, sizeof text, "%s %d-%d-%d", Lang_Text("Autosave"), ds.Game_Year,
                 ds.Game_Month, ds.Game_Day);
    uint8_t name[0x14] = {};
    size_t len = SDL_min(SDL_strlen(text), sizeof name - 1);
    name[0] = (uint8_t)len;
    SDL_memcpy(name + 1, text, len);
    return SaveLoad_SaveToMemory(name, size);
}

static void take_copy(void)
{
    drop_copy();
    autosave.data = save_now(&autosave.size);
    autosave.id++;
}

static bool write_file(const void *data, size_t size)
{
    if (!SaveLoad_WriteAutosave(data, size)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Autosave: %s", SDL_GetError());
        return false;
    }
    autosave.midnights = 0;
    autosave.written_ms = ticks();
    return true;
}

static void write_copy(void)
{
    if (autosave.data && autosave.written != autosave.id) {
        write_file(autosave.data, autosave.size);
        autosave.written = autosave.id;
    }
}

void Autosave_Reset(void)
{
    autosave.midnights = 0;
    autosave.written_ms = ticks();
    autosave.quick_save = false;
    autosave.status_frames = 0;
    if (improvements.autosave)
        take_copy();
    else
        drop_copy();
}

void Autosave_HourStart(void)
{
    if (!improvements.autosave) {
        drop_copy();
        return;
    }
    take_copy();
    if (ds.Game_Hour == 23 && ++autosave.midnights >= WEEK)
        write_copy();
}

// Unit assignment, space battle, ground battle, alien talk.
static bool battle_or_talk(int16_t screen)
{
    return screen == 19 || screen == 29 || screen == 32 || screen == 34;
}

void Autosave_ScreenEntered(int16_t screen)
{
    if (improvements.autosave && battle_or_talk(screen))
        write_copy();
}

void Autosave_QuickSaveKey(bool waiting)
{
    if (!improvements.autosave)
        return;
    if (!waiting)
        Gfx_DrawStatusText(Lang_Text("Can't save now"));
    else
        autosave.quick_save = true;
}

// The timed save, due and allowed now.
static bool timed_due(void)
{
    int16_t screen = ds.Game_CurrentScreen;
    return improvements.autosave && config.autosave_minutes > 0 && ds.Game_ClockRunning
        && !battle_or_talk(screen) && screen != 33
        && ticks() - autosave.written_ms >= (uint64_t)config.autosave_minutes * 60000;
}

void Autosave_Frame(void)
{
    // The hint line is redrawn by the screens at once; the word stays a while.
    if (autosave.status_frames > 0) {
        autosave.status_frames--;
        Gfx_DrawStatusText(autosave.status);
    }
    if (!autosave.quick_save && !timed_due())
        return;
    autosave.quick_save = false;
    autosave.written_ms = ticks();  // a failed write is tried again after the same time
    size_t size;
    void *data = ds.Game_ClockRunning ? save_now(&size) : nullptr;
    bool saved = data && write_file(data, size);
    SDL_free(data);
    autosave.status = Lang_Text(saved ? "Game saved" : "Can't save now");
    autosave.status_frames = STATUS_FRAMES;
    Gfx_DrawStatusText(autosave.status);
}
