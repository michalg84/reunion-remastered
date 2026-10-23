// Improvement (game/improvements.h, adaptive_threat): game/adaptive.h. The
// player's strength (a combat weight); a wave scaled into the band (never below scale_min or
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
#include "game/groups.h"
#include "game/improvements.h"
#include "game/intel.h"
#include "game/lang.h"
#include "game/main_program.h"
#include "game/message.h"
#include "game/mouse.h"
#include "game/pub.h"
#include "game/saveload.h"
#include "game/sim.h"
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
    CHECK(Adaptive_PlayerPower() == base + 10 * 80000 / config.adaptive.credits_per_unit);

    // A player with nothing against a wave of 40: the wave is lowered to
    // the least it may be, 70%.
    ds.Game_Credits = 0;
    TRaceForce *f = wave(MORGRULS, 2, 30, 10);
    Adaptive_Dispatched(WAVE_MORGRUL2, MORGRULS, 2);
    CHECK(f->Units[0] == 21 && f->Units[4] == 7);

    // A rich player (ratio under the band): raised to the most, 120%.
    ds.Game_Credits = config.adaptive.credits_per_unit * 400;
    f = wave(MORGRULS, 2, 30, 10);
    Adaptive_Dispatched(WAVE_MORGRUL2, MORGRULS, 2);
    CHECK(f->Units[0] == 36 && f->Units[4] == 12);

    // A player in the band leaves it as it is: 40 against 40 + 2 colonies.
    ds.Game_Credits = config.adaptive.credits_per_unit * 30;
    int32_t p = Adaptive_PlayerPower() / WEIGHT_GROUND;  // in Hunters
    f = wave(MORGRULS, 2, (int16_t)(p * 3 / 4), (int16_t)(p / 4));
    int before = total(f);
    Adaptive_Dispatched(WAVE_MORGRUL2, MORGRULS, 2);
    CHECK(total(f) >= before - 1 && total(f) <= before + 1);

    // Easy lowers the band: the same waves come out weaker.
    config.adaptive.level = CONFIG_LEVEL_EASY;
    ds.Game_Credits = config.adaptive.credits_per_unit * 200;
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

// A ship weighs by damage and hit points, not by number: a Starfighter is
// about four Hunters, so a wave of Starfighters is scaled down where as many
// Hunters are not.
static void weights(void)
{
    reset(true);
    ds.Game_Credits = config.adaptive.credits_per_unit * 1000;
    TRaceForce *h = wave(MORGRULS, 2, 200, 0);
    Adaptive_Dispatched(WAVE_MORGRUL2, MORGRULS, 2);
    CHECK(h->Units[0] == 240);  // under the band: raised to the most, 120%
    TRaceForce *sf = wave(MORGRULS, 2, 0, 0);
    sf->Units[1] = 200;
    Adaptive_Dispatched(WAVE_MORGRUL2, MORGRULS, 2);
    CHECK(sf->Units[1] > 150 && sf->Units[1] < 240);  // 200 SF weigh ~780 of ~1000: nearly the band

    // The player's own ships weigh by their weapons: more guns, more weight.
    reset(true);
    TGroup *g = Groups_At(0, 1);
    memset(g->Units, 0, sizeof g->Units);
    g->Kind = 1;
    ds.Game_GroupCount = 1;
    ds.Game_PlanetForceCount = 0;
    ds.Game_StaffExp[2] = 1;
    g->Units[0][0].Count = 10;
    g->Units[0][0].Weapons[0] = 20;  // two Lasers each
    int32_t light = Adaptive_PlayerPower();
    g->Units[0][0].Weapons[1] = 10;  // and a Twin laser each
    CHECK(Adaptive_PlayerPower() > light);
}

