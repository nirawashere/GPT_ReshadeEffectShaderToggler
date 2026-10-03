# FHX Restoration ReShade UI Mask

Current version: **v4.5**

A lightweight ReShade screen-space UI restore mask for **FHX Restoration**.

This shader is designed for players who want ReShade effects such as depth blur, MXAO, bloom, color grading, or other post-processing to affect the game world while leaving the fixed FHX HUD readable and visually unchanged.

It does **not** inspect or modify FHX game files, shaders, Vulkan command lists, or game memory. It works by capturing the untouched frame before your effects run and restoring selected HUD regions afterward.

## Installation

1. Install ReShade for FHX Restoration.
2. Copy `Shaders/FHX_UIMaskv4.5.fx` into your ReShade shader folder.
3. Reload ReShade.
4. Enable both:
   - `FHX_UIMask_Capture`
   - `FHX_UIMask_Restore`
5. In ReShade's technique order, place them like this:

```text
FHX_UIMask_Capture       <- VERY TOP

your normal ReShade effects
MXAO
DepthHaze
DOF
Bloom
color grading
etc.

FHX_UIMask_Restore       <- VERY BOTTOM
```

The ordering is required. Capture stores the clean frame before the effects run; Restore composites the clean HUD pixels back afterward.

## Resolution support

The shader includes scaling profiles for:

- 1280x720
- 1920x1080
- 2560x1440
- Auto / current render buffer

The HUD geometry was measured against 1920x1080 FHX screenshots and scales from that reference.

Both **Windowed** and **Fullscreen** profiles are exposed in the shader settings.

## Protected HUD regions

Persistent regions are enabled by default:

- Buff bar
- Party / Create window
- Target bar
- Minimap, including compass tabs and lower controls
- Skill / Combo / Crafting window
- Shop button
- Bottom HP / MP / SP / mercenary / hotbar UI
- Three bottom chat areas
- Bottom-left button strip

Optional UI regions default to OFF and can be enabled when needed:

- Quest status bar
- Quest tracker
- Quest detail window
- Skill / action progress bar

Inventory is intentionally not masked because it is movable.

## Mask setup / debugging

Under **00 Global**, use:

- `Mask preview -> Magenta overlay` to see protected regions over the game
- `Mask preview -> White mask` for a clean black/white mask view
- `Restore strength = 1.0` for complete HUD restoration
- `Feather inward` and `Feather outward` to soften mask boundaries

A good starting point is:

```text
Restore strength: 1.00
Feather inward:   1.00 px
Feather outward:  0.00 px
```

## How it works

The shader uses two ReShade techniques:

1. **Capture** copies the untouched back buffer into a texture.
2. Your normal ReShade effects process the screen.
3. **Restore** blends the original pixels back only where the FHX UI mask is active.

Conceptually:

```text
UI pixel     -> original untouched frame
world pixel  -> processed ReShade frame
```

This avoids the need for game-specific shader interception.

## Notes

- The fixed mask layout is intended for FHX Restoration's standard HUD placement.
- If FHX changes its HUD geometry in a future update, the mask coordinates may need updating.
- Quest and action-progress masks are optional because those windows are not always visible.
- This project is not affiliated with the FHX Restoration project or the ReShade project.

## License

This shader is distributed under the MIT license used by this repository.
