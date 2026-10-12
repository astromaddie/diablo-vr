/**
 * @file sprites.cpp
 *
 * The sprite atlas: the game's 8-bit sprites as palette index + coverage, stored once on first
 * use. Colour comes from the palette in the shader, through the game's light tables.
 *
 * A sprite whose file has a hi-res pack (`vrassets/sprites/<path>.png`) is taken from the pack instead of being
 * rasterised: the same picture at three times the resolution, in the same palette indices.
 * Which file a sprite came from is learnt when the game loads it (OnClxSpritesLoaded, and the
 * level's monster types, whose files are loaded another way). Packs are decoded on a worker
 * thread; until one is there its sprites are drawn from the game's own data.
 *
 * The folder is looked for next to the executable and up to four directories above it;
 * DEVILUTIONX_VR_ASSETS=<dir> names it outright.
 */
#include "platform/vr/render/bridge.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "engine/load_cel.hpp"
#include "engine/load_file.hpp"
#include "engine/palette.h"
#include "engine/render/clx_render.hpp"
#include "engine/surface.hpp"
#include "monster.h"
#include "utils/is_of.hpp"
#include "utils/log.hpp"
#include "utils/png.h"
#include "utils/str_cat.hpp"

namespace devilution {
namespace vr {
namespace render {

namespace {

constexpr int AtlasSize = 8192;
constexpr int MaxSpriteSize = 512;

struct Key {
	const void *pixels;
	const void *trn;
	/** The game's own picture with its ground shadow taken out (CacheSpriteArt). */
	bool art = false;
	bool operator==(const Key &other) const { return pixels == other.pixels && trn == other.trn && art == other.art; }
};
struct KeyHash {
	size_t operator()(const Key &key) const { return std::hash<const void *>()(key.pixels) ^ (std::hash<const void *>()(key.trn) * 31) ^ (key.art ? 0x9E3779B9U : 0U); }
};

/** A sprite file's hi-res sheet: cells of `cellWidth` x `cellHeight` art pixels, `scale` texels per pixel. */
struct Pack {
	enum State : int {
		Unloaded,
		Queued,
		Ready,
		Missing,
	};
	std::string path;
	std::atomic<int> state { Unloaded };
	SDL_Surface *sheet = nullptr;
	int scale = 1;
	int cellWidth = 0;
	int cellHeight = 0;
	int columns = 0;
	int rows = 0;
	int transparent = 0;
	/** The colour translation the game applied to the file's sprites after loading (monster types). */
	bool translated = false;
	std::array<uint8_t, 256> trn;
	/** Sprites drawn from the game's data while the pack was still loading. */
	std::vector<Key> fallbacks;
};

/** Where a loaded sprite sits in its file. */
struct Origin {
	uint16_t pack;
	uint16_t column;
	uint16_t row;
	uint16_t width;
	uint16_t height;
};

Texture atlas;
std::unordered_map<Key, SpriteSlot, KeyHash> slots;
/** Images stored by CacheImage; a deque-like list is not needed, the count is small and reserved. */
std::vector<SpriteSlot> imageSlots;
uint32_t epoch;
int shelfX, shelfY, shelfHeight;
bool full;
std::vector<uint8_t> indexAlpha;
std::vector<uint8_t> framed;

// The registry is written by whichever thread loads sprites and read by the renderer.
std::mutex registryMutex;
std::deque<Pack> packs;
std::unordered_map<std::string, uint16_t> packByPath;
std::unordered_map<const void *, Origin> origins;
std::string assetsDirectory;
bool packsEnabled;

std::thread worker;
std::mutex queueMutex;
std::condition_variable queueSignal;
std::deque<Pack *> queue;
bool workerQuit;
std::atomic<int> packsLoading { 0 };
std::atomic<bool> packArrived { false };

size_t monsterTypesSeen;
const void *monsterDataSeen;

Surface Scratch(int index)
{
	static OwnedSurface scratch[2] = { OwnedSurface { MaxSpriteSize, MaxSpriteSize }, OwnedSurface { MaxSpriteSize, MaxSpriteSize } };
	return scratch[index];
}

float Linear(uint8_t value)
{
	return std::pow(static_cast<float>(value) / 255.0F, 2.2F);
}

/** Shelf packing with a one-texel empty border, so filtering never reaches a neighbour. */
bool Allocate(int width, int height, SpriteSlot &slot)
{
	if (shelfX + width + 1 > AtlasSize) {
		shelfX = 1;
		shelfY += shelfHeight + 1;
		shelfHeight = 0;
	}
	if (shelfY + height + 1 > AtlasSize || width + 2 > AtlasSize) {
		if (!full)
			LogVerbose("VR: sprite atlas full ({} sprites), it is emptied on the next frame", slots.size());
		full = true;
		return false;
	}
	slot.x = static_cast<int16_t>(shelfX);
	slot.y = static_cast<int16_t>(shelfY);
	slot.width = static_cast<int16_t>(width);
	slot.height = static_cast<int16_t>(height);
	shelfX += width + 1;
	shelfHeight = std::max(shelfHeight, height);
	return true;
}

/**
 * Puts each texel's distance to the picture's outline into its coverage byte: covered texels hold
 * 128 + 4 * distance in art pixels (up to 31.75), uncovered ones 0. The sprite shader reads a texel
 * as covered from 128 up and bulges an upright sprite towards the viewer by that distance (stereo
 * roundness), and takes a light direction from its slope. Chamfer distance, 3-4 weights.
 */
void EncodeRelief(uint8_t *pairs, int width, int height, int scale)
{
	static std::vector<int> distance;
	distance.assign(static_cast<size_t>(width) * height, 0);
	const auto at = [&](int x, int y) -> int & { return distance[static_cast<size_t>(y) * width + x]; };
	constexpr int Far = 1 << 20;
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++)
			at(x, y) = pairs[(static_cast<size_t>(y) * width + x) * 2 + 1] >= 128 ? Far : 0;
	}
	const auto relax = [&](int x, int y, int dx, int dy, int cost) {
		const int nx = x + dx;
		const int ny = y + dy;
		const int other = nx < 0 || ny < 0 || nx >= width || ny >= height ? 0 : at(nx, ny);
		at(x, y) = std::min(at(x, y), other + cost);
	};
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			if (at(x, y) == 0)
				continue;
			relax(x, y, -1, 0, 3);
			relax(x, y, 0, -1, 3);
			relax(x, y, -1, -1, 4);
			relax(x, y, 1, -1, 4);
		}
	}
	for (int y = height - 1; y >= 0; y--) {
		for (int x = width - 1; x >= 0; x--) {
			if (at(x, y) == 0)
				continue;
			relax(x, y, 1, 0, 3);
			relax(x, y, 0, 1, 3);
			relax(x, y, 1, 1, 4);
			relax(x, y, -1, 1, 4);
		}
	}
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			uint8_t &coverage = pairs[(static_cast<size_t>(y) * width + x) * 2 + 1];
			if (coverage < 128) {
				coverage = 0;
				continue;
			}
			// Chamfer units are thirds of a texel; a texel is 1 / scale art pixels.
			const float pixels = static_cast<float>(at(x, y)) / 3.0F / static_cast<float>(scale);
			coverage = static_cast<uint8_t>(128 + std::min(static_cast<int>(pixels * 4 + 0.5F), 127));
		}
	}
}

