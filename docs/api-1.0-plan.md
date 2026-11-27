# API/ABI 1.0.0 plan

Consolidated on 2026-11-26 from the three parallel reviews of the same
question -- two Claude Code sessions and one Pi session, all against
`2d0862b` -- and verified item by item against the tree at that commit.
Nothing was built or run for this plan; the facts below are read from the
headers, the ABI tooling, the generated `build/linux/release/generated/`
dump, the sources, `DESIGN.md`, `README.md`, `TODO.md` and the CI file.

The verdict all three reached is the same: **the declared API is close to
complete, but the ABI baseline cannot yet back a 1.0 promise.** There are
four blockers -- a stale `abidump`, versioning rules that are prose rather
than mechanism, a Vulkan seam that cannot request anything, and `UNSTABLE`
struct fields that 1.0 would freeze -- and a tail-padding trap that makes a
future field addition silent. The rest is documentation, small API
decisions and verification debt.

Item IDs (`A`, `B`, `C`, `D`, `E`) are used by the implementation plan at
the end.

---

## 1. Consolidated items

### A. ABI baseline and checks

**A1. `abidump` misses three `FnlInitDesc` fields.** `content_is_ui`
(`fenestal.h:395`), `no_titlebar_controls` (`:406`) and `no_gpu_ranking`
(`:416`) are in the struct but not in `tools/abidump.c:60-78`, which dumps
18 of the struct's 21 fields. They sit at offsets 96/100/104, inside what
used to be tail padding, so `sizeof` stays 112 and `want_color` stays 108:
the size check cannot see them. The committed `tools/abi/fenestal-0.abi`
and the generated `build/linux/release/generated/fenestal.abi` are
byte-identical and both lack them. A build today is green with three public
fields outside the contract. This is the most dangerous item.

**A2. `abidump` misses public constants:** `FNL_TITLEBAR_*` and
`FNL_TITLEBAR_ALL` (`fenestal.h:158-163`), `FNL_IMAGE_RGBA8` and
`FNL_IMAGE_FORMAT_COUNT` (`:1514-1517`), `FNL_KEY_VOLUME_DOWN/UP` and
`FNL_KEY_MUTE` (`:690-692`), and `FNL_EVENT_COLOR` (`:787`). None appears
in any `.abi` file.

**A3. `abidump` misses `FnlEvent.presence.x/.y`.** The union member exists
at `fenestal.h:982`; no `.abi` has a `presence` line.

**A4. The named event payload structs are not pinned as types.** `FnlKeyEvent`
through `FnlGpuEvent` (`fenestal.h:795-941`) are represented only by their
union offsets inside `FnlEvent`; their own `sizeof` and offsets are not
dumped.

**A5. No field types and no function signatures are pinned.** `.abi` records
offsets, sizes and enum values; `.syms` records names only. A field changing
`float` to `int`, or a function parameter changing `int` to `double`, keeps
the symbol and the offset and silently breaks every compiled caller.

**A6. The baseline filenames are hardwired to major 0.**
`CMakeLists.txt:1403-1423` names `fenestal-0*`; the soname (`:2052-2060`)
and the `fnl_init()` check (`app.c:3184-3190`) switch automatically at major
1, the baseline selection does not. Correction: `tools/abidump.c:59` prints
`FNL_VERSION_MAJOR`, not a hardcoded 0; only the filenames and the
committed `major 0` line are fixed.

**A7. `check.cmake` cannot enforce "additions only".** It does an exact
`compare_files` (`tools/abi/check.cmake:16-25`); the documented remedy is to
copy the dump over the baseline, which blesses a removal or a moved offset
as readily as an addition.

**A8. `fenestal.map` cannot express per-minor nodes.** It is one node,
`FENESTAL_0`, with `global: fnl_*;` (`fenestal.map:13-20`). A wildcard in
the newest node moves every symbol to it; a wildcard in the oldest puts
additions in the wrong node. Nothing verifies the built `.so`'s version
information either.

**A9. After 1.0 the header version check compares major only.**
`app.c:3184` uses `vshift = 16` when the major is nonzero, so a 1.3 header
passes a 1.1 library. Symbol nodes would catch this on ELF, but Windows,
macOS and static builds have none.

**A10. The tail-padding trap.** `FnlWindowDesc` ends with `frame_times` at
offset 68 and `sizeof` 72, so a new `bool` appended after it lands at 69,
`sizeof` stays 72, and an older caller's indeterminate padding is copied
into the new field. `FnlInitDesc` is exactly full at 112 today, so its next
append grows; `FnlWindowDesc` is the live trap. A1 is the same class of
problem already realised.

