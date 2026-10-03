#include "crc32_hash.hpp"

#include <Windows.h>
#include <imgui.h>
#include <reshade.hpp>

#include <cstdint>
#include <mutex>
#include <unordered_map>

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "FHX Vulkan REST";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "FHX-specific Vulkan ReShade effect injector that renders before the Primary UI pass.";

namespace
{
constexpr uint32_t kPrimaryUiPixelShader = 0xCF49F7D6u;

struct CommandState
{
    bool insideRenderPass = false;
    bool pendingPrimaryUiBoundary = false;
    bool injectedThisRecording = false;
};

std::mutex g_mutex;
std::unordered_map<uint64_t, uint32_t> g_pixelShaderByPipeline;
std::unordered_map<command_list *, CommandState> g_commandStates;

effect_runtime *g_runtime = nullptr;
bool g_enabled = true;

thread_local bool g_insideManualRender = false;

uint64_t g_primaryUiBindsOutsidePass = 0;
uint64_t g_primaryUiBindsInsidePass = 0;
uint64_t g_preUiRenders = 0;
uint64_t g_rejectedBoundaries = 0;
uint64_t g_presentCount = 0;

uint32_t calculateShaderHash(const void *shaderData)
{
    if (shaderData == nullptr)
        return 0;

    const shader_desc shader = *static_cast<const shader_desc *>(shaderData);
    if (shader.code == nullptr || shader.code_size == 0)
        return 0;

    return compute_crc32(static_cast<const uint8_t *>(shader.code), shader.code_size);
}

bool isUsablePrimaryUiTarget(command_list *cmdList,
                             uint32_t count,
                             const render_pass_render_target_desc *rts)
{
    if (cmdList == nullptr || count != 1 || rts == nullptr || rts[0].view.handle == 0)
        return false;

    effect_runtime *runtime = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        runtime = g_runtime;
    }

    if (runtime == nullptr || runtime->get_device() != cmdList->get_device())
        return false;

    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    runtime->get_screenshot_width_and_height(&outputWidth, &outputHeight);
    if (outputWidth == 0 || outputHeight == 0)
        return false;

    device *dev = cmdList->get_device();
    const resource colorResource = dev->get_resource_from_view(rts[0].view);
    if (colorResource.handle == 0)
        return false;

    const resource_desc desc = dev->get_resource_desc(colorResource);
    if (desc.type != resource_type::texture_2d ||
        desc.texture.width != outputWidth ||
        desc.texture.height != outputHeight ||
        desc.texture.samples != 1)
        return false;

    const resource_view_desc viewDesc = dev->get_resource_view_desc(rts[0].view);
    const format fmt =
        viewDesc.format != format::unknown ? viewDesc.format : desc.texture.format;

    switch (format_to_default_typed(fmt, 0))
    {
        case format::r8g8b8a8_unorm:
        case format::b8g8r8a8_unorm:
        case format::r10g10b10a2_unorm:
        case format::r16g16b16a16_float:
            return true;
        default:
            return false;
    }
}

void onInitPipeline(device *,
                    pipeline_layout,
                    uint32_t subobjectCount,
                    const pipeline_subobject *subobjects,
                    pipeline pipelineHandle)
{
    if (pipelineHandle.handle == 0 || subobjects == nullptr)
        return;

    uint32_t pixelHash = 0;
    for (uint32_t i = 0; i < subobjectCount; ++i)
    {
        if (subobjects[i].type == pipeline_subobject_type::pixel_shader)
        {
            pixelHash = calculateShaderHash(subobjects[i].data);
            break;
        }
    }

    if (pixelHash == 0)
        return;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_pixelShaderByPipeline[pipelineHandle.handle] = pixelHash;
}

void onDestroyPipeline(device *, pipeline pipelineHandle)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pixelShaderByPipeline.erase(pipelineHandle.handle);
}

