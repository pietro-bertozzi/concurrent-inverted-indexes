#!/usr/bin/env python3
"""Figures for the report: one script, reads the committed results, writes PNG and SVG.

    analysis/.venv/bin/python analysis/make_figures.py                  # regenerate all figures
    analysis/.venv/bin/python analysis/make_figures.py --rebuild-latency-table
                                         # first recompute analysis/data/q5-latency-percentiles.tsv
                                         # from results/q5/*/latencies/ (needs the local raw files)

Input : results/{q1,q2,q2b,q3,q4a,q4b,q5}/<timestamp>/results.tsv (never results/full) and
        analysis/data/q5-latency-percentiles.tsv.
Output: analysis/figures/<name>.png and <name>.svg.

Only the measured repetitions (warmup == 0) are used, and every number plotted is the median of
those repetitions. Speedup = median time of the language's `sequential` / median time of the point.
"""
import argparse
import glob
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parent.parent
FIG_DIR = ROOT / "analysis" / "figures"
SLIDES = False          # --slides: 16:9, no title, large fonts, thick lines, written to figures/slides/
DROP_MARKERS = False    # slides: the dense q1 figure is drawn without markers
LAT_TABLE = ROOT / "analysis" / "data" / "q5-latency-percentiles.tsv"

LANGS = ["java", "go", "cpp", "c"]
LANG_NAME = {"java": "Java", "go": "Go", "cpp": "C++", "c": "C"}
WORKERS = [1, 2, 4, 6, 8, 12, 16, 22]
HW_LINES = [(6, "6 P-cores"), (12, "12 P-threads"), (16, "16 cores")]

# One color per variant (configuration), the same in every figure. Hues in the order of the
# reference palette; sequential is neutral gray. Language is never a color: it is a panel.
COLOR = {
    "sequential": "#6b6a66",
    "global-lock": "#e34948",
    "partition-lock": "#2a78d6",
    "partition-writers": "#eb6834",
    "partition-rwlock": "#008300",
    "private-merge": "#1baf7a",
    "private-merge-heap": "#6250d6",
    "private-merge-static": "#e87ba4",
}
INK, INK2, GRID = "#0b0b0b", "#52514e", "#e3e2de"

plt.rcParams.update({
    "font.size": 9, "axes.titlesize": 10, "axes.titlepad": 14, "axes.labelsize": 9, "legend.fontsize": 8,
    "axes.edgecolor": "#a8a7a2", "axes.labelcolor": INK2, "xtick.color": INK2, "ytick.color": INK2,
    "text.color": INK, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6,
    "axes.axisbelow": True, "axes.spines.top": False, "axes.spines.right": False,
    "figure.facecolor": "white", "axes.facecolor": "white", "savefig.facecolor": "white",
    "lines.linewidth": 1.8, "lines.markersize": 5, "svg.fonttype": "none", "svg.hashsalt": "concurrent-inverted-indexes",
})


def msize(x):
    """Marker size: larger in slides, none where DROP_MARKERS."""
    return 0 if DROP_MARKERS else (x * 2.2 if SLIDES else x)


def lwid(x):
    """Line width of an explicit line: thicker in slides."""
    return x * 2 if SLIDES else x


def fsize(x):
    """Font size of an explicit text: about twice as large in slides."""
    return x * 2 if SLIDES else x


def mk(marker):
    """A marker, or none in the figures drawn without markers."""
    return None if DROP_MARKERS else marker


# Slides: the same figures at 12 x 6.75 in, text about twice as large. Applied by main() on --slides.
SLIDE_RC = {
    "font.size": 17, "axes.titlesize": 20, "axes.titlepad": 8, "axes.labelsize": 17, "legend.fontsize": 16,
    "xtick.labelsize": 16, "ytick.labelsize": 16, "lines.linewidth": 3.6, "lines.markersize": 10,
    "axes.linewidth": 1.4, "grid.linewidth": 1.0, "xtick.major.width": 1.4, "ytick.major.width": 1.4,
    "xtick.major.size": 5, "ytick.major.size": 5,
}
# Slide axis labels: short, the headline of the slide gives the rest.
SLIDE_YLABEL = {
    "speedup vs sequential (×)": "speedup (×)",
    "speedup vs sequential, build + merge (×)": "speedup (×)",
    "time, median (s)": "time (s)",
    "peak memory, first measured rep (MiB)": "peak memory (MiB)",
    "time lock / time messages (>1: messages win)": "time ratio",
    "kernel share: sys / (user + sys) (%)": "kernel share (%)",
    "build time, median (s)": "build time (s)",
    "build + merge, median (s)": "build + merge (s)",
    "throughput, median (thousand ops/s)": "throughput (k ops/s)",
    "p99 query latency, median of reps (µs)": "p99 latency (µs)",
}


# ----------------------------------------------------------------------------- loading

