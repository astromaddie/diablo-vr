/**
 * @file mesh.cpp
 *
 * Mesh building, procedural shapes and skeletal animation.
 */
#include "platform/vr/render/mesh.h"

#include <cstring>

namespace devilution {
namespace vr {
namespace render {

namespace {

constexpr float Pi = 3.14159265F;

Vertex MakeVertex(Vec3 position, Vec3 normal, float u, float v, float layer, uint32_t color, uint8_t joint = 0)
{
	Vertex vertex {};
	std::memcpy(vertex.position, &position, sizeof(vertex.position));
	std::memcpy(vertex.normal, &normal, sizeof(vertex.normal));
	vertex.uv[0] = u;
	vertex.uv[1] = v;
	vertex.layer = layer;
	vertex.color = color;
	vertex.joints[0] = joint;
	vertex.weights[0] = 255;
	return vertex;
}

Vec3 Direction(const Mat34 &m, Vec3 v)
{
	return Transform(m, v) - Transform(m, { 0, 0, 0 });
}

} // namespace

Mesh &Mesh::operator=(Mesh &&other) noexcept
{
	if (this != &other) {
		ReleaseStaticBytes(bytes);
		vertices = std::move(other.vertices);
		indices = std::move(other.indices);
		indexCount = other.indexCount;
		bytes = other.bytes;
		boundsMin = other.boundsMin;
		boundsMax = other.boundsMax;
		other.indexCount = 0;
		other.bytes = 0;
	}
	return *this;
}

Mesh::~Mesh()
{
	ReleaseStaticBytes(bytes);
}

uint32_t PackColor(float r, float g, float b, float glow)
{
	const auto channel = [](float value) { return static_cast<uint32_t>(std::clamp(value, 0.F, 1.F) * 255 + 0.5F); };
	return channel(r) | channel(g) << 8 | channel(b) << 16 | channel(glow) << 24;
}

void MeshBuilder::Clear()
{
	vertices.clear();
	indices.clear();
}

void MeshBuilder::Quad(const Vec3 corners[4], const float uv[4][2], float layer, uint32_t color)
{
	const Vec3 normal = Normalize(Cross(corners[1] - corners[0], corners[3] - corners[0]));
	const auto base = static_cast<uint32_t>(vertices.size());
	for (int i = 0; i < 4; i++)
		vertices.push_back(MakeVertex(corners[i], normal, uv != nullptr ? uv[i][0] : 0, uv != nullptr ? uv[i][1] : 0, layer, color));
	for (const uint32_t i : { 0U, 1U, 2U, 0U, 2U, 3U })
		indices.push_back(base + i);
}

void MeshBuilder::Triangle(const Vec3 corners[3], const float uv[3][2], float layer, uint32_t color)
{
	const Vec3 normal = Normalize(Cross(corners[1] - corners[0], corners[2] - corners[0]));
	for (int i = 0; i < 3; i++) {
		indices.push_back(static_cast<uint32_t>(vertices.size()));
		vertices.push_back(MakeVertex(corners[i], normal, uv != nullptr ? uv[i][0] : 0, uv != nullptr ? uv[i][1] : 0, layer, color));
	}
}

void MeshBuilder::Box(const Mat34 &transform, Vec3 h, uint32_t color, uint8_t joint)
{
	// Each face: a normal axis and the two axes spanning it.
	const Vec3 axes[6][3] = {
		{ { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } },
		{ { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
		{ { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 0 } },
		{ { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
		{ { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },
		{ { 0, 0, -1 }, { 0, 1, 0 }, { 1, 0, 0 } },
	};
	for (const auto &face : axes) {
		const auto scaled = [&](Vec3 axis) { return Vec3 { axis.x * h.x, axis.y * h.y, axis.z * h.z }; };
		const Vec3 n = scaled(face[0]);
		const Vec3 a = scaled(face[1]);
		const Vec3 b = scaled(face[2]);
		const Vec3 normal = Normalize(Direction(transform, face[0]));
		const auto base = static_cast<uint32_t>(vertices.size());
		for (const Vec3 corner : { n - a - b, n + a - b, n + a + b, n - a + b })
			vertices.push_back(MakeVertex(Transform(transform, corner), normal, 0, 0, -1, color, joint));
		for (const uint32_t i : { 0U, 1U, 2U, 0U, 2U, 3U })
			indices.push_back(base + i);
	}
}

void MeshBuilder::Tube(const Mat34 &transform, float radiusFrom, float radiusTo, float length, int sides, uint32_t color, uint8_t joint, uint8_t jointFrom, uint8_t jointTo)
{
	const auto base = static_cast<uint32_t>(vertices.size());
	for (int ring = 0; ring < 2; ring++) {
		const float radius = ring == 0 ? radiusFrom : radiusTo;
		const uint8_t other = ring == 0 ? jointFrom : jointTo;
		for (int i = 0; i < sides; i++) {
			const float angle = 2 * Pi * static_cast<float>(i) / static_cast<float>(sides);
			const Vec3 around { std::cos(angle), 0, std::sin(angle) };
			Vertex vertex = MakeVertex(Transform(transform, { around.x * radius, ring * length, around.z * radius }), Normalize(Direction(transform, around)), 0, 0, -1, color, joint);
			if (other != joint) {
				// The ring at a joint follows both bones equally, which keeps the skin closed when the joint bends.
				vertex.joints[1] = other;
				vertex.weights[0] = 128;
				vertex.weights[1] = 127;
			}
			vertices.push_back(vertex);
		}
	}
	for (int i = 0; i < sides; i++) {
		const uint32_t a = base + i;
		const uint32_t b = base + (i + 1) % sides;
		for (const uint32_t index : { a, b, b + sides, a, b + sides, a + sides })
			indices.push_back(index);
	}
	// Caps, as fans around the ring's first vertex.
	for (int ring = 0; ring < 2; ring++) {
		const auto capBase = static_cast<uint32_t>(vertices.size());
		const Vec3 normal = Normalize(Direction(transform, { 0, ring == 0 ? -1.F : 1.F, 0 }));
		for (int i = 0; i < sides; i++) {
			Vertex vertex = vertices[base + ring * sides + i];
			std::memcpy(vertex.normal, &normal, sizeof(vertex.normal));
			vertices.push_back(vertex);
		}
		for (int i = 1; i + 1 < sides; i++) {
			for (const uint32_t index : { capBase, capBase + i, capBase + i + 1 })
				indices.push_back(index);
		}
	}
}

Mesh MeshBuilder::Build() const
{
	Mesh mesh;
	if (indices.empty())
		return mesh;
	const auto vertexBytes = static_cast<uint32_t>(vertices.size() * sizeof(Vertex));
	const auto indexBytes = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));
	mesh.vertices = CreateStaticBuffer(vertices.data(), vertexBytes, D3D11_BIND_VERTEX_BUFFER);
	mesh.indices = CreateStaticBuffer(indices.data(), indexBytes, D3D11_BIND_INDEX_BUFFER);
	if (mesh.vertices == nullptr || mesh.indices == nullptr)
		return mesh;
	mesh.bytes = vertexBytes + indexBytes;
	mesh.indexCount = static_cast<uint32_t>(indices.size());
	mesh.boundsMin = { 1e9F, 1e9F, 1e9F };
	mesh.boundsMax = { -1e9F, -1e9F, -1e9F };
	for (const Vertex &vertex : vertices) {
		mesh.boundsMin = { std::min(mesh.boundsMin.x, vertex.position[0]), std::min(mesh.boundsMin.y, vertex.position[1]), std::min(mesh.boundsMin.z, vertex.position[2]) };
		mesh.boundsMax = { std::max(mesh.boundsMax.x, vertex.position[0]), std::max(mesh.boundsMax.y, vertex.position[1]), std::max(mesh.boundsMax.z, vertex.position[2]) };
	}
	return mesh;
}

void Skeleton::Finish()
{
	inverseBind.clear();
	std::vector<Vec3> rest;
	for (const Joint &joint : joints) {
		rest.push_back(joint.parent < 0 ? joint.offset : rest[joint.parent] + joint.offset);
		inverseBind.push_back(Compose(rest.back() * -1, IdentityQuat));
	}
}

void Skeleton::Pose(const Clip &clip, float time, std::vector<Mat34> &out) const
{
	constexpr size_t MaxJoints = 32;
	Mat34 global[MaxJoints];
	if (clip.loops)
		time = std::fmod(time, clip.duration);
	time = std::clamp(time, 0.F, clip.duration);
	for (size_t j = 0; j < joints.size() && j < MaxJoints; j++) {
		Quat rotation = IdentityQuat;
		Vec3 translation = joints[j].offset;
		if (j < clip.tracks.size() && !clip.tracks[j].empty()) {
			const std::vector<Keyframe> &keys = clip.tracks[j];
			size_t next = 0;
			while (next < keys.size() && keys[next].time < time)
				next++;
			const Keyframe &a = keys[next == 0 ? 0 : next - 1];
			const Keyframe &b = keys[std::min(next, keys.size() - 1)];
			const float t = b.time > a.time ? (time - a.time) / (b.time - a.time) : 0;
			rotation = Nlerp(a.rotation, b.rotation, t);
			translation = translation + Lerp(a.translation, b.translation, t);
		}
		const Mat34 local = Compose(translation, rotation);
		global[j] = joints[j].parent < 0 ? local : global[joints[j].parent] * local;
		out.push_back(global[j] * inverseBind[j]);
	}
}

void BuildFigure(Figure &figure)
{
	enum : uint8_t { Hips, Chest, Head, ArmL, ForearmL, ArmR, ForearmR, ThighL, ShinL, ThighR, ShinR, Count };
	Skeleton &skeleton = figure.skeleton;
	skeleton.joints = {
		{ -1, { 0, 0.95F, 0 } },
		{ Hips, { 0, 0.25F, 0 } },
		{ Chest, { 0, 0.33F, 0 } },
		{ Chest, { -0.23F, 0.26F, 0 } },
		{ ArmL, { 0, -0.30F, 0 } },
		{ Chest, { 0.23F, 0.26F, 0 } },
		{ ArmR, { 0, -0.30F, 0 } },
		{ Hips, { -0.11F, 0, 0 } },
		{ ThighL, { 0, -0.47F, 0 } },
		{ Hips, { 0.11F, 0, 0 } },
		{ ThighR, { 0, -0.47F, 0 } },
	};
	skeleton.Finish();

	// Vertex colour is white: the instance colour tints the whole figure.
	const uint32_t white = PackColor(1, 1, 1);
	const Quat down = AxisAngle({ 0, 0, 1 }, Pi);
	MeshBuilder builder;
	builder.Tube(Compose({ 0, 0.88F, 0 }, IdentityQuat), 0.15F, 0.17F, 0.32F, 10, white, Hips, Hips, Chest);
	builder.Tube(Compose({ 0, 1.20F, 0 }, IdentityQuat), 0.17F, 0.19F, 0.30F, 10, white, Chest, Hips, Chest);
	builder.Tube(Compose({ 0, 1.50F, 0 }, IdentityQuat), 0.05F, 0.05F, 0.06F, 8, white, Head, Chest, Head);
	builder.Box(Compose({ 0, 1.68F, 0.01F }, IdentityQuat), { 0.095F, 0.12F, 0.105F }, white, Head);
	for (const float side : { -1.F, 1.F }) {
		const uint8_t arm = side < 0 ? ArmL : ArmR;
		const uint8_t forearm = side < 0 ? ForearmL : ForearmR;
		const uint8_t thigh = side < 0 ? ThighL : ThighR;
		const uint8_t shin = side < 0 ? ShinL : ShinR;
		builder.Tube(Compose({ 0.23F * side, 1.46F, 0 }, down), 0.06F, 0.05F, 0.30F, 8, white, arm, Chest, forearm);
		builder.Tube(Compose({ 0.23F * side, 1.16F, 0 }, down), 0.05F, 0.04F, 0.28F, 8, white, forearm, arm, forearm);
		builder.Box(Compose({ 0.23F * side, 0.84F, 0 }, IdentityQuat), { 0.04F, 0.05F, 0.05F }, white, forearm);
		builder.Tube(Compose({ 0.11F * side, 0.95F, 0 }, down), 0.09F, 0.07F, 0.47F, 8, white, thigh, Hips, shin);
		builder.Tube(Compose({ 0.11F * side, 0.48F, 0 }, down), 0.07F, 0.05F, 0.44F, 8, white, shin, thigh, shin);
		builder.Box(Compose({ 0.11F * side, 0.035F, 0.06F }, IdentityQuat), { 0.05F, 0.035F, 0.12F }, white, shin);
	}
	figure.mesh = builder.Build();
	// Animation swings limbs well outside the rest pose.
	figure.mesh.boundsMin = { -1.2F, -0.2F, -1.4F };
	figure.mesh.boundsMax = { 1.2F, 2.2F, 1.4F };

	const Vec3 x { 1, 0, 0 };
	const Vec3 y { 0, 1, 0 };
	const auto turn = [](float time, Vec3 axis, float angle, Vec3 move = {}) { return Keyframe { time, AxisAngle(axis, angle), move }; };
	const auto clip = [](float duration, bool loops) {
		Clip result { duration, loops, {} };
		result.tracks.resize(Count);
		return result;
	};

	Clip idle = clip(2.4F, true);
	idle.tracks[Chest] = { turn(0, x, 0), turn(1.2F, x, 0.04F), turn(2.4F, x, 0) };
	idle.tracks[ArmL] = { turn(0, x, 0.05F), turn(1.2F, x, -0.03F), turn(2.4F, x, 0.05F) };
	idle.tracks[ArmR] = { turn(0, x, -0.03F), turn(1.2F, x, 0.05F), turn(2.4F, x, -0.03F) };
	figure.clips[Figure::Idle] = std::move(idle);

	Clip walk = clip(1.0F, true);
	for (const bool left : { true, false }) {
		const float s = left ? 1.F : -1.F;
		walk.tracks[left ? ThighL : ThighR] = { turn(0, x, 0.55F * s), turn(0.5F, x, -0.55F * s), turn(1, x, 0.55F * s) };
		walk.tracks[left ? ArmL : ArmR] = { turn(0, x, -0.45F * s), turn(0.5F, x, 0.45F * s), turn(1, x, -0.45F * s) };
		// The knee bends while its leg swings forward.
		walk.tracks[left ? ShinL : ShinR] = left
		    ? std::vector<Keyframe> { turn(0, x, 0.1F), turn(0.5F, x, 0.1F), turn(0.75F, x, 0.9F), turn(1, x, 0.1F) }
		    : std::vector<Keyframe> { turn(0, x, 0.1F), turn(0.25F, x, 0.9F), turn(0.5F, x, 0.1F), turn(1, x, 0.1F) };
		walk.tracks[left ? ForearmL : ForearmR] = { turn(0, x, -0.3F), turn(1, x, -0.3F) };
	}
	walk.tracks[Hips] = { turn(0, y, 0.08F, { 0, 0, 0 }), turn(0.25F, y, 0, { 0, -0.03F, 0 }), turn(0.5F, y, -0.08F), turn(0.75F, y, 0, { 0, -0.03F, 0 }), turn(1, y, 0.08F) };
	figure.clips[Figure::Walk] = std::move(walk);

	Clip attack = clip(1.0F, false);
	attack.tracks[ArmR] = { turn(0, x, -0.2F), turn(0.35F, x, -2.6F), turn(0.6F, x, -0.7F), turn(1, x, -0.1F) };
	attack.tracks[ForearmR] = { turn(0, x, -0.3F), turn(0.35F, x, -1.2F), turn(0.6F, x, -0.1F), turn(1, x, -0.3F) };
	attack.tracks[Chest] = { turn(0, y, 0), turn(0.35F, y, -0.45F), turn(0.6F, y, 0.4F), turn(1, y, 0) };
	attack.tracks[ThighL] = { turn(0, x, 0), turn(0.6F, x, -0.4F), turn(1, x, 0) };
	figure.clips[Figure::Attack] = std::move(attack);

	Clip hit = clip(1.0F, false);
	hit.tracks[Chest] = { turn(0, x, 0), turn(0.3F, x, -0.4F), turn(1, x, 0) };
	hit.tracks[Head] = { turn(0, x, 0), turn(0.3F, x, -0.3F), turn(1, x, 0) };
	hit.tracks[Hips] = { turn(0, x, 0), turn(0.3F, x, -0.1F, { 0, -0.04F, -0.08F }), turn(1, x, 0) };
	figure.clips[Figure::Hit] = std::move(hit);

	Clip death = clip(1.0F, false);
	death.tracks[Hips] = { turn(0, x, 0), turn(0.4F, x, -0.5F, { 0, -0.25F, -0.1F }), turn(1, x, -Pi / 2, { 0, -0.82F, -0.3F }) };
	death.tracks[ThighL] = { turn(0, x, 0), turn(0.4F, x, 0.7F), turn(1, x, 0.1F) };
	death.tracks[ThighR] = { turn(0, x, 0), turn(0.4F, x, 0.5F), turn(1, x, -0.1F) };
	death.tracks[ArmL] = { turn(0, x, 0), turn(1, x, -1.2F) };
	death.tracks[ArmR] = { turn(0, x, 0), turn(1, x, -0.9F) };
	figure.clips[Figure::Death] = std::move(death);
}

Mesh BuildCube()
{
	MeshBuilder builder;
	builder.Box(IdentityMat34, { 0.5F, 0.5F, 0.5F }, PackColor(1, 1, 1));
	return builder.Build();
}

} // namespace render
} // namespace vr
} // namespace devilution
