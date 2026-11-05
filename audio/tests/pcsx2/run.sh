#!/bin/bash
# Builds each test with ps2build and runs it headless in PCSX2, passing when the
# program prints "<name>: PASS" through kputs. A <toolchain>.skip file (gcc.skip or
# llvm.skip) in a test directory skips it for that toolchain and gives the reason.
#
# Usage: tests/run.sh [--llvm] [test-name ...]   (--llvm builds with --toolchain llvm)
#   PS2BUILD  ps2build executable (default: ps2build)
#   PCSX2     emulator executable (default: pcsx2-qt, or the Windows install path)
#   TIMEOUT   seconds to wait for each result (default: 40)
#   PCSX2_INI PCSX2.ini to check for a USB disk image (default: ~/.config/PCSX2/inis/PCSX2.ini on Linux,
#             ~/Documents/PCSX2/inis/PCSX2.ini on Windows)
#
# A needs-usb-disk file in a test directory skips the test unless PCSX2.ini sets Msd_ImagePathMsd.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
PS2BUILD="${PS2BUILD:-ps2build}"
TIMEOUT="${TIMEOUT:-40}"

if [ -z "${PCSX2:-}" ]; then
  if [ -x "/c/Program Files/PCSX2/pcsx2-qt.exe" ]; then
    PCSX2="/c/Program Files/PCSX2/pcsx2-qt.exe"
  else
    PCSX2="pcsx2-qt"
  fi
fi

llvm=0
if [ "${1:-}" = "--llvm" ]; then
  llvm=1
  shift
fi

if [ "$#" -gt 0 ]; then
  names=("$@")
else
  names=()
  for d in "$HERE"/*/; do
    [ -f "$d/ps2.yaml" ] && names+=("$(basename "$d")")
  done
fi

if [ -z "${PCSX2_INI:-}" ]; then
  if [ -f "$HOME/.config/PCSX2/inis/PCSX2.ini" ]; then
    PCSX2_INI="$HOME/.config/PCSX2/inis/PCSX2.ini"
  else
    PCSX2_INI="$HOME/Documents/PCSX2/inis/PCSX2.ini"
  fi
fi

native_path() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else echo "$1"; fi
}

stop_emulator() {
  local pid="$1"
  if [ -r "/proc/$pid/winpid" ]; then
    taskkill //F //PID "$(cat "/proc/$pid/winpid")" >/dev/null 2>&1
  else
    kill "$pid" 2>/dev/null
  fi
}

pass=0
fail=0
skip=0
for name in "${names[@]}"; do
  dir="$HERE/$name"
  if [ "$llvm" = 1 ]; then
    config="ps2.yaml"; flags="--toolchain llvm"; out="build-llvm"; skipfile="$dir/llvm.skip"
  else
    config="ps2.yaml"; flags="--toolchain gcc"; out="build-gcc"; skipfile="$dir/gcc.skip"
  fi

  if [ -f "$skipfile" ]; then
    echo "SKIP  $name ($(head -1 "$skipfile"))"
    skip=$((skip + 1))
    continue
  fi

  if [ -f "$dir/needs-usb-disk" ] && ! grep -qE '^Msd_ImagePathMsd *= *[^ ]' "$PCSX2_INI" 2>/dev/null; then
    echo "SKIP  $name ($(head -1 "$dir/needs-usb-disk"))"
    skip=$((skip + 1))
    continue
  fi

  if ! (cd "$dir" && "$PS2BUILD" build $flags -c "$config" -o "$out" >"$out.log" 2>&1); then
    echo "FAIL  $name (build failed, see $dir/$out.log)"
    fail=$((fail + 1))
    continue
  fi

  elf="$dir/$out/bin/$name.elf"
  log="$dir/$out/pcsx2.log"
  rm -f "$log"
  "$PCSX2" -elf "$(native_path "$elf")" -nogui -batch -earlyconsolelog -logfile "$(native_path "$log")" >/dev/null 2>&1 &
  emu=$!

  result=""
  for _ in $(seq 1 "$TIMEOUT"); do
    sleep 1
    if [ -f "$log" ] && grep -qE "$name: (PASS|FAIL)" "$log"; then
      result="$(grep -oE "$name: (PASS|FAIL)[^\r]*" "$log" | head -1 | tr -d '\r')"
      break
    fi
  done
  stop_emulator "$emu"
  wait "$emu" 2>/dev/null

  case "$result" in
    *": PASS"*) echo "PASS  $name"; pass=$((pass + 1)) ;;
    "")         echo "FAIL  $name (no result within ${TIMEOUT}s, see $log)"; fail=$((fail + 1)) ;;
    *)          echo "FAIL  $name ($result)"; fail=$((fail + 1)) ;;
  esac
done

echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