/**
 * Stores the painted part of an image (index + alpha pairs, `stride` pairs per row) in the atlas.
 * `scale` is how many of its texels make one art pixel. False when the atlas is full.
 */
bool Store(const uint8_t *pairs, int stride, int width, int height, int scale, SpriteSlot &slot)
{
	int left = width;
	int top = height;
	int right = -1;
	int bottom = -1;
	for (int y = 0; y < height; y++) {
		const uint8_t *row = &pairs[static_cast<size_t>(y) * stride * 2];
		for (int x = 0; x < width; x++) {
			if (row[x * 2 + 1] == 0)
				continue;
			left = std::min(left, x);
			right = std::max(right, x);
			top = std::min(top, y);
			bottom = std::max(bottom, y);
		}
	}
	if (right < 0)
		left = top = right = bottom = 0;
	if (!Allocate(right - left + 1, bottom - top + 1, slot))
		return false;
	slot.scale = static_cast<float>(scale);
	slot.left = static_cast<float>(left) / slot.scale;
	slot.top = static_cast<float>(top) / slot.scale;
	slot.artWidth = static_cast<float>(slot.width) / slot.scale;
	slot.artHeight = static_cast<float>(slot.height) / slot.scale;
	slot.frameWidth = static_cast<int16_t>(width / scale);
	slot.frameHeight = static_cast<int16_t>(height / scale);

	// Uploaded with a transparent one-texel frame around it.
	const int framedWidth = slot.width + 2;
	const int framedHeight = slot.height + 2;
	framed.assign(static_cast<size_t>(framedWidth) * framedHeight * 2, 0);
	float sum[3] = {};
	int opaque = 0;
	for (int y = 0; y < slot.height; y++) {
		const uint8_t *source = &pairs[(static_cast<size_t>(y + top) * stride + left) * 2];
		std::memcpy(&framed[(static_cast<size_t>(y + 1) * framedWidth + 1) * 2], source, static_cast<size_t>(slot.width) * 2);
		// The average colour does not need every texel of a hi-res sprite.
		for (int x = 0; x < slot.width; x += scale) {
			if (source[x * 2 + 1] == 0)
				continue;
			float color[3];
			PaletteLinear(source[x * 2], color);
			for (int c = 0; c < 3; c++)
				sum[c] += color[c];
			opaque++;
		}
	}
	for (int c = 0; c < 3; c++)
		slot.color[c] = opaque != 0 ? sum[c] / static_cast<float>(opaque) : 0;
	EncodeRelief(framed.data(), framedWidth, framedHeight, scale);
	UploadTexture(atlas, 0, slot.x - 1, slot.y - 1, framedWidth, framedHeight, framed.data(), 2);
	return true;
}

