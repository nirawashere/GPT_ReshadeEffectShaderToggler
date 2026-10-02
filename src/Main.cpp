///////////////////////////////////////////////////////////////////////
//
// Part of ShaderToggler, a shader toggler add on for Reshade 5+ which allows you
// to define groups of shaders to toggle them on/off with one key press
//
// (c) Frans 'Otis_Inf' Bouma.
//
// All rights reserved.
// https://github.com/FransBouma/ShaderToggler
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met :
//
//  * Redistributions of source code must retain the above copyright notice, this
//	  list of conditions and the following disclaimer.
//
//  * Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and / or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
/////////////////////////////////////////////////////////////////////////
#include "AddonUIData.h"
#include "AddonUIDisplay.h"
#include "CDataFile.h"
#include "ConstantManager.h"
#include "KeyMonitor.h"
#include "PipelinePrivateData.h"
#include "RenderingBindingManager.h"
#include "RenderingEffectManager.h"
#include "RenderingManager.h"
#include "RenderingPreviewManager.h"
#include "RenderingQueueManager.h"
#include "RenderingShaderManager.h"
#include "ResourceManager.h"
#include "ShaderManager.h"
#include "StateTracking.h"
#include "TechniqueManager.h"
#include "ToggleGroup.h"
#include "crc32_hash.hpp"
#include <MinHook.h>
#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <imgui.h>
#include <reshade.hpp>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

using namespace reshade::api;
using namespace ShaderToggler;
using namespace AddonImGui;
using namespace Shim::Constants;
using namespace std;

extern "C" __declspec(dllexport) const char* NAME = "Reshade Effect Shader Toggler";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Addon which allows you to define groups of shaders to render Reshade effects on.";

constexpr auto MAX_EFFECT_HANDLES = 128;
constexpr auto REST_VAR_ANNOTATION = "source";

static filesystem::path g_dllPath;
static filesystem::path g_basePath;

static ShaderToggler::ShaderManager g_pixelShaderManager;
static ShaderToggler::ShaderManager g_vertexShaderManager;
static ShaderToggler::ShaderManager g_computeShaderManager;

static ConstantManager constantManager;
static ConstantHandlerBase* constantHandler = nullptr;
static ConstantCopyBase* constantCopy = nullptr;
static bool constantHandlerHooked = false;

static atomic_uint32_t g_activeCollectorFrameCounter = 0;
static AddonUIData g_addonUIData(&g_pixelShaderManager, &g_vertexShaderManager, &g_computeShaderManager, constantHandler, &g_activeCollectorFrameCounter);

static KeyMonitor keyMonitor;
static Rendering::ResourceManager resourceManager;
static Rendering::ToggleGroupResourceManager groupResourceManager;
static Rendering::RenderingShaderManager renderingShaderManager(g_addonUIData, resourceManager);
static Rendering::RenderingEffectManager renderingEffectManager(g_addonUIData, resourceManager, renderingShaderManager, groupResourceManager);
static Rendering::RenderingBindingManager renderingBindingManager(g_addonUIData, resourceManager, groupResourceManager);
static Rendering::RenderingPreviewManager renderingPreviewManager(g_addonUIData, resourceManager, renderingShaderManager);
static Rendering::RenderingQueueManager renderingQueueManager(g_addonUIData, resourceManager);
static ShaderToggler::TechniqueManager techniqueManager(keyMonitor);

// TODO: actually implement ability to turn off srgb-view generation
static vector<effect_runtime*> runtimes;

/// <summary>
/// Calculates a crc32 hash from the passed in shader bytecode. The hash is used to identity the shader in future runs.
/// </summary>
/// <param name="shaderData"></param>
/// <returns></returns>
static uint32_t calculateShaderHash(void* shaderData) {
    if (nullptr == shaderData) {
        return 0;
    }

    const auto shaderDesc = *static_cast<shader_desc*>(shaderData);
    return compute_crc32(static_cast<const uint8_t*>(shaderDesc.code), shaderDesc.code_size);
}

static void onInitDevice(device* device) {
    device->create_private_data<DeviceDataContainer>();
}

static void onDestroyDevice(device* device) {
    DeviceDataContainer& data = device->get_private_data<DeviceDataContainer>();

    groupResourceManager.DisposeGroupBuffers(device, g_addonUIData.GetToggleGroups());
    renderingBindingManager.DisposeTextureBindings(device, g_addonUIData.GetToggleGroups());
    resourceManager.OnDestroyDevice(device);
    renderingShaderManager.DestroyShaders(device);

    device->destroy_private_data<DeviceDataContainer>();
}

static void onInitCommandList(command_list* commandList) {
    commandList->create_private_data<CommandListDataContainer>();
}

static void onDestroyCommandList(command_list* commandList) {
    commandList->destroy_private_data<CommandListDataContainer>();
}

static void onResetCommandList(command_list* commandList) {
    CommandListDataContainer& commandListData = commandList->get_private_data<CommandListDataContainer>();
    commandListData.Reset();
}

static bool onCreateSwapchain(swapchain_desc& desc, void* hwnd) {
    return resourceManager.OnCreateSwapchain(desc, hwnd);
}

static void onInitSwapchain(reshade::api::swapchain* swapchain) {
    resourceManager.OnInitSwapchain(swapchain);
}

static void onDestroySwapchain(reshade::api::swapchain* swapchain) {
    resourceManager.OnDestroySwapchain(swapchain);
}

static bool onCreateResource(device* device, resource_desc& desc, subresource_data* initial_data, resource_usage initial_state) {
    return resourceManager.OnCreateResource(device, desc, initial_data, initial_state);
}

static void onInitResource(device* device, const resource_desc& desc, const subresource_data* initData, resource_usage usage, reshade::api::resource handle) {
    resourceManager.OnInitResource(device, desc, initData, usage, handle);

    if (constantCopy != nullptr)
        constantCopy->OnInitResource(device, desc, initData, usage, handle);
}

static void onDestroyResource(device* device, resource res) {
    resourceManager.OnDestroyResource(device, res);

    if (constantCopy != nullptr)
        constantCopy->OnDestroyResource(device, res);
}