def load(experiment):
    """Read results/<experiment>/<timestamp>/results.tsv, keep the measured repetitions."""
    files = sorted(glob.glob(str(ROOT / "results" / experiment / "*" / "results.tsv")))
    if len(files) != 1:
        sys.exit(f"expected one results.tsv for {experiment}, found {len(files)}")
    d = pd.read_csv(files[0], sep="\t", dtype={"run": str, "partitions": str, "batch": str, "queue": str})
    d = d[(d.warmup == 0) & d.scaling.str.contains("strong")].copy()   # design.md: a "contains" test, 1-worker runs are "strong,weak"
    d["total_ns"] = d.build_ns + d.finalize_ns          # build + merge
    d["cpu_ns"] = d.cpu_user_ns + d.cpu_sys_ns
    return d


def med(d, value, by):
    """Median of `value` per group: a DataFrame indexed by `by`."""
    return d.groupby(by)[value].median()


def curve(d, value, **where):
    """Median of `value` per worker count for the rows matching `where`: a Series by threads."""
    for k, v in where.items():
        d = d[d[k] == v]
    return d.groupby("threads")[value].median()


def speedup(d, lang, value="build_ns", **where):
    """Speedup curve of the matching variant against the language's sequential (same file)."""
    base = d[(d.language == lang) & (d.variant == "sequential")][value].median()
    return base / curve(d[d.language == lang], value, **where)


# ----------------------------------------------------------------------------- drawing

def hw_lines(ax, label=False, compact=False):
    """Vertical dashed lines at the hardware boundaries (numeric worker axis)."""
    for x, text in HW_LINES:
        ax.axvline(x, color="#a8a7a2", ls=(0, (4, 3)), lw=lwid(0.9), zorder=0.5)
        if label:
            hw_label(ax, x, str(x) if compact else text)


def hw_label(ax, x, text, xpos=None):
    """Small label above a boundary line; the 12 and 16 labels lean apart so they never touch."""
    if SLIDES:          # slides: the dashed lines are explained in the legend instead
        return
    ha = {"12 P-threads": "right", "16 cores": "left"}.get(text, "center")
    ax.text(x if xpos is None else xpos, 1.01, text, transform=ax.get_xaxis_transform(), ha=ha,
            va="bottom", fontsize=fsize(6.5), color=INK2)


def worker_axis(ax):
    ax.set_xticks(WORKERS)
    ax.set_xlim(0.5, 23)
    ax.set_xlabel("workers (threads)")


def slide_labels(fig):
    """Slides: short y labels, and a label that every panel of a row or column shares is written once."""
    axes = fig.axes
    first = axes[0].get_ylabel()
    same_y = all(SLIDE_YLABEL.get(a.get_ylabel(), a.get_ylabel()) == SLIDE_YLABEL.get(first, first) for a in axes)
    for a in axes:
        a.set_ylabel(SLIDE_YLABEL.get(a.get_ylabel(), a.get_ylabel()))
        spec = a.get_subplotspec()
        if same_y and spec.colspan.start > 0:
            a.set_ylabel("")
        if spec.rowspan.stop < axes[0].get_subplotspec().get_gridspec().nrows:
            a.set_xlabel("")


def save(fig, name):
    FIG_DIR.mkdir(parents=True, exist_ok=True)
    if SLIDES:
        slide_labels(fig)
    for ext in ("png", "svg"):
        # fixed hash salt and no date: the same data gives byte-identical files (clean git diffs)
        meta = {"Date": None} if ext == "svg" else {"Software": None}
        if SLIDES:      # exactly 12 x 6.75 in: no tight bounding box
            fig.savefig(FIG_DIR / f"{name}.{ext}", dpi=200, metadata=meta)
        else:
            fig.savefig(FIG_DIR / f"{name}.{ext}", dpi=160, bbox_inches="tight", metadata=meta)
    plt.close(fig)
    print("wrote", name)


def wrap_title(fig, t):
    """Break the title into lines no wider than the panels, so it never widens the figure.
    The lines are then balanced: the narrowest limit that keeps the same number of lines."""
    r = fig.canvas.get_renderer()
    boxes = [ax.get_tightbbox(r) for ax in fig.axes]
    limit = max(b.x1 for b in boxes) - min(b.x0 for b in boxes)
    original = t.get_text()

    def width(line):
        t.set_text(line)
        return t.get_window_extent(r).width

    def wrap(max_width):
        lines = []
        for paragraph in original.split("\n"):
            current = ""
            for word in paragraph.split():
                trial = (current + " " + word).strip()
                if current and width(trial) > max_width:
                    lines.append(current)
                    current = word
                else:
                    current = trial
            lines.append(current)
        return lines

    lines = wrap(limit)
    lo, hi = limit / 4, limit
    for _ in range(12):                       # bisection on the line width
        mid = (lo + hi) / 2
        if len(wrap(mid)) <= len(lines):
            hi = mid
        else:
            lo = mid
    lines = wrap(hi)
    t.set_text("\n".join(lines))
    return len(lines)


