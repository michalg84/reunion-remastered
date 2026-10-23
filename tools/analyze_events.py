#!/usr/bin/env python3
"""Reads the game's event logs (logs/events-*.jsonl, src/game/evlog.h) and
writes a report with charts: how strong the player and each race were day by
day, every attack and battle with the forces on both sides, the story's
timeline and who took which body.

    python tools/analyze_events.py [LOGS_OR_FILES ...] [--out DIR]

Without arguments it reads every events-*.jsonl in the player's logs folder
(Windows: %APPDATA%/Reunion/Reunion/logs). The report goes to DIR
(default: analysis/ next to the first log): report.md, and with matplotlib
installed strength.png, economy.png and attacks.png.
"""
import argparse
import json
import os
import sys
from collections import defaultdict
from pathlib import Path

SYSTEMS = ["Amnesty", "Phoenix", "Mirach", "Antares", "Orionis", "Lyrae", "Rigel", "Sun"]
STATUS = {-1: "eliminated", 0: "not met", 2: "hostile", 4: "neutral", 6: "allied"}
MISSIONS = {1: "space", 2: "ground", 3: "space (strong)", 4: "ground (strong)"}

# Chart colours: categorical slots in fixed order (the player is always slot
# 1, races follow by id), the status pair for won / lost, ink for text.
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
OTHER = "#8a8984"
GOOD, CRITICAL, NEUTRAL = "#0ca30c", "#d03b3b", "#8a8984"
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"


def default_logs():
    base = os.environ.get("APPDATA")
    if base:
        return Path(base) / "Reunion" / "Reunion" / "logs"
    home = Path.home()
    mac = home / "Library/Application Support/Reunion/Reunion/logs"
    return mac if mac.exists() else home / ".local/share/Reunion/Reunion/logs"


def game_day(date):
    """'2263-05-12 14' -> days (fractional) in the game's 30-day months."""
    ymd, hour = date.split(" ")
    y, m, d = (int(x) for x in ymd.split("-"))
    return ((y * 12 + m - 1) * 30 + d - 1) + int(hour) / 24


