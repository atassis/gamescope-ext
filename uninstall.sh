#!/usr/bin/env bash
# Removes what install.sh installed, from the list it recorded. Plugins are removed by
# their own installers.
#
#   uninstall.sh [--prefix DIR] [--name NAME]
set -euo pipefail

prefix=$HOME/.local
name=gamescope-ext

while [ $# -gt 0 ]; do
	case "$1" in
		--prefix) prefix=$2; shift 2 ;;
		--name) name=$2; shift 2 ;;
		*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

list=$prefix/share/$name/installed-files
[ -f "$list" ] || { echo "no $list: nothing installed as $name under $prefix" >&2; exit 1; }

parents=("$prefix/share/$name/x")
while IFS= read -r f; do
	case "$f" in
		'#'*|''|"$prefix") continue ;;
		"$prefix"/*) ;;
		*) echo "skipping $f: outside $prefix" >&2; continue ;;
	esac
	[ -d "$f" ] && [ ! -L "$f" ] && continue
	rm -f -- "$f"
	parents+=("$f")
done <"$list"
rm -rf -- "$prefix/share/$name"
# Drop directories this left empty, up to but not including the prefix.
for f in "${parents[@]}"; do
	d=$(dirname -- "$f")
	while [ "$d" != "$prefix" ] && rmdir -- "$d" 2>/dev/null; do d=$(dirname -- "$d"); done
done
echo "removed $name from $prefix"
