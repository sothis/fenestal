#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""The Vulkan loader and Khronos' validation layer, built here for x86-64
macOS, for a checker round on the Mac.

    python3 tools/vulkan_macos.py --build       # fetch, check, build
    python3 tools/vulkan_macos.py --status      # where it is, or that it is not
    python3 tools/vulkan_macos.py --files       # what goes beside a program

A Mac has MoltenVK and nothing in front of it: Khronos builds no loader for
one and Homebrew no longer does for an Intel Mac, and without a loader
there are no layers. The library opens a loader beside the program before
MoltenVK (src/renderer/vulkan/vk_load.c), so these go there for a checker
round alone -- the harness copies them (environment.py, CocoaNative), and
names MoltenVK's manifest and the layer's directory to the loader -- and
never into a tree: a release round is MoltenVK opened directly, as a
program shipped from a tree is.

Built by the macos tree's toolchain (cmake/macos-x86_64.cmake: clang-21,
ld64.lld, Apple's SDK from tools/macos_sdk.py), from one Vulkan SDK
release's sources, each archive pinned by its SHA-256: a download that does
not match is refused, not unpacked. The sources and the build are kept
under ~/.local/state/fenestal, and survive `rm -rf build`. A new release is
a new SDK and new rows in SOURCES; the commits are the ones the layer's
scripts/known_good.json names for it.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK_TAG = "vulkan-sdk-1.4.363.0"
GH = "https://github.com/KhronosGroup/%s/archive/%s.tar.gz"
# name: (ref, SHA-256 of GitHub's archive of it)
SOURCES = {
    "Vulkan-Headers": (
        SDK_TAG,
        "4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e"),
    "Vulkan-Loader": (
        SDK_TAG,
        "941eff558fc74f248745fb7df367640cf89a8457f216dae498a74ae14b6ba6f7"),
    "Vulkan-ValidationLayers": (
        SDK_TAG,
        "36720627913a591ae8ffc0f24b3c04f98af65242c9b7215d1944a85ff5b70e00"),
    "Vulkan-Utility-Libraries": (
        "d498a40c2e1fa6ce49667e8aaa4472c0db623cda",
        "84c35444280800fdfde09e947edc0a5a41afad73af712b75314af9ba8066a08b"),
    "SPIRV-Headers": (
        "496543121ce6419f23d6fa5d7194ba66c36212d2",
        "a9bb9c48713245eacf97cc539b6f1d45405a92d8813f8b82e635f0a085ee9898"),
    "SPIRV-Tools": (
        "ef96ed763b43b59b33b31b362f09a02b729fa1c9",
        "82c62146083fd558735a3171cf97cfc47903ca7d368482e87f94bd44883c0f00"),
}

STATE = os.path.join(os.environ.get("XDG_STATE_HOME")
                     or os.path.expanduser("~/.local/state"), "fenestal")
HOME = os.path.join(STATE, "vulkan-macos", SDK_TAG.rsplit("-", 1)[-1])
SRC = os.path.join(HOME, "src")
BUILD = os.path.join(HOME, "b")
PREFIX = os.path.join(HOME, "prefix")
OUT = os.path.join(HOME, "out")

LOADER = "libvulkan.1.dylib"
LAYER = "libVkLayer_khronos_validation.dylib"
LAYER_JSON = "VkLayer_khronos_validation.json"
# MoltenVK's manifest, beside its dylib (tools/moltenvk.py): the loader
# finds no driver by itself on a Mac but in an app bundle or a system
# directory, so the round names this one (VK_DRIVER_FILES).
ICD_JSON = "MoltenVK_icd.json"
LICENCE = "Vulkan-LICENSE.txt"
FILES = (LOADER, LAYER, LAYER_JSON, ICD_JSON, LICENCE)


def ready():
    return all(os.path.isfile(os.path.join(OUT, f)) for f in FILES)


def files():
    """What goes beside the program, by path here."""
    return [os.path.join(OUT, f) for f in FILES]


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def _fetch(name):
    ref, want = SOURCES[name]
    archive = os.path.join(SRC, name + ".tar.gz")
    if not os.path.isfile(archive) or _sha256(archive) != want:
        url = GH % (name, ref)
        print("vulkan_macos: downloading %s" % url, file=sys.stderr,
              flush=True)
        part = archive + ".part"
        with urllib.request.urlopen(url, timeout=600) as r, \
                open(part, "wb") as f:
            shutil.copyfileobj(r, f, 1 << 20)
        os.replace(part, archive)
    got = _sha256(archive)
    if got != want:
        os.remove(archive)
        sys.exit("vulkan_macos: %s has SHA-256 %s, not the pinned %s; "
                 "removed" % (name, got, want))
    tree = os.path.join(SRC, name)
    shutil.rmtree(tree, ignore_errors=True)
    os.makedirs(tree)
    subprocess.run(["tar", "-C", tree, "--strip-components=1", "-xzf",
                    archive], check=True)
    return tree


