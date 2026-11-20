---
date: 2026-11-20T20:38:39+0100
author: Janos Sonntag
commit: 41bf851
branch: main
repository: fenestal
topic: "ambienceDJ handoff resumed and verified; paused for a desktop restart"
tags: [handoff, performance, latency, ambienceDJ, overlay, nfs, nas]
status: complete
last_updated: 2026-11-20T20:38:39+0100
last_updated_by: Janos Sonntag
type: research
---

# Handoff: the ambienceDJ handoff resumed and verified; paused for a restart

## Task(s)

**This session did no implementation.** It resumed
`.rpiv/artifacts/handoffs/2026-11-20_20-08-51_ambiencedj-frame-cost-and-latency.md`
and verified it against both trees, then paused at your request because you are
restarting the desktop.

- Read and analyse the handoff — **done**.
- Verify every file reference and learning in it — **done**, all still true
  (detail below).
- Finish the push — **still undone**, blocked by the NAS mount, unchanged.
- Verify by ear and by eye — **still undone**, and yours.
- Rebuild the checker trees — **not started**, you chose to pause first.

The desktop restart is why this file exists. Nothing is in flight: both trees are
clean at a known commit, so the next session starts from exactly here.

## Critical References

- `/home/json/ws/ambienceDJ` at `da77310` — `src/ui.c`, `src/radio_run.c`.
- `/home/json/ws/fenestal` at `41bf851` — `src/core/api_overlay.c`,
  `include/fenestal/fenestal.h`.
- `.rpiv/artifacts/handoffs/2026-11-20_20-08-51_ambiencedj-frame-cost-and-latency.md`
  — the handoff this one resumes; read it first, its *Learnings* and *Action
  Items* are the substance and are not repeated here.

## Recent changes

None. No file in either tree was written this session.

fenestal's only commit since the previous handoff is `41bf851`, the handoff file
itself. `git status` is clean in both trees.

## Learnings

- **The NAS is still unreachable, and the cause is server-side.** `stat` on
  `/mnt/storage/scm` reads `json:json 0700` with `user::rwx`, and the session
  *is* uid 1000 (`id` says so), yet `ls` and `touch` both return `Permission
  denied`. A local mode/owner that looks right plus an EACCES is NFS squashing
  on the export, not a client permission problem. The mount is
  `10.0.0.49:/mnt/storage` `nfs4,sec=sys` by `/etc/fstab:17` (autofs, 10-minute
  idle timeout). **Fixing it needs root on 10.0.0.49's `/etc/exports`** —
  `all_squash,anonuid=…`, or an `anonuid=` remount here — and `sudo` on this
  desktop asks for a password, so nothing was attempted. Do not re-diagnose the
  client; `stat`, `getfacl` and `findmnt` have all been run and are all clean.
- **The checker trees' staleness, measured rather than assumed.** `9132f46` is
  2026-11-20 20:00:35. `build/linux/release` is 20:06 (current);
  `build/linux/asan` 14:05, `clang` 14:06, `gc` 14:06 — all before `a8b84ad` and
  `29d46fb`; `analyzer` is Nov 5 and `tsan` Nov 6. So five trees are behind, and
  `analyzer` and `tsan` by a fortnight rather than a commit.
- **The fenestal.h alpha documentation has moved, not changed.** The handoff
  cited `:1013` and `:1082`; it is now at `:1055-1059` (the `FnlColor` comment,
  "a hole in the window, not a wash") and `:1128` (the `FnlDrawState` field).
  Grep for "hole in the window" rather than trusting the line number.
- **Every other reference in the previous handoff is still exact.**
  `COLOR_MEMO 64` and the FNV hash are `src/core/api_overlay.c:41-90`; the
  `record_gen` bump is `src/core/frame.c:2151` (the handoff said 2146);
  `src/core/text_measure.c:83-90` is the binary search. ambienceDJ's
  `text_w()` is `src/ui.c:205-229`, the redraw gate `:137/141/1914`.
- **`git for-each-ref` is the way to read push state here.** `refs/remotes/
  origin/main` is stale at `a630f90` because nothing can fetch from the NAS;
  `ambienceDJ`'s `ahead 1` came from `git branch -vv` instead. Do not read
  fenestal's `origin/main` and conclude its GitHub leg is missing — the previous
  handoff recorded `a630f90..9132f46` as having reached GitHub.

## Artifacts

Read in this order:

1. `.rpiv/artifacts/handoffs/2026-11-20_20-08-51_ambiencedj-frame-cost-and-latency.md`
   — the work and its reasoning.
2. This file — what was verified, and what is still blocked.
3. `/home/json/ws/ambienceDJ` at `da77310`, then `/home/json/ws/fenestal` at
   `41bf851`, for the code itself.

## Action Items & Next Steps

**First — the push, once the NAS reads.** Two commands, in this order:

```sh
cd /home/json/ws/fenestal  && git push /mnt/storage/scm/fenestal.git main
cd /home/json/ws/ambienceDJ && git push
```

`ambienceDJ` is `ahead 1` (`da77310` unpushed). fenestal's NAS leg wants
`41bf851`; its GitHub leg is believed already at `9132f46`, so confirm rather
than assume. fenestal's `pre-push` hook refuses any ref but `main` to GitHub.

**Second — the checker trees.** Five of six Linux trees predate the fenestal
commits:

```sh
./build.sh linux clang && ./build.sh linux asan && ./build.sh linux gc
./build.sh linux tsan && ./build.sh linux analyzer
```

`release` is already current. The Windows and macOS trees were not checked this
session.

**Third — verify by ear and by eye**, still nobody's:

1. Listen for the mix: a seek, a solo, a crossfade and a shorts overlay land
   where they did. The piece-splitting moves a cue at most one 256-frame piece
   (about 5 ms) earlier.
2. Look at the meter with the mix paused: the clip zone is a dim band, not a
   bright block and not a hole.
3. Watch the new card line while dragging a knob, then while paused. That second
   number is the baseline for the rest of the work.

**Fourth — the deferred review items** are unchanged and still deferred, each
with its reason in the previous handoff: `buffer_ms`, the full `radio_view()`
snapshot, `engine_seek()` on the feeder, `radio_gone_sweep()` per frame, and the
four fenestal API additions (cached measure, `fnl_overlay_palette()`,
`fnl_overlay_counts()`, an honest UI clock).

## Other Notes

- **Nothing needs re-verifying after the restart.** Both trees are clean at
  `41bf851` / `da77310`, and the restart cannot change a commit. Start at *Action
  Items*, not at the verification.
- `origin` on fenestal has one fetch URL (the NAS) and two push URLs (the NAS,
  then GitHub `sothis/fenestal`); a bare `git push` sends `main` to both, and
  `remote.origin.push` names `main` alone.
- The ambienceDJ output card's `%d fps   frame %.1f ms   in %.1f ms` line is the
  instrument for all of it, and it did not exist before that session.