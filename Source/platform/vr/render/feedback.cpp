/**
 * @file feedback.cpp
 *
 * What first person needs to be told and the isometric view showed by itself: a crosshair where
 * the hand aims that turns red when an attack would reach a monster, an arc when the character
 * swings, a mark on a monster the moment it is hurt, and the names of what lies on the floor.
 * None of it changes what the game does: it only reads the game's state.
 */
#include "platform/vr/render/bridge.h"

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "cursor.h"
#include "engine/render/text_render.hpp"
#include "engine/surface.hpp"
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "monster.h"
#include "player.h"

namespace devilution {
namespace vr {
namespace render {

namespace {

const bool Enabled = Knob("DEVILUTIONX_VR_FEEDBACK", 1) != 0;
/** Tiles within which loot is named. */
const float LabelTiles = Knob("DEVILUTIONX_VR_LABEL_TILES", 10.0F);
/** Seconds the swing's arc and a hit's mark stay. */
constexpr float SlashTime = 0.22F;
constexpr float HitTime = 0.25F;

Mesh bar;
Mesh crescent;

struct Label {
	Texture texture;
	int width = 0;
	int height = 0;
};
std::unordered_map<std::string, Label> labels;
int labelsLevel = -1;
int labelsType = -1;

decltype(Monster::hitPoints) lastHitPoints[MaxMonsters];
bool hitPointsKnown[MaxMonsters];
struct HitMark {
	Vec3 at;
	float since;
};
std::vector<HitMark> hitMarks;

int lastMode = -1;
int lastAttackFrame;
float slashSince = -1;
float slashSide = 1;

/** The shortest turn that takes one direction to another. */
Quat Turn(Vec3 from, Vec3 to)
{
	const Vec3 axis = Cross(from, to);
	const float sine = Length(axis);
	const float cosine = from.x * to.x + from.y * to.y + from.z * to.z;
	if (sine < 1e-5F)
		return cosine > 0 ? IdentityQuat : AxisAngle({ 0, 1, 0 }, 3.14159265F);
	return AxisAngle(axis * (1 / sine), std::atan2(sine, cosine));
}

void Build()
{
	MeshBuilder builder;
	builder.Box(IdentityMat34, { 0.5F, 0.5F, 0.5F }, PackColor(1, 1, 1));
	bar = builder.Build();

	// A crescent in the x, y plane, a quarter and a bit of a circle of radius 1, thickest in its middle.
	builder.Clear();
	constexpr int Steps = 16;
	constexpr float From = 0.35F;
	constexpr float Sweep = 2.0F;
	Vec3 outer[Steps + 1];
	Vec3 inner[Steps + 1];
	for (int i = 0; i <= Steps; i++) {
		const float u = static_cast<float>(i) / Steps;
		const float angle = From + Sweep * u;
		const float thickness = 0.16F * std::sin(3.14159265F * u);
		outer[i] = { std::cos(angle), std::sin(angle), 0 };
		inner[i] = outer[i] * (1 - thickness);
	}
	for (int i = 0; i < Steps; i++) {
		const Vec3 front[4] = { inner[i], outer[i], outer[i + 1], inner[i + 1] };
		const Vec3 back[4] = { inner[i + 1], outer[i + 1], outer[i], inner[i] };
		builder.Quad(front, nullptr, -1, PackColor(1, 1, 1));
		builder.Quad(back, nullptr, -1, PackColor(1, 1, 1));
	}
	crescent = builder.Build();
}

/** A thin unlit bar from `from` to `to`. */
void AddBar(Scene &scene, Vec3 from, Vec3 to, float thickness, const float color[3])
{
	const float length = Length(to - from);
	if (length < 1e-4F)
		return;
	MeshInstance instance {};
	instance.world = Compose((from + to) * 0.5F, Turn({ 1, 0, 0 }, (to - from) * (1 / length)), { length, thickness, thickness });
	instance.color[0] = color[0];
	instance.color[1] = color[1];
	instance.color[2] = color[2];
	instance.color[3] = 0.8F;
	instance.boneBase = -1;
	scene.meshes.push_back({ &bar, instance, false });
}

/** Whether an attack made now would go for the monster under the aim: next to the character, or anywhere in sight with a bow. */
bool AttackViable()
{
	if (leveltype == DTYPE_TOWN || pcursmonst < 0 || pcursmonst >= static_cast<int>(MaxMonsters) || MyPlayer == nullptr)
		return false;
	const Monster &monster = Monsters[pcursmonst];
	if (monster.hitPoints <= 0 || monster.isPlayerMinion())
		return false;
	if (MyPlayer->UsesRangedWeapon())
		return true;
	const Point self = MyPlayer->position.future;
	const Point target = monster.position.future;
	return std::abs(target.x - self.x) <= 1 && std::abs(target.y - self.y) <= 1;
}

/** A plus where the hand aims, the same size in the eye at any distance; red when an attack would reach. */
void AddCrosshair(Scene &scene, const float hit[3])
{
	Vec3 at { hit[0], hit[1], hit[2] };
	const Vec3 toHead = scene.headPosition - at;
	const float distance = Length(toHead);
	if (distance < 0.2F)
		return;
	// A little towards the eye, so what it lies on does not cut through it.
	at = at + toHead * (std::min(0.08F, distance * 0.1F) / distance);
	const Vec3 forward = toHead * (-1 / distance);
	const Vec3 right = Normalize(Cross(forward, { 0, 1, 0 }));
	const Vec3 up = Cross(right, forward);
	const float size = 0.012F * distance;
	const float white[3] = { 0.9F, 0.9F, 0.85F };
	const float red[3] = { 1.0F, 0.08F, 0.05F };
	const float *color = AttackViable() ? red : white;
	// Four ticks round an open middle.
	for (const Vec3 direction : { right, right * -1, up, up * -1 })
		AddBar(scene, at + direction * (size * 0.35F), at + direction * size, size * 0.14F, color);
}

/** A white arc sweeping across the view when the character swings. */
void AddSlash(Scene &scene)
{
	const Player &player = *MyPlayer;
	const int mode = static_cast<int>(player._pmode);
	const int frame = player.AnimInfo.currentFrame;
	// A new swing: the attack begins, or begins again while the last one is still held.
	if (player._pmode == PM_ATTACK && (lastMode != mode || frame < lastAttackFrame)) {
		slashSince = scene.time;
		slashSide = -slashSide;
	}
	lastMode = mode;
	lastAttackFrame = player._pmode == PM_ATTACK ? frame : 0;
	const float age = scene.time - slashSince;
	if (slashSince < 0 || age < 0 || age > SlashTime)
		return;
	const float progress = age / SlashTime;
	const Vec3 forward = Cross(scene.headUp, scene.headRight);
	// The arc turns through the view as the blade would, growing a little and thinning out as it goes.
	const float roll = (0.9F - 1.8F * progress) * slashSide + (slashSide < 0 ? 3.14159265F : 0);
	const Vec3 right = scene.headRight * std::cos(roll) + scene.headUp * std::sin(roll);
	const Vec3 up = scene.headUp * std::cos(roll) - scene.headRight * std::sin(roll);
	const float radius = 0.34F + 0.1F * progress;
	const float thin = 1 - progress * progress;
	const Vec3 centre = scene.headPosition + forward * 0.85F - scene.headUp * 0.12F;
	MeshInstance instance {};
	const Vec3 x = right * radius;
	const Vec3 y = up * (radius * (0.75F + 0.25F * thin));
	const Vec3 z = forward * radius;
	instance.world = { { { x.x, y.x, z.x, centre.x }, { x.y, y.y, z.y, centre.y }, { x.z, y.z, z.z, centre.z } } };
	instance.color[0] = instance.color[1] = instance.color[2] = 0.95F * thin + 0.05F;
	instance.color[3] = 1.2F * thin;
	instance.boneBase = -1;
	scene.meshes.push_back({ &crescent, instance, false });
}

/** A red cross on a monster for a moment after its life dropped: the blow landed. */
void AddHitMarks(Scene &scene)
{
	bool active[MaxMonsters] = {};
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const unsigned id = ActiveMonsters[i];
		const Monster &monster = Monsters[id];
		active[id] = true;
		if (hitPointsKnown[id] && monster.hitPoints < lastHitPoints[id] && !monster.isPlayerMinion()) {
			const Vec3 foot = TileToWorld(monster.position.tile, 0, 0);
			hitMarks.push_back({ { foot.x, 1.1F, foot.z }, scene.time });
		}
		lastHitPoints[id] = monster.hitPoints;
		hitPointsKnown[id] = true;
	}
	for (size_t id = 0; id < MaxMonsters; id++) {
		if (!active[id])
			hitPointsKnown[id] = false;
	}
	const float red[3] = { 1.0F, 0.1F, 0.05F };
	for (size_t i = 0; i < hitMarks.size();) {
		const HitMark &mark = hitMarks[i];
		const float age = scene.time - mark.since;
		if (age < 0 || age > HitTime) {
			hitMarks[i] = hitMarks.back();
			hitMarks.pop_back();
			continue;
		}
		i++;
		const Vec3 toHead = scene.headPosition - mark.at;
		const float distance = Length(toHead);
		if (distance < 0.2F)
			continue;
		const Vec3 at = mark.at + toHead * (std::min(0.5F, distance * 0.3F) / distance);
		const Vec3 forward = toHead * (-1 / distance);
		const Vec3 right = Normalize(Cross(forward, { 0, 1, 0 }));
		const Vec3 up = Cross(right, forward);
		const float size = 0.03F * Length(scene.headPosition - at) * (1 + age / HitTime * 0.5F);
		for (const Vec3 direction : { right + up, right - up, up - right, (right + up) * -1 })
			AddBar(scene, at + direction * (size * 0.35F), at + direction * size, size * 0.16F, red);
	}
}

/** A line of the game's own lettering as a texture, made once per text and colour. */
const Label *LabelFor(const std::string &text, UiFlags color)
{
	const std::string key = text + '\n' + std::to_string(static_cast<int>(color));
	if (const auto found = labels.find(key); found != labels.end())
		return found->second.texture ? &found->second : nullptr;
	if (labels.size() > 96)
		labels.clear();
	Label &label = labels[key];
	const int width = std::min(GetLineWidth(text) + 10, 400);
	constexpr int Height = 18;
	OwnedSurface surface { width, Height };
	for (int y = 0; y < Height; y++)
		std::memset(surface.at(0, y), 0, width);
	DrawString(surface, text, Rectangle { { 5, 3 }, { width - 10, 0 } }, { .flags = color });
	std::vector<uint32_t> pixels(static_cast<size_t>(width) * Height);
	for (int y = 0; y < Height; y++) {
		const uint8_t *source = surface.at(0, y);
		for (int x = 0; x < width; x++)
			pixels[static_cast<size_t>(y) * width + x] = source[x] == 0 ? 0x90000000 : PaletteRgba(source[x], 255);
	}
	label.texture = CreateTexture(width, Height, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	if (!label.texture)
		return nullptr;
	UploadTexture(label.texture, 0, 0, 0, width, Height, pixels.data(), 4);
	label.width = width;
	label.height = Height;
	return &label;
}

/** A label standing over a point, turned to the viewer about the vertical, a steady size in the eye. */
void AddLabel(Scene &scene, const std::string &text, UiFlags color, Vec3 at)
{
	const Label *label = LabelFor(text, color);
	if (label == nullptr)
		return;
	const Vec3 toHead { scene.headPosition.x - at.x, 0, scene.headPosition.z - at.z };
	const float distance = Length(toHead);
	if (distance < 0.05F)
		return;
	const Vec3 normal = toHead * (1 / distance);
	const float meters = 0.0028F * std::clamp(distance, 1.5F, 9.F);
	Panel panel {};
	panel.texture = label->texture.srv.Get();
	panel.center = at;
	panel.right = Vec3 { normal.z, 0, -normal.x } * (meters * static_cast<float>(label->width) / 2);
	panel.up = Vec3 { 0, 1, 0 } * (meters * static_cast<float>(label->height) / 2);
	panel.opacity = 0.95F;
	scene.panels.push_back(panel);
}

/** The name of everything lying on the floor in sight, and of the monster under the aim. */
void AddLabels(Scene &scene, bool pointerValid)
{
	if (labelsLevel != currlevel || labelsType != static_cast<int>(leveltype)) {
		labels.clear();
		hitMarks.clear();
		std::memset(hitPointsKnown, 0, sizeof(hitPointsKnown));
		labelsLevel = currlevel;
		labelsType = static_cast<int>(leveltype);
	}
	const float headX = scene.headPosition.x / TileSize;
	const float headY = scene.headPosition.z / TileSize;
	for (int i = 0; i < ActiveItemCount; i++) {
		const Item &item = Items[ActiveItems[i]];
		if (item.isEmpty() || !IsTileLit(item.position))
			continue;
		const float x = static_cast<float>(item.position.x) + 0.5F;
		const float y = static_cast<float>(item.position.y) + 0.5F;
		const float away = std::hypot(x - headX, y - headY);
		// Panels are drawn over everything, so a wall between must be found here.
		if (away > LabelTiles || (away > 1 && !LevelLineOfSight(headX, headY, x, y)))
			continue;
		const std::string text = item._itype == ItemType::Gold
		    ? std::to_string(item._ivalue) + " gold"
		    : std::string(item.getName().str());
		AddLabel(scene, text, item.getTextColor(), { x * TileSize, 0.62F, y * TileSize });
	}
	if (pointerValid && leveltype != DTYPE_TOWN && pcursmonst >= 0 && pcursmonst < static_cast<int>(MaxMonsters)) {
		const Monster &monster = Monsters[pcursmonst];
		const Vec3 foot = TileToWorld(monster.position.tile, 0, 0);
		AddLabel(scene, std::string(monster.name()), monster.isUnique() ? UiFlags::ColorWhitegold : UiFlags::ColorWhite, { foot.x, 2.25F, foot.z });
	}
}

} // namespace

void FeedbackAddToScene(Scene &scene, bool pointerValid, const float pointerHit[3])
{
	if (!Enabled || MyPlayer == nullptr)
		return;
	if (bar.indexCount == 0)
		Build();
	if (pointerValid)
		AddCrosshair(scene, pointerHit);
	AddSlash(scene);
	AddLabels(scene, pointerValid);
	AddHitMarks(scene);
}

void FeedbackRelease()
{
	bar = Mesh {};
	crescent = Mesh {};
	labels.clear();
	hitMarks.clear();
}

} // namespace render
} // namespace vr
} // namespace devilution