// ---------------------------------------------------------------- hi-res packs

std::string FindAssetsDirectory()
{
	if (const char *directory = SDL_getenv("DEVILUTIONX_VR_ASSETS"); directory != nullptr)
		return directory;
	char *base = SDL_GetBasePath();
	std::string path = base != nullptr ? base : "";
	SDL_free(base);
	for (int up = 0; up < 5; up++) {
		const std::string candidate = path + "vrassets";
		if (SDL_RWops *probe = SDL_RWFromFile((candidate + "/vrassets.txt").c_str(), "rb"); probe != nullptr) {
			SDL_RWclose(probe);
			return candidate;
		}
		path += "../";
	}
	return "";
}

/** "Monsters\\SkelAxe\\SklAxN.CL2" -> "monsters/skelaxe/sklaxn". */
std::string PackPath(const char *file)
{
	std::string path = file;
	if (const size_t dot = path.rfind('.'); dot != std::string::npos)
		path.resize(dot);
	for (char &c : path)
		c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return path;
}

/** To be called with the registry locked. */
uint16_t PackFor(const std::string &path)
{
	if (const auto found = packByPath.find(path); found != packByPath.end())
		return found->second;
	const auto index = static_cast<uint16_t>(packs.size());
	packs.emplace_back();
	packs.back().path = path;
	packByPath.emplace(path, index);
	return index;
}

/** To be called with the registry locked. */
void RegisterList(uint16_t pack, ClxSpriteList list, uint16_t row, bool wraps)
{
	const uint32_t count = list.numSprites();
	for (uint32_t i = 0; i < count; i++) {
		const ClxSprite sprite = list[i];
		// A file that is one long strip was laid out sixteen frames to the row.
		const Origin origin { pack, static_cast<uint16_t>(wraps ? i % 16 : i), static_cast<uint16_t>(wraps ? i / 16 : row), sprite.width(), sprite.height() };
		origins[sprite.pixelData()] = origin;
	}
}

