#!/usr/bin/env bash
# Builds the VR mode for the Steam Frame (Linux ARM64) inside the pinned environment from
# docker/linux.Dockerfile. The target architecture is the Docker host's: arm64 on Apple Silicon.
#
#   tools/steam_frame/build.sh [step...]
#
# Steps (default: image dxvk game package):
#   image    build the Docker image
#   dxvk     fetch dxvk-native at the pinned commit, apply patches/dxvk-*.patch and build its
#            d3d11 and dxgi into build-frame/dxvk-install
#   probe    build the OpenXR probe (tools/steam_frame/probe) into build-frame/probe
#   game     configure and build the game with DEVILUTIONX_VR=ON into build-frame/game
#   package  stage build-frame/package/DiabloVR: the game, its libraries, assets and the launcher
#
# Everything is built under build-frame/, which git ignores.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
image="${DIABLO_VR_IMAGE:-diablo-vr-linux}"
out=build-frame
dxvk_tag=v3.1.1
dxvk_commit=b1a1c99ab52b687cf950d62c88bc2fa316b41663

in_container() {
	docker run --rm -v "$repo":/src -w /src "$image" bash -c "set -euo pipefail; $1"
}

step_image() {
	docker build -t "$image" -f "$repo/tools/steam_frame/docker/linux.Dockerfile" "$repo/tools/steam_frame/docker"
}

step_dxvk() {
	local src="$repo/$out/dxvk-src"
	if [ ! -d "$src/.git" ]; then
		git clone -q --depth 1 --branch "$dxvk_tag" --recurse-submodules --shallow-submodules https://github.com/doitsujin/dxvk.git "$src"
	fi
	[ "$(git -C "$src" rev-parse HEAD)" = "$dxvk_commit" ] || { echo "dxvk is not at $dxvk_commit" >&2; exit 1; }
	# Back to the pinned tree, then the patches, so a changed patch is picked up.
	git -C "$src" reset -q --hard
	git -C "$src" clean -q -fd -- src include
	for patch in "$repo"/tools/steam_frame/patches/dxvk-*.patch; do
		git -C "$src" apply "$patch"
	done
	in_container "
		cd $out/dxvk-src
		[ -f /src/$out/dxvk-build/build.ninja ] || meson setup --buildtype release --strip --prefix /src/$out/dxvk-install --libdir lib \
			-Denable_dxgi=true -Denable_d3d11=true -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false \
			-Dnative_sdl2=enabled -Dnative_sdl3=disabled -Dnative_glfw=disabled -Dbuild_id=false \
			--force-fallback-for=libdisplay-info /src/$out/dxvk-build
		ninja -C /src/$out/dxvk-build install > /dev/null
		rm -f subprojects/.wraplock
		ls -l /src/$out/dxvk-install/lib
	"
}

step_probe() {
	in_container "
		cmake -S tools/steam_frame/probe -B $out/probe -GNinja -DCMAKE_BUILD_TYPE=Release -DDXVK_PREFIX=/src/$out/dxvk-install
		cmake --build $out/probe
	"
}

[ $# -gt 0 ] || set -- image dxvk game package
for step in "$@"; do
	case "$step" in
	image | dxvk | probe | game | package) "step_$step" ;;
	*)
		echo "unknown step: $step" >&2
		exit 1
		;;
	esac
done
