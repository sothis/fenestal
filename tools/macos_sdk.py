#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""The macOS SDK the macos trees are cross-compiled against.

    python3 tools/macos_sdk.py --fetch [HOST]   # copy it from a Mac, by ssh
    python3 tools/macos_sdk.py --path           # where it is, for build.sh
    python3 tools/macos_sdk.py --status         # which, or that it is not

The macOS trees are built on this machine, as the Windows ones are, by
clang and ld64.lld against Apple's SDK: the headers and the .tbd stubs of
the system's libraries and frameworks, no code. Apple ships it with the
Command Line Tools, which only run on a Mac, so it is copied once from one
-- the machine that runs the programs, by default (FNL_DRIVE_MAC, else
schenker-mac) -- and kept under ~/.local/state/fenestal, which survives
`rm -rf build`.

One version, pinned: the SDK the tree is known to build against. A Mac
whose tools carry another is refused rather than copied, and a new SDK is a
new VERSION here, in the commit that builds against it.
"""

import argparse
import json
import os
import subprocess
import sys

VERSION = "26.5"
NAME = "MacOSX%s.sdk" % VERSION
# Where the Command Line Tools put it. Xcode's would do as well, from its
# own path; the tools are what the Mac here has.
REMOTE = "/Library/Developer/CommandLineTools/SDKs"

STATE = os.path.join(os.environ.get("XDG_STATE_HOME")
                     or os.path.expanduser("~/.local/state"), "fenestal")
HOME = os.path.join(STATE, "macos-sdk")
SDK = os.path.join(HOME, NAME)


def version(sdk):
    """The version the SDK says it is, or None for no SDK there."""
    try:
        with open(os.path.join(sdk, "SDKSettings.json"),
                  encoding="utf-8") as f:
            return json.load(f).get("Version")
    except (OSError, ValueError):
        return None


def ready():
    return version(SDK) == VERSION


def fetch(host):
    """Copy the pinned SDK from `host`, a name ssh knows, as a tar stream
    into a directory of its own, renamed into place once it is whole."""
    os.makedirs(HOME, exist_ok=True)
    part = SDK + ".part"
    subprocess.run(["rm", "-rf", part], check=True)
    os.makedirs(part)
    print("macos_sdk: copying %s from %s" % (NAME, host), file=sys.stderr,
          flush=True)
    # The SDK's own symlinks (the frameworks' Versions/Current) are copied
    # as links, which is what they are. Without the Mac's file flags and
    # extended attributes, which GNU tar has no use for and warns about
    # once a file.
    tar = subprocess.Popen(
        ["ssh", "-o", "BatchMode=yes", host,
         "COPYFILE_DISABLE=1 tar --no-fflags --no-xattrs --no-mac-metadata "
         "-C %s -cf - %s" % (REMOTE, NAME)],
        stdout=subprocess.PIPE)
    untar = subprocess.run(["tar", "--warning=no-unknown-keyword", "-C",
                            part, "-xf", "-"], stdin=tar.stdout)
    tar.stdout.close()
    if tar.wait() != 0 or untar.returncode != 0:
        subprocess.run(["rm", "-rf", part], check=False)
        sys.exit("macos_sdk: %s has no %s/%s, or the copy failed"
                 % (host, REMOTE, NAME))
    got = version(os.path.join(part, NAME))
    if got != VERSION:
        subprocess.run(["rm", "-rf", part], check=False)
        sys.exit("macos_sdk: the copy says it is %s, not the pinned %s"
                 % (got, VERSION))
    subprocess.run(["rm", "-rf", SDK], check=True)
    os.rename(os.path.join(part, NAME), SDK)
    os.rmdir(part)
    print("macOS SDK %s: %s" % (VERSION, SDK))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--fetch", nargs="?", const="", metavar="HOST")
    ap.add_argument("--path", action="store_true")
    ap.add_argument("--status", action="store_true")
    args = ap.parse_args()
    if args.fetch is not None:
        fetch(args.fetch or os.environ.get("FNL_DRIVE_MAC") or "schenker-mac")
        return 0
    if args.path:
        if not ready():
            sys.exit("macos_sdk: no macOS SDK %s here; "
                     "python3 tools/macos_sdk.py --fetch" % VERSION)
        print(SDK)
        return 0
    print("macOS SDK %s: %s" % (VERSION, SDK if ready() else
                                "not here -- --fetch"))
    return 0 if ready() else 1


if __name__ == "__main__":
    sys.exit(main())
