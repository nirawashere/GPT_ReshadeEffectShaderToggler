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

struct PassSignature
{
    bool valid = false;
    uint32_t colorWidth = 0;
    uint32_t colorHeight = 0;
    uint32_t colorSamples = 0;
    format colorFormat = format::unknown;
    render_pass_load_op colorLoad = render_pass_load_op::load;
    render_pass_store_op colorStore = render_pass_store_op::store;

    bool hasDepth = false;
    uint32_t depthWidth = 0;
    uint32_t depthHeight = 0;
    uint32_t depthSamples = 0;
    format depthFormat = format::unknown;
    render_pass_load_op depthLoad = render_pass_load_op::load;
    render_pass_store_op depthStore = render_pass_store_op::store;
};

struct LearnedUiTarget
{
    PassSignature signature = {};
    uint32_t localOccurrence = 0;
};

struct CommandState
{
    bool insideRenderPass = false;
    resource_view currentRenderTarget = { 0 };
    resource currentRenderTargetResource = { 0 };
    PassSignature currentPassSignature = {};
    uint32_t currentResourceOccurrence = 0;
    std::unordered_map<uint64_t, uint32_t> resourcePassOccurrences;
};

std::mutex g_mutex;
std::unordered_map<uint64_t, uint32_t> g_pixelShaderByPipeline;
std::unordered_map<command_list *, CommandState> g_commandStates;
std::unordered_map<uint64_t, LearnedUiTarget> g_learnedUiTargets;

effect_runtime *g_runtime = nullptr;
bool g_enabled = true;
bool g_injectedThisPresent = false;

thread_local bool g_insideManualRender = false;

uint64_t g_targetLearnEvents = 0;
uint64_t g_targetRelearnEvents = 0;
uint64_t g_preUiRenders = 0;
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

bool samePassSignature(const PassSignature &a, const PassSignature &b)
{
    return a.valid && b.valid &&
           a.colorWidth == b.colorWidth &&
           a.colorHeight == b.colorHeight &&
           a.colorSamples == b.colorSamples &&
           a.colorFormat == b.colorFormat &&
           a.colorLoad == b.colorLoad &&
           a.colorStore == b.colorStore &&
           a.hasDepth == b.hasDepth &&
           (!a.hasDepth ||
            (a.depthWidth == b.depthWidth &&
             a.depthHeight == b.depthHeight &&
             a.depthSamples == b.depthSamples &&
             a.depthFormat == b.depthFormat &&
             a.depthLoad == b.depthLoad &&
             a.depthStore == b.depthStore));
}

PassSignature buildPassSignature(command_list *cmdList,
                                 uint32_t count,
                                 const render_pass_render_target_desc *rts,
                                 const render_pass_depth_stencil_desc *ds)
{
    PassSignature sig = {};

    if (cmdList == nullptr || count != 1 || rts == nullptr || rts[0].view.handle == 0)
        return sig;

    device *dev = cmdList->get_device();
    if (dev == nullptr)
        return sig;

    const resource colorResource = dev->get_resource_from_view(rts[0].view);
    if (colorResource.handle == 0)
        return sig;

    const resource_desc colorDesc = dev->get_resource_desc(colorResource);
    if (colorDesc.type != resource_type::texture_2d)
        return sig;

    const resource_view_desc colorViewDesc = dev->get_resource_view_desc(rts[0].view);

    sig.colorWidth = colorDesc.texture.width;
    sig.colorHeight = colorDesc.texture.height;
    sig.colorSamples = colorDesc.texture.samples;
    sig.colorFormat =
        colorViewDesc.format != format::unknown ? colorViewDesc.format : colorDesc.texture.format;
    sig.colorLoad = rts[0].load_op;
    sig.colorStore = rts[0].store_op;

    if (ds != nullptr && ds->view.handle != 0)
    {
        const resource depthResource = dev->get_resource_from_view(ds->view);
        if (depthResource.handle != 0)
        {
            const resource_desc depthDesc = dev->get_resource_desc(depthResource);
            const resource_view_desc depthViewDesc = dev->get_resource_view_desc(ds->view);

            if (depthDesc.type == resource_type::texture_2d)
            {
                sig.hasDepth = true;
                sig.depthWidth = depthDesc.texture.width;
                sig.depthHeight = depthDesc.texture.height;
                sig.depthSamples = depthDesc.texture.samples;
                sig.depthFormat =
                    depthViewDesc.format != format::unknown ? depthViewDesc.format : depthDesc.texture.format;
                sig.depthLoad = ds->depth_load_op;
                sig.depthStore = ds->depth_store_op;
            }
        }
    }

    sig.valid = true;
    return sig;
}

