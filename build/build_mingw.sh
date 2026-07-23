#!/usr/bin/env bash
#
# V8.0 — MinGW cross build of the full plugin DLL on Linux.
#
# Compiles the exact source list from project/Conan-Exiles-TeamSpeak.vcxproj
# (29 .c files) plus project/Resource.rc into bin/mingw/conan_exiles.dll.
#
# This is a BUILD GATE, not the release build: the shipped DLL is still built
# with MSVC on Windows (build/build_msvc.ps1). The gate is "compiles + links".
# Warnings are enabled but NOT fatal (V7 code carries known warnings).
#
# Usage: bash build/build_mingw.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CC=x86_64-w64-mingw32-gcc
WINDRES=x86_64-w64-mingw32-windres

OUT_DIR="bin/mingw"
OBJ_DIR="obj/mingw"
TARGET="$OUT_DIR/conan_exiles.dll"

mkdir -p "$OUT_DIR" "$OBJ_DIR"

# Mirrors the vcxproj Release|x64 settings (Unicode, defines, include dirs).
# MINGW_HAS_SECURE_API=1 makes the MinGW CRT declare the _s secure functions.
CFLAGS=(
    -O2
    -municode
    -DUNICODE -D_UNICODE
    -DNDEBUG
    -DCONAN_EXILES_TS_EXPORTS
    -D_WINDOWS
    -D_USRDLL
    -D_CRT_SECURE_NO_WARNINGS
    -DMINGW_HAS_SECURE_API=1
    -I. -Isrc -Iproject -Isdk/include
    -Wall -Wextra
    -Wno-unknown-pragmas
    # The TS SDK headers use MSVC __int16/__int64 on the _WIN32 branch and
    # size_t without <stddef.h>; MSVC resolves both implicitly, MinGW needs
    # these force-includes (keeps the external SDK headers untouched).
    -include stddef.h -include _mingw.h
)

# Default Win32 libs + the vcxproj additions (comctl32, uxtheme).
# Msimg32 from the vcxproj is dropped: no GradientFill/AlphaBlend/
# TransparentBlt call exists in src/ (verified 2026-07-23).
LDLIBS=(
    -lkernel32 -luser32 -lgdi32 -lcomctl32 -lcomdlg32 -luxtheme
    -lole32 -loleaut32 -luuid -lshell32 -lshlwapi -ladvapi32 -lwinmm
)

# Exact ClCompile list from project/Conan-Exiles-TeamSpeak.vcxproj (29 files).
SOURCES=(
    src/ts/entry/ts3_entry.c
    src/ts/entry/ts3_info.c
    src/ts/info/ts3_plugin_version.c
    src/core/util/log.c
    src/core/config/config.c
    src/core/mod_file/path_detect.c
    src/core/mod_file/pos_file.c
    src/ts/adapter/ts3_adapter.c
    src/core/proximity/player_table.c
    src/core/proximity/proximity_math.c
    src/ts/proximity/ts3_cepos.c
    src/ts/proximity/ts3_proximity_audio.c
    src/ts/proximity/ts3_3d.c
    src/core/channel/channel_manage.c
    src/core/hub/hub_parser.c
    src/ts/profile/ts3_server_profile.c
    src/core/proximity/zone_resolve.c
    src/core/voice/voice_modes.c
    src/core/nick/nick_anonymize.c
    src/ui/overlay/voice_overlay.c
    src/ui/dialogs/ui_main.c
    src/ui/dialogs/ui_dynamic.c
    src/ui/dialogs/ui_messages.c
    src/ui/input/key_watcher.c
    src/ui/plugin_ui_compat.c
    src/core/config/config_files.c
    src/core/validation/validation.c
    src/core/util/util_base.c
    src/core/proximity/proximity_volume.c
)

echo "== MinGW cross build ($($CC -dumpversion)) =="

OBJECTS=()
for src in "${SOURCES[@]}"; do
    obj="$OBJ_DIR/$(echo "${src%.c}" | tr '/' '_').o"
    echo "CC  $src"
    "$CC" "${CFLAGS[@]}" -c "$src" -o "$obj"
    OBJECTS+=("$obj")
done

echo "RC  project/Resource.rc"
# Resource.rc is stored as UTF-16LE (Visual Studio default); windres only
# reads 8-bit input. Convert a build-local copy, repo file stays untouched.
# The copy lives in project/ so the ..\assets\ paths keep resolving.
RC_UTF8="project/.Resource.mingw.rc"
iconv -f UTF-16LE -t UTF-8 "project/Resource.rc" \
    | sed -e 's/^\xEF\xBB\xBF//' -e 's|\\\\|/|g' > "$RC_UTF8"
trap 'rm -f "$RC_UTF8"' EXIT
"$WINDRES" -Iproject "$RC_UTF8" -O coff -o "$OBJ_DIR/resource.o"
OBJECTS+=("$OBJ_DIR/resource.o")

echo "LD  $TARGET"
"$CC" -shared -static-libgcc -o "$TARGET" "${OBJECTS[@]}" "${LDLIBS[@]}"

echo "OK: built $TARGET ($(stat -c%s "$TARGET") bytes)"
