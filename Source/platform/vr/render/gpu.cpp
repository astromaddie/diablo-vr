/**
 * @file gpu.cpp
 *
 * D3D11 resource helpers of the 3D renderer.
 */
#include "platform/vr/render/gpu.h"

#include <algorithm>
#include <cstring>

#include <d3dcompiler.h>

#include "utils/log.hpp"

namespace devilution {
namespace vr {
namespace render {

ID3D11Device *device;
ID3D11DeviceContext *context;
uint64_t memoryBytes[static_cast<size_t>(MemoryKind::Count)];

namespace {

uint32_t BytesPerPixel(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_R8_UNORM:
		return 1;
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return 8;
	default:
		return 4;
	}
}

} // namespace

Texture &Texture::operator=(Texture &&other) noexcept
{
	if (this != &other) {
		Release();
		texture = std::move(other.texture);
		srv = std::move(other.srv);
		rtv = std::move(other.rtv);
		dsv = std::move(other.dsv);
		sliceRtv = std::move(other.sliceRtv);
		sliceDsv = std::move(other.sliceDsv);
		width = other.width;
		height = other.height;
		layers = other.layers;
		bytes = other.bytes;
		kind = other.kind;
		other.bytes = 0;
	}
	return *this;
}

Texture::~Texture()
{
	Release();
}

void Texture::Release()
{
	memoryBytes[static_cast<size_t>(kind)] -= bytes;
	bytes = 0;
	sliceRtv.clear();
	sliceDsv.clear();
	rtv.Reset();
	dsv.Reset();
	srv.Reset();
	texture.Reset();
}

Texture CreateTexture(int width, int height, int layers, DXGI_FORMAT format, uint32_t flags, int samples, MemoryKind kind)
{
	Texture result;
	const bool depth = (flags & TextureDepth) != 0;
	const bool mips = (flags & TextureMips) != 0;
	D3D11_TEXTURE2D_DESC desc {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = mips ? 0 : 1;
	desc.ArraySize = layers;
	desc.Format = depth ? DXGI_FORMAT_R32_TYPELESS : format;
	desc.SampleDesc.Count = samples;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (depth)
		desc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
	if ((flags & TextureRenderTarget) != 0 || mips)
		desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
	if (mips)
		desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, &result.texture))) {
		LogError("VR: texture {}x{}x{} (format {}) could not be created", width, height, layers, static_cast<int>(format));
		return result;
	}

	const bool multisampled = samples > 1;
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc {};
	srvDesc.Format = depth ? DXGI_FORMAT_R32_FLOAT : format;
	if (multisampled) {
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
		srvDesc.Texture2DMSArray.ArraySize = layers;
	} else {
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Texture2DArray.MipLevels = static_cast<UINT>(-1);
		srvDesc.Texture2DArray.ArraySize = layers;
	}
	device->CreateShaderResourceView(result.texture.Get(), &srvDesc, &result.srv);

	const auto renderView = [&](int first, int count, ID3D11RenderTargetView **view) {
		D3D11_RENDER_TARGET_VIEW_DESC viewDesc {};
		viewDesc.Format = format;
		if (multisampled) {
			viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
			viewDesc.Texture2DMSArray.FirstArraySlice = first;
			viewDesc.Texture2DMSArray.ArraySize = count;
		} else {
			viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			viewDesc.Texture2DArray.FirstArraySlice = first;
			viewDesc.Texture2DArray.ArraySize = count;
		}
		device->CreateRenderTargetView(result.texture.Get(), &viewDesc, view);
	};
	const auto depthView = [&](int first, int count, ID3D11DepthStencilView **view) {
		D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc {};
		viewDesc.Format = DXGI_FORMAT_D32_FLOAT;
		if (multisampled) {
			viewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
			viewDesc.Texture2DMSArray.FirstArraySlice = first;
			viewDesc.Texture2DMSArray.ArraySize = count;
		} else {
			viewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
			viewDesc.Texture2DArray.FirstArraySlice = first;
			viewDesc.Texture2DArray.ArraySize = count;
		}
		device->CreateDepthStencilView(result.texture.Get(), &viewDesc, view);
	};
	if ((flags & TextureRenderTarget) != 0) {
		renderView(0, layers, &result.rtv);
		if ((flags & TextureSliceViews) != 0) {
			result.sliceRtv.resize(layers);
			for (int i = 0; i < layers; i++)
				renderView(i, 1, &result.sliceRtv[i]);
		}
	}
	if (depth) {
		depthView(0, layers, &result.dsv);
		if ((flags & TextureSliceViews) != 0) {
			result.sliceDsv.resize(layers);
			for (int i = 0; i < layers; i++)
				depthView(i, 1, &result.sliceDsv[i]);
		}
	}