static bool onCreateResourceView(device* device, resource resource, resource_usage usage_type, resource_view_desc& desc) {
    return resourceManager.OnCreateResourceView(device, resource, usage_type, desc);
}

static void onInitResourceView(device* device, resource resource, resource_usage usage_type, const resource_view_desc& desc, resource_view view) {
    resourceManager.OnInitResourceView(device, resource, usage_type, desc, view);
}

static void onDestroyResourceView(device* device, resource_view view) {
    resourceManager.OnDestroyResourceView(device, view);
}

static void onReshadeReloadedEffects(effect_runtime* runtime) {
    RuntimeDataContainer& runtimeData = runtime->get_private_data<RuntimeDataContainer>();
    DeviceDataContainer& deviceData = runtime->get_device()->get_private_data<DeviceDataContainer>();

    techniqueManager.OnReshadeReloadedEffects(runtime);

    if (deviceData.current_runtime == runtime) {
        shared_lock<shared_mutex> techLock(runtimeData.technique_mutex);
        g_addonUIData.AssignPreferredGroupTechniques(runtimeData.allTechniques);
    }
}

static bool onReshadeSetTechniqueState(effect_runtime* runtime, effect_technique technique, bool enabled) {
    RuntimeDataContainer& data = runtime->get_private_data<RuntimeDataContainer>();

    bool ret = techniqueManager.OnReshadeSetTechniqueState(runtime, technique, enabled);

    return ret;
}

static bool onReshadeReorderTechniques(effect_runtime* runtime, size_t count, effect_technique* techniques) {
    RuntimeDataContainer& runtimeData = runtime->get_private_data<RuntimeDataContainer>();
    DeviceDataContainer& deviceData = runtime->get_device()->get_private_data<DeviceDataContainer>();

    bool ret = techniqueManager.OnReshadeReorderTechniques(runtime, count, techniques);

    if (deviceData.current_runtime == runtime) {
        shared_lock<shared_mutex> techLock(runtimeData.technique_mutex);
        g_addonUIData.AssignPreferredGroupTechniques(runtimeData.allTechniques);
    }

    return ret;
}

static void onInitEffectRuntime(effect_runtime* runtime) {
    runtime->create_private_data<RuntimeDataContainer>();
    DeviceDataContainer& data = runtime->get_device()->get_private_data<DeviceDataContainer>();

    keyMonitor.Init(runtime);
    renderingShaderManager.InitShaders(runtime->get_device());

    // Push new runtime on top
    runtimes.push_back(runtime);
    data.current_runtime = runtime;

    renderingBindingManager.InitTextureBingings(runtime);

    if (constantHandler != nullptr) {
        constantHandler->ReloadConstantVariables(runtime);
    }
}

static void onDestroyEffectRuntime(effect_runtime* runtime) {
    DeviceDataContainer& data = runtime->get_device()->get_private_data<DeviceDataContainer>();

    renderingBindingManager.DisposeTextureBindings(runtime->get_device(), g_addonUIData.GetToggleGroups());

    // Remove runtime from stack
    for (auto it = runtimes.begin(); it != runtimes.end();) {
        if (runtime == *it) {
            it = runtimes.erase(it);
            continue;
        }

        it++;
    }

    // Pick the runtime on top of our stack if there are any
    if (runtime == data.current_runtime) {
        if (runtimes.size() > 0) {
            data.current_runtime = runtimes[runtimes.size() - 1];

            if (constantHandler != nullptr) {
                constantHandler->ReloadConstantVariables(data.current_runtime);
            }
        } else {
            data.current_runtime = nullptr;

            if (constantHandler != nullptr) {
                constantHandler->ClearConstantVariables();
            }
        }
    }

    runtime->destroy_private_data<RuntimeDataContainer>();
}

static void onInitPipeline(device* device, pipeline_layout, uint32_t subobjectCount, const pipeline_subobject* subobjects, pipeline pipelineHandle) {
    // shader has been created, we will now create a hash and store it with the handle we got.
    for (uint32_t i = 0; i < subobjectCount; ++i) {
        switch (subobjects[i].type) {
            case pipeline_subobject_type::vertex_shader: {
                g_vertexShaderManager.addHashHandlePair(calculateShaderHash(subobjects[i].data), pipelineHandle.handle);
            } break;
            case pipeline_subobject_type::pixel_shader: {
                g_pixelShaderManager.addHashHandlePair(calculateShaderHash(subobjects[i].data), pipelineHandle.handle);
            } break;
            case pipeline_subobject_type::compute_shader: {
                g_computeShaderManager.addHashHandlePair(calculateShaderHash(subobjects[i].data), pipelineHandle.handle);
            } break;
        }
    }
}

static void onDestroyPipeline(device* device, pipeline pipelineHandle) {
    g_pixelShaderManager.removeHandle(pipelineHandle.handle);
    g_vertexShaderManager.removeHandle(pipelineHandle.handle);
    g_computeShaderManager.removeHandle(pipelineHandle.handle);
}

