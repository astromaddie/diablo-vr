/**
 * @file gltf.h
 *
 * Reads binary glTF 2.0 (.glb) files: static triangle meshes with one base colour texture
 * per material, which is what the model pack in `3dobjects-vr` consists of. Nodes are
 * flattened into scene space. No game types and no D3D here.
 *
 * Not read: skins, animations, cameras, vertex colours, anything but the base colour
 * texture, external buffers and images (a .glb carries its own).
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <SDL.h>

#include "platform/vr/render/math.h"

namespace devilution {
namespace vr {
namespace render {

struct GltfImage {
	std::string name;
	/** RGBA32, or null when the image could not be decoded. */
	SDL_Surface *pixels = nullptr;
};

/** The triangles of one mesh primitive of one node, in scene space. */
struct GltfPrimitive {
	std::string node;
	std::string material;
	/** Index into GltfScene::images, or -1 for an untextured material. */
	int image = -1;
	std::vector<Vec3> positions;
	std::vector<Vec3> normals;
	/** u, v per vertex; v runs downwards as in the image. */
	std::vector<float> uvs;
	std::vector<uint32_t> indices;
};

struct GltfScene {
	std::vector<GltfImage> images;
	std::vector<GltfPrimitive> primitives;

	GltfScene() = default;
	GltfScene(const GltfScene &) = delete;
	GltfScene &operator=(const GltfScene &) = delete;
	~GltfScene();
};

/** Logs the reason and returns false when the file is missing or not a .glb this loader understands. */
bool LoadGlb(const char *path, GltfScene &scene);

} // namespace render
} // namespace vr
} // namespace devilution
