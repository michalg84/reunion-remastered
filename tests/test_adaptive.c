// Improvement (game/improvements.h, adaptive_threat): game/adaptive.h. The
// player's strength; a wave scaled into the band (never below scale_min or
// above scale_max, more for waiting); a wave waiting at half speed for a
// weak player, for at most delay_max of its interval; the launch message
// and the advance warning (a contract, or an observatory in the race's home
// system); the contracts (offered once the race is known, a price between
// the limits that stays, paid once); the state in a save. With the switch
// off the timers and the units are the original's. Needs the game data.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "game/adaptive.h"
#include "game/dseg.h"
#include "game/gfx.h"
#include "game/improvements.h"
#include "game/intel.h"
#include "game/lang.h"
#include "game/main_program.h"
#include "game/message.h"
#include "game/mouse.h"
#include "game/saveload.h"
#include "game/system.h"
#include "game/textfile.h"
#include "platform/config.h"
#include "platform/fs.h"
#include "platform/video.h"

static bool ok = true;
#define CHECK(c) do { if (!(c)) { printf("FAILED line %d: %s\n", __LINE__, #c); ok = false; } } while (0)

enum { MORGRULS = 3, KALLS = 4 };

static bool wait_retrace(void) { return true; }
// Message boxes wait for a key: answer at once.
static bool key_pressed(void) { return true; }
static char read_key(void) { return '\r'; }

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
    TextFile_KeyPressed = key_pressed;
    TextFile_ReadKey = read_key;
    ds.Message_LogCount = 0;
    improvements.adaptive_threat = on;
    ds.Game_Credits = 0;
    ds.System_RandSeed = 1234;
}

// A force of `ships` ships of the first class and `ground` ground units.
static TRaceForce *wave(int16_t race, int16_t force, int16_t ships, int16_t ground)
{
    TRaceForce *f = Intel_Force(race, force);
    memset(f->Units, 0, sizeof f->Units);
    f->Units[0] = ships;
    f->Units[4] = ground;
    f->System = 1;
    f->Body = 5;
    f->TravelTime = 55;
    f->Timer = 30;
    return f;
}

static int total(const TRaceForce *f)
{
    int n = 0;
    for (int u = 0; u < 8; u++)
        n += f->Units[u];
    return n;
}

static void power_and_scale(void)
{
    reset(true);
    int32_t base = Adaptive_PlayerPower();
    ds.Game_Credits = 80000;
    CHECK(Adaptive_PlayerPower() == base + 10);  // credits_per_unit 8000

    // A player with nothing against a wave of 40: the wave is lowered to
    // the least it may be, 70%.
    ds.Game_Credits = 0;
    TRaceForce *f = wave(MORGRULS, 2, 30, 10);
    Adaptive_Dispatched(WAVE_MORGRUL2, MORGRULS, 2);
    CHECK(f->Units[0] == 21 && f->Units[4] == 7);

    // A rich player (ratio under the band): raised to the most, 120%.
    ds.Game_Credits = 8000 * 400;
    f = wave(MORGRULS, 2, 30, 10);
    Adaptive_Dispatched(WAVE_MORGRUL2, MORGRULS, 2);
    CHECK(f->Units[0] == 36 && f->Units[4] == 12);

    // A player in the band leaves it as it is: 40 against 40 + 2 colonies.
    ds.Game_Credits = 8000 * (40 - 2 * config.adaptive.colony_units);
    int32_t p = Adaptive_PlayerPower();
    f = wave(MORGRULS, 2, (int16_t)(p * 3 / 4), (int16_t)(p / 4));
    int before = total(f);
    Adaptive_Dispatched(WAVE_MORGRUL2, MORGRULS, 2);
    CHECK(total(f) >= before - 1 && total(f) <= before + 1);

    // Easy lowers the band: the same waves come out weaker.
    config.adaptive.level = CONFIG_LEVEL_EASY;
    ds.Game_Credits = 8000 * 200;
    f = wave(MORGRULS, 2, 40, 0);
    Adaptive_Dispatched(WAVE_MORGRUL2, MORGRULS, 2);
    int easy = total(f);
    config.adaptive.level = CONFIG_LEVEL_HARD;
    f = wave(MORGRULS, 2, 40, 0);
    Adaptive_Dispatched(WAVE_MORGRUL2, MORGRULS, 2);
    CHECK(easy <= total(f));
    config.adaptive.level = CONFIG_LEVEL_NORMAL;

    // Waiting adds 4% per 100 h.
    ds.Game_Credits = 0;
    f = wave(MORGRULS, 2, 100, 0);
    Adaptive_State.waited[WAVE_MORGRUL2] = 200;
    Adaptive_Dispatched(WAVE_MORGRUL2, MORGRULS, 2);
    CHECK(f->Units[0] == 78);  // 70% + 8%
    CHECK(Adaptive_State.waited[WAVE_MORGRUL2] == 0);
}