**A11. The growth policy is undocumented and inconsistent.** The stability
block names only `FnlInitDesc`, `FnlWindowDesc` and `FnlCallbacks`
(`fenestal.h:45-47`). `FnlFrameStats` has `size`; `FnlFrameSample` and
`FnlFrameShown` have `stride`; `FnlEvent` is fixed at 40/48; `FnlFrame` is
library-owned and can grow; `FnlMode`, `FnlOutputColor`, `FnlOverlayCounts`
and the interop `Fnl*Gpu`/`*SceneLayout`/`*Scene` structs have neither
`size` nor `stride` and are frozen at 1.0.

**A12. `FnlEvent`'s growth instruction contradicts 1.0.**
`fenestal.h:989-995` says "append to the union, bump `FNL_VERSION_MINOR`,
update this and the ABI baseline". At 1.0 a minor bump cannot grow
caller-allocated storage; a new payload must fit the union or be fetched by
length and getter, as compose and clipboard are.

**A13. The release mechanics are only prose.** Bumping the version,
renaming the baselines, renaming the map node and rewriting the stability
block in the header, README and DESIGN are not written down as a sequence.

**A14. No check that every public field and constant is dumped.** This is
why A1 happened. A text checker comparing the public headers' structs and
enums against `abidump.c`'s `S`/`F`/`E` calls would have caught A1-A3.

**A15. The D3D12/Metal layout baselines have no native check in CI.** The
macOS and win64-msvc CI trees are cross builds, which skip the layout
dump, and the Linux job's native tree checks the Vulkan set alone. The
D3D12 baselines were dumped under wine, and Apple's compiler layout is
checked nowhere.

**A16. `VK_EXT_memory_budget` is tested but never enabled.**
`device.c:1096-1098` sets `memory_budget` from `device_has_extension()`,
but the name never reaches the `exts[]` passed to `vkCreateDevice`;
`vk_gpu_memory()` (`device.c:1969-1985`) then chains
`VkPhysicalDeviceMemoryBudgetPropertiesEXT`. The metrics panel's VRAM
figure depends on an extension that is not enabled.

### B. API surface

**B1. The Vulkan seam cannot ask for anything.** The instance is created at
`VK_API_VERSION_1_0` (`device.c:242-247`); the device enables only
`dualSrcBlend` plus the library's own optional chains (`device.c:917-1078`).
A scene cannot request core features (anisotropy, geometry or tessellation,
fill mode, depth clamp), device extensions (timeline semaphores, dynamic
rendering, buffer device address, descriptor indexing), instance extensions,
or a newer API version, and cannot query what was enabled. This is the one
functional gap that would make 1.0 restrictive for real scenes.

**B2. Application identity is the library's.** Wayland sets
`app_id = "fenestal"` (`wayland.c:2713`), X11 writes `WM_CLASS
"fenestal\0Fenestal"` (`x11.c:1588`), Vulkan sets `pApplicationName =
"fenestal"` (`device.c:246`). Every program shares one desktop-file and
taskbar identity. There is no window icon API. Correction: an X11
`WM_CLASS` writer exists; it is hardcoded, not absent.

**B3. There is no window icon API.** A companion gap to B2. Deferred; see
the post-1.0 backlog.

**B4. Window position is asymmetric and has a sentinel problem.**
`FnlInitDesc` has no x/y; `FnlWindowDesc` does; there is no runtime move;
and 0/0 means "let the window server place", so the origin is
unrepresentable. The backends are not consistent either: X11 creates at
0/0, Win32 uses `CW_USEDEFAULT`, Cocoa centres.

**B5. Output-list changes have no event.** `app_publish_outputs()`
(`app.c:731-760`) diffs the list and bumps `display_gen`, but
`events_post_state()`'s `d_output` (`events.c:327-329`) compares only the
current output's name and size. `fnl_output_count()`'s doc promises a
monitor plugged in or out joins or leaves the list, but nothing says so.
The GPU list got `FNL_EVENT_GPU`; outputs did not.

**B6. There is no `FnlBackend` enum.** `fnl_backend_name()` says "For
showing rather than branching on; the enum is for that"
(`fenestal.h:2187-2198`), but only `FnlRenderer` exists. A caller that must
distinguish KMS (the window is the output) or Wayland (no placement)
string-compares a name it is told not to branch on.

**B7. `UNSTABLE` fields would be frozen into 1.0.**
`FnlInitDesc::late_latch_fixed` (`:281`), `scale_override120` (`:287`) and
`gpu_index` (`:304`); `FnlWindowDesc::gpu_index` (`:468`) and
`scale_override120` (`:479`). Their own policy says they may change meaning
or stop doing anything in any release, including a patch. Correction:
`late_latch_fixed` is only in `FnlInitDesc`; the other two are in both.

