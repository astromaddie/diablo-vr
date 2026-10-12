/**
 * @file math.h
 *
 * Vector, quaternion and matrix helpers of the 3D renderer.
 *
 * World space: meters, Y up, right-handed. Matrices are row-major and multiply row
 * vectors (v * M), which is what the shaders declare with `row_major`.
 */
#pragma once

#include <algorithm>
#include <cmath>

namespace devilution {
namespace vr {
namespace render {

struct Vec3 {
	float x, y, z;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Normalize(Vec3 a)
{
	const float length = Length(a);
	return length > 1e-6F ? a * (1 / length) : Vec3 { 0, 1, 0 };
}
inline Vec3 Lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

/** x, y, z, w */
struct Quat {
	float x, y, z, w;
};

constexpr Quat IdentityQuat { 0, 0, 0, 1 };

inline Quat AxisAngle(Vec3 axis, float angle)
{
	const float s = std::sin(angle / 2);
	return { axis.x * s, axis.y * s, axis.z * s, std::cos(angle / 2) };
}

inline Quat operator*(Quat a, Quat b)
{
	return {
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	};
}

inline Vec3 Rotate(Quat q, Vec3 v)
{
	const Vec3 u { q.x, q.y, q.z };
	const Vec3 t = Cross(u, v) + v * q.w;
	return v + Cross(u, t) * 2;
}

/** Normalised linear interpolation along the shorter arc: enough between neighbouring keyframes. */
inline Quat Nlerp(Quat a, Quat b, float t)
{
	const float sign = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0 ? -1.F : 1.F;
	Quat q { a.x + (b.x * sign - a.x) * t, a.y + (b.y * sign - a.y) * t, a.z + (b.z * sign - a.z) * t, a.w + (b.w * sign - a.w) * t };
	const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	return { q.x / length, q.y / length, q.z / length, q.w / length };
}

/**
 * A rigid or affine transform as three rows of a column-vector matrix:
 * p' = (dot(row0, (p, 1)), dot(row1, (p, 1)), dot(row2, (p, 1))).
 */
struct Mat34 {
	float m[3][4];
};

constexpr Mat34 IdentityMat34 { { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } } };

/** Rotation, then per-axis scale applied before it, then translation. */
inline Mat34 Compose(Vec3 translation, Quat rotation, Vec3 scale = { 1, 1, 1 })
{
	const Vec3 x = Rotate(rotation, { scale.x, 0, 0 });
	const Vec3 y = Rotate(rotation, { 0, scale.y, 0 });
	const Vec3 z = Rotate(rotation, { 0, 0, scale.z });
	return { { { x.x, y.x, z.x, translation.x }, { x.y, y.y, z.y, translation.y }, { x.z, y.z, z.z, translation.z } } };
}

inline Vec3 Transform(const Mat34 &m, Vec3 p)
{
	return {
		m.m[0][0] * p.x + m.m[0][1] * p.y + m.m[0][2] * p.z + m.m[0][3],
		m.m[1][0] * p.x + m.m[1][1] * p.y + m.m[1][2] * p.z + m.m[1][3],
		m.m[2][0] * p.x + m.m[2][1] * p.y + m.m[2][2] * p.z + m.m[2][3],
	};
}

/** a after b: (a * b)(p) = a(b(p)). */
inline Mat34 operator*(const Mat34 &a, const Mat34 &b)
{
	Mat34 r;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 4; j++)
			r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + (j == 3 ? a.m[i][3] : 0);
	}
	return r;
}

/** Inverse of a rotation + translation (no scale). */
inline Mat34 InverseRigid(const Mat34 &m)
{
	Mat34 r;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++)
			r.m[i][j] = m.m[j][i];
		r.m[i][3] = -(m.m[0][i] * m.m[0][3] + m.m[1][i] * m.m[1][3] + m.m[2][i] * m.m[2][3]);
	}
	return r;
}

struct Mat4 {
	float m[4][4];
};