	result.width = width;
	result.height = height;
	result.layers = layers;
	result.kind = kind;
	result.bytes = static_cast<uint64_t>(width) * height * layers * samples * BytesPerPixel(format);
	if (mips)
		result.bytes += result.bytes / 3;
	memoryBytes[static_cast<size_t>(kind)] += result.bytes;
	return result;
}

void UploadTexture(const Texture &texture, int layer, int x, int y, int width, int height, const void *pixels, int bytesPerPixel)
{
	if (width <= 0 || height <= 0 || !texture)
		return;
	D3D11_TEXTURE2D_DESC desc;
	texture.texture->GetDesc(&desc);
	const D3D11_BOX box { static_cast<UINT>(x), static_cast<UINT>(y), 0, static_cast<UINT>(x + width), static_cast<UINT>(y + height), 1 };
	context->UpdateSubresource(texture.texture.Get(), D3D11CalcSubresource(0, layer, desc.MipLevels), &box, pixels, width * bytesPerPixel, 0);
}

DynamicBuffer::~DynamicBuffer()
{
	Release();
}

void DynamicBuffer::Release()
{
	memoryBytes[static_cast<size_t>(MemoryKind::Dynamic)] -= capacity;
	capacity = 0;
	srv.Reset();
	buffer.Reset();
}

void DynamicBuffer::Upload(const void *data, uint32_t bytes, UINT bindFlags, uint32_t structureStride, DXGI_FORMAT format)
{
	if (bytes == 0)
		return;
	if (bytes > capacity || bindFlags != bind || structureStride != stride) {
		Release();
		// Constant buffers are sized exactly (in 16-byte units); the others get headroom so growth is rare.
		const bool constants = (bindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
		D3D11_BUFFER_DESC desc {};
		desc.ByteWidth = constants ? (bytes + 15) / 16 * 16 : std::max<uint32_t>(bytes + bytes / 2, 4096);
		if (structureStride != 0)
			desc.ByteWidth = desc.ByteWidth / structureStride * structureStride;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = bindFlags;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (structureStride != 0 && format == DXGI_FORMAT_UNKNOWN) {
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = structureStride;
		}
		if (FAILED(device->CreateBuffer(&desc, nullptr, &buffer))) {
			LogError("VR: dynamic buffer of {} bytes could not be created", desc.ByteWidth);
			return;
		}
		if ((bindFlags & D3D11_BIND_SHADER_RESOURCE) != 0) {
			D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
			viewDesc.Format = format;
			viewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			viewDesc.Buffer.NumElements = desc.ByteWidth / structureStride;
			device->CreateShaderResourceView(buffer.Get(), &viewDesc, &srv);
		}
		capacity = desc.ByteWidth;
		bind = bindFlags;
		stride = structureStride;
		memoryBytes[static_cast<size_t>(MemoryKind::Dynamic)] += capacity;
	}
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (SUCCEEDED(context->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		std::memcpy(mapped.pData, data, bytes);
		context->Unmap(buffer.Get(), 0);
	}
}

ComPtr<ID3D11Buffer> CreateStaticBuffer(const void *data, uint32_t bytes, UINT bindFlags)
{
	ComPtr<ID3D11Buffer> buffer;
	D3D11_BUFFER_DESC desc {};
	desc.ByteWidth = bytes;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = bindFlags;
	const D3D11_SUBRESOURCE_DATA initial { data, 0, 0 };
	if (SUCCEEDED(device->CreateBuffer(&desc, &initial, &buffer)))
		memoryBytes[static_cast<size_t>(MemoryKind::Geometry)] += bytes;
	return buffer;
}

void ReleaseStaticBytes(uint32_t bytes)
{
	memoryBytes[static_cast<size_t>(MemoryKind::Geometry)] -= bytes;
}

ComPtr<ID3DBlob> CompileShader(std::string_view source, const char *entry, const char *profile)
{
	ComPtr<ID3DBlob> blob;
	ComPtr<ID3DBlob> errors;
	if (FAILED(D3DCompile(source.data(), source.size(), "vr_render", nullptr, nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors))) {
		LogError("VR: shader {} failed to compile: {}", entry, errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
		return nullptr;
	}
	return blob;
}

bool SupportsLayeredVertexShader()
{
	D3D11_FEATURE_DATA_D3D11_OPTIONS3 options {};
	return SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &options, sizeof(options)))
	    && options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer != FALSE;
}

} // namespace render
} // namespace vr
} // namespace devilution