def _cmake(name, *defs, targets=(), install=True):
    sdk = subprocess.run([sys.executable, os.path.join(ROOT, "tools",
                                                       "macos_sdk.py"),
                          "--path"], capture_output=True, text=True,
                         check=True).stdout.strip()
    # The tree's toolchain, with the prefix the earlier projects install
    # into made a root it searches: it searches the SDK's alone.
    chain = os.path.join(HOME, "toolchain.cmake")
    with open(chain, "w") as f:
        f.write("include(%s)\nlist(APPEND CMAKE_FIND_ROOT_PATH %s)\n"
                % (os.path.join(ROOT, "cmake", "macos-x86_64.cmake"), PREFIX))
    env = dict(os.environ, FNL_MACOS_SDK=sdk)
    b = os.path.join(BUILD, name)
    subprocess.run(["cmake", "-S", os.path.join(SRC, name), "-B", b,
                    "-DCMAKE_TOOLCHAIN_FILE=" + chain,
                    "-DFNL_MACOS_SDK=" + sdk,
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DCMAKE_INSTALL_PREFIX=" + PREFIX,
                    "-DCMAKE_PREFIX_PATH=" + PREFIX] + list(defs),
                   env=env, check=True)
    build = ["cmake", "--build", b, "-j", str(os.cpu_count() or 4)]
    for t in targets or (None,):
        subprocess.run(build + (["--target", t] if t else []), env=env,
                       check=True)
    if install:
        subprocess.run(["cmake", "--install", b], env=env, check=True,
                       stdout=subprocess.DEVNULL)
    return b


def build():
    os.makedirs(SRC, exist_ok=True)
    for name in SOURCES:
        _fetch(name)
    shutil.rmtree(BUILD, ignore_errors=True)
    shutil.rmtree(PREFIX, ignore_errors=True)
    _cmake("Vulkan-Headers")
    # The dylib alone: the framework beside it is linked by rules of its
    # own that leave the toolchain's linker out.
    loader = _cmake("Vulkan-Loader", "-DBUILD_TESTS=OFF",
                    targets=("vulkan",), install=False)
    _cmake("SPIRV-Headers", "-DSPIRV_HEADERS_ENABLE_TESTS=OFF")
    _cmake("Vulkan-Utility-Libraries", "-DBUILD_TESTS=OFF",
           "-DUPDATE_DEPS=OFF")
    _cmake("SPIRV-Tools",
           "-DSPIRV-Headers_SOURCE_DIR=" + os.path.join(SRC, "SPIRV-Headers"),
           "-DSPIRV_SKIP_TESTS=ON", "-DSPIRV_SKIP_EXECUTABLES=ON",
           "-DSPIRV_WERROR=OFF", "-DBUILD_SHARED_LIBS=OFF")
    layer = _cmake("Vulkan-ValidationLayers", "-DBUILD_TESTS=OFF",
                   "-DUPDATE_DEPS=OFF", "-DBUILD_WERROR=OFF",
                   install=False)

    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    shutil.copyfile(os.path.join(loader, "loader", "libvulkan.1.dylib"),
                    os.path.join(OUT, LOADER))
    shutil.copyfile(os.path.join(layer, "layers", LAYER),
                    os.path.join(OUT, LAYER))
    # The layer's manifest names its library beside it, as here.
    with open(os.path.join(layer, "layers", LAYER_JSON)) as f:
        manifest = json.load(f)
    manifest["layer"]["library_path"] = "./" + LAYER
    with open(os.path.join(OUT, LAYER_JSON), "w") as f:
        json.dump(manifest, f, indent=4)
    with open(os.path.join(OUT, ICD_JSON), "w") as f:
        json.dump({"file_format_version": "1.0.0",
                   "ICD": {"library_path": "./libMoltenVK.dylib",
                           "api_version": "1.4.0",
                           "is_portability_driver": True}}, f, indent=4)
    # The loader's and the layer's licences are both Apache-2.0, the
    # layer's the same text with its own copyright lines.
    shutil.copyfile(os.path.join(SRC, "Vulkan-Loader", "LICENSE.txt"),
                    os.path.join(OUT, LICENCE))
    print("Vulkan loader and validation layer (%s): %s" % (SDK_TAG, OUT))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build", action="store_true")
    ap.add_argument("--files", action="store_true")
    ap.add_argument("--status", action="store_true")
    args = ap.parse_args()
    if args.build:
        build()
        return 0
    if args.files:
        if not ready():
            sys.exit("vulkan_macos: not built -- --build")
        print("\n".join(files()))
        return 0
    print("%s for macOS: %s" % (SDK_TAG, OUT if ready() else
                                "not built -- --build"))
    return 0 if ready() else 1


if __name__ == "__main__":
    sys.exit(main())
