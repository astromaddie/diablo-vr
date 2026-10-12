/**
 * @file palette.cpp
 *
 * The game's palette and light tables as textures, so the 3D view is shaded the way the game
 * shades: a colour at light level L is the palette entry its light table names, not the
 * colour multiplied by a brightness. This is what keeps dark stone the dungeon's own dark
 * colours.
 *
 * Two lookups are kept:
 *  - `lit`: 256 x 16, palette index and light level to colour. Sprites are stored as indices
 *    and use it directly. Refreshed whenever the palette moves (lava and water cycle).
 *  - `cube`: for surfaces that are stored as colours. A 32-step RGB cube per light level
 *    (layer = blue + 32 * level) holding, for each colour, the nearest palette colour at that
 *    level. Built when a level is entered.
 */
#include "platform/vr/render/bridge.h"

#include <cstring>

#include "engine/light_tables.hpp"
#include "engine/palette.h"

namespace devilution {
namespace vr {
namespace render {

namespace {

constexpr int CubeSize = 32;

Texture lit, cube;
uint32_t litHash;
bool cubeBuilt;

uint32_t Hash(const void *data, size_t size, uint32_t hash = 2166136261U)
{
	const auto *bytes = static_cast<const uint8_t *>(data);
	for (size_t i = 0; i < size; i++)
		hash = (hash ^ bytes[i]) * 16777619U;
	return hash;
}

void BuildColorCube()
{
	// Nearest palette entry of every cell, once; the light tables then give it at each level.
	static uint8_t nearest[CubeSize * CubeSize * CubeSize];
	for (int b = 0; b < CubeSize; b++) {
		for (int g = 0; g < CubeSize; g++) {
			for (int r = 0; r < CubeSize; r++) {
				const int cr = r * 8 + 4;
				const int cg = g * 8 + 4;
				const int cb = b * 8 + 4;
				int best = 1 << 30;
				uint8_t bestIndex = 0;
				for (int i = 0; i < 256; i++) {
					const SDL_Color color = logical_palette[i];
					// Green counts most and blue least, as the eye has it.
					const int distance = 3 * (color.r - cr) * (color.r - cr) + 6 * (color.g - cg) * (color.g - cg) + (color.b - cb) * (color.b - cb);
					if (distance < best) {
						best = distance;
						bestIndex = static_cast<uint8_t>(i);
					}
				}
				nearest[(b * CubeSize + g) * CubeSize + r] = bestIndex;
			}
		}
	}
	static uint32_t slice[CubeSize * CubeSize];
	for (int level = 0; level < static_cast<int>(NumLightingLevels); level++) {
		for (int b = 0; b < CubeSize; b++) {
			for (int i = 0; i < CubeSize * CubeSize; i++)
				slice[i] = PaletteRgba(LightTables[level][nearest[b * CubeSize * CubeSize + i]], 255);
			UploadTexture(cube, b + CubeSize * level, 0, 0, CubeSize, CubeSize, slice, 4);
		}
	}
	cubeBuilt = true;
}

} // namespace

bool PaletteInit()
{
	lit = CreateTexture(256, NumLightingLevels, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	cube = CreateTexture(CubeSize, CubeSize, CubeSize * NumLightingLevels, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	litHash = 0;
	cubeBuilt = false;
	return lit && cube;
}

void PaletteRelease()
{
	lit.Release();
	cube.Release();
}

void PaletteLevelChanged()
{
	cubeBuilt = false;
}

void PaletteSync()
{
	const uint32_t hash = Hash(LightTables.data(), sizeof(LightTables), Hash(logical_palette.data(), sizeof(logical_palette)));
	if (hash != litHash) {
		litHash = hash;
		static uint32_t pixels[256 * NumLightingLevels];
		for (size_t level = 0; level < NumLightingLevels; level++) {
			for (int i = 0; i < 256; i++)
				pixels[level * 256 + i] = PaletteRgba(LightTables[level][i], 255);
		}
		UploadTexture(lit, 0, 0, 0, 256, NumLightingLevels, pixels, 4);
	}
	if (!cubeBuilt)
		BuildColorCube();
}

ID3D11ShaderResourceView *PaletteLitView()
{
	return lit.srv.Get();
}

ID3D11ShaderResourceView *PaletteCubeView()
{
	return cube.srv.Get();
}

} // namespace render
} // namespace vr
} // namespace devilution
