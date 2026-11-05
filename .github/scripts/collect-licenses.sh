#!/bin/sh
# usage: collect-licenses.sh <dest-dir>   (run from the repo root)
set -e

DEST="$1"
if [ -z "$DEST" ]; then
  echo "usage: collect-licenses.sh <dest-dir>" >&2
  exit 1
fi

mkdir -p "$DEST"
cp LICENSE "$DEST/LICENSE"

ALLOWED_MISSING=""

is_allowed_missing() {
  for m in $ALLOWED_MISSING; do
    if [ "$m" = "$1" ]; then
      return 0
    fi
  done
  return 1
}

find_license_file() {
  dir="$1"
  for name in LICENSE LICENSE.TXT LICENSE.md COPYING COPYING.LIB COPYRIGHT; do
    for f in "$dir"/*; do
      [ -f "$f" ] || continue
      base="$(basename "$f")"
      lower_base="$(printf '%s' "$base" | tr '[:upper:]' '[:lower:]')"
      lower_name="$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]')"
      if [ "$lower_base" = "$lower_name" ]; then
        printf '%s\n' "$f"
        return 0
      fi
    done
  done
  return 1
}

missing=0

for dir in $(awk '/^  - /{print $2}' ps2workspace.yaml | cut -d/ -f1 | sort -u); do
  [ -d "$dir" ] || continue

  if lic="$(find_license_file "$dir")"; then
    mkdir -p "$DEST/$dir"
    cp "$lic" "$DEST/$dir/$(basename "$lic")"
    continue
  fi

  pkg_license="$(grep '^license:' "$dir/package.yaml" 2>/dev/null | sed 's/^license: *//; s/"//g')"
  if [ "$pkg_license" = "AFL-2.0" ]; then
    continue
  fi

  if is_allowed_missing "$dir"; then
    echo "NOTE: $dir ($pkg_license) has no license file of its own" >&2
    continue
  fi

  echo "ERROR: $dir ($pkg_license) has no license file of its own and is not AFL-2.0" >&2
  missing=1
done

[ "$missing" -eq 0 ]
