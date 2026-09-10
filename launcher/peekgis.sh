#!/bin/sh
# peekGIS Linux launcher (source of truth lives in launcher/, repo root is one level above).
# On Linux, launching a GUI app from a file manager/desktop does not open a terminal, so
# there is no console to hide; running this from a terminal puts spdlog output on stdout.
# Binary is located relative to this script's folder.
ROOT="$(cd "$(dirname "$0")/.." 2>/dev/null && pwd)"
EXE="$ROOT/build/linux/x86_64/release/bin/peekgis"
if [ ! -x "$EXE" ]; then
  EXE="$ROOT/build/linux/x86_64/debug/bin/peekgis"
fi
if [ ! -x "$EXE" ]; then
  echo "[peekGIS] not found: $EXE (build first: xmake build peekgis)" >&2
  exit 1
fi
cd "$(dirname "$EXE")" || exit 1
exec "$EXE" "$@"