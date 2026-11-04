#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Tables from fenestal-bench/1 records.

    python3 tools/bench/report.py [<record or directory>...]
    python3 tools/bench/report.py --csv [...]
    python3 tools/bench/report.py --compare <a> <b>
    python3 tools/bench/report.py --fit [...]
    python3 tools/bench/report.py --html [--compare <a> <b>] [...] > out.html

With no path, the records in the state directory the testbed writes them
to ($XDG_STATE_HOME/fenestal/bench, or ~/.local/state/fenestal/bench).

A row is a series: the repeats of one start, which share `run.series`, each
a window of its own. Its figures are the mean over its complete runs with
the standard deviation beside it, and a series of one run says so -- one
run indicates and several that agree measure, as docs/benchmarks.md has
it. Invalid and aborted runs are counted and left out of the figures.
Markdown by default, in docs/benchmarks.md's shape; --csv a line a run,
every metric a column.

The presented image's size is a column, because it is what lets two rows
be compared: on KMS the window is the output and its area the mode's, a
windowed run draws the bench's own. `MP/s` is the image the path
delivered a second, `GPU ms/MP` and `CPU ms/MP` what one cost, and the
two latency columns are the record's own stamps -- a time, which needs no
normalising, and on KMS the shortest in the set (DESIGN *Benchmarking*).

--compare takes two records, or two series by id, and sets each metric of
the second against the first, with the change in per cent; as Markdown or
--html, never --csv.

--fit reads the records of each place at more than one presented size --
what bench.py --sizes makes, a run a size -- and fits a frame's cost
against the area: the milliseconds it costs at no pixels and the
milliseconds a megapixel adds, GPU and CPU each. That is the figure one
size cannot give: a compositor's per-frame cost is the intercept and a
copy path's readback the slope.

--html writes the same table, or the same comparison, as one HTML page
with nothing outside it -- no script, no stylesheet, no font -- and under
it the charts a record can draw, each an inline SVG: the frame interval's
distribution, the per-interval rate and frame costs over the run, and the
library's per-frame phases. A comparison puts A and B on the same axes.

Every record is checked first (check.py); one that fails is named and
left out.
"""

import argparse
import csv
import html
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check  # noqa: E402

def area_mp(doc):
    """The presented image's area in megapixels.

    On KMS the window is the output and its area the mode's; a windowed
    run draws the bench's own -- so this is what makes two rows
    comparable, and the copy path's cost in particular is its size's."""
    p = doc["platform"]
    w, h = p.get("area_w_px"), p.get("area_h_px")
    return w * h / 1e6 if w and h else None


def size_px(doc):
    p = doc["platform"]
    w, h = p.get("area_w_px"), p.get("area_h_px")
    return "%dx%d" % (w, h) if w and h else None


def mp_s(doc):
    """The image the path delivered a second: the rate, size and all."""
    rate, area = metric(doc, ("frame", "rate_fps"), None), area_mp(doc)
    return rate * area if rate is not None and area else None


def per_mp(doc, path):
    """A frame's cost for one megapixel of what it drew."""
    cost, area = metric(doc, path, "mean"), area_mp(doc)
    return cost / area if cost is not None and area else None


def fit(xs, ys):
    """y = a + b x by least squares, as (a, b); None with fewer than two
    distinct x."""
    pts = [(x, y) for x, y in zip(xs, ys, strict=False)
           if x is not None and y is not None]
    if len({round(x, 9) for x, _ in pts}) < 2:
        return None
    mx = sum(x for x, _ in pts) / len(pts)
    my = sum(y for _, y in pts) / len(pts)
    sxx = sum((x - mx) ** 2 for x, _ in pts)
    sxy = sum((x - mx) * (y - my) for x, y in pts)
    b = sxy / sxx
    return my - b * mx, b


# The options a fit does not make its key from: the size it is fitted over
# and the record's own (its name, where the file went, a note).
FIT_APART = ("width-pt", "height-pt", "mode", "label", "out", "notes")


def full_place(doc):
    """place() with the GPU as its driver names it: NVK's and NVIDIA's own
    share a short name and are two drivers, which a fit must not mix."""
    p = doc["platform"]
    return "%s, %s on %s, %s" % (doc["machine"]["host"] or "?", p["renderer"],
                                 p["backend"], p["gpu"] or p["gpu_short"])


