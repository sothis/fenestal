# Third-party notices

This file has two parts. **Part 1** is the material in this repository that
this project did not author: what it is, where it ends up, and under which
licence. **Part 2** is what a build brings in from outside the repository --
code compiled in from the build machine, and libraries put beside the
programs -- and what stays out of every copy.

The sections of Part 1 are the authoritative enumeration. Prose that
summarises them, here or in [`LICENSE`](LICENSE), says "and everything else
in Part 1" rather than reciting them, because a recited list goes stale.

The grants in [`LICENSE`](LICENSE) clauses 1 to 3 -- MIT for the code, CC BY
4.0 for the documentation, CC0 for `demos/` and `examples/` -- cover **this
project's own contribution**. They do not, and cannot, grant rights in what
is recorded here; see `LICENSE` clause 4. Anyone redistributing Fenestal, in
source or binary form, carries this file with it.

---

# Part 1 -- In this repository

## 1.1 Ubuntu Sans and Ubuntu Sans Mono -- `fonts/ubuntu/`

| | |
|---|---|
| Files | `UbuntuSans-Regular.ttf` (1.006), `UbuntuSansMono-Regular.ttf` (1.100) |
| From | Canonical's own releases |
| Copyright | Copyright 2011, 2022, 2023 Canonical Ltd. (`fonts/ubuntu/COPYRIGHT.txt`) |
| Licence | Ubuntu Font Licence 1.0 (`fonts/ubuntu/LICENCE.txt`) |
| Ends up in | every binary: `tools/mkfont` bakes a glyph atlas from both faces at build time -- Fenestal Glyphs Sans and Mono -- and the atlas is compiled into the library |

The atlas is a Modified Version of the font software, and stays under the
Ubuntu Font Licence inside the binary that carries it; the rest of that
binary is not covered by it. Condition 1 of the licence has every copy carry
the notice and the licence, so both are built into the library beside the
atlas: a program reaches them through `fnl_font_notice()` and
`fnl_font_licence()`, the testbed prints them with `--licence`, and
`examples/hello.c` shows a program doing the same. A program that ships
Fenestal should offer them likewise.

The licence is neither OSI-approved nor DFSG-free. Its condition 2 governs
the *name* of a Modified Version, and the atlas is renamed as its condition
2(b) asks of one Substantially Changed -- a fixed-size coverage subset with
no outlines: **Fenestal Glyphs Sans**, from Ubuntu Sans, and **Fenestal
Glyphs Mono**, from Ubuntu Sans Mono. The name is stated in
`fonts/ubuntu/COPYRIGHT.txt`, the notice built into every binary, so it
travels with every copy; nothing calls the atlas by either face's name, and
"baked from" the faces is acknowledgement, which condition 3 allows.
DESIGN.md *Licence* has the reasoning.

## 1.2 Wayland protocol definitions -- `protocols/`

Copied from wayland-protocols, each file unmodified and carrying its own
notice in its `<copyright>` element. `wayland-scanner` generates C from them
in every Linux tree, and that code -- the interface tables and the request
stubs -- is compiled into the library, with the notice copied into each
generated file.

| File | Copyright | Licence |
|---|---|---|
| `cursor-shape-v1.xml` | 2018 The Chromium Authors; 2023 Simon Ser | MIT |
| `fractional-scale-v1.xml` | 2022 Kenny Levinsen | MIT |
| `pointer-constraints-unstable-v1.xml` | 2014 Jonas Ådahl; 2015 Red Hat Inc. | MIT |
| `presentation-time.xml` | 2013-2014 Collabora, Ltd. | MIT |
| `relative-pointer-unstable-v1.xml` | 2014 Jonas Ådahl; 2015 Red Hat Inc. | MIT |
| `text-input-unstable-v3.xml` | 2012, 2013 Intel Corporation; 2015, 2016 Jan Arne Petersen; 2017, 2018 Red Hat, Inc.; 2018 Purism SPC | HPND-sell-variant |
| `viewporter.xml` | 2013-2016 Collabora, Ltd. | MIT |
| `xdg-decoration-unstable-v1.xml` | 2018 Simon Ser | MIT |
| `xdg-shell.xml` | 2008-2013 Kristian Høgsberg; 2013 Rafael Antognolli; 2013 Jasper St. Pierre; 2010-2013 Intel Corporation; 2015-2017 Samsung Electronics Co., Ltd; 2015-2017 Red Hat Inc. | MIT |

