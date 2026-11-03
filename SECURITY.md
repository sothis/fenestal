# Security Policy

Fenestal reads bytes other programs control: the keymap a Wayland compositor
sends as text, the clipboard another program offers, the X server's replies
and properties, the names a compositor or a driver gives an output or a GPU,
and on the console the keyboard files the system holds. Anything a hostile
peer can reach through those is handled as a security report rather than an
ordinary bug.

## Reporting a Vulnerability

**Do not open a public issue.** Report it privately, through this
repository's *Report a vulnerability* (GitHub's private vulnerability
reporting) or by email to **fenestal@mail.de**, with the details and a
reproducer, and allow time for a fix before disclosing.

Please include, where you have them:

1. The version (`fnl_version()`), the platform -- operating system, window
   server or compositor and its version, or the console -- and the compiler,
   including whether the build was a checker tree (`./build.sh linux asan`,
   say).
2. The program's log: the backend and renderer it opened and the GPUs it
   found.
3. A reproducer. For a parser finding, attach the input as a file -- a
   keymap, a clipboard's bytes, a rules file -- rather than pasting it: text
   that is not valid UTF-8 does not survive copy-paste, and a corrupted input
   reproduces a different problem. A fuzz harness's crashing input is the
   ideal form (`tools/fuzz/`).
4. The observed effect -- the sanitizer report, the crashing stack, or the
   hang, with what the other program was doing at the time.

Expect an acknowledgement within a few days. A fix ships in the next release,
with the reporter credited in `CHANGELOG.md` unless they ask otherwise.

## Supported Versions

| Version | Supported |
|---|---|
| 0.9.x | Yes |

The API is 0.9 and not yet stable (DESIGN.md *Compatibility*): a fix lands on
the newest release only, and before 1.0 a minor release may change the API
as well.

## In Scope

- Memory-safety defects -- out-of-bounds access, use-after-free,
  uninitialised reads -- reachable from what another program sends: the
  keymap text (`src/backend/keymap_xkb.c`), a paste, the X server's replies,
  an output's or a GPU's name, the console's keyboard files and
  `FNL_KMS_LAYOUT` (`src/backend/keymap_rules.c`).
- A crash, or a hang of the thread that pumps input, on such bytes --
  including a peer that sends malformed, truncated or endless data, or stops
  answering in the middle of a transfer. A paste is bounded by a deadline and
  reported as no text when it runs out; one that blocks the program instead is
  a defect.
- An event that says something other than what the peer sent: a key typed
  through the wrong level of the layout, text that is not the clipboard's.

## Out of Scope

These are properties of the design, not defects. They are listed so a report
does not have to guess.

- **The caller.** A program that misuses the API -- a call from the wrong
  thread, a descriptor it filled wrongly, a scene that records out of bounds
  -- is the program's defect. What a caller can get wrong is answered
  through the two error tiers where the library can tell; `FNL_ASSERT` is
  for invariants, and is nothing in a release build.
- **The window server.** A compositor or X server sees and controls
  everything its clients show and are sent; a client cannot defend against
  one beyond not being corrupted by what it sends.
- **The GPU driver.** A fault inside a driver is the driver's, reported
  upstream; the library steps past a renderer or a device that fails, and
  recovers from a lost device.
- **The console.** On KMS the program takes the output whole and grabs the
  keyboards for as long as it runs, so nothing typed reaches the console
  beneath; that is the backend's purpose, and its user is in the groups
  `video` and `input` for it.
- **The harness.** `tools/drive/` drives the built programs on machines its
  user controls, as root where it needs to be; it is development tooling and
  is never shipped.

## Hardening

Optimised builds are hardened (`FNL_HARDEN`, on by default: `-fhardened`
where gcc has it, and otherwise each of its parts the compiler takes). The
parsers that read another program's bytes -- the keymap, the rules files,
UTF-8 text and the font codec -- have fuzz harnesses (`tools/fuzz/`), replayed
over their corpus on every native build and built against libFuzzer, with
ASan and UBSan, in the clang trees. The checker trees run the library under
AddressSanitizer with UBSan, ThreadSanitizer, and gcc's `-fanalyzer`, and the
clang tree checks the lock discipline with `-Wthread-safety`.