**B8. The per-window log sink and level are process statics.** Both are in
`error.c:39-44`; `fnl_init()` writes them from the descriptor
(`app.c:3200`). A second `fnl_init()` on a second host silently re-points
the first host's sink and level; `fnl_window_create()` takes neither.

**B9. There is no general minimum-size or aspect constraint.**
`fnl_min_content_size_pt()` is advisory and constant; only
`fnl_titlebar_custom(min_width_pt)` gives a caller any floor. A gap, but
additive later if documented as out of 1.0.

**B10. There are no window flags.** No non-resizable, maximum size,
always-on-top, opacity or attention.

**B11. There is no `FNL_CURSOR_NONE`, and no capture without a click.**
`fnl_set_cursor` says there is no `FNL_CURSOR_NONE` (`fenestal.h:2133`);
`fnl_release_pointer()` exists but nothing takes the pointer except a click
in the presentable area.

**B12. There is no `fnl_cursor_name()`/`fnl_cursor_from_name()`** beside
the key pair, so a caller saving a cursor preference has to hand-roll it.

**B13. There is no work-area exposure.** `fnl_output_size_pt()` is the full
output; the work area the library uses internally (`_NET_WORKAREA`,
`MONITORINFO::rcWork`) is not exposed.

**B14. Per-frame data is push-constant-only and `FnlFrame` carries no frame
slot or image index.** This is deliberate: DESIGN *Using it from another
project* says "128 bytes is all a device must offer", and `fnl_vulkan.h`
says memory written later is read by every recording. It is a
documentation gap in the header, not a code gap.

**B15. Multi-window's open semantics are frozen with it.** One renderer per
host; GPU moves refused while two windows are open; a window created during
a renderer move is refused; shared pipeline cache; host-level colour; no
per-window `want_color`; no runtime position. Keep, and document the limits.

**B16. TAAU's motion is one uniform vector.** A per-pixel motion texture is
the likely growth path for MetalFX and FSR 2. Freeze the vector as the 1.0
contract; a per-pixel seam must be a new API, not a change.

**B17. HDR has never been seen on a panel.** `TODO.md` *HDR* lists the
unverified items; schenker's Mac panel is the one known extended-range
screen. Freezing it now freezes untested semantics.

**B18. Naming drift.** Getters are `fnl_vsync`, `fnl_antialias`,
`fnl_scene_motion` and `fnl_metrics_panel` beside `fnl_is_fullscreen` and
`fnl_is_text_input`; setters are `fnl_set_*` beside `fnl_request_*`;
`FnlMode` is the only `typedef struct Tag {}`. A decision, not a defect.

**B19. `FNL_ERR_DEVICE_LOST` is never returned by a public entry point.**
All uses are internal renderer and swapchain paths; the public `FnlResult`
enum exposes an internal code. Keep the value (it is pinned) and document
it as internal and reserved.

**B20. `fnl_vulkan_queue_submit()` returns `VkResult`** while every other
fallible call returns `FnlResult`; a caller cannot pass it to
`fnl_result_str()`. It is the right result for a Vulkan interop call;
document that it is not an `FnlResult` rather than change it.

**B21. `fnl_renderer()`'s doc does not cover a window with no renderer
yet.** It says "With no window...", but the code returns `DEFAULT` for a
window whose renderer is not built (`app.c:2529-2537`).

**B22. Repeat-rate wording.** `FnlKeyEvent::repeat` says "at the rate the
desktop is configured for"; Wayland reads `wl_keyboard`'s rate and period,
X11 uses the server's repeat, Win32 and Cocoa the system's, KMS the
kernel's. "The system's rate" is the accurate wording.

**B23. `fnl_output_name()`'s paragraph contradicts `fnl_set_vsync()`.**
"A window is always vsynced" at `fenestal.h:2170-2175`, with
`fnl_set_vsync()` at `:2184`.

**B24. `FnlInitDesc::gpu_name` is a 40-line essay that narrates history.**
"used to be measured... now opens on the automatic pick"
(`fenestal.h:306-354`), against the house rule of no history narration.

**B25. D3D12 devices are created at feature level 11_0.** All four
`D3D12CreateDevice` calls (`d3d12/device.c:147,171,248,459`) use
`D3D_FEATURE_LEVEL_11_0`. A caller's scene needing 12_0 or 12_1 features is
outside the contract, and `fnl_d3d12.h` does not say so. This is the D3D12
analogue of B1, found during verification.

**B26. The Metal capability contract is not stated.** Metal is the chosen
device; the interop header should say what guarantees a scene has.

### C. The stability promise