def load(paths):
    files = []
    for p in paths:
        p = Path(p)
        files += sorted(p.glob("events-*.jsonl")) if p.is_dir() else [p]
    events = []
    for f in files:
        for n, line in enumerate(f.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if not line.strip():
                continue
            try:
                e = json.loads(line)
            except json.JSONDecodeError:
                print(f"{f.name}:{n}: not JSON, skipped", file=sys.stderr)
                continue
            e["_file"] = f.name
            e["_day"] = game_day(e["date"])
            events.append(e)
    return files, events


def total(units):
    return sum(units or [])


def site(e):
    s = e.get("system", 0)
    name = SYSTEMS[s - 1] if 1 <= s <= len(SYSTEMS) else f"system {s}"
    moon = f", moon {e['moon']}" if e.get("moon") else ""
    return f"{name} planet {e.get('planet', '?')}{moon}"


def race_strength(r):
    """A race's mobile forces: what can attack (garrisons stay home)."""
    return total(r.get("units"))


# --- The report ------------------------------------------------------------------

def report(files, events, charts):
    out = ["# Reunion: event log analysis", ""]
    out.append(f"Logs: {', '.join(f.name for f in files)} - {len(events)} events.")
    days = [e for e in events if e["type"] == "day"]
    if days:
        out.append(f"Game time: {days[0]['date']} .. {days[-1]['date']} ({len(days)} days logged).")
    out.append("")
    for c in charts:
        out.append(f"![{c}]({c})")
    out.append("")

    attacks = [e for e in events if e["type"] == "attack"]
    out += ["## Attacks", "",
            "| Date | Race | Where | Mission | Their ships / ground | Your ships / ground there | Battle |",
            "|---|---|---|---|---|---|---|"]
    for a in attacks:
        u = a["units"]
        out.append(f"| {a['date']} | {a['name']} | {site(a)} | {MISSIONS.get(a['mission'], a['mission'])} "
                   f"| {total(u[:4])} / {total(u[4:])} | {total(a['player_ships'])} / {total(a['player_ground'])} "
                   f"| {'yes' if a['battle'] else 'no (undefended)'} |")
    out.append("")

    battles = [e for e in events if e["type"].endswith("_battle_end")]
    out += ["## Battles", "",
            "| Date | Kind | Race | Where | You (start / lost) | Enemy (start / lost) | Result |",
            "|---|---|---|---|---|---|---|"]
    for b in battles:
        if b["type"] == "space_battle_end":
            kind, you, them = "space", b["player_ships_start"], b["enemy_ships_start"]
        else:
            kind, you, them = "ground", total(b["player_units"]), total(b["enemy_units"])
        result = "won" if b["player_won"] else ("retreated" if b["retreated"] else "lost")
        out.append(f"| {b['date']} | {kind} | {b['name'] or '-'} | {site(b)} | {you} / {total(b['player_losses'])} "
                   f"| {them} / {total(b['enemy_losses'])} | {result} |")
    won = sum(1 for b in battles if b["player_won"])
    if days:
        out += ["", "## Races at the end", "", "| Race | Status | Bodies | Forces (travelling) | Mobile units | Garrisons |",
                "|---|---|---|---|---|---|"]
        for r in days[-1]["races"]:
            out.append(f"| {r['name']} | {STATUS.get(r['status'], r['status'])} | {r['bodies']} | {r['forces']} ({r['travelling']}) "
                       f"| {total(r['units'])} | {total(r['garrison'])} |")
    out += ["", f"Won {won} of {len(battles)} battles. Attacks without a battle: "
            f"{sum(1 for a in attacks if not a['battle'])}.", ""]

    out += ["## Timeline", ""]
    for e in events:
        t, line = e["type"], None
        if t == "story":
            line = f"story **{e['name']}** {e['what']}" + (f" ({e['timer']} h)" if e["what"] == "armed" else "")
        elif t == "race_status":
            line = f"{e['name']}: {STATUS.get(e['from'], e['from'])} -> {STATUS.get(e['to'], e['to'])}"
        elif t == "body_owner":
            who = lambda o: "nobody" if o == 0 else "you" if o == 1 else f"race {o}"
            line = f"{e['name']} ({SYSTEMS[e['system'] - 1]}): {who(e['from'])} -> {who(e['to'])}"
        elif t == "force_dispatch":
            line = (f"{e['name']} force {e['force']} sets off for {site(e)}, {e['travel']} h, "
                    f"{total(e['units'][:4])} ships / {total(e['units'][4:])} ground")
        elif t == "research_done":
            line = f"invention {e['invention']} done: {e['name']}"
        elif t in ("session", "time_jump"):
            line = f"_{t}_ ({e['_file']})"
        if line:
            out.append(f"- {e['date']}: {line}")
    return "\n".join(out) + "\n"


# --- Charts -----------------------------------------------------------------------

def style(ax, title, ylabel):
    ax.set_facecolor(SURFACE)
    ax.set_title(title, loc="left", color=INK, fontsize=11)
    ax.set_ylabel(ylabel, color=INK2)
    ax.set_xlabel("game day", color=INK2)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.tick_params(colors=INK2)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)