uint16_t Register(const std::string &path, ClxSpriteListOrSheet sprites)
{
	const std::lock_guard<std::mutex> lock(registryMutex);
	const uint16_t pack = PackFor(path);
	if (sprites.isSheet()) {
		const ClxSpriteSheet sheet = sprites.sheet();
		for (uint16_t row = 0; row < sheet.numLists(); row++)
			RegisterList(pack, sheet[row], row, false);
	} else {
		RegisterList(pack, sprites.list(), 0, sprites.list().numSprites() > 16);
	}
	return pack;
}

void OnLoaded(const char *path, ClxSpriteListOrSheet sprites)
{
	Register(PackPath(path), sprites);
}

// Set before any sprite is loaded, long before the renderer exists.
const bool hooked = (OnClxSpritesLoaded = &OnLoaded, true);

void LoadPack(Pack &pack)
{
	const std::string base = assetsDirectory + "/sprites/" + pack.path;
	int state = Pack::Missing;
	if (SDL_RWops *meta = SDL_RWFromFile((base + ".txt").c_str(), "rb"); meta != nullptr) {
		char text[128] = {};
		SDL_RWread(meta, text, 1, sizeof(text) - 1);
		SDL_RWclose(meta);
		if (SDL_sscanf(text, "%d %d %d %d %d %d", &pack.scale, &pack.cellWidth, &pack.cellHeight, &pack.columns, &pack.rows, &pack.transparent) == 6 && pack.scale >= 1) {
			if (SDL_RWops *file = SDL_RWFromFile((base + ".png").c_str(), "rb"); file != nullptr) {
				pack.sheet = IMG_LoadPNG_RW(file);
				SDL_RWclose(file);
			}
			if (pack.sheet != nullptr && pack.sheet->format->BitsPerPixel == 8
			    && pack.sheet->w >= pack.columns * pack.cellWidth * pack.scale && pack.sheet->h >= pack.rows * pack.cellHeight * pack.scale) {
				state = Pack::Ready;
			} else {
				SDL_FreeSurface(pack.sheet);
				pack.sheet = nullptr;
			}
		}
	}
	pack.state = state;
	if (state == Pack::Ready)
		packArrived = true;
	else
		LogVerbose("VR: no hi-res pack for {}", pack.path);
	packsLoading--;
}

void Work()
{
	for (;;) {
		Pack *pack;
		{
			std::unique_lock<std::mutex> lock(queueMutex);
			queueSignal.wait(lock, [] { return workerQuit || !queue.empty(); });
			if (workerQuit)
				return;
			pack = queue.front();
			queue.pop_front();
		}
		LoadPack(*pack);
	}
}

void Request(Pack &pack)
{
	pack.state = Pack::Queued;
	packsLoading++;
	{
		const std::lock_guard<std::mutex> lock(queueMutex);
		queue.push_back(&pack);
	}
	queueSignal.notify_one();
}

void WaitForPacks()
{
	while (packsLoading > 0)
		SDL_Delay(1);
}