static void switch_off(void)
{
    reset(false);
    int16_t timer = 3;
    CHECK(!Adaptive_Tick(WAVE_MORGRUL2, &timer, MORGRULS, 2) && timer == 2);
    CHECK(!Adaptive_Tick(WAVE_MORGRUL2, &timer, MORGRULS, 2));
    CHECK(Adaptive_Tick(WAVE_MORGRUL2, &timer, MORGRULS, 2) && timer == 0);
    CHECK(!Adaptive_Tick(WAVE_MORGRUL2, &timer, MORGRULS, 2));  // 0: not armed

    TRaceForce *f = wave(MORGRULS, 2, 30, 10);
    Adaptive_Dispatched(WAVE_MORGRUL2, MORGRULS, 2);
    CHECK(f->Units[0] == 30 && f->Units[4] == 10 && ds.Message_LogCount == 0);
    CHECK(!Adaptive_ContractOffered(KALLS));
}

// Hours until a timer of `start` runs out, one tick an hour.
static int run(int wave_no, int16_t start, int16_t race, int16_t force)
{
    int16_t timer = start;
    int hours = 0;
    while (hours < 10000) {
        ds.Game_Hour = (uint8_t)(hours % 24);
        hours++;
        if (Adaptive_Tick(wave_no, &timer, race, force))
            break;
    }
    return hours;
}

static void waiting(void)
{
    // A weak player: wave 2 waits 40% of its interval, at half speed, no more.
    reset(true);
    wave(MORGRULS, 2, 100, 50);
    int h = run(WAVE_MORGRUL2, 1000, MORGRULS, 2);
    CHECK(h > 1000 && h <= 1000 + 400 + 1);
    CHECK(Adaptive_State.waited[WAVE_MORGRUL2] > 300);

    // A strong one: the wave comes on time.
    reset(true);
    ds.Game_Credits = 8000 * 1000;
    wave(MORGRULS, 2, 100, 50);
    CHECK(run(WAVE_MORGRUL2, 1000, MORGRULS, 2) == 1000);

    // The first wave never waits, nor do the launches.
    reset(true);
    wave(MORGRULS, 1, 100, 50);
    CHECK(run(WAVE_MORGRUL1, 600, MORGRULS, 1) == 600);

    // delay_max 0: no waiting at all.
    reset(true);
    config.adaptive.delay_max = 0;
    wave(MORGRULS, 2, 100, 50);
    CHECK(run(WAVE_MORGRUL2, 1000, MORGRULS, 2) == 1000);
}

static void warnings(void)
{
    // The launch message is a log entry; with warn_on_launch off, none.
    reset(true);
    wave(MORGRULS, 2, 30, 10);
    Adaptive_Dispatched(WAVE_MORGRUL2, MORGRULS, 2);
    int per = ds.Message_LogCount;  // a box logs a line for each of its lines
    CHECK(per > 0 && per <= 6);
    reset(true);
    config.adaptive.warn_on_launch = false;
    wave(MORGRULS, 2, 30, 10);
    Adaptive_Dispatched(WAVE_MORGRUL2, MORGRULS, 2);
    CHECK(ds.Message_LogCount == 0);

    // Nothing early without a contract or an observatory.
    reset(true);
    config.adaptive.delay_max = 0;
    wave(MORGRULS, 2, 30, 10);
    ds.Sim_Observatories[Intel_Race(MORGRULS)->HomeSystem - 1] = 0;
    run(WAVE_MORGRUL2, 500, MORGRULS, 2);
    CHECK(ds.Message_LogCount == 0);

    // With an observatory in the home system: once, 100..200 h before.
    reset(true);
    config.adaptive.delay_max = 0;
    wave(MORGRULS, 2, 30, 10);
    ds.Sim_Observatories[Intel_Race(MORGRULS)->HomeSystem - 1] = 1;
    int16_t timer = 500;
    int warned_at = 0;
    for (int h = 1; h <= 500; h++) {
        int before = ds.Message_LogCount;
        Adaptive_Tick(WAVE_MORGRUL2, &timer, MORGRULS, 2);
        if (ds.Message_LogCount > before)
            warned_at = timer;
    }
    CHECK(ds.Message_LogCount == per && warned_at >= 99 && warned_at <= 200);

    // With warn_intel off the observatory is not enough; a contract is.
    reset(true);
    config.adaptive.delay_max = 0;
    config.adaptive.warn_intel = false;
    wave(MORGRULS, 2, 30, 10);
    ds.Sim_Observatories[Intel_Race(MORGRULS)->HomeSystem - 1] = 1;
    run(WAVE_MORGRUL2, 500, MORGRULS, 2);
    CHECK(ds.Message_LogCount == 0);
    reset(true);
    config.adaptive.delay_max = 0;
    wave(MORGRULS, 2, 30, 10);
    Adaptive_State.contract[MORGRULS] = 1;
    run(WAVE_MORGRUL2, 500, MORGRULS, 2);
    CHECK(ds.Message_LogCount == per);
}