// The joint attack's two forces are one wave: the scale follows their sum.
static void joint(void)
{
    reset(true);
    int32_t base = Adaptive_PlayerPower();
    ds.Game_Credits = (int32_t)((1000 - base) * (int64_t)config.adaptive.credits_per_unit / WEIGHT_GROUND);
    TRaceForce *m = wave(MORGRULS, 4, 0, 100);
    TRaceForce *k = wave(KALLS, 2, 0, 100);
    int32_t p = Adaptive_PlayerPower();
    CHECK(p >= 990 && p <= 1010);  // each force alone is in the band
    config.adaptive.warn_on_launch = false;
    Adaptive_Dispatched(WAVE_JOINT, MORGRULS, 4);
    CHECK(m->Units[4] == 70 && k->Units[4] == 70);  // together at twice the band: the least, 70%
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
    ds.Game_Credits = config.adaptive.credits_per_unit * 1000;
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

// The advance warning for the other launches: the scheduled one (when the
// race is an enemy), the League's first, and the joint attack's Kalls.
static void launches(void)
{
    // The scheduled launch of the Lisonians: a contract warns, once.
    reset(true);
    config.adaptive.warn_on_launch = false;
    Intel_Race(7)->Status = 2;
    wave(7, 1, 30, 10);
    Adaptive_State.contract[7] = 1;
    int16_t timer = 500;
    int logged = 0;
    for (int h = 1; h <= 450; h++) {
        ds.Game_Hour = (uint8_t)(h % 24);
        Adaptive_Tick(WAVE_LAUNCH, &timer, 7, 1);
    }
    logged = ds.Message_LogCount;
    CHECK(logged > 0 && logged <= 6);

    // Not an enemy any more: the launch won't happen, nothing is announced.
    reset(true);
    config.adaptive.warn_on_launch = false;
    Intel_Race(7)->Status = 6;
    wave(7, 1, 30, 10);
    Adaptive_State.contract[7] = 1;
    timer = 500;
    for (int h = 1; h <= 450; h++)
        Adaptive_Tick(WAVE_LAUNCH, &timer, 7, 1);
    CHECK(ds.Message_LogCount == 0);

    // The League's first force has a slot of its own.
    reset(true);
    config.adaptive.warn_on_launch = false;
    wave(7, 1, 30, 10);
    Adaptive_State.contract[7] = 1;
    timer = 500;
    for (int h = 1; h <= 450; h++)
        Adaptive_Tick(WAVE_RACE7, &timer, 7, 1);
    CHECK(ds.Message_LogCount > 0);
    CHECK(Adaptive_State.warned[WAVE_RACE7] == 1 && !Adaptive_State.warned[WAVE_LAUNCH]);

    // The joint attack: a contract with the Kalls alone warns of both.
    reset(true);
    config.adaptive.warn_on_launch = false;
    config.adaptive.delay_max = 0;
    wave(MORGRULS, 4, 30, 10);
    wave(KALLS, 2, 30, 10);
    Adaptive_State.contract[KALLS] = 1;
    timer = 500;
    for (int h = 1; h <= 450; h++)
        Adaptive_Tick(WAVE_JOINT, &timer, MORGRULS, 4);
    CHECK(ds.Message_LogCount > 0);
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

// The pub person `who` talking, in his first menu.
static void talk(int16_t who)
{
    char digit[2] = {(char)('0' + who % 10), 0};
    ds.Pub_Person = who;
    ds.Pub_MenuIdx = 1;
    ds.Pub_PersonStr.len = 1;
    ds.Pub_PersonStr.s[0] = digit[0];
    if (!ds.Pub_Lines)
        ds.Pub_Lines = game_alloc(0x500);
    Pub_NoteEntryMenu();
    Pub_BuildQuestionMenu();
}

static bool has_item(int16_t item)
{
    for (int i = 0; i < ds.Pub_CurMenu.Count; i++)
        if (((const int8_t *)&ds.Pub_CurMenu)[1 + i] == item)
            return true;
    return false;
}

static void pub(void)
{
    reset(true);
    // Nobody is met: the informer's menu is his own.
    talk(4);
    int own = ds.Pub_CurMenu.Count;
    CHECK(own >= 1 && !has_item(PUB_CONTRACT_ITEM + MORGRULS));

    // Morgruls and Kalls met: the Informer offers both, as the last lines.
    Intel_Race(MORGRULS)->Status = 2;
    Intel_Race(KALLS)->Status = 4;
    talk(4);
    CHECK(ds.Pub_CurMenu.Count == own + 2);
    CHECK(has_item(PUB_CONTRACT_ITEM + MORGRULS) && has_item(PUB_CONTRACT_ITEM + KALLS));
    CHECK(Pub_IsContractItem(PUB_CONTRACT_ITEM + MORGRULS) && !Pub_IsContractItem(5));
    char price[16];
    SDL_snprintf(price, sizeof price, "%d", (int)Adaptive_ContractPrice(MORGRULS));
    CHECK(strstr((char *)ds.Pub_Lines + own * 256, price) && strstr((char *)ds.Pub_Lines + own * 256, "Morgruls"));

    // Others sell the others' only: the spy-hunter nothing for the Morgruls;
    // the Earthlings from the hired spy, until then from the Spy-Hunter.
    talk(1);
    CHECK(!has_item(PUB_CONTRACT_ITEM + MORGRULS));
    Intel_Race(12)->Status = 2;
    talk(2);
    CHECK(!has_item(PUB_CONTRACT_ITEM + 12));  // not hired
    talk(1);
    CHECK(has_item(PUB_CONTRACT_ITEM + 12));
    ds.Pub_StoryFlags[1] = 1;
    talk(2);
    CHECK(has_item(PUB_CONTRACT_ITEM + 12));
    talk(1);
    CHECK(!has_item(PUB_CONTRACT_ITEM + 12));
    Intel_Race(7)->Status = 2;
    talk(1);
    CHECK(has_item(PUB_CONTRACT_ITEM + 7));

    // Too poor: nothing signed. Then paid, and the line is gone.
    talk(4);
    ds.Game_Credits = 100;
    Pub_ContractAnswer(PUB_CONTRACT_ITEM + MORGRULS);
    CHECK(!Adaptive_ContractBought(MORGRULS) && ds.Game_Credits == 100 && ds.Pub_MenuIdx == 1);
    ds.Game_Credits = Adaptive_ContractPrice(MORGRULS);
    Pub_ContractAnswer(PUB_CONTRACT_ITEM + MORGRULS);
    CHECK(Adaptive_ContractBought(MORGRULS) && ds.Game_Credits == 0 && ds.Pub_MenuIdx == 1);
    talk(4);
    CHECK(!has_item(PUB_CONTRACT_ITEM + MORGRULS) && has_item(PUB_CONTRACT_ITEM + KALLS));

    // However many races are known, no menu goes over the window's seven
    // rows, and a contract's rows are no wider than its 36 characters.
    ds.Pub_StoryFlags[1] = 0;
    for (int16_t r = 2; r <= 12; r++) {
        Intel_Race(r)->Status = 2;
        Adaptive_State.contract[r] = 0;
    }
    for (int16_t who = 1; who <= 4; who++)
        for (int16_t mi = 1; mi <= 10; mi++) {
            talk(who);
            ds.Pub_MenuIdx = mi;
            Pub_NoteEntryMenu();
            Pub_BuildQuestionMenu();
            CHECK(ds.Pub_LineCount <= 7);
            for (int i = 1; i <= ds.Pub_CurMenu.Count; i++) {
                if (!Pub_IsContractItem(((const int8_t *)&ds.Pub_CurMenu)[i]))
                    continue;
                int width = 0;
                for (const char *c = (char *)ds.Pub_Lines + (i - 1) * 256 + 3; *c; c++) {
                    width = *c == '|' ? 0 : width + 1;
                    CHECK(width <= 36);
                }
            }
        }
    // Spy-Hunter, the spy in the pub not hired: his menu has four rows, room for one contract.
    talk(1);
    ds.Pub_MenuIdx = 3;
    Pub_NoteEntryMenu();
    Pub_BuildQuestionMenu();
    CHECK(ds.Pub_CurMenu.Count == 3 && ds.Pub_LineCount <= 7);
    Adaptive_State.contract[MORGRULS] = 1;
    Intel_Race(MORGRULS)->Status = 2;
    talk(4);

    // Only the first menu of a talk offers them; the switch off, none.
    ds.Pub_MenuIdx = 2;
    Pub_BuildQuestionMenu();
    CHECK(!has_item(PUB_CONTRACT_ITEM + KALLS));
    improvements.adaptive_threat = false;
    talk(4);
    CHECK(ds.Pub_CurMenu.Count == own);
}

// The story timers of sim.c, an hour at a time: the second Morgrul wave,
// 600 h after the first one's timer. A weak player: it comes later and
// smaller; the same game with the switch off: on time and as it was.
static void story(void)
{
    int original = 0, hours_on = 0, hours_off = 0, units_on = 0;
    for (int on = 0; on <= 1; on++) {
        reset(on);
        ds.Intel_MorgrulAttack2Done = 0;
        ds.Intel_MorgrulAttack2Timer = 600;
        ds.Intel_MorgrulAttack3Done = 1;
        ds.Intel_MorgrulAttack1Done = 1;
        config.adaptive.warn_on_launch = false;
        TRaceForce *f = Intel_Force(MORGRULS, 2);
        original = total(f);
        int h = 0;
        while (f->Active != 2 && h < 2000) {
            ds.Game_Hour = (uint8_t)(h % 24);
            h++;
            Sim_ProcessStoryTimers();
        }
        if (on) {
            hours_on = h;
            units_on = total(f);
        } else {
            hours_off = h;
            CHECK(total(f) == original);
        }
    }
    CHECK(hours_off == 600);
    CHECK(hours_on > 600 && hours_on <= 600 + 240 + 1);
    CHECK(units_on < original);
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

    // A contract keeps the saves in the port's format when the switch is off.
    improvements.adaptive_threat = false;
    CHECK(Improvements_PortSaves());
    Adaptive_Reset();
    CHECK(!Improvements_PortSaves());

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
    weights();
    joint();
    switch_off();
    waiting();
    warnings();
    launches();
    contracts();
    pub();
    story();
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