/** The level's monster types: their files are loaded several to a buffer and recoloured afterwards. */
void RegisterMonsters()
{
	monsterTypesSeen = LevelMonsterTypeCount;
	monsterDataSeen = LevelMonsterTypeCount != 0 && LevelMonsterTypes[0].anims[0].sprites ? LevelMonsterTypes[0].anims[0].sprites->list().data() : nullptr;
	static constexpr char Letters[] = "nwahds";
	int files = 0;
	for (size_t i = 0; i < LevelMonsterTypeCount; i++) {
		const CMonster &type = LevelMonsterTypes[i];
		const MonsterData &data = type.data();
		std::array<uint8_t, 256> trn;
		const bool translated = !data.trnFile.empty();
		if (translated) {
			char path[64];
			*BufCopy(path, "monsters\\", data.trnFile, ".trn") = '\0';
			LoadFileInMem(path, trn);
			std::replace(trn.begin(), trn.end(), 255, 0);
		}
		for (size_t a = 0; a < 6; a++) {
			const AnimStruct &anim = type.anims[a];
			if (!data.hasAnim(a) || !anim.sprites)
				continue;
			const uint16_t index = Register(PackPath(StrCat("monsters\\", data.spritePath(), std::string_view(&Letters[a], 1), ".").c_str()), *anim.sprites);
			Pack &pack = packs[index];
			// The game leaves the second animation of these four untranslated (InitMonsterTRN).
			pack.translated = translated && !(a == 1 && IsAnyOf(type.type, MT_COUNSLR, MT_MAGISTR, MT_CABALIST, MT_ADVOCATE));
			pack.trn = trn;
			// A level's monsters will all be seen: have their sheets ready before they are.
			if (packsEnabled && pack.state == Pack::Unloaded)
				Request(pack);
			files++;
		}
	}
	LogVerbose("VR: {} monster sprite files of {} monster types registered", files, LevelMonsterTypeCount);
}

/** The hi-res picture of a sprite as index + alpha pairs in `indexAlpha`, or false when it has none (yet). */
bool HiresPixels(ClxSprite sprite, const uint8_t *trn, const Key &key, int &width, int &height, int &scale)
{
	if (!packsEnabled)
		return false;
	Origin origin;
	Pack *pack;
	{
		const std::lock_guard<std::mutex> lock(registryMutex);
		const auto found = origins.find(sprite.pixelData());
		if (found == origins.end() || found->second.width != sprite.width() || found->second.height != sprite.height())
			return false;
		origin = found->second;
		pack = &packs[origin.pack];
	}
	if (pack->state == Pack::Unloaded)
		Request(*pack);
	if (pack->state != Pack::Ready) {
		if (pack->state == Pack::Queued)
			pack->fallbacks.push_back(key);
		return false;
	}
	if (origin.column >= pack->columns || origin.row >= pack->rows)
		return false;
	scale = pack->scale;
	width = std::min<int>(sprite.width(), pack->cellWidth) * scale;
	height = std::min<int>(sprite.height(), pack->cellHeight) * scale;
	indexAlpha.resize(static_cast<size_t>(width) * height * 2);
	const SDL_Surface *sheet = pack->sheet;
	const auto transparent = static_cast<uint8_t>(pack->transparent);
	for (int y = 0; y < height; y++) {
		const uint8_t *source = static_cast<const uint8_t *>(sheet->pixels) + static_cast<size_t>(origin.row * pack->cellHeight * scale + y) * sheet->pitch + origin.column * pack->cellWidth * scale;
		uint8_t *out = &indexAlpha[static_cast<size_t>(y) * width * 2];
		for (int x = 0; x < width; x++) {
			uint8_t index = source[x];
			const bool painted = index != transparent;
			if (pack->translated)
				index = pack->trn[index];
			if (trn != nullptr)
				index = trn[index];
			out[x * 2] = painted ? index : 0;
			out[x * 2 + 1] = painted ? 255 : 0;
		}
	}
	return true;
}

} // namespace

bool SpriteOrigin(ClxSprite sprite, std::string &path, int &frame)
{
	const std::lock_guard<std::mutex> lock(registryMutex);
	const auto found = origins.find(sprite.pixelData());
	if (found == origins.end() || found->second.width != sprite.width() || found->second.height != sprite.height())
		return false;
	path = packs[found->second.pack].path;
	// Sheets keep one direction per row; a long strip was wrapped at sixteen frames.
	frame = found->second.column + found->second.row * 16;
	return true;
}

const std::string &AssetsDirectory()
{
	static const std::string directory = FindAssetsDirectory();
	return directory;
}

void PaletteLinear(uint8_t index, float rgb[3])
{
	const SDL_Color color = logical_palette[index];
	rgb[0] = Linear(color.r);
	rgb[1] = Linear(color.g);
	rgb[2] = Linear(color.b);
}

