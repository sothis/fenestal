---
name: checkers
description: Run the testbed's checker builds — Vulkan validation (with sync and best-practices), ThreadSanitizer, ASan+UBSan, and the GCC static analyser — with each one's baseline, and how to confirm the checker was actually switched on. Use when asked to check, validate, sanitize, or analyse the tree.
disable-model-invocation: true
---

Run the checkers the user asked for. If they did not narrow it, ask which — and
which *target*: the runtime ones each need a Vulkan device and a live display
server. The analyser is the only one that is build-only. Left alone the runtime
ones sit there until the window is closed, which checks almost nothing; drive
them instead (see below).

The native target, `linux`, takes the runtime checkers, and its tree has
every Linux backend and both Linux renderers: `FNL_BACKEND=wayland` runs it
against the compositor and `FNL_BACKEND=x11` against XWayland, and
`FNL_RENDERER=vulkan` or `gl` draws with that one. Name both every time --
this desktop has both `WAYLAND_DISPLAY` and `DISPLAY`, and left to itself
the program opens Wayland with Vulkan. Every command below reads the
same way with the backend swapped, and running both is the cheapest way to
tell a finding in the shared core from one in a backend.

Every tree is `build/<target>/<variant>`, and `./build.sh <target> <variant>`
configures and builds it with the right flags. The variants below are what that
wrapper sets; the raw cmake is shown so the flags are visible, not because it
has to be typed.

Only the Linux trees take the sanitizer runtimes -- `linux`, and
`linux-arm64`, whose checker rounds run on the Pi's own. mingw-w64 has none,
so `win64` takes `release`, `analyzer` and `gc` alone (its `gc` with UBSan
in trap mode, which needs no runtime); `macos` takes `release` and `gc`;
`build.sh` refuses the rest.

## Vulkan validation

Turns on the two checks that are off by default. Runs against the ordinary
release tree — no separate configure.

```sh
./build.sh linux
VK_LOADER_LAYERS_ENABLE='*validation' VK_LAYER_VALIDATE_SYNC=1 \
VK_LAYER_VALIDATE_BEST_PRACTICES=1 FNL_BACKEND=wayland FNL_RENDERER=vulkan \
  ./build/linux/release/fenestal-testbed
```

A Windows laptop the harness knows takes it too, against that machine's own
driver: the same environment in front of `stress.py win64-vulkan-native`,
which hands `VK_*` to the program there and stops if the layer is not then
loaded in it. So does the bare X server's machine, `stress.py
x11-vulkan-bare`, against lavapipe; that machine also takes the `asan`,
`tsan` and `gc` trees, with this desktop's runtimes shipped beside the
program and `tsan-lavapipe.supp` added to `tsan.supp` there.

Does **not** compose with the Mesa screenshot layer — enable both and no PNG is
written. Validate and capture in separate runs.

## ThreadSanitizer

```sh
./build.sh linux tsan
TSAN_OPTIONS=suppressions=$PWD/tsan.supp \
FNL_BACKEND=wayland FNL_RENDERER=vulkan \
  ./build/linux/tsan/fenestal-testbed
```

The wrapper sets `-fsanitize=thread` on the compile *and* on both linker
variables. Both matter: this project links an archive and a shared object, and
instrumenting only the executable would leave nearly all the code out.

`tsan.supp` documents what each suppression covers and why. It also carries an
explicit "do NOT add `called_from_lib:libvulkan_radeon.so`" warning — that entry
manufactures ten fake libwayland races. Any new suppression must state what was
established and how it was established.

## Address + UndefinedBehaviour

They share one build.

```sh
./build.sh linux asan
FNL_BACKEND=wayland FNL_RENDERER=vulkan ./build/linux/asan/fenestal-testbed
```

## GCC static analyser

Build-only; nothing is run.

```sh
./build.sh linux analyzer
```

One tree compiles every Linux backend, so one analyser run reads Wayland's,
X11's and KMS's code together.

The Windows target takes it, since `-fanalyzer` is a compile-time pass and
needs no runtime:

```sh
./build.sh win64 analyzer
```

**Build it from scratch when the claim matters.** The analyser tree is an
ordinary incremental build, so a translation unit compiled into it once is never
recompiled and never warned about again — a clean run over a tree that rebuilt
nothing says nothing at all. `rm -rf build/<target>/analyzer` before the sweep,
or force one file by deleting its object under that tree's `CMakeFiles/` and
building the target again. To confirm the flag reached a target you doubt, read
it back out of that target's own `flags.make` under the same `CMakeFiles/`.

The sanitizer and analyser builds keep `mkfont` and `verifyfont` out of the
instrumentation (`-fno-sanitize=all` per target). They run *during* the build,
so instrumenting them slows the bake for no benefit and fails the build on the
tool's exit status rather than on anything wrong with the project.

## Driving them, rather than sitting in front of them

A checker that only sees startup and teardown checks almost nothing.
`tools/drive/stress.py` exercises the state space under it — the titlebar hit
test, maximize, four fullscreen cycles, the resize edges, a pointer capture held
across fullscreen, the reticle release, keys and scroll — in every environment
that will run here, in one round:

```sh
python3 tools/drive/stress.py --variant asan
python3 tools/drive/stress.py --variant tsan     # sets TSAN_OPTIONS itself
python3 tools/drive/stress.py x11-vulkan --variant asan   # one of them
```

Running it in more than one environment is what places a finding: the same
round on more than one platform is what tells a bug in the shared core from
one in a backend or a renderer, and it is the default. The harness names
each environment's platform to the program itself.

Validation has no variant of its own — it runs against `release` — so put the
layer's environment in front of the round instead:

```sh
VK_LOADER_LAYERS_ENABLE='*validation' VK_LAYER_VALIDATE_SYNC=1 \
VK_LAYER_VALIDATE_BEST_PRACTICES=1 \
  python3 tools/drive/stress.py wayland-vulkan
