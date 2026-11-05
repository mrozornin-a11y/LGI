#!/bin/sh
# Сборка LinkamCaptureXP.exe для 32-битной Windows XP SP3 (MinGW-w64, модель потоков win32).
set -e
CXX=${CXX:-i686-w64-mingw32-g++-win32}
"$CXX" -Os -std=c++11 -Wall -Wextra -static -municode -mwindows \
  -fno-exceptions -fno-rtti -Wl,--subsystem,windows:5.01 \
  -Wl,--major-os-version,5,--minor-os-version,1 linkam_capture.cpp \
  -o LinkamCaptureXP.exe -lgdiplus -lcomctl32 -lcomdlg32 -lshell32 -lole32 -lwinmm
i686-w64-mingw32-strip LinkamCaptureXP.exe 2>/dev/null || true
