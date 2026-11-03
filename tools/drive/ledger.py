# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""What was run where: one line a round, kept beside the portal's token.

A round prints its totals and they are gone with the terminal; the figures
that stay are DESIGN.md's, each dated by whichever commit last touched its
paragraph. So the rounds write down what they did -- stress.py a line
for each environment, pace.py one for each column -- and
status.py reads the lines back as a table of what was last run where, and
how many commits ago.

A line is one JSON object. Every one has

    when        UTC, to the second
    commit      the tree's, abbreviated, and `dirty` if it had changes
    instrument  stress, pace, bench, soak: the file that ran; checks for
                tools/probe/bare_checks.py
    env         where: the environment's name, with its machine where it has one
    variant     the build tree's
    layer       whether the validation layer was asked for

and then either `skipped`, the reason the round never started, or what it
found: `checks`, `failed`, `na` and the names in `fails` for a round, `cells`
(rate: [fps, cpu]) for a sweep. `device` is the GPU the program said it was
on, `asked` the one the round named for it where it named one, `driver`
the kernel driver the round bound the NVIDIA GPU to first where it bound
one (a KMS round on a driver, `kms-vulkan-bare@schenker:nouveau`), and
`output` the monitor the round was sent to, where one was named; `wine`
the wine a Windows round on this desk ran under, as it gives its version,
where FNL_DRIVE_WINE named one, and `vkd3d` "wine" where FNL_DRIVE_VKD3D
put a Direct3D 12 round on wine's own.

Under $XDG_STATE_HOME and not under build/, for the reason the token is:
`rm -rf build` is how this tree is cleaned, and what a machine was last seen
doing is not a build artefact. Not tracked either -- it is this desk's
account of this desk's machines. FNL_DRIVE_LEDGER names another file, and
set to nothing switches the writing off.
"""

import json
import os
import re
import subprocess
import time

from testbed import ROOT, STATE_DIR

PATH_ENV = "FNL_DRIVE_LEDGER"
PATH = os.environ.get(PATH_ENV, os.path.join(STATE_DIR, "ledger.jsonl"))

# The library's startup inventory, one line a device, and the line after it
# where there was a choice: pick_device() in src/renderer/vulkan/device.c.
LISTED = re.compile(r"GPU \d+: (.*) \(([^()]*)\)\s*$")
IN_USE = "GPU in use: "
# Where the opening is over: the first move to another GPU (frame.c), and
# the other renderers' GPUs listed beside the first frame (app.c), whose
# lines are no longer the starting renderer's inventory. Such a listing
# also comes ahead of that inventory where GL draws or the backend shares
# a driver (gpu_list_early()): only one after the inventory ends it, and
# there, where none follows, a second run of GPU lines does (device()).
OPENED = re.compile(r"^(moving to |\S.* lists \d+ GPUs? here)")


def git(*args):
    try:
        return subprocess.run(("git", "-C", ROOT) + args, capture_output=True,
                              text=True, timeout=10).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return ""


def commit():
    """(abbreviated hash, whether tracked files differ from it)."""
    return (git("rev-parse", "--short", "HEAD"),
            bool(git("status", "--porcelain", "--untracked-files=no")))


def device(lines):
    """The GPU a program's own log says it is on, or "" if it does not say.

    The library names the one in use only where there were several; with
    one line of inventory that line is the answer, and with several and no
    name there is none to give. Read up to OPENED alone: past it a `GPU in
    use` line is a move's, which a round opened on the only device its
    renderer has -- lavapipe in the guests -- would otherwise record as
    its own."""
    listed, gap = [], False
    for line in lines:
        if OPENED.match(line) and (listed or line.startswith("moving to ")):
            break
        if IN_USE in line:
            return line.split(IN_USE, 1)[1].strip()
        m = LISTED.search(line)
        if m:
            # A second run of them is the whole list said after the first
            # frame (app_gpu_list_say()), where gpu_list_early() left no
            # listing after the inventory to end the opening.
            if gap:
                break
            listed.append(m.group(1))
        elif listed:
            gap = True
    return listed[0] if len(listed) == 1 else ""


def place(name):
    """A round's name without the device it asked for, or the kernel driver
    it bound: each is a field of its own, the device in the program's words
    and not the round's."""
    return name.split(":")[0]


def bound(b):
    """The `driver` field for environment `b`, where it bound one."""
    d = getattr(b, "driver", "")
    return {"driver": d} if d else {}


def wine(b):
    """The `wine` and `vkd3d` fields for environment `b`, a class or one
    of its places, where a round named either. Neither is said of the
    path's wine or of vkd3d-proton: the default is no fact about the
    round, as an unnamed monitor is not."""
    what = {}
    try:
        if hasattr(b, "wine_version") and b.wine_version():
            what["wine"] = b.wine_version()
        if hasattr(b, "vkd3d") and b.vkd3d():
            what["vkd3d"] = b.vkd3d()
    except Exception:
        pass        # a choice the round refused is in its `skipped`
    return what


def layer():
    """Whether this desk's environment asks for the validation layer: the
    library's way, FNL_VK_VALIDATION=1, which puts it in the library's own
    instance alone (src/renderer/vulkan/device.c), or the loader's, which
    puts it in every instance of the process."""
    return os.environ.get("FNL_VK_VALIDATION") == "1" or "validation" in \
        " ".join(os.environ.get(k, "")
                 for k in ("VK_LOADER_LAYERS_ENABLE", "VK_INSTANCE_LAYERS"))


def record(instrument, env, variant, **what):
    """Append a line. A ledger that cannot be written costs one line of
    output and never the round. `layer` is the environment's (layer()) unless
    the caller says otherwise: a program that draws with OpenGL loads no
    Vulkan layer whatever the environment asks."""
    if not PATH:
        return
    head, dirty = commit()
    line = dict(when=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                commit=head, dirty=dirty, instrument=instrument, env=env,
                variant=variant, layer=layer())
    line.update(what)
    try:
        os.makedirs(os.path.dirname(PATH) or ".", exist_ok=True)
        with open(PATH, "a") as f:
            f.write(json.dumps(line, sort_keys=True) + "\n")
    except OSError as e:
        print("(not written to the ledger: %s)" % e, flush=True)


def read():
    """Every line, oldest first. One that will not parse is left out: a
    round killed mid-write leaves half of one."""
    lines = []
    try:
        with open(PATH) as f:
            for raw in f:
                try:
                    lines.append(json.loads(raw))
                except ValueError:
                    pass
    except OSError:
        pass
    return lines