```

Under `asan` and `tsan` the round answers more slowly and knows it — it waits
longer at each step for that reason, so expect it to take several times as long
as `release` rather than assuming it has hung.

Every environment on this desktop is driven through the RemoteDesktop portal,
which holds a saved token and asks nothing. If a round reports no input
arriving, look for a modal dialog on the desktop before reading it as a finding
— and do not reach for XTEST to check: on this desktop each XTEST client raises
exactly such a dialog. tools/drive/README.md *Driving a window* has the account.

## Reading the results

The baseline is: `src/` clean under the analyser, all three sanitizers, and
validation including sync and best-practices. Report anything that breaks that.
Both native targets have been held to it -- and `x11-vulkan` is where TSan
earned its keep, finding a race in `src/core/api_state.c` that neither of the
other two platforms had ever tripped.

Last held to it whole, from the three targets' trees deleted and rebuilt,
every checker driven by the round rather than watched at idle:

```
./build.sh                              13 trees, not one gcc warning
stress.py --capture                     56 / 74 / 65 / 41 / 69 / 69, 0 failed
stress.py --variant asan --capture      56 / 74,                     0 failed
stress.py --variant tsan --capture      56 / 74,                     0 failed
validation --capture, native targets    56 / 74,                     0 failed
```

The six-figure lines are Wayland, X11, wine on X11, wine on Wayland and the
Windows 10 and 11 guests, in that order; the others are the two native
targets.

Two things to read from those figures rather than around them. Every round
takes `--capture`, under the sanitizers and the validation layer as at
release, so a checker round is the release round's size and one that comes up
short was run without it. And the sweep has to be from scratch for the
analyser line to mean anything at all -- an incremental tree recompiles
nothing it was not made to and therefore warns about nothing there, which
reads exactly like a clean run.

The cross-monitor section is the newest thing under them, and the one most
worth having there: it is what writes `want_output` and `want_out_*` under
`App::lock` from the input thread while the render thread latches the pair.
TSan had nothing to say about it on either platform.

`src/core/` has a second baseline worth checking when anything moves between
directories: it must name no Wayland, X11, Win32 or Vulkan symbol at all.
That is what makes the frame policy shareable between platforms, and
`python3 tools/check-layering.py` is what checks it — it counts every
directory's references to every API and exits non-zero on one that should not
be there.

`tools/` **had two known `-fanalyzer` false positives and now has none.** That
is a stronger baseline rather than a missing one: with nothing known, *any*
analyser warning anywhere in the tree is a finding, so report all of them.
Verified from scratch on all three targets, tools included, with the flag
confirmed in their `flags.make`.

Both were removed rather than suppressed, and *how* is the part worth reusing.
Each was an invariant the analyser was right not to be able to carry:

* `mkfont.c` — "every element of this range was written", where the fill loop
  walks `(scale, size, glyph)` through `CELL_AT` and the dedup pass reads the
  same memory as a flat run, the two index expressions being equal without
  looking it.
* `font_codec.h` — `prev = out[o - off]` reads a byte the same loop wrote a
  moment earlier, which is the overlapping self-copy the format leans on for
  runs. It only ever fired from `verifyfont`'s translation unit, where the
  destination is a plain `malloc` the analyser can model; the library's own
  `text.c` decodes into mapped device memory it cannot see into.

Both were answered by making the invariant true *at the allocation* instead —
`xcalloc()` in `mkfont.c` and in `verifyfont.c`, each carrying the reasoning and
what the warning was. Neither is a pragma and neither should become one: a
suppression hides the next warning too, and the analyser was not wrong, only
unable to carry an argument. If a new one appears, prefer construction over
suppression and write down which it was.

Silence is only worth something if the checker was switched on. Sync validation
reports nothing both when the code is correct and when the setting was ignored,
so a clean run is worth reporting only alongside what actually loaded:

```sh
ldd build/linux/tsan/fenestal-testbed | grep libtsan  # TSan is linked
VK_LOADER_DEBUG=layer VK_LOADER_LAYERS_ENABLE='*validation' \
FNL_BACKEND=wayland FNL_RENDERER=vulkan \
  timeout 5 ./build/linux/release/fenestal-testbed 2>&1 \
  | grep -iE 'loading layer library|insert instance layer'
```

Run that second one *separately* from the round it is vouching for: the loader's
debug output buries the very messages being looked for. And DESIGN.md names the
way to confirm validation bites rather than merely loads — delete the render
pass's external subpass dependency and the same run produces ten
`SYNC-HAZARD-WRITE-AFTER-READ`.
