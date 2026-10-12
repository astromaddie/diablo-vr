/**
 * @file profile.h
 *
 * Frame timing for the VR renderer: CPU scope timers, per-pass GPU timers (D3D11
 * timestamp queries), draw counters and memory. Knows nothing about OpenXR.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace devilution {
namespace vr {

enum class ProfileScope : uint8_t {
	/** Reading game state into the scene (level mesh updates, actors, particles). */
	Extract,
	/** Culling, uploads and issuing the draw calls. */
	Render,
	Count,
};

enum class GpuPass : uint8_t {
	Shadow,
	Scene,
	Post,
	Count,
};

struct FrameSample {
	/** All VR work of the frame on the calling thread, runtime waits excluded. */
	float cpuMs;
	float cpuScopeMs[static_cast<size_t>(ProfileScope::Count)];
	/** GPU time between the frame's first and last command; negative when the query was lost. */
	float gpuMs;
	float gpuPassMs[static_cast<size_t>(GpuPass::Count)];
	uint32_t drawCalls;
	uint32_t triangles;
	uint32_t meshInstances;
	uint32_t sprites;
	uint32_t particles;
	uint32_t lights;
};

/** Brackets the frame's rendering. GPU times arrive a few frames late and never block. */
void ProfileBeginFrame();
void ProfileEndFrame();
/** Drops the GPU queries; they belong to the device that is going away. */
void ProfileRelease();

/** Completed samples are appended to `sink` until it is reset to null. The caller reserves. */
void ProfileRecord(std::vector<FrameSample> *sink);

/** The newest completed sample (GPU times included), and frames per second over the last second. */
const FrameSample &ProfileLatest();
float ProfileFps();

/** Video memory this process has in use on the renderer's adapter, as the OS reports it. */
uint64_t ProfileVramBytes();

class ScopedTimer {
public:
	explicit ScopedTimer(ProfileScope scope);
	~ScopedTimer();

private:
	ProfileScope scope_;
	uint64_t start_;
};

/** Times the GPU work issued while it is alive. Passes do not nest. */
class ScopedGpuTimer {
public:
	explicit ScopedGpuTimer(GpuPass pass);
	~ScopedGpuTimer();

private:
	GpuPass pass_;
};

} // namespace vr
} // namespace devilution
