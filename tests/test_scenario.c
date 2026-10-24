// Scenario tests: a save from tests/golden/saves/ as the starting point, the
// improvements switched on (or off), then game hours run through the whole
// simulation (as Main runs them, minus the screens) with assertions on
// what happens and on what must always hold. Nothing here judges how the
// game looks or plays. Needs the game data.
//
//   stability       90 days from each save, all improvements on, then all off:
//                   the date moves, the raid record stays valid, no blockade
//                   outlasts its limit, with them off no fleet of ours exists;
//   transparency    saving in the middle of a run and loading it back changes
//                   nothing later: the same state, the same raid and
//                   adaptive records after the same hours;
//   raids           from a save with a colony outside New Earth, raids come
//                   within the schedule's span.
// A summary line per scenario goes to the output.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "game/adaptive.h"
#include "game/dseg.h"
#include "game/gfx.h"
#include "game/improvements.h"
#include "game/intel.h"
#include "game/main_program.h"
#include "game/message.h"
#include "game/raid.h"
#include "game/routes.h"
#include "game/saveload.h"
#include "game/sim.h"
#include "game/state.h"
#include "game/system.h"
#include "game/textfile.h"
#include "platform/config.h"
#include "platform/fs.h"
#include "platform/video.h"

#ifndef REUNION_SOURCE_DIR
#define REUNION_SOURCE_DIR "."
#endif

static bool ok = true;
#define CHECK(c) do { if (!(c)) { printf("FAILED line %d: %s\n", __LINE__, #c); ok = false; } } while (0)

static bool wait_retrace(void) { return true; }
static bool key_pressed(void) { return true; }
static char read_key(void) { return '\r'; }

static char dir[128];

static const char *const SAVES[] = {"J01_contact", "M01_wave1", "L01_pilot", "X01_rigel1"};

static void boot(void)
{
    dseg_clear();
    vid_clear();
    dseg_init();
    system_init();
    Game_InitGameData();
    Message_Init();
    SaveLoad_LoadInit();
    Adaptive_Reset();
    Raid_Reset();
    config_reset();
    ds.Gfx_LineStride = 320;
    ds.Gfx_LineStrideCopy = 320;
    gfx_wait_retrace = wait_retrace;
    TextFile_KeyPressed = key_pressed;
    TextFile_ReadKey = read_key;
    config.colony_stats = false;
    config.detoxin_rate = false;
}

static void all_improvements(bool on)
{
    for (int i = 0; i < Improvements_Count; i++)
        config_set_improvement(Improvements_List[i].key, on);
    Improvements_Init(false);
}

static bool load(const char *name)
{
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/tests/golden/saves/%s.sav", REUNION_SOURCE_DIR, name);
    uint8_t slot_name[0x14] = {};
    return SaveLoad_TransferFile(path, slot_name, false);
}

// What a battle would have started: counted and cleared, there is no screen.
typedef struct Counts {
    int waves, battles, raid_battles;
    int fleets_seen;       // hours with a fleet of the raids out
    int blockade_run;      // longest run of hours with a blockade
    bool saw_raid, saw_blockade, saw_scout;
} Counts;

static void note(Counts *c, int *run)
{
    if (ds.Game_ScreenSwitch && ds.Game_CurrentScreen == 0x1D) {
        c->battles++;
        if (Raid_BattleActive()) {
            c->raid_battles++;
            Raid_State.in_battle = 0;
            Raid_State.fled = 0;
        } else {
            c->waves++;
        }
        ds.Game_ScreenSwitch = 0;
        ds.Game_CurrentScreen = 1;
    }
    bool blockade = false;
    for (int i = 0; i < RAID_FLEETS; i++) {
        const RaidFleet *f = &Raid_State.fleet[i];
        if (f->phase == RAID_PHASE_NONE)
            continue;
        c->fleets_seen++;
        c->saw_raid |= f->mission == RAID_RAID;
        c->saw_scout |= f->mission == RAID_SCOUT;
        if (f->mission == RAID_BLOCKADE && f->phase == RAID_PHASE_STAYING) {
            c->saw_blockade = true;
            blockade = true;
        }
    }
    *run = blockade ? *run + 1 : 0;
    c->blockade_run = SDL_max(c->blockade_run, *run);
}