def fits(docs):
    """Each place's records as a line in the area presented.

    Returns the rows -- where, run under, the areas, and the GPU and CPU
    fits -- and the places run at one size only, which no line describes.
    A place is the GPU as its driver names it, which is what tells NVK's
    from NVIDIA's own where place()'s short name does not, and the run's
    options make the key too, so a vsync or a measure of its own is a
    line of its own."""
    groups = {}
    for d in docs:
        if d["run"]["status"] != "complete":
            continue
        opts = tuple(sorted((k, v) for k, v in d["options"].items()
                            if k not in FIT_APART))
        groups.setdefault((full_place(d), opts), []).append(d)
    rows, single = [], []
    for runs in groups.values():
        where = full_place(runs[0])
        areas = [area_mp(d) for d in runs]
        if len({round(a, 9) for a in areas if a is not None}) < 2:
            single.append(where)
            continue
        rows.append((where, conditions(runs[0]),
                     sorted({round(a, 3) for a in areas if a is not None}),
                     fit(areas, [metric(d, ("frame", "gpu_ms"), "mean")
                                 for d in runs]),
                     fit(areas, [metric(d, ("frame", "cpu_ms"), "mean")
                                 for d in runs])))
    return rows, sorted(set(single))


def fits_markdown(docs):
    """The fitted lines as a Markdown table."""
    rows, single = fits(docs)

    def cell(f, i):
        return "-" if f is None else "%.2f" % f[i]

    out = ["| where | run under | sizes MP | GPU fixed ms | GPU ms/MP | "
           "CPU fixed ms | CPU ms/MP |",
           "|---|---|---|---|---|---|---|"]
    for where, under, areas, g, c in rows:
        out.append("| %s | %s | %s | %s | %s | %s | %s |"
                   % (where, under, ", ".join("%.2f" % a for a in areas),
                      cell(g, 0), cell(g, 1), cell(c, 0), cell(c, 1)))
    if single:
        out += ["", "one size, not fitted: " + ", ".join(single)]
    return "\n".join(out)


# The columns of the Markdown table: a title, a getter from a record and a
# format -- None where the column is a fact of the place and not a
# measurement, which takes the first complete run's own value.
#
# The size is in the table because only with it are two rows comparable:
# on KMS the window is the output, and a windowed run draws the bench's
# area, so a path reads differently at 1280x720 and at 1920x1046. MP/s is
# the image it delivered a second and ms/MP what one cost; the latencies
# are the record's own stamps, a time that needs no normalising and the
# figure the window servers lose on (DESIGN *Benchmarking*).
COLUMNS = [
    ("size px", size_px, None),
    ("MP", area_mp, "%.2f"),
    ("fps", lambda d: metric(d, ("frame", "rate_fps"), None), "%.1f"),
    ("MP/s", mp_s, "%.0f"),
    ("p50 ms", lambda d: metric(d, ("frame", "interval_ms"), "p50"), "%.2f"),
    ("p99 ms", lambda d: metric(d, ("frame", "interval_ms"), "p99"), "%.2f"),
    ("1% low", lambda d: metric(d, ("frame", "low_1pct_fps"), None), "%.0f"),
    ("GPU ms", lambda d: metric(d, ("frame", "gpu_ms"), "mean"), "%.2f"),
    ("GPU ms/MP", lambda d: per_mp(d, ("frame", "gpu_ms")), "%.2f"),
    ("CPU ms", lambda d: metric(d, ("frame", "cpu_ms"), "mean"), "%.2f"),
    ("CPU ms/MP", lambda d: per_mp(d, ("frame", "cpu_ms")), "%.2f"),
    ("CPU %", lambda d: metric(d, ("process", "cpu_pct"), None), "%.0f"),
    ("latch→shown", lambda d: metric(d, ("frame", "latch_to_shown_ms"),
                                     "p50"), "%.1f"),
    ("read→shown", lambda d: metric(d, ("input", "read_to_shown_ms"),
                                    "p50"), "%.1f"),
]

# The options a row names when they differ from the default, which is
# what tells two rows of one place apart.
SHOWN = [("vsync", lambda v: None if v else "vsync off"),
         ("redraw", lambda v: None if v == "every-frame" else v),
         ("scene-motion", lambda v: "moving" if v else None),
         ("load-threads", lambda v: "load %d" % v if v else None),
         ("antialias", lambda v: None if v == "default" else v),
         ("late-latch", lambda v: None if v == "auto" else "latch " + v),
         ("fullscreen", lambda v: "fullscreen" if v else None),
         ("load-affinity", lambda v: "load on " + v if v else None),
         ("load-priority", lambda v: "load nice %d" % v if v else None),
         ("process-affinity", lambda v: "program on " + v if v else None),
         ("priority", lambda v: "nice %d" % v if v else None)]


def state_dir():
    xdg = os.environ.get("XDG_STATE_HOME")
    base = xdg or os.path.join(os.path.expanduser("~"), ".local", "state")
    return os.path.join(base, "fenestal", "bench")