static void onBindPipeline(command_list* commandList, pipeline_stage stages, pipeline pipelineHandle) {
    if (nullptr == commandList || pipelineHandle.handle == 0 ||
        !((uint32_t)(stages & pipeline_stage::pixel_shader) || (uint32_t)(stages & pipeline_stage::vertex_shader) ||
          (uint32_t)(stages & pipeline_stage::compute_shader))) {
        return;
    }

    const uint32_t handleHasPixelShaderAttached =
      (uint32_t)(stages & pipeline_stage::pixel_shader) ? g_pixelShaderManager.safeGetShaderHash(pipelineHandle.handle) : 0;
    const uint32_t handleHasVertexShaderAttached =
      (uint32_t)(stages & pipeline_stage::vertex_shader) ? g_vertexShaderManager.safeGetShaderHash(pipelineHandle.handle) : 0;
    const uint32_t handleHasComputeShaderAttached =
      (uint32_t)(stages & pipeline_stage::compute_shader) ? g_computeShaderManager.safeGetShaderHash(pipelineHandle.handle) : 0;

    if (!handleHasPixelShaderAttached && !handleHasVertexShaderAttached && !handleHasComputeShaderAttached) {
        // draw call with unknown handle, don't collect it
        return;
    }
    CommandListDataContainer& commandListData = commandList->get_private_data<CommandListDataContainer>();
    DeviceDataContainer& deviceData = commandList->get_device()->get_private_data<DeviceDataContainer>();

    if (deviceData.current_runtime == nullptr || !deviceData.current_runtime->get_effects_state()) {
        return;
    }

    uint32_t pipelineChanged = 0;

    if ((uint32_t)(stages & pipeline_stage::pixel_shader) && handleHasPixelShaderAttached) {
        if (g_activeCollectorFrameCounter > 0) {
            // in collection mode
            g_pixelShaderManager.addActivePipelineHandle(pipelineHandle.handle);
        }
        if (commandListData.ps.activeShaderHash != handleHasPixelShaderAttached) {
            pipelineChanged |= Rendering::MATCH_EFFECT_PS | Rendering::MATCH_BINDING_PS | Rendering::MATCH_PREVIEW_PS | Rendering::MATCH_CONST_PS;
            commandListData.ps.constantBuffersToUpdate.clear();
        }

        commandListData.ps.blockedShaderGroups = g_addonUIData.GetToggleGroupsForPixelShaderHash(handleHasPixelShaderAttached);
        commandListData.ps.activeShaderHash = handleHasPixelShaderAttached;
    }

    if ((uint32_t)(stages & pipeline_stage::vertex_shader) && handleHasVertexShaderAttached) {
        if (g_activeCollectorFrameCounter > 0) {
            // in collection mode
            g_vertexShaderManager.addActivePipelineHandle(pipelineHandle.handle);
        }
        if (commandListData.vs.activeShaderHash != handleHasVertexShaderAttached) {
            pipelineChanged |= Rendering::MATCH_EFFECT_VS | Rendering::MATCH_BINDING_VS | Rendering::MATCH_PREVIEW_VS | Rendering::MATCH_CONST_VS;
            commandListData.vs.constantBuffersToUpdate.clear();
        }

        commandListData.vs.blockedShaderGroups = g_addonUIData.GetToggleGroupsForVertexShaderHash(handleHasVertexShaderAttached);
        commandListData.vs.activeShaderHash = handleHasVertexShaderAttached;
    }

    if ((uint32_t)(stages & pipeline_stage::compute_shader) && handleHasComputeShaderAttached) {
        if (g_activeCollectorFrameCounter > 0) {
            // in collection mode
            g_computeShaderManager.addActivePipelineHandle(pipelineHandle.handle);
        }
        if (commandListData.cs.activeShaderHash != handleHasComputeShaderAttached) {
            pipelineChanged |= Rendering::MATCH_EFFECT_CS | Rendering::MATCH_BINDING_CS | Rendering::MATCH_PREVIEW_CS | Rendering::MATCH_CONST_CS;
            commandListData.cs.constantBuffersToUpdate.clear();
        }

        commandListData.cs.blockedShaderGroups = g_addonUIData.GetToggleGroupsForComputeShaderHash(handleHasComputeShaderAttached);
        commandListData.cs.activeShaderHash = handleHasComputeShaderAttached;
    }

    if (pipelineChanged > 0) {
        // Perform updates scheduled for after a shader has been applied. Only do so once the draw flag has been cleared.
        if (commandListData.commandQueue & Rendering::CHECK_MATCH_BIND_PIPELINE_PREVIEW &&
            !(commandListData.commandQueue & pipelineChanged & Rendering::MATCH_PREVIEW)) {
            renderingPreviewManager.UpdatePreview(commandList, Rendering::CALL_BIND_PIPELINE, pipelineChanged & Rendering::MATCH_PREVIEW);
        }

        if (commandListData.commandQueue & Rendering::CHECK_MATCH_BIND_PIPELINE_BINDING &&
            !(commandListData.commandQueue & pipelineChanged & Rendering::MATCH_BINDING)) {
            renderingBindingManager.UpdateTextureBindings(commandList, Rendering::CALL_BIND_PIPELINE, pipelineChanged & Rendering::MATCH_BINDING);
        }

        if (commandListData.commandQueue & Rendering::CHECK_MATCH_BIND_PIPELINE_EFFECT &&
            !(commandListData.commandQueue & pipelineChanged & Rendering::MATCH_EFFECT)) {
            renderingEffectManager.RenderEffects(commandList, Rendering::CALL_BIND_PIPELINE, pipelineChanged & Rendering::MATCH_EFFECT);
        }

        renderingQueueManager.ClearQueue(commandListData, pipelineChanged);
        renderingQueueManager.CheckCallForCommandList(commandList);
    }
}

static void onBindRenderTargetsAndDepthStencil(command_list* cmd_list, uint32_t count, const resource_view* rtvs, resource_view dsv) {
    if (cmd_list == nullptr || cmd_list->get_device() == nullptr) {
        return;
    }

    device* device = cmd_list->get_device();
    CommandListDataContainer& commandListData = cmd_list->get_private_data<CommandListDataContainer>();
    DeviceDataContainer& deviceData = device->get_private_data<DeviceDataContainer>();

    // if (count > 0)
    //{
    if (commandListData.commandQueue & Rendering::CHECK_MATCH_BIND_RENDERTARGET_PREVIEW &&
        !(commandListData.commandQueue & Rendering::CHECK_MATCH_DRAW_PREVIEW)) {
        renderingPreviewManager.UpdatePreview(cmd_list, Rendering::CALL_BIND_RENDER_TARGET, Rendering::MATCH_PREVIEW);
    }

    if (commandListData.commandQueue & Rendering::CHECK_MATCH_BIND_RENDERTARGET_BINDING &&
        !(commandListData.commandQueue & Rendering::CHECK_MATCH_DRAW_BINDING)) {
        renderingBindingManager.UpdateTextureBindings(cmd_list, Rendering::CALL_BIND_RENDER_TARGET, Rendering::MATCH_BINDING);
    }

    if (commandListData.commandQueue & Rendering::CHECK_MATCH_BIND_RENDERTARGET_EFFECT &&
        !(commandListData.commandQueue & Rendering::CHECK_MATCH_DRAW_EFFECT)) {
        renderingEffectManager.RenderEffects(cmd_list, Rendering::CALL_BIND_RENDER_TARGET, Rendering::MATCH_EFFECT);
    }

    renderingQueueManager.RescheduleGroups(commandListData, deviceData);
    //}
}

