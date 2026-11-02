#!/usr/bin/env python3
"""Overview of a TOTK_SESSION_LOG=1 file (logs/session.tsv): hours of play, any number of launches.

    python tools/bench/session_report.py logs/session.tsv [--cell 250] [--top 15] [--since 11-02]
    python tools/bench/session_report.py --selftest

Prints, per launch and overall: time played, fps percentiles over the one-second windows, the share of windows under
60 / 55 / 45 fps, what bound them, the spikes (single frames of 25 ms or more) and what they waited on; then the worst
windows with where the camera was and which way it looked, and a map of cells (--cell metres square) by time spent and
worst fps, so a slow area stands out without anyone watching the run. Only windows after the world clock started
(a save is loaded) count; loading screens and the title are left out.
"""
import math
import sys

DIRS = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"]


def heading(yaw):
    return DIRS[int((yaw + 22.5) % 360 // 45)]


def num(v):
    try:
        return float(v)
    except ValueError:
        return None


def parse(lines):
    """Rows as dicts; each carries its launch number. Header lines start with '#' or 'kind'."""
    launches, rows, cols = [], [], None
    for line in lines:
        line = line.rstrip("\r\n")
        if not line:
            continue
        if line.startswith("# launch"):
            launches.append(line[len("# launch "):])
            continue
        f = line.split("\t")
        if f[0] == "kind":
            cols = f
            continue
        if cols is None or len(f) != len(cols):
            continue   # a line cut off by a crash
        r = dict(zip(cols, f))
        r["launch"] = len(launches) - 1
        for k in cols[2:]:
            if k not in ("bound",):
                r[k] = num(r[k])
        rows.append(r)
    return launches, rows


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p / 100 * len(xs)))] if xs else float("nan")


def summary(wins, spikes, label):
    fps = [w["fps"] for w in wins]
    out = [f"{label}: {len(wins) / 60:.0f} min of play in {len(wins)} windows, {len(spikes)} spikes"]
    if not fps:
        return out
    low = sorted(fps)
    out.append("  fps per window: p50 %.1f  p10 %.1f  p1 %.1f  min %.1f" % (
        pct(fps, 50), pct(fps, 10), pct(fps, 1), low[0]))
    under = lambda t: 100.0 * sum(1 for x in fps if x < t) / len(fps)
    out.append("  windows under 59: %.1f %%  under 55: %.1f %%  under 45: %.1f %%" % (under(59), under(55), under(45)))
    gpu = [w["gpu_ms"] for w in wins if w["gpu_ms"]]
    rep = [w["replay_ms"] for w in wins if w["replay_ms"] is not None]
    if gpu:
        out.append("  gpu ms: mean %.2f  p99 %.2f   replay ms: mean %.2f  p99 %.2f" % (
            sum(gpu) / len(gpu), pct(gpu, 99), sum(rep) / max(1, len(rep)), pct(rep, 99)))
    slow = [w for w in wins if w["fps"] < 59]
    if slow:
        b = {}
        for w in slow:   # one frame of 50 ms or more drags a window down by itself: a hitch, not a sustained bound
            k = "hitch" if w["max_ms"] >= 50 else w["bound"]
            b[k] = b.get(k, 0) + 1
        out.append("  slow windows bound by: " + ", ".join(f"{k} {100 * v / len(slow):.0f} %" for k, v in
                                                          sorted(b.items(), key=lambda kv: -kv[1])))
    if spikes:
        def share(k):
            return sum(1 for s in spikes if (s[k] or 0) >= 0.5 * s["max_ms"])
        out.append("  spikes: worst %.0f ms; mostly shader xlate %d, texture %d, pipeline %d, other (CPU/replay) %d" % (
            max(s["max_ms"] for s in spikes), share("worst_xlate"), share("worst_tex"), share("worst_pipe"),
            sum(1 for s in spikes if max(s["worst_xlate"] or 0, s["worst_tex"] or 0, s["worst_pipe"] or 0)
                < 0.5 * s["max_ms"])))
    return out


def where(r):
    if r["x"] is None:
        return "position unknown"
    return "at %.0f %.0f %.0f facing %s (%.0f deg, pitch %.0f)" % (r["x"], r["y"], r["z"], heading(r["yaw"]), r["yaw"],
                                                                    r["pitch"])