**C1. The block promises too little and says nothing about several
interfaces.** It lacks: source compatibility and the rule that removals
only follow `FNL_DEPRECATED` and only at a major; key-name strings for
config files; which `FNL_*` environment variables are contract and which
are diagnostics; descriptor strings copied and truncated (`title`,
`gpu_name`, the new app fields); the `UNSTABLE` twins in `FnlWindowDesc`;
and what 1.0 is *not*.

**C2. There is no non-goals list.** Add one: no touch, pen or gamepad; no
window icons (unless B3 is taken); no always-on-top, opacity or attention;
no runtime move or size limits; no clipboard images or drag and drop; no
popups or dialogs; no 32-bit targets; fonts are the baked faces only.

**C3. `FnlWindowDesc`'s `UNSTABLE` twins lack the tags their `FnlInitDesc`
counterparts carry.** Resolved by the B7 decision.

### D. Verification debt a 1.0 tag would freeze in

**D1. HDR** has not been seen on a real panel; TODO *HDR* has the list.

**D2. The 175% atlas rung** has not been baked since it was added (TODO
*The 175% rung*).

**D3. The Wayland two-window round** is listed as owed, but the ledger has
a passing `wayland-vulkan` round at `5c1ce69` and the log has second-window
lines. Verify before treating it as owed.

**D4. The D3D12 and Metal layout baselines** have no native macOS check
(A15).

**D5. TAAU's history** is two presents old and wants a panning measurement
(TODO *Temporal upscaling*).

**D6. MSVC warnings** have never been gone over; the tree builds at `/W3`
and is not held to `/WX`.

**D7. The full round and checker matrix** has not been run on one revision
as a 1.0 gate.

### E. Dismissed or corrected findings

**E1. `fnl_min_content_size_pt` "changes over a window's life" is false.**
`src/core/api_state.c:172-180` returns the constants `CONTENT_MIN_W/H` and
ignores the app; `deco_min_w` (`decor.c:72-80`) is the window's floor, a
different figure. The header already says it is advisory and "not the size
the window refuses to go below" (`fenestal.h:2071-2081`). No change.

**E2. "There is no X11 `WM_CLASS` writer" is false.**
`src/backend/x11/x11.c:1588-1591` writes `"fenestal\0Fenestal"`. The
identity gap is real; the writer is hardcoded.

**E3. "`abidump` prints major 0" is false.** `tools/abidump.c:59` prints
`FNL_VERSION_MAJOR`; only the CMake filenames and the committed line are
fixed.

**E4. "The header has 20 `FnlInitDesc` fields" is off by one.** It has 21
(`int width_pt, height_pt` is two); the dump has 18.

**E5. "The `UNSTABLE` fields are in both descriptors" is partly wrong.**
`late_latch_fixed` is only in `FnlInitDesc`.

**E6. "The Wayland two-window round has not run" may be stale.**
`tools/drive/stress.py:4444` runs `two_window()` in the round, and the
latest `wayland-vulkan` log (23:13 on `5c1ce69`) contains second-window
lines. Verify the ledger first.

**E7. `fnl_renderer()`'s item is a doc gap, not a code defect.** The
fallback is correct; only the sentence is incomplete.

---

## 2. Decisions before executing

**D1 (fixture fields).** Remove `late_latch_fixed`, `scale_override120` and
`gpu_index` from the public descriptors and keep the behaviour as
diagnostics: `FNL_SCALE120`, `FNL_LATE_LATCH_MS`, and `FNL_GPU`'s numeric
form. Update `testbed/`, `tools/drive/pace.c` and
`tools/drive/lifecycle.c`. If minimal churn is preferred, keep the fields
and mark both descriptors' twins `UNSTABLE`, but 1.0 then freezes test
fixtures.

**D2 (Vulkan seam).** Add `FnlVulkanSetup` -- extension lists, core
features and a feature-chain pointer -- raise the instance and device API
version to the highest the loader supports, expose `api_version` on
`FnlVulkanGpu`, and fix `VK_EXT_memory_budget`.

**D3 (naming).** Keep the names and document the convention (`set_`
changes a setting; `request_` asks the server and may be refused), and make
`FnlMode` an anonymous typedef. A full rename is 41 call sites and comments
with no functional gain; if uniform `fnl_set_*` is wanted, it must be done
now, because it is permanent after 1.0.

**D4 (1.0 scope).** In 1.0: app identity, `FnlBackend` plus `fnl_backend`,
the output-list event, position at creation with a `positioned` flag, the
Vulkan setup and the D3D12 highest feature level. Deferred and additive in
1.x: window icon, cursor names, `FNL_CURSOR_NONE` and capture, minimum and
maximum size, always-on-top, work area, runtime move.

