#!/usr/bin/env python3
"""Monte Carlo of the space battle (src/game/spwar.c step_side) with the
planetary defences of the mod's stage 2 (PLAN-otwarty-swiat.md, E2.3/E2.4):
how often a defence holds against a raid and what it loses.

    python tools/spacebattle_sim.py                     # the stage 2 scenarios
    python tools/spacebattle_sim.py --defence twin=10,ion=1,SF=10 --raid H=27,SF=27
    python tools/spacebattle_sim.py --defence twin=4 --raid H=8 --shield 0.3

The rule, as in the original: each step the player's side fires first, then
the aliens. Every live unit hits a random live foe with chance `hit` (11% for
ships); a small ship (Hunter, Starfighter) hitting a big one (Destroyer,
Cruiser) does a third of its damage; a small ship hitting a small one may
destroy it outright (SpWar_KillChance). The battle runs until one side is gone.

Mod additions, with the values accepted on 2026-10-23: the Twin laser battery
(68 damage, 3000 HP, half the outright-kill chance), the Ion cannon (600
damage, hits 40% of steps, 3000 HP, targeted as a small ship but never killed
outright, fires as a big ship) and the Energy shield (enemy hits on buildings
miss with the given chance). Player ships are fully equipped (Hunter 2 Laser
+ 1 Twin, Starfighter 4 Twin + 1 Missile) with a commander factor of 1; the
buildings take no commander factor.

Decided on 2026-10-23: a raid retreats after losing half its combat weight
(sqrt(damage per step x HP), in Hunters), which counts as the defence holding;
a fifth of the destroyed ships of both sides survive, as in the original,
destroyed buildings do not. --retreat 0 --no-fifth gives the old rule.
"""
import argparse
import random
from dataclasses import dataclass

# SpWar_KillChance [shooter][target], 1 Hunter, 2 Starfighter: r = Random(100) <= chance
KILL = {(1, 1): 3, (1, 2): 1, (2, 1): 6, (2, 2): 3}

# kind: (class, damage, hp, hit %, outright-kill factor, fires as class, building)
UNITS = {
    "H": (1, 11, 800, 11, 1.0, 1, False),       # Hunter (SpWar_ClassB / ClassA)
    "SF": (2, 68, 2000, 11, 1.0, 2, False),     # Starfighter
    "D": (3, 374, 5000, 11, 1.0, 3, False),     # Destroyer
    "C": (4, 840, 12000, 11, 1.0, 4, False),    # Cruiser
    "twin": (2, 68, 3000, 11, 0.5, 2, True),    # Twin laser battery
    "ion": (2, 600, 3000, 40, 0.0, 4, True),    # Ion cannon
}

# Stage 2 raids: 30% of the next main wave, Hunters and Starfighters only.
SCENARIOS = [
    ("after wave 1: 8H", "twin=3", "H=8"),
    ("after wave 1: 8H", "twin=4", "H=8"),
    ("after wave 2: 6H 3SF", "twin=5", "H=6,SF=3"),
    ("after wave 2: 6H 3SF", "twin=10", "H=6,SF=3"),
    ("League: 6H 8SF", "twin=5", "H=6,SF=8"),
    ("League: 6H 8SF", "twin=10", "H=6,SF=8"),
    ("League: 6H 8SF", "twin=10,ion=1", "H=6,SF=8"),
    ("before wave 5: 27H 27SF", "twin=10,ion=1", "H=27,SF=27"),
    ("before wave 5: 27H 27SF", "twin=10,ion=1,SF=10", "H=27,SF=27"),
    ("before wave 5: 27H 27SF", "twin=10,ion=1,SF=20", "H=27,SF=27"),
]


@dataclass
class Unit:
    kind: str
    cls: int
    dmg: int
    hp: int
    hit: int
    killmul: float
    fires_as: int
    building: bool


def parse(spec):
    """'twin=10,ion=1,SF=10' -> {'twin': 10, 'ion': 1, 'SF': 10}"""
    out = {}
    for part in filter(None, (p.strip() for p in spec.split(","))):
        kind, _, n = part.partition("=")
        if kind not in UNITS:
            raise SystemExit(f"unknown unit {kind!r}; known: {', '.join(UNITS)}")
        out[kind] = out.get(kind, 0) + int(n or 1)
    return out


def make(spec):
    return [Unit(kind, *UNITS[kind]) for kind, n in spec.items() for _ in range(n)]


