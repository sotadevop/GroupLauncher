#!/bin/sh
# Builds GroupLauncher.exe with MinGW-w64 (Linux, WSL or MSYS2).
set -e
cd "$(dirname "$0")"
CC=${CC:-x86_64-w64-mingw32-gcc}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
mkdir -p build
(cd res && $WINDRES app.rc -O coff -o ../build/app.res)
$CC -std=c11 -O2 -Wall -Wextra -municode -mwindows -s -static-libgcc \
    src/grouplauncher.c build/app.res -o build/GroupLauncher.exe \
    -lshell32 -lshlwapi -lole32 -luuid -lshcore -lgdi32 -luser32 -ladvapi32 \
    -lcomctl32 -luxtheme -ldwmapi -lmsimg32
echo "Built build/GroupLauncher.exe"
