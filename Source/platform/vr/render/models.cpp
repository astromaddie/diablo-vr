/**
 * @file models.cpp
 *
 * An optional model pack (`3dobjects-vr/*.glb`) in the game: loads the scenes, cuts them into the
 * things they contain, and stands those where the game has the same thing.
 *
 * The pack's files are scenes, not single objects: `Tristram_plaza` is eight houses and a
 * well laid out as a village of its own, `Adria_shack_hinges` a hut among spare doors. A
 * scene is therefore split into its free-standing pieces, and a piece is fitted onto a
 * footprint the game knows (a town building's solid tiles, a stair tile).
 *
 * The folder is looked for next to the executable and up to four directories above it;
 * DEVILUTIONX_VR_MODELS=<dir> names it outright.
 */
#include "platform/vr/render/bridge.h"

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

#include "levels/gendung.h"
#include "levels/trigs.h"
#include "platform/vr/render/gltf.h"
#include "towners.h"
#include "utils/log.hpp"

namespace devilution {
namespace vr {
namespace render {

namespace {

constexpr int TextureSize = 512;
constexpr int MaxTextures = 192;
/** Generated models are dropped on a level change once their textures have filled this many layers. */
constexpr int CrowdedTextures = 150;
/** Pieces of a scene closer together than this are one thing. */
constexpr float SplitCell = 0.5F;
/** Mean value (0..255) a model texture is lifted to when it is darker. */
const float MinTextureBrightness = Knob("DEVILUTIONX_VR_MODEL_MIN_BRIGHTNESS", 45.0F);

/** Triangles of a scene that belong together. After Finish() its ground is y = 0 and it is centred on its footprint. */
struct Part {
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	Vec3 boundsMin { 1e9F, 1e9F, 1e9F };
	Vec3 boundsMax { -1e9F, -1e9F, -1e9F };

	Vec3 Size() const { return boundsMax - boundsMin; }
	float Volume() const { return indices.empty() ? 0 : Size().x * Size().y * Size().z; }

	void Add(const Vertex &vertex)
	{
		vertices.push_back(vertex);
		const float *p = vertex.position;
		boundsMin = { std::min(boundsMin.x, p[0]), std::min(boundsMin.y, p[1]), std::min(boundsMin.z, p[2]) };
		boundsMax = { std::max(boundsMax.x, p[0]), std::max(boundsMax.y, p[1]), std::max(boundsMax.z, p[2]) };
	}

	void Append(const Part &other)
	{
		const auto base = static_cast<uint32_t>(vertices.size());
		for (const Vertex &vertex : other.vertices)
			Add(vertex);
		for (const uint32_t index : other.indices)
			indices.push_back(base + index);
	}

