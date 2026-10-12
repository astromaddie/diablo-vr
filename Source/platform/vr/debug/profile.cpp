/**
 * @file profile.cpp
 *
 * Frame timing for the VR renderer.
 */
#include "platform/vr/debug/profile.h"

#include "platform/vr/vr_internal.h"

#include <dxgi1_4.h>

#include "platform/vr/render/renderer.h"
#include "utils/log.hpp"

namespace devilution {
namespace vr {

namespace {

/** A frame's queries are read when its slot comes round again, three frames later. */
constexpr int RingSize = 4;
constexpr size_t PassCount = static_cast<size_t>(GpuPass::Count);

struct Slot {
	ID3D11Query *disjoint;
	ID3D11Query *begin;
	ID3D11Query *end;
	ID3D11Query *passBegin[PassCount];
	ID3D11Query *passEnd[PassCount];
	bool passUsed[PassCount];
	FrameSample sample;
	bool pending;
};

Slot slots[RingSize];
unsigned frameIndex;
bool inFrame;
uint64_t frameStart;
FrameSample current;
FrameSample latest;
std::vector<FrameSample> *recordSink;

// Running totals for the log line and the frame rate.
FrameSample sum;
int logFrames;
int logGpuFrames;
Uint32 nextLog;
uint64_t fpsStart;
int fpsFrames;
float fps;

float MsSince(uint64_t start)
{
	return static_cast<float>(SDL_GetPerformanceCounter() - start) * 1000.0F / static_cast<float>(SDL_GetPerformanceFrequency());
}

bool EnsureQueries()
{
	if (slots[0].disjoint != nullptr)
		return true;
	const D3D11_QUERY_DESC disjointDesc { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
	const D3D11_QUERY_DESC timestampDesc { D3D11_QUERY_TIMESTAMP, 0 };
	for (Slot &slot : slots) {
		bool ok = SUCCEEDED(device->CreateQuery(&disjointDesc, &slot.disjoint))
		    && SUCCEEDED(device->CreateQuery(&timestampDesc, &slot.begin))
		    && SUCCEEDED(device->CreateQuery(&timestampDesc, &slot.end));
		for (size_t i = 0; i < PassCount; i++) {
			ok = ok && SUCCEEDED(device->CreateQuery(&timestampDesc, &slot.passBegin[i]))
			    && SUCCEEDED(device->CreateQuery(&timestampDesc, &slot.passEnd[i]));
		}
		if (!ok) {
			ProfileRelease();
			return false;
		}
	}
	return true;
}

void Deliver(const FrameSample &sample)
{
	latest = sample;
	if (recordSink != nullptr && recordSink->size() < recordSink->capacity())
		recordSink->push_back(sample);

	sum.cpuMs += sample.cpuMs;
	for (size_t i = 0; i < static_cast<size_t>(ProfileScope::Count); i++)
		sum.cpuScopeMs[i] += sample.cpuScopeMs[i];
	logFrames++;
	if (sample.gpuMs >= 0) {
		sum.gpuMs += sample.gpuMs;
		for (size_t i = 0; i < PassCount; i++)
			sum.gpuPassMs[i] += std::max(sample.gpuPassMs[i], 0.F);
		logGpuFrames++;
	}
	if (!SDL_TICKS_PASSED(SDL_GetTicks(), nextLog))
		return;
	nextLog = SDL_GetTicks() + 5000;
	const auto frames = static_cast<float>(logFrames);
	const float gpuFrames = logGpuFrames != 0 ? static_cast<float>(logGpuFrames) : 1;
	LogVerbose("VR: perf {:.0f} fps, cpu {:.2f} ms (extract {:.2f}, render {:.2f}), gpu {:.2f} ms (shadow {:.2f}, scene {:.2f}, post {:.2f}), "
	           "{} draws, {} triangles, {} meshes, {} sprites, {} particles, {} lights, gpu memory {:.0f} MB (targets {:.0f}, textures {:.0f}, geometry {:.1f}, dynamic {:.1f})",
	    fps, sum.cpuMs / frames, sum.cpuScopeMs[0] / frames, sum.cpuScopeMs[1] / frames,
	    sum.gpuMs / gpuFrames, sum.gpuPassMs[0] / gpuFrames, sum.gpuPassMs[1] / gpuFrames, sum.gpuPassMs[2] / gpuFrames,
	    sample.drawCalls, sample.triangles, sample.meshInstances, sample.sprites, sample.particles, sample.lights,
	    static_cast<double>(ProfileVramBytes()) / 1048576.0,
	    static_cast<double>(render::memoryBytes[0]) / 1048576.0, static_cast<double>(render::memoryBytes[1]) / 1048576.0,
	    static_cast<double>(render::memoryBytes[2]) / 1048576.0, static_cast<double>(render::memoryBytes[3]) / 1048576.0);
	sum = {};
	logFrames = logGpuFrames = 0;
}

} // namespace

void ProfileBeginFrame()
{
	if (device == nullptr || !EnsureQueries())
		return;
	Slot &slot = slots[frameIndex % RingSize];
	for (bool &used : slot.passUsed)
		used = false;
	context->Begin(slot.disjoint);
	context->End(slot.begin);
	current = {};
	inFrame = true;
	frameStart = SDL_GetPerformanceCounter();
}

void ProfileEndFrame()
{
	if (!inFrame)
		return;
	inFrame = false;
	Slot &slot = slots[frameIndex % RingSize];
	context->End(slot.end);
	context->End(slot.disjoint);
	// Submit the frame as one batch. Left queued, its tail would run with the next frame's
	// commands and the two timestamps would span the wait in between.
	context->Flush();
	current.cpuMs = MsSince(frameStart);
	const render::RenderStats &stats = render::RendererStats();
	current.drawCalls = stats.drawCalls;
	current.triangles = stats.triangles;
	current.meshInstances = stats.meshInstances;
	current.sprites = stats.sprites;
	current.particles = stats.particles;
	current.lights = stats.lights;
	slot.sample = current;
	slot.pending = true;
	frameIndex++;

	fpsFrames++;
	const uint64_t now = SDL_GetPerformanceCounter();
	if (now - fpsStart >= SDL_GetPerformanceFrequency()) {
		fps = static_cast<float>(fpsFrames) * static_cast<float>(SDL_GetPerformanceFrequency()) / static_cast<float>(now - fpsStart);
		fpsStart = now;
		fpsFrames = 0;
	}

	Slot &oldest = slots[frameIndex % RingSize];
	if (!oldest.pending)
		return;
	oldest.pending = false;
	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
	const UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
	bool ready = context->GetData(oldest.disjoint, &disjoint, sizeof(disjoint), flags) == S_OK && disjoint.Disjoint == FALSE;
	const auto elapsed = [&](ID3D11Query *from, ID3D11Query *to) {
		UINT64 begin;
		UINT64 end;
		if (!ready || context->GetData(from, &begin, sizeof(begin), flags) != S_OK || context->GetData(to, &end, sizeof(end), flags) != S_OK)
			return -1.0F;
		return static_cast<float>(static_cast<double>(end - begin) * 1000.0 / static_cast<double>(disjoint.Frequency));
	};
	oldest.sample.gpuMs = elapsed(oldest.begin, oldest.end);
	for (size_t i = 0; i < PassCount; i++)
		oldest.sample.gpuPassMs[i] = oldest.passUsed[i] ? elapsed(oldest.passBegin[i], oldest.passEnd[i]) : 0;
	Deliver(oldest.sample);
}

void ProfileRelease()
{
	for (Slot &slot : slots) {
		for (ID3D11Query *query : { slot.disjoint, slot.begin, slot.end }) {
			if (query != nullptr)
				query->Release();
		}
		for (size_t i = 0; i < PassCount; i++) {
			if (slot.passBegin[i] != nullptr)
				slot.passBegin[i]->Release();
			if (slot.passEnd[i] != nullptr)
				slot.passEnd[i]->Release();
		}
		slot = {};
	}
	inFrame = false;
}

void ProfileRecord(std::vector<FrameSample> *sink)
{
	recordSink = sink;
}

const FrameSample &ProfileLatest()
{
	return latest;
}

float ProfileFps()
{
	return fps;
}

uint64_t ProfileVramBytes()
{
	DXGI_QUERY_VIDEO_MEMORY_INFO info {};
	IDXGIDevice *dxgiDevice = nullptr;
	if (device == nullptr || FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgiDevice))))
		return 0;
	IDXGIAdapter *adapter = nullptr;
	if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
		IDXGIAdapter3 *adapter3 = nullptr;
		if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void **>(&adapter3)))) {
			adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
			adapter3->Release();
		}
		adapter->Release();
	}
	dxgiDevice->Release();
	return info.CurrentUsage;
}

ScopedTimer::ScopedTimer(ProfileScope scope)
    : scope_(scope)
    , start_(SDL_GetPerformanceCounter())
{
}

ScopedTimer::~ScopedTimer()
{
	current.cpuScopeMs[static_cast<size_t>(scope_)] += MsSince(start_);
}

ScopedGpuTimer::ScopedGpuTimer(GpuPass pass)
    : pass_(pass)
{
	if (inFrame)
		context->End(slots[frameIndex % RingSize].passBegin[static_cast<size_t>(pass_)]);
}

ScopedGpuTimer::~ScopedGpuTimer()
{
	if (!inFrame)
		return;
	Slot &slot = slots[frameIndex % RingSize];
	context->End(slot.passEnd[static_cast<size_t>(pass_)]);
	slot.passUsed[static_cast<size_t>(pass_)] = true;
}

} // namespace vr
} // namespace devilution