def report(launches, rows, cell=250.0, top=15):
    rows = [r for r in rows if r["world_s"] is not None and r["world_s"] > 0]   # -1 us (prints -0.0): no save yet
    wins = [r for r in rows if r["kind"] == "win"]
    spikes = [r for r in rows if r["kind"] == "spike"]
    out = summary(wins, spikes, f"all {len(launches)} launches")
    for i, name in enumerate(launches):
        w = [r for r in wins if r["launch"] == i]
        if w:
            out += summary(w, [r for r in spikes if r["launch"] == i], f"launch {name}")
    out.append("")
    out.append(f"worst {top} windows:")
    for r in sorted(wins, key=lambda r: r["fps"])[:top]:
        out.append("  %s  %5.1f fps  max %5.1f ms  gpu %5.2f  replay %5.2f  %-4s %s" % (
            r["time"], r["fps"], r["max_ms"], r["gpu_ms"] or 0, r["replay_ms"] or 0, r["bound"], where(r)))
    placed = [r for r in wins if r["x"] is not None]
    if placed:
        cells = {}
        for r in placed:
            k = (math.floor(r["x"] / cell), math.floor(r["z"] / cell))
            c = cells.setdefault(k, {"n": 0, "fps": 0.0, "low": 999.0, "y": 0.0, "spikes": 0})
            c["n"] += 1
            c["fps"] += r["fps"]
            c["low"] = min(c["low"], r["fps"])
            c["y"] += r["y"]
        for s in spikes:
            if s["x"] is not None:
                k = (math.floor(s["x"] / cell), math.floor(s["z"] / cell))
                if k in cells:
                    cells[k]["spikes"] += 1
        out.append("")
        out.append(f"areas ({cell:.0f} m cells, x/z of the cell's corner, mean height), slowest first:")
        ranked = sorted(cells.items(), key=lambda kv: kv[1]["fps"] / kv[1]["n"])
        for (cx, cz), c in ranked[:top]:
            out.append("  x %6.0f z %6.0f  y %6.0f  %4.1f min  avg %5.1f fps  worst %5.1f  spikes %d" % (
                cx * cell, cz * cell, c["y"] / c["n"], c["n"] / 60, c["fps"] / c["n"], c["low"], c["spikes"]))
        out.append(f"  ({len(cells)} cells visited, {100 * len(placed) / len(wins):.0f} % of windows had a position)")
    return "\n".join(out)


def selftest():
    hdr = ("kind\ttime\tup_s\tworld_s\tframe\tfps\tavg_ms\tmax_ms\tgpu_ms\treplay_ms\tfence_ms\tbound\t"
           "worst_xlate\tworst_tex\tworst_pipe\tx\ty\tz\tyaw\tpitch")
    lines = ["# launch 2026-11-02 20:00:00 port test", hdr]
    for i in range(120):
        fps = 60.0 if i % 10 else 40.0
        lines.append("win\t11-02 20:00:%02d\t%d\t%d\t%d\t%.1f\t16.6\t30.0\t11.0\t9.0\t1.0\t%s\t0\t0\t0\t%d\t100\t%d\t%d\t0"
                     % (i % 60, i, i + 1, i * 60, fps, "cpu" if fps < 59 else "pace", i * 10, -i * 10, (i * 30) % 360))
    lines.append("spike\t11-02 20:01:00\t60\t60\t3600\t20.0\t50.0\t50.0\t-\t-\t-\t-\t40.0\t0\t0\t300\t100\t-300\t90\t0")
    lines.append("win\t11-02 20:02:00\t121\t121\t7260\t60")   # cut off by a crash
    launches, rows = parse(lines)
    assert len(launches) == 1 and len(rows) == 121, (launches, len(rows))
    text = report(launches, rows)
    assert "2 min of play in 120 windows, 1 spikes" in text, text
    assert "under 59: 10.0 %" in text, text
    assert "shader xlate 1" in text, text
    assert "facing" in text and "cells visited" in text, text
    assert heading(0) == "N" and heading(90) == "E" and heading(350) == "N" and heading(200) == "S"
    print("selftest: passed")


def main(argv):
    if "--selftest" in argv:
        return selftest()
    args = [a for a in argv if not a.startswith("--")]
    opt = lambda k, d: type(d)(argv[argv.index(k) + 1]) if k in argv else d
    cell, top, since = opt("--cell", 250.0), opt("--top", 15), opt("--since", "")
    path = next((a for a in args if a not in (str(cell), str(top), since)), "logs/session.tsv")
    with open(path, encoding="utf-8", errors="replace") as f:
        launches, rows = parse(f)
    if since:
        rows = [r for r in rows if r["time"] >= since]
    print(report(launches, rows, cell, top))


if __name__ == "__main__":
    main(sys.argv[1:])
