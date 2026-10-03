/*
    FHX_UIMaskv4.5.fx
    FHX Restoration fixed-HUD restore mask.

    Purpose:
      Save the untouched frame before ReShade effects run, then restore only
      known FHX UI regions afterward.

    Supported render resolutions:
      1280x720
      1920x1080
      2560x1440
      Auto/current buffer

    Display profiles:
      Windowed
      Fullscreen

    Technique order:
      FHX_UIMask_Capture     <- VERY TOP
      effects to keep off UI
      FHX_UIMask_Restore     <- VERY BOTTOM

    v4.5 cleanup:
      - Removed all per-mask Move/Grow controls.
      - Removed all custom/manual rectangle controls.
      - Kept one Protect toggle for every supported mask.
      - Split the left quest UI into three independently switchable masks:
        quest status, quest tracker, quest detail.
      - Added the fixed skill/action progress bar.
      - Reworked Party/Create coverage and the Buff/Party seam.
      - Rebuilt the minimap as a compound mask instead of one oversized ellipse.
      - Inventory remains intentionally excluded because it is movable.

    Reference geometry is measured from supplied 1920x1080 screenshots and then
    scaled to the selected FHX resolution.
*/

#include "ReShade.fxh"

// ============================================================================
// 00 GLOBAL
// ============================================================================

uniform int FHX_ResolutionPreset <
    ui_category = "00 Global";
    ui_label = "FHX resolution";
    ui_type = "combo";
    ui_items = "Auto / current buffer\0"
               "1280x720\0"
               "1920x1080\0"
               "2560x1440\0";
> = 0;

uniform int FHX_DisplayMode <
    ui_category = "00 Global";
    ui_label = "Display profile";
    ui_type = "combo";
    ui_items = "Windowed\0Fullscreen\0";
    ui_tooltip = "FHX fixed HUD geometry currently scales identically in both profiles. The selector is retained so separate profiles can be introduced without changing presets.";
> = 0;

uniform int FHX_DebugMode <
    ui_category = "00 Global";
    ui_label = "Mask preview";
    ui_type = "combo";
    ui_items = "Off\0White mask\0Magenta overlay\0";
> = 0;

uniform float FHX_RestoreStrength <
    ui_category = "00 Global";
    ui_label = "Restore strength";
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 0.01;
> = 1.0;

uniform float FHX_FeatherInPixels <
    ui_category = "00 Global";
    ui_label = "Feather inward (pixels)";
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 24.0;
    ui_step = 0.25;
    ui_tooltip = "Transition distance moving inward from a UI boundary.";
> = 1.0;

uniform float FHX_FeatherOutPixels <
    ui_category = "00 Global";
    ui_label = "Feather outward (pixels)";
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 24.0;
    ui_step = 0.25;
    ui_tooltip = "Transition distance extending outside a measured UI boundary.";
> = 0.0;


// ============================================================================
// 01 CORE HUD
// ============================================================================

uniform bool FHX_Protect_Buffs <
    ui_category = "01 Core HUD";
    ui_label = "Protect Buff Bar";
> = true;

uniform bool FHX_Protect_Party <
    ui_category = "01 Core HUD";
    ui_label = "Protect Party / Create";
> = true;

uniform bool FHX_Protect_TargetBar <
    ui_category = "01 Core HUD";
    ui_label = "Protect Target Bar";
> = true;

uniform bool FHX_Protect_Minimap <
    ui_category = "01 Core HUD";
    ui_label = "Protect Minimap";
> = true;

uniform bool FHX_Protect_SkillWindow <
    ui_category = "01 Core HUD";
    ui_label = "Protect Skill / Combo / Crafting";
> = true;

uniform bool FHX_Protect_Shop <
    ui_category = "01 Core HUD";
    ui_label = "Protect Shop Button";
> = true;

uniform bool FHX_Protect_BottomHUD <
    ui_category = "01 Core HUD";
    ui_label = "Protect Bottom HUD / Hotbar";
> = true;


// ============================================================================
// 02 CHAT
// ============================================================================

uniform bool FHX_Protect_LeftButtons <
    ui_category = "02 Chat";
    ui_label = "Protect Left Button Strip";
> = true;

uniform bool FHX_Protect_ChatLeft <
    ui_category = "02 Chat";
    ui_label = "Protect Chat Left";
> = true;

uniform bool FHX_Protect_ChatMiddle <
    ui_category = "02 Chat";
    ui_label = "Protect Chat Middle";
> = true;

uniform bool FHX_Protect_ChatRight <
    ui_category = "02 Chat";
    ui_label = "Protect Chat Right";
> = true;


// ============================================================================
// 03 QUEST UI
//
// These are OFF by default because the quest windows are not always visible.
// ============================================================================

uniform bool FHX_Protect_QuestStatus <
    ui_category = "03 Quest UI";
    ui_label = "Protect Quest Status Bar";