**D5 (HDR).** Verify on schenker's Mac panel before the tag. If it cannot
be verified, mark HDR experimental in the header and README rather than
pretending the semantics are proven.

**D6 (multi-window).** Keep it in 1.0 with the limits written down: one
renderer, no GPU move while two are open, host colour, no per-window log
sink.

**D7 (TAAU).** Freeze the uniform vector as the 1.0 contract; a per-pixel
motion seam is a new API in 1.x.

**D8 (log sink).** Document one process, one sink and one level; do not
move it per host now.

**D9 (D3D12).** Try the highest feature level the adapter supports and
expose it, falling back to 11_0. If that is more Windows testing than is
wanted, document 11_0 as the 1.0 contract.

**D10 (`VkResult` and `FNL_ERR_DEVICE_LOST`).** Keep both; document that
the former is a `VkResult`, not an `FnlResult`, and the latter is internal
and reserved.

---

## 3. Implementation plan

### Phase 0 -- make the ABI baseline describe the headers

No API decision is needed for this phase; it should land first.

1. **`tools/abidump.c`**
   - add the three `FnlInitDesc` fields (A1);
   - add `FNL_TITLEBAR_*`, `FNL_TITLEBAR_ALL`, `FNL_IMAGE_RGBA8`,
     `FNL_IMAGE_FORMAT_COUNT`, `FNL_KEY_VOLUME_DOWN/UP`, `FNL_KEY_MUTE`
     and `FNL_EVENT_COLOR` (A2);
   - add `FnlEvent.presence.x/.y` (A3);
   - add `S(...)` and `F(...)` for every named event payload struct
     `FnlKeyEvent` through `FnlGpuEvent` (A4);
   - update the file's header comment: a missed field no longer always
     fails on its size.
2. **Add `tools/check-abidump.py`** -- parse the public headers'
   `typedef struct` and `typedef enum` bodies (flattening `FnlEvent`'s
   anonymous union) and compare them with `abidump.c`'s `S`/`F`/`E` calls.
   Fail on any public field or constant that is not dumped, or any dump
   line that names something not in the headers. Run it from `build.sh`
   beside `check-layering.py` and `check-tables.py` (A14).
3. **Add `tools/abi/types.c`** -- one `_Static_assert(_Generic(...))` per
   public function (exact prototype) and per public struct field (exact
   type), including the nested event payloads and the interop structs.
   Compile it in the C-only part of the header check
   (`CMakeLists.txt:1310-1355`), not in the C++ or Objective-C runs (A5).
4. **Rewrite `tools/abi/check.cmake`**
   - read the `major N` line; for the same major of 1 or more, every
     baseline `sizeof`, offset and enum line must still exist with the
     same value;
   - a `sizeof` may grow, but every new offset must be at or after that
     struct's **old** `sizeof` (this rejects padding insertion);
   - a new struct or enum constant is allowed; a `_COUNT` enum may grow;
     a removed or renamed line fails;
   - every baseline `.syms` line must still exist; new names are allowed;
   - major 0 keeps today's exact compare and copy workflow;
   - a larger current major expects new baseline files and fails with
     "create `fenestal-1*`" if they are missing;
   - update the message to say "from 1.0 an addition only" (A7).
5. **CMake baseline selection** -- replace the literal `fenestal-0`
   prefixes at `CMakeLists.txt:1403-1423` with
   `fenestal-${PROJECT_VERSION_MAJOR}`, and fail at configure if the
   selected files do not exist (A6).
6. **`fenestal.map`** -- at 1.0 replace the wildcard with an explicit
   `FENESTAL_1_0 { global: <every FNL_API name, all headers>; local: *; };`.
   Add `tools/check-version-map.py` to prove every `FNL_API` name appears
   exactly once in the map and nothing else does. Add an ELF-only
   `build.sh` step that reads the built `.so`
   (`readelf --dyn-syms --wide`) and checks every exported `fnl_*` carries
   the expected version node. Document that Windows and macOS rely on the
   `.def` or visibility and the `fnl_init()` check (A8).
7. **Version check** -- in `fnl_init()` and `fnl_window_create()`, replace
   the major-only shift at 1.0 with: the major must equal; for major 0 the
   minor must equal; for major 1 or more the header minor must be at most
   the library minor. The patch is ignored. Update both comments (A9).
8. **Growth rules** -- add the rule to the stability block: an appended
   field must make `sizeof` larger; a field placed in old tail padding is
   an ABI change, not an addition, and `check.cmake` enforces it. Add the
   trailing `positioned` field to `FnlWindowDesc` at offset 72 (the first
   real append; see Phase 1) so the current 3 bytes of padding are no
   longer a trap. Document per-struct growth (A11) and rewrite
   `FnlEvent`'s growth instruction (A12).
