# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""What was last run where, and how long ago: the ledger, read back.

    python3 tools/drive/status.py              # everything
    python3 tools/drive/status.py native       # environments with that in
    python3 tools/drive/status.py --stale 5    # only what is that far behind

bench.py's lines are shown where there are any, the latest run's status,
rate and p99 interval; nothing is owed one.

A block for each environment and device -- an environment, its machine where it
has one, the GPU its program said it was on -- and under it a row for each
kind of run: the instrument, the build variant, whether the validation
layer was in front, the monitor if one was named. A row is the *latest*
such run, with the commit it was run at and how many commits the tree has
moved since. `+` after a commit is a tree that had uncommitted changes.

    win64-vulkan-native@schenker  --  NVIDIA GeForce GTX 1050 Ti
      stress   release              66 checks, none failed  3fdf243+  HEAD
      pace     release              never

`never` is the point of the table. The environments are the ones a bare
round would drive today (environment.py's, with the machines FNL_DRIVE_VM and
native-hosts name), and each is owed a round of each instrument that
applies to it at `release`, on every device it has been seen with and
every one written down for it, and on every kernel driver bare-hosts names
for its machine (`kms-vulkan-bare@schenker  --  …, on nouveau`); a row
nobody has run is printed and not left out. A device written down and
never run on is a block under the name it was written by, since the
program has not yet said its own.

Reads only. The lines are ledger.py's, written by the rounds themselves.
"""

import argparse
import sys

import ledger

# What every environment is owed, and pace.py's columns beside it: the
# programs on this desk and the laptops, never a guest.
ROUNDS = ("stress",)
PACED = ("win64-vulkan-native", "win64-d3d12-native", "macos-metal-native",
         "macos-vulkan-native", "macos-gl-native", "x11-vulkan-bare",
         "wayland-vulkan-bare", "kms-vulkan-bare")


def environments():
    """{name: (whether pace.py has a column for it, the (device, kernel
    driver) pairs a round names there, either empty)}, for everything a
    bare round would drive. Empty if the harness will not import here, and
    the table is then the ledger's own environments and no `never`."""
    owed = {}
    try:
        import environment as B
        for cls in B.ALL:
            for b in cls.each():
                if "@" not in b.NAME and not cls.LOCAL:
                    continue    # an environment with no machine named
                paced, asked = owed.setdefault(
                    ledger.place(b.NAME),
                    (cls.LOCAL or cls.NAME in PACED, []))
                driver = getattr(b, "driver", "")
                if b.gpu or driver:
                    asked.append((b.gpu, driver))
    except Exception as e:
        print("(the environments could not be listed: %s)\n" % e)
    return owed


def kind(line):
    """What a row is: one way of running one instrument."""
    how = line.get("variant", "?")
    if line.get("layer"):
        how += "+validation"
    if line.get("late_latch") is False:
        how += ", no late latch"
    if line.get("vsync") is False:
        how += ", vsync off"
    if line.get("output"):
        how += " on " + line["output"]
    if line.get("wine"):
        how += " under " + line["wine"]
    if line.get("vkd3d"):
        how += ", %s's vkd3d" % line["vkd3d"]
    return line.get("instrument", "?"), how


def result(line):
    if line.get("instrument") == "bench" and "status" in line:
        rate, p99 = line.get("rate_fps"), line.get("p99_ms")
        return "%s, %s fps, p99 %s ms" % (
            line["status"], "-" if rate is None else "%.1f" % rate,
            "-" if p99 is None else "%.2f" % p99)
    if "cells" in line:
        cells = line["cells"]
        idle = cells.get("0")
        rates = len(cells) - ("border" in cells) - ("draw" in cells)
        said = "%d rate%s%s" % (rates, "" if rates == 1 else "s",
                                ", border" if "border" in cells else "")
        if idle:
            said = "idle %.0f fps, %s" % (idle[0], said)
        if cells.get("draw"):
            said += ", drawing %.0f fps" % cells["draw"][0]
        if "skipped" in line:
            said += ", then stopped"
        return said
    failed = line.get("failed", 0)
    return "%d checks, %s" % (line.get("checks", 0),
                              "%d FAILED" % failed if failed
                              else "none failed")


