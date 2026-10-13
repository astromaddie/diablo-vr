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
# The VR mode is tuned with DEVILUTIONX_VR_* environment variables (the README lists them).
# vr-settings.env in the data folder holds the ones to keep, one NAME=value a line; deploy.sh
# puts the Frame's defaults there once and never overwrites the file.
#
# For a test run with other arguments or settings, put them in launch-once.args (arguments) or
# launch-once.env (NAME=value lines) in the data folder. They are used for one launch: the files
# are deleted before the game starts, so the next normal launch does not get them.
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$here"

# The Frame runs gamescope with Xwayland on :0.
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
export DISPLAY="${DISPLAY:-:0}"
# The Frame's Vulkan driver (Turnip) chooses per render pass between tiled rendering and
# drawing straight into memory. The renderer's passes are few and large, and drawing straight
# into memory measured 5 to 10 percent faster for them.
export TU_DEBUG="${TU_DEBUG-sysmem}"

data="${XDG_DATA_HOME:-$HOME/.local/share}/DiabloVR"
mkdir -p "$data"

log="$data/run.log"
[[ ! -e "$log" ]] || mv -f "$log" "$data/run.prev.log"

once="$data/launch-once.args"
extra=()
skip='^[[:space:]]*(#|$)' # blank lines and comments
if [[ -f "$once" ]]; then
	while IFS= read -r line || [[ -n "$line" ]]; do
		[[ "$line" =~ $skip ]] && continue
		read -r -a words <<< "$line"
		extra+=("${words[@]}")
	done < "$once"
	rm -f "$once"
fi

# NAME=value lines, DEVILUTIONX_VR_ names only: nothing else gets into the environment this way.
load_env() {
	local line
	[[ -f "$1" ]] || return 0
	while IFS= read -r line || [[ -n "$line" ]]; do
		[[ "$line" =~ ^(DEVILUTIONX_VR_[A-Z0-9_]+)=(.*)$ ]] || continue
		export "${BASH_REMATCH[1]}=${BASH_REMATCH[2]}"
		applied+=("${BASH_REMATCH[1]}=${BASH_REMATCH[2]}")
	done < "$1"
}
applied=()
load_env "$data/vr-settings.env"
once_env="$data/launch-once.env"
load_env "$once_env"
rm -f "$once_env"

{
	echo "[launch] $(date -u +%Y-%m-%dT%H:%M:%SZ) $here/devilutionx $*${extra[*]:+ (once: ${extra[*]})}"
	echo "[launch] data dir $data"
	echo "[launch] settings: ${applied[*]:-none}"
	[[ -e "$data/DIABDAT.MPQ" || -e "$data/diabdat.mpq" ]] || echo "[launch] no DIABDAT.MPQ in $data"
} > "$log"

exec ./devilutionx --data-dir "$data" --save-dir "$data" --config-dir "$data" \
	"$@" ${extra[@]+"${extra[@]}"} >> "$log" 2>&1