9. **`VK_EXT_memory_budget`** -- add
   `VK_EXT_MEMORY_BUDGET_EXTENSION_NAME` to the device extension list in
   `create_device()` when offered and there is room; keep
   `memory_budget` true only when enabled (A16).
10. **Regenerate the baselines** for the four layouts and five export
    sets (commands in section 5), then build the native trees; the exact
    compare passes with the new files.

Deliverable: a green build where every public field, constant and
function is pinned, and a check that fails if it happens again.

### Phase 1 -- API surface changes (still 0.9, source-breaking allowed)

11. **D1: remove the `UNSTABLE` fields** (if chosen). Remove
    `late_latch_fixed`, `scale_override120` and `gpu_index` from
    `FnlInitDesc`; remove `gpu_index` and `scale_override120` from
    `FnlWindowDesc`. Read `FNL_LATE_LATCH_MS` and `FNL_SCALE120` in
    `fnl_init()` and `fnl_window_create()`; read `FNL_GPU` for the numeric
    index in `gpu_choice()`. Update `testbed/main.c` (`--late-latch`,
    `--scale`, `--gpu`), `testbed/bench.c`, `tools/drive/pace.c` and
    `tools/drive/lifecycle.c`; add the two variables to README's table as
    diagnostics. Remove the `UNSTABLE` paragraph from the stability block
    and note that a second window takes the host's scale and GPU (B7).
12. **App identity** (B2) -- add to the end of `FnlInitDesc`:
    `const char *app_id;` and `const char *app_name;`. Copy them into
    `Host` (128-byte buffers, UTF-8-safe truncation like
    `app_title_copy()`); defaults are `"fenestal"` and `"Fenestal"`. Use
    them for Wayland's `xdg_toplevel_set_app_id()`, X11's `WM_CLASS`
    (`app_id\0app_name`), Vulkan's `pApplicationName` with
    `pEngineName = "Fenestal"` and `pEngineVersion = FNL_VERSION`, and
    Win32's process AppUserModelID through a dynamically loaded
    `shell32.dll` (no new link line). Cocoa and KMS have no runtime
    identity; document that. A second window uses the host's identity;
    `FnlWindowDesc` has none.
13. **`FnlBackend` and `fnl_backend()`** (B6) -- add
    `FNL_BACKEND_DEFAULT`, `FNL_BACKEND_WAYLAND`, `FNL_BACKEND_X11`,
    `FNL_BACKEND_KMS`, `FNL_BACKEND_WIN32` and `FNL_BACKEND_COCOA` with
    values matching `BackendId` (1-5), `_Static_assert`s in `backend.h`,
    and `FNL_API FnlBackend fnl_backend(const FnlApp *)` returning
    `BACKEND(a)->id` or `DEFAULT`. Call it in `examples/events.c` and
    `tools/headercheck.c`. Keep `fnl_backend_name()`.
14. **Output-list event** (B5) -- add `FNL_EVENT_OUTPUT_LIST = 24`,
    `FnlOutputListEvent { int count; }` and the union member
    `output_list`. Add `FnlAtomicU32 out_list_gen` to `App`, bump it in
    `app_publish_outputs()` when the list changes, seed
    `ev_last_out_list_gen`, diff it in `events_post_state()` and post the
    event. Add the echo case in `src/core/echo.c` (`outputs %d`) so
    `check-tables.py` passes; handle it in `examples/events.c` and
    `testbed/bench.c` if its switch is exhaustive.
