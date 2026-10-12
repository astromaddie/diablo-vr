/**
 * @file carved.cpp
 *
 * Models carved out of the game's own pictures (`vrassets/carve/<key>.crv`):
 * objects, the standing props of the dungeon tile sets, Tristram's houses, trees, rocks and fences,
 * and held items, each with a texture of original pixels baked onto it. What moves in an object's
 * animation (a flame, a liquid) is not carved: each frame carries that part as a picture, drawn as
 * a sprite over the model. Creatures are not carved; they stay the game's sprites.
 *
 * Model space is art pixels: x to the right of the art's first direction, y up, z towards the
 * art's camera, the middle of the tile (for a tile-set prop, of the megatile) at the origin.
 */
#include "platform/vr/render/bridge.h"

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

#include "utils/log.hpp"
#include "utils/png.h"

namespace devilution {
namespace vr {
namespace render {

namespace {

const bool Enabled = Knob("DEVILUTIONX_VR_CARVED", 1) != 0;
/** The file format still has a slot per original view for each vertex; nothing reads them now. */
constexpr int Views = 8;

/** A picture kept as index + coverage pairs, stored in the sprite atlas when first drawn. */
struct Picture {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> pairs;
	/** Its place in the sprite atlas, good while `epoch` is the atlas's. */
	uint32_t epoch = 0;
	SpriteSlot slot {};
};

struct Record {
	bool present = false;
	Mesh mesh;
	/** The baked texture: an index into the file's pictures (frames of one model share it). */
	int texture = -1;
	/** The part of the frame that moves, or -1; `effectLeft`, `effectTop`: where it sits in the frame. */
	int effect = -1;
	int effectLeft = 0;
	int effectTop = 0;
	float frameWidth = 0;
	float frameHeight = 0;
	Vec3 size {};
};

struct File {
	uint32_t kind = 0;
	std::vector<Record> records;
	std::vector<Picture> pictures;
};

#pragma pack(push, 1)
struct FileVertex {
	float position[3];
	float normal[3];
	float uv[2];
	uint8_t seen[Views];
};
struct RecordHeader {
	uint32_t id, vertexCount, indexCount, textureWidth, textureHeight;
	float frameWidth, frameHeight;
	int32_t offsets[Views][2];
};
struct EffectHeader {
	uint32_t width, height;
	int32_t left, top;
};
#pragma pack(pop)

std::unordered_map<std::string, std::unique_ptr<File>> files;
MeshBuilder builder;

/** The picture `pairs` as one of the file's, shared with an identical one already there. */
int AddPicture(File &file, const uint8_t *pairs, int width, int height)
{
	const size_t bytes = static_cast<size_t>(width) * height * 2;
	for (size_t i = 0; i < file.pictures.size(); i++) {
		const Picture &picture = file.pictures[i];
		if (picture.width == width && picture.height == height && std::memcmp(picture.pairs.data(), pairs, bytes) == 0)
			return static_cast<int>(i);
	}
	Picture picture;
	picture.width = width;
	picture.height = height;
	picture.pairs.assign(pairs, pairs + bytes);
	file.pictures.push_back(std::move(picture));
	return static_cast<int>(file.pictures.size() - 1);
}

/** Index + coverage pairs from a grey + alpha PNG (grey: palette index, alpha: coverage); empty when it does not decode. */
std::vector<uint8_t> DecodePairs(const uint8_t *bytes, size_t size, int width, int height)
{
	std::vector<uint8_t> pairs;
	SDL_RWops *source = SDL_RWFromConstMem(bytes, static_cast<int>(size));
	SDL_Surface *decoded = source != nullptr ? IMG_LoadPNG_RW(source) : nullptr;
	if (source != nullptr)
		SDL_RWclose(source);
	if (decoded == nullptr)
		return pairs;
	SDL_Surface *rgba = SDL_ConvertSurfaceFormat(decoded, SDL_PIXELFORMAT_RGBA32, 0);
	SDL_FreeSurface(decoded);
	if (rgba == nullptr)
		return pairs;
	if (rgba->w == width && rgba->h == height) {
		pairs.resize(static_cast<size_t>(width) * height * 2);
		for (int y = 0; y < height; y++) {
			const auto *row = static_cast<const uint8_t *>(rgba->pixels) + static_cast<size_t>(y) * rgba->pitch;
			for (int x = 0; x < width; x++) {
				pairs[(static_cast<size_t>(y) * width + x) * 2] = row[x * 4];
				pairs[(static_cast<size_t>(y) * width + x) * 2 + 1] = row[x * 4 + 3];
			}
		}
	}
	SDL_FreeSurface(rgba);
	return pairs;
}

std::unique_ptr<File> Load(const std::string &key)
{
	if (!Enabled || AssetsDirectory().empty())
		return nullptr;
	const std::string path = AssetsDirectory() + "/carve/" + key + ".crv";
	SDL_RWops *in = SDL_RWFromFile(path.c_str(), "rb");
	if (in == nullptr)
		return nullptr;
	std::vector<uint8_t> data(static_cast<size_t>(SDL_RWsize(in)));
	const size_t got = SDL_RWread(in, data.data(), 1, data.size());
	SDL_RWclose(in);
	if (got != data.size() || data.size() < 12)
		return nullptr;
	// CRV2 records end with the moving part of their frame; CRV1 records have none. CRV3 is CRV2
	// with each picture stored as a PNG (and a record may share the texture of the one before).
	const bool png = std::memcmp(data.data(), "CRV3", 4) == 0;
	const bool effects = png || std::memcmp(data.data(), "CRV2", 4) == 0;
	if (!effects && std::memcmp(data.data(), "CRV1", 4) != 0)
		return nullptr;
	int lastTexture = -1;
	/** A picture of the file: PNG bytes (CRV3) or raw pairs. -1 when it is missing or broken. */
	const auto readPicture = [&](File &into, size_t &cursor, int width, int height) -> int {
		if (!png) {
			const size_t bytes = static_cast<size_t>(width) * height * 2;
			if (cursor + bytes > data.size())
				return -2;
			const int picture = bytes != 0 ? AddPicture(into, &data[cursor], width, height) : -1;
			cursor += bytes;
			return picture;
		}
		uint32_t bytes;
		if (cursor + 4 > data.size())
			return -2;
		std::memcpy(&bytes, &data[cursor], 4);
		cursor += 4;
		if (cursor + bytes > data.size())
			return -2;
		if (bytes == 0)
			return -1;
		const std::vector<uint8_t> pairs = DecodePairs(&data[cursor], bytes, width, height);
		cursor += bytes;
		return pairs.empty() ? -2 : AddPicture(into, pairs.data(), width, height);
	};
	auto file = std::make_unique<File>();
	uint32_t count;
	std::memcpy(&file->kind, &data[4], 4);
	std::memcpy(&count, &data[8], 4);
	size_t at = 12;
	uint32_t triangles = 0;
	for (uint32_t i = 0; i < count; i++) {
		RecordHeader header;
		if (at + sizeof(header) > data.size())
			return nullptr;
		std::memcpy(&header, &data[at], sizeof(header));
		at += sizeof(header);
		const size_t vertexBytes = static_cast<size_t>(header.vertexCount) * sizeof(FileVertex);
		const size_t indexBytes = static_cast<size_t>(header.indexCount) * 4;
		if (at + vertexBytes + indexBytes > data.size() || header.id > 4096)
			return nullptr;
		if (file->records.size() <= header.id)
			file->records.resize(header.id + 1);
		Record &record = file->records[header.id];
		builder.Clear();
		builder.vertices.resize(header.vertexCount);
		Vec3 boundsMin { 1e9F, 1e9F, 1e9F };
		Vec3 boundsMax { -1e9F, -1e9F, -1e9F };
		for (uint32_t v = 0; v < header.vertexCount; v++) {
			FileVertex source;
			std::memcpy(&source, &data[at + static_cast<size_t>(v) * sizeof(FileVertex)], sizeof(source));
			Vertex &vertex = builder.vertices[v];
			vertex = {};
			std::memcpy(vertex.position, source.position, sizeof(vertex.position));
			std::memcpy(vertex.normal, source.normal, sizeof(vertex.normal));
			std::memcpy(vertex.uv, source.uv, sizeof(vertex.uv));
			boundsMin = { std::min(boundsMin.x, source.position[0]), std::min(boundsMin.y, source.position[1]), std::min(boundsMin.z, source.position[2]) };
			boundsMax = { std::max(boundsMax.x, source.position[0]), std::max(boundsMax.y, source.position[1]), std::max(boundsMax.z, source.position[2]) };
		}
		at += vertexBytes;
		builder.indices.resize(header.indexCount);
		std::memcpy(builder.indices.data(), &data[at], indexBytes);
		at += indexBytes;
		record.texture = readPicture(*file, at, static_cast<int>(header.textureWidth), static_cast<int>(header.textureHeight));
		if (record.texture == -2)
			return nullptr;
		// CRV3: no texture of its own means the record before's (the frames of one model share it).
		if (record.texture == -1 && png)
			record.texture = lastTexture;
		lastTexture = record.texture;
		if (effects) {
			EffectHeader effect;
			if (at + sizeof(effect) > data.size())
				return nullptr;
			std::memcpy(&effect, &data[at], sizeof(effect));
			at += sizeof(effect);
			record.effect = readPicture(*file, at, static_cast<int>(effect.width), static_cast<int>(effect.height));
			if (record.effect == -2)
				return nullptr;
			record.effectLeft = effect.left;
			record.effectTop = effect.top;
		}
		record.mesh = builder.Build();
		record.present = header.indexCount != 0 && record.texture >= 0;
		record.frameWidth = header.frameWidth;
		record.frameHeight = header.frameHeight;
		record.size = header.vertexCount != 0 ? boundsMax - boundsMin : Vec3 {};
		triangles = std::max(triangles, header.indexCount / 3);
	}
	LogVerbose("VR: carved {}: {} records, {} pictures, up to {} triangles", key, count, file->pictures.size(), triangles);
	return file;
}

File *Find(const std::string &key)
{
	auto found = files.find(key);
	if (found == files.end())
		found = files.emplace(key, Load(key)).first;
	return found->second.get();
}

Record *RecordOf(File *file, int id)
{
	if (file == nullptr || id < 0 || static_cast<size_t>(id) >= file->records.size())
		return nullptr;
	Record &record = file->records[id];
	return record.present ? &record : nullptr;
}

/** A picture's place in the atlas, storing it there when the atlas has been emptied since. */
const SpriteSlot *Store(Picture &picture)
{
	if (picture.epoch == SpritesEpoch())
		return &picture.slot;
	const SpriteSlot *slot = CacheImage(picture.pairs.data(), picture.width, picture.height);
	if (slot == nullptr)
		return nullptr;
	picture.slot = *slot;
	picture.epoch = SpritesEpoch();
	return &picture.slot;
}

/** texel = base + pixel * scale, for a picture whose painted part is what the atlas holds. */
void MapSlot(CarvedInstance &instance, const SpriteSlot &slot)
{
	instance.rect[0] = slot.x;
	instance.rect[1] = slot.y;
	instance.rect[2] = slot.width;
	instance.rect[3] = slot.height;
	instance.map[0] = static_cast<float>(slot.x) - slot.left * slot.scale;
	instance.map[1] = static_cast<float>(slot.y) - slot.top * slot.scale;
	instance.map[2] = slot.scale;
	instance.map[3] = 1;
}

bool AddBaked(Scene &scene, File &file, Record &record, const Mat34 &world, float emissive)
{
	const SpriteSlot *slot = Store(file.pictures[record.texture]);
	if (slot == nullptr)
		return false;
	CarvedInstance instance {};
	instance.world = world;
	instance.params[0] = 0;
	instance.params[1] = emissive;
	MapSlot(instance, *slot);
	scene.carved.push_back({ &record.mesh, instance });
	return true;
}

void SetVec(float target[3], Vec3 v)
{
	target[0] = v.x;
	target[1] = v.y;
	target[2] = v.z;
}

/**
 * The moving part of a frame, upright over the model and turned to the viewer as the game's sprites
 * are, standing where it stands in the frame: the frame's bottom centre is SpriteFootPixels below `foot`.
 */
void AddEffect(Scene &scene, File &file, const Record &record, Vec3 foot, float emissive)
{
	if (record.effect < 0)
		return;
	const SpriteSlot *slot = Store(file.pictures[record.effect]);
	if (slot == nullptr)
		return;
	const float metersH = TileSize / PixelsPerTileH;
	const float metersV = TileSize / PixelsPerTileV;
	const Vec3 toViewer { scene.headPosition.x - foot.x, 0, scene.headPosition.z - foot.z };
	const Vec3 normal = Length(toViewer) < 0.05F ? Vec3 { 0, 0, 1 } : Normalize(toViewer);
	const Vec3 right { normal.z, 0, -normal.x };
	const float left = static_cast<float>(record.effectLeft) + slot->left;
	const float top = static_cast<float>(record.effectTop) + slot->top;
	const float shift = (left + slot->artWidth / 2 - record.frameWidth / 2) * metersH;
	const float bottom = (record.frameHeight - top - slot->artHeight - SpriteFootPixels) * metersV;
	// A hair towards the viewer, so the picture is not cut by the model it was painted over.
	const Vec3 base = foot + right * shift + Vec3 { 0, bottom, 0 } + normal * 0.03F;
	SpriteInstance instance {};
	SetVec(instance.position, base);
	SetVec(instance.axisU, right * (slot->artWidth * metersH / 2));
	SetVec(instance.axisV, { 0, slot->artHeight * metersV, 0 });
	instance.emissive = emissive;
	instance.uv[0] = slot->x;
	instance.uv[1] = slot->y;
	instance.uv[2] = static_cast<float>(slot->x + slot->width);
	instance.uv[3] = static_cast<float>(slot->y + slot->height);
	instance.tint[0] = instance.tint[1] = instance.tint[2] = instance.tint[3] = 1;
	scene.sprites.push_back(instance);
}

/** The world placement of art-pixel model space with its origin at `foot`. */
Mat34 Standing(Vec3 foot)
{
	const float scale = CarvedMetersPerPixel();
	// The art's camera stands to the south: its right is +x -z in the world, towards it is +x +z.
	return Compose(foot, AxisAngle({ 0, 1, 0 }, 0.7853982F), { scale, scale, scale });
}

} // namespace

void CarvedRelease()
{
	files.clear();
}

float CarvedMetersPerPixel()
{
	return TileSize / PixelsPerTileH;
}

bool CarvedAddObject(Scene &scene, ClxSprite sprite, Vec3 foot, float emissive, Vec3 *size, float effectEmissive)
{
	std::string path;
	int frame;
	if (!Enabled || !SpriteOrigin(sprite, path, frame))
		return false;
	File *file = Find(path);
	Record *record = RecordOf(file, frame);
	if (record == nullptr)
		return false;
	if (!AddBaked(scene, *file, *record, Standing(foot), emissive))
		return false;
	AddEffect(scene, *file, *record, foot, effectEmissive);
	if (size != nullptr)
		*size = record->size * CarvedMetersPerPixel();
	return true;
}

bool CarvedExists(const std::string &key)
{
	return Enabled && RecordOf(Find(key), 0) != nullptr;
}

bool CarvedAddModel(Scene &scene, const std::string &key, Vec3 foot, float emissive, Vec3 *size)
{
	File *file = Enabled ? Find(key) : nullptr;
	Record *record = RecordOf(file, 0);
	if (record == nullptr || !AddBaked(scene, *file, *record, Standing(foot), emissive))
		return false;
	if (size != nullptr)
		*size = record->size * CarvedMetersPerPixel();
	return true;
}

bool CarvedAddRecord(Scene &scene, const std::string &key, int id, const Mat34 &world, float emissive)
{
	File *file = Enabled ? Find(key) : nullptr;
	Record *record = RecordOf(file, id);
	return record != nullptr && AddBaked(scene, *file, *record, world, emissive);
}

bool CarvedAddItem(Scene &scene, int icon, const Mat34 &grip, float metersPerPixel, float emissive)
{
	char key[32];
	SDL_snprintf(key, sizeof(key), "icons/objcurs_%03d", icon);
	File *file = Enabled ? Find(key) : nullptr;
	Record *record = RecordOf(file, 0);
	if (record == nullptr)
		return false;
	// The model's origin is where the hand holds it (a shield: the middle of its back).
	return AddBaked(scene, *file, *record, grip * Compose({ 0, 0, 0 }, IdentityQuat, { metersPerPixel, metersPerPixel, metersPerPixel }), emissive);
}

} // namespace render
} // namespace vr
} // namespace devilution