uint32_t PaletteRgba(uint8_t index, uint8_t alpha)
{
	const SDL_Color color = logical_palette[index];
	return color.r | color.g << 8 | color.b << 16 | static_cast<uint32_t>(alpha) << 24;
}

bool SpritesInit()
{
	atlas = CreateTexture(AtlasSize, AtlasSize, 1, DXGI_FORMAT_R8G8_UNORM);
	// Carved models' textures, their moving parts and the town's pictures: a few hundred per level.
	imageSlots.reserve(4096);
	assetsDirectory = AssetsDirectory();
	// Hi-res packs are optional and off unless asked for. Creatures are the game's own pixels.
	packsEnabled = !assetsDirectory.empty() && Knob("DEVILUTIONX_VR_HIRES_SPRITES", 0) != 0;
	LogInfo("VR: hi-res sprite packs: {}", packsEnabled ? assetsDirectory.c_str() : "none");
	if (packsEnabled && !worker.joinable()) {
		workerQuit = false;
		worker = std::thread(Work);
	}
	SpritesReset();
	return static_cast<bool>(atlas);
}

void SpritesRelease()
{
	if (worker.joinable()) {
		WaitForPacks();
		{
			const std::lock_guard<std::mutex> lock(queueMutex);
			workerQuit = true;
		}
		queueSignal.notify_one();
		worker.join();
	}
	SpritesLevelChanged();
	atlas.Release();
	slots.clear();
	imageSlots.clear();
}

uint32_t SpritesEpoch()
{
	return epoch;
}

void SpritesReset()
{
	epoch++;
	slots.clear();
	imageSlots.clear();
	for (Pack &pack : packs)
		pack.fallbacks.clear();
	shelfX = 1;
	shelfY = 1;
	shelfHeight = 0;
	full = false;
}

void SpritesLevelChanged()
{
	// Sheets are large: only those of the level being played are kept.
	WaitForPacks();
	const std::lock_guard<std::mutex> lock(registryMutex);
	for (Pack &pack : packs) {
		SDL_FreeSurface(pack.sheet);
		pack.sheet = nullptr;
		pack.state = Pack::Unloaded;
		pack.fallbacks.clear();
	}
	monsterTypesSeen = 0;
	monsterDataSeen = nullptr;
}

void SpritesSync()
{
	const void *first = LevelMonsterTypeCount != 0 && LevelMonsterTypes[0].anims[0].sprites ? LevelMonsterTypes[0].anims[0].sprites->list().data() : nullptr;
	if (leveltype != DTYPE_TOWN && (LevelMonsterTypeCount != monsterTypesSeen || first != monsterDataSeen))
		RegisterMonsters();
	// Sprites that were drawn from the game's data because their pack was not there yet are done again.
	if (packArrived.exchange(false)) {
		const std::lock_guard<std::mutex> lock(registryMutex);
		for (Pack &pack : packs) {
			if (pack.state != Pack::Ready || pack.fallbacks.empty())
				continue;
			for (const Key &key : pack.fallbacks)
				slots.erase(key);
			pack.fallbacks.clear();
		}
	}
}

bool SpritesFull()
{
	return full;
}

const uint8_t *RasterizeSprite(ClxSprite sprite, const uint8_t *trn)
{
	const int width = sprite.width();
	const int height = sprite.height();
	if (width > MaxSpriteSize || height > MaxSpriteSize)
		return nullptr;
	// Drawn twice over different backgrounds: a pixel the draw did not touch differs between the two.
	for (int pass = 0; pass < 2; pass++) {
		const Surface out = Scratch(pass).subregion(0, 0, width, height);
		for (int y = 0; y < height; y++)
			std::memset(out.at(0, y), pass, width);
		if (trn != nullptr)
			RenderClxSpriteWithTRN(out, sprite, { 0, 0 }, trn);
		else
			RenderClxSprite(out, sprite, { 0, 0 });
	}
	indexAlpha.resize(std::max(indexAlpha.size(), static_cast<size_t>(width) * height * 2));
	for (int y = 0; y < height; y++) {
		const uint8_t *a = Scratch(0).at(0, y);
		const uint8_t *b = Scratch(1).at(0, y);
		uint8_t *out = &indexAlpha[static_cast<size_t>(y) * width * 2];
		for (int x = 0; x < width; x++) {
			out[x * 2] = a[x];
			out[x * 2 + 1] = a[x] == b[x] ? 255 : 0;
		}
	}
	return indexAlpha.data();
}

