#!/usr/bin/env sh

# Remove old Gamescope default configs and add our own. $1 is the data directory name.
d="${DESTDIR}/${MESON_INSTALL_PREFIX}/share/${1:-gamescope}"
mkdir -p "$d"
rm -rf "$d/scripts" || true
rm -rf "$d/looks" || true
cp -r "${MESON_SOURCE_ROOT}/scripts" "$d/scripts"
cp -r "${MESON_SOURCE_ROOT}/looks" "$d/looks"