	/** `sunken`: the scene's own y = 0 is the ground and the part reaches below it (stairs going down). */
	void Finish(bool sunken = false)
	{
		const Vec3 origin { (boundsMin.x + boundsMax.x) / 2, sunken ? 0 : boundsMin.y, (boundsMin.z + boundsMax.z) / 2 };
		for (Vertex &vertex : vertices) {
			vertex.position[0] -= origin.x;
			vertex.position[1] -= origin.y;
			vertex.position[2] -= origin.z;
		}
		boundsMin = boundsMin - origin;
		boundsMax = boundsMax - origin;
	}
};

Texture textures;
std::vector<std::string> textureNames;
std::vector<uint32_t> packedPixels;
bool texturesDirty;
bool loaded;
/** Off while a generated model loads: its texture is the game's own art and stays as dark as that is. */
bool liftDarkTextures = true;
size_t packTextureCount;
std::unordered_map<std::string, std::unique_ptr<GeneratedModel>> generated;
std::unordered_map<const void *, const GeneratedModel *> generatedBySprite;
const bool GeneratedEnabled = Knob("DEVILUTIONX_VR_GENERATED_MODELS", 1) != 0;
std::vector<Part> houses;
Part church;
Part shack;
Part boneStairs;

std::vector<Mesh> placed;
bool propsPending;
MeshBuilder builder;

std::string ModelDirectory()
{
	if (const char *directory = SDL_getenv("DEVILUTIONX_VR_MODELS"); directory != nullptr)
		return directory;
	char *base = SDL_GetBasePath();
	std::string path = base != nullptr ? base : "";
	SDL_free(base);
	for (int up = 0; up < 5; up++) {
		const std::string candidate = path + "3dobjects-vr";
		if (SDL_RWops *probe = SDL_RWFromFile((candidate + "/Tristram_plaza.glb").c_str(), "rb"); probe != nullptr) {
			SDL_RWclose(probe);
			return candidate;
		}
		path += "../";
	}
	return "3dobjects-vr";
}

/** The layer value for vertices that wear this image, uploading it on first use; -1 when it cannot be had. */
float TextureLayer(const GltfImage &image)
{
	for (size_t i = 0; i < textureNames.size(); i++) {
		if (textureNames[i] == image.name)
			return ModelLayerBase + static_cast<float>(i);
	}
	if (image.pixels == nullptr || textureNames.size() >= MaxTextures)
		return -1;
	SDL_Surface *scaled = SDL_CreateRGBSurfaceWithFormat(0, TextureSize, TextureSize, 32, SDL_PIXELFORMAT_RGBA32);
	if (scaled == nullptr || SDL_SoftStretchLinear(image.pixels, nullptr, scaled, nullptr) != 0) {
		SDL_FreeSurface(scaled);
		return -1;
	}
	packedPixels.resize(static_cast<size_t>(TextureSize) * TextureSize);
	for (int y = 0; y < TextureSize; y++)
		std::memcpy(&packedPixels[static_cast<size_t>(y) * TextureSize], static_cast<const uint8_t *>(scaled->pixels) + static_cast<size_t>(y) * scaled->pitch, TextureSize * 4);
	SDL_FreeSurface(scaled);
	// Some of the pack's textures (thatch, town stone, wood) were made near-black and show as holes
	// in the picture: those are lifted to the brightness of the darkest ones that read well.
	uint64_t sum = 0;
	for (const uint32_t pixel : packedPixels)
		sum += (pixel & 0xFF) + (pixel >> 8 & 0xFF) + (pixel >> 16 & 0xFF);
	const float mean = static_cast<float>(sum) / static_cast<float>(packedPixels.size() * 3);
	const float gain = liftDarkTextures ? std::clamp(MinTextureBrightness / std::max(mean, 1.F), 1.F, 3.F) : 1.F;
	for (uint32_t &pixel : packedPixels) {
		const auto lift = [&](uint32_t channel) { return std::min(static_cast<uint32_t>(static_cast<float>(channel) * gain), 255U); };
		pixel = lift(pixel & 0xFF) | lift(pixel >> 8 & 0xFF) << 8 | lift(pixel >> 16 & 0xFF) << 16 | (pixel & 0xFF000000);
	}
	UploadTexture(textures, static_cast<int>(textureNames.size()), 0, 0, TextureSize, TextureSize, packedPixels.data(), 4);
	texturesDirty = true;
	textureNames.push_back(image.name);
	return ModelLayerBase + static_cast<float>(textureNames.size() - 1);
}

/**
 * Cuts a scene into its free-standing pieces: triangles whose footprints touch (on a grid of
 * SplitCell) are one piece. `keep` drops materials; `split` false keeps everything as one piece.
 */
template <typename Keep>
std::vector<Part> Split(const GltfScene &scene, Keep keep, bool split = true)
{
	// Union-find over the grid cells any triangle covers.
	std::unordered_map<uint64_t, uint32_t> cellIds;
	std::vector<uint32_t> parent;
	const auto find = [&](uint32_t node) {
		while (parent[node] != node)
			node = parent[node] = parent[parent[node]];
		return node;
	};
	const auto key = [](int x, int z) { return static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32 | static_cast<uint32_t>(z); };
	const auto cell = [&](int x, int z) {
		const auto [entry, added] = cellIds.try_emplace(key(x, z), static_cast<uint32_t>(parent.size()));
		if (added)
			parent.push_back(entry->second);
		return entry->second;
	};
	const auto cellOf = [](float value) { return static_cast<int>(std::floor(value / SplitCell)); };

	struct Triangle {
		const GltfPrimitive *primitive;
		uint32_t first;
		uint32_t group;
	};
	std::vector<Triangle> triangles;
	for (const GltfPrimitive &primitive : scene.primitives) {
		if (!keep(primitive))
			continue;
		for (uint32_t i = 0; i + 2 < primitive.indices.size(); i += 3) {
			float lo[2] = { 1e9F, 1e9F };
			float hi[2] = { -1e9F, -1e9F };
			for (int corner = 0; corner < 3; corner++) {
				const Vec3 p = primitive.positions[primitive.indices[i + corner]];
				lo[0] = std::min(lo[0], p.x);
				lo[1] = std::min(lo[1], p.z);
				hi[0] = std::max(hi[0], p.x);
				hi[1] = std::max(hi[1], p.z);
			}
			const uint32_t root = cell(split ? cellOf(lo[0]) : 0, split ? cellOf(lo[1]) : 0);
			// A triangle's bounding box stands in for the triangle; 256 cells a side is beyond any piece of the pack.
			for (int x = cellOf(lo[0]); split && x <= std::min(cellOf(hi[0]), cellOf(lo[0]) + 256); x++) {
				for (int z = cellOf(lo[1]); z <= std::min(cellOf(hi[1]), cellOf(lo[1]) + 256); z++)
					parent[find(cell(x, z))] = find(root);
			}
			triangles.push_back({ &primitive, i, root });
		}
	}
	// Cells next to each other belong together too. Copied: joining adds no cells, but must not walk a map being read.
	const std::vector<std::pair<uint64_t, uint32_t>> cells(cellIds.begin(), cellIds.end());
	for (const auto &[cellKey, id] : cells) {
		const int x = static_cast<int>(cellKey >> 32);
		const int z = static_cast<int>(cellKey & 0xFFFFFFFF);
		for (const auto &[dx, dz] : { std::pair { 1, -1 }, std::pair { 1, 0 }, std::pair { 1, 1 }, std::pair { 0, 1 } }) {
			if (const auto neighbour = cellIds.find(key(x + dx, z + dz)); neighbour != cellIds.end())
				parent[find(neighbour->second)] = find(id);
		}
	}

	std::vector<Part> parts;
	std::unordered_map<uint32_t, size_t> partOfGroup;
	for (const Triangle &triangle : triangles) {
		const auto [entry, added] = partOfGroup.try_emplace(find(triangle.group), parts.size());
		if (added)
			parts.emplace_back();
		Part &part = parts[entry->second];
		const GltfPrimitive &primitive = *triangle.primitive;
		const float layer = primitive.image >= 0 ? TextureLayer(scene.images[primitive.image]) : -1;
		// Vertices are not shared between triangles: the pack is small, and it keeps this simple.
		for (int corner = 0; corner < 3; corner++) {
			const uint32_t index = primitive.indices[triangle.first + corner];
			Vertex vertex {};
			std::memcpy(vertex.position, &primitive.positions[index], sizeof(vertex.position));
			std::memcpy(vertex.normal, &primitive.normals[index], sizeof(vertex.normal));
			vertex.uv[0] = primitive.uvs[index * 2];
			vertex.uv[1] = primitive.uvs[index * 2 + 1];
			vertex.layer = layer;
			vertex.color = layer >= 0 ? 0x00FFFFFF : PackColor(0.4F, 0.38F, 0.35F);
			vertex.weights[0] = 255;
			part.indices.push_back(static_cast<uint32_t>(part.vertices.size()));
			part.Add(vertex);
		}
	}
	return parts;
}

Part *Largest(std::vector<Part> &parts)
{
	Part *best = nullptr;
	for (Part &part : parts) {
		if (best == nullptr || part.Volume() > best->Volume())
			best = &part;
	}
	return best;
}

void Load()
{
	loaded = true;
	const std::string directory = ModelDirectory();
	// The pack is optional: without it there is nothing to load.
	if (SDL_RWops *probe = SDL_RWFromFile((directory + "/Tristram_plaza.glb").c_str(), "rb"); probe != nullptr)
		SDL_RWclose(probe);
	else
		return;
	const auto everything = [](const GltfPrimitive &) { return true; };
	int scenes = 0;

	if (GltfScene scene; LoadGlb((directory + "/Tristram_plaza.glb").c_str(), scene)) {
		scenes++;
		// The stray guide wires of the scene tie a house to the ground far below it.
		for (Part &part : Split(scene, [](const GltfPrimitive &primitive) { return primitive.material.rfind("wire_", 0) != 0; })) {
			const Vec3 size = part.Size();
			if (size.x < 3 || size.z < 3 || size.y < 2.5F)
				continue; // The well: the town's own is a tile painting that stays.
			part.Finish();
			houses.push_back(std::move(part));
		}
	}
	if (GltfScene scene; LoadGlb((directory + "/church.glb").c_str(), scene)) {
		scenes++;
		std::vector<Part> parts = Split(scene, everything, false);
		if (!parts.empty()) {
			church = std::move(parts[0]);
			church.Finish();
		}
	}
	if (GltfScene scene; LoadGlb((directory + "/Adria_shack_hinges.glb").c_str(), scene)) {
		scenes++;
		// The hut is the largest thing in the scene; her cauldron stands beside it. The rest are spare doors.
		std::vector<Part> parts = Split(scene, everything);
		Part *hut = Largest(parts);
		const Vec3 hutMin = hut->boundsMin;
		const Vec3 hutMax = hut->boundsMax;
		for (const Part &part : parts) {
			constexpr float Reach = 3.0F;
			if (&part != hut && part.boundsMax.x > hutMin.x - Reach && part.boundsMin.x < hutMax.x + Reach && part.boundsMax.z > hutMin.z - Reach && part.boundsMin.z < hutMax.z + Reach)
				hut->Append(part);
		}
		shack = std::move(*hut);
		shack.Finish();
	}
	if (GltfScene scene; LoadGlb((directory + "/hell_stairs.glb").c_str(), scene)) {
		scenes++;
		// The rib cage without the square of ground it is displayed on.
		std::vector<Part> parts = Split(scene, [](const GltfPrimitive &primitive) { return primitive.material == "bone"; }, false);
		if (!parts.empty()) {
			boneStairs = std::move(parts[0]);
			boneStairs.Finish(true);
		}
	}
	if (texturesDirty)
		context->GenerateMips(textures.srv.Get());
	texturesDirty = false;
	packTextureCount = textureNames.size();
	LogInfo("VR: model pack in {}: {} scenes, {} houses, {} textures", directory, scenes, houses.size(), textureNames.size());
}

/**
 * Stands a part in the world: its footprint is stretched over the rectangle of `sizeX` by
 * `sizeZ` meters around (centerX, centerZ), turned a quarter when that fits the shape better.
 * Height follows the mean of the two stretches. `maxScale` caps the stretch: a hut stays a hut on a
 * footprint that takes in its yard.
 */
void Place(const Part &part, float centerX, float centerZ, float sizeX, float sizeZ, float maxScale = 100)
{
	const Vec3 size = part.Size();
	if (part.indices.empty() || size.x <= 0 || size.z <= 0)
		return;
	const bool quarter = (size.x > size.z) != (sizeX > sizeZ);
	const float yaw = quarter ? 1.5707963F : 0;
	const float scaleX = std::min(sizeX / (quarter ? size.z : size.x), maxScale);
	const float scaleZ = std::min(sizeZ / (quarter ? size.x : size.z), maxScale);
	const float scaleY = (scaleX + scaleZ) / 2;
	const float c = std::cos(yaw);
	const float s = std::sin(yaw);
	builder.Clear();
	builder.indices = part.indices;
	builder.vertices.reserve(part.vertices.size());
	for (Vertex vertex : part.vertices) {
		const float *p = vertex.position;
		const float *n = vertex.normal;
		const Vec3 position { (p[0] * c + p[2] * s) * scaleX + centerX, p[1] * scaleY, (p[2] * c - p[0] * s) * scaleZ + centerZ };
		const Vec3 normal { n[0] * c + n[2] * s, n[1], n[2] * c - n[0] * s };
		std::memcpy(vertex.position, &position, sizeof(vertex.position));
		std::memcpy(vertex.normal, &normal, sizeof(vertex.normal));
		builder.vertices.push_back(vertex);
	}
	placed.push_back(builder.Build());
}

/** Things that stand on a single known tile rather than on a building's footprint. */
void PlaceProps()
{
	propsPending = false;
	// The plain look has no models that are not the game's own art.
	if (leveltype != DTYPE_HELL || Classic())
		return;
	// Hell's stairs lead through a cage of ribs.
	for (int i = 0; i < numtrigs; i++) {
		if (trigs[i]._tmsg != WM_DIABNEXTLVL && trigs[i]._tmsg != WM_DIABPREVLVL)
			continue;
		// As wide as it can be and still stand in a room: the player arrives inside it, between the ribs.
		const float span = Knob("DEVILUTIONX_VR_BONE_STAIRS_TILES", 5.0F) * TileSize;
		Place(boneStairs, (static_cast<float>(trigs[i].position.x) + 0.5F) * TileSize, (static_cast<float>(trigs[i].position.y) + 0.5F) * TileSize, span, span);
		LogVerbose("VR: bone stairs at tile {}, {}", trigs[i].position.x, trigs[i].position.y);
	}
}

} // namespace

bool ModelsInit()
{
	textures = CreateTexture(TextureSize, TextureSize, MaxTextures, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, TextureMips);
	return static_cast<bool>(textures);
}

void ModelsRelease()
{
	placed.clear();
	houses.clear();
	church = shack = boneStairs = Part {};
	generated.clear();
	generatedBySprite.clear();
	textureNames.clear();
	textures.Release();
	loaded = false;
}

void ModelsReset()
{
	placed.clear();
	propsPending = true;
	// Sprite data is reloaded with the level and may land on old addresses.
	generatedBySprite.clear();
	if (textureNames.size() > CrowdedTextures) {
		generated.clear();
		textureNames.resize(packTextureCount);
	}
}

const GeneratedModel *ModelsGenerated(const std::string &key)
{
	if (const auto found = generated.find(key); found != generated.end())
		return found->second.get();
	std::unique_ptr<GeneratedModel> &entry = generated[key];
	if (!GeneratedEnabled || AssetsDirectory().empty())
		return nullptr;
	const std::string path = AssetsDirectory() + "/models/" + key + ".glb";
	SDL_RWops *probe = SDL_RWFromFile(path.c_str(), "rb");
	if (probe == nullptr)
		return nullptr;
	SDL_RWclose(probe);
	if (!loaded)
		Load();
	GltfScene scene;
	if (!LoadGlb(path.c_str(), scene))
		return nullptr;
	// Tristram's houses share their textures (they are named alike): each is kept once.
	for (GltfImage &image : scene.images) {
		if (image.name.rfind("tristram_", 0) != 0)
			image.name = key + "#" + image.name;
	}
	liftDarkTextures = false;
	std::vector<Part> parts = Split(scene, [](const GltfPrimitive &) { return true; }, false);
	liftDarkTextures = true;
	if (texturesDirty)
		context->GenerateMips(textures.srv.Get());
	texturesDirty = false;
	if (parts.empty() || parts[0].indices.empty())
		return nullptr;
	// A building is modelled about the tile it is placed on and stays there; anything else is
	// centred on its own footprint.
	if (key.rfind("houses/", 0) != 0)
		parts[0].Finish();
	builder.Clear();
	builder.vertices = parts[0].vertices;
	builder.indices = parts[0].indices;
	entry = std::make_unique<GeneratedModel>();
	entry->mesh = builder.Build();
	entry->size = parts[0].Size();
	LogVerbose("VR: generated model {} ({} triangles)", key, parts[0].indices.size() / 3);
	return entry.get();
}

const GeneratedModel *ModelsForSprite(ClxSprite sprite)
{
	if (const auto found = generatedBySprite.find(sprite.pixelData()); found != generatedBySprite.end())
		return found->second;
	const GeneratedModel *model = nullptr;
	std::string path;
	int frame;
	if (GeneratedEnabled && SpriteOrigin(sprite, path, frame)) {
		// An opened chest has its own model; a frame without one (a barrel mid-burst) keeps the last that had.
		for (int f = frame; f >= 0 && f > frame - 16 && model == nullptr; f--)
			model = ModelsGenerated(path + ".f" + std::to_string(f + 1));
	}
	generatedBySprite.emplace(sprite.pixelData(), model);
	return model;
}

void ModelsAddGenerated(Scene &scene, const GeneratedModel &model, const Mat34 &world, float glow)
{
	MeshInstance instance {};
	instance.world = world;
	instance.color[0] = instance.color[1] = instance.color[2] = 1;
	instance.color[3] = glow;
	instance.boneBase = -1;
	instance.specular = 0.15F;
	instance.lit = 1;
	scene.meshes.push_back({ &model.mesh, instance, true });
}

void ModelsAddGenerated(Scene &scene, const GeneratedModel &model, Vec3 foot, float spriteHeight)
{
	// The art looks down at 30 degrees: the far top edge of a thing shows above its middle's ground
	// line by its height plus half its depth times tan 30.
	const float shown = model.size.y + model.size.z * 0.2887F;
	const float scale = spriteHeight / std::max(shown, 1e-3F);
	// The art's camera stands to the south: +x +z in the world.
	ModelsAddGenerated(scene, model, Compose(foot, AxisAngle({ 0, 1, 0 }, 0.7853982F), { scale, scale, scale }));
}

bool ModelsPlaceBuilding(int minX, int minY, int maxX, int maxY)
{
	if (!loaded)
		Load();
	// Whose house it is: the one by the way down is the church, the one by the witch is hers.
	const auto beside = [&](Point tile) { return tile.x >= minX - 3 && tile.x <= maxX + 3 && tile.y >= minY - 3 && tile.y <= maxY + 3; };
	const Part *part = nullptr;
	const char *name = "house";
	for (int i = 0; i < numtrigs && part == nullptr; i++) {
		if (trigs[i]._tmsg == WM_DIABNEXTLVL && beside(trigs[i].position)) {
			part = &church;
			name = "church";
		}
	}
	for (const Towner &towner : Towners) {
		if (part == nullptr && towner._ttype == TOWN_WITCH && beside(towner.position)) {
			part = &shack;
			name = "Adria's hut";
		}
	}
	const auto width = static_cast<float>(maxX - minX + 1);
	const auto depth = static_cast<float>(maxY - minY + 1);
	if (part == nullptr && !houses.empty()) {
		// The house whose shape is nearest the footprint's; the tile breaks ties so neighbours differ.
		const float wanted = std::max(width, depth) / std::min(width, depth);
		float best = 1e9F;
		for (size_t i = 0; i < houses.size(); i++) {
			const Vec3 size = houses[i].Size();
			const float error = std::fabs(std::max(size.x, size.z) / std::min(size.x, size.z) - wanted) + static_cast<float>((i + minX * 3 + minY * 5) % houses.size()) * 0.02F;
			if (error < best) {
				best = error;
				part = &houses[i];
			}
		}
	}
	if (part == nullptr || part->indices.empty())
		return false;
	Place(*part, (static_cast<float>(minX) + width / 2) * TileSize, (static_cast<float>(minY) + depth / 2) * TileSize, width * TileSize, depth * TileSize, part == &shack ? 1.3F : 100.F);
	LogVerbose("VR: {} on tiles {}, {} to {}, {}", name, minX, minY, maxX, maxY);
	return true;
}

void ModelsAddToScene(Scene &scene)
{
	if (!loaded)
		Load();
	if (propsPending)
		PlaceProps();
	scene.environment.models = textures.srv.Get();
	MeshInstance instance {};
	instance.world = IdentityMat34;
	instance.color[0] = instance.color[1] = instance.color[2] = Knob("DEVILUTIONX_VR_MODEL_BRIGHTNESS", 1.0F);
	instance.boneBase = -1;
	instance.specular = 0.15F;
	instance.lit = 1;
	for (const Mesh &mesh : placed)
		scene.meshes.push_back({ &mesh, instance, true });
}

} // namespace render
} // namespace vr
} // namespace devilution
