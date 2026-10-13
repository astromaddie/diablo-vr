#!/usr/bin/env bash
# What Steam runs for the devkit title DiabloVR (deploy.sh registers it). Starting the game from
# Steam is also what gives it the controllers: a binary started over SSH sees none.
#
# The package holds no game data. The user's own DIABDAT.MPQ goes in the data folder
# (${XDG_DATA_HOME:-$HOME/.local/share}/DiabloVR, deploy.sh --data copies it there). The same
# folder gets the saves, diablo.ini and run.log. The game's own assets are in assets/ beside
# the executable, which it finds through SDL_GetBasePath().
#
# run.log holds the output of this launch, run.prev.log the one before.
#
# For a test run with other arguments, put them in launch-once.args in the data folder. They are
# added to the command line, and the file is deleted before the game starts, so the next normal
# launch does not get them.
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$here"

# The Frame runs gamescope with Xwayland on :0.
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
export DISPLAY="${DISPLAY:-:0}"

data="${XDG_DATA_HOME:-$HOME/.local/share}/DiabloVR"
mkdir -p "$data"

log="$data/run.log"
[[ ! -e "$log" ]] || mv -f "$log" "$data/run.prev.log"

once="$data/launch-once.args"
extra=()
if [[ -f "$once" ]]; then
	while IFS= read -r line || [[ -n "$line" ]]; do
		[[ "$line" =~ ^[[:space:]]*(#|$) ]] && continue
		read -r -a words <<< "$line"
		extra+=("${words[@]}")
	done < "$once"
	rm -f "$once"
fi

{
	echo "[launch] $(date -u +%Y-%m-%dT%H:%M:%SZ) $here/devilutionx $*${extra[*]:+ (once: ${extra[*]})}"
	echo "[launch] data dir $data"
	[[ -e "$data/DIABDAT.MPQ" || -e "$data/diabdat.mpq" ]] || echo "[launch] no DIABDAT.MPQ in $data"
} > "$log"

exec ./devilutionx --data-dir "$data" --save-dir "$data" --config-dir "$data" \
	"$@" ${extra[@]+"${extra[@]}"} >> "$log" 2>&1