const SpriteSlot *CacheSprite(ClxSprite sprite, const uint8_t *trn)
{
	const Key key { sprite.pixelData(), trn };
	if (const auto found = slots.find(key); found != slots.end())
		return &found->second;
	int width = sprite.width();
	int height = sprite.height();
	int scale = 1;
	if (!HiresPixels(sprite, trn, key, width, height, scale) && RasterizeSprite(sprite, trn) == nullptr)
		return nullptr;
	SpriteSlot slot {};
	if (!Store(indexAlpha.data(), width, width, height, scale, slot))
		return nullptr;
	return &slots.emplace(key, slot).first->second;
}

const SpriteSlot *CacheSpriteArt(ClxSprite sprite, const uint8_t *trn)
{
	const Key key { sprite.pixelData(), trn, true };
	if (const auto found = slots.find(key); found != slots.end())
		return &found->second;
	const int width = sprite.width();
	const int height = sprite.height();
	// Untranslated: the shadow is index 0 in the art, whatever a colour translation makes of it.
	if (RasterizeSprite(sprite, nullptr) == nullptr)
		return nullptr;
	// Index 0 is the shadow unless painted pixels close it in on all four sides (the silhouette rule the models were built with).
	static std::vector<uint8_t> closed;
	closed.assign(static_cast<size_t>(width) * height, 0);
	const auto body = [&](int x, int y) { return indexAlpha[(static_cast<size_t>(y) * width + x) * 2 + 1] != 0 && indexAlpha[(static_cast<size_t>(y) * width + x) * 2] != 0; };
	for (int y = 0; y < height; y++) {
		bool seen = false;
		for (int x = 0; x < width; x++) {
			seen = seen || body(x, y);
			closed[static_cast<size_t>(y) * width + x] |= seen ? 1 : 0;
		}
		seen = false;
		for (int x = width - 1; x >= 0; x--) {
			seen = seen || body(x, y);
			closed[static_cast<size_t>(y) * width + x] |= seen ? 2 : 0;
		}
	}
	for (int x = 0; x < width; x++) {
		bool seen = false;
		for (int y = 0; y < height; y++) {
			seen = seen || body(x, y);
			closed[static_cast<size_t>(y) * width + x] |= seen ? 4 : 0;
		}
		seen = false;
		for (int y = height - 1; y >= 0; y--) {
			seen = seen || body(x, y);
			closed[static_cast<size_t>(y) * width + x] |= seen ? 8 : 0;
		}
	}
	for (size_t i = 0; i < closed.size(); i++) {
		uint8_t *pair = &indexAlpha[i * 2];
		if (pair[1] != 0 && pair[0] == 0 && closed[i] != 15)
			pair[1] = 0;
		else if (trn != nullptr)
			pair[0] = trn[pair[0]];
	}
	SpriteSlot slot {};
	if (!Store(indexAlpha.data(), width, width, height, 1, slot))
		return nullptr;
	return &slots.emplace(key, slot).first->second;
}

const SpriteSlot *CacheImage(const uint8_t *pairs, int width, int height)
{
	SpriteSlot slot {};
	if (imageSlots.size() == imageSlots.capacity() || !Store(pairs, width, width, height, 1, slot))
		return nullptr;
	imageSlots.push_back(slot);
	return &imageSlots.back();
}

ID3D11ShaderResourceView *SpriteAtlasView()
{
	return atlas.srv.Get();
}

} // namespace render
} // namespace vr
} // namespace devilution