static void onBeginRenderPass(command_list* cmd_list, uint32_t count, const render_pass_render_target_desc* rts, const render_pass_depth_stencil_desc* ds) {
    if (cmd_list == nullptr || cmd_list->get_device() == nullptr) {
        return;
    }

    device* device = cmd_list->get_device();
    CommandListDataContainer& commandListData = cmd_list->get_private_data<CommandListDataContainer>();
    DeviceDataContainer& deviceData = device->get_private_data<DeviceDataContainer>();

    if (!deviceData.current_runtime->get_effects_state()) {
        return;
    }

    if (commandListData.commandQueue & Rendering::CHECK_MATCH_DRAW_BINDING) {
        renderingBindingManager.UpdateTextureBindings(cmd_list, Rendering::CALL_DRAW, Rendering::MATCH_BINDING_PS | Rendering::MATCH_BINDING_VS);
    }

    if (commandListData.commandQueue & Rendering::CHECK_MATCH_DRAW_EFFECT) {
        renderingEffectManager.RenderEffects(cmd_list, Rendering::CALL_DRAW, Rendering::MATCH_EFFECT_PS | Rendering::MATCH_EFFECT_VS);
    }
}

static void onReshadeOverlay(effect_runtime* runtime) {
    DisplayOverlay(g_addonUIData, resourceManager, runtime);
}

static void onPresent(command_queue* queue,
                      swapchain* swapchain,
                      const rect* source_rect,
                      const rect* dest_rect,
                      uint32_t dirty_rect_count,
                      const rect* dirty_rects) {
    device* dev = queue->get_device();
    DeviceDataContainer& deviceData = dev->get_private_data<DeviceDataContainer>();

    if (deviceData.current_runtime == nullptr) {
        return;
    }

    effect_runtime* runtime = deviceData.current_runtime;

    if (queue == runtime->get_command_queue()) {
        if (runtime->get_effects_state()) {
            renderingEffectManager.RenderRemainingEffects(runtime);
        }
    }

    if (dev->get_api() != device_api::d3d12 && dev->get_api() != device_api::vulkan)
        onResetCommandList(runtime->get_command_queue()->get_immediate_command_list());
}

static void onReshadePresent(effect_runtime* runtime) {
    device* dev = runtime->get_device();
    DeviceDataContainer& deviceData = dev->get_private_data<DeviceDataContainer>();
    command_queue* queue = runtime->get_command_queue();

    deviceData.rendered_effects = false;

    keyMonitor.PollKeyStates(runtime);

    if (g_addonUIData.GetPreventRuntimeReload()) {
        renderingEffectManager.PreventRuntimeReload(runtime, queue->get_immediate_command_list());
    }

    if (runtime->get_effects_state()) {
        resourceManager.CheckPreview(queue->get_immediate_command_list(), dev);
        groupResourceManager.CheckGroupBuffers(runtime, g_addonUIData.GetToggleGroups());
        renderingBindingManager.ClearUnmatchedTextureBindings(runtime->get_command_queue()->get_immediate_command_list());
        resourceManager.CheckResourceViews(runtime);
    }

    techniqueManager.OnReshadePresent(runtime);

    deviceData.bindingsUpdated.clear();
    deviceData.constantsUpdated.clear();
    deviceData.huntPreview.Reset();

    CheckHotkeys(g_addonUIData, runtime);
}

static void onMapBufferRegion(device* device, resource resource, uint64_t offset, uint64_t size, map_access access, void** data) {
    if (constantCopy != nullptr)
        constantCopy->OnMapBufferRegion(device, resource, offset, size, access, data);
}

static void onUnmapBufferRegion(device* device, resource resource) {
    if (constantCopy != nullptr)
        constantCopy->OnUnmapBufferRegion(device, resource);
}

static bool onUpdateBufferRegion(device* device, const void* data, resource resource, uint64_t offset, uint64_t size) {
    if (constantCopy != nullptr)
        constantCopy->OnUpdateBufferRegion(device, data, resource, offset, size);

    return false;
}

static void displaySettings(effect_runtime* runtime) {
    DisplaySettings(g_addonUIData, runtime);
}

static void Init() {
    resourceManager.SetResourceShim(g_addonUIData.GetResourceShim());
    resourceManager.Init();
    constantManager.Init(g_addonUIData, groupResourceManager, &constantCopy, &constantHandler);

    g_addonUIData.AddToggleGroupRemovalCallback(
      std::bind(&Rendering::ToggleGroupResourceManager::ToggleGroupRemoved, &groupResourceManager, std::placeholders::_1, std::placeholders::_2));
    techniqueManager.AddEffectsReloadingCallback(std::bind(&Shim::Constants::ConstantHandlerBase::OnEffectsReloading, constantHandler, std::placeholders::_1));
    techniqueManager.AddEffectsReloadedCallback(std::bind(&Shim::Constants::ConstantHandlerBase::OnEffectsReloaded, constantHandler, std::placeholders::_1));
    techniqueManager.AddEffectsReloadingCallback(std::bind(&Rendering::ResourceManager::OnEffectsReloading, &resourceManager, std::placeholders::_1));
    techniqueManager.AddEffectsReloadedCallback(std::bind(&Rendering::ResourceManager::OnEffectsReloaded, &resourceManager, std::placeholders::_1));
}

static void UnInit() {
    constantManager.UnInit();
}