void onResetCommandList(command_list *cmdList)
{
    if (cmdList == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_commandStates[cmdList] = CommandState {};
}

void onDestroyCommandList(command_list *cmdList)
{
    if (cmdList == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_commandStates.erase(cmdList);
}

void onBindPipeline(command_list *cmdList, pipeline_stage stages, pipeline pipelineHandle)
{
    if (g_insideManualRender || cmdList == nullptr || pipelineHandle.handle == 0)
        return;

    if ((static_cast<uint32_t>(stages & pipeline_stage::pixel_shader)) == 0)
        return;

    std::lock_guard<std::mutex> lock(g_mutex);

    const auto pipelineIt = g_pixelShaderByPipeline.find(pipelineHandle.handle);
    if (pipelineIt == g_pixelShaderByPipeline.end() ||
        pipelineIt->second != kPrimaryUiPixelShader)
        return;

    CommandState &state = g_commandStates[cmdList];

    if (state.insideRenderPass)
    {
        ++g_primaryUiBindsInsidePass;
        return;
    }

    state.pendingPrimaryUiBoundary = true;
    ++g_primaryUiBindsOutsidePass;
}

void onBeginRenderPass(command_list *cmdList,
                       uint32_t count,
                       const render_pass_render_target_desc *rts,
                       const render_pass_depth_stencil_desc *)
{
    if (g_insideManualRender || cmdList == nullptr)
        return;

    bool shouldAttempt = false;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        CommandState &state = g_commandStates[cmdList];

        shouldAttempt =
            g_enabled &&
            state.pendingPrimaryUiBoundary &&
            !state.injectedThisRecording;
    }

    if (shouldAttempt)
    {
        effect_runtime *runtime = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            runtime = g_runtime;
        }

        if (runtime != nullptr &&
            runtime->get_effects_state() &&
            runtime->get_device() == cmdList->get_device() &&
            isUsablePrimaryUiTarget(cmdList, count, rts))
        {
            // ReShade calls this add-on event before forwarding the game's
            // vkCmdBeginRenderPass/vkCmdBeginRendering. The command list is
            // therefore outside the application render pass here.
            //
            // effect_runtime::render_effects performs its own state capture and
            // restoration for out-of-present rendering and marks effects as
            // rendered for the frame, so the normal end-of-frame pass will not
            // apply the same effects a second time.
            g_insideManualRender = true;
            runtime->render_effects(cmdList, rts[0].view, rts[0].view);
            g_insideManualRender = false;

            std::lock_guard<std::mutex> lock(g_mutex);
            CommandState &state = g_commandStates[cmdList];
            state.pendingPrimaryUiBoundary = false;
            state.injectedThisRecording = true;
            ++g_preUiRenders;
        }
        else
        {
            // Keep the pending boundary alive for the next render pass on this
            // same command list. Pipeline binds may legally happen several
            // commands before the actual UI render pass begins.
            std::lock_guard<std::mutex> lock(g_mutex);
            ++g_rejectedBoundaries;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_commandStates[cmdList].insideRenderPass = true;
    }

}

void onEndRenderPass(command_list *cmdList)
{
    if (g_insideManualRender || cmdList == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_commandStates[cmdList].insideRenderPass = false;
}

void onInitEffectRuntime(effect_runtime *runtime)
{
    if (runtime == nullptr || runtime->get_device() == nullptr)
        return;

    if (runtime->get_device()->get_api() != device_api::vulkan)
        return;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_runtime = runtime;
}

void onDestroyEffectRuntime(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_runtime == runtime)
        g_runtime = nullptr;
}

void onReshadePresent(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (runtime == g_runtime)
        ++g_presentCount;

    // Do not carry an unresolved pre-UI trigger into a later presented frame.
    // Command-list reset will normally clear this first on Vulkan, but this is
    // a conservative safety net for unusual reuse patterns.
    for (auto &[cmdList, state] : g_commandStates)
        state.pendingPrimaryUiBoundary = false;
}

void displaySettings(effect_runtime *)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    ImGui::TextUnformatted("FHX Vulkan REST");
    ImGui::Separator();
    ImGui::Checkbox("Render enabled ReShade effects before Primary UI", &g_enabled);
    ImGui::Text("Primary UI shader: 0x%08X", kPrimaryUiPixelShader);

    if (!g_enabled)
        ImGui::TextUnformatted("Status: disabled");
    else if (g_runtime == nullptr)
        ImGui::TextUnformatted("Status: waiting for Vulkan runtime");
    else
        ImGui::TextUnformatted("Status: active");

    ImGui::Spacing();
    ImGui::Text("Pre-UI effect renders: %llu",
                static_cast<unsigned long long>(g_preUiRenders));
    ImGui::Text("Primary UI binds before render pass: %llu",
                static_cast<unsigned long long>(g_primaryUiBindsOutsidePass));
    ImGui::Text("Primary UI binds inside render pass: %llu",
                static_cast<unsigned long long>(g_primaryUiBindsInsidePass));
    ImGui::Text("Rejected candidate passes: %llu",
                static_cast<unsigned long long>(g_rejectedBoundaries));
    ImGui::Text("Presented frames: %llu",
                static_cast<unsigned long long>(g_presentCount));

    ImGui::Spacing();
    ImGui::TextWrapped(
        "This is an active FHX-specific implementation, not a learning or "
        "diagnostic harness. When the known Primary UI pixel shader is bound "
        "outside a Vulkan render pass, the add-on renders the currently enabled "
        "ReShade effect chain into the next matching full-resolution color pass "
        "on that same command list before the game's UI pass begins.");
}
} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(hModule))
                return FALSE;

            reshade::register_event<reshade::addon_event::init_pipeline>(onInitPipeline);
            reshade::register_event<reshade::addon_event::destroy_pipeline>(onDestroyPipeline);
            reshade::register_event<reshade::addon_event::reset_command_list>(onResetCommandList);
            reshade::register_event<reshade::addon_event::destroy_command_list>(onDestroyCommandList);
            reshade::register_event<reshade::addon_event::bind_pipeline>(onBindPipeline);
            reshade::register_event<reshade::addon_event::begin_render_pass>(onBeginRenderPass);
            reshade::register_event<reshade::addon_event::end_render_pass>(onEndRenderPass);
            reshade::register_event<reshade::addon_event::init_effect_runtime>(onInitEffectRuntime);
            reshade::register_event<reshade::addon_event::destroy_effect_runtime>(onDestroyEffectRuntime);
            reshade::register_event<reshade::addon_event::reshade_present>(onReshadePresent);
            reshade::register_overlay(nullptr, &displaySettings);
            break;

        case DLL_PROCESS_DETACH:
            reshade::unregister_overlay(nullptr, &displaySettings);
            reshade::unregister_event<reshade::addon_event::reshade_present>(onReshadePresent);
            reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(onDestroyEffectRuntime);
            reshade::unregister_event<reshade::addon_event::init_effect_runtime>(onInitEffectRuntime);
            reshade::unregister_event<reshade::addon_event::end_render_pass>(onEndRenderPass);
            reshade::unregister_event<reshade::addon_event::begin_render_pass>(onBeginRenderPass);
            reshade::unregister_event<reshade::addon_event::bind_pipeline>(onBindPipeline);
            reshade::unregister_event<reshade::addon_event::destroy_command_list>(onDestroyCommandList);
            reshade::unregister_event<reshade::addon_event::reset_command_list>(onResetCommandList);
            reshade::unregister_event<reshade::addon_event::destroy_pipeline>(onDestroyPipeline);
            reshade::unregister_event<reshade::addon_event::init_pipeline>(onInitPipeline);
            reshade::unregister_addon(hModule);
            break;
    }

    return TRUE;
}
