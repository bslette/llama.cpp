# Steam + Proton on the Xavier NX (NVK) — feasibility, 2026-10-29

Status: **not started**. This page records what was checked and what the plan is.
Probably doable. Our NVK build already has nearly everything DXVK/vkd3d-proton
need. The risky part is getting any desktop onto the HDMI output.

## What was checked on nx-linux (slot b)

- Ubuntu 24.04.5, kernel 6.18.21-nx+, aarch64, **4 KiB pages** (FEX requires 4K).
- CPU: Carmel, 6 cores. Features include `atomics` and `asimdrdm`, but there is
  **no `lrcpc`**, so FEX's TSO emulation takes its slower path.
- RAM: about 7 GB. NVMe: 73 GB free.
- DRM:
  - `card0` is `tegra-drm` (display). HDMI-A-1 is connected; DP-1 is disconnected.
  - `card1` / `renderD129` is nouveau (render).
- Software: `graphical.target` is the default, but no X server, Wayland
  compositor, display manager or snapd is installed, and no session is running.
- The user `slettela` is **not** in the `video` or `render` groups. Only root can
  open the GPU; the board tests run as root via `/root/nvkenv.sh`.
- The distro's Mesa 25.2.8 doesn't support gv11b, so vulkaninfo as a user shows
  only llvmpipe.

### NVK (`/root/mesa-nvk`, e943159, Mesa 26.2.3) as root

`vulkaninfo` reports NVK GV11B with Vulkan 1.3.354.

Present:
- VK_EXT_robustness2, VK_EXT_transform_feedback, VK_EXT_extended_dynamic_state3
- VK_EXT_graphics_pipeline_library, VK_KHR_maintenance5, VK_KHR_maintenance6
- VK_EXT_descriptor_buffer, VK_EXT_mutable_descriptor_type
- VK_KHR_present_wait, VK_EXT_swapchain_maintenance1, VK_KHR_display
- VK_EXT_depth_clip_enable, VK_EXT_custom_border_color, VK_EXT_shader_object
- VK_EXT_image_drm_format_modifier, VK_EXT_external_memory_dma_buf
- Features: geometryShader, tessellationShader, shaderInt64, sparseBinding,
  sparseResidencyBuffer, nullDescriptor, multiViewport, depthBounds,
  fillModeNonSolid, textureCompressionBC

Missing:
- **VK_KHR_xcb_surface and VK_KHR_wayland_surface.** The board build has no
  X11/Wayland WSI and needs a rebuild with `-Dplatforms=x11,wayland`.

The driver also needs `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1`, because Volta isn't
conformant. That variable has to be set for the whole session.

## Steam on ARM64 (state as of Oct 2026)

Valve doesn't officially support Steam on ARM Linux. There are two working routes:

1. **Valve's native ARM64 client (public beta) + Proton 11 ARM64 build** (preferred)
   - Proton 11 ships FEX in an Arm64EC configuration. Only the game's x86 code
     is emulated; Wine's ARM64 parts, DXVK and Vulkan run natively, so the game
     uses our host aarch64 NVK directly.
   - Armbian's configng (PR #1066) installs this client, and ROCKNIX ships it.
2. **Canonical's Steam snap** (stable since 2026-06-04) (fallback)
   - Runs the x86 Steam client under FEX. Its stable release can forward
     Vulkan/GL calls to the host libraries ("thunking").
   - The snap brings its own Mesa (core24 = Ubuntu 24.04 ABI, so our build
     should be ABI-compatible). Pointing it at our NVK would take work.

## Blockers

1. **No desktop session** on the board at all.
2. **Split display/render.** HDMI scanout is on `tegra-drm` (card0) and rendering
   is on nouveau (card1). The compositor has to render with NVK and import the
   result into tegra-drm. Both sides support NVIDIA block-linear format
   modifiers, so this should work, but it's the most likely thing to need
   debugging.
3. **NVK has no X11/Wayland WSI** in the board build (needs a rebuild).
4. **User permissions:** add `slettela` to `video` and `render`.

## Expectations

- About 7 GB of RAM is shared between Steam's CEF web helper (often ~1 GB), FEX
  and the game.
- Carmel has no LRCPC, so FEX runs slower.
- The GPU is a 384-core Volta.
- Realistic targets are older or light DX9–DX11 titles at low resolution, not
  recent AAA games.

## Plan (not started; waiting for go-ahead)

1. Rebuild `/root/mesa-nvk` with `-Dplatforms=x11,wayland` and install it to a
   prefix the user can read.
2. Add `slettela` to `video` and `render`.
3. Install a light Wayland compositor and get a desktop on HDMI with NVK
   rendering. Go/no-go check: `vkcube` running in a window.
4. Install Valve's native ARM64 Steam client, enable Proton 11 (ARM64/FEX), and
   try a light DX11 title. Fall back to the snap with thunking if the native
   client won't launch games.

Steps 2 and 3 install packages and change the user account on nx-linux.

## Sources

- https://itsfoss.com/news/arm64-steam-snap-goes-stable/
- https://discourse.ubuntu.com/t/call-for-testing-steam-snap-for-arm64/74719
- https://github.com/armbian/configng/pull/1066
- https://www.gamingonlinux.com/2026/04/proton-11-beta-arrives-to-bring-enhanced-gaming-compatibility-to-linux-steamos/
- https://www.tomshardware.com/video-games/handheld-gaming/steam-shown-running-on-nintendo-switch-thanks-to-latest-proton-beta-fex-2604-translates-x86-to-arm-friendly-instructions-on-linux
- https://www.techpowerup.com/353418/valve-confirms-steam-for-linux-isnt-officially-supported-on-arm-chips
- https://www.xda-developers.com/steam-will-finally-run-better-on-arm-devices-and-you-can-test-the-new-version-right-now/
- https://interfacinglinux.com/community/sbcsoftware/native-steam-client-for-arm-linux/
