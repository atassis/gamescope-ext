#!/usr/bin/env bash
# Builds this compositor under its own name and installs it beside, never over, a system gamescope:
# executable <name> (+ <name>ctl, <name>reaper, ...), the <name>-run launcher, Vulkan WSI layer
# VK_LAYER_<name>_wsi enabled by ENABLE_<NAME>_WSI, data in <prefix>/share/<name>. Upscaler plugins
# are installed separately (each plugin has its own installer).
#
#   install.sh [--prefix DIR] [--name NAME] [--build-dir DIR] [--jobs N]
#
# Defaults: --prefix ~/.local, --name gamescope-ext. The installed file list is kept in
# <prefix>/share/<name>/installed-files for uninstall.sh.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
prefix=$HOME/.local
name=gamescope-ext
build=
jobs=$(( $(nproc) / 2 ))

while [ $# -gt 0 ]; do
	case "$1" in
		--prefix) prefix=$2; shift 2 ;;
		--name) name=$2; shift 2 ;;
		--build-dir) build=$2; shift 2 ;;
		--jobs) jobs=$2; shift 2 ;;
		*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

[[ $name =~ ^[a-z][a-z0-9-]*$ ]] || { echo "--name must be lowercase letters, digits and '-'" >&2; exit 2; }
[ "$name" != gamescope ] || { echo "--name gamescope would collide with a system gamescope" >&2; exit 2; }
build=${build:-${XDG_CACHE_HOME:-$HOME/.cache}/$name/build}
layer=${name//-/_}_wsi
env=$(tr 'a-z-' 'A-Z_' <<<"$name")_WSI

opts=(--prefix "$prefix" --buildtype release -Dexe_name="$name" -Dwsi_layer_name="$layer"
	-Dwsi_layer_env="$env" -Denable_tests=false)
if [ -d "$build/meson-private" ]; then
	meson setup --reconfigure "$build" "$here" "${opts[@]}"
else
	meson setup "$build" "$here" "${opts[@]}"
fi
ninja -C "$build" -j "$jobs"
meson install -C "$build" --no-rebuild --skip-subprojects

install -Dm755 "$here/tools/gamescope-ext-run" "$prefix/bin/$name-run"
install -Dm644 "$here/tools/run.conf.example" "$prefix/share/$name/run.conf.example"

list=$prefix/share/$name/installed-files
cp "$build/meson-logs/install-log.txt" "$list"
printf '%s\n' "$prefix/bin/$name-run" "$prefix/share/$name/run.conf.example" "$list" >>"$list"
echo "installed $prefix/bin/$name and $name-run (layer VK_LAYER_$layer, env ENABLE_$env); uninstall: $here/uninstall.sh --prefix $prefix --name $name"