def load(paths):
    """Every sound record under `paths`, oldest first; the unsound named."""
    files = []
    for p in paths or [state_dir()]:
        if os.path.isdir(p):
            files += [os.path.join(p, f) for f in sorted(os.listdir(p))
                      if f.endswith(".json")]
        else:
            files.append(p)
    schema = check.load_schema()
    docs = []
    for f in files:
        try:
            with open(f) as fh:
                doc = json.load(fh)
        except (OSError, ValueError) as e:
            print("%s: %s; left out" % (f, e), file=sys.stderr)
            continue
        found = check.problems(doc, schema)
        if found:
            print("%s: %s; left out" % (f, found[0]), file=sys.stderr)
            continue
        doc["_path"] = f
        docs.append(doc)
    docs.sort(key=lambda d: d["run"]["started_utc"])
    return docs


def metric(doc, path, part):
    node = doc["metrics"]
    for key in path:
        node = node.get(key) if isinstance(node, dict) else None
        if node is None:
            return None
    return node.get(part or "value")


def place(doc):
    p, m = doc["platform"], doc["machine"]
    return "%s, %s on %s, %s" % (m["host"] or "?", p["renderer"],
                                 p["backend"], p["gpu_short"] or p["gpu"])


def conditions(doc):
    o = doc["options"]
    said = [f(o[k]) for k, f in SHOWN if k in o]
    said = [s for s in said if s]
    if doc["run"]["label"]:
        said.insert(0, doc["run"]["label"])
    return ", ".join(said) or "defaults"


def mean_sd(values):
    values = [v for v in values if v is not None and math.isfinite(v)]
    if not values:
        return None, None, 0
    m = sum(values) / len(values)
    if len(values) < 2:
        return m, None, 1
    var = sum((v - m) ** 2 for v in values) / (len(values) - 1)
    return m, math.sqrt(var), len(values)


def series(docs):
    """The records as series, in the order each began."""
    out = {}
    for d in docs:
        out.setdefault(d["run"]["series"], []).append(d)
    return list(out.values())


def table(docs):
    """The table's rows, a series each: its runs and its cells as text."""
    out = []
    for runs in series(docs):
        good = [d for d in runs if d["run"]["status"] == "complete"]
        bad = len(runs) - len(good)
        n = "%d" % len(good) + (" (+%d not)" % bad if bad else "")
        if len(good) == 1:
            n += ", indication"
        cells = []
        for _title, get, fmt in COLUMNS:
            if fmt is None:
                # A fact of the place, not a measurement of the run.
                cells.append(next((get(d) for d in good
                                   if get(d) is not None), "-"))
                continue
            m, sd, k = mean_sd([get(d) for d in good])
            if m is None:
                cells.append("-")
            elif sd is None:
                cells.append(fmt % m)
            else:
                cells.append((fmt + " ± " + fmt) % (m, sd))
        first = runs[0]
        out.append((runs, [place(first), conditions(first), n] + cells))
    return out


def markdown(docs):
    lines = ["| where | run under | n | "
             + " | ".join(c[0] for c in COLUMNS) + " |",
             "|---|---|---|" + "---|" * len(COLUMNS)]
    for _runs, cells in table(docs):
        lines.append("| " + " | ".join(cells) + " |")
    return "\n".join(lines)


def flat(doc):
    """One record as one row of named scalars, for --csv."""
    row = {"id": doc["run"]["id"], "series": doc["run"]["series"],
           "started_utc": doc["run"]["started_utc"],
           "status": doc["run"]["status"], "host": doc["machine"]["host"],
           "backend": doc["platform"]["backend"],
           "renderer": doc["platform"]["renderer"],
           "gpu": doc["platform"]["gpu"],
           "present_mode": doc["platform"]["present_mode"]}
    row["area_w_px"] = doc["platform"].get("area_w_px")
    row["area_h_px"] = doc["platform"].get("area_h_px")
    row["area_mp"] = area_mp(doc)
    row["mp_s"] = mp_s(doc)
    row["gpu_ms_per_mp"] = per_mp(doc, ("frame", "gpu_ms"))
    row["cpu_ms_per_mp"] = per_mp(doc, ("frame", "cpu_ms"))
    for k, v in doc["options"].items():
        row["option." + k] = v
    for group, members in doc["metrics"].items():
        for name, v in (members or {}).items():
            if isinstance(v, dict) and "value" in v:
                row["%s.%s" % (group, name)] = v["value"]
            elif isinstance(v, dict):
                for part in ("mean", "p50", "p99", "max"):
                    row["%s.%s.%s" % (group, name, part)] = v[part]
    return row


def write_csv(docs, out):
    rows = [flat(d) for d in docs]
    names = []
    for r in rows:
        names += [k for k in r if k not in names]
    w = csv.DictWriter(out, fieldnames=names)
    w.writeheader()
    for r in rows:
        w.writerow(r)