// One game hour, as main_program.c's advance_hour runs it.
static void hour(void)
{
    Sim_AdvanceHour();
    Sim_ProcessStoryTimers();
    Sim_ProgressResearch();
    Sim_ProgressProduction();
    if (ds.Game_Hour == 0)
        Sim_ProcessAllBodyEvents();
    Sim_GrowStaffExperience();
    Sim_UpdateBuildings();
    Sim_UpdateColonySetups();
    Sim_MineAllBodies();
    Sim_ProcessTravellingGroups();
    Routes_Hour();
    Sim_CheckEnemyAttacks();
    Raid_Hour();
    Sim_ProcessPubTimers();
    ds.Game_Ticks = 0;
}

static int hours_of(int y, int m, int d, int h)
{
    return ((y * 12 + m) * 30 + d) * 24 + h;  // enough to compare dates
}

static int now(void)
{
    return hours_of(ds.Game_Year, ds.Game_Month, ds.Game_Day, ds.Game_Hour);
}

static void run(int n, Counts *c, int *run_len)
{
    for (int i = 0; i < n; i++) {
        hour();
        note(c, run_len);
    }
}

static bool raid_valid(void)
{
    for (int i = 0; i < RAID_FLEETS; i++) {
        const RaidFleet *f = &Raid_State.fleet[i];
        if (f->phase > RAID_PHASE_STAYING || f->mission > RAID_BLOCKADE)
            return false;
        if (f->phase != RAID_PHASE_NONE && (f->race < 2 || f->race > 12 || f->system > 255))
            return false;
    }
    return Raid_State.in_battle <= RAID_FLEETS;
}

static void stability(void)
{
    enum { DAYS = 90 };
    for (size_t s = 0; s < SDL_arraysize(SAVES); s++) {
        for (int on = 1; on >= 0; on--) {
            boot();
            all_improvements(on);
            if (!load(SAVES[s])) {
                printf("FAILED: can't load %s\n", SAVES[s]);
                ok = false;
                continue;
            }
            all_improvements(on);
            int start = now();
            Counts c = {};
            int run_len = 0;
            bool valid = true;
            for (int d = 0; d < DAYS; d++) {
                run(24, &c, &run_len);
                valid &= raid_valid();
            }
            CHECK(valid);
            CHECK(now() - start == DAYS * 24);
            // A blockade nobody breaks lifts after blockade_max hours.
            CHECK(c.blockade_run <= config.adaptive.blockade_max + 24);
            if (!on) {
                CHECK(c.fleets_seen == 0 && !Raid_Any());
                CHECK(c.raid_battles == 0);
            }
            printf("stability %-12s improvements %-3s: %d days, waves %d, raid battles %d, "
                   "fleet-hours %d\n",
                   SAVES[s], on ? "on" : "off", DAYS, c.waves, c.raid_battles, c.fleets_seen);
        }
    }
}

static bool save_slot(int16_t slot)
{
    SaveLoad_ReadSlotNames();
    uint8_t *e = ds.SaveLoad_SlotTable + (slot - 1) * 0x16;
    memcpy(e + 1, "\x08" "scenario", 9);
    ds.SaveLoad_SelectedSlot = slot;
    return SaveLoad_Transfer(true);
}

static bool load_slot(int16_t slot)
{
    SaveLoad_ReadSlotNames();
    ds.SaveLoad_SelectedSlot = slot;
    return SaveLoad_Transfer(false);
}

// Everything that should make the next hours the same.
typedef struct Snapshot {
    uint64_t state;
    StateBuf buf;
    RaidState raid;
    AdaptiveState adaptive;
} Snapshot;

static Snapshot snapshot(void)
{
    Snapshot s = {.raid = Raid_State, .adaptive = Adaptive_State};
    state_serialize(&s.buf);
    s.state = state_buf_hash(&s.buf);
    return s;
}

static bool fleet_out(void)
{
    for (int i = 0; i < RAID_FLEETS; i++)
        if (Raid_State.fleet[i].phase != RAID_PHASE_NONE)
            return true;
    return false;
}