static void CheckDrawCall(command_list* cmd_list, const uint64_t match_modifier = Rendering::MATCH_ALL) {
    CommandListDataContainer& commandListData = cmd_list->get_private_data<CommandListDataContainer>();

    if (commandListData.commandQueue & Rendering::MATCH_ALL & match_modifier) {
        if (constantHandler != nullptr && (commandListData.commandQueue & Rendering::MATCH_CONST & match_modifier)) {
            constantHandler->UpdateConstants(cmd_list);
            commandListData.commandQueue &= ~(Rendering::MATCH_CONST & match_modifier);
        }

        if (commandListData.commandQueue & Rendering::MATCH_PREVIEW & match_modifier) {
            renderingPreviewManager.UpdatePreview(cmd_list, Rendering::CALL_DRAW, Rendering::MATCH_PREVIEW & match_modifier);
        }

        if (commandListData.commandQueue & Rendering::MATCH_BINDING & match_modifier) {
            renderingBindingManager.UpdateTextureBindings(cmd_list, Rendering::CALL_DRAW, Rendering::MATCH_BINDING & match_modifier);
        }

        if (commandListData.commandQueue & Rendering::MATCH_EFFECT & match_modifier) {
            renderingEffectManager.RenderEffects(cmd_list, Rendering::CALL_DRAW, Rendering::MATCH_EFFECT & match_modifier);
        }
    }
}

static bool onDraw(command_list* cmd_list, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
    CheckDrawCall(cmd_list, Rendering::MATCH_PS | Rendering::MATCH_VS);

    return false;
}

static bool onDispatch(command_list* cmd_list, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) {
    CheckDrawCall(cmd_list, Rendering::MATCH_CS);

    return false;
}

static bool onDrawIndexed(command_list* cmd_list,
                          uint32_t index_count,
                          uint32_t instance_count,
                          uint32_t first_index,
                          int32_t vertex_offset,
                          uint32_t first_instance) {
    CheckDrawCall(cmd_list, Rendering::MATCH_PS | Rendering::MATCH_VS);

    return false;
}

static bool onDrawOrDispatchIndirect(command_list* cmd_list, indirect_command type, resource buffer, uint64_t offset, uint32_t draw_count, uint32_t stride) {
    switch (type) {
        case indirect_command::unknown:
            CheckDrawCall(cmd_list);
            break;
        case indirect_command::draw:
        case indirect_command::draw_indexed:
            CheckDrawCall(cmd_list, Rendering::MATCH_PS | Rendering::MATCH_VS);
            break;
        case indirect_command::dispatch:
            CheckDrawCall(cmd_list, Rendering::MATCH_CS);
            break;
    }

    return false;
}

/// <summary>
/// copied from Reshade
/// Returns the path to the module file identified by the specified <paramref name="module"/> handle.
/// </summary>
filesystem::path getModulePath(HMODULE module) {
    WCHAR buf[4096];
    return GetModuleFileNameW(module, buf, ARRAYSIZE(buf)) ? buf : filesystem::path();
}

// FHX Restoration / 32-bit DXVK/Vulkan diagnostic mode.
// Keeps upstream REST resource/descriptor/constant systems disabled. Adds a
// draw-order profiler and an OPTIONAL one-shot mid-frame ReShade injection test.
static std::mutex g_fhxMutex;
static std::unordered_set<uint32_t> g_fhxSeenPixelShaders;
static std::unordered_set<uint32_t> g_fhxSeenVertexShaders;
static std::unordered_set<uint32_t> g_fhxSeenComputeShaders;

static uint32_t g_fhxSelectedPixelHash = 0;
static uint32_t g_fhxSelectedVertexHash = 0;
static bool g_fhxBlockSelectedPixel = false;
static bool g_fhxBlockSelectedVertex = false;

struct FHXShaderFrameStat {
    uint64_t firstDraw = 0;
    uint64_t lastDraw = 0;
    uint32_t count = 0;
};

enum class FHXInjectStatus : uint32_t {
    disabled,
    waiting,
    no_runtime,
    effects_disabled,
    no_rtv,
    bad_resource,
    size_mismatch,
    injected
};

static std::unordered_map<uint32_t, FHXShaderFrameStat> g_fhxCurrentPixelFrame;
static std::unordered_map<uint32_t, FHXShaderFrameStat> g_fhxLastPixelFrame;
static std::unordered_map<command_list *, resource_view> g_fhxCurrentRTV;
static effect_runtime *g_fhxRuntime = nullptr;
static uint64_t g_fhxCurrentDrawIndex = 0;
static uint64_t g_fhxProfiledFrame = 0;
static bool g_fhxFreezeProfiler = false;

static bool g_fhxInjectionEnabled = false;
static uint32_t g_fhxInjectionTrigger = 0xCF49F7D6u;
static bool g_fhxInjectedThisFrame = false;
static uint64_t g_fhxInjectionCount = 0;
static FHXInjectStatus g_fhxInjectStatus = FHXInjectStatus::disabled;
static thread_local bool g_fhxInsideEffectRender = false;

static const char *fhxInjectStatusText(FHXInjectStatus status)
{
    switch (status) {
        case FHXInjectStatus::disabled: return "disabled";
        case FHXInjectStatus::waiting: return "waiting for trigger";
        case FHXInjectStatus::no_runtime: return "trigger hit: no effect runtime";
        case FHXInjectStatus::effects_disabled: return "trigger hit: ReShade effects disabled";
        case FHXInjectStatus::no_rtv: return "trigger hit: no active render target";
        case FHXInjectStatus::bad_resource: return "trigger hit: render target resource unavailable";
        case FHXInjectStatus::size_mismatch: return "trigger hit: render target is not output-sized";
        case FHXInjectStatus::injected: return "ReShade injected before trigger shader";
        default: return "unknown";
    }
}