def pick(docs, key):
    """A record by path or id, or a series by id: its runs."""
    if os.path.exists(key):
        path = os.path.abspath(key)
        found = [d for d in docs if os.path.abspath(d["_path"]) == path]
        return found or load([key])
    return [d for d in docs if key in (d["run"]["id"], d["run"]["series"])]


def pick_two(docs, a, b):
    """The runs of A and of B, or None with the one not found named."""
    ra, rb = pick(docs, a), pick(docs, b)
    if not ra or not rb:
        print("no record or series %r" % (a if not ra else b),
              file=sys.stderr)
        return None
    return ra, rb


def side(name, runs):
    return "%s: %s, %s (%d run%s)" % (name, place(runs[0]),
                                      conditions(runs[0]), len(runs),
                                      "" if len(runs) == 1 else "s")


def compare_rows(ra, rb):
    """Each metric either side has: its name, A, B and the change."""
    rows = []
    names = []
    for d in ra + rb:
        for group, members in d["metrics"].items():
            for name, v in (members or {}).items():
                if v is not None and (group, name) not in names:
                    names.append((group, name))
    for group, name in names:
        part = None
        sample = next((d["metrics"][group][name] for d in ra + rb
                       if (d["metrics"].get(group) or {}).get(name)), None)
        if isinstance(sample, dict) and "value" not in sample:
            part = "mean"
        ma, _s, _n = mean_sd([metric(d, (group, name), part) for d in ra])
        mb, _s, _n = mean_sd([metric(d, (group, name), part) for d in rb])
        if ma is None and mb is None:
            continue
        change = "-"
        if ma and mb is not None:
            change = "%+.1f %%" % (100.0 * (mb - ma) / abs(ma))
        rows.append(("%s.%s%s" % (group, name,
                                  "" if part is None else " (mean)"),
                     "-" if ma is None else "%.4g" % ma,
                     "-" if mb is None else "%.4g" % mb, change))
    return rows


def compare(docs, a, b):
    two = pick_two(docs, a, b)
    if two is None:
        return 1
    ra, rb = two
    print(side("A", ra))
    print(side("B", rb))
    print()
    print("| metric | A | B | change |")
    print("|---|---|---|---|")
    for row in compare_rows(ra, rb):
        print("| %s | %s | %s | %s |" % row)
    return 0


# ---- the page ------------------------------------------------------------

# A chart's frame in the SVG's own units. The picture scales to its column,
# so these fix its proportions and the text's size against the plot.
CW, CH = 360, 214
ML, MR, MT, MB = 48, 12, 12, 38

# The tail axis is -log10(1 - p): each decade of the tail is as wide as
# the body, since the frames that are noticed are in the last per cent and
# a linear percentile axis gives them a hundredth of the width.
TAIL_TICKS = [(0.0, "0"), (math.log10(2), "50"), (1.0, "90"), (2.0, "99"),
              (3.0, "99.9"), (4.0, "99.99")]

# What `series` is drawn as: a column, a title and its unit.
SERIES_CHARTS = [("rate_fps", "Frame rate", "fps"),
                 ("gpu_ms", "GPU time a frame", "ms"),
                 ("cpu_ms", "CPU time a frame", "ms")]

# The library's per-frame phases: a `library_frames` column each, or the
# record's distribution of it where the rows were not kept.
PHASES = [("pace", "Pace"), ("acquire", "Acquire"), ("present", "Present"),
          ("draw", "Draw"), ("gpu", "GPU")]

# The two sides' colours: the first two slots of a palette checked for
# colour-blind separation and contrast on each surface, light and dark.
LIGHT = {"bg": "#f9f9f7", "surface": "#fcfcfb", "ink": "#0b0b0b",
         "ink2": "#52514e", "muted": "#6f6d68", "grid": "#e1e0d9",
         "axis": "#c3c2b7", "border": "rgba(11,11,11,0.10)",
         "c0": "#2a78d6", "c1": "#eb6834"}
DARK = {"bg": "#0d0d0d", "surface": "#1a1a19", "ink": "#f0efec",
        "ink2": "#c3c2b7", "muted": "#9a9890", "grid": "#2c2c2a",
        "axis": "#4a4a46", "border": "rgba(255,255,255,0.10)",
        "c0": "#3987e5", "c1": "#d95926"}