def step(own, foe, shield, rng):
    for s in list(own):
        if s.hp <= 0:
            continue
        if not foe:
            return
        if rng.randrange(100) >= s.hit:
            continue
        tg = foe[rng.randrange(len(foe))]
        if tg.building and rng.random() < shield:
            continue
        tg.hp -= s.dmg // 3 if s.fires_as < 3 and tg.cls > 2 else s.dmg
        if s.fires_as < 3 and tg.cls < 3 and tg.killmul > 0:
            if rng.random() < (KILL[(s.fires_as, tg.cls)] + 1) / 100 * tg.killmul:
                tg.hp = 0
        if tg.hp < 1:
            foe.remove(tg)


def weight(u):
    """Combat weight in Hunters: sqrt(average damage per step x HP)."""
    return (u.hit / 100 * u.dmg * u.hp) ** 0.5 / (0.11 * 11 * 800) ** 0.5


def battle(defence, raid, shield, rng, retreat=0.0, cap=20000):
    """'won', 'retreat' (the raid left after losing `retreat` of its weight)
    or 'lost' (None on reaching the step cap); and the survivors."""
    d, a = make(defence), make(raid)
    start = sum(weight(u) for u in a)
    for _ in range(cap):
        step(d, a, shield, rng)
        step(a, d, shield, rng)
        if not d:
            return "lost", d, a
        if not a:
            return "won", d, a
        if retreat and sum(weight(u) for u in a) < (1 - retreat) * start:
            return "retreat", d, a
    return None, d, a


def lost_ships(spec, alive, fifth):
    """Ships lost by kind; with `fifth` a fifth of the destroyed survive (SpWar_ApplySpaceLosses)."""
    n = 0
    for kind, orig in spec.items():
        if UNITS[kind][6]:
            continue
        dead = orig - sum(u.kind == kind for u in alive)
        n += dead - (dead // 5 if fifth else 0)
    return n


def simulate(defence, raid, shield, runs, rng, retreat=0.0, fifth=False):
    wins = retreats = buildings_lost = ships_lost = raiders_lost = 0
    n_buildings = sum(n for k, n in defence.items() if UNITS[k][6])
    n_ships = sum(defence.values()) - n_buildings
    for _ in range(runs):
        result, d, a = battle(defence, raid, shield, rng, retreat)
        wins += result in ("won", "retreat")
        retreats += result == "retreat"
        buildings_lost += n_buildings - sum(u.building for u in d)
        ships_lost += lost_ships(defence, d, fifth)
        raiders_lost += lost_ships(raid, a, fifth)
    return {
        "hold": 100 * wins / runs, "retreat": 100 * retreats / runs,
        "buildings_lost": buildings_lost / runs, "buildings": n_buildings,
        "ships_lost": ships_lost / runs, "ships": n_ships,
        "raiders_lost": raiders_lost / runs, "raiders": sum(raid.values()),
    }


def row(label, defence, raid, args, rng):
    r = simulate(parse(defence), parse(raid), args.shield, args.runs, rng,
                 args.retreat, not args.no_fifth)
    print(f"{label:26s} {defence:22s} vs {raid:12s} holds {r['hold']:5.1f}% "
          f"(retreat {r['retreat']:5.1f}%)  "
          f"lost: buildings {r['buildings_lost']:4.1f}/{r['buildings']}  "
          f"ships {r['ships_lost']:4.1f}/{r['ships']}  "
          f"raiders {r['raiders_lost']:4.1f}/{r['raiders']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--defence", help="player side, e.g. twin=10,ion=1,SF=10")
    ap.add_argument("--raid", help="alien side, e.g. H=27,SF=27")
    ap.add_argument("--shield", type=float, default=0.0,
                    help="Energy shield: chance an enemy hit on a building misses (accepted: 0.3)")
    ap.add_argument("--retreat", type=float, default=0.5,
                    help="the raid leaves after losing this share of its combat weight (0: fights to the end)")
    ap.add_argument("--no-fifth", action="store_true",
                    help="count every destroyed ship as lost (the original returns a fifth)")
    ap.add_argument("--runs", type=int, default=200, help="battles per row (default 200)")
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    if args.defence or args.raid:
        if not (args.defence and args.raid):
            ap.error("--defence and --raid go together")
        row("", args.defence, args.raid, args, rng)
        return
    for label, defence, raid in SCENARIOS:
        row(label, defence, raid, args, rng)


if __name__ == "__main__":
    main()