static void onBindPipelineFHX(command_list *commandList, pipeline_stage stages, pipeline pipelineHandle)
{
    if (g_fhxInsideEffectRender || commandList == nullptr || pipelineHandle.handle == 0)
        return;

    CommandListDataContainer &data = commandList->get_private_data<CommandListDataContainer>();

    uint32_t pixelHash = 0;
    uint32_t vertexHash = 0;
    uint32_t computeHash = 0;

    if ((uint32_t)(stages & pipeline_stage::pixel_shader))
        pixelHash = g_pixelShaderManager.safeGetShaderHash(pipelineHandle.handle);
    if ((uint32_t)(stages & pipeline_stage::vertex_shader))
        vertexHash = g_vertexShaderManager.safeGetShaderHash(pipelineHandle.handle);
    if ((uint32_t)(stages & pipeline_stage::compute_shader))
        computeHash = g_computeShaderManager.safeGetShaderHash(pipelineHandle.handle);

    if (pixelHash != 0)
        data.ps.activeShaderHash = pixelHash;
    if (vertexHash != 0)
        data.vs.activeShaderHash = vertexHash;
    if (computeHash != 0)
        data.cs.activeShaderHash = computeHash;

    if (pixelHash == 0 && vertexHash == 0 && computeHash == 0)
        return;

    std::lock_guard<std::mutex> lock(g_fhxMutex);
    if (pixelHash != 0)
        g_fhxSeenPixelShaders.insert(pixelHash);
    if (vertexHash != 0)
        g_fhxSeenVertexShaders.insert(vertexHash);
    if (computeHash != 0)
        g_fhxSeenComputeShaders.insert(computeHash);
}

static bool shouldBlockCurrentDrawFHX(command_list *commandList)
{
    if (g_fhxInsideEffectRender || commandList == nullptr)
        return false;

    const CommandListDataContainer &data = commandList->get_private_data<CommandListDataContainer>();

    std::lock_guard<std::mutex> lock(g_fhxMutex);

    if (g_fhxBlockSelectedPixel &&
        g_fhxSelectedPixelHash != 0 &&
        data.ps.activeShaderHash == g_fhxSelectedPixelHash)
        return true;

    if (g_fhxBlockSelectedVertex &&
        g_fhxSelectedVertexHash != 0 &&
        data.vs.activeShaderHash == g_fhxSelectedVertexHash)
        return true;

    return false;
}

static void onInitEffectRuntimeFHX(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_fhxMutex);
    g_fhxRuntime = runtime;
}

static void onDestroyEffectRuntimeFHX(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_fhxMutex);
    if (g_fhxRuntime == runtime)
        g_fhxRuntime = nullptr;
}

static void onBindRenderTargetsFHX(command_list *cmdList, uint32_t count, const resource_view *rtvs, resource_view)
{
    if (g_fhxInsideEffectRender || cmdList == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_fhxMutex);
    g_fhxCurrentRTV[cmdList] = (count != 0 && rtvs != nullptr) ? rtvs[0] : resource_view { 0 };
}

static void onBeginRenderPassFHX(command_list *cmdList,
                                 uint32_t count,
                                 const render_pass_render_target_desc *rts,
                                 const render_pass_depth_stencil_desc *)
{
    if (!g_fhxInsideEffectRender && cmdList != nullptr) {
        std::lock_guard<std::mutex> lock(g_fhxMutex);
        g_fhxCurrentRTV[cmdList] = (count != 0 && rts != nullptr) ? rts[0].view : resource_view { 0 };
    }
}

static void onEndRenderPassFHX(command_list *cmdList)
{
    if (!g_fhxInsideEffectRender && cmdList != nullptr) {
        std::lock_guard<std::mutex> lock(g_fhxMutex);
        g_fhxCurrentRTV[cmdList] = resource_view { 0 };
    }
}

static void onResetCommandListFHX(command_list *cmdList)
{
    if (cmdList == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_fhxMutex);
    g_fhxCurrentRTV.erase(cmdList);
}

static void onDestroyCommandListFHX(command_list *cmdList)
{
    if (cmdList == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_fhxMutex);
    g_fhxCurrentRTV.erase(cmdList);
}

static void profileAndMaybeInjectFHX(command_list *cmdList)
{
    if (g_fhxInsideEffectRender || cmdList == nullptr)
        return;

    const CommandListDataContainer &data = cmdList->get_private_data<CommandListDataContainer>();
    const uint32_t pixelHash = data.ps.activeShaderHash;
    if (pixelHash == 0)
        return;

    effect_runtime *runtime = nullptr;
    resource_view rtv = { 0 };
    bool tryInjection = false;

    {
        std::lock_guard<std::mutex> lock(g_fhxMutex);

        const uint64_t drawIndex = ++g_fhxCurrentDrawIndex;
        FHXShaderFrameStat &stat = g_fhxCurrentPixelFrame[pixelHash];
        if (stat.count == 0)
            stat.firstDraw = drawIndex;
        stat.lastDraw = drawIndex;
        ++stat.count;

        if (g_fhxInjectionEnabled && !g_fhxInjectedThisFrame && pixelHash == g_fhxInjectionTrigger) {
            runtime = g_fhxRuntime;
            const auto it = g_fhxCurrentRTV.find(cmdList);
            if (it != g_fhxCurrentRTV.end())
                rtv = it->second;
            tryInjection = true;
        }
    }

    if (!tryInjection)
        return;

    if (runtime == nullptr) {
        std::lock_guard<std::mutex> lock(g_fhxMutex);
        g_fhxInjectStatus = FHXInjectStatus::no_runtime;
        return;
    }
    if (!runtime->get_effects_state()) {
        std::lock_guard<std::mutex> lock(g_fhxMutex);
        g_fhxInjectStatus = FHXInjectStatus::effects_disabled;
        return;
    }
    if (rtv == 0) {
        std::lock_guard<std::mutex> lock(g_fhxMutex);
        g_fhxInjectStatus = FHXInjectStatus::no_rtv;
        return;
    }

    device *dev = cmdList->get_device();
    const resource target = (dev != nullptr) ? dev->get_resource_from_view(rtv) : resource { 0 };
    if (dev == nullptr || target.handle == 0) {
        std::lock_guard<std::mutex> lock(g_fhxMutex);
        g_fhxInjectStatus = FHXInjectStatus::bad_resource;
        return;
    }

    uint32_t width = 0, height = 0;
    runtime->get_screenshot_width_and_height(&width, &height);
    const resource_desc desc = dev->get_resource_desc(target);

    if (desc.texture.width != width || desc.texture.height != height) {
        std::lock_guard<std::mutex> lock(g_fhxMutex);
        g_fhxInjectStatus = FHXInjectStatus::size_mismatch;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_fhxMutex);
        if (g_fhxInjectedThisFrame)
            return;
        g_fhxInjectedThisFrame = true;
        ++g_fhxInjectionCount;
        g_fhxInjectStatus = FHXInjectStatus::injected;
    }

    // ReShade 6.x captures/restores application API state for a mid-frame
    // render_effects call. FHX currently presents an UNORM Vulkan target, so
    // the same view is used for linear and sRGB in this diagnostic build.
    g_fhxInsideEffectRender = true;
    runtime->render_effects(cmdList, rtv, rtv);
    g_fhxInsideEffectRender = false;
}

