/**
 * @file knob.h
 *
 * Tuning values for the VR modules.
 */
#pragma once

#include <SDL.h>

namespace devilution {
namespace vr {

/** Tuning knob with an environment override, e.g. `DEVILUTIONX_VR_SCREEN_WIDTH=3.2`. */
inline float Knob(const char *name, float fallback)
{
	const char *value = SDL_getenv(name);
	return value != nullptr ? static_cast<float>(SDL_atof(value)) : fallback;
}

/**
 * The plain first-person Diablo look (`DEVILUTIONX_VR_CLASSIC`, default 1): walls and floors are
 * the least geometry the art needs, wearing the game's own tile pictures, as high as their
 * painting. 0 brings back the material libraries and grit; each of those also keeps its own knob.
 */
inline bool Classic()
{
	static const bool classic = Knob("DEVILUTIONX_VR_CLASSIC", 1) != 0;
	return classic;
}

} // namespace vr
} // namespace devilution