CSS = """
:root { color-scheme: light; %(light)s }
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) { color-scheme: dark; %(dark)s }
}
:root[data-theme="dark"] { color-scheme: dark; %(dark)s }
body { margin: 0 auto; padding: 16px; max-width: 1180px;
       background: var(--bg); color: var(--ink);
       font: 14px/1.45 system-ui, -apple-system, "Segoe UI", sans-serif; }
h1 { font-size: 20px; margin: 4px 0 4px; }
h2 { font-size: 16px; margin: 32px 0 2px; }
h3 { font-size: 14px; margin: 18px 0 8px; color: var(--ink2); }
p { margin: 4px 0; }
.meta, .sub, .none { color: var(--ink2); font-size: 12.5px; }
.scroll { overflow-x: auto; margin: 12px 0; }
table { border-collapse: collapse; font-variant-numeric: tabular-nums;
        font-size: 13px; }
th, td { padding: 4px 8px; border-bottom: 1px solid var(--border);
         text-align: left; vertical-align: top; }
th { color: var(--ink2); font-weight: 600; white-space: nowrap; }
td.num { text-align: right; white-space: nowrap; }
a { color: inherit; }
.charts { display: grid; gap: 12px;
          grid-template-columns: repeat(auto-fill,
                                        minmax(min(100%%, 330px), 1fr)); }
figure { margin: 0; padding: 8px 10px 6px; background: var(--surface);
         border: 1px solid var(--border); border-radius: 8px; }
figcaption { font-weight: 600; }
.legend { display: flex; flex-wrap: wrap; gap: 4px 14px;
          font-size: 12.5px; color: var(--ink2); margin: 2px 0; }
.sw { display: inline-block; width: 14px; height: 3px; border-radius: 2px;
      vertical-align: middle; margin-right: 5px; }
.sw.c0 { background: var(--c0); } .sw.c1 { background: var(--c1); }
svg { display: block; width: 100%%; height: auto; }
svg text { fill: var(--muted); font-size: 10px; font-family: inherit; }
svg .gl { stroke: var(--grid); stroke-width: 1; }
svg .ax { stroke: var(--axis); stroke-width: 1; }
svg .ref { stroke: var(--ink2); stroke-width: 1; stroke-dasharray: 4 3; }
svg .ln { fill: none; stroke-width: 1.6; stroke-linejoin: round;
          stroke-linecap: round; vector-effect: non-scaling-stroke; }
svg .ln.c0 { stroke: var(--c0); } svg .ln.c1 { stroke: var(--c1); }
svg .mk { stroke: var(--surface); stroke-width: 1.5; }
svg .mk.c0 { fill: var(--c0); } svg .mk.c1 { fill: var(--c1); }
"""


def css():
    def tokens(t):
        return " ".join("--%s: %s;" % kv for kv in t.items())
    return CSS % {"light": tokens(LIGHT), "dark": tokens(DARK)}


def esc(text):
    return html.escape(str(text), quote=True)


def known(v):
    """A figure to draw, or None. Missing, not finite, or negative -- the
    sentinel some writers put in an unread time -- is unknown, and
    unknown is left out of a line, never drawn as zero; every column
    drawn here is a time or a rate, which no measure makes negative."""
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return None
    if not math.isfinite(v) or v < 0:
        return None
    return float(v)


def column(table, name):
    """A column of a {columns, rows} table, or None where it has none."""
    if not isinstance(table, dict) or name not in table.get("columns", []):
        return None
    i = table["columns"].index(name)
    return [known(r[i]) if i < len(r) else None for r in table["rows"]]


def tail_frames(values, cap=300):
    """Every frame's figure on the tail axis, thinned evenly along it."""
    v = sorted(x for x in values if x is not None)
    n = len(v)
    if not n:
        return []
    picks = range(n)
    if n > cap:
        top = math.log10(n)
        picks = sorted({min(n - 1, int(n - n / 10 ** (top * k / cap)))
                        for k in range(cap + 1)})
    # Nearest rank: the i-th of n sorted is the (i / n)-th quantile.
    return [(math.log10(n / (n - i)), v[i]) for i in picks]


def tail_summary(d):
    """A record's seven figures on the tail axis, the mean left out as no
    quantile; max at its own rank, which n gives."""
    if not isinstance(d, dict) or "p50" not in d:
        return []
    n = d.get("n") or 1
    at = [(0.0, "min"), (math.log10(2), "p50"), (math.log10(20), "p95"),
          (2.0, "p99"), (max(2.0, math.log10(n)), "max")]
    return [(x, known(d.get(k)), k) for x, k in at
            if known(d.get(k)) is not None]


