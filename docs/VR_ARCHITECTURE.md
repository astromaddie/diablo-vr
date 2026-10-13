# Steam Frame build of the VR mode

This is the current state of running the VR mode (`--vr`) natively on the Steam Frame: a Linux
ARM64 build, no PC. The VR mode itself is described in the [README](../README.md); this file only
covers what the Frame build adds or changes. Everything it adds is behind the same
`DEVILUTIONX_VR` flag, and the Windows build is meant to stay as it is.

## 1. Target

| Item | Value |
| --- | --- |
| Device | Steam Frame, SteamOS `vr`, SteamVR OpenXR 2.18.2, Adreno 750 on Turnip (Mesa 26.2.99, Vulkan 1.4) |
| Package route | Linux ARM64 ELF, Steam compat tool `SteamLinuxRuntime_4-arm64`, devkit title `DiabloVR` |
| Build | `tools/steam_frame/build.sh`, in the Docker image from `tools/steam_frame/docker/linux.Dockerfile` (Ubuntu 24.04, gcc 13; needs glibc 2.39 and GLIBCXX 3.4.32 at most) |
| Renderer route | The Direct3D 11 renderer is kept and runs on dxvk-native 3.1.1 (D3D11 to Vulkan). Section 2 |
| Shaders | Still HLSL compiled at start-up, by vkd3d 2.1's compiler behind our own `D3DCompile`. Section 2 |
| OpenXR binding | `XR_KHR_vulkan_enable` with DXVK's Vulkan device, the runtime's swapchain images wrapped as D3D11 textures. Section 3 |
| Game data | The user's own `DIABDAT.MPQ`. Never committed, never packaged |

## 2. Renderer route

The renderer is about 7,000 lines of Direct3D 11 with 17 HLSL entry points, written by an author
who is still changing it daily. Rewriting it for Vulkan or GL would fork it for good, so the Frame
build translates instead:

- **dxvk-native** provides `d3d11` and `dxgi` as shared libraries. Pinned at v3.1.1
  (`b1a1c99ab52b`), fetched and built by `build.sh dxvk`, with the patches in
  `tools/steam_frame/patches/`:
  - `dxvk-0001-native-openxr-extension-provider.patch`: DXVK creates the Vulkan instance and
    device, and the OpenXR runtime needs some extensions on both. On Windows DXVK asks
    wineopenxr for them; off Windows nothing did. The patch adds a provider that reads them from
    `DXVK_OPENXR_INSTANCE_EXTENSIONS` and `DXVK_OPENXR_DEVICE_EXTENSIONS`, which the game sets
    from `xrGetVulkanInstanceExtensionsKHR` and `xrGetVulkanDeviceExtensionsKHR`.
- **vkd3d** provides the HLSL compiler (`libvkd3d-shader`, static, built into the Docker image).
  `Source/platform/vr/dxvk/d3dcompiler_vkd3d.cpp` is `D3DCompile` on top of it, so the renderer's
  shader code is unchanged. One vkd3d patch (`docker/vkd3d-0001-samplegrad-array-gradients.patch`):
  2.1 wants a three-component gradient for `SampleGrad` on a `Texture2DArray`, where Direct3D's
  compiler takes two. With it, all 17 entry points compile from the unmodified source, with and
  without `LAYERED`.
- `Source/platform/vr/dxvk/wrl/client.h` is the small part of `Microsoft::WRL::ComPtr` the
  renderer uses.

DXVK needs a window system driver even though the VR mode never makes a DXGI swapchain. It is
told `DXVK_WSI_DRIVER=SDL2` and looks SDL2 up by soname at run time, so the game's SDL2 has to be
a shared library, with its video subsystem up before the device is created.

## 3. OpenXR graphics binding

SteamVR on the Frame offers `XR_KHR_vulkan_enable`, `vulkan_enable2`, `opengl_enable` and
`opengl_es_enable`; there is no D3D11 binding off Windows. `Source/platform/vr/xr_d3d11.h` is the
handful of OpenXR calls that depend on the binding, in DXGI and D3D11 terms. On the Frame they are
implemented by `dxvk/xr_d3d11_dxvk.cpp`, the same way Proton's wineopenxr does it for Windows games:

- The runtime's Vulkan extension lists go to DXVK (section 2) before the first DXGI object exists.
- The DXGI adapter is the one whose Vulkan physical device `xrGetVulkanGraphicsDeviceKHR` names.
- The session is created with DXVK's instance, device and queue, from `IDXGIVkInteropDevice1`.
- Swapchain formats are translated between DXGI and Vulkan numbers. Images are wrapped with
  `CreateTexture2DFromVkImage`. They are created as attachments only (no shader-resource bind), so
  DXVK leaves them in the attachment layout the runtime expects. Copying from them still works.
- DXVK's submission queue is locked around every call in which the runtime may use the Vulkan
  queue (create and destroy, acquire, release, begin and end frame), and DXVK is flushed before
  release and end frame.

## 4. Probe facts (2026-10-13, unattended, launched over SSH)

`tools/steam_frame/probe/xr_probe.cpp` runs the binding and the shader compiler with no game
around them. On the Frame it:

- created the session and reached SYNCHRONIZED;
- was offered DXGI formats 29 (`R8G8B8A8_UNORM_SRGB`), 91, 10, 24, 40, 55 and 20
  (`D32_FLOAT_S8X24_UINT`), and a depth layer;
- got 1728x1728 recommended eyes (8192 max), three images per swapchain, 90 Hz offered;
- left eye fov -1.028, 0.885, 0.864, -1.048 rad (l, r, u, d), right eye -0.877, 1.033, 0.866, -1.042;
- created one of each texture kind the renderer uses at its real size (4x MSAA `R11G11B10_FLOAT`
  and depth arrays, the 64x128x2048 surface array with mips, the 8192x8192 RG8 sprite atlas);