> = false;

uniform bool FHX_Protect_QuestTracker <
    ui_category = "03 Quest UI";
    ui_label = "Protect Quest Tracker";
> = false;

uniform bool FHX_Protect_QuestDetail <
    ui_category = "03 Quest UI";
    ui_label = "Protect Quest Detail";
> = false;


// ============================================================================
// 04 ACTION UI
//
// This bar only appears while an action is in progress, so it defaults OFF.
// ============================================================================

uniform bool FHX_Protect_ActionBar <
    ui_category = "04 Action UI";
    ui_label = "Protect Skill / Action Progress Bar";
> = false;


// ============================================================================
// CLEAN FRAME STORAGE
// ============================================================================

texture2D FHX_CleanFrame
{
    Width = BUFFER_WIDTH;
    Height = BUFFER_HEIGHT;
    Format = RGBA8;
};

sampler2D FHX_CleanFrameSampler
{
    Texture = FHX_CleanFrame;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = POINT;
    AddressU = CLAMP;
    AddressV = CLAMP;
};


// ============================================================================
// RESOLUTION / MASK HELPERS
// ============================================================================

float2 FHX_GetResolutionScale()
{
    if (FHX_ResolutionPreset == 1)
        return float2(1280.0 / 1920.0, 720.0 / 1080.0);

    if (FHX_ResolutionPreset == 2)
        return float2(1.0, 1.0);

    if (FHX_ResolutionPreset == 3)
        return float2(2560.0 / 1920.0, 1440.0 / 1080.0);

    return float2(BUFFER_WIDTH / 1920.0, BUFFER_HEIGHT / 1080.0);
}

float4 FHX_ScaleRect(float4 rect1080)
{
    float2 s = FHX_GetResolutionScale();
    return float4(rect1080.xy * s, rect1080.zw * s);
}

// signedDistance > 0: inside protected UI
// signedDistance < 0: outside protected UI
float FHX_Feather(float signedDistancePx)
{
    float inPx = max(FHX_FeatherInPixels, 0.0);
    float outPx = max(FHX_FeatherOutPixels, 0.0);

    if (inPx <= 0.001 && outPx <= 0.001)
        return signedDistancePx >= 0.0 ? 1.0 : 0.0;

    return smoothstep(-outPx, inPx, signedDistancePx);
}

float FHX_RectMask(float2 p, float4 rect1080)
{
    float4 rectPx = FHX_ScaleRect(rect1080);
    float2 lo = min(rectPx.xy, rectPx.zw);
    float2 hi = max(rectPx.xy, rectPx.zw);

    float2 dLo = p - lo;
    float2 dHi = hi - p;

    float signedDistancePx =
        min(min(dLo.x, dLo.y), min(dHi.x, dHi.y));

    return FHX_Feather(signedDistancePx);
}

float FHX_EllipseMask(float2 p, float4 rect1080)
{
    float4 rectPx = FHX_ScaleRect(rect1080);
    float2 lo = min(rectPx.xy, rectPx.zw);
    float2 hi = max(rectPx.xy, rectPx.zw);

    float2 center = (lo + hi) * 0.5;
    float2 radius = max((hi - lo) * 0.5, float2(1.0, 1.0));

    float normalizedDistance = length((p - center) / radius);
    float signedDistancePx =
        (1.0 - normalizedDistance) * min(radius.x, radius.y);

    return FHX_Feather(signedDistancePx);
}


// ============================================================================
// MINIMAP COMPOUND MASK
//
// The minimap is not a simple circle. The ornate N/W/E/S tabs and the lower
// plus-button/frame protrude outside the round map body, so v4.5 masks these
// pieces separately. This avoids the oversized left-side ellipse from v4.3/4.4.
// ============================================================================

float FHX_MinimapMask(float2 p)
{
    float mask = 0.0;

    // Main circular body / outer ring.
    mask = max(mask, FHX_EllipseMask(
        p, float4(1775.0, 47.0, 1918.0, 198.0)));

    // North compass protrusion.
    mask = max(mask, FHX_RectMask(
        p, float4(1831.0, 39.0, 1860.0, 59.0)));

    // West tab.
    mask = max(mask, FHX_RectMask(
        p, float4(1766.0, 103.0, 1783.0, 131.0)));

    // East tab / right rim.
    mask = max(mask, FHX_RectMask(
        p, float4(1908.0, 103.0, 1920.0, 132.0)));

    // South tab.
    mask = max(mask, FHX_RectMask(
        p, float4(1834.0, 183.0, 1861.0, 204.0)));

    // Bottom-right plus button.
    mask = max(mask, FHX_EllipseMask(
        p, float4(1895.0, 170.0, 1920.0, 201.0)));

    // Thin lower frame under the circle.
    mask = max(mask, FHX_RectMask(
        p, float4(1776.0, 188.0, 1912.0, 205.0)));

    return mask;
}