inline Mat4 operator*(const Mat4 &a, const Mat4 &b)
{
	Mat4 r;
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++)
			r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
	}
	return r;
}

/** World to view for a camera at `position` turned by `orientation` (looking down its -Z). */
inline Mat4 ViewMatrix(Vec3 position, Quat orientation)
{
	const Vec3 axes[3] = { Rotate(orientation, { 1, 0, 0 }), Rotate(orientation, { 0, 1, 0 }), Rotate(orientation, { 0, 0, 1 }) };
	Mat4 view {};
	for (int j = 0; j < 3; j++) {
		view.m[0][j] = axes[j].x;
		view.m[1][j] = axes[j].y;
		view.m[2][j] = axes[j].z;
		view.m[3][j] = -Dot(position, axes[j]);
	}
	view.m[3][3] = 1;
	return view;
}

/**
 * Off-centre perspective projection from the tangents of the four half-angles, with
 * reversed depth: the near plane maps to 1 and the far plane to 0, which keeps a float
 * depth buffer precise over the whole range.
 */
inline Mat4 ProjectionMatrix(float left, float right, float up, float down, float nearPlane, float farPlane)
{
	Mat4 p {};
	p.m[0][0] = 2 / (right - left);
	p.m[1][1] = 2 / (up - down);
	p.m[2][0] = (right + left) / (right - left);
	p.m[2][1] = (up + down) / (up - down);
	p.m[2][2] = nearPlane / (farPlane - nearPlane);
	p.m[2][3] = -1;
	p.m[3][2] = nearPlane * farPlane / (farPlane - nearPlane);
	return p;
}

/** Orthographic projection of the box [l, r] x [b, t] x [-far, -near] in view space, regular depth (near 0, far 1). */
inline Mat4 OrthoMatrix(float left, float right, float bottom, float top, float nearPlane, float farPlane)
{
	Mat4 p {};
	p.m[0][0] = 2 / (right - left);
	p.m[1][1] = 2 / (top - bottom);
	p.m[2][2] = 1 / (nearPlane - farPlane);
	p.m[3][0] = (left + right) / (left - right);
	p.m[3][1] = (top + bottom) / (bottom - top);
	p.m[3][2] = nearPlane / (nearPlane - farPlane);
	p.m[3][3] = 1;
	return p;
}

/** True when the box is entirely outside one of the clip planes of `viewProjection`. */
inline bool OutsideFrustum(const Mat4 &viewProjection, Vec3 boxMin, Vec3 boxMax)
{
	// Clip space: -w <= x <= w, -w <= y <= w, 0 <= z <= w. A box is out when all 8 corners fail the same test.
	int outside[5] = {};
	for (int corner = 0; corner < 8; corner++) {
		const Vec3 p { (corner & 1) != 0 ? boxMax.x : boxMin.x, (corner & 2) != 0 ? boxMax.y : boxMin.y, (corner & 4) != 0 ? boxMax.z : boxMin.z };
		const auto &m = viewProjection.m;
		const float x = p.x * m[0][0] + p.y * m[1][0] + p.z * m[2][0] + m[3][0];
		const float y = p.x * m[0][1] + p.y * m[1][1] + p.z * m[2][1] + m[3][1];
		const float z = p.x * m[0][2] + p.y * m[1][2] + p.z * m[2][2] + m[3][2];
		const float w = p.x * m[0][3] + p.y * m[1][3] + p.z * m[2][3] + m[3][3];
		outside[0] += x < -w ? 1 : 0;
		outside[1] += x > w ? 1 : 0;
		outside[2] += y < -w ? 1 : 0;
		outside[3] += y > w ? 1 : 0;
		outside[4] += z > w ? 1 : 0; // In front of the near plane (reversed depth).
	}
	return std::any_of(std::begin(outside), std::end(outside), [](int count) { return count == 8; });
}

} // namespace render
} // namespace vr
} // namespace devilution