The full text of each notice is the file's own; this table is a summary of
it.

## 1.3 The keysym table -- `src/backend/keysym_table.h`

`tools/mkkeysyms.py` generates the table from X.Org's `keysymdef.h`: each
keysym's name, its value and the code point it stands for. The generator and
the table's shape are this project's; the correspondence is X.Org's, and
travels under `keysymdef.h`'s notice, reproduced in full in
`src/backend/keysym_notice.txt` beside it -- The Open Group's (Copyright
1987, 1994, 1998) and Digital Equipment Corporation's (Copyright 1987), both
permissive. The table's values are compiled into every Linux binary, and its
names into the XKB reader that Wayland and KMS share (`keymap_xkb.c`). The
table's header carries an
`SPDX-FileComment` saying so, for a scanner that reads the tag and not this
file.

## 1.4 The fuzz corpus -- `tools/fuzz/corpus/`

Inputs the fuzz harnesses replay on every native build; nothing compiles
them into a binary.

- `rules/evdev-small.rules` is an excerpt of xkeyboard-config's
  `rules/evdev`, cut to one row of each shape the file has, under
  xkeyboard-config's own permissive notices (its `COPYING`).
- `rules/console.conf` is a console keyboard configuration in the format of
  Debian's `keyboard(5)`, header comment included: a handful of variable
  assignments, recorded here for completeness.

The other corpus files were written for the harnesses.

---

# Part 2 -- Outside this repository

## 2.1 Compiled in from the build machine

- **Wayland's core protocol.** Every Linux tree generates the core
  protocol's interface objects from the build machine's installed
  `wayland.xml` and compiles them into the library, whose own they are in
  place of `libwayland-client`'s. That file is Copyright
  2008-2011 Kristian Høgsberg, 2010-2011 Intel Corporation and 2012-2013
  Collabora, Ltd., under the MIT licence, and its notice is copied into the
  generated file.
- **Tool output.** `glslangValidator`, `spirv-cross` and Microsoft's `dxc`
  translate this project's own shaders; FreeType, in `tools/mkfont`,
  rasterises the fonts of 1.1. What they produce is this project's shaders
  and the fonts' glyphs, under clause 1 and 1.1 respectively; none of those
  tools is linked into anything the build makes.

## 2.2 Beside the programs

- **MoltenVK** (Apache-2.0). `tools/moltenvk.py` fetches one pinned release,
  and `build.sh` puts `libMoltenVK.dylib` into every macOS tree beside the
  programs, with its licence as `MoltenVK-LICENSE.txt`. The library opens it
  at run time; nothing links it.
- **The Vulkan loader and Khronos' validation layer for macOS** (Apache-2.0).
  `tools/vulkan_macos.py` builds them from pinned sources; the harness puts
  them beside a program for a checker round on the Mac, with their licence
  as `Vulkan-LICENSE.txt`, and never into a tree.

## 2.3 Never in a copy

- **vkd3d-proton** (LGPL-2.1) and **DXVK's `dxgi.dll`** (zlib), fetched by
  `tools/vkd3d_proton.py`, are staged under `build/drive/` for the wine
  rounds alone and never put into a tree, where on Windows they would shadow
  the system's.
- **Apple's macOS SDK**, which the macOS trees are compiled against, is
  copied from a Mac by `tools/macos_sdk.py` into
  `~/.local/state/fenestal/` and is never packed or redistributed.

Neither source archive (`build/fenestal.zip`, `fenestal-commented.zip`)
contains anything from Part 2: both are made from this repository's files
alone.