15. **Position at creation** (B4) -- append `bool positioned` to
    `FnlWindowDesc` at offset 72; append `int x_pt, y_pt; bool positioned;`
    to `FnlInitDesc`. Add `App::init_positioned`, set it from both
    descriptors, and replace the backends' `init_x_pt != 0 ||
    init_y_pt != 0` tests (`win32.c:2592`, `cocoa.m:1785`, and X11's
    unconditional create) with `init_positioned`. Update
    `testbed/main.c:1073` to set `positioned = true`. Document that 0/0
    is a valid position when `positioned` and that Wayland and KMS ignore
    it.
16. **Vulkan setup** (B1) -- in `fnl_vulkan.h`:
    `FnlVulkanSetup` with `size`, `api_version`, `instance_extensions` and
    `instance_extension_count`, `instance_pnext`, `device_extensions` and
    `device_extension_count`, `features`, and `features2`.
    `FnlInitDesc` gains `const struct FnlVulkanSetup *vulkan_setup;`.
    `Host` stores it as `const void *renderer_setup`; core never reads it.
    In `make_instance_locked()`: append the instance extensions, link
    `instance_pnext` into the chain, and use `api_version` or the highest
    `vkEnumerateInstanceVersion()` result capped at 1.3. In
    `create_device()`: append the device extensions; OR `*features` into
    `want` and keep `dualSrcBlend`; link the internal feature chain's tail
    to `features2`. Reject a `VkPhysicalDeviceFeatures2` in `features2`
    with a clear line (use `features` for core features). Log and fail
    with the extension or feature name on an unsupported request.
    Document that the setup and everything it points at must outlive the
    host and is read on every device build. Add `uint32_t api_version` to
    `FnlVulkanGpu` (the instance's) and document the library's base device
    extensions in `fnl_vulkan.h`. Add a small setup to
    `examples/scene_vulkan.c`, for example anisotropy and a timeline
    semaphore feature chain, so the reference shows it.
17. **D3D12 feature level** (B25, if D9 says yes) -- factor the four
    `D3D12CreateDevice` calls through a helper that tries 12_2, 12_1,
    12_0, 11_1 and 11_0 and returns the level; add
    `D3D_FEATURE_LEVEL feature_level` to `FnlD3D12Gpu`; document it. If
    not, add "created at 11_0" to `fnl_d3d12.h`.
18. **Documentation and promise** (C1, C2, B14-B24, B26) -- rewrite the
    header's stability block with the 1.0 source and binary promise, the
    deprecation rule, the enum and struct growth rules, the `FnlFrame`
    append rule, the fixed `FnlEvent`, which structs are frozen, key-name
    stability, contract versus diagnostic environment variables,
    truncation of `title`, `gpu_name`, `app_id` and `app_name`, the
    64-bit platform scope, and the non-goals list. Reword the repeat
    rate, the vsync paragraph, `fnl_renderer()`'s no-renderer case,
    `FNL_ERR_DEVICE_LOST`, `fnl_vulkan_queue_submit()`,
    `fnl_set_log_level()`'s process scope and `gpu_name`'s essay (state
    the current rules; move history to DESIGN if it is load-bearing). Add
    the multi-window limits, the TAAU uniform-vector note, the HDR
    experimental note if D5 cannot verify it, the push-constant-only note,
    and the Metal and D3D12 capability notes. Make `FnlMode` an
    anonymous typedef. Update README *Compatibility*, DESIGN
    *Compatibility* and the `CHANGELOG.md` Unreleased entries.
19. **Update every consumer** -- examples, testbed, demos, `pace.c`,
    `lifecycle.c`, `headercheck.c`, `abidump.c`, the type manifest and
    the `.syms`/`.abi` baselines. Run `mkapidoc.py --check`: every new
    function needs a comment, a thread tag and a caller.
20. **Regenerate the baselines again after the surface settles.**

### Phase 2 -- verification (the user drives the rounds)

Do not start these unasked. When asked:

- Build every target and variant this host can build: `linux`
  release/clang/asan/tsan/analyzer/gc; `linux-arm64` release/asan/tsan/gc;
  `win64` release/analyzer/gc; `macos` release/gc (both slices); `freebsd`
  release/asan/tsan/gc; and `win64-msvc` release/gc on Windows or CI.
- Add a native macOS `abidump` step to the `macos` CI job: compile
  `tools/abidump.c` with the system compiler against `include` and
  `mac/include`, run it, and diff against `tools/abi/fenestal-1.abi`
  (A15).
- Rounds: `x11-vulkan`, `wayland-vulkan` (confirm the
  two-window check is in the round), `win64-vulkan`, `win64-d3d12`,
  `macos-metal-native`, `macos-vulkan-native`, the
  bare, KMS and weston environments, the wine environments and the Pi;
  the checker variants `asan`, `tsan` and `gc` under the validation layer;
  `tools/probe/bare_checks.py` on the Pi. Sweep order: other machines
  first, this desktop and the guests last.
- HDR: `macos-metal-native` with `FNL_COLOR=linear` and a Windows HDR
  laptop if one is available; read the `colour:` line and capture the
  ramp. Add the harness colour check TODO *HDR* asks for. If no panel can
  be reached, mark HDR experimental (D1, B17).
- 175%: one release build re-bakes the atlases; run `verifyfont` and
  re-measure the blob (D2).
- TAAU: `bench.py` with `--bench-antialias=taau --bench-scene-motion` on a
  panning scene; check the history and reprojection (D5).
- MSVC: review the `/W3` warnings on the `win64-msvc` tree; either keep
  `/W3` documented or plan a `/W4` pass (D6).
- `pace.py` on the desktop environments for regressions from the new
  per-frame checks; they should be negligible.

### Phase 3 -- freeze 1.0.0

1. Final API diff review: no renames, no new fields, no pending
   decisions.
2. Bump `FNL_VERSION_MAJOR` to 1, `FNL_VERSION_MINOR` to 0 and
   `FNL_VERSION_PATCH` to 0; set `project(fenestal VERSION 1.0.0)`;
   configure's check passes.
3. Snapshot the final baselines: rename `tools/abi/fenestal-0*` to
   `fenestal-1*`; the CMake prefix is already
   `${PROJECT_VERSION_MAJOR}`; regenerate from the final trees.
4. Replace `fenestal.map`'s wildcard with the explicit `FENESTAL_1_0`
   node; run the map and ELF checks.
5. Rewrite the stability text for 1.0 in the header, README and DESIGN;
   add the 1.0.0 CHANGELOG section; update TODO *The ABI baseline* and add
   the post-1.0 backlog.
6. Run the full build sweep and the CI push; confirm the native Windows,
   FreeBSD and macOS ABI checks and the full test matrix.
7. Tag `v1.0.0`. GitHub only sees the tag with `--no-verify` because of
   the pre-push hook; the push is by hand. CI publishes the release from
   the tag.
8. Post-release: file the deferred items in TODO -- window icon, cursor
   names, `FNL_CURSOR_NONE` and capture, minimum and maximum size, window
   flags, work area, runtime move, per-pixel motion, D3D12 and Metal
   capability requests, gamepad and touch, and multi-card KMS.

---

## 4. What is not in 1.0

Document these in the stability block so 1.0 does not read as an omission:

- touch, pen and gamepad;
- window icons, unless B3 is taken;
- always-on-top, opacity and attention;
- runtime window move and minimum or maximum size;
- clipboard images, rich text and drag and drop (plain text is in);
- popups and dialogs;
- 32-bit targets (the baselines are 64-bit only);
- fonts other than the baked faces;
- per-window log sinks and per-window colour (a host has one of each).

---

## 5. Baseline refresh commands

The four layouts and five export sets come from different
configurations. The tree directory is fixed, so each is a build, copy and
rebuild:

```sh
# Linux (Vulkan only): layout fenestal-0.abi, exports fenestal-0.syms
./build.sh linux release
cp build/linux/release/generated/fenestal.abi tools/abi/fenestal-0.abi
cp build/linux/release/generated/fenestal.syms tools/abi/fenestal-0.syms

