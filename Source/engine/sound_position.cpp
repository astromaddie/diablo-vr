#include "engine/sound_position.hpp"

#include "engine/sound_defs.hpp"
#include "player.h"
#ifdef DEVILUTIONX_VR
#include "platform/vr/render/world.h"
#include "platform/vr/vr.h"
#endif

namespace devilution {

bool CalculateSoundPosition(Point soundPosition, int *plVolume, int *plPan)
{
#ifdef DEVILUTIONX_VR
	// In the headset the head is the listener: panned by where the sound lies from it, muffled behind walls.
	if (VrShowsWorld())
		return VrWorldSoundPosition(soundPosition.x, soundPosition.y, plVolume, plPan);
#endif
	const Point playerPosition { MyPlayer->position.tile };
	const Displacement delta = soundPosition - playerPosition;

	const int pan = (delta.deltaX - delta.deltaY) * 256;
	*plPan = std::clamp(pan, PAN_MIN, PAN_MAX);

	const int volume = playerPosition.ApproxDistance(soundPosition) * -64;

	if (volume <= ATTENUATION_MIN)
		return false;

	*plVolume = volume;

	return true;
}

} // namespace devilution
