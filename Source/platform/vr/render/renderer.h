/**
 * @file renderer.h
 *
 * The 3D renderer: turns a Scene into pictures. Knows nothing about the game or OpenXR.
 *
 * Passes, in order:
 *  1. sun shadow map (only when the scene has a sun)
 *  2. scene: opaque meshes and sprites into a multisampled HDR target, both eyes in one
 *     pass where the GPU allows; then particles
 *  3. resolve, bloom, tone mapping into the caller's colour (and depth) targets
 *  4. panels (UI in the world), straight onto the tone-mapped picture
 */
#pragma once

#include "platform/vr/render/scene.h"

namespace devilution {
namespace vr {
namespace render {

extern const char ShaderSource[];

/** Where one view's finished picture goes. */
struct Output {
	ID3D11RenderTargetView *color;
	/** Optional: receives the scene's depth, for compositors that reproject with it. */
	ID3D11DepthStencilView *depth;
	/** Optional: triangles (x, y pairs in view space at z = -1) the lenses never show. */
	ID3D11Buffer *maskVertices;
	uint32_t maskVertexCount;
};

struct RenderStats {
	uint32_t drawCalls;
	uint32_t triangles;
	uint32_t meshInstances;
	uint32_t sprites;
	uint32_t particles;
	uint32_t lights;
};

bool RendererInit(ID3D11Device *device, ID3D11DeviceContext *context);
void RendererRelease();

/**
 * Draws the scene once per view. All views have the same size.
 * @param scene mesh draws are culled and sorted here; the caller only lists what exists
 */
void RendererRender(const Scene &scene, const View *views, int viewCount, int width, int height, const Output *outputs);

/** Resolved, tone-mapped picture of view 0 of the last render, for mirrors and captures. */
void RendererBlitView(ID3D11RenderTargetView *target, int width, int height);

const RenderStats &RendererStats();

Mat4 ViewProjection(const View &view);

} // namespace render
} // namespace vr
} // namespace devilution