# Windows (mingw), Direct3D 12 and Vulkan: exports
# fenestal-0-d3d12-vulkan.syms; the layout is dumped by a mingw abidump
# run under wine
./build.sh win64 release
cp build/win64/release/generated/fenestal.syms \
    tools/abi/fenestal-0-d3d12-vulkan.syms
x86_64-w64-mingw32-gcc -std=c11 -I include -I <vulkan headers> \
    -DFNL_ABI_D3D12 -DFNL_ABI_VULKAN tools/abidump.c -o /tmp/abidump.exe
wine /tmp/abidump.exe /tmp/fenestal-d3d12-vulkan.abi
cp /tmp/fenestal-d3d12-vulkan.abi tools/abi/fenestal-0-d3d12-vulkan.abi

# Windows, Direct3D 12 only: exports fenestal-0-d3d12.syms, layout
# fenestal-0-d3d12.abi
./build.sh win64 release -DFNL_RENDERER=d3d12
cp build/win64/release/generated/fenestal.syms tools/abi/fenestal-0-d3d12.syms
x86_64-w64-mingw32-gcc -std=c11 -I include -DFNL_ABI_D3D12 \
    tools/abidump.c -o /tmp/abidump.exe
wine /tmp/abidump.exe /tmp/fenestal-d3d12.abi
cp /tmp/fenestal-d3d12.abi tools/abi/fenestal-0-d3d12.abi

# macOS (cross): the exports are written at configure
./build.sh macos release
cp build/macos/release/generated/fenestal.syms \
    tools/abi/fenestal-0-metal-vulkan.syms
./build.sh macos release -DFNL_RENDERER=metal
cp build/macos/release/generated/fenestal.syms tools/abi/fenestal-0-metal.syms
```

The Metal structs are in every `.abi`; the macOS layout is checked natively
by the new CI `abidump` step against `fenestal-0.abi`. At 1.0 every
`fenestal-0` above becomes `fenestal-1`.

---

## 6. Order and risk

The highest-risk items are A1 (a green build currently promises nothing
about three public fields) and A10 (the tail-padding trap). Both should be
fixed before any other API change, because every later change will
otherwise be measured against a baseline that cannot see it. The
recommended order is Phase 0, then decisions D1-D10, then Phase 1, then the
verification gates of Phase 2, then the 1.0 freeze. The API itself does not
need a redesign.
