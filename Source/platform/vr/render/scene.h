/**
 * @file scene.h
 *
 * What the renderer draws in one frame, as plain data.
 *
 * This is the boundary between the game and the renderer: the bridge (level.cpp,
 * actors.cpp) reads game state and fills a Scene; the renderer (renderer.cpp) reads
 * the Scene and nothing else. No game type appears here and no D3D call is made by
 * the bridge, so either side can be replaced, or moved to another thread, on its own.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "platform/vr/render/gpu.h"
#include "platform/vr/render/math.h"

namespace devilution {
namespace vr {
namespace render {

constexpr int MaxLights = 32;
constexpr int MaxViews = 2;
constexpr float NearPlane = 0.05F;
constexpr float FarPlane = 400.0F;
/** Vertex::layer values from here on index the model texture array (Environment::models), whose images repeat. */
constexpr float ModelLayerBase = 4096.0F;
/**
 * A level surface that wears a material has (material + 1) * MaterialLayerStep added to its layer.
 * The material, a seamless texture laid out by world position, is the surface; the layer's own
 * picture is kept only where its alpha says it differs from the material (stains, arches, shadows).
 */
constexpr float MaterialLayerStep = 8192.0F;

/** One eye, or the single camera of a desktop/debug render. */
struct View {
	Vec3 position;
	Quat orientation;
	/** Tangents of the half-angles: left, right, up, down. */
	float fov[4];
};

struct Vertex {
	float position[3];
	float normal[3];
	float uv[2];
	/** Layer of the level's surface texture array, ModelLayerBase + a model texture, or negative for an untextured (vertex-coloured) surface. */
	float layer;
	/** RGBA, multiplied with the texture. */
	uint32_t color;
	uint8_t joints[4];
	/** Sum to 255. Ignored by instances that are not skinned. */
	uint8_t weights[4];
};

struct Mesh {
	ComPtr<ID3D11Buffer> vertices;
	ComPtr<ID3D11Buffer> indices;
	uint32_t indexCount = 0;
	uint32_t bytes = 0;
	Vec3 boundsMin {};
	Vec3 boundsMax {};

	Mesh() = default;
	Mesh(Mesh &&other) noexcept { *this = std::move(other); }
	Mesh &operator=(Mesh &&other) noexcept;
	~Mesh();
};

/** Per-instance data of a mesh draw, laid out as the shader's MeshInstance. */
struct MeshInstance {
	Mat34 world;
	/** rgb multiplies the surface colour; a > 0 makes it glow by that much (HDR). */
	float color[4];
	/** First bone of this instance in the frame's bone buffer, or -1 when not skinned. */
	float boneBase;
	float specular;
	/** 1 = receives light, 0 = drawn as is (UI-like world geometry such as the aim beam). */
	float lit;
	float pad;
};

struct MeshDraw {
	const Mesh *mesh;
	MeshInstance instance;
	bool castsShadow;
};

/**
 * Per-instance data of a model carved from the game's sprites, laid out as the shader's CarvedInstance.
 * Its colours are palette indices in the sprite atlas: one baked texture, read through the vertices' uv.
 */
struct CarvedInstance {
	Mat34 world;
	/** Unused; self-lit amount; unused; unused. */
	float params[4];
	/** Where the texture sits in the atlas: left, top, width, height in texels. */
	float rect[4];
	/** texel = xy + uv * z. */
	float map[4];
};

struct CarvedDraw {
	const Mesh *mesh;
	CarvedInstance instance;
};

/** A textured quad from the sprite atlas: pos + axisU * [-1, 1] + axisV * [0, 1]. */
struct SpriteInstance {
	float position[3];
	/** 0 = lit like a surface facing the viewer, > 0 = self-lit by that much (HDR). */
	float emissive;
	float axisU[3];
	/** Sprites lit as flat ground decals (corpses) use the up normal. */
	float flat;
	float axisV[3];
	/** Meters an upright sprite bulges towards the viewer per art pixel of distance to its outline (0 = flat). */
	float relief;
	/** Texels in the atlas: left, top, right, bottom. */
	float uv[4];
	float tint[4];
};