- reported MSAA 2x, 4x and 8x, and render-target array index from the vertex shader (the
  renderer's single-pass stereo);
- drew a triangle with shaders compiled on the device, and read both eye images back from the
  runtime's swapchain with exactly the expected pixels;
- ran 850 frames in 12 s with every `xrEndFrame` accepted.

Nobody was wearing the headset, so this proves the route, not the picture in the lenses.

## 5. What the game does with it

Nothing about the VR mode's design changes on the Frame. For reference when working on it:

- **Frame loop.** The game keeps its own 20 Hz logic tick on the wall clock and interpolates
  between ticks itself. `VrPresent` is called from the game's present, where `xrWaitFrame` then
  paces the loop. Views, hands and the screen placement use `predictedDisplayTime`; scene time,
  particles and input steps use `SDL_GetTicks`.
- **Layers.** One projection layer (two eye swapchains, plus two depth swapchains with
  `XR_KHR_composition_layer_depth`), then the game's 2D frame as a quad, then sometimes a second
  quad. The 2D frame is one more swapchain, filled from the CPU every frame.
- **Renderer.** Both eyes in one pass (render-target array index from the vertex shader), into
  app-owned 4x MSAA `R11G11B10_FLOAT` targets, resolved by a shader, bloom, then a final pass
  into the runtime's images. The hidden-area mesh is used when the runtime gives one.
- **Without a headset.** `DEVILUTIONX_VR_OFFSCREEN=<dir>` draws levels to pictures and
  `--vr-bench <level>` times what a headset frame draws, both with a plain D3D11 device and no
  OpenXR. `build.sh render` runs the first on lavapipe on the build machine, and
  `unattended.sh bench` the second on the Frame.

## 6. Controls on the Frame

The Frame's controllers are a gamepad split over two hands, and the VR mode already feeds the
game through a virtual gamepad, so the Frame profile
(`/interaction_profiles/valve/frame_controller_valve`) is the Touch map with these differences:

| Input | What it does |
| --- | --- |
| A, B, X, Y (all on the right hand) | What the same letters do on Touch: primary, secondary, character, inventory |
| Menu (right) | Game menu |
| View (left) | The gamepad's Select. Held for a second: re-centre, with a buzz in both hands |
| Left / right bumper | Health / mana potion from the belt, as on a gamepad. The right stick's up and down no longer drink potions on the Frame |
| D-pad | The gamepad's D-pad wherever the game shows its own interface (menus, inventory, shops). Does nothing out in the world |

Sticks, triggers, grips and motion are unchanged. This map has not been worn or approved yet.
`tools/steam_frame/tests/check_bindings.py` checks the binding tables against each profile's
component list.

## 7. Settings and diagnostics

The VR mode is tuned with `DEVILUTIONX_VR_*` environment variables. On the Frame the launcher
reads them from `vr-settings.env` in the data folder (`~/.local/share/DiabloVR`), which
`deploy.sh` installs once with the Frame's defaults (snap turn 30 degrees). `launch-once.env` and
`launch-once.args` there apply to one launch and are deleted before the game starts. Added for
the Frame: `DEVILUTIONX_VR_SNAP_TURN` (degrees) and `DEVILUTIONX_VR_EYE_SIZE` (eye image width in
pixels, 0 for what the runtime recommends). There is no in-headset settings page.

`tools/steam_frame/unattended.sh` runs the benchmark or a short session over SSH. It refuses
when another Steam title is running or the headset is not in standby.

## 8. Measurements on the Frame (2026-10-14)

Dungeon level 2, both eyes at 1728x1728, 4x MSAA, `--vr-bench`, headset in standby:

| Run | Frames in 20 s | GPU p50 | Scene pass | Post |
| --- | --- | --- | --- | --- |
| First | 1000 | 14.6 ms | 12.1 ms | 2.6 ms |
| Again, a few minutes later | 728 | 20.9 ms | 16.7 ms | 3.1 ms |

CPU time per frame was 0.2 ms, so the game is bound by the GPU, and the same settings gave
different numbers minutes apart (the headset was warm, 66 to 74 C on its CPU sensors, with the
GPU at its top frequency). Runs at 2x MSAA and at 2160x2160 came out slower still, in an order
that makes no sense for the settings, so none of this ranks anything yet. It needs repeats with
cool-down.

In a real session in Tristram, worn, the game ran at 42 to 43 fps on a 72 Hz display: GPU
19 ms a frame (scene 15, post 3.2), 768,000 triangles in 250 draws. That is below Valve's floor
of 72 fps and the next thing to fix.

## 9. Status

| Item | State |
| --- | --- |
| Toolchain image, dxvk-native and vkd3d builds | done |
| OpenXR session and swapchains through DXVK | verified unattended on the Frame (probe), and in the game |
| HLSL through vkd3d | all 17 entry points compile; the town drawn on lavapipe matches the author's screenshots |
| VR mode built for Linux ARM64 | done; package passes the ABI and allowlist gates |
| Flat game from the package | boots to the main menu on the build machine; not started from Steam on the Frame |
| VR mode on the Frame | ran for 75 s from SSH with the headset on someone's head by accident: tracked, stereo in one pass, depth layer and hidden-area mesh in use, Frame controller profile bound. Nobody has played it |
| Frame rate | 42 fps at 72 Hz in town. Not acceptable yet |
| Started from the Steam library, controls, saves | not tried |
| Standard system layer beyond View re-centre, focus-loss pause, settings page | not started |
| Windows build after the binding change | not compiled |
