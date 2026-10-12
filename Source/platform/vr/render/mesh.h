/**
 * @file mesh.h
 *
 * Mesh building, procedural shapes and skeletal animation.
 *
 * Final art does not exist yet for most of the game, so everything here can be made
 * from code: the level's geometry, stand-in weapons and a jointed stand-in figure.
 * The formats are the ones loaded assets will use (indexed triangles, four joint
 * weights per vertex, keyframed clips), so swapping in real models changes no renderer code.
 */
#pragma once

#include <vector>

#include "platform/vr/render/scene.h"

namespace devilution {
namespace vr {
namespace render {

/** Packs a colour (sRGB, 0..1) and a glow amount (0..1, scaled up in the shader) into Vertex::color. */
uint32_t PackColor(float r, float g, float b, float glow = 0);

struct MeshBuilder {
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;

	void Clear();
	/** Corners in order around the quad; the normal is taken from them. `uv` per corner, may be null. */
	void Quad(const Vec3 corners[4], const float uv[4][2], float layer, uint32_t color = 0x00FFFFFF);
	void Triangle(const Vec3 corners[3], const float uv[3][2], float layer, uint32_t color = 0x00FFFFFF);
	/** A box of the given half-extents, transformed. All vertices follow `joint`. */
	void Box(const Mat34 &transform, Vec3 halfExtents, uint32_t color, uint8_t joint = 0);
	/**
	 * A capped tube around the local Y axis from y = 0 to y = length, following `joint`. Its lower
	 * ring is shared half and half with `jointFrom` and its upper ring with `jointTo` (when they
	 * differ from `joint`), so the skin stays closed where two tubes meet at a bending joint.
	 */
	void Tube(const Mat34 &transform, float radiusFrom, float radiusTo, float length, int sides, uint32_t color, uint8_t joint = 0, uint8_t jointFrom = 0, uint8_t jointTo = 0);
	/** Uploads what was built. An empty builder gives a mesh that draws nothing. */
	Mesh Build() const;
};

struct Joint {
	/** -1 for the root. Parents come before their children. */
	int parent;
	/** Rest position relative to the parent. */
	Vec3 offset;
};

struct Keyframe {
	float time;
	Quat rotation;
	Vec3 translation;
};

/** One track per joint; a joint without keys stays in its rest pose. Translation keys add to the rest offset. */
struct Clip {
	float duration;
	bool loops;
	std::vector<std::vector<Keyframe>> tracks;
};

struct Skeleton {
	std::vector<Joint> joints;
	/** Model space to joint space in the rest pose. */
	std::vector<Mat34> inverseBind;

	void Finish();
	/** Appends the skin matrices (model space, rest pose to animated pose) of `clip` at `time`. */
	void Pose(const Clip &clip, float time, std::vector<Mat34> &out) const;
};

/** The stand-in figure used where no model exists: a jointed humanoid, 1.8 m tall, feet at the origin, facing +Z. */
struct Figure {
	Mesh mesh;
	Skeleton skeleton;
	enum Action : uint8_t {
		Idle,
		Walk,
		Attack,
		Hit,
		Death,
		ActionCount,
	};
	Clip clips[ActionCount];
};

void BuildFigure(Figure &figure);

/** A unit cube centred on the origin, for beams and markers. */
Mesh BuildCube();

} // namespace render
} // namespace vr
} // namespace devilution
