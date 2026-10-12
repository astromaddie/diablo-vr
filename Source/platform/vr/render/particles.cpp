/**
 * @file particles.cpp
 *
 * Sparks and embers: simulated here, drawn by the renderer as one instanced batch.
 */
#include "platform/vr/render/bridge.h"

#include <cstring>

namespace devilution {
namespace vr {
namespace render {

namespace {

struct Particle {
	Vec3 position;
	Vec3 velocity;
	float color[3];
	float life;
	float lifetime;
	float size;
};

constexpr int PoolSize = 2048;
Particle pool[PoolSize];
int next;
uint32_t randomState = 0x1234567;

/** -1..1. A fixed generator, so captures of the same frame match. */
float Random()
{
	randomState = randomState * 1664525U + 1013904223U;
	return static_cast<float>(randomState >> 8) / 8388608.0F - 1;
}

} // namespace

void ParticlesReset()
{
	std::memset(pool, 0, sizeof(pool));
	next = 0;
}

void ParticlesEmit(Vec3 position, const float color[3], int count, float speed, float life, float size)
{
	for (int i = 0; i < count; i++) {
		// The oldest particle makes room: the pool never grows.
		Particle &particle = pool[next++ % PoolSize];
		particle.position = position + Vec3 { Random(), Random(), Random() } * 0.08F;
		particle.velocity = Vec3 { Random(), Random() * 0.5F + 0.8F, Random() } * speed;
		std::memcpy(particle.color, color, sizeof(particle.color));
		particle.lifetime = particle.life = life * (0.6F + 0.4F * std::fabs(Random()));
		particle.size = size;
	}
}

void ParticlesUpdate(Scene &scene, float dt)
{
	for (Particle &particle : pool) {
		if (particle.life <= 0)
			continue;
		particle.life -= dt;
		// Hot air carries sparks up and slows them down.
		particle.velocity = particle.velocity * (1 - std::min(dt * 1.5F, 1.F)) + Vec3 { 0, 0.5F * dt, 0 };
		particle.position = particle.position + particle.velocity * dt;
		const float fade = std::clamp(particle.life / particle.lifetime, 0.F, 1.F);
		ParticleInstance instance {};
		std::memcpy(instance.position, &particle.position, sizeof(instance.position));
		instance.size = particle.size * (0.5F + 0.5F * fade);
		for (int c = 0; c < 3; c++)
			instance.color[c] = particle.color[c] * fade * fade;
		scene.particles.push_back(instance);
	}
}

} // namespace render
} // namespace vr
} // namespace devilution