// ============================================================================
// COMBINED MASK
//
// All coordinates below are measured against the supplied 1920x1080 source.
// They are automatically scaled for 720p and 1440p.
// ============================================================================

float FHX_GetCombinedMask(float2 uv)
{
    float2 p = uv * float2(BUFFER_WIDTH, BUFFER_HEIGHT);
    float mask = 0.0;

    // ---- Core HUD ----------------------------------------------------------

    // Buff row supports the longer multi-buff layout seen in the references.
    if (FHX_Protect_Buffs)
        mask = max(mask, FHX_RectMask(
            p, float4(0.0, 0.0, 250.0, 55.0)));

    // Party/Create is wider than the previous mask. It intentionally overlaps
    // the buff mask vertically to remove the thin unprotected seam.
    if (FHX_Protect_Party)
        mask = max(mask, FHX_RectMask(
            p, float4(0.0, 42.0, 174.0, 130.0)));

    if (FHX_Protect_TargetBar)
        mask = max(mask, FHX_RectMask(
            p, float4(838.0, 0.0, 1075.0, 39.0)));

    if (FHX_Protect_Minimap)
        mask = max(mask, FHX_MinimapMask(p));

    if (FHX_Protect_SkillWindow)
        mask = max(mask, FHX_RectMask(
            p, float4(1731.0, 196.0, 1920.0, 523.0)));

    if (FHX_Protect_Shop)
        mask = max(mask, FHX_RectMask(
            p, float4(1877.0, 920.0, 1920.0, 976.0)));

    if (FHX_Protect_BottomHUD)
        mask = max(mask, FHX_RectMask(
            p, float4(1274.0, 958.0, 1920.0, 1080.0)));


    // ---- Chat --------------------------------------------------------------

    if (FHX_Protect_LeftButtons)
        mask = max(mask, FHX_RectMask(
            p, float4(0.0, 947.0, 42.0, 1080.0)));

    if (FHX_Protect_ChatLeft)
        mask = max(mask, FHX_RectMask(
            p, float4(40.0, 947.0, 371.0, 1080.0)));

    if (FHX_Protect_ChatMiddle)
        mask = max(mask, FHX_RectMask(
            p, float4(370.0, 947.0, 730.0, 1080.0)));

    if (FHX_Protect_ChatRight)
        mask = max(mask, FHX_RectMask(
            p, float4(729.0, 947.0, 1090.0, 1080.0)));


    // ---- Quest UI ----------------------------------------------------------

    // Small "Lake Slime xx/60" status/progress window.
    if (FHX_Protect_QuestStatus)
        mask = max(mask, FHX_RectMask(
            p, float4(0.0, 337.0, 172.0, 395.0)));

    // Upper quest tracker: Edward's Location / current quest + Give Up.
    if (FHX_Protect_QuestTracker)
        mask = max(mask, FHX_RectMask(
            p, float4(0.0, 392.0, 382.0, 579.0)));

    // Lower Quest / Reward description + objectives pane.
    if (FHX_Protect_QuestDetail)
        mask = max(mask, FHX_RectMask(
            p, float4(0.0, 574.0, 382.0, 950.0)));


    // ---- Action UI ---------------------------------------------------------

    // Fixed skill/action progress bar measured from the supplied raw screenshot.
    if (FHX_Protect_ActionBar)
        mask = max(mask, FHX_RectMask(
            p, float4(418.0, 628.0, 614.0, 655.0)));

    return saturate(mask);
}


// ============================================================================
// SHADERS
// ============================================================================

float4 FHX_CapturePS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return tex2D(ReShade::BackBuffer, uv);
}

float4 FHX_RestorePS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float4 processed = tex2D(ReShade::BackBuffer, uv);
    float4 original = tex2D(FHX_CleanFrameSampler, uv);

    float mask = FHX_GetCombinedMask(uv);

    if (FHX_DebugMode == 1)
        return float4(mask.xxx, 1.0);

    if (FHX_DebugMode == 2)
    {
        float3 magenta = float3(1.0, 0.0, 1.0);
        return float4(
            lerp(processed.rgb, magenta, mask * 0.60),
            processed.a);
    }

    return lerp(
        processed,
        original,
        mask * FHX_RestoreStrength);
}


// ============================================================================
// TECHNIQUES
// ============================================================================

technique FHX_UIMask_Capture <
    ui_tooltip = "Place at the VERY TOP of the active technique order.";
>
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = FHX_CapturePS;
        RenderTarget = FHX_CleanFrame;
    }
}

technique FHX_UIMask_Restore <
    ui_tooltip = "Place at the VERY BOTTOM of the active technique order.";
>
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = FHX_RestorePS;
    }
}
