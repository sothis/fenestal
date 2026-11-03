# Contributing to Fenestal

Bug reports, questions, documentation fixes, portability patches, reports
from platforms the project has not run on, and new demos are all welcome.

Participants are expected to follow the [Code of Conduct](CODE_OF_CONDUCT.md).

---

## Table of Contents

- [API changes and implementation changes](#api-changes-and-implementation-changes)
- [Getting Support](#getting-support)
- [Reporting a Bug](#reporting-a-bug)
- [Reporting a Security Issue](#reporting-a-security-issue)
- [Proposing a Change](#proposing-a-change)
- [Development Setup](#development-setup)
- [Before You Open a Pull Request](#before-you-open-a-pull-request)
- [Generated Files](#generated-files)
- [Code Style](#code-style)
- [Documentation](#documentation)
- [Commit Messages](#commit-messages)
- [Licensing](#licensing)

---

## API changes and implementation changes

Fenestal is a library with a public API (`include/fenestal/`) and a binary
interface pinned against a baseline (`tools/abi/`). The API is 0.9 and not yet
stable: a minor release may rename, change or remove anything, and
`fnl_init()` refuses a header from another minor (`fenestal.h` *stability*,
DESIGN.md *Compatibility*). It is already held to what 1.0 will promise in two
ways: enum values are appended, never inserted or renumbered, and descriptors
grow only at their end.

Changing what the library does behind the API is an ordinary code change.
Changing the API -- a new function, a field, an enum value, a changed meaning
-- is reviewed as an API change; see [Proposing a Change](#proposing-a-change).
Please do not combine the two in one pull request.

## Getting Support

The documentation covers most usage questions:

| Question | Where |
|---|---|
| How do I build it and use it from my project? | [`README.md`](README.md) *Building*, *Using it from another project* |
| What does a function do, and on which thread? | the API reference, `build/api/index.html` after a build, generated from the headers' own comments |
| What does a working program look like? | [`demos/`](demos/README.md), each a folder that configures on its own, and the snippets in [`examples/`](examples/) |
| Why does it behave this way? | [`DESIGN.md`](DESIGN.md), the measurements and the alternatives rejected |
| What is known not to work, or not yet tested? | [`TODO.md`](TODO.md) |

Otherwise, open an issue. Questions the documentation should have answered are
treated as documentation bugs.

## Reporting a Bug

Open an issue and include:

1. **The version and the platform** -- `fnl_version()`, the operating system,
   the window server or compositor and its version (GNOME, KDE, weston, Xorg
   with or without a window manager, the console), and the compiler.
2. **The program's log.** Every program logs the backend it opened
   (`backend: <Name>`), the renderer it draws with (`renderer: <Name>`) and
   one line per GPU it found; that block identifies most environments. Say
   whether you named any of them (`FNL_BACKEND`, `FNL_RENDERER`, `FNL_GPU`).
3. **The GPU and its driver** -- `vulkaninfo --summary` or `glxinfo -B` on
   Linux, the driver version on Windows.
4. **How to reproduce it**, against `fenestal-testbed` or a demo where you can.
   For an input problem, run with `FNL_ECHO_EVENTS=1`: every event the program
   is handed is echoed on stderr, which shows whether the library heard the
   key or the click at all.
5. **Expected and actual behaviour.** For a drawing problem, a screenshot.

A checker tree (`./build.sh linux asan`, say) is a Debug build under a
sanitizer, and a fault it finds is usually the report.

## Reporting a Security Issue

Do not open a public issue for a memory-safety problem or a crash on input
another program controls. Report it privately, through this repository's
*Report a vulnerability* (GitHub's private vulnerability reporting) or by
email to **fenestal@mail.de**, with the details and a reproducer, and allow
time for a fix before disclosing.

The library reads bytes it does not control: the keymap a compositor sends as
text, the clipboard, X properties, xkeyboard-config's files on the console.
Those parsers are fuzzed (`tools/fuzz/`), and a finding reachable from them is
handled as a security report. [`SECURITY.md`](SECURITY.md) has the full
policy: what to include, which versions are supported, and what is in scope
and what is a property of the design.

## Proposing a Change

**Implementation changes** -- bug fixes, portability work, performance,
messages, documentation -- need no preliminaries. Open an issue first if the
change is large enough that you would rather not write it twice; otherwise send
a pull request.

**API changes** are better discussed in an issue before code is written. An API
change touches all of the following, and a pull request missing any of them
cannot be reviewed as a whole:

- the header, with a comment directly above each new `FNL_API` function and a
  thread tag (`[main]`, `[any]`, `[overlay]`)
- a caller in a program the build makes -- an example, the testbed, a demo,
  `fenestal-dev` or `tools/headercheck.c` -- since `build.sh` fails a public
  function nothing calls
- the ABI baseline in `tools/abi/`, updated in the same commit, since the build
  compares against it and fails on a difference
- `CHANGELOG.md`, for anything a caller would notice
- the `DESIGN.md` section that gives the reasoning

Some changes are a row in several places, and each is listed in one spot:

- **A key** is `FnlKey` in `fenestal.h`, `src/backend/evdev_keys.h`, the two
  tables in `win32.c`, `MAC_KEYS` in `cocoa.m`, and a name in `KEY_NAMES` in
  `src/core/echo.c`. `tools/check-tables.py` checks them after every build.
- **A new fallback path** gets a fault site, `FNL_FAULT("name")`, and a row in
  `src/core/fault.c`, so it can be forced in a Debug tree.
- **A new call into a runtime the library opens** (the Vulkan loader,
  `libEGL`, `libwayland-client`, `libxcb`) gets a row in that runtime's list
  (`vk_fns.h`, `egl_dyn.h`, `wl_dyn.h`, `xcb_dyn.h`).
- **A push-constant or instance struct** changes in the shader, the C struct
  and its `_Static_assert` together.

## Development Setup

The tree builds on Linux; Windows and macOS are cross-compiled from there.
[`README.md`](README.md) *Building* lists what each target needs: CMake 3.18 or
later, gcc or clang, the Wayland, xcb and Vulkan headers, `glslangValidator`,
`spirv-cross` and FreeType; mingw-w64 for Windows; clang-21, `lld-21` and
Apple's SDK for macOS.

```sh
./build.sh linux                  # the release tree: Wayland, X11, KMS
./build.sh linux asan             # the same under AddressSanitizer
./build.sh win64                  # Windows, cross; needs linux first
./build.sh                        # every target and variant
```

Every tree lands under `build/<target>/<variant>`, and clean is
`rm -rf build`. The cross targets borrow the `linux` release tree's baked
font, so build that first.

`tools/drive/` is the harness that drives the built programs on real
desktops, guests and machines (`tools/drive/README.md`). It needs a desktop
and one-time consents, and is not needed to contribute: the checks below are.

## Before You Open a Pull Request

CI (`.github/workflows/ci.yml`) builds every tree below, and the ARM
Linux one, on each pull request, and runs ruff; running them first saves
a round trip:

```sh
./build.sh linux                  # warnings, ABI, tables, layering, fuzz replay
./build.sh linux clang            # clang with -Werror and -Wthread-safety
./build.sh linux asan             # AddressSanitizer and UBSan
./build.sh linux tsan             # ThreadSanitizer, under tsan.supp
./build.sh linux analyzer         # gcc's -fanalyzer
./build.sh win64                  # if you touched Windows or a shared file
ruff check tools                  # if you touched the Python
```

A build that succeeds has already run most of what there is to run: the
`verifyfont` and `verifykeymap` steps, the fuzz harnesses over their corpus,
the ABI and export-list comparison, the API reference's caller check,
`tools/check-layering.py` (core names no window server or graphics API, a
backend no graphics API, a renderer no window server outside its bridges)
and `tools/check-tables.py`. The tree is warning-clean under gcc and clang
alike; please keep it so.

There is no test target. For a change in how a window behaves, say which
desktops you ran it on, and what you looked at.

## Generated Files

One generated file is tracked: `src/backend/keysym_table.h`, made by
`tools/mkkeysyms.py` from X11's `keysymdef.h`. Change the generator and run it
again; a hand edit is lost at the next run.

Everything under `build/<target>/<variant>/generated/` is made by the build
and untracked: the Wayland protocol code (`wayland-scanner`, from
`protocols/*.xml`), the SPIR-V and its translations (`glslangValidator`,
`spirv-cross`, `dxc`, from `shaders/` and `testbed/shaders/`) and the font
atlas (`tools/mkfont`). Edit the sources.

The ABI baseline in `tools/abi/` is written by hand on purpose: it changes when
the API is meant to, in the commit that changes it.

## Code Style

The surrounding file is the authority, and [`CLAUDE.md`](CLAUDE.md) *House
style* has the full list. In summary:

- **C11**, linking nothing beyond the system's own libraries. Every other
  runtime -- the Vulkan loader, `libEGL`, `libwayland-client`, `libxcb` -- is
  opened at run time, never linked. A patch that adds a library to the link
  line will be asked to open it instead, or to run it as a subprocess.
- **Tabs for indentation** in C, Objective-C and GLSL, four columns wide
  (`.editorconfig` says so to editors and to GitHub): one tab a level, and a
  continuation line tabs for every four columns and spaces for the rest. CMake,
  shell and Python indent with four spaces. Allman braces for function
  definitions and K&R inside them, 80 columns in code and in prose, a tab
  counted as four.
- `/* ... */` comments only, never `//`. **Comments explain why, not what**:
  a non-obvious choice carries the constraint that forced it and the
  alternative rejected.
- `snake_case` functions and variables, prefixed by subsystem; `PascalCase`
  types; `UPPER_SNAKE` macros. `#include "core/app.h"` first in every `.c`.
- Two tiers of error: a recorded `FnlResult` for what cannot be recovered from,
  and `bool`/NULL with one log line for what can. Nothing calls `exit()`.
- Untrusted sizes are range-checked by division on a known-good size, never by
  a multiplication that can wrap.
- British spelling in prose.
- Every source file opens with its SPDX header -- `SPDX-License-Identifier:
  MIT` and a copyright line, or `CC0-1.0` in `demos/` and `examples/`. A file
  that holds material this project did not write adds an `SPDX-FileComment:`
  line naming whose it is and pointing at `THIRD_PARTY_NOTICES.md`, because a
  licence scanner reads the tag and not the notices; `keysym_table.h` is the
  pattern.
- The Python under `tools/` is held to `ruff check tools`, clean.

`.clang-format` encodes the style, but the tree predates it: use it as a check
on new lines, not over a file.

## Documentation

`README.md` is the user's introduction: building, consuming, the API reference,
compatibility. `DESIGN.md` is the specification: the measurements, the rejected
alternatives, and the reason behind nearly every constant. A comment's
`DESIGN *Name*` names a section there, so renaming a section means moving the
comments that cite it (`grep -rn 'DESIGN \*Name'`); nothing checks it for you.

`TODO.md` is what is owed: platforms not yet tested and questions left open. A
question it lists is answered in `DESIGN.md`, with the measurement, and taken
off the list in the same change.

## Commit Messages

A subject is a statement of what is now true, behind the subsystem it is in
where there is one:

```
X11: a completion stamped before its present goes to the next refresh
Wayland: a refused present the compositor said was shown is counted
harness: the validation layer under wine; a wine Direct3D 12 resize expected
```

Common scopes: `Wayland`, `X11`, `Win32`, `Cocoa`, `KMS`, `Vulkan`, `GL`,
`D3D12`, `Metal`, `testbed`, `harness`, `docs`, `TODO`. Keep one logical
change per commit, and put the reasoning in the body when the subject cannot
carry it.

## Licensing

**There is no CLA, and you retain copyright in what you write.** By
contributing you agree that your contribution is licensed under the terms this
project already publishes for the part of the tree you are contributing to --
inbound matching outbound, in the words of the usual formula:

| What you touch | The terms your contribution is under |
|---|---|
| Source code -- C, Objective-C, GLSL, Python, shell, CMake, build files | [MIT](LICENSE) |
| Documentation -- `docs/`, and Markdown that serves as documentation | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| `demos/` and `examples/` | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) |

When adding a new file, copy the SPDX header from an existing one and add your
own copyright line.

### If your patch brings in someone else's work

`LICENSE` clause 4 and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) exist
because this project ships material it did not author: two fonts, nine Wayland
protocol definitions, X.Org's keysyms, an excerpt of xkeyboard-config's rules.
**Nothing in the build can detect that you have added to that set** -- a licence
gate would have to know where a file came from -- so it is a review question,
and this section is where it is asked.

* **A Wayland protocol.** Copy the XML from wayland-protocols unmodified, with
  its `<copyright>` element, and add its row to Part 1's table.
* **A font, or anything baked into the binary from one.** An entry in Part 1,
  and, where the licence requires its text to travel with every copy (the
  Ubuntu Font Licence does), that text built in beside it, as
  `fnl_font_licence()` is.
* **A runtime put beside the programs** -- a library a tree carries, as
  MoltenVK is. An entry in Part 2, and its licence file beside it in the tree.
* **Code adapted from a published implementation**, rather than written from a
  specification or documentation. Credit it in the file's header, with an
  `SPDX-FileComment:`, and add it to Part 1 even when the terms demand nothing.
* **A new fuzz input taken from another project's files.** A line under
  Part 1's corpus section.

If you are unsure whether something counts, say so in the pull request. An
unrecorded dependency is a great deal more expensive to find later than an
unnecessary paragraph is to delete.