bool isFullResolutionUiCandidate(effect_runtime *runtime, const PassSignature &sig)
{
    if (runtime == nullptr || !sig.valid || sig.colorSamples != 1)
        return false;

    uint32_t width = 0;
    uint32_t height = 0;
    runtime->get_screenshot_width_and_height(&width, &height);

    return width != 0 &&
           height != 0 &&
           sig.colorWidth == width &&
           sig.colorHeight == height;
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

void onDestroyResource(device *, resource resourceHandle)
{
    if (resourceHandle.handle == 0)
        return;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_learnedUiTargets.erase(resourceHandle.handle);
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

    const auto stateIt = g_commandStates.find(cmdList);
    if (stateIt == g_commandStates.end())
        return;

    const CommandState &state = stateIt->second;
    if (!state.insideRenderPass ||
        state.currentRenderTargetResource.handle == 0 ||
        !state.currentPassSignature.valid ||
        state.currentResourceOccurrence == 0)
        return;

    const uint64_t resourceHandle = state.currentRenderTargetResource.handle;
    const LearnedUiTarget learned {
        state.currentPassSignature,
        state.currentResourceOccurrence
    };

    const auto learnedIt = g_learnedUiTargets.find(resourceHandle);
    if (learnedIt == g_learnedUiTargets.end())
    {
        g_learnedUiTargets.emplace(resourceHandle, learned);
        ++g_targetLearnEvents;
    }
    else if (!samePassSignature(learnedIt->second.signature, learned.signature) ||
             learnedIt->second.localOccurrence != learned.localOccurrence)
    {
        learnedIt->second = learned;
        ++g_targetRelearnEvents;
    }
}

void onBeginRenderPass(command_list *cmdList,
                       uint32_t count,
                       const render_pass_render_target_desc *rts,
                       const render_pass_depth_stencil_desc *ds)
{
    if (g_insideManualRender || cmdList == nullptr)
        return;

    resource_view target = { 0 };
    resource targetResource = { 0 };
    PassSignature signature = buildPassSignature(cmdList, count, rts, ds);

    if (count != 0 && rts != nullptr && rts[0].view.handle != 0)
    {
        target = rts[0].view;
        targetResource = cmdList->get_device()->get_resource_from_view(target);
    }

    effect_runtime *runtime = nullptr;
    uint32_t localOccurrence = 0;
    bool shouldInject = false;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        CommandState &state = g_commandStates[cmdList];

        if (targetResource.handle != 0)
            localOccurrence = ++state.resourcePassOccurrences[targetResource.handle];

        state.currentRenderTarget = target;
        state.currentRenderTargetResource = targetResource;
        state.currentPassSignature = signature;
        state.currentResourceOccurrence = localOccurrence;

        runtime = g_runtime;

        if (g_enabled &&
            !g_injectedThisPresent &&
            runtime != nullptr &&
            runtime->get_effects_state() &&
            runtime->get_device() == cmdList->get_device() &&
            target.handle != 0 &&
            targetResource.handle != 0 &&
            isFullResolutionUiCandidate(runtime, signature))
        {
            const auto learnedIt = g_learnedUiTargets.find(targetResource.handle);
            if (learnedIt != g_learnedUiTargets.end() &&
                learnedIt->second.localOccurrence == localOccurrence &&
                samePassSignature(learnedIt->second.signature, signature))
            {
                shouldInject = true;
            }
        }
    }

    if (shouldInject)
    {
        // This is the actual pre-UI injection point. The target was learned
        // from a previous Primary-UI draw on this exact render-target resource,
        // render-pass structure and resource-local occurrence. No frame-global
        // pass numbers, command-list identities or candidate ordering are used.
        g_insideManualRender = true;
        runtime->render_effects(cmdList, target, target);
        g_insideManualRender = false;

        std::lock_guard<std::mutex> lock(g_mutex);
        g_injectedThisPresent = true;
        ++g_preUiRenders;
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
    CommandState &state = g_commandStates[cmdList];
    state.insideRenderPass = false;
    state.currentRenderTarget = { 0 };
    state.currentRenderTargetResource = { 0 };
    state.currentPassSignature = {};
    state.currentResourceOccurrence = 0;
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
    {
        g_runtime = nullptr;
        g_learnedUiTargets.clear();
        g_injectedThisPresent = false;
    }
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
    else if (g_learnedUiTargets.empty())
        ImGui::TextUnformatted("Status: active - learning first UI target");
    else
        ImGui::TextUnformatted("Status: active - direct learned-target injection");

    ImGui::Spacing();
    ImGui::Text("Learned UI render targets: %u",
                static_cast<unsigned int>(g_learnedUiTargets.size()));
    ImGui::Text("Target learns: %llu",
                static_cast<unsigned long long>(g_targetLearnEvents));
    ImGui::Text("Target relearns: %llu",
                static_cast<unsigned long long>(g_targetRelearnEvents));
    ImGui::Text("Pre-UI effect renders: %llu",
                static_cast<unsigned long long>(g_preUiRenders));
    ImGui::Text("Presented frames: %llu",
                static_cast<unsigned long long>(g_presentCount));

    ImGui::Spacing();
    ImGui::TextWrapped(
        "FHX renders Primary UI into a full-resolution offscreen target rather "
        "than directly into the swapchain. The first Primary-UI draw teaches "
        "the add-on that target's resource, render-pass structure and local pass "
        "occurrence. On later uses of that same target, enabled ReShade effects "
        "are rendered immediately before the learned UI pass begins. This is "
        "an active implementation; there is no multi-stage validation harness.");
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
            reshade::register_event<reshade::addon_event::destroy_resource>(onDestroyResource);
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
            reshade::unregister_event<reshade::addon_event::destroy_resource>(onDestroyResource);
            reshade::unregister_event<reshade::addon_event::destroy_command_list>(onDestroyCommandList);
            reshade::unregister_event<reshade::addon_event::reset_command_list>(onResetCommandList);
            reshade::unregister_event<reshade::addon_event::destroy_pipeline>(onDestroyPipeline);
            reshade::unregister_event<reshade::addon_event::init_pipeline>(onInitPipeline);
            reshade::unregister_addon(hModule);
            break;
    }

    return TRUE;
}