/** A soft disc. Colour is premultiplied: alpha 0 adds light, colour 0 darkens (blob shadows). */
struct ParticleInstance {
	float position[3];
	float size;
	float color[4];
	/** 1 = lies on the ground, 0 = faces the viewer. */
	float flat;
	float pad[3];
};

struct Light {
	float position[3];
	float range;
	float color[3];
	/** 1 = walls cast shadows from this light. */
	float shadowed;
};

/** A flat panel floating in the world, drawn after tone mapping so it keeps its own colours (UI). */
struct Panel {
	ID3D11ShaderResourceView *texture;
	Vec3 center;
	/** Half-extents along the panel's right and up directions, in meters. */
	Vec3 right;
	Vec3 up;
	float opacity;
};

/** Per-level look: fog, ambient light, sun (town only), tone mapping. */
struct Environment {
	float fogColor[3];
	float fogDensity;
	/** How far the eye sees, in meters: from a little over half of that everything sinks into this colour (as shown on the display, linear). */
	float viewFogColor[3];
	float viewDistance;
	float ambient[3];
	/** Direction towards the sun; intensity 0 turns it and its shadow pass off. */
	Vec3 sunDirection;
	float sunColor[3];
	float exposure;
	float bloom;
	/** Centre of the area the sun's shadow map covers. */
	Vec3 shadowCenter;
	/** Meters per tile and tiles per level side: maps world x,z onto the level textures below. */
	float tileSize;
	float levelTiles;
	/** Height at which surfaces start fading to black, and the meters it takes (the roofless dark above a dungeon). */
	float abyssStart;
	float abyssFade;
	/** Surface colour and normal arrays (Vertex::layer indexes them), baked light, wall occupancy, per-tile light masks. */
	ID3D11ShaderResourceView *surfaces;
	ID3D11ShaderResourceView *surfaceNormals;
	ID3D11ShaderResourceView *lightmap;
	ID3D11ShaderResourceView *occupancy;
	ID3D11ShaderResourceView *sprites;
	ID3D11ShaderResourceView *models;
	/** Palette index x light level to colour, and the same for colours (see palette.cpp). */
	/** The tile set's material library and its relief maps; null when it has none. */
	ID3D11ShaderResourceView *materials;
	ID3D11ShaderResourceView *materialNormals;
	/** Meters one repeat of a material covers. */
	float materialSize;
	/**
	 * Floors: meters one repeat of a floor material covers (0 = as walls), and the level's floor
	 * map (8 texels per tile: the two materials that meet there, how far the second shows, and
	 * how far the nearest wall is). Null when the level has none.
	 */
	float floorSize;
	ID3D11ShaderResourceView *floorMap;
	ID3D11ShaderResourceView *paletteLit;
	ID3D11ShaderResourceView *paletteCube;
	/** 1 = surfaces and sprites are shaded through the game's light tables, 0 = colour times light. */
	float paletteLight;
	/** Light that counts as fully lit is 1 / this. */
	float paletteGain;
	/** Comfort while moving or turning: how much of the edge of view is darkened (0..1). */
	float vignette;
};

struct Scene {
	Environment environment {};
	std::vector<MeshDraw> meshes;
	std::vector<CarvedDraw> carved;
	/** Skin matrices of all skinned instances, MeshInstance::boneBase indexes it. */
	std::vector<Mat34> bones;
	std::vector<SpriteInstance> sprites;
	std::vector<ParticleInstance> particles;
	std::vector<Panel> panels;
	Light lights[MaxLights];
	int lightCount = 0;
	/** Billboards turn towards the head, not towards each eye, or the two eyes would disagree. */
	Vec3 headPosition {};
	Vec3 headRight { 1, 0, 0 };
	Vec3 headUp { 0, 1, 0 };
	float time = 0;

	/** Empties the lists and keeps their storage: a steady frame allocates nothing. */
	void Clear()
	{
		meshes.clear();
		carved.clear();
		bones.clear();
		sprites.clear();
		particles.clear();
		panels.clear();
		lightCount = 0;
	}
};

} // namespace render
} // namespace vr
} // namespace devilution
