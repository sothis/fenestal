#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# PostToolUse/Write|Edit guard for the house style described in CLAUDE.md:
# tabs to indent C, GLSL and the linker script and spaces everywhere else,
# no trailing whitespace, wrap at 80 with a tab four columns wide.
#
# There is no clang-format run over this tree, so this is the only mechanical
# check. It reports only lines that are *not yet committed* -- the working-tree
# diff against HEAD, or the whole file if it is untracked. Checking the whole
# file would flag the ~150 pre-existing lines that run past 80 on every edit,
# and a guard that always fires is a guard nobody reads.
#
# Output is advisory: hookSpecificOutput.additionalContext, no `decision`.
# Nothing here is worth blocking an edit over.
set -uo pipefail

read -r -d '' payload || true
file=$(printf '%s' "$payload" |
       jq -r '.tool_response.filePath // .tool_input.file_path // empty' \
       2>/dev/null)
[ -n "$file" ] || exit 0
[ -f "$file" ] || exit 0

root=$(git -C "$(dirname "$file")" rev-parse --show-toplevel 2>/dev/null) ||
    exit 0
rel=${file#"$root"/}

case "$rel" in
    src/*.c|src/*.h|src/*.m|tools/*.c|tools/*.h|tools/*.m|shaders/*) ;;
    CMakeLists.txt) ;;
    include/*.h|examples/*.c|examples/*.h|examples/*.m) ;;
    testbed/*.c|testbed/*.h|testbed/*.m|testbed/shaders/*) ;;
    demos/*.c|demos/*.h|demos/*.m|demos/*/shaders/*) ;;
    cmake/*.cmake|tools/*.cmake|demos/*/CMakeLists.txt) ;;
    tools/*.py|tools/*.sh|tools/*.ps1|build.sh|run_all.sh) ;;
    *) exit 0 ;;
esac

# Added-or-modified lines only, with their line numbers in the new file:
# against HEAD, so a line already staged is still checked. Untracked files
# have no HEAD side, so the whole file is "added".
if git -C "$root" ls-files --error-unmatch -- "$rel" >/dev/null 2>&1; then
    added=$(git -C "$root" diff -U0 --no-color HEAD -- "$rel" 2>/dev/null |
            awk '
        /^@@/ { split($3, h, ","); n = substr(h[1], 2) + 0; inhunk = 1; next }
        !inhunk { next }
        /^\+/ { print n "\t" substr($0, 2); n++ }
    ')
else
    added=$(awk '{ print NR "\t" $0 }' "$file")
fi

[ -n "$added" ] || exit 0

# C, Objective-C, GLSL and the linker script indent with tabs; CMake, shell,
# PowerShell and Python with spaces, as bovnar does and as Python must.
case "$rel" in
    *.c|*.h|*.m|*.vert|*.frag|*.map|shaders/*|*/shaders/*) tabs=1 ;;
    *) tabs=0 ;;
esac

findings=$(printf '%s\n' "$added" | awk -F'\t' -v file="$rel" -v tabs="$tabs" '
    {
        num = $1
        line = substr($0, index($0, "\t") + 1)
        if (tabs && line ~ /^\t* {4,}/)
            print file ":" num ": indented with spaces -- tabs here"
        else if (tabs && line ~ /^ *\t/ && line !~ /^\t/)
            print file ":" num ": space before a tab in the indent"
        else if (!tabs && line ~ /\t/)
            print file ":" num ": tab character -- spaces only here"
        else if (line ~ /[ \t]+$/)
            print file ":" num ": trailing whitespace"
        # Columns with a tab four wide, as .editorconfig sets it.
        w = 0
        for (i = 1; i <= length(line); i++)
            w = substr(line, i, 1) == "\t" ? w - w % 4 + 4 : w + 1
        if (w > 80)
            print file ":" num ": " w \
                  " columns -- house style wraps at 80"
    }
' | head -20)

[ -n "$findings" ] || exit 0

jq -n --arg f "$findings" '{
    hookSpecificOutput: {
        hookEventName: "PostToolUse",
        additionalContext:
            ("Style guard (uncommitted lines only; see CLAUDE.md):\n" + $f)
    },
    suppressOutput: true
}'