def title(fig, text, sub=None):
    if SLIDES:          # the slide headline carries the title
        return
    if len(fig.axes) <= 2:
        fig.subplots_adjust(top=0.8)
    t = fig.suptitle(text, x=0.01, ha="left", fontsize=12, fontweight="bold", y=1.0)
    if wrap_title(fig, t) > 1:               # room for a two-line title above the panels
        fig.subplots_adjust(top=0.72 if len(fig.axes) <= 2 else 0.84)
    if sub:
        fig.text(0.01, 0.0, sub, ha="left", va="top", fontsize=7.5, color=INK2)


def panels(n, ncols, w=3.6, h=3.0, **kw):
    nrows = -(-n // ncols)
    if SLIDES:          # 16:9 whatever the number of panels; two columns at most
        ncols = min(ncols, 2)
        nrows = -(-n // ncols)
        fig, axes = plt.subplots(nrows, ncols, figsize=(12, 6.75), squeeze=False, **kw)
        fig.subplots_adjust(left=0.105, right=0.99, top=0.93, bottom=0.30 if nrows == 2 else 0.33,
                            hspace=0.62 if nrows == 2 else 0.3, wspace=0.26)
        return fig, axes.ravel()
    fig, axes = plt.subplots(nrows, ncols, figsize=(w * ncols, h * nrows), squeeze=False,
                             gridspec_kw={"hspace": 0.55, "wspace": 0.28}, **kw)
    return fig, axes.ravel()


def legend_below(fig, handles, ncol, y=-0.01):
    if SLIDES:          # outside the data, under the panels
        # the grey vertical lines get one legend entry (the word "dashed" is taken by the data lines)
        handles = [h for h in handles if not h.get_label().startswith("dashed:")]
        handles.append(line("#a8a7a2", (0, (4, 3)), label="vertical lines: 6 P-cores, 12 P-threads, 16 cores"))
        columns = 3
        for size in (16, 15, 14, 13, 12):      # the largest text that fits the width of the slide
            leg = fig.legend(handles=handles, loc="lower center", bbox_to_anchor=(0.5, 0.0), ncol=columns,
                             frameon=False, handlelength=2.0, columnspacing=1.0, labelspacing=0.25, fontsize=size)
            box = leg.get_window_extent(fig.canvas.get_renderer())
            if box.x0 >= 0 and box.x1 <= fig.bbox.width:
                break
            leg.remove()
        fig.subplots_adjust(bottom=0.135 + 0.062 * (-(-len(handles) // columns)))    # room for the legend rows
        return
    fig.legend(handles=handles, loc="upper center", bbox_to_anchor=(0.5, y), ncol=ncol,
               frameon=False, handlelength=2.6)


def line(color, ls="-", marker=None, label=None):
    return plt.Line2D([], [], color=color, ls=ls, marker=marker, label=label)


# ----------------------------------------------------------------------------- Q1

def fig_q1():
    global DROP_MARKERS
    DROP_MARKERS = SLIDES      # slides: the densest figure has no markers
    d = load("q1")
    d = d[d.variant != "sequential"]
    all_d = load("q1")
    ls_of = {"1": ":", "64": "--", "1024": "-"}
    mk_of = {"1": "o", "64": "s", "1024": "^"}
    shade = {"global-lock": COLOR["global-lock"], ("partition-lock", "4"): "#6da7ec",
             ("partition-lock", "64"): COLOR["partition-lock"]}
    fig, axes = panels(4, 2, w=5.2, h=3.4)
    best = {}
    for ax, lang in zip(axes, LANGS):
        for (var, part, batch), _ in d[d.language == lang].groupby(["variant", "partitions", "batch"]):
            key = var if var == "global-lock" else (var, part)
            sp = speedup(all_d, lang, variant=var, partitions=part, batch=batch)
            ax.plot(sp.index, sp.values, color=shade[key], ls=ls_of[batch], marker=mk(mk_of[batch]), ms=msize(3.5))
            best[(lang, var, part)] = max(best.get((lang, var, part), 0), sp.max())
        ax.axhline(1, color=INK2, lw=lwid(0.9))
        hw_lines(ax, label=(lang == LANGS[0]))
        worker_axis(ax)
        ax.set_yscale("log", base=2)
        ax.set_yticks([0.125, 0.25, 0.5, 1, 2, 4, 8])
        ax.set_yticklabels(["0.125", "0.25", "0.5", "1", "2", "4", "8"])
        ax.set_ylim(0.11, 8)
        ax.set_ylabel("speedup vs sequential (×)")
        ax.set_title(LANG_NAME[lang], loc="left", fontweight="bold")
    h = [line(shade["global-lock"], label="global-lock"),
         line(shade[("partition-lock", "4")], label="partition-lock, 4 partitions"),
         line(shade[("partition-lock", "64")], label="partition-lock, 64 partitions"),
         line(INK2, ":", mk("o"), "batch 1"), line(INK2, "--", mk("s"), "batch 64"), line(INK2, "-", mk("^"), "batch 1024")]
    legend_below(fig, h, 3)
    p64 = [best[(l, "partition-lock", "64")] for l in LANGS]
    title(fig, f"Q1: one lock stops at 1.3-1.7×; 64 partitions reach {min(p64):.1f}-{max(p64):.1f}×, "
               f"and a lock per posting is slower than no parallelism")
    save(fig, "q1-speedup")
    DROP_MARKERS = False


# ----------------------------------------------------------------------------- Q2

def fig_q2():
    q2, q2b = load("q2"), load("q2b")
    q2 = q2[q2.corpus_mb == 128]
    variants = [("partition-lock", q2, {"partitions": "64", "batch": "64"}),
                ("private-merge", q2, {}), ("private-merge-heap", q2b, {})]
    fig, axes = panels(4, 2, w=5.6, h=3.3)
    shares = {}
    for ax, lang in zip(axes, LANGS):
        x = np.arange(len(WORKERS))
        w = 0.27
        seq = q2[(q2.language == lang) & (q2.variant == "sequential")]
        ax.axhline(seq.total_ns.median() / 1e9, color=COLOR["sequential"], lw=lwid(1.2), ls="-.", zorder=1)
        for i, (var, d, where) in enumerate(variants):
            dd = d[(d.language == lang) & (d.variant == var)]
            for k, v in where.items():
                dd = dd[dd[k] == v]
            b = dd.groupby("threads").build_ns.median().reindex(WORKERS) / 1e9
            m = dd.groupby("threads").finalize_ns.median().reindex(WORKERS) / 1e9
            xs = x + (i - 1) * w
            ax.bar(xs, b, w * 0.9, color=COLOR[var], label=var)
            ax.bar(xs, m, w * 0.9, bottom=b, color="white", edgecolor=COLOR[var], hatch="////", lw=lwid(0.6))
            if var == "private-merge":
                shares[lang] = (m / (b + m)).loc[16]
        ax.set_xticks(x, [str(t) for t in WORKERS])
        ax.set_ylim(top=ax.get_ylim()[1] * 1.06)
        ax.set_xlabel("workers (threads)")
        ax.set_ylabel("time, median (s)")
        for xv, (wk, _) in zip([WORKERS.index(6), WORKERS.index(12), WORKERS.index(16)], HW_LINES):
            ax.axvline(xv, color="#a8a7a2", ls=(0, (4, 3)), lw=lwid(0.9), zorder=0.5)
        if lang == LANGS[0]:
            for wk, text in HW_LINES:
                hw_label(ax, WORKERS.index(wk), text)
        ax.set_title(LANG_NAME[lang], loc="left", fontweight="bold")
    h = [plt.Rectangle((0, 0), 1, 1, color=COLOR[v], label=v) for v in [v for v, _, _ in variants]]
    h += [line(COLOR["sequential"], "-.", label="sequential")]
    h += [plt.Rectangle((0, 0), 1, 1, fill=False, hatch="////", edgecolor=INK2, label="merge (hatched, on top of build)")]
    legend_below(fig, h, 5)
    title(fig, f"Q2: private indexes build faster, but the merge costs more than it saves "
               f"(merge is {min(shares.values()):.0%}-{max(shares.values()):.0%} of private-merge at 16 workers)")
    save(fig, "q2-build-merge")


def first_measured(d):
    """The first measured repetition of every run (design.md, Memory semantics: the process-wide
    peak never decreases across repetitions, so only the first one is a clean reading)."""
    return d[d.rep == d.groupby("run").rep.transform("min")]


def fig_q2_memory():
    q2, q2b = first_measured(load("q2")), first_measured(load("q2b"))
    q2 = q2[q2.corpus_mb == 128]
    variants = [("partition-lock", q2, {"partitions": "64", "batch": "64"}),
                ("private-merge", q2, {}), ("private-merge-heap", q2b, {})]
    fig, axes = panels(4, 2, w=5.2, h=3.3)
    ratio = {}
    for ax, lang in zip(axes, LANGS):
        for var, d, where in variants:
            dd = d[d.language == lang]
            s = curve(dd, "peak_rss_bytes", variant=var, **where) / 2**20
            ax.plot(s.index, s.values, color=COLOR[var], marker="o", ms=msize(3.5), label=var)
            if var == "private-merge":
                ratio[lang] = s.loc[22] / (curve(dd, "peak_rss_bytes", variant="partition-lock", **variants[0][2]).loc[22] / 2**20)
        s = curve(q2[q2.language == lang], "peak_rss_bytes", variant="sequential") / 2**20
        ax.axhline(s.iloc[0], color=COLOR["sequential"], lw=lwid(1), ls="-.", label="sequential")
        hw_lines(ax, label=(lang == LANGS[0]))
        worker_axis(ax)
        ax.set_ylim(bottom=0)
        ax.set_ylabel("peak memory, first measured rep (MiB)")
        ax.set_title(LANG_NAME[lang], loc="left", fontweight="bold")
    handles, labels = axes[0].get_legend_handles_labels()
    legend_below(fig, handles, 4)
    r = [ratio[l] for l in ["go", "cpp", "c"]]
    title(fig, f"Q2: private-merge needs {min(r):.1f}-{max(r):.1f}× the memory of partition-lock at 22 workers "
               f"in Go, C++ and C; Java's heap sizing hides it")
    save(fig, "q2-memory")


# ----------------------------------------------------------------------------- Q3

def fig_q3():
    d = load("q3")
    shade = {"1": "#5b1a05", "64": COLOR["partition-writers"], "1024": "#f5a26f"}
    ls_of = {"4": "--", "64": "-"}
    fig, axes = panels(4, 2, w=5.2, h=3.4)
    gm = {}
    for ax, lang in zip(axes, LANGS):
        dl = d[d.language == lang]
        for batch in ["1", "64", "1024"]:
            lock = curve(dl, "build_ns", variant="partition-lock", batch=batch)
            for q in ["4", "64"]:
                wr = curve(dl, "build_ns", variant="partition-writers", batch=batch, queue=q)
                r = (lock / wr).loc[WORKERS]
                ax.plot(r.index, r.values, color=shade[batch], ls=ls_of[q], marker="o", ms=msize(3))
                if batch == "64" and q == "64":
                    gm[lang] = float(np.exp(np.log(r.loc[2:]).mean()))
        ax.axhline(1, color=INK2, lw=lwid(0.9))
        ax.set_yscale("log")
        ax.set_ylim(0.025, 1.5)
        ax.set_yticks([0.03, 0.1, 0.3, 1])
        ax.set_yticklabels(["0.03", "0.1", "0.3", "1"])
        hw_lines(ax, label=(lang == LANGS[0]))
        worker_axis(ax)
        ax.set_ylabel("time lock / time messages (>1: messages win)")
        ax.set_title(LANG_NAME[lang], loc="left", fontweight="bold")
    h = [line(shade["1"], label="batch 1"), line(shade["64"], label="batch 64"), line(shade["1024"], label="batch 1024"),
         line(INK2, "-", label="mailbox 64"), line(INK2, "--", label="mailbox 4")]
    legend_below(fig, h, 5)
    title(fig, f"Q3: messages beat locks only with batch 64 and a mailbox of 64 "
               f"(+{(min(gm.values())-1):.0%} to +{(max(gm.values())-1):.0%} on average); one message per posting is 3-25× slower")
    save(fig, "q3-ratio")


def fig_q3_cpu():
    d = load("q3")
    d["util"] = d.cpu_ns / (d.build_ns * d.threads) * 100
    d["kernel"] = d.cpu_sys_ns / d.cpu_ns * 100
    ls_of = {"1": ":", "64": "-"}
    mk_of = {"1": "o", "64": "s"}
    # (variant, batch, where): row 1 compares lock and messages at batches 1 and 64; row 2 shows the
    # kernel share of partition-lock b64 and partition-writers b1 and b64.
    rows = [("util", [("partition-lock", "1", {"partitions": "64"}), ("partition-lock", "64", {"partitions": "64"}),
                      ("partition-writers", "1", {"queue": "64"}), ("partition-writers", "64", {"queue": "64"})]),
            ("kernel", [("partition-lock", "64", {"partitions": "64"}),
                        ("partition-writers", "1", {"queue": "64"}), ("partition-writers", "64", {"queue": "64"})])]
    if SLIDES:          # lighter version: only the kernel-time row, as a 2 x 2 grid
        rows = rows[1:]
        fig, axes = panels(4, 2)
    else:
        fig, axes = panels(8, 4, w=3.6, h=3.0)
    for r, (col, series) in enumerate(rows):
        for c, lang in enumerate(LANGS):
            ax = axes[(r * 4 + c)]
            dl = d[d.language == lang]
            for var, batch, where in series:
                s_ = curve(dl, col, variant=var, batch=batch, **where)
                ax.plot(s_.index, s_.values, color=COLOR[var], ls=ls_of[batch], marker=mk_of[batch], ms=msize(3.5))
            hw_lines(ax, label=(c == 0 if SLIDES else r == 0), compact=not SLIDES)
            worker_axis(ax)
            if col == "util":
                ax.set_ylim(0, 150)
                ax.axhline(100, color=INK2, lw=lwid(0.9))
                ax.set_ylabel("CPU time / (wall × workers) (%)")
            else:
                ax.set_ylim(0, 100)
                ax.set_ylabel("kernel share: sys / (user + sys) (%)")
            ax.set_title(LANG_NAME[lang] + ("" if SLIDES else (" - CPU utilization" if r == 0 else " - kernel time")),
                         loc="left", fontweight="bold", fontsize=fsize(9))
    h = [line(COLOR["partition-lock"], label="partition-lock (64 partitions)"),
         line(COLOR["partition-writers"], label="partition-writers (mailbox 64)"),
         line(INK2, ":", "o", "batch 1"), line(INK2, "-", "s", "batch 64"),
         line("#a8a7a2", (0, (4, 3)), label="dashed: 6 P-cores, 12 P-threads, 16 cores")]
    legend_below(fig, h, 5)
    j = curve(d[d.language == "java"], "util", variant="partition-writers", batch="1", queue="64").loc[22]
    k = [curve(d[d.language == l], "kernel", variant="partition-writers", batch="1", queue="64").loc[22] for l in ["cpp", "c"]]
    title(fig, f"Q3: at batch 1 Java's threads sleep (CPU falls to {j:.0f}%), C and C++ spend {min(k):.0f}-{max(k):.0f}% "
               f"of their CPU time in the kernel yielding, Go waits in user space")
    save(fig, "q3-cpu")


# ----------------------------------------------------------------------------- Q4

def fig_q4_pinning():
    q2, q4a = load("q2"), load("q4a")
    q2 = q2[q2.corpus_mb == 128]
    cfg = [("partition-lock", {"partitions": "64", "batch": "64"}), ("private-merge", {})]
    fig, axes = panels(4, 2, w=5.2, h=3.4)
    for ax, lang in zip(axes, LANGS):
        for var, where in cfg:
            for d, ls, mk in [(q2, "-", "o"), (q4a, "--", "s")]:
                s = speedup(d, lang, "total_ns", variant=var, **where)
                ax.plot(s.index, s.values, color=COLOR[var], ls=ls, marker=mk, ms=msize(3.5))
        ax.plot([1, 22], [1, 22], color="#c9c8c3", lw=lwid(1), zorder=0.4)
        hw_lines(ax, label=(lang == LANGS[0]))
        worker_axis(ax)
        ax.set_ylim(0, 7.5)
        ax.set_ylabel("speedup vs sequential, build + merge (×)")
        ax.set_title(LANG_NAME[lang], loc="left", fontweight="bold")
    h = [line(COLOR["partition-lock"], label="partition-lock"), line(COLOR["private-merge"], label="private-merge"),
         line(INK2, "-", "o", "unpinned (Q2)"), line(INK2, "--", "s", "pinned to P-cores, up to 12 (Q4a)"),
         line("#c9c8c3", "-", label="ideal linear speedup")]
    legend_below(fig, h, 5)
    r = pd.concat([speedup(q4a, l, "total_ns", variant="partition-lock", partitions="64", batch="64")
                   / speedup(q2, l, "total_ns", variant="partition-lock", partitions="64", batch="64")
                   for l in LANGS]).dropna()
    r = r[r.index > 1]
    title(fig, f"Q4: pinning to the P-cores changes partition-lock's speedup from {r.min()-1:+.0%} to {r.max()-1:+.0%} up to "
               f"12 workers; the bend at 6 stays, so it is the hardware")
    save(fig, "q4-pinning")


def fig_q4_static():
    q4a, q4b = load("q4a"), load("q4b")
    fig, axes = panels(2, 2, w=5.4, h=3.4)
    for ax, (value, ylabel) in zip(axes, [("build_ns", "build time, median (s)"), ("total_ns", "build + merge, median (s)")]):
        for var in ["private-merge", "private-merge-static"]:
            for d, ls, mk in [(q4b, "-", "o"), (q4a, "--", "s")]:
                s = curve(d[d.language == "c"], value, variant=var) / 1e9
                ax.plot(s.index, s.values, color=COLOR[var], ls=ls, marker=mk, ms=msize(3.5))
        hw_lines(ax, label=(value == "build_ns"))
        worker_axis(ax)
        ax.set_ylim(bottom=0)
        ax.set_ylabel(ylabel)
        ax.set_title("C: " + ("build only" if value == "build_ns" else "build + merge"), loc="left", fontweight="bold")
    dyn, sta = (curve(q4b[q4b.language == "c"], "build_ns", variant=v) for v in ["private-merge", "private-merge-static"])
    gap = (sta / dyn).loc[[8, 12, 16, 22]]
    h = [line(COLOR["private-merge"], label="dynamic (private-merge)"), line(COLOR["private-merge-static"], label="static split"),
         line(INK2, "-", "o", "unpinned (Q4b)"), line(INK2, "--", "s", "pinned, up to 12 (Q4a)")]
    legend_below(fig, h, 4)
    title(fig, f"Q4 (C): an equal static split costs {gap.min()-1:.0%}-{gap.max()-1:.0%} of the build from 8 workers up, "
               f"and the merge hides it in the total")
    save(fig, "q4-static-dynamic")


# ----------------------------------------------------------------------------- Q5

def build_latency_table():
    """Percentiles of every measured repetition of every q5 run, from the raw latency files."""
    res = glob.glob(str(ROOT / "results" / "q5" / "*" / "results.tsv"))[0]
    d = pd.read_csv(res, sep="\t", dtype={"run": str, "partitions": str, "batch": str})
    d = d[d.warmup == 0]
    keys = d[["run", "language", "variant", "mode", "threads"]].drop_duplicates()
    rows = []
    for _, k in keys.iterrows():
        f = Path(res).parent / "latencies" / f"{k.run}.tsv"
        lat = pd.read_csv(f, sep="\t", header=None, names=["rep", "warmup", "latency_ns"])
        lat = lat[lat.warmup == 0]
        for rep, g in lat.groupby("rep"):
            v = g.latency_ns.to_numpy()
            p50, p95, p99 = np.percentile(v, [50, 95, 99])
            rows.append([k.run, k.language, k.variant, k["mode"], k.threads, rep, len(v), p50, p95, p99, v.max()])
    out = pd.DataFrame(rows, columns=["run", "language", "variant", "mode", "threads", "rep", "n_queries",
                                      "p50_ns", "p95_ns", "p99_ns", "max_ns"])
    LAT_TABLE.parent.mkdir(parents=True, exist_ok=True)
    out.round(1).to_csv(LAT_TABLE, sep="\t", index=False)
    print("wrote", LAT_TABLE.relative_to(ROOT), len(out), "rows")


def fig_q5():
    d = load("q5")
    d["throughput"] = d.ops / d.run_ns * 1e9 / 1e3          # thousand operations per second
    modes = ["query", "mixed-95-5", "mixed-50-50", "mixed-20-80"]
    ls_of = {"java": "-", "c": "--"}
    mk_of = {"java": "o", "c": "s"}
    for name, kind in [("q5-throughput", "tp"), ("q5-p99", "p99")]:
        if kind == "p99":
            lat = pd.read_csv(LAT_TABLE, sep="\t", dtype={"run": str})
        fig, axes = panels(4, 2, w=5.2, h=3.4)
        for ax, mode in zip(axes, modes):
            for lang in ["java", "c"]:
                for var in ["partition-lock", "partition-rwlock"]:
                    if kind == "tp":
                        s = curve(d[(d.language == lang) & (d["mode"] == mode)], "throughput", variant=var)
                    else:
                        s = curve(lat[(lat.language == lang) & (lat["mode"] == mode)], "p99_ns", variant=var) / 1e3
                    ax.plot(s.index, s.values, color=COLOR[var], ls=ls_of[lang], marker=mk_of[lang], ms=msize(3.5))
            hw_lines(ax, label=(mode == modes[0]))
            worker_axis(ax)
            if kind == "tp":
                ax.set_ylim(bottom=0)
                ax.set_ylabel("throughput, median (thousand ops/s)")
            else:
                ax.set_yscale("log")
                lo, hi = ax.get_ylim()
                wanted = [500, 1000, 2000, 5000, 10000, 20000] if SLIDES else [200, 300, 500, 1000, 2000, 3000, 5000, 10000, 20000]
                ticks = [t for t in wanted if lo <= t <= hi]
                ax.set_yticks(ticks, [f"{t:,}" for t in ticks])
                ax.minorticks_off()
                ax.set_ylabel("p99 query latency, median of reps (µs)")
            ax.set_title(mode, loc="left", fontweight="bold")
        h = [line(COLOR["partition-lock"], label="partition-lock"), line(COLOR["partition-rwlock"], label="partition-rwlock"),
             line(INK2, "-", "o", "Java"), line(INK2, "--", "s", "C")]
        legend_below(fig, h, 4)
        if kind == "tp":
            r = {l: curve(d[(d.language == l) & (d["mode"] == "query")], "throughput", variant="partition-rwlock").loc[22]
                 / curve(d[(d.language == l) & (d["mode"] == "query")], "throughput", variant="partition-lock").loc[22]
                 for l in ["java", "c"]}
            title(fig, f"Q5: the read/write lock gives {r['java']:.1f}× (Java) and {r['c']:.1f}× (C) the throughput\n"
                       f"of a plain lock in query mode at 22 workers, and still wins with 80% writes")
        else:
            r = pd.Series({(l, mo): curve(lat[(lat.language == l) & (lat["mode"] == mo)], "p99_ns", variant="partition-rwlock").loc[22]
                           / curve(lat[(lat.language == l) & (lat["mode"] == mo)], "p99_ns", variant="partition-lock").loc[22]
                           for l in ["java", "c"] for mo in modes})
            title(fig, f"Q5: at 22 workers the read/write lock's p99 is {r.min():.2f}-{r.max():.2f}× the plain lock's, "
                       f"lower in every mode")
        save(fig, name)


# ----------------------------------------------------------------------------- numbers

def print_numbers():
    """The numbers quoted in docs/experiment-plan.md that this script is the source of."""
    q2 = load("q2"); q2 = q2[q2.corpus_mb == 128]
    pl = {"partitions": "64", "batch": "64"}
    print("Q2 merge share of private-merge total, 16 workers (%):")
    for l in LANGS:
        d = q2[q2.language == l]
        print(f"  {l}: {curve(d, 'finalize_ns', variant='private-merge').loc[16] / curve(d, 'total_ns', variant='private-merge').loc[16] * 100:.1f}")
    q2f = first_measured(q2)
    print("Q2 peak memory, first measured repetition, MiB: private-merge 1 and 22 workers; partition-lock 1 and 22; ratio at 22")
    for l in LANGS:
        d = q2f[q2f.language == l]
        a, b = curve(d, "peak_rss_bytes", variant="private-merge") / 2**20, curve(d, "peak_rss_bytes", variant="partition-lock", **pl) / 2**20
        print(f"  {l}: {a.loc[1]:.0f} {a.loc[22]:.0f} | {b.loc[1]:.0f} {b.loc[22]:.0f} | {a.loc[22] / b.loc[22]:.2f}")
    q4a, q4b = load("q4a"), load("q4b")
    print("Q4 (C) static / dynamic - 1, in %, build and build + merge, unpinned then pinned:")
    for name, d in [("unpinned", q4b), ("pinned", q4a)]:
        d = d[d.language == "c"]
        for col in ["build_ns", "total_ns"]:
            r = (curve(d, col, variant="private-merge-static") / curve(d, col, variant="private-merge") - 1) * 100
            print(f"  {name} {col}: " + " ".join(f"{w}:{v:.1f}" for w, v in r.items()))
    dyn8 = curve(q4b[q4b.language == "c"], "total_ns", variant="private-merge").loc[8]
    print(f"  static at 22 workers vs dynamic at 8 (total): {(curve(q4b[q4b.language == 'c'], 'total_ns', variant='private-merge-static').loc[22] / dyn8 - 1) * 100:.1f}%")
    q3 = load("q3"); q3["kernel"] = q3.cpu_sys_ns / q3.cpu_ns * 100
    print("Q3 kernel share at 22 workers, partition-writers batch 1 mailbox 64 (%):")
    for l in LANGS:
        print(f"  {l}: {curve(q3[q3.language == l], 'kernel', variant='partition-writers', batch='1', queue='64').loc[22]:.0f}")
    q5 = load("q5"); lat = pd.read_csv(LAT_TABLE, sep="\t", dtype={"run": str})
    print("Q5 p99 rwlock / plain lock, by workers:")
    for l in ["java", "c"]:
        for mo in ["query", "mixed-95-5", "mixed-50-50", "mixed-20-80"]:
            la = lat[(lat.language == l) & (lat["mode"] == mo)]
            r = curve(la, "p99_ns", variant="partition-rwlock") / curve(la, "p99_ns", variant="partition-lock")
            print(f"  {l} {mo}: " + " ".join(f"{w}:{v:.2f}" for w, v in r.items()))


# ----------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rebuild-latency-table", action="store_true",
                    help="recompute analysis/data/q5-latency-percentiles.tsv from the raw latency files")
    ap.add_argument("--slides", action="store_true",
                    help="write the slide versions (16:9, no title, large text) to analysis/figures/slides/")
    ap.add_argument("--numbers", action="store_true",
                    help="print the numbers quoted in docs/experiment-plan.md, then exit")
    args = ap.parse_args()
    if args.numbers:
        print_numbers()
        return
    if args.slides:
        global SLIDES, FIG_DIR
        SLIDES, FIG_DIR = True, FIG_DIR / "slides"
        plt.rcParams.update(SLIDE_RC)
    if args.rebuild_latency_table:
        build_latency_table()
    if not LAT_TABLE.exists():
        sys.exit(f"{LAT_TABLE} is missing: run with --rebuild-latency-table (needs results/q5/*/latencies/)")
    fig_q1(); fig_q2(); fig_q2_memory(); fig_q3(); fig_q3_cpu()
    fig_q4_pinning(); fig_q4_static(); fig_q5()


if __name__ == "__main__":
    main()
