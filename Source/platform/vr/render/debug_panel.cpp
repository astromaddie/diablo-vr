/**
 * @file debug_panel.cpp
 *
 * The performance panel: frame timings, draw counts and memory as text on a panel that
 * floats in the world, low in front of the viewer (DEVILUTIONX_VR_PERF=1). It is the first
 * user of the world-space panels the in-game interface will be built from.
 */
#include "platform/vr/render/bridge.h"

#include <cstring>
#include <vector>

#include "engine/palette.h"
#include "engine/render/text_render.hpp"
#include "engine/surface.hpp"
#include "platform/vr/debug/profile.h"

namespace devilution {
namespace vr {
namespace render {

namespace {

constexpr int Width = 448;
constexpr int Height = 96;

const bool Enabled = SDL_getenv("DEVILUTIONX_VR_PERF") != nullptr;
Texture texture;
Uint32 nextUpdate;

void Redraw()
{
	static OwnedSurface surface { Width, Height };
	for (int y = 0; y < Height; y++)
		std::memset(surface.at(0, y), 0, Width);
	const FrameSample &sample = ProfileLatest();
	const auto megabytes = [](MemoryKind kind) { return static_cast<double>(memoryBytes[static_cast<size_t>(kind)]) / 1048576.0; };
	char lines[6][96];
	SDL_snprintf(lines[0], sizeof(lines[0]), "%.0f FPS   CPU %.2f MS   GPU %.2f MS", ProfileFps(), sample.cpuMs, sample.gpuMs);
	SDL_snprintf(lines[1], sizeof(lines[1]), "EXTRACT %.2f  RENDER %.2f", sample.cpuScopeMs[0], sample.cpuScopeMs[1]);
	SDL_snprintf(lines[2], sizeof(lines[2]), "SHADOW %.2f  SCENE %.2f  POST %.2f", sample.gpuPassMs[0], sample.gpuPassMs[1], sample.gpuPassMs[2]);
	SDL_snprintf(lines[3], sizeof(lines[3]), "%u DRAWS  %u TRIS  %u LIGHTS", sample.drawCalls, sample.triangles, sample.lights);
	SDL_snprintf(lines[4], sizeof(lines[4]), "%u MESHES  %u SPRITES  %u PARTICLES", sample.meshInstances, sample.sprites, sample.particles);
	SDL_snprintf(lines[5], sizeof(lines[5]), "GPU MB: TARGETS %.0f  TEX %.0f  GEO %.1f", megabytes(MemoryKind::Targets), megabytes(MemoryKind::Textures), megabytes(MemoryKind::Geometry));
	int y = 2;
	for (const char *line : lines) {
		DrawString(surface, line, Rectangle { { 6, y }, { Width - 12, 0 } }, { .flags = UiFlags::ColorWhite });
		y += 15;
	}
	static std::vector<uint32_t> pixels(static_cast<size_t>(Width) * Height);
	for (int row = 0; row < Height; row++) {
		const uint8_t *source = surface.at(0, row);
		for (int x = 0; x < Width; x++) {
			// Index 0 is the cleared background: a dark, mostly opaque backdrop for the text.
			pixels[static_cast<size_t>(row) * Width + x] = source[x] == 0 ? 0xB0000000 : PaletteRgba(source[x], 255);
		}
	}
	UploadTexture(texture, 0, 0, 0, Width, Height, pixels.data(), 4);
}

} // namespace

void DebugPanelAddToScene(Scene &scene)
{
	if (!Enabled)
		return;
	if (!texture)
		texture = CreateTexture(Width, Height, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	if (SDL_TICKS_PASSED(SDL_GetTicks(), nextUpdate)) {
		nextUpdate = SDL_GetTicks() + 250;
		Redraw();
	}
	const Vec3 forward = Cross(scene.headUp, scene.headRight);
	constexpr float HalfWidth = 0.17F;
	Panel panel {};
	panel.texture = texture.srv.Get();
	panel.center = scene.headPosition + forward * 0.8F - scene.headUp * 0.26F - scene.headRight * 0.2F;
	panel.right = scene.headRight * HalfWidth;
	panel.up = scene.headUp * (HalfWidth * Height / Width);
	panel.opacity = 0.9F;
	scene.panels.push_back(panel);
}

void DebugPanelRelease()
{
	texture.Release();
}

} // namespace render
} // namespace vr
} // namespace devilution
