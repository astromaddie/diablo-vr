/**
 * @file xr_frame.cpp
 *
 * One headset frame of the 3D world: poses from OpenXR in, both eyes' images out.
 */
#include "platform/vr/vr_internal.h"

#include <cmath>
#include <string>

#include "platform/vr/render/world.h"
#include "utils/log.hpp"

namespace devilution {
namespace vr {

/** Chained to the projection views, so it has to outlive xrEndFrame. */
XrCompositionLayerDepthInfoKHR depthInfos[2];

/** Renders the 3D world for both eyes and fills in the projection layer. Returns false if nothing was rendered. */
bool RenderWorld(XrTime time, XrCompositionLayerProjectionView projectionViews[2])
{
	XrViewLocateInfo locateInfo { XR_TYPE_VIEW_LOCATE_INFO };
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = time;
	locateInfo.space = localSpace;
	XrViewState viewState { XR_TYPE_VIEW_STATE };
	XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	uint32_t viewCount = 0;
	XrPosef head;
	if (XR_FAILED(xrLocateViews(session, &locateInfo, &viewState, 2, &viewCount, views)) || viewCount != 2
	    || (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0 || !Locate(viewSpace, time, head))
		return false;

	float headPosition[3];
	float headOrientation[4];
	ToWorld(head, headPosition, headOrientation);
	VrHandPose hands[2];
	for (const Hand hand : { Left, Right }) {
		XrPosef pose;
		hands[hand].valid = Locate(aimSpace[hand], time, pose);
		if (hands[hand].valid)
			ToWorld(pose, hands[hand].position, hands[hand].orientation);
	}
	VrWorldUpdate(headPosition, headOrientation, hands[Left], hands[Right], worldPointerValid, worldPointerHit);

	// Both eyes are drawn in one pass, so both images are acquired before anything is drawn.
	VrView eyeViews[2];
	VrViewTarget targets[2] {};
	uint32_t imageIndex[2] = {};
	XrSwapchainImageAcquireInfo acquireInfo { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XrSwapchainImageWaitInfo waitInfo { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	XrSwapchainImageReleaseInfo releaseInfo { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	int acquired = 0;
	for (; acquired < 2; acquired++) {
		EyeTarget &eye = eyes[acquired];
		if (XR_FAILED(xrAcquireSwapchainImage(eye.swapchain, &acquireInfo, &imageIndex[acquired])))
			break;
		xrWaitSwapchainImage(eye.swapchain, &waitInfo);
		targets[acquired].color = eye.views[imageIndex[acquired]];
		if (eye.depthSwapchain != XR_NULL_HANDLE) {
			uint32_t depthIndex = 0;
			if (XR_FAILED(xrAcquireSwapchainImage(eye.depthSwapchain, &acquireInfo, &depthIndex))) {
				xrReleaseSwapchainImage(eye.swapchain, &releaseInfo);
				break;
			}
			xrWaitSwapchainImage(eye.depthSwapchain, &waitInfo);
			targets[acquired].depth = eye.depthViews[depthIndex];
		}
		targets[acquired].maskVertices = eye.maskVertices;
		targets[acquired].maskVertexCount = eye.maskVertexCount;
		const XrFovf &fov = views[acquired].fov;
		ToWorld(views[acquired].pose, eyeViews[acquired].position, eyeViews[acquired].orientation);
		eyeViews[acquired].fov[0] = std::tan(fov.angleLeft);
		eyeViews[acquired].fov[1] = std::tan(fov.angleRight);
		eyeViews[acquired].fov[2] = std::tan(fov.angleUp);
		eyeViews[acquired].fov[3] = std::tan(fov.angleDown);
	}
	const bool complete = acquired == 2;
	if (complete) {
		VrWorldRender(eyeViews, 2, eyes[0].width, eyes[0].height, targets);
		RenderMirror();
		if (screenshotRequested) {
			screenshotRequested = false;
			SaveScreenshot(eyes[0].images[imageIndex[0]].texture);
		}

		static const std::string dumpDirectory = SDL_getenv("DEVILUTIONX_VR_DUMP") != nullptr ? SDL_getenv("DEVILUTIONX_VR_DUMP") : "";
		static int frameCounter;
		if (!dumpDirectory.empty() && ++frameCounter % 150 == 0)
			DumpTexture(eyes[0].images[imageIndex[0]].texture, (dumpDirectory + "\\eye.bmp").c_str());
	}

	for (int i = 0; i < acquired; i++) {
		EyeTarget &eye = eyes[i];
		xrReleaseSwapchainImage(eye.swapchain, &releaseInfo);
		projectionViews[i] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
		projectionViews[i].pose = views[i].pose;
		projectionViews[i].fov = views[i].fov;
		projectionViews[i].subImage.swapchain = eye.swapchain;
		projectionViews[i].subImage.imageRect.extent = { eye.width, eye.height };
		if (eye.depthSwapchain != XR_NULL_HANDLE) {
			xrReleaseSwapchainImage(eye.depthSwapchain, &releaseInfo);
			// The compositor reprojects with real depth instead of assuming everything is far away.
			// Depth is reversed (1 at the near plane), which OpenXR expresses by swapping the two distances.
			depthInfos[i] = { XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR };
			depthInfos[i].subImage.swapchain = eye.depthSwapchain;
			depthInfos[i].subImage.imageRect.extent = { eye.width, eye.height };
			depthInfos[i].minDepth = 0;
			depthInfos[i].maxDepth = 1;
			depthInfos[i].nearZ = VrFarPlane;
			depthInfos[i].farZ = VrNearPlane;
			projectionViews[i].next = &depthInfos[i];
		}
	}
	return complete;
}

} // namespace vr
} // namespace devilution
