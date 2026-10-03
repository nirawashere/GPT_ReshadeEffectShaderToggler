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
    resource_view currentRenderTarget = { 0 };
    resource currentRenderTargetResource = { 0 };

    bool containsPrimaryUi = false;
    resource_view primaryUiRenderTarget = { 0 };
    resource primaryUiResource = { 0 };
};

std::mutex g_mutex;
std::unordered_map<uint64_t, uint32_t> g_pixelShaderByPipeline;
std::unordered_map<command_list *, CommandState> g_commandStates;

effect_runtime *g_runtime = nullptr;
bool g_enabled = true;

thread_local bool g_insideManualRender = false;

bool g_injectedThisPresent = false;

uint64_t g_primaryUiCommandLists = 0;
uint64_t g_preUiSubmissions = 0;
uint64_t g_nonBackbufferUiSubmissions = 0;
uint64_t g_wrongQueueSubmissions = 0;
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

    // FHX/DXVK binds the Primary UI pipeline from inside the render pass.
    // Capture the target that pass is rendering to and tag this command list.
    if (state.insideRenderPass &&
        state.currentRenderTarget.handle != 0 &&
        state.currentRenderTargetResource.handle != 0)
    {
        if (!state.containsPrimaryUi)
            ++g_primaryUiCommandLists;

        state.containsPrimaryUi = true;
        state.primaryUiRenderTarget = state.currentRenderTarget;
        state.primaryUiResource = state.currentRenderTargetResource;
    }
}

void onBeginRenderPass(command_list *cmdList,
                       uint32_t count,
                       const render_pass_render_target_desc *rts,
                       const render_pass_depth_stencil_desc *)
{
    if (g_insideManualRender || cmdList == nullptr)
        return;

    resource_view target = { 0 };
    resource targetResource = { 0 };

    if (count != 0 && rts != nullptr && rts[0].view.handle != 0)
    {
        target = rts[0].view;
        targetResource = cmdList->get_device()->get_resource_from_view(target);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    CommandState &state = g_commandStates[cmdList];
    state.insideRenderPass = true;
    state.currentRenderTarget = target;
    state.currentRenderTargetResource = targetResource;
}

void onEndRenderPass(command_list *cmdList)
{
    if (g_insideManualRender || cmdList == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_mutex);
    CommandState &state = g_commandStates[cmdList];
    state.insideRenderPass = false;
    state.currentRenderTarget = { 0 };
    state.currentRenderTargetResource = { 0 };
}

void onExecuteCommandList(command_queue *queue, command_list *cmdList)
{
    if (g_insideManualRender || queue == nullptr || cmdList == nullptr)
        return;

    effect_runtime *runtime = nullptr;
    resource_view uiTarget = { 0 };
    resource uiResource = { 0 };
    bool shouldInject = false;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        runtime = g_runtime;

        const auto stateIt = g_commandStates.find(cmdList);
        if (stateIt == g_commandStates.end() ||
            !stateIt->second.containsPrimaryUi ||
            g_injectedThisPresent)
            return;

        uiTarget = stateIt->second.primaryUiRenderTarget;
        uiResource = stateIt->second.primaryUiResource;
        shouldInject =
            g_enabled &&
            runtime != nullptr &&
            runtime->get_effects_state() &&
            uiTarget.handle != 0 &&
            uiResource.handle != 0;
    }

    if (!shouldInject)
        return;

    // ReShade's Vulkan vkQueueSubmit hook invokes execute_command_list before
    // submitting the application's command buffers. Any commands recorded on
    // the queue's immediate list here are flushed by ReShade immediately
    // before that application submit, inheriting its wait semaphores. This
    // puts the effect pass on the GPU before the command buffer that contains
    // FHX's Primary UI draw without predicting render-pass order.
    if (runtime->get_command_queue() != queue)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_wrongQueueSubmissions;
        return;
    }

    const resource currentBackBuffer = runtime->get_current_back_buffer();
    if (currentBackBuffer.handle == 0 ||
        currentBackBuffer.handle != uiResource.handle)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_nonBackbufferUiSubmissions;
        return;
    }

    command_list *immediate = queue->get_immediate_command_list();
    if (immediate == nullptr)
        return;

    // render_effects requires the supplied target to be in render_target
    // state. The immediate submission waits on the application's acquire
    // semaphore, then restores PRESENT before the application's UI command
    // buffer is submitted.
    g_insideManualRender = true;
    immediate->barrier(uiResource, resource_usage::present, resource_usage::render_target);
    runtime->render_effects(immediate, uiTarget, uiTarget);
    immediate->barrier(uiResource, resource_usage::render_target, resource_usage::present);
    g_insideManualRender = false;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_injectedThisPresent = true;
        ++g_preUiSubmissions;
    }
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
    {
        ++g_presentCount;
        g_injectedThisPresent = false;
    }
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
        ImGui::TextUnformatted("Status: active - submit-boundary injection");

    ImGui::Spacing();
    ImGui::Text("Primary UI command lists seen: %llu",
                static_cast<unsigned long long>(g_primaryUiCommandLists));
    ImGui::Text("Pre-UI effect submissions: %llu",
                static_cast<unsigned long long>(g_preUiSubmissions));
    ImGui::Text("UI submissions not targeting swapchain: %llu",
                static_cast<unsigned long long>(g_nonBackbufferUiSubmissions));
    ImGui::Text("UI submissions on non-primary queue: %llu",
                static_cast<unsigned long long>(g_wrongQueueSubmissions));
    ImGui::Text("Presented frames: %llu",
                static_cast<unsigned long long>(g_presentCount));

    ImGui::Spacing();
    ImGui::TextWrapped(
        "FHX binds its Primary UI shader from inside a Vulkan render pass, so "
        "the add-on tags the command buffer while it is being recorded. When "
        "that command buffer is later submitted, enabled ReShade effects are "
        "recorded onto ReShade's immediate Vulkan command list and ordered "
        "before the FHX UI submission. This is an active implementation; it "
        "does not learn or predict render-pass hashes.");
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
            reshade::register_event<reshade::addon_event::execute_command_list>(onExecuteCommandList);
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
            reshade::unregister_event<reshade::addon_event::execute_command_list>(onExecuteCommandList);
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
