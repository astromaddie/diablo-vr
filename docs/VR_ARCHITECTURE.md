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
`deploy.sh` installs once with the Frame's defaults. `launch-once.env` and `launch-once.args`
there apply to one launch and are deleted before the game starts. What the launcher's own
environment sets wins over the one-shot file, and that over the settings file. There is no
in-headset settings page.

Settings added with the Frame build (defaults are upstream's behaviour unless noted):

| Setting | What it does | On the Frame |
| --- | --- | --- |
| `DEVILUTIONX_VR_MSAA=1` | No MSAA: the scene is drawn straight into the post-processing source | 1 |
| `DEVILUTIONX_VR_WALL_SHADOW_STEPS`, `_SPACING` | Samples a pixel takes towards a light: at most this many (16), one per this many tiles (0: always all) | spacing 0.4 |
| `DEVILUTIONX_VR_SUN_SHADOW_TAPS` | 9, 4 or 1 filtered comparisons against the sun's shadow map | 4 |
| `DEVILUTIONX_VR_NEAREST_FIRST` | Solid things drawn nearest first, carved models before the level (default 1) | 1 |
| `DEVILUTIONX_VR_DEPTH_PREPASS` | Levels without materials drawn depth first, then shaded where seen. Without MSAA only | 1 |
| `DEVILUTIONX_VR_BLOOM=0` | No bloom passes (0.4 ms) | on |
| `DEVILUTIONX_VR_DEPTH_LAYER=0` | No depth handed to the compositor (0.1 ms) | on |
| `DEVILUTIONX_VR_ANISOTROPY` | Of the surface sampler (8). 2 measured no faster | 8 |
| `DEVILUTIONX_VR_DESKTOP_WINDOW=0` | The desktop window gets a frame a second during a session. Saves 0.6 ms of CPU a frame | 0 |
| `DEVILUTIONX_VR_SNAP_TURN` | Degrees of a snap turn (45) | 30 |
| `DEVILUTIONX_VR_EYE_SIZE` | Width of the eye images in pixels (0: what the runtime recommends, 1728) | 0 |
| `DEVILUTIONX_VR_FOVEATION` | `lo`, `med` or `hi`: launcher only, see below | off |

Levels with no materials, floor map or grit (the default look) are always drawn by a variant of
the surface shader without those lookups; that one has no switch because the picture is the same.

**Foveation.** The Frame ships a Vulkan layer, `VK_LAYER_VALVE_fdm_injection`, that attaches a
fragment density map to render passes the size of an OpenXR swapchain, following the eyes. With
`DEVILUTIONX_VR_FOVEATION` set the launcher enables it (`VK_INSTANCE_LAYERS`,
`FDM_DEBUG=enable,<level>`), has the renderer draw one eye per pass (the layer's map is not
layered) and leaves Turnip to tiled rendering, which the map needs. It works through DXVK, from
SSH and from a Steam launch. At `hi` the edge of the eye images is visibly coarser in a dump.
Nobody has looked at it in the headset, so it is off.

For checks with nobody wearing the headset: `DEVILUTIONX_VR_FORCE_RENDER=1` draws the world from a
made-up head standing in the room (`DEVILUTIONX_VR_DIAG_YAW` turns it), `DEVILUTIONX_VR_AUTOSTART`
takes the game from the menus into town, `DEVILUTIONX_VR_DEBUG_LEVEL` on into a dungeon level,
`DEVILUTIONX_VR_DUMP` saves both eyes every 150 frames (each save stalls the game some 45 ms), and
`DEVILUTIONX_VR_DEBUG_PACING=1` logs the frames around each missed refresh. With `--verbose` the
log has a `VR: pacing` line every ten seconds: frames a second against the display's rate,
refreshes missed, how long the game was away between frames and how long the runtime held it.

`tools/steam_frame/unattended.sh` runs the benchmark, a session over SSH or the Steam title
itself. It refuses when another Steam title is running or the headset is not in standby.

## 8. Measurements on the Frame (2026-10-14)

The game is bound by the GPU: the VR mode's CPU work is about 0.2 ms a frame.

**Benchmark** (`unattended.sh bench`: `--vr-bench`, both eyes at 1728x1728 including the part the
lenses hide, a fixed 20 second path, no headset session), GPU time per frame with the Frame's
settings:

| Level | Median | 95th percentile |
| --- | --- | --- |
| Town | 8.4 ms | 9.1 ms |
| 2, cathedral | 6.6 ms | 8.1 ms |
| 6, catacombs | 6.4 ms | 7.1 ms |
| 10, caves | 9.1 ms | 10.0 ms |
| 14, hell | 7.1 ms | 8.7 ms |

As upstream draws it (4x MSAA and the rest) the same benchmark gave 17.1 ms on level 2 and
19.5 ms in town, at 1832x1920. The path has few monsters and lights; fights will cost more.

**Where it went** (level 2 at 1832x1920, median GPU time, each step on top of the one before):

| Change | GPU |
| --- | --- |
| Upstream: 4x MSAA | 17.1 ms |
| MSAA off. With MSAA another 4 ms or so falls outside the GPU timers, so 4x cost almost half the frame rate | 12.8 ms |
| Surfaces without the material lookups the default look never uses | 8.9 ms |
| Wall shadows a sample per 0.4 tiles | 8.5 ms |
| Turnip drawing straight into memory (`TU_DEBUG=sysmem`, the launcher's default) | 7.6 ms |

In town, nearest-first drawing saved 1.3 ms and four sun shadow taps 0.8 ms. The depth pre-pass
is about even on average and takes 1.3 to 1.7 ms off the 95th percentile in the caves, hell and
town. What is left on level 2 splits roughly into 1.7 ms that any shader would cost, 1.0 ms for
carved models and sprites, 1.4 ms lighting, 1.1 ms the relief on surfaces, 1.2 ms everything
else in the surface shader, 0.8 ms the final pass and 0.4 ms bloom.

Tried and dropped: branching round texture lookups a pixel doesn't need and a one-texel path for
carved models (0.4 ms slower in town), drawing surfaces without cut-outs for the early depth
test, HLSL minimum-precision types (vkd3d reads `min16float` as `float`).

**In a session** (headset in standby, forced rendering from a made-up head, one view each):

| | Display | Frames a second | Missed | GPU |
| --- | --- | --- | --- | --- |
| Steam title, town | 72 Hz | 72.0 | 0 in 10 s | 7.3 ms |
| Steam title, level 2 | 72 Hz | 72.0 | 0 | 7.7 ms |
| Level 2 | 90 Hz | 90.0 | 0 | 6.8 ms |
| Town | 90 Hz | 90.0 | 0 | 7.5 ms |
| Level 2, no foveation | 120 Hz | 106 | many | 9.3 ms |
| Level 2, foveation `hi` | 120 Hz | 120.0 | 0 | 5.4 ms |
| Town, foveation `hi` | 120 Hz | 118 | 2 a second | 8.4 ms |
| Level 2, 2160x2160, foveation `hi` | 90 Hz | 90.0 | 0 | 7.1 ms |
| Level 2, 2160x2160, no foveation | 90 Hz | 45 (half rate) | | 12.8 ms |

The title's SteamVR refresh rate is set to 72 Hz (`steam.app.3259726774.preferredRefreshRate`),
which leaves room for fights. 90 Hz held in both places tried; it has not been tried in a fight
or worn. Start-up takes 2.5 seconds to a running session.

## 9. Status

| Item | State |
| --- | --- |
| Toolchain image, dxvk-native and vkd3d builds | done |
| OpenXR session and swapchains through DXVK | verified unattended on the Frame (probe), and in the game |
| HLSL through vkd3d | all entry points compile; town, cathedral and caves drawn on lavapipe match the author's screenshots |
| VR mode built for Linux ARM64 | done; package passes the ABI and allowlist gates |
| Stereo in the headset | verified unattended: both eyes dumped from a session in town, the cathedral (two headings) and the caves; parallax, hidden-area mesh and heading are right |
| Started from the Steam library | verified unattended: runs in Steam Linux Runtime 4 at the title's 72 Hz, 72.0 fps, exits on SIGTERM. Not started by a person, so controllers are untried |
| Frame rate | Worn once at upstream's settings (42 fps in town, juddery). With the Frame's settings 72.0 fps with no missed refreshes, unattended. Not worn since |
| Even stepping of walking and turning (display time instead of the millisecond clock) | built; needs a wearer to say whether it shows |
| Foveation | measured (table above), off; needs a wearer |
| Frame controller profile | bound by SteamVR in the worn session; no button has been pressed |
| Standard system layer beyond View re-centre, focus-loss pause, settings page | not started |
| Windows build after the binding change | not compiled |