class Ages:
    """How many commits HEAD is past another, asked of git once each."""

    def __init__(self):
        self.known = {}

    def behind(self, commit):
        if commit not in self.known:
            # A line written where git said nothing has no commit, and
            # "..HEAD" would count it as HEAD itself.
            n = ledger.git("rev-list", "--count", "%s..HEAD" % commit) \
                if commit else ""
            self.known[commit] = int(n) if n.isdigit() else None
        return self.known[commit]

    def say(self, commit):
        n = self.behind(commit)
        # A commit git no longer has: rebased away, or another clone's.
        return "?" if n is None else "HEAD" if n == 0 else "%d behind" % n


def main():
    ap = argparse.ArgumentParser(
        description="What was last run where, from the rounds' ledger.")
    ap.add_argument("only", nargs="*",
                    help="show environments whose name has one of these in")
    ap.add_argument("--stale", type=int, metavar="N", default=None,
                    help="only rows never run or at least N commits behind")
    args = ap.parse_args()

    lines = ledger.read()
    owed = environments()

    # The latest run of each kind in each block -- an environment, the
    # device, the kernel driver bound -- and the latest skip of each
    # environment: a skip has no device, so it is the environment's.
    runs, skips, devices = {}, {}, {}
    for line in lines:
        env = line.get("env", "?")
        if "skipped" in line and "cells" not in line:
            skips[env] = line
            continue
        dev = (line.get("device", ""), line.get("driver", ""))
        devices.setdefault(env, [])
        if dev not in devices[env]:
            devices[env].append(dev)
        runs.setdefault((env, dev), {})[kind(line)] = line

    ages = Ages()
    names = list(owed) + sorted(e for e in set(devices) | set(skips)
                                if e not in owed)
    shown = 0
    for env in names:
        if args.only and not any(o in env for o in args.only):
            continue
        skip = skips.get(env)
        paced, asked = owed.get(env, (False, []))
        seen = devices.get(env, [])
        # A device a round would name and no program has yet answered to,
        # matched as the library matches the name: any part, case aside;
        # and on the driver the round binds, which an older line names not
        # at all.
        unmet = [a for a in asked
                 if not any(a[0].lower() in d.lower() and a[1] == drv
                            for d, drv in seen)]
        slots = seen + unmet or [("", "")]
        for dev, drv in slots:
            rows = dict(runs.get((env, (dev, drv)), {}))
            # A line that names no device is an older instrument's; what is
            # owed is the devices' to show, and only an environment that
            # has none shows it here.
            if env in owed and (dev or drv or len(slots) == 1):
                for inst in ROUNDS + (("pace",) if paced else ()):
                    rows.setdefault((inst, "release"), None)
            out = []
            for (inst, how), line in sorted(
                    rows.items(), key=lambda kv: (kv[0][0], kv[0][1])):
                if line is None:
                    out.append("  %-8s %-20s never" % (inst, how))
                    continue
                n = ages.behind(line["commit"])
                if args.stale is not None and n is not None \
                        and n < args.stale:
                    continue
                out.append("  %-8s %-20s %-24s %-9s %-10s %s"
                           % (inst, how, result(line),
                              line["commit"] + ("+" if line.get("dirty")
                                                else ""),
                              ages.say(line["commit"]), line["when"][:10]))
                for f in line.get("fails", []):
                    out.append("  %-8s   %s" % ("", f))
            if not out:
                continue
            print(env + ("  --  " + dev if dev else "")
                  + (", on " + drv if drv else ""))
            print("\n".join(out))
            # Said only when it is the news: a machine that was off for the
            # last attempt has older figures than its rows suggest.
            newest = max((r["when"] for r in rows.values() if r), default="")
            if skip and skip["when"] > newest:
                print("  last tried %s, skipped -- %s"
                      % (skip["when"][:10], skip["skipped"]))
            print()
            shown += 1

    if not shown:
        print("nothing to show%s" % ("" if lines else
                                     " -- no round has written to %s yet"
                                     % ledger.PATH))
    return 0


if __name__ == "__main__":
    sys.exit(main())
