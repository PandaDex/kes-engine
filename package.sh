#!/usr/bin/env bash
# Builds and packages the game.
#
# For each target: configures the "package-<target>" CMake preset, builds it in
# Release and runs the kes_package target. Packages land in out/package/.
#
#   linux -> out/package/<name>-linux-<version>.tar.gz
#
# The external binaries are downloaded first if slangc is missing.
#
# Usage:
#   ./package.sh            # linux
#   ./package.sh linux
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KNOWN="linux"

targets=("$@")
[ ${#targets[@]} -gt 0 ] || targets=(linux)

for target in "${targets[@]}"; do
    case "$target" in
        all) target="$KNOWN" ;;
        linux) ;;
        windows|gdk) echo "error: '$target' packages are built on Windows with package.ps1" >&2; exit 1 ;;
        -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "error: unknown target '$target' (known: $KNOWN, all)" >&2; exit 1 ;;
    esac

    if [ ! -x "$ROOT/external/slang/linux64/bin/slangc" ]; then
        echo "== Downloading external binaries"
        "$ROOT/external/download_external.sh"
    fi

    echo
    echo "== Packaging $target (preset package-$target)"
    (cd "$ROOT" && cmake --preset "package-$target" && cmake --build --preset "package-$target" --parallel)
done

echo
echo "== Done. Packages are in $ROOT/out/package"