def thin(points, cap=600):
    """A long line's points cut to `cap`, each bucket's least and greatest
    kept in their order, so a spike survives; a bucket with nothing known
    stays a break."""
    if len(points) <= cap:
        return points
    out = []
    buckets = cap // 2
    for b in range(buckets):
        chunk = points[len(points) * b // buckets:
                       len(points) * (b + 1) // buckets]
        have = [p for p in chunk if p is not None]
        if not have:
            out.append(None)
            continue
        lo = min(have, key=lambda p: p[1])
        hi = max(have, key=lambda p: p[1])
        out += sorted({lo, hi})
    return out


def nice_step(span, want=5):
    raw = span / want
    mag = 10 ** math.floor(math.log10(raw))
    for m in (1, 2, 5, 10):
        if raw <= m * mag * (1 + 1e-9):
            return m * mag
    return 10 * mag


def ticks(hi):
    """Ticks from zero past `hi`, at a step of 1, 2 or 5 a decade."""
    if hi <= 0:
        hi = 1.0
    step = nice_step(hi)
    count = max(1, math.ceil(hi / step - 1e-9))
    return [i * step for i in range(count + 1)], step


def tick_text(v, step):
    places = max(0, -math.floor(math.log10(step))) if step < 1 else 0
    return "%.*f" % (places, v)


def chart_svg(lines, xkind, xcaption, ycaption, refs=()):
    """One chart: its lines on one pair of axes, or None with nothing known.
    `lines` are dicts of `cls`, `tip` and `pts`, (x, y) with None a break,
    or `marks`, (x, y, name) each drawn as a point; `refs` are (y, text)
    lines across the plot."""
    pts = [p for ln in lines for p in ln.get("pts", []) if p is not None]
    pts += [m[:2] for ln in lines for m in ln.get("marks", [])]
    if not pts:
        return None
    xmax = max(p[0] for p in pts)
    if xkind == "tail":
        x1 = max(xmax, 1.0)
        xt = [(x, t) for x, t in TAIL_TICKS if x <= x1 + 1e-9]
    else:
        xs, xstep = ticks(xmax)
        x1 = xs[-1]
        xt = [(x, tick_text(x, xstep)) for x in xs]
    ys, ystep = ticks(max([p[1] for p in pts] + [r[0] for r in refs]))
    y1 = ys[-1]
    pw, ph = CW - ML - MR, CH - MT - MB

    def sx(x):
        return ML + x / x1 * pw

    def sy(y):
        return MT + ph - y / y1 * ph

    out = ['<svg viewBox="0 0 %d %d" role="img" aria-label="%s against %s">'
           % (CW, CH, esc(ycaption), esc(xcaption))]
    for y in ys:
        out.append('<line class="%s" x1="%d" x2="%d" y1="%.1f" y2="%.1f"/>'
                   % ("ax" if y == 0 else "gl", ML, CW - MR, sy(y), sy(y)))
        out.append('<text x="%d" y="%.1f" text-anchor="end" '
                   'dominant-baseline="middle">%s</text>'
                   % (ML - 5, sy(y), tick_text(y, ystep)))
    for x, text in xt:
        out.append('<line class="ax" x1="%.1f" x2="%.1f" y1="%d" y2="%d"/>'
                   % (sx(x), sx(x), MT + ph, MT + ph + 4))
        out.append('<text x="%.1f" y="%d" text-anchor="middle">%s</text>'
                   % (sx(x), MT + ph + 15, esc(text)))
    out.append('<text x="%.1f" y="%d" text-anchor="middle">%s</text>'
               % (ML + pw / 2, CH - 4, esc(xcaption)))
    out.append('<text transform="translate(11 %.1f) rotate(-90)" '
               'text-anchor="middle">%s</text>'
               % (MT + ph / 2, esc(ycaption)))
    for y, text in refs:
        out.append('<line class="ref" x1="%d" x2="%d" y1="%.1f" y2="%.1f">'
                   '<title>%s</title></line>'
                   % (ML, CW - MR, sy(y), sy(y), esc(text)))
        out.append('<text x="%d" y="%.1f" text-anchor="end">%s</text>'
                   % (CW - MR - 2, sy(y) - 4, esc(text)))
    for ln in lines:
        cls, tip = ln["cls"], esc(ln["tip"])
        seg = []
        for p in ln.get("pts", []) + [None]:
            if p is not None:
                seg.append(p)
                continue
            if len(seg) == 1:
                out.append('<circle class="mk %s" cx="%.1f" cy="%.1f" r="3">'
                           '<title>%s</title></circle>'
                           % (cls, sx(seg[0][0]), sy(seg[0][1]), tip))
            elif seg:
                d = "M" + " L".join("%.1f %.1f" % (sx(x), sy(y))
                                    for x, y in seg)
                out.append('<path class="ln %s" d="%s"><title>%s</title>'
                           '</path>' % (cls, d, tip))
            seg = []
        marks = ln.get("marks", [])
        if len(marks) > 1:
            d = "M" + " L".join("%.1f %.1f" % (sx(x), sy(y))
                                for x, y, _k in marks)
            out.append('<path class="ln %s" d="%s"><title>%s</title></path>'
                       % (cls, d, tip))
        for x, y, k in marks:
            out.append('<circle class="mk %s" cx="%.1f" cy="%.1f" r="3.5">'
                       '<title>%s, %s %.3g %s</title></circle>'
                       % (cls, sx(x), sy(y), tip, k, y, esc(ycaption)))
    out.append("</svg>")
    return "\n".join(out)


def run_name(name, doc):
    r = doc["run"]
    return "%srun %d of %d, %s" % (name + ", " if name else "",
                                   r["repeat"] + 1, r["repeats_n"], r["id"])


def figure(title, svg, sides, note=""):
    """A chart with its title, and a legend where there are two sides."""
    out = ["<figure>", "<figcaption>%s</figcaption>" % esc(title)]
    if len(sides) > 1:
        out.append('<div class="legend">' + "".join(
            '<span><span class="sw %s"></span>%s</span>' % (cls, esc(name))
            for name, cls, _runs in sides) + "</div>")
    if note:
        out.append('<p class="sub">%s</p>' % esc(note))
    out.append(svg if svg else
               '<p class="none">unknown in every run drawn</p>')
    out.append("</figure>")
    return "\n".join(out)


def interval_chart(sides):
    lines, how, refs = [], set(), {}
    for name, cls, runs in sides:
        for d in runs:
            each = column(d.get("frames"), "interval_ms")
            if each and any(v is not None for v in each):
                lines.append({"cls": cls, "tip": run_name(name, d),
                              "pts": tail_frames(each)})
                how.add("every frame")
            else:
                lines.append({"cls": cls, "tip": run_name(name, d),
                              "marks": tail_summary(
                                  d["metrics"]["frame"].get("interval_ms"))})
                how.add("the record's percentiles")
            hz = known(d["platform"].get("refresh_hz"))
            if hz:
                period = round(1000.0 / hz, 2)
                refs[period] = "refresh %.2f ms" % period
    note = "a line a run, from %s" % " and ".join(sorted(how))
    svg = chart_svg(lines, "tail", "percentile", "ms",
                    sorted(refs.items()))
    return figure("Frame interval", svg, sides, note)


def frame_time_chart(sides):
    """Each interval in its order, where the frames were kept; None
    where no run kept them."""
    lines = []
    for name, cls, runs in sides:
        for d in runs:
            each = column(d.get("frames"), "interval_ms")
            if each and any(v is not None for v in each):
                pts = [None if v is None else (float(i + 1), v)
                       for i, v in enumerate(each)]
                lines.append({"cls": cls, "tip": run_name(name, d),
                              "pts": thin(pts)})
    if not lines:
        return None
    return figure("Frame interval, frame by frame",
                  chart_svg(lines, "linear", "frame", "ms"), sides,
                  "a line a run")


def series_charts(sides):
    out = []
    for col, title, unit in SERIES_CHARTS:
        lines = []
        for name, cls, runs in sides:
            for d in runs:
                t, v = column(d["series"], "t_s"), column(d["series"], col)
                if t is None or v is None:
                    continue
                pts = [None if a is None or b is None else (a, b)
                       for a, b in zip(t, v, strict=True)]
                lines.append({"cls": cls, "tip": run_name(name, d),
                              "pts": pts})
        out.append(figure(title, chart_svg(lines, "linear", "time, s", unit),
                          sides, "a point an interval, a line a run"))
    return out


def phase_charts(sides):
    """The library's phases, or None where no run has any of them."""
    out, drawn = [], False
    for key, title in PHASES:
        lines = []
        for name, cls, runs in sides:
            for d in runs:
                each = column(d.get("library_frames"), key + "_ms")
                if each and any(v is not None for v in each):
                    lines.append({"cls": cls, "tip": run_name(name, d),
                                  "pts": tail_frames(each)})
                    continue
                lib = d["metrics"].get("library") or {}
                marks = tail_summary(lib.get("frame_%s_ms" % key))
                if marks:
                    lines.append({"cls": cls, "tip": run_name(name, d),
                                  "marks": marks})
        svg = chart_svg(lines, "tail", "percentile", "ms")
        drawn = drawn or svg is not None
        out.append(figure(title, svg, sides))
    return out if drawn else None


def charts(sides):
    """Every chart the runs of `sides` can draw, as HTML."""
    first = [interval_chart(sides)]
    each = frame_time_chart(sides)
    if each:
        first.append(each)
    out = ['<div class="charts">', *first, *series_charts(sides), "</div>"]
    phases = phase_charts(sides)
    out.append("<h3>The library's frame, by phase</h3>")
    if phases:
        out += ['<div class="charts">', *phases, "</div>"]
    else:
        out.append('<p class="none">not in these records</p>')
    return out


def page(title, body):
    return "\n".join([
        "<!doctype html>", '<html lang="en">', "<head>",
        '<meta charset="utf-8">',
        '<meta name="viewport" content="width=device-width, '
        'initial-scale=1">',
        "<title>%s</title>" % esc(title),
        "<style>%s</style>" % css(), "</head>", "<body>", *body,
        "</body>", "</html>", ""])


def html_table(head, rows, first_text):
    """`rows` are (anchor or None, cells); the first `first_text` columns
    are words and the rest figures."""
    out = ['<div class="scroll"><table>', "<thead><tr>"
           + "".join("<th>%s</th>" % esc(h) for h in head) + "</tr></thead>",
           "<tbody>"]
    for anchor, cells in rows:
        tds = []
        for i, c in enumerate(cells):
            text = esc(c)
            if i == 0 and anchor:
                text = '<a href="#%s">%s</a>' % (anchor, text)
            tds.append(("<td>%s</td>" if i < first_text
                        else '<td class="num">%s</td>') % text)
        out.append("<tr>" + "".join(tds) + "</tr>")
    out.append("</tbody></table></div>")
    return out


def span(docs):
    when = sorted(d["run"]["started_utc"] for d in docs)
    return when[0] if when[0] == when[-1] else "%s to %s" % (when[0],
                                                             when[-1])


def report_html(docs):
    rows = table(docs)
    body = ["<h1>fenestal-bench</h1>",
            '<p class="meta">%d record%s in %d series, started %s. A row '
            "is a series: the mean of its complete runs, ± their standard "
            "deviation; one run is an indication.</p>"
            % (len(docs), "" if len(docs) == 1 else "s", len(rows),
               esc(span(docs)))]
    head = ["where", "run under", "n"] + [c[0] for c in COLUMNS]
    body += html_table(head, [("s-" + runs[0]["run"]["series"], cells)
                              for runs, cells in rows], 4)
    for runs, cells in rows:
        good = [d for d in runs if d["run"]["status"] == "complete"]
        body.append('<section id="s-%s">' % esc(runs[0]["run"]["series"]))
        body.append("<h2>%s</h2>" % esc(cells[0]))
        body.append('<p class="sub">%s; n %s; series %s</p>'
                    % (esc(cells[1]), esc(cells[2]),
                       esc(runs[0]["run"]["series"])))
        for d in runs:
            if d["run"]["status"] != "complete":
                body.append('<p class="sub">%s, %s, left out: %s</p>' % (
                    esc(run_name("", d)), esc(d["run"]["status"]),
                    esc("; ".join(d["run"]["invalid_reasons"]) or "-")))
        if good:
            body += charts([("", "c0", good)])
        else:
            body.append('<p class="none">no complete run to draw</p>')
        body.append("</section>")
    return page("fenestal-bench", body)


def compare_html(ra, rb):
    body = ["<h1>fenestal-bench, A against B</h1>",
            '<p class="meta"><span class="sw c0"></span>%s</p>'
            % esc(side("A", ra)),
            '<p class="meta"><span class="sw c1"></span>%s</p>'
            % esc(side("B", rb))]
    body += html_table(["metric", "A", "B", "change"],
                       [(None, list(r)) for r in compare_rows(ra, rb)], 1)
    body += charts([("A", "c0", ra), ("B", "c1", rb)])
    return page("fenestal-bench comparison", body)


def main(argv):
    ap = argparse.ArgumentParser(
        description="Tables from fenestal-bench/1 records")
    ap.add_argument("paths", nargs="*",
                    help="records or directories; default the state "
                         "directory's")
    form = ap.add_mutually_exclusive_group()
    form.add_argument("--csv", action="store_true",
                      help="a line a run, every metric a column")
    ap.add_argument("--compare", nargs=2, metavar=("A", "B"),
                    help="two records, or two series by id")
    form.add_argument("--html", action="store_true",
                      help="the table or the comparison as one HTML page, "
                           "with its charts")
    form.add_argument("--fit", action="store_true",
                      help="fit each place's records over the sizes they "
                           "were run at: a frame's fixed cost and its cost "
                           "a megapixel")
    args = ap.parse_args(argv)
    # Refused rather than one of them dropped: a comparison is a table of
    # two series' figures, and --csv's line a run has no column for it.
    if args.compare and args.csv:
        ap.error("--csv and --compare do not combine: --csv is a line a "
                 "run, and --compare one table of two")
    docs = load(args.paths)
    if args.compare and args.html:
        two = pick_two(docs, *args.compare)
        if two is None:
            return 1
        sys.stdout.write(compare_html(*two))
        return 0
    if args.compare:
        return compare(docs, *args.compare)
    if not docs:
        print("no records", file=sys.stderr)
        return 1
    if args.fit:
        print(fits_markdown(docs))
        return 0
    if args.html:
        sys.stdout.write(report_html(docs))
    elif args.csv:
        write_csv(docs, sys.stdout)
    else:
        print(markdown(docs))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