static void contracts(void)
{
    reset(true);
    // The Morgruls are not known at the start, nor are the Kalls.
    CHECK(Intel_Race(MORGRULS)->Status == 0 && !Adaptive_ContractOffered(MORGRULS));
    CHECK(!Adaptive_ContractOffered(KALLS));
    // Met: offered (hostile or neutral), not allied, not eliminated.
    Intel_Race(MORGRULS)->Status = 2;
    CHECK(Adaptive_ContractOffered(MORGRULS));
    Intel_Race(MORGRULS)->Status = 6;
    CHECK(!Adaptive_ContractOffered(MORGRULS));
    Intel_Race(MORGRULS)->Status = -1;
    CHECK(!Adaptive_ContractOffered(MORGRULS));
    Intel_Race(MORGRULS)->Status = 2;

    // The price: between the limits, a multiple of 10, the same each time,
    // and not the same for every race.
    int32_t price = Adaptive_ContractPrice(MORGRULS);
    CHECK(price >= 50000 && price <= 80000 && price % 10 == 0);
    CHECK(Adaptive_ContractPrice(MORGRULS) == price);
    bool differ = false;
    for (int16_t r = 4; r <= 12; r++) {
        int32_t pr = Adaptive_ContractPrice(r);
        CHECK(pr >= 50000 && pr <= 80000);
        differ |= pr != price;
    }
    CHECK(differ);

    // Not enough credits: nothing signed. Then paid once.
    ds.Game_Credits = price - 1;
    CHECK(!Adaptive_BuyContract(MORGRULS) && !Adaptive_ContractBought(MORGRULS));
    ds.Game_Credits = price + 5;
    CHECK(Adaptive_BuyContract(MORGRULS) && Adaptive_ContractBought(MORGRULS));
    CHECK(ds.Game_Credits == 5);
    CHECK(!Adaptive_ContractOffered(MORGRULS) && !Adaptive_BuyContract(MORGRULS));

    // A race known by the player's presence in its home system.
    reset(true);
    int16_t home = Intel_Race(KALLS)->HomeSystem;
    CHECK(home > 1 && !Adaptive_ContractOffered(KALLS));
    ds.Galaxy_Bodies[home - 1][0].Owner = 1;
    CHECK(Adaptive_ContractOffered(KALLS));

    // The limits are the config's.
    reset(true);
    config.adaptive.contract_min = 20000;
    config.adaptive.contract_max = 20000;
    CHECK(Adaptive_ContractPrice(5) == 20000);
}

static bool save_slot(int16_t slot)
{
    SaveLoad_ReadSlotNames();
    uint8_t *e = ds.SaveLoad_SlotTable + (slot - 1) * 0x16;
    memcpy(e + 1, "\x08" "adaptive", 9);
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
    Intel_Race(MORGRULS)->Status = 2;
    int32_t price = Adaptive_ContractPrice(MORGRULS);
    ds.Game_Credits = price;
    CHECK(Adaptive_BuyContract(MORGRULS));
    Adaptive_State.waited[WAVE_MORGRUL3] = 77;
    Adaptive_State.lead[WAVE_MORGRUL3] = 150;
    CHECK(SaveLoad_PortFormat() && save_slot(3));

    Adaptive_Reset();
    CHECK(!Adaptive_ContractBought(MORGRULS));
    CHECK(load_slot(3));
    CHECK(Adaptive_ContractBought(MORGRULS) && Adaptive_ContractPrice(MORGRULS) == price);
    CHECK(Adaptive_State.waited[WAVE_MORGRUL3] == 77 && Adaptive_State.lead[WAVE_MORGRUL3] == 150);

    // A DOS save has none.
    reset(false);
    CHECK(!SaveLoad_PortFormat() && save_slot(4));
    Adaptive_State.contract[MORGRULS] = 1;
    CHECK(load_slot(4));
    CHECK(!Adaptive_ContractBought(MORGRULS));
}

int main(int argc, char *argv[])
{
    if (argc < 2 || !fs_init(argv[1])) {
        printf("no game data, skipping\n");
        return 77;
    }
    // Never touch the real save directory.
    SDL_snprintf(dir, sizeof dir, "/tmp/reunion_test_adaptive_%d",
                 (int)SDL_GetCurrentTime(nullptr) & 0xFFFF);
    SaveLoad_SetSaveDir(dir);

    power_and_scale();
    switch_off();
    waiting();
    warnings();
    contracts();
    saves();

    for (int n = 3; n <= 4; n++) {
        char p[256];
        SDL_snprintf(p, sizeof p, "%s/reunion.%d", dir, n);
        SDL_RemovePath(p);
        SDL_snprintf(p, sizeof p, "%s/spidysav.%d", dir, n);
        SDL_RemovePath(p);
    }
    SDL_RemovePath(dir);
    SaveLoad_SetSaveDir(nullptr);
    fs_shutdown();
    printf("%s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
