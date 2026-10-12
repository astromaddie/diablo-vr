/**
 * @file gltf.cpp
 *
 * Binary glTF reader. See gltf.h for what it covers.
 */
#include "platform/vr/render/gltf.h"

#include <cstdlib>
#include <cstring>

#include "utils/log.hpp"
#include "utils/png.h"

namespace devilution {
namespace vr {
namespace render {

namespace {

/** A parsed JSON value. Looking up what is not there gives a null value, so chains of lookups need no checks. */
struct Json {
	enum Type : uint8_t {
		Null,
		Number,
		String,
		Array,
		Object,
	};
	Type type = Null;
	double number = 0;
	std::string string;
	std::vector<Json> items;
	/** Objects: keys[i] names items[i]. */
	std::vector<std::string> keys;

	const Json &operator[](const char *key) const
	{
		for (size_t i = 0; i < keys.size(); i++) {
			if (keys[i] == key)
				return items[i];
		}
		return None();
	}
	const Json &operator[](int64_t index) const
	{
		return type == Array && index >= 0 && index < static_cast<int64_t>(items.size()) ? items[static_cast<int64_t>(index)] : None();
	}
	explicit operator bool() const { return type != Null; }
	/** Indices and counts; -1 (or `fallback`) when absent. */
	int Int(int fallback = -1) const { return type == Number ? static_cast<int>(number) : fallback; }
	float Float(float fallback = 0) const { return type == Number ? static_cast<float>(number) : fallback; }

	static const Json &None()
	{
		static const Json none;
		return none;
	}
};

struct JsonParser {
	const char *at;
	const char *end;
	bool failed = false;
	int depth = 0;

	void SkipSpace()
	{
		while (at < end && (*at == ' ' || *at == '\n' || *at == '\r' || *at == '\t'))
			at++;
	}

	bool Take(char c)
	{
		SkipSpace();
		if (at < end && *at == c) {
			at++;
			return true;
		}
		return false;
	}

	std::string ParseString()
	{
		std::string out;
		while (at < end && *at != '"') {
			if (*at == '\\' && at + 1 < end) {
				at++;
				switch (*at) {
				case 'n':
					out += '\n';
					break;
				case 't':
					out += '\t';
					break;
				case 'u':
					// Names are only compared and logged: other scripts need not survive.
					out += '?';
					at += std::min<ptrdiff_t>(4, end - at - 1);
					break;
				default:
					out += *at;
					break;
				}
				at++;
			} else {
				out += *at++;
			}
		}
		failed = failed || at >= end;
		at++;
		return out;
	}

	Json Parse()
	{
		Json value;
		SkipSpace();
		if (at >= end || ++depth > 64) {
			failed = true;
			return value;
		}
		if (Take('{')) {
			value.type = Json::Object;
			while (!failed && !Take('}')) {
				if (!value.items.empty() && !Take(','))
					failed = true;
				if (!Take('"'))
					failed = true;
				value.keys.push_back(ParseString());
				if (!Take(':'))
					failed = true;
				value.items.push_back(Parse());
			}
		} else if (Take('[')) {
			value.type = Json::Array;
			while (!failed && !Take(']')) {
				if (!value.items.empty() && !Take(','))
					failed = true;
				value.items.push_back(Parse());
			}
		} else if (Take('"')) {
			value.type = Json::String;
			value.string = ParseString();
		} else if (*at == 't' || *at == 'f' || *at == 'n') {
			if (*at != 'n') {
				value.type = Json::Number;
				value.number = *at == 't' ? 1 : 0;
			}
			while (at < end && *at >= 'a' && *at <= 'z')
				at++;
		} else {
			const std::string text(at, static_cast<size_t>(std::min<ptrdiff_t>(end - at, 40)));
			char *after = nullptr;
			// Not SDL_strtod: on Windows SDL brings its own, which stops at an exponent.
			value.number = std::strtod(text.c_str(), &after);
			if (after == text.c_str())
				failed = true;
			value.type = Json::Number;
			at += after - text.c_str();
		}
		depth--;
		return value;
	}
};

struct Bytes {
	const uint8_t *data = nullptr;
	size_t size = 0;
};

/** The bytes of a buffer view, or nothing when it does not lie inside the binary chunk. */
Bytes ViewBytes(const Json &gltf, Bytes bin, int view)
{
	const Json &bufferView = gltf["bufferViews"][static_cast<int64_t>(view)];
	const int offset = bufferView["byteOffset"].Int(0);
	const int length = bufferView["byteLength"].Int(0);
	if (view < 0 || !bufferView || bufferView["buffer"].Int(0) != 0 || offset < 0 || length <= 0 || static_cast<size_t>(offset) + static_cast<size_t>(length) > bin.size)
		return {};
	return { bin.data + offset, static_cast<size_t>(length) };
}

/** An accessor's elements, checked against the buffer: `count` elements of `components` values, `stride` bytes apart. */
struct Accessor {
	const uint8_t *data = nullptr;
	size_t stride = 0;
	int count = 0;
	int componentType = 0;

