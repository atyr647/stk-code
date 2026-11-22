#!/usr/bin/env python3
"""Check that downscaled tracks still play like the originals.

For every race track, run the same headless AI race (STK's profile mode:
fixed 1/60 s time step, 4 karts, 1 lap) on the original assets and on the
output of stk_n64_assets.py, with a desktop SERVER_ONLY build, and compare:

  * finished   -- every kart crossed the line before the time limit
  * rescues    -- karts reset after falling off / through the track
  * lap time   -- mean finish time over all karts

Decimation that opens a hole, drops a wall or roughens the road shows up as
rescues, as a race that never finishes, or as karts losing speed. STK races
are not bit-reproducible (worker threads), so each side is run `--runs`
times and means are compared against a tolerance.

  n64/tools/validate_downscale.py --stk build-x86/bin/supertuxkart \\
      --original /path/root-original --downscaled /path/root-downscaled \\
      --report n64/validate_report.json

A "root" is a directory containing data/ (what SUPERTUXKART_DATADIR wants).
"""
import argparse
import concurrent.futures
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ANSI = re.compile(r"\x1b\[[0-9;]*m")
ROW = re.compile(r"profile:\s+(\S+)\s+(\S+)\s+(\d+)\s+(\d+)\s+(\S+)\s+(\S+)\s+"
                 r"(\S+)\s+(\S+)\s+(\S+)\s+(\d+)")


def race(stk, root, track, timeout):
    """One headless race; returns list of (finish_time, rescues) per kart,
    or None if it did not finish in time."""
    with tempfile.TemporaryDirectory() as home:
        env = dict(os.environ, SUPERTUXKART_DATADIR=root, HOME=home,
                   XDG_CONFIG_HOME=home + "/.config",
                   XDG_DATA_HOME=home + "/.local/share",
                   XDG_CACHE_HOME=home + "/.cache")
        try:
            out = subprocess.run(
                [stk, "--no-graphics", "--track=" + track, "--numkarts=4",
                 "--kart=tux", "--profile-laps=1", "--unlock-all"],
                env=env, capture_output=True, text=True, timeout=timeout,
                errors="replace").stdout
        except subprocess.TimeoutExpired:
            return None
    karts = []
    for line in ANSI.sub("", out).splitlines():
        # per-kart line: ident controller start pos finish avsp top skid
        #                rescue_time rescue_count ...
        m = ROW.search(line)
        if m and m.group(2) == "Skidding" and m.group(1) != "Skidding":
            karts.append((float(m.group(5)), int(m.group(10))))
    return karts or None


def summarise(results):
    ok = [r for r in results if r]
    if not ok:
        return {"finished": False}
    times = [t for r in ok for t, _ in r]
    rescues = [sum(c for _, c in r) for r in ok]
    return {"finished": len(ok) == len(results),
            "mean_time": round(statistics.mean(times), 2),
            "rescues_per_race": round(statistics.mean(rescues), 2)}


def race_tracks(root):
    tracks = []
    tdir = os.path.join(root, "data", "tracks")
    for t in sorted(os.listdir(tdir)):
        try:
            x = ET.parse(os.path.join(tdir, t, "track.xml")).getroot()
        except (ET.ParseError, FileNotFoundError):
            continue
        a = x.attrib
        if any(a.get(k) == "Y" for k in ("arena", "soccer", "cutscene",
                                          "internal")):
            continue
        tracks.append(t)
    return tracks


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--stk", required=True)
    ap.add_argument("--original", required=True)
    ap.add_argument("--downscaled", required=True)
    ap.add_argument("--tracks", help="comma list (default: all race tracks)")
    ap.add_argument("--runs", type=int, default=2)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--time-tolerance", type=float, default=0.10,
                    help="max relative change in mean finish time")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--report")
    a = ap.parse_args()

    tracks = a.tracks.split(",") if a.tracks else race_tracks(a.original)
    jobs = [(t, side, root, i) for t in tracks
            for side, root in (("original", a.original),
                               ("downscaled", a.downscaled))
            for i in range(a.runs)]
    raw = {}
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(race, a.stk, root, t, a.timeout): (t, side)
                for t, side, root, _ in jobs}
        for f in concurrent.futures.as_completed(futs):
            raw.setdefault(futs[f], []).append(f.result())

    report, failures = {}, []
    print("%-22s %-28s %-28s %s" % ("track", "original", "downscaled", "verdict"))
    for t in tracks:
        o = summarise(raw[(t, "original")])
        d = summarise(raw[(t, "downscaled")])
        verdict = "ok"
        if o.get("finished") and not d.get("finished"):
            verdict = "FAIL: does not finish"
        elif d.get("finished") and o.get("finished"):
            dt = (d["mean_time"] - o["mean_time"]) / o["mean_time"]
            if d["rescues_per_race"] > o["rescues_per_race"] + 1:
                verdict = "FAIL: rescues %.1f -> %.1f" % (
                    o["rescues_per_race"], d["rescues_per_race"])
            elif abs(dt) > a.time_tolerance:
                verdict = "FAIL: lap time %+.0f%%" % (100 * dt)
            else:
                verdict = "ok (lap time %+.1f%%)" % (100 * dt)
        elif not o.get("finished"):
            verdict = "n/a: original does not finish headless"
        fmt = lambda s: ("%.1fs, %.1f resc" % (s["mean_time"], s["rescues_per_race"])
                         if s.get("finished") else "did not finish")
        print("%-22s %-28s %-28s %s" % (t, fmt(o), fmt(d), verdict))
        report[t] = {"original": o, "downscaled": d, "verdict": verdict}
        if verdict.startswith("FAIL"):
            failures.append(t)
    if a.report:
        with open(a.report, "w") as f:
            json.dump(report, f, indent=1, sort_keys=True)
    print("\n%d/%d race tracks pass" % (len(tracks) - len(failures), len(tracks)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
