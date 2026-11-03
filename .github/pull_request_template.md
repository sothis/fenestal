## What this changes

<!-- What is now true that was not, and why. Keep one logical change per pull
request: an API change and an implementation change go separately. -->

**Kind:** implementation change / API change

## Checks run

CI builds each target's release tree on the pull request, and no checker
tree (CONTRIBUTING.md *Before You Open a Pull Request*). Tick what you ran.

- [ ] `./build.sh linux` -- warnings, ABI, tables, layering, fuzz replay
- [ ] `./build.sh linux clang` -- `-Werror` and `-Wthread-safety`
- [ ] `./build.sh linux asan` and `./build.sh linux tsan`
- [ ] `./build.sh linux analyzer`
- [ ] `./build.sh win64`, if Windows code or a shared file changed
- [ ] `ruff check tools`, if the Python changed

**Desktops it ran on**, for a change in how a window behaves:

## An API change also has

- [ ] a comment directly above each new `FNL_API` function, with its thread tag
- [ ] a caller in a program the build makes
- [ ] the ABI baseline in `tools/abi/`, updated in this pull request
- [ ] a line in `CHANGELOG.md`
- [ ] the reasoning in `DESIGN.md`

## Licensing

- [ ] New files carry the SPDX header of the part of the tree they are in.
- [ ] Anything not written for this pull request -- adapted code, a protocol,
      a font, a fuzz input from another project -- has its entry in
      `THIRD_PARTY_NOTICES.md`, or there is none.

By opening this pull request I agree that my contribution is licensed under
the terms this project publishes for the part of the tree it touches
(CONTRIBUTING.md *Licensing*).
