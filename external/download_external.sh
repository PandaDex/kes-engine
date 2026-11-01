#!/usr/bin/env bash
# Downloads and unpacks the prebuilt external binaries (slang, FBX2glTF).
#
# The version comes from the "external_version" file next to this script
# (shared with download_external.ps1). The archive is fetched from:
#   https://github.com/PandaDex/kes-engine/releases/download/external-v<version>/external.zip
# Each top-level folder in the archive replaces the folder of the same name in
# external/. Folders that are not in the archive (SDL3, imgui, cgltf) are left alone.
#
# Usage:
#   ./external/download_external.sh               install if not already at this version
#   ./external/download_external.sh --force       reinstall
#   ./external/download_external.sh --zip FILE    install from a local external.zip
set -euo pipefail

REPO="PandaDex/kes-engine"
ASSET_NAME="external.zip"
EXTERNAL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION_FILE="$EXTERNAL_DIR/external_version"
STAMP_FILE="$EXTERNAL_DIR/.external_installed"

FORCE=0
LOCAL_ZIP=""
while [ $# -gt 0 ]; do
    case "$1" in
        -f|--force) FORCE=1 ;;
        -z|--zip)
            [ $# -ge 2 ] || { echo "error: --zip needs a file" >&2; exit 1; }
            LOCAL_ZIP="$2"; shift ;;
        -h|--help)
            sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "error: unknown argument '$1' (see --help)" >&2; exit 1 ;;
    esac
    shift
done

die() { echo "error: $*" >&2; exit 1; }

# http_get URL OUTFILE
http_get() {
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$2" "$1"
    else
        die "curl or wget is required"
    fi
}

[ -f "$VERSION_FILE" ] || die "version file not found: $VERSION_FILE"
VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"
[ -n "$VERSION" ] || die "version file is empty: $VERSION_FILE"

URL="https://github.com/$REPO/releases/download/external-v$VERSION/$ASSET_NAME"

if [ "$FORCE" -eq 0 ] && [ -z "$LOCAL_ZIP" ] && [ -f "$STAMP_FILE" ]; then
    if [ "$(tr -d '[:space:]' < "$STAMP_FILE")" = "$VERSION" ]; then
        echo "External binaries v$VERSION already installed. Use --force to reinstall."
        exit 0
    fi
fi

TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/kes-external-XXXXXX")"
trap 'rm -rf "$TEMP_DIR"' EXIT

if [ -n "$LOCAL_ZIP" ]; then
    [ -f "$LOCAL_ZIP" ] || die "archive not found: $LOCAL_ZIP"
    ARCHIVE="$(cd "$(dirname "$LOCAL_ZIP")" && pwd)/$(basename "$LOCAL_ZIP")"
    echo "Using local archive $ARCHIVE"
else
    ARCHIVE="$TEMP_DIR/$ASSET_NAME"
    echo "Downloading external binaries v$VERSION"
    echo "  $URL"
    http_get "$URL" "$ARCHIVE"         || die "download failed. Check that release external-v$VERSION exists and has $ASSET_NAME."
fi

EXTRACT_DIR="$TEMP_DIR/extract"
mkdir -p "$EXTRACT_DIR"
echo "Unpacking..."
if command -v unzip >/dev/null 2>&1; then
    unzip -q "$ARCHIVE" -d "$EXTRACT_DIR"
elif command -v python3 >/dev/null 2>&1; then
    python3 -m zipfile -e "$ARCHIVE" "$EXTRACT_DIR"
else
    die "unzip or python3 is required"
fi

shopt -s nullglob dotglob
entries=("$EXTRACT_DIR"/*)
[ ${#entries[@]} -gt 0 ] || die "archive is empty: $ARCHIVE"

for entry in "${entries[@]}"; do
    name="$(basename "$entry")"
    rm -rf "${EXTERNAL_DIR:?}/$name"
    mv "$entry" "$EXTERNAL_DIR/$name"
    echo "  installed external/$name"
done

# Zips made on Windows don't keep the executable bit.
for exe in \
    "$EXTERNAL_DIR"/slang/linux64/bin/* \
    "$EXTERNAL_DIR"/FBX2glTF/linux64/FBX2glTF; do
    case "$exe" in
        *.slang) ;;  # module sources, not executables
        *) if [ -f "$exe" ]; then chmod +x "$exe"; fi ;;
    esac
done

if [ -n "$LOCAL_ZIP" ]; then
    rm -f "$STAMP_FILE"
    echo "Installed from local archive (version stamp not written)."
else
    printf '%s' "$VERSION" > "$STAMP_FILE"
    echo "External binaries v$VERSION installed."
fi