static bool onDrawFHX(command_list *commandList, uint32_t, uint32_t, uint32_t, uint32_t)
{
    if (g_fhxInsideEffectRender)
        return false;

    profileAndMaybeInjectFHX(commandList);
    return shouldBlockCurrentDrawFHX(commandList);
}

static bool onDrawIndexedFHX(command_list *commandList, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
{
    if (g_fhxInsideEffectRender)
        return false;

    profileAndMaybeInjectFHX(commandList);
    return shouldBlockCurrentDrawFHX(commandList);
}

static void onReshadePresentFHX(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_fhxMutex);

    if (g_fhxRuntime != nullptr && runtime != g_fhxRuntime)
        return;

    if (!g_fhxFreezeProfiler) {
        g_fhxLastPixelFrame = g_fhxCurrentPixelFrame;
        ++g_fhxProfiledFrame;
    }

    g_fhxCurrentPixelFrame.clear();
    g_fhxCurrentDrawIndex = 0;
    g_fhxInjectedThisFrame = false;
    g_fhxInjectStatus = g_fhxInjectionEnabled ? FHXInjectStatus::waiting : FHXInjectStatus::disabled;
}

static void displayFHXHuntOverlay(effect_runtime *)
{
    std::vector<uint32_t> pixelHashes;
    std::vector<uint32_t> vertexHashes;
    std::vector<std::pair<uint32_t, FHXShaderFrameStat>> frameStats;
    size_t computeCount = 0;
    uint64_t profiledFrame = 0;
    uint64_t injectionCount = 0;
    FHXInjectStatus injectStatus = FHXInjectStatus::disabled;

    {
        std::lock_guard<std::mutex> lock(g_fhxMutex);
        pixelHashes.assign(g_fhxSeenPixelShaders.begin(), g_fhxSeenPixelShaders.end());
        vertexHashes.assign(g_fhxSeenVertexShaders.begin(), g_fhxSeenVertexShaders.end());
        computeCount = g_fhxSeenComputeShaders.size();
        frameStats.reserve(g_fhxLastPixelFrame.size());
        for (const auto &entry : g_fhxLastPixelFrame)
            frameStats.push_back(entry);
        profiledFrame = g_fhxProfiledFrame;
        injectionCount = g_fhxInjectionCount;
        injectStatus = g_fhxInjectStatus;
    }

    std::sort(pixelHashes.begin(), pixelHashes.end());
    std::sort(vertexHashes.begin(), vertexHashes.end());
    std::sort(frameStats.begin(), frameStats.end(),
              [](const auto &a, const auto &b) { return a.second.firstDraw < b.second.firstDraw; });

    auto indexOf = [](const std::vector<uint32_t> &values, uint32_t value) -> int {
        if (values.empty() || value == 0)
            return -1;
        const auto it = std::lower_bound(values.begin(), values.end(), value);
        return (it != values.end() && *it == value) ? static_cast<int>(std::distance(values.begin(), it)) : -1;
    };

    ImGui::TextUnformatted("FHX 32-bit DXVK/Vulkan Shader Hunter");
    ImGui::Separator();
    ImGui::Text("Seen pixel shaders: %zu", pixelHashes.size());
    ImGui::Text("Seen vertex shaders: %zu", vertexHashes.size());
    ImGui::Text("Seen compute shaders: %zu", computeCount);
    ImGui::Spacing();

    int pixelIndex = indexOf(pixelHashes, g_fhxSelectedPixelHash);
    ImGui::Text("Selected pixel shader: %u (0x%08X)", g_fhxSelectedPixelHash, g_fhxSelectedPixelHash);
    ImGui::SameLine();
    ImGui::TextDisabled("[%d / %zu]", pixelIndex >= 0 ? pixelIndex + 1 : 0, pixelHashes.size());

    if (ImGui::Button("Previous Pixel") && !pixelHashes.empty()) {
        const int next = pixelIndex <= 0 ? static_cast<int>(pixelHashes.size()) - 1 : pixelIndex - 1;
        g_fhxSelectedPixelHash = pixelHashes[next];
    }
    ImGui::SameLine();
    if (ImGui::Button("Next Pixel") && !pixelHashes.empty()) {
        const int next = (pixelIndex < 0 || pixelIndex + 1 >= static_cast<int>(pixelHashes.size())) ? 0 : pixelIndex + 1;
        g_fhxSelectedPixelHash = pixelHashes[next];
    }
    ImGui::Checkbox("Suppress draws using selected pixel shader", &g_fhxBlockSelectedPixel);

    ImGui::Spacing();

    int vertexIndex = indexOf(vertexHashes, g_fhxSelectedVertexHash);
    ImGui::Text("Selected vertex shader: %u (0x%08X)", g_fhxSelectedVertexHash, g_fhxSelectedVertexHash);
    ImGui::SameLine();
    ImGui::TextDisabled("[%d / %zu]", vertexIndex >= 0 ? vertexIndex + 1 : 0, vertexHashes.size());

    if (ImGui::Button("Previous Vertex") && !vertexHashes.empty()) {
        const int next = vertexIndex <= 0 ? static_cast<int>(vertexHashes.size()) - 1 : vertexIndex - 1;
        g_fhxSelectedVertexHash = vertexHashes[next];
    }
    ImGui::SameLine();
    if (ImGui::Button("Next Vertex") && !vertexHashes.empty()) {
        const int next = (vertexIndex < 0 || vertexIndex + 1 >= static_cast<int>(vertexHashes.size())) ? 0 : vertexIndex + 1;
        g_fhxSelectedVertexHash = vertexHashes[next];
    }
    ImGui::Checkbox("Suppress draws using selected vertex shader", &g_fhxBlockSelectedVertex);

    ImGui::Spacing();
    if (ImGui::Button("Use selected pixel as injection trigger") && g_fhxSelectedPixelHash != 0)
        g_fhxInjectionTrigger = g_fhxSelectedPixelHash;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Per-frame pixel shader order");
    ImGui::Checkbox("Freeze profiler snapshot", &g_fhxFreezeProfiler);
    ImGui::Text("Snapshot frame: %llu", static_cast<unsigned long long>(profiledFrame));
    ImGui::TextUnformatted("Hash        First   Last    Count   Note");

    for (const auto &[hash, stat] : frameStats) {
        const char *note = "";
        switch (hash) {
            case 0xCF49F7D6u: note = "primary UI"; break;
            case 0x30B96240u: note = "bars"; break;
            case 0xAFB6F656u: note = "minimap"; break;
            case 0xBBB19C94u: note = "minimap"; break;
            case 0xDEA3862Du: note = "mixed world/UI"; break;
            case 0x64787F0Fu: note = "post-process"; break;
            default: break;
        }

        ImGui::Text("0x%08X  %6llu  %6llu  %6u   %s",
                    hash,
                    static_cast<unsigned long long>(stat.firstDraw),
                    static_cast<unsigned long long>(stat.lastDraw),
                    stat.count,
                    note);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("One-shot mid-frame ReShade test");
    ImGui::Text("Trigger hash: 0x%08X", g_fhxInjectionTrigger);
    ImGui::Checkbox("Enable injection before trigger shader", &g_fhxInjectionEnabled);
    ImGui::Text("Injection count: %llu", static_cast<unsigned long long>(injectionCount));
    ImGui::Text("Status: %s", fhxInjectStatusText(injectStatus));
    ImGui::TextWrapped(
        "OFF by default. When enabled, ReShade is rendered once at the first output-sized draw "
        "using the trigger pixel shader. Upstream REST resource, descriptor, texture-binding, "
        "constant-copy and preview systems remain disabled.");

    ImGui::Spacing();
    if (ImGui::Button("Clear observed shader list")) {
        std::lock_guard<std::mutex> lock(g_fhxMutex);
        g_fhxSeenPixelShaders.clear();
        g_fhxSeenVertexShaders.clear();
        g_fhxSeenComputeShaders.clear();
        g_fhxSelectedPixelHash = 0;
        g_fhxSelectedVertexHash = 0;
        g_fhxBlockSelectedPixel = false;
        g_fhxBlockSelectedVertex = false;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(hModule))
                return FALSE;

            g_dllPath = getModulePath(hModule);

            reshade::register_event<reshade::addon_event::init_pipeline>(onInitPipeline);
            reshade::register_event<reshade::addon_event::destroy_pipeline>(onDestroyPipeline);

            reshade::register_event<reshade::addon_event::init_command_list>(onInitCommandList);
            reshade::register_event<reshade::addon_event::destroy_command_list>(onDestroyCommandList);
            reshade::register_event<reshade::addon_event::destroy_command_list>(onDestroyCommandListFHX);
            reshade::register_event<reshade::addon_event::reset_command_list>(onResetCommandList);
            reshade::register_event<reshade::addon_event::reset_command_list>(onResetCommandListFHX);
            reshade::register_event<reshade::addon_event::bind_pipeline>(onBindPipelineFHX);

            reshade::register_event<reshade::addon_event::init_effect_runtime>(onInitEffectRuntimeFHX);
            reshade::register_event<reshade::addon_event::destroy_effect_runtime>(onDestroyEffectRuntimeFHX);
            reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(onBindRenderTargetsFHX);
            reshade::register_event<reshade::addon_event::begin_render_pass>(onBeginRenderPassFHX);
            reshade::register_event<reshade::addon_event::end_render_pass>(onEndRenderPassFHX);

            reshade::register_event<reshade::addon_event::draw>(onDrawFHX);
            reshade::register_event<reshade::addon_event::draw_indexed>(onDrawIndexedFHX);
            reshade::register_event<reshade::addon_event::reshade_present>(onReshadePresentFHX);

            reshade::register_overlay("REST FHX Debug", &displayFHXHuntOverlay);
            break;

        case DLL_PROCESS_DETACH:
            reshade::unregister_event<reshade::addon_event::reshade_present>(onReshadePresentFHX);
            reshade::unregister_event<reshade::addon_event::draw_indexed>(onDrawIndexedFHX);
            reshade::unregister_event<reshade::addon_event::draw>(onDrawFHX);

            reshade::unregister_event<reshade::addon_event::end_render_pass>(onEndRenderPassFHX);
            reshade::unregister_event<reshade::addon_event::begin_render_pass>(onBeginRenderPassFHX);
            reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(onBindRenderTargetsFHX);
            reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(onDestroyEffectRuntimeFHX);
            reshade::unregister_event<reshade::addon_event::init_effect_runtime>(onInitEffectRuntimeFHX);

            reshade::unregister_event<reshade::addon_event::bind_pipeline>(onBindPipelineFHX);
            reshade::unregister_event<reshade::addon_event::reset_command_list>(onResetCommandListFHX);
            reshade::unregister_event<reshade::addon_event::reset_command_list>(onResetCommandList);
            reshade::unregister_event<reshade::addon_event::destroy_command_list>(onDestroyCommandListFHX);
            reshade::unregister_event<reshade::addon_event::destroy_command_list>(onDestroyCommandList);
            reshade::unregister_event<reshade::addon_event::init_command_list>(onInitCommandList);

            reshade::unregister_event<reshade::addon_event::destroy_pipeline>(onDestroyPipeline);
            reshade::unregister_event<reshade::addon_event::init_pipeline>(onInitPipeline);
            reshade::unregister_overlay("REST FHX Debug", &displayFHXHuntOverlay);
            reshade::unregister_addon(hModule);
            break;
    }

    return TRUE;
}