def charts(events, out_dir, plt):
    days = [e for e in events if e["type"] == "day"]
    if not days:
        return []
    start = days[0]["_day"]
    x = [d["_day"] - start for d in days]
    made = []

    # Strength: the player and the strongest races (fixed colours by race id;
    # beyond seven they fold into "other races").
    peak = defaultdict(int)
    names = {}
    for d in days:
        for r in d["races"]:
            peak[r["race"]] = max(peak[r["race"]], race_strength(r))
            names[r["race"]] = r["name"]
    shown = sorted((r for r in peak if peak[r] > 0), key=lambda r: -peak[r])[:7]
    colour = {r: SERIES[1 + i] for i, r in enumerate(sorted(shown))}
    fig, ax = plt.subplots(figsize=(10, 5), facecolor=SURFACE)
    ax.plot(x, [total(d["ships"]) + total(d["ground"]) for d in days], color=SERIES[0], lw=2, label="You")
    for r in sorted(shown):
        ax.plot(x, [next((race_strength(q) for q in d["races"] if q["race"] == r), 0) for d in days],
                color=colour[r], lw=2, label=names[r])
    rest = [r for r in peak if r not in shown and peak[r] > 0]
    if rest:
        ax.plot(x, [sum(race_strength(q) for q in d["races"] if q["race"] in rest) for d in days],
                color=OTHER, lw=2, ls="--", label="Other races")
    style(ax, "Units: yours and the races' mobile forces (garrisons left out)", "units")
    ax.legend(frameon=False, ncol=4, loc="upper center", bbox_to_anchor=(0.5, -0.15), labelcolor=INK)
    fig.tight_layout()
    fig.savefig(out_dir / "strength.png", dpi=120)
    plt.close(fig)
    made.append("strength.png")

    # Economy: small multiples, one measure each (no second y axis).
    fig, axes = plt.subplots(1, 3, figsize=(12, 3.6), facecolor=SURFACE)
    for ax, key, title in zip(axes, ("credits", "colonies", "inventions"),
                              ("Credits", "Colonies", "Inventions done")):
        ax.plot(x, [d[key] for d in days], color=SERIES[0], lw=2)
        style(ax, title, "")
    fig.tight_layout()
    fig.savefig(out_dir / "economy.png", dpi=120)
    plt.close(fig)
    made.append("economy.png")

    # Battles: enemy size against yours, won / lost by colour and marker.
    battles = [e for e in events if e["type"].endswith("_battle_end")]
    if battles:
        fig, ax = plt.subplots(figsize=(10, 4.5), facecolor=SURFACE)
        for won, col, mark, label in ((True, GOOD, "o", "won"), (False, CRITICAL, "X", "lost / retreated")):
            pts = [b for b in battles if bool(b["player_won"]) == won]
            if not pts:
                continue
            xs = [b["_day"] - start for b in pts]
            ys = [(b["enemy_ships_start"] if b["type"] == "space_battle_end" else total(b["enemy_units"]))
                  - (b["player_ships_start"] if b["type"] == "space_battle_end" else total(b["player_units"]))
                  for b in pts]
            ax.scatter(xs, ys, s=64, color=col, marker=mark, edgecolors=SURFACE, linewidths=2, label=label, zorder=3)
        ax.axhline(0, color=NEUTRAL, lw=1)
        style(ax, "Battles: enemy units minus yours at the start (above 0: outnumbered)", "units")
        ax.legend(frameon=False, loc="upper left", labelcolor=INK)
        fig.tight_layout()
        fig.savefig(out_dir / "attacks.png", dpi=120)
        plt.close(fig)
        made.append("attacks.png")
    return made


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="*", help="events-*.jsonl files or folders holding them")
    ap.add_argument("--out", type=Path, help="report folder (default: analysis/ beside the logs)")
    args = ap.parse_args()
    paths = args.paths or [default_logs()]
    files, events = load(paths)
    if not events:
        sys.exit(f"No events in {', '.join(map(str, paths))} (event_log = on in dev.ini?)")
    out_dir = args.out or files[0].parent / "analysis"
    out_dir.mkdir(parents=True, exist_ok=True)
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        made = charts(events, out_dir, plt)
    except ImportError:
        print("matplotlib not installed: report without charts (pip install matplotlib)", file=sys.stderr)
        made = []
    (out_dir / "report.md").write_text(report(files, events, made), encoding="utf-8")
    print(f"Report: {out_dir / 'report.md'}")


if __name__ == "__main__":
    main()