	float FloatAt(int element, int component) const
	{
		float value;
		std::memcpy(&value, data + element * stride + component * sizeof(float), sizeof(value));
		return value;
	}
	uint32_t IndexAt(int element) const
	{
		const uint8_t *p = data + element * stride;
		switch (componentType) {
		case 5121:
			return *p;
		case 5123:
			return static_cast<uint32_t>(p[0] | p[1] << 8);
		default: {
			uint32_t value;
			std::memcpy(&value, p, sizeof(value));
			return value;
		}
		}
	}
};

Accessor Access(const Json &gltf, Bytes bin, int index, int components, bool wantFloat)
{
	const Json &accessor = gltf["accessors"][static_cast<int64_t>(index)];
	if (index < 0 || !accessor)
		return {};
	Accessor out;
	out.componentType = accessor["componentType"].Int();
	out.count = accessor["count"].Int(0);
	const size_t componentSize = out.componentType == 5121 ? 1 : (out.componentType == 5123 ? 2 : 4);
	const bool isFloat = out.componentType == 5126;
	const bool isIndex = out.componentType == 5121 || out.componentType == 5123 || out.componentType == 5125;
	const char *type = components == 1 ? "SCALAR" : (components == 2 ? "VEC2" : "VEC3");
	if (accessor["type"].string != type || (wantFloat ? !isFloat : !isIndex) || out.count <= 0)
		return {};
	const Json &bufferView = gltf["bufferViews"][static_cast<int64_t>(accessor["bufferView"].Int())];
	const Bytes view = ViewBytes(gltf, bin, accessor["bufferView"].Int());
	const size_t elementSize = componentSize * components;
	out.stride = static_cast<size_t>(bufferView["byteStride"].Int(0));
	if (out.stride == 0)
		out.stride = elementSize;
	const int offset = accessor["byteOffset"].Int(0);
	if (view.data == nullptr || offset < 0 || out.stride < elementSize
	    || static_cast<size_t>(offset) + static_cast<size_t>(out.count - 1) * out.stride + elementSize > view.size)
		return {};
	out.data = view.data + offset;
	return out;
}

Mat34 NodeTransform(const Json &node)
{
	if (const Json &m = node["matrix"]; m.items.size() == 16) {
		// Column-major in the file.
		Mat34 out;
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 4; j++)
				out.m[i][j] = m[static_cast<int64_t>(j * 4 + i)].Float();
		}
		return out;
	}
	const Json &t = node["translation"];
	const Json &r = node["rotation"];
	const Json &s = node["scale"];
	const Quat rotation = r.items.size() == 4 ? Quat { r[int64_t { 0 }].Float(), r[int64_t { 1 }].Float(), r[int64_t { 2 }].Float(), r[int64_t { 3 }].Float() } : IdentityQuat;
	const Vec3 scale = s.items.size() == 3 ? Vec3 { s[int64_t { 0 }].Float(), s[int64_t { 1 }].Float(), s[int64_t { 2 }].Float() } : Vec3 { 1, 1, 1 };
	return Compose({ t[int64_t { 0 }].Float(), t[int64_t { 1 }].Float(), t[int64_t { 2 }].Float() }, rotation, scale);
}

void AddNode(const Json &gltf, Bytes bin, int index, const Mat34 &parent, int depth, GltfScene &scene)
{
	const Json &node = gltf["nodes"][static_cast<int64_t>(index)];
	if (index < 0 || !node || depth > 32)
		return;
	const Mat34 world = parent * NodeTransform(node);
	for (const Json &primitive : gltf["meshes"][static_cast<int64_t>(node["mesh"].Int())]["primitives"].items) {
		if (primitive["mode"].Int(4) != 4)
			continue; // Triangle lists only.
		const Json &attributes = primitive["attributes"];
		const Accessor positions = Access(gltf, bin, attributes["POSITION"].Int(), 3, true);
		const Accessor normals = Access(gltf, bin, attributes["NORMAL"].Int(), 3, true);
		const Accessor uvs = Access(gltf, bin, attributes["TEXCOORD_0"].Int(), 2, true);
		const Accessor indices = Access(gltf, bin, primitive["indices"].Int(), 1, false);
		if (positions.data == nullptr)
			continue;
		GltfPrimitive out;
		out.node = node["name"].string;
		const Json &material = gltf["materials"][static_cast<int64_t>(primitive["material"].Int())];
		out.material = material["name"].string;
		const int texture = material["pbrMetallicRoughness"]["baseColorTexture"]["index"].Int();
		out.image = gltf["textures"][static_cast<int64_t>(texture)]["source"].Int();
		if (out.image >= static_cast<int>(scene.images.size()))
			out.image = -1;
		const Vec3 origin = Transform(world, { 0, 0, 0 });
		out.positions.reserve(positions.count);
		out.normals.reserve(positions.count);
		out.uvs.reserve(static_cast<size_t>(positions.count) * 2);
		for (int i = 0; i < positions.count; i++) {
			out.positions.push_back(Transform(world, { positions.FloatAt(i, 0), positions.FloatAt(i, 1), positions.FloatAt(i, 2) }));
			const bool hasNormal = normals.data != nullptr && i < normals.count;
			const Vec3 normal = hasNormal ? Vec3 { normals.FloatAt(i, 0), normals.FloatAt(i, 1), normals.FloatAt(i, 2) } : Vec3 { 0, 1, 0 };
			out.normals.push_back(Normalize(Transform(world, normal) - origin));
			const bool hasUv = uvs.data != nullptr && i < uvs.count;
			out.uvs.push_back(hasUv ? uvs.FloatAt(i, 0) : 0);
			out.uvs.push_back(hasUv ? uvs.FloatAt(i, 1) : 0);
		}
		const int indexCount = (indices.data != nullptr ? indices.count : positions.count) / 3 * 3;
		out.indices.reserve(indexCount);
		bool valid = true;
		for (int i = 0; i < indexCount; i++) {
			const uint32_t vertex = indices.data != nullptr ? indices.IndexAt(i) : static_cast<uint32_t>(i);
			valid = valid && vertex < static_cast<uint32_t>(positions.count);
			out.indices.push_back(vertex);
		}
		if (valid && !out.indices.empty())
			scene.primitives.push_back(std::move(out));
	}
	for (const Json &child : node["children"].items)
		AddNode(gltf, bin, child.Int(), world, depth + 1, scene);
}

uint32_t ReadU32(const uint8_t *p)
{
	uint32_t value;
	std::memcpy(&value, p, sizeof(value));
	return value;
}

} // namespace

GltfScene::~GltfScene()
{
	for (GltfImage &image : images)
		SDL_FreeSurface(image.pixels);
}

bool LoadGlb(const char *path, GltfScene &scene)
{
	SDL_RWops *file = SDL_RWFromFile(path, "rb");
	if (file == nullptr) {
		LogError("VR: cannot open model {}", path);
		return false;
	}
	std::vector<uint8_t> data(static_cast<size_t>(std::max<Sint64>(SDL_RWsize(file), 0)));
	const size_t read = SDL_RWread(file, data.data(), 1, data.size());
	SDL_RWclose(file);

	constexpr uint32_t Magic = 0x46546C67; // "glTF"
	constexpr uint32_t JsonChunk = 0x4E4F534A;
	constexpr uint32_t BinChunk = 0x004E4942;
	if (read != data.size() || data.size() < 20 || ReadU32(data.data()) != Magic || ReadU32(data.data() + 4) != 2) {
		LogError("VR: {} is not a binary glTF 2.0 file", path);
		return false;
	}
	Bytes json;
	Bytes bin;
	for (size_t at = 12; at + 8 <= data.size();) {
		const size_t length = ReadU32(data.data() + at);
		const uint32_t type = ReadU32(data.data() + at + 4);
		if (length > data.size() - at - 8)
			break;
		if (type == JsonChunk && json.data == nullptr)
			json = { data.data() + at + 8, length };
		else if (type == BinChunk && bin.data == nullptr)
			bin = { data.data() + at + 8, length };
		at += 8 + ((length + 3) & ~static_cast<size_t>(3));
	}
	JsonParser parser { reinterpret_cast<const char *>(json.data), reinterpret_cast<const char *>(json.data) + json.size };
	const Json gltf = json.data != nullptr ? parser.Parse() : Json {};
	if (parser.failed || gltf.type != Json::Object) {
		LogError("VR: {} has no readable glTF description", path);
		return false;
	}

	for (const Json &image : gltf["images"].items) {
		GltfImage out;
		out.name = image["name"].string;
		const Bytes bytes = ViewBytes(gltf, bin, image["bufferView"].Int());
		if (bytes.data != nullptr) {
			SDL_Surface *decoded = IMG_LoadPNG_RW(SDL_RWFromConstMem(bytes.data, static_cast<int>(bytes.size)));
			if (decoded != nullptr) {
				out.pixels = SDL_ConvertSurfaceFormat(decoded, SDL_PIXELFORMAT_RGBA32, 0);
				SDL_FreeSurface(decoded);
			}
		}
		if (out.pixels == nullptr)
			LogError("VR: {}: image '{}' is not an embedded PNG, its surfaces stay untextured", path, out.name);
		scene.images.push_back(out);
	}
	const Json &roots = gltf["scenes"][static_cast<int64_t>(gltf["scene"].Int(0))]["nodes"];
	for (const Json &root : roots.items)
		AddNode(gltf, bin, root.Int(), IdentityMat34, 0, scene);
	if (scene.primitives.empty()) {
		LogError("VR: {} holds no triangle meshes", path);
		return false;
	}
	return true;
}

} // namespace render
} // namespace vr
} // namespace devilution