// With `in_fleet` the save is taken while a fleet of the raids is out.
static void transparency(bool in_fleet)
{
    enum { BEFORE = 24 * 40, AFTER = 24 * 40 };
    for (size_t s = 0; s < SDL_arraysize(SAVES); s++) {
        boot();
        all_improvements(true);
        if (!load(SAVES[s])) {
            printf("FAILED: can't load %s\n", SAVES[s]);
            ok = false;
            continue;
        }
        all_improvements(true);
        Counts c = {};
        int run_len = 0;
        if (in_fleet) {
            for (int h = 0; h < 3000 && !fleet_out(); h++)
                run(1, &c, &run_len);
            if (!fleet_out())
                continue;  // this save has no colony for a fleet to go to
        } else {
            run(BEFORE, &c, &run_len);
        }
        CHECK(save_slot(3));
        // The random seed is not in a save (the original's): the same one for both runs.
        ds.System_RandSeed = 1234;
        run(AFTER, &c, &run_len);
        Snapshot straight = snapshot();

        CHECK(load_slot(3));
        ds.System_RandSeed = 1234;
        c = (Counts){};
        run(AFTER, &c, &run_len);
        Snapshot reloaded = snapshot();

        // The log of messages is not in a save (a DOS one neither): its text
        // is the only thing allowed to differ.
        int real = 0;
        StateBuf x = straight.buf, y = reloaded.buf;
        for (int shown = 0; shown < 12; shown++) {
            size_t at = state_first_difference(&x, &y);
            if (at == SIZE_MAX || at >= x.len || at >= y.len)
                break;
            char name[128];
            state_describe(&x, at, name, sizeof name);
            if (SDL_strncmp(name, "Ems_GameData", 12) != 0 && SDL_strncmp(name, "Message_Log", 11) != 0) {
                real++;
                printf("  differs at %s: %02X | %02X\n", name, x.data[at], y.data[at]);
            }
            size_t next = at + 1;
            for (size_t i = 0; i < x.nparts; i++)
                if (at >= x.parts[i].start && at < x.parts[i].start + x.parts[i].size)
                    next = x.parts[i].start + x.parts[i].size;
            if (next >= x.len || next >= y.len)
                break;
            memcpy(y.data, x.data, next);
        }
        CHECK(real == 0);
        CHECK(memcmp(&straight.raid, &reloaded.raid, sizeof straight.raid) == 0);
        CHECK(memcmp(&straight.adaptive, &reloaded.adaptive, sizeof straight.adaptive) == 0);
        printf("transparency %-12s%s: %d days after the save, state %016llx %s\n", SAVES[s],
               in_fleet ? " (fleet out)" : "", AFTER / 24, (unsigned long long)straight.state,
               real == 0 ? "same" : "DIFFERENT");
    }
}

static void raids(void)
{
    enum { DAYS = 120 };
    int found = 0;
    for (size_t s = 0; s < SDL_arraysize(SAVES); s++) {
        boot();
        all_improvements(true);
        if (!load(SAVES[s]))
            continue;
        all_improvements(true);
        Counts c = {};
        int run_len = 0;
        for (int d = 0; d < DAYS; d++)
            run(24, &c, &run_len);
        bool any = c.saw_raid || c.saw_blockade || c.saw_scout;
        found += any;
        printf("raids %-12s: %d days, scout %d, raid %d, blockade %d, raid battles %d\n", SAVES[s],
               DAYS, c.saw_scout, c.saw_raid, c.saw_blockade, c.raid_battles);
    }
    // At least one of the saves has a colony for them to go to.
    CHECK(found >= 1);
}

int main(int argc, char *argv[])
{
    if (argc < 2 || !fs_init(argv[1])) {
        printf("no game data, skipping\n");
        return 77;
    }
    // Never touch the real save directory.
    SDL_snprintf(dir, sizeof dir, "/tmp/reunion_test_scenario_%d",
                 (int)SDL_GetCurrentTime(nullptr) & 0xFFFF);
    SaveLoad_SetSaveDir(dir);

    stability();
    transparency(false);
    transparency(true);
    raids();

    char p[256];
    SDL_snprintf(p, sizeof p, "%s/reunion.3", dir);
    SDL_RemovePath(p);
    SDL_snprintf(p, sizeof p, "%s/spidysav.3", dir);
    SDL_RemovePath(p);
    SDL_RemovePath(dir);
    return ok ? 0 : 1;
}
