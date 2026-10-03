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
#include <cstring>
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

// FHX Restoration / 32-bit DXVK/Vulkan shader-hunting diagnostic mode.
//
// This intentionally stays independent from REST's resource/descriptor/render-target
// machinery. It tracks shaders exposed by ReShade's Vulkan API, tracks the currently
// bound shader on each command list, and can suppress draws matching one selected
// pixel or vertex shader. This is enough to identify FHX UI shaders without enabling
// the subsystems implicated in the upstream startup crash.
static std::mutex g_fhxHuntMutex;
static std::unordered_set<uint32_t> g_fhxSeenPixelShaders;
static std::unordered_set<uint32_t> g_fhxSeenVertexShaders;
static std::unordered_set<uint32_t> g_fhxSeenComputeShaders;

static uint32_t g_fhxSelectedPixelHash = 0;
static uint32_t g_fhxSelectedVertexHash = 0;
static bool g_fhxBlockSelectedPixel = false;
static bool g_fhxBlockSelectedVertex = false;

struct FHXRenderTargetInfo
{
    uint64_t viewHandle = 0;
    uint64_t resourceHandle = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t formatValue = 0;
    uint16_t samples = 0;
    bool valid = false;
};

struct FHXShaderFrameStat
{
    uint64_t firstDraw = 0;
    uint64_t lastDraw = 0;
    uint32_t count = 0;
    uint32_t firstPass = 0;
    uint32_t lastPass = 0;
    uint32_t firstRT = 0;
    uint32_t lastRT = 0;
    uint32_t rtIds[4] = {};
    uint32_t rtCount = 0;
    bool rtOverflow = false;
};

struct FHXShaderRTStat
{
    uint32_t shaderHash = 0;
    uint32_t rtId = 0;
    uint64_t firstDraw = 0;
    uint64_t lastDraw = 0;
    uint32_t count = 0;
    uint32_t firstPass = 0;
    uint32_t lastPass = 0;
};

static std::unordered_map<uint32_t, FHXShaderFrameStat> g_fhxCurrentPixelFrame;
static std::unordered_map<uint32_t, FHXShaderFrameStat> g_fhxLastPixelFrame;
static std::unordered_map<uint64_t, FHXShaderRTStat> g_fhxCurrentShaderRTFrame;
static std::unordered_map<uint64_t, FHXShaderRTStat> g_fhxLastShaderRTFrame;
static std::unordered_map<command_list *, uint32_t> g_fhxActiveRenderPass;
static std::unordered_map<command_list *, FHXRenderTargetInfo> g_fhxCurrentRenderTarget;
static std::vector<FHXRenderTargetInfo> g_fhxCurrentRTCatalog;
static std::vector<FHXRenderTargetInfo> g_fhxLastRTCatalog;
static uint64_t g_fhxCurrentDrawIndex = 0;
static uint64_t g_fhxProfiledFrame = 0;
static uint32_t g_fhxNextRenderPass = 0;
static bool g_fhxFreezeProfiler = false;

enum class FHXBoundaryInjectStatus : uint32_t
{
    disabled,
    waiting_for_ui_prepass,
    waiting_for_candidate,
    candidate_detected,
    armed,
    no_runtime,
    effects_disabled,
    bad_target,
    injected
};

static FHXRenderTargetInfo inspectRenderTargetFHX(command_list *commandList, resource_view view);

static effect_runtime *g_fhxRuntime = nullptr;
static uint32_t g_fhxOutputWidth = 0;
static uint32_t g_fhxOutputHeight = 0;

enum class FHXBoundaryTeacher : uint32_t
{
    primary_ui = 0,
    bars_fullres,
    minimap,
    count
};

struct FHXPassSignature
{
    uint32_t colorCount = 0;
    uint32_t colorFormat = 0;
    uint32_t colorWidth = 0;
    uint32_t colorHeight = 0;
    uint16_t colorSamples = 0;
    uint32_t colorLoadOp = 0;
    uint32_t colorStoreOp = 0;

    bool hasDepth = false;
    uint32_t depthFormat = 0;
    uint32_t depthWidth = 0;
    uint32_t depthHeight = 0;
    uint16_t depthSamples = 0;
    uint32_t depthLoadOp = 0;
    uint32_t depthStoreOp = 0;
    uint32_t stencilLoadOp = 0;
    uint32_t stencilStoreOp = 0;

    bool valid = false;
};

static constexpr uint32_t kFHXSignatureStableFramesRequired = 20;
static constexpr uint32_t kFHXValidationPresentsRequired = 60;
static constexpr uint32_t kFHXWarmupSettledPresents = 30;

static uint32_t g_fhxSelectedBoundaryTeacher = static_cast<uint32_t>(FHXBoundaryTeacher::primary_ui);

static std::unordered_map<command_list *, FHXPassSignature> g_fhxCurrentPassSignature;
static std::unordered_map<command_list *, resource_view> g_fhxCurrentColorView;
static std::unordered_map<uint64_t, uint32_t> g_fhxCurrentSignatureCounts;

// Global pass numbers move between frames/command buffers in FHX.  Instead,
// learn the teacher pass by its structural signature plus its occurrence on
// that command list and the immediately preceding structural signature.
static std::unordered_map<command_list *, std::unordered_map<uint64_t, uint32_t>> g_fhxLocalSignatureCounts;
static std::unordered_map<command_list *, uint32_t> g_fhxCurrentPassLocalOccurrence;
static std::unordered_map<command_list *, uint64_t> g_fhxCurrentPassPreviousSignatureKey;
static std::unordered_map<command_list *, uint64_t> g_fhxLastPassSignatureKey;

static FHXPassSignature g_fhxObservedTeacherSignature = {};
static FHXPassSignature g_fhxLearnedSignature = {};
static uint64_t g_fhxObservedTeacherSignatureKey = 0;
static uint64_t g_fhxLearnedSignatureKey = 0;
static uint32_t g_fhxObservedTeacherLocalOccurrence = 0;
static uint32_t g_fhxLearnedTeacherLocalOccurrence = 0;
static uint64_t g_fhxObservedTeacherPreviousSignatureKey = 0;
static uint64_t g_fhxLearnedTeacherPreviousSignatureKey = 0;
static uint32_t g_fhxSignatureStableFrames = 0;
static uint32_t g_fhxLastSignatureOccurrences = 0;
static uint64_t g_fhxSignatureChanges = 0;
static uint64_t g_fhxTeacherObservations = 0;
static uint64_t g_fhxSafeBoundaryHits = 0;

// Before allowing any ReShade injection, validate the learned contextual
// boundary for 60 presents with zero ReShade work.
static bool g_fhxValidationActive = false;
static bool g_fhxValidationAttempted = false;
static bool g_fhxBoundaryValidated = false;
static uint64_t g_fhxValidationStartSerial = 0;
static uint64_t g_fhxValidationLastHitSerial = UINT64_MAX;
static uint32_t g_fhxValidationUniqueHitFrames = 0;
static uint32_t g_fhxValidationDuplicateHits = 0;
static uint32_t g_fhxValidationMissedFrames = 0;

static uint32_t g_fhxRequestedInjectionFrames = 0;
static uint32_t g_fhxRemainingInjectionFrames = 0;
static bool g_fhxContinuousInjection = false;
static bool g_fhxWarmupPending = false;
static bool g_fhxWarmupIssued = false;
static bool g_fhxWarmupReady = false;
static uint32_t g_fhxWarmupQuietPresents = 0;
static uint64_t g_fhxWarmupReloadEvents = 0;
static uint64_t g_fhxWarmupCount = 0;

static uint64_t g_fhxPresentSerial = 0;
static uint64_t g_fhxLastInjectionPresentSerial = UINT64_MAX;
static uint64_t g_fhxInjectionCount = 0;
static uint64_t g_fhxSkippedAmbiguous = 0;
static uint64_t g_fhxSkippedUnstable = 0;

static FHXBoundaryInjectStatus g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::disabled;
static thread_local bool g_fhxInsideEffectRender = false;

static const char *fhxBoundaryStatusText(FHXBoundaryInjectStatus status)
{
    switch (status)
    {
        case FHXBoundaryInjectStatus::disabled: return "learning contextual UI-pass fingerprint";
        case FHXBoundaryInjectStatus::waiting_for_ui_prepass: return "dry-run validating learned UI boundary";
        case FHXBoundaryInjectStatus::waiting_for_candidate: return "warm-up settling";
        case FHXBoundaryInjectStatus::candidate_detected: return "validated contextual pre-UI boundary ready";
        case FHXBoundaryInjectStatus::armed: return "test armed";
        case FHXBoundaryInjectStatus::no_runtime: return "no ReShade runtime";
        case FHXBoundaryInjectStatus::effects_disabled: return "ReShade effects disabled";
        case FHXBoundaryInjectStatus::bad_target: return "boundary target rejected";
        case FHXBoundaryInjectStatus::injected: return "test running";
        default: return "unknown";
    }
}

static const char *fhxBoundaryTeacherName(uint32_t teacher)
{
    switch (static_cast<FHXBoundaryTeacher>(teacher))
    {
        case FHXBoundaryTeacher::primary_ui: return "Primary UI 0xCF49F7D6";
        case FHXBoundaryTeacher::bars_fullres: return "Bars/full-res 0x30B96240";
        case FHXBoundaryTeacher::minimap: return "Minimap 0xAFB6F656";
        default: return "Unknown";
    }
}

static uint32_t fhxBoundaryTeacherHash(uint32_t teacher)
{
    switch (static_cast<FHXBoundaryTeacher>(teacher))
    {
        case FHXBoundaryTeacher::primary_ui: return 0xCF49F7D6u;
        case FHXBoundaryTeacher::bars_fullres: return 0x30B96240u;
        case FHXBoundaryTeacher::minimap: return 0xAFB6F656u;
        default: return 0;
    }
}

static bool sameFHXPassSignature(const FHXPassSignature &a, const FHXPassSignature &b)
{
    return a.valid && b.valid &&
           a.colorCount == b.colorCount &&
           a.colorFormat == b.colorFormat &&
           a.colorWidth == b.colorWidth &&
           a.colorHeight == b.colorHeight &&
           a.colorSamples == b.colorSamples &&
           a.colorLoadOp == b.colorLoadOp &&
           a.colorStoreOp == b.colorStoreOp &&
           a.hasDepth == b.hasDepth &&
           a.depthFormat == b.depthFormat &&
           a.depthWidth == b.depthWidth &&
           a.depthHeight == b.depthHeight &&
           a.depthSamples == b.depthSamples &&
           a.depthLoadOp == b.depthLoadOp &&
           a.depthStoreOp == b.depthStoreOp &&
           a.stencilLoadOp == b.stencilLoadOp &&
           a.stencilStoreOp == b.stencilStoreOp;
}

static uint64_t hashFHXPassSignature(const FHXPassSignature &sig)
{
    if (!sig.valid)
        return 0;

    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };

    mix(sig.colorCount);
    mix(sig.colorFormat);
    mix(sig.colorWidth);
    mix(sig.colorHeight);
    mix(sig.colorSamples);
    mix(sig.colorLoadOp);
    mix(sig.colorStoreOp);
    mix(sig.hasDepth ? 1u : 0u);
    mix(sig.depthFormat);
    mix(sig.depthWidth);
    mix(sig.depthHeight);
    mix(sig.depthSamples);
    mix(sig.depthLoadOp);
    mix(sig.depthStoreOp);
    mix(sig.stencilLoadOp);
    mix(sig.stencilStoreOp);
    return h;
}

static FHXPassSignature buildFHXPassSignature(
    command_list *commandList,
    uint32_t count,
    const render_pass_render_target_desc *rts,
    const render_pass_depth_stencil_desc *ds)
{
    FHXPassSignature sig;
    if (commandList == nullptr || count == 0 || rts == nullptr || rts[0].view.handle == 0)
        return sig;

    const FHXRenderTargetInfo color = inspectRenderTargetFHX(commandList, rts[0].view);
    if (!color.valid)
        return sig;

    sig.colorCount = count;
    sig.colorFormat = color.formatValue;
    sig.colorWidth = color.width;
    sig.colorHeight = color.height;
    sig.colorSamples = color.samples;
    sig.colorLoadOp = static_cast<uint32_t>(rts[0].load_op);
    sig.colorStoreOp = static_cast<uint32_t>(rts[0].store_op);

    if (ds != nullptr && ds->view.handle != 0)
    {
        const FHXRenderTargetInfo depth = inspectRenderTargetFHX(commandList, ds->view);
        if (depth.valid)
        {
            sig.hasDepth = true;
            sig.depthFormat = depth.formatValue;
            sig.depthWidth = depth.width;
            sig.depthHeight = depth.height;
            sig.depthSamples = depth.samples;
            sig.depthLoadOp = static_cast<uint32_t>(ds->depth_load_op);
            sig.depthStoreOp = static_cast<uint32_t>(ds->depth_store_op);
            sig.stencilLoadOp = static_cast<uint32_t>(ds->stencil_load_op);
            sig.stencilStoreOp = static_cast<uint32_t>(ds->stencil_store_op);
        }
    }

    sig.valid = true;
    return sig;
}

static bool isFullResolutionMainTargetFHX(const FHXRenderTargetInfo &rt)
{
    if (!rt.valid || g_fhxOutputWidth == 0 || g_fhxOutputHeight == 0)
        return false;

    return rt.width == g_fhxOutputWidth &&
           rt.height == g_fhxOutputHeight &&
           static_cast<reshade::api::format>(rt.formatValue) == reshade::api::format::b8g8r8a8_unorm;
}

static bool learnedFHXBoundaryContextStable()
{
    return g_fhxLearnedSignature.valid &&
           g_fhxLearnedSignatureKey != 0 &&
           g_fhxLearnedTeacherLocalOccurrence != 0 &&
           g_fhxSignatureStableFrames >= kFHXSignatureStableFramesRequired;
}

static bool learnedFHXBoundaryReady()
{
    return learnedFHXBoundaryContextStable() && g_fhxBoundaryValidated;
}

static void resetFHXValidation(bool clearAttempt)
{
    g_fhxValidationActive = false;
    g_fhxBoundaryValidated = false;
    g_fhxValidationStartSerial = 0;
    g_fhxValidationLastHitSerial = UINT64_MAX;
    g_fhxValidationUniqueHitFrames = 0;
    g_fhxValidationDuplicateHits = 0;
    g_fhxValidationMissedFrames = 0;
    if (clearAttempt)
        g_fhxValidationAttempted = false;
}

static void startFHXValidation()
{
    resetFHXValidation(false);
    g_fhxValidationActive = true;
    g_fhxValidationAttempted = true;
    g_fhxValidationStartSerial = g_fhxPresentSerial;
    g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::waiting_for_ui_prepass;
    reshade::log::message(
        reshade::log::level::info,
        "[REST FHX] starting 60-present dry-run validation of contextual UI boundary");
}

static void resetFHXBoundaryTest()
{
    g_fhxRequestedInjectionFrames = 0;
    g_fhxRemainingInjectionFrames = 0;
    g_fhxContinuousInjection = false;
    g_fhxWarmupPending = false;
    g_fhxWarmupIssued = false;
    g_fhxWarmupReady = false;
    g_fhxWarmupQuietPresents = 0;
    g_fhxBoundaryInjectStatus = learnedFHXBoundaryReady()
        ? FHXBoundaryInjectStatus::candidate_detected
        : FHXBoundaryInjectStatus::disabled;
}

static void queueFHXInjectionTest(uint32_t frameCount)
{
    g_fhxRequestedInjectionFrames = frameCount;
    g_fhxRemainingInjectionFrames = frameCount;
    g_fhxContinuousInjection = false;
    g_fhxWarmupPending = true;
    g_fhxWarmupIssued = false;
    g_fhxWarmupReady = false;
    g_fhxWarmupQuietPresents = 0;
    g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::armed;

    const std::string msg = std::format(
        "[REST FHX] queued structural-boundary test: teacher='{}', signature=0x{:016X}, local-occurrence={}, previous=0x{:016X}, frames={}",
        fhxBoundaryTeacherName(g_fhxSelectedBoundaryTeacher),
        g_fhxLearnedSignatureKey,
        g_fhxLearnedTeacherLocalOccurrence,
        g_fhxLearnedTeacherPreviousSignatureKey,
        frameCount);
    reshade::log::message(reshade::log::level::info, msg.c_str());
}

static const char *fhxFormatName(uint32_t value)
{
    switch (static_cast<reshade::api::format>(value))
    {
        case reshade::api::format::r8g8b8a8_unorm: return "RGBA8_UNORM";
        case reshade::api::format::r8g8b8a8_unorm_srgb: return "RGBA8_SRGB";
        case reshade::api::format::b8g8r8a8_unorm: return "BGRA8_UNORM";
        case reshade::api::format::b8g8r8a8_unorm_srgb: return "BGRA8_SRGB";
        case reshade::api::format::r10g10b10a2_unorm: return "RGB10A2_UNORM";
        case reshade::api::format::r11g11b10_float: return "R11G11B10_FLOAT";
        case reshade::api::format::r16g16b16a16_float: return "RGBA16_FLOAT";
        default: return "other";
    }
}

static FHXRenderTargetInfo inspectRenderTargetFHX(command_list *commandList, resource_view view)
{
    FHXRenderTargetInfo info;
    info.viewHandle = view.handle;

    if (commandList == nullptr || view.handle == 0)
        return info;

    device *dev = commandList->get_device();
    if (dev == nullptr)
        return info;

    const resource resourceHandle = dev->get_resource_from_view(view);
    if (resourceHandle.handle == 0)
        return info;

    const resource_desc resourceDesc = dev->get_resource_desc(resourceHandle);
    const resource_view_desc viewDesc = dev->get_resource_view_desc(view);

    info.resourceHandle = resourceHandle.handle;
    info.width = resourceDesc.texture.width;
    info.height = resourceDesc.texture.height;
    info.samples = resourceDesc.texture.samples;
    info.formatValue = static_cast<uint32_t>(
        viewDesc.format != format::unknown ? viewDesc.format : resourceDesc.texture.format);
    info.valid = true;
    return info;
}

static bool sameRenderTargetFHX(const FHXRenderTargetInfo &a, const FHXRenderTargetInfo &b)
{
    return a.valid && b.valid &&
           a.viewHandle == b.viewHandle &&
           a.resourceHandle == b.resourceHandle &&
           a.width == b.width &&
           a.height == b.height &&
           a.formatValue == b.formatValue;
}

static uint32_t getRenderTargetIdFHX(const FHXRenderTargetInfo &info)
{
    if (!info.valid)
        return 0;

    for (size_t i = 0; i < g_fhxCurrentRTCatalog.size(); ++i)
        if (sameRenderTargetFHX(g_fhxCurrentRTCatalog[i], info))
            return static_cast<uint32_t>(i + 1);

    g_fhxCurrentRTCatalog.push_back(info);
    return static_cast<uint32_t>(g_fhxCurrentRTCatalog.size());
}

static void rememberShaderRenderTargetFHX(FHXShaderFrameStat &stat, uint32_t rtId)
{
    if (rtId == 0)
        return;

    for (uint32_t i = 0; i < stat.rtCount; ++i)
        if (stat.rtIds[i] == rtId)
            return;

    if (stat.rtCount < 4)
        stat.rtIds[stat.rtCount++] = rtId;
    else
        stat.rtOverflow = true;
}

static void onBeginRenderPassFHX(command_list *commandList,
                                 uint32_t count,
                                 const render_pass_render_target_desc *rts,
                                 const render_pass_depth_stencil_desc *ds)
{
    if (g_fhxInsideEffectRender || commandList == nullptr)
        return;

    const resource_view targetView =
        (count != 0 && rts != nullptr) ? rts[0].view : resource_view {};
    const FHXRenderTargetInfo rt =
        (targetView.handle != 0) ? inspectRenderTargetFHX(commandList, targetView) : FHXRenderTargetInfo {};
    const FHXPassSignature signature = buildFHXPassSignature(commandList, count, rts, ds);
    const uint64_t signatureKey = hashFHXPassSignature(signature);

    effect_runtime *runtime = nullptr;
    bool performWarmup = false;
    bool performInjection = false;

    {
        std::lock_guard<std::mutex> lock(g_fhxHuntMutex);

        const uint32_t passIndex = ++g_fhxNextRenderPass;
        g_fhxActiveRenderPass[commandList] = passIndex;
        g_fhxCurrentRenderTarget[commandList] = rt;
        g_fhxCurrentColorView[commandList] = targetView;
        g_fhxCurrentPassSignature[commandList] = signature;

        uint32_t localOccurrence = 0;
        uint64_t previousSignatureKey = 0;
        if (signatureKey != 0)
        {
            ++g_fhxCurrentSignatureCounts[signatureKey];
            localOccurrence = ++g_fhxLocalSignatureCounts[commandList][signatureKey];

            const auto previousIt = g_fhxLastPassSignatureKey.find(commandList);
            if (previousIt != g_fhxLastPassSignatureKey.end())
                previousSignatureKey = previousIt->second;

            g_fhxCurrentPassLocalOccurrence[commandList] = localOccurrence;
            g_fhxCurrentPassPreviousSignatureKey[commandList] = previousSignatureKey;
            g_fhxLastPassSignatureKey[commandList] = signatureKey;
        }

        const bool contextMatches =
            learnedFHXBoundaryContextStable() &&
            sameFHXPassSignature(signature, g_fhxLearnedSignature) &&
            localOccurrence == g_fhxLearnedTeacherLocalOccurrence &&
            previousSignatureKey == g_fhxLearnedTeacherPreviousSignatureKey;

        // Validation is intentionally passive: count whether this contextual
        // boundary resolves exactly once per presented frame, then return.
        if (g_fhxValidationActive && contextMatches)
        {
            if (g_fhxValidationLastHitSerial == g_fhxPresentSerial)
                ++g_fhxValidationDuplicateHits;
            else
            {
                g_fhxValidationLastHitSerial = g_fhxPresentSerial;
                ++g_fhxValidationUniqueHitFrames;
            }
        }

        const bool testRequested =
            g_fhxRemainingInjectionFrames != 0 || g_fhxContinuousInjection || g_fhxWarmupPending;

        if (!testRequested)
            return;

        if (!learnedFHXBoundaryReady())
        {
            ++g_fhxSkippedUnstable;
            return;
        }

        if (!contextMatches)
            return;

        ++g_fhxSafeBoundaryHits;

        // The add-on callback runs before ReShade forwards vkCmdBeginRenderPass,
        // so this is the safe side of the Vulkan render-pass boundary.
        if (g_fhxLastInjectionPresentSerial == g_fhxPresentSerial)
            return;

        if (!isFullResolutionMainTargetFHX(rt) || targetView.handle == 0)
        {
            g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::bad_target;
            return;
        }

        runtime = g_fhxRuntime;
        if (runtime == nullptr)
        {
            g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::no_runtime;
            return;
        }

        g_fhxLastInjectionPresentSerial = g_fhxPresentSerial;

        if (g_fhxWarmupPending && !g_fhxWarmupIssued)
        {
            g_fhxWarmupPending = false;
            g_fhxWarmupIssued = true;
            g_fhxWarmupReady = false;
            g_fhxWarmupQuietPresents = 0;
            performWarmup = true;
        }
        else if (g_fhxWarmupReady)
        {
            performInjection = true;
        }
    }

    if (runtime == nullptr)
        return;

    if (!runtime->get_effects_state())
    {
        std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
        g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::effects_disabled;
        return;
    }

    if (performWarmup)
    {
        reshade::log::message(
            reshade::log::level::info,
            "[REST FHX] safe-boundary warm-up begin");

        g_fhxInsideEffectRender = true;
        runtime->render_effects(commandList, targetView, targetView);
        g_fhxInsideEffectRender = false;

        std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
        ++g_fhxWarmupCount;
        g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::waiting_for_candidate;
        reshade::log::message(
            reshade::log::level::info,
            "[REST FHX] safe-boundary warm-up end");
        return;
    }

    if (!performInjection)
        return;

    reshade::log::message(
        reshade::log::level::info,
        "[REST FHX] safe-boundary render_effects begin");

    g_fhxInsideEffectRender = true;
    runtime->render_effects(commandList, targetView, targetView);
    g_fhxInsideEffectRender = false;

    {
        std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
        ++g_fhxInjectionCount;

        if (!g_fhxContinuousInjection && g_fhxRemainingInjectionFrames != 0)
            --g_fhxRemainingInjectionFrames;

        if (!g_fhxContinuousInjection && g_fhxRemainingInjectionFrames == 0)
        {
            g_fhxRequestedInjectionFrames = 0;
            g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::candidate_detected;
            reshade::log::message(
                reshade::log::level::info,
                "[REST FHX] finite structural-boundary test complete");
        }
        else
        {
            g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::injected;
        }
    }

    reshade::log::message(
        reshade::log::level::info,
        "[REST FHX] safe-boundary render_effects end");
}

static void onEndRenderPassFHX(command_list *commandList)
{
    if (g_fhxInsideEffectRender || commandList == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
    g_fhxActiveRenderPass[commandList] = 0;
    g_fhxCurrentRenderTarget[commandList] = FHXRenderTargetInfo {};
    g_fhxCurrentColorView[commandList] = resource_view {};
    g_fhxCurrentPassSignature[commandList] = FHXPassSignature {};
}

static void onBindRenderTargetsFHX(command_list *commandList,
                                   uint32_t count,
                                   const resource_view *rtvs,
                                   resource_view)
{
    if (g_fhxInsideEffectRender || commandList == nullptr)
        return;

    const FHXRenderTargetInfo rt =
        (count != 0 && rtvs != nullptr) ? inspectRenderTargetFHX(commandList, rtvs[0]) : FHXRenderTargetInfo {};

    std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
    g_fhxCurrentRenderTarget[commandList] = rt;
    g_fhxCurrentColorView[commandList] =
        (count != 0 && rtvs != nullptr) ? rtvs[0] : resource_view {};
}

static void onResetCommandListFHX(command_list *commandList)
{
    if (commandList == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
    g_fhxActiveRenderPass.erase(commandList);
    g_fhxCurrentRenderTarget.erase(commandList);
    g_fhxCurrentColorView.erase(commandList);
    g_fhxCurrentPassSignature.erase(commandList);
    g_fhxLocalSignatureCounts.erase(commandList);
    g_fhxCurrentPassLocalOccurrence.erase(commandList);
    g_fhxCurrentPassPreviousSignatureKey.erase(commandList);
    g_fhxLastPassSignatureKey.erase(commandList);
}

static void onDestroyCommandListFHX(command_list *commandList)
{
    if (commandList == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
    g_fhxActiveRenderPass.erase(commandList);
    g_fhxCurrentRenderTarget.erase(commandList);
    g_fhxCurrentColorView.erase(commandList);
    g_fhxCurrentPassSignature.erase(commandList);
    g_fhxLocalSignatureCounts.erase(commandList);
    g_fhxCurrentPassLocalOccurrence.erase(commandList);
    g_fhxCurrentPassPreviousSignatureKey.erase(commandList);
    g_fhxLastPassSignatureKey.erase(commandList);
}

static void onInitEffectRuntimeFHX(effect_runtime *runtime)
{
    if (runtime == nullptr)
        return;

    uint32_t width = 0, height = 0;
    runtime->get_screenshot_width_and_height(&width, &height);

    std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
    g_fhxRuntime = runtime;
    g_fhxOutputWidth = width;
    g_fhxOutputHeight = height;
}

static void onDestroyEffectRuntimeFHX(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
    if (g_fhxRuntime == runtime) {
        g_fhxRuntime = nullptr;
        g_fhxOutputWidth = 0;
        g_fhxOutputHeight = 0;
    }
}

static void onReshadeReloadedEffectsFHX(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_fhxHuntMutex);

    if (runtime != g_fhxRuntime || !g_fhxWarmupIssued || g_fhxWarmupReady)
        return;

    g_fhxWarmupQuietPresents = 0;
    ++g_fhxWarmupReloadEvents;
    reshade::log::message(
        reshade::log::level::info,
        "[REST FHX] effect reload observed during safe-boundary warm-up; settle counter reset");
}

static void profileCurrentDrawFHX(command_list *commandList)
{
    if (g_fhxInsideEffectRender || commandList == nullptr)
        return;

    const CommandListDataContainer &data = commandList->get_private_data<CommandListDataContainer>();
    const uint32_t pixelHash = data.ps.activeShaderHash;
    if (pixelHash == 0)
        return;

    std::lock_guard<std::mutex> lock(g_fhxHuntMutex);

    const uint64_t drawIndex = ++g_fhxCurrentDrawIndex;
    const auto passIt = g_fhxActiveRenderPass.find(commandList);
    const uint32_t passIndex = (passIt != g_fhxActiveRenderPass.end()) ? passIt->second : 0;

    FHXRenderTargetInfo rt;
    const auto rtIt = g_fhxCurrentRenderTarget.find(commandList);
    if (rtIt != g_fhxCurrentRenderTarget.end())
        rt = rtIt->second;
    const uint32_t rtId = getRenderTargetIdFHX(rt);

    if (pixelHash == fhxBoundaryTeacherHash(g_fhxSelectedBoundaryTeacher) &&
        isFullResolutionMainTargetFHX(rt))
    {
        const auto sigIt = g_fhxCurrentPassSignature.find(commandList);
        if (sigIt != g_fhxCurrentPassSignature.end() && sigIt->second.valid)
        {
            if (!g_fhxObservedTeacherSignature.valid)
            {
                g_fhxObservedTeacherSignature = sigIt->second;
                g_fhxObservedTeacherSignatureKey = hashFHXPassSignature(sigIt->second);

                const auto occurrenceIt = g_fhxCurrentPassLocalOccurrence.find(commandList);
                g_fhxObservedTeacherLocalOccurrence =
                    occurrenceIt != g_fhxCurrentPassLocalOccurrence.end() ? occurrenceIt->second : 0;

                const auto previousIt = g_fhxCurrentPassPreviousSignatureKey.find(commandList);
                g_fhxObservedTeacherPreviousSignatureKey =
                    previousIt != g_fhxCurrentPassPreviousSignatureKey.end() ? previousIt->second : 0;

                ++g_fhxTeacherObservations;
            }
        }
    }

    FHXShaderFrameStat &stat = g_fhxCurrentPixelFrame[pixelHash];
    if (stat.count == 0) {
        stat.firstDraw = drawIndex;
        stat.firstPass = passIndex;
        stat.firstRT = rtId;
    }

    stat.lastDraw = drawIndex;
    stat.lastPass = passIndex;
    stat.lastRT = rtId;
    rememberShaderRenderTargetFHX(stat, rtId);
    ++stat.count;

    if (rtId != 0) {
        const uint64_t pairKey = (static_cast<uint64_t>(pixelHash) << 32) | rtId;
        FHXShaderRTStat &rtStat = g_fhxCurrentShaderRTFrame[pairKey];
        if (rtStat.count == 0) {
            rtStat.shaderHash = pixelHash;
            rtStat.rtId = rtId;
            rtStat.firstDraw = drawIndex;
            rtStat.firstPass = passIndex;
        }

        rtStat.lastDraw = drawIndex;
        rtStat.lastPass = passIndex;
        ++rtStat.count;
    }
}

static void onReshadePresentFHX(effect_runtime *runtime)
{
    uint32_t width = 0, height = 0;
    if (runtime != nullptr)
        runtime->get_screenshot_width_and_height(&width, &height);

    std::lock_guard<std::mutex> lock(g_fhxHuntMutex);

    ++g_fhxPresentSerial;

    if (runtime != nullptr)
    {
        g_fhxRuntime = runtime;
        g_fhxOutputWidth = width;
        g_fhxOutputHeight = height;
    }

    if (!g_fhxFreezeProfiler)
    {
        g_fhxLastPixelFrame = g_fhxCurrentPixelFrame;
        g_fhxLastShaderRTFrame = g_fhxCurrentShaderRTFrame;
        g_fhxLastRTCatalog = g_fhxCurrentRTCatalog;
        ++g_fhxProfiledFrame;
    }

    if (g_fhxObservedTeacherSignature.valid &&
        g_fhxObservedTeacherSignatureKey != 0 &&
        g_fhxObservedTeacherLocalOccurrence != 0)
    {
        const auto countIt = g_fhxCurrentSignatureCounts.find(g_fhxObservedTeacherSignatureKey);
        const uint32_t occurrences =
            countIt != g_fhxCurrentSignatureCounts.end() ? countIt->second : 0;

        const bool sameBoundary =
            sameFHXPassSignature(g_fhxObservedTeacherSignature, g_fhxLearnedSignature) &&
            g_fhxObservedTeacherLocalOccurrence == g_fhxLearnedTeacherLocalOccurrence &&
            g_fhxObservedTeacherPreviousSignatureKey == g_fhxLearnedTeacherPreviousSignatureKey;

        if (sameBoundary)
        {
            if (g_fhxSignatureStableFrames < 0xFFFFFFFFu)
                ++g_fhxSignatureStableFrames;
        }
        else
        {
            if (g_fhxLearnedSignature.valid)
                ++g_fhxSignatureChanges;

            g_fhxLearnedSignature = g_fhxObservedTeacherSignature;
            g_fhxLearnedSignatureKey = g_fhxObservedTeacherSignatureKey;
            g_fhxLearnedTeacherLocalOccurrence = g_fhxObservedTeacherLocalOccurrence;
            g_fhxLearnedTeacherPreviousSignatureKey = g_fhxObservedTeacherPreviousSignatureKey;
            g_fhxSignatureStableFrames = 1;

            // Any learned-boundary change invalidates an earlier dry-run result.
            resetFHXValidation(true);
        }

        g_fhxLastSignatureOccurrences = occurrences;
    }

    // Automatic no-injection verification. This prevents another crash loop
    // from a false-positive structural match.
    if (g_fhxValidationActive)
    {
        const uint64_t elapsed =
            g_fhxPresentSerial >= g_fhxValidationStartSerial
            ? g_fhxPresentSerial - g_fhxValidationStartSerial
            : 0;

        if (elapsed >= kFHXValidationPresentsRequired)
        {
            g_fhxValidationActive = false;
            g_fhxValidationMissedFrames =
                g_fhxValidationUniqueHitFrames < kFHXValidationPresentsRequired
                ? kFHXValidationPresentsRequired - g_fhxValidationUniqueHitFrames
                : 0;

            g_fhxBoundaryValidated =
                g_fhxValidationUniqueHitFrames == kFHXValidationPresentsRequired &&
                g_fhxValidationDuplicateHits == 0;

            if (g_fhxBoundaryValidated)
            {
                g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::candidate_detected;
                reshade::log::message(
                    reshade::log::level::info,
                    "[REST FHX] contextual UI boundary passed 60-present dry-run validation");
            }
            else
            {
                g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::disabled;
                const std::string msg = std::format(
                    "[REST FHX] contextual UI boundary validation failed: hitFrames={}, missed={}, duplicates={}",
                    g_fhxValidationUniqueHitFrames,
                    g_fhxValidationMissedFrames,
                    g_fhxValidationDuplicateHits);
                reshade::log::message(reshade::log::level::warning, msg.c_str());
            }
        }
    }
    else if (learnedFHXBoundaryContextStable() &&
             !g_fhxBoundaryValidated &&
             !g_fhxValidationAttempted)
    {
        startFHXValidation();
    }

    if (g_fhxWarmupIssued && !g_fhxWarmupReady)
    {
        if (g_fhxWarmupQuietPresents < kFHXWarmupSettledPresents)
            ++g_fhxWarmupQuietPresents;

        if (g_fhxWarmupQuietPresents >= kFHXWarmupSettledPresents)
        {
            g_fhxWarmupReady = true;
            g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::candidate_detected;
            reshade::log::message(
                reshade::log::level::info,
                "[REST FHX] safe-boundary warm-up settled; requested test is live");
        }
        else
        {
            g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::waiting_for_candidate;
        }
    }
    else if (g_fhxRemainingInjectionFrames == 0 &&
             !g_fhxContinuousInjection &&
             !g_fhxValidationActive)
    {
        g_fhxBoundaryInjectStatus = learnedFHXBoundaryReady()
            ? FHXBoundaryInjectStatus::candidate_detected
            : FHXBoundaryInjectStatus::disabled;
    }

    g_fhxCurrentPixelFrame.clear();
    g_fhxCurrentShaderRTFrame.clear();
    g_fhxCurrentRTCatalog.clear();
    g_fhxCurrentSignatureCounts.clear();
    g_fhxLocalSignatureCounts.clear();
    g_fhxCurrentPassLocalOccurrence.clear();
    g_fhxCurrentPassPreviousSignatureKey.clear();
    g_fhxLastPassSignatureKey.clear();
    g_fhxObservedTeacherSignature = FHXPassSignature {};
    g_fhxObservedTeacherSignatureKey = 0;
    g_fhxObservedTeacherLocalOccurrence = 0;
    g_fhxObservedTeacherPreviousSignatureKey = 0;
    g_fhxCurrentDrawIndex = 0;
    g_fhxNextRenderPass = 0;
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

    std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
    if (pixelHash != 0)
        g_fhxSeenPixelShaders.insert(pixelHash);
    if (vertexHash != 0)
        g_fhxSeenVertexShaders.insert(vertexHash);
    if (computeHash != 0)
        g_fhxSeenComputeShaders.insert(computeHash);
}

static bool shouldBlockCurrentDrawFHX(command_list *commandList)
{
    if (commandList == nullptr)
        return false;

    const CommandListDataContainer &data = commandList->get_private_data<CommandListDataContainer>();

    std::lock_guard<std::mutex> lock(g_fhxHuntMutex);

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

static bool onDrawFHX(command_list *commandList, uint32_t, uint32_t, uint32_t, uint32_t)
{
    profileCurrentDrawFHX(commandList);
    return shouldBlockCurrentDrawFHX(commandList);
}

static bool onDrawIndexedFHX(command_list *commandList, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
{
    profileCurrentDrawFHX(commandList);
    return shouldBlockCurrentDrawFHX(commandList);
}

static void displayFHXHuntOverlay(effect_runtime *)
{
    std::vector<uint32_t> pixelHashes;
    std::vector<uint32_t> vertexHashes;
    std::vector<std::pair<uint32_t, FHXShaderFrameStat>> frameStats;
    std::vector<FHXShaderRTStat> shaderRTStats;
    std::vector<FHXRenderTargetInfo> rtCatalog;
    size_t computeCount = 0;
    uint64_t profiledFrame = 0;

    uint32_t boundaryTeacher = 0;
    uint32_t requestedFrames = 0;
    uint32_t remainingFrames = 0;
    bool continuousInjection = false;
    bool warmupPending = false;
    bool warmupIssued = false;
    bool warmupReady = false;
    uint32_t warmupQuietPresents = 0;
    uint64_t warmupReloadEvents = 0;
    uint64_t warmupCount = 0;
    uint64_t injectionCount = 0;
    uint64_t presentSerial = 0;
    uint64_t learnedSignatureKey = 0;
    uint32_t learnedTeacherLocalOccurrence = 0;
    uint64_t learnedTeacherPreviousSignatureKey = 0;
    uint32_t signatureStableFrames = 0;
    uint32_t lastSignatureOccurrences = 0;
    bool validationActive = false;
    bool validationAttempted = false;
    bool boundaryValidated = false;
    uint32_t validationUniqueHitFrames = 0;
    uint32_t validationDuplicateHits = 0;
    uint32_t validationMissedFrames = 0;
    uint64_t signatureChanges = 0;
    uint64_t teacherObservations = 0;
    uint64_t safeBoundaryHits = 0;
    uint64_t skippedAmbiguous = 0;
    uint64_t skippedUnstable = 0;
    FHXPassSignature learnedSignature = {};
    FHXBoundaryInjectStatus harnessStatus = FHXBoundaryInjectStatus::disabled;

    {
        std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
        pixelHashes.assign(g_fhxSeenPixelShaders.begin(), g_fhxSeenPixelShaders.end());
        vertexHashes.assign(g_fhxSeenVertexShaders.begin(), g_fhxSeenVertexShaders.end());
        computeCount = g_fhxSeenComputeShaders.size();
        frameStats.reserve(g_fhxLastPixelFrame.size());
        for (const auto &entry : g_fhxLastPixelFrame)
            frameStats.push_back(entry);
        shaderRTStats.reserve(g_fhxLastShaderRTFrame.size());
        for (const auto &entry : g_fhxLastShaderRTFrame)
            shaderRTStats.push_back(entry.second);
        rtCatalog = g_fhxLastRTCatalog;
        profiledFrame = g_fhxProfiledFrame;

        boundaryTeacher = g_fhxSelectedBoundaryTeacher;
        requestedFrames = g_fhxRequestedInjectionFrames;
        remainingFrames = g_fhxRemainingInjectionFrames;
        continuousInjection = g_fhxContinuousInjection;
        warmupPending = g_fhxWarmupPending;
        warmupIssued = g_fhxWarmupIssued;
        warmupReady = g_fhxWarmupReady;
        warmupQuietPresents = g_fhxWarmupQuietPresents;
        warmupReloadEvents = g_fhxWarmupReloadEvents;
        warmupCount = g_fhxWarmupCount;
        injectionCount = g_fhxInjectionCount;
        presentSerial = g_fhxPresentSerial;
        learnedSignatureKey = g_fhxLearnedSignatureKey;
        learnedTeacherLocalOccurrence = g_fhxLearnedTeacherLocalOccurrence;
        learnedTeacherPreviousSignatureKey = g_fhxLearnedTeacherPreviousSignatureKey;
        signatureStableFrames = g_fhxSignatureStableFrames;
        validationActive = g_fhxValidationActive;
        validationAttempted = g_fhxValidationAttempted;
        boundaryValidated = g_fhxBoundaryValidated;
        validationUniqueHitFrames = g_fhxValidationUniqueHitFrames;
        validationDuplicateHits = g_fhxValidationDuplicateHits;
        validationMissedFrames = g_fhxValidationMissedFrames;
        lastSignatureOccurrences = g_fhxLastSignatureOccurrences;
        signatureChanges = g_fhxSignatureChanges;
        teacherObservations = g_fhxTeacherObservations;
        safeBoundaryHits = g_fhxSafeBoundaryHits;
        skippedAmbiguous = g_fhxSkippedAmbiguous;
        skippedUnstable = g_fhxSkippedUnstable;
        learnedSignature = g_fhxLearnedSignature;
        harnessStatus = g_fhxBoundaryInjectStatus;
    }

    std::sort(pixelHashes.begin(), pixelHashes.end());
    std::sort(vertexHashes.begin(), vertexHashes.end());
    std::sort(frameStats.begin(), frameStats.end(),
              [](const auto &a, const auto &b) { return a.second.firstDraw < b.second.firstDraw; });
    std::sort(shaderRTStats.begin(), shaderRTStats.end(),
              [](const FHXShaderRTStat &a, const FHXShaderRTStat &b) {
                  if (a.shaderHash != b.shaderHash)
                      return a.shaderHash < b.shaderHash;
                  return a.firstDraw < b.firstDraw;
              });

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
    ImGui::Separator();
    ImGui::TextUnformatted("Per-frame draw / render-pass profiler");
    ImGui::Checkbox("Freeze profiler snapshot", &g_fhxFreezeProfiler);
    ImGui::Text("Snapshot frame: %llu", static_cast<unsigned long long>(profiledFrame));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("FHX structural pre-UI boundary harness");
    ImGui::Text("Output size: %ux%u", g_fhxOutputWidth, g_fhxOutputHeight);

    const char *teacherItems[] = {
        "Primary UI 0xCF49F7D6",
        "Bars/full-res 0x30B96240",
        "Minimap 0xAFB6F656"
    };

    int selectedTeacher = static_cast<int>(boundaryTeacher);
    if (ImGui::Combo("Boundary teacher", &selectedTeacher, teacherItems, 3))
    {
        std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
        g_fhxSelectedBoundaryTeacher = static_cast<uint32_t>(selectedTeacher);
        g_fhxObservedTeacherSignature = FHXPassSignature {};
        g_fhxLearnedSignature = FHXPassSignature {};
        g_fhxObservedTeacherSignatureKey = 0;
        g_fhxLearnedSignatureKey = 0;
        g_fhxObservedTeacherLocalOccurrence = 0;
        g_fhxLearnedTeacherLocalOccurrence = 0;
        g_fhxObservedTeacherPreviousSignatureKey = 0;
        g_fhxLearnedTeacherPreviousSignatureKey = 0;
        g_fhxSignatureStableFrames = 0;
        g_fhxLastSignatureOccurrences = 0;
        resetFHXValidation(true);
        resetFHXBoundaryTest();
    }

    ImGui::Text("Status: %s", fhxBoundaryStatusText(harnessStatus));
    ImGui::Text("Learned signature: 0x%016llX | stable %u/%u frames",
                static_cast<unsigned long long>(learnedSignatureKey),
                signatureStableFrames,
                kFHXSignatureStableFramesRequired);
    ImGui::Text("Context: occurrence %u of %u matching passes | previous signature 0x%016llX",
                learnedTeacherLocalOccurrence,
                lastSignatureOccurrences,
                static_cast<unsigned long long>(learnedTeacherPreviousSignatureKey));
    ImGui::Text("Dry-run validation: %s | hit frames %u/%u | missed %u | duplicate hits %u",
                boundaryValidated ? "PASSED" : (validationActive ? "RUNNING" : (validationAttempted ? "FAILED" : "WAITING")),
                validationUniqueHitFrames,
                kFHXValidationPresentsRequired,
                validationMissedFrames,
                validationDuplicateHits);
    ImGui::Text("Teacher observations: %llu | signature changes: %llu | safe boundary hits: %llu",
                static_cast<unsigned long long>(teacherObservations),
                static_cast<unsigned long long>(signatureChanges),
                static_cast<unsigned long long>(safeBoundaryHits));

    if (learnedSignature.valid)
    {
        ImGui::Text("Color: %ux%u fmt=%u samples=%u load=%u store=%u attachments=%u",
                    learnedSignature.colorWidth,
                    learnedSignature.colorHeight,
                    learnedSignature.colorFormat,
                    static_cast<unsigned>(learnedSignature.colorSamples),
                    learnedSignature.colorLoadOp,
                    learnedSignature.colorStoreOp,
                    learnedSignature.colorCount);
        ImGui::Text("Depth: %s fmt=%u %ux%u samples=%u load=%u store=%u",
                    learnedSignature.hasDepth ? "yes" : "no",
                    learnedSignature.depthFormat,
                    learnedSignature.depthWidth,
                    learnedSignature.depthHeight,
                    static_cast<unsigned>(learnedSignature.depthSamples),
                    learnedSignature.depthLoadOp,
                    learnedSignature.depthStoreOp);
    }

    ImGui::Text("Warm-up: %s | quiet presents %u/%u | warmups %llu | reload events %llu",
                warmupReady ? "READY" : (warmupIssued ? "SETTLING" : (warmupPending ? "QUEUED" : "NOT STARTED")),
                warmupQuietPresents,
                kFHXWarmupSettledPresents,
                static_cast<unsigned long long>(warmupCount),
                static_cast<unsigned long long>(warmupReloadEvents));
    ImGui::Text("Requested/remaining: %u / %u | completed injections %llu | present serial %llu",
                requestedFrames,
                remainingFrames,
                static_cast<unsigned long long>(injectionCount),
                static_cast<unsigned long long>(presentSerial));
    ImGui::Text("Skipped while unstable: %llu | skipped ambiguous: %llu",
                static_cast<unsigned long long>(skippedUnstable),
                static_cast<unsigned long long>(skippedAmbiguous));

    const bool boundaryReady =
        learnedSignature.valid &&
        learnedTeacherLocalOccurrence != 0 &&
        signatureStableFrames >= kFHXSignatureStableFramesRequired &&
        boundaryValidated;

    if (validationAttempted && !validationActive && !boundaryValidated &&
        learnedSignature.valid &&
        learnedTeacherLocalOccurrence != 0 &&
        signatureStableFrames >= kFHXSignatureStableFramesRequired)
    {
        if (ImGui::Button("Retry 60-frame dry-run"))
        {
            std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
            startFHXValidation();
        }
    }

    if (!boundaryReady)
        ImGui::BeginDisabled();

    if (ImGui::Button("Test 1 frame"))
    {
        std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
        queueFHXInjectionTest(1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Test 120 frames"))
    {
        std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
        queueFHXInjectionTest(120);
    }
    ImGui::SameLine();
    if (ImGui::Button("Test 600 frames"))
    {
        std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
        queueFHXInjectionTest(600);
    }
    ImGui::SameLine();
    if (ImGui::Button("Test 3600 frames"))
    {
        std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
        queueFHXInjectionTest(3600);
    }

    bool continuousValue = continuousInjection;
    if (ImGui::Checkbox("Continuous", &continuousValue))
    {
        std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
        g_fhxContinuousInjection = continuousValue;
        g_fhxRequestedInjectionFrames = 0;
        g_fhxRemainingInjectionFrames = 0;

        if (continuousValue)
        {
            g_fhxWarmupPending = true;
            g_fhxWarmupIssued = false;
            g_fhxWarmupReady = false;
            g_fhxWarmupQuietPresents = 0;
            g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::armed;
        }
        else
        {
            g_fhxWarmupPending = false;
            g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::candidate_detected;
        }
    }

    if (!boundaryReady)
        ImGui::EndDisabled();

    ImGui::TextWrapped(
        "The UI shader is only a passive teacher. Because this pass type repeats many times per frame, "
        "the learner now identifies it by structural signature + relative occurrence on the command "
        "list + the immediately preceding structural signature. After 20 stable frames, a 60-present "
        "dry-run automatically verifies that this contextual boundary resolves exactly once per frame. "
        "Only after that passes are the injection tests unlocked. No ReShade work occurs from draw callbacks, "
        "and global pass numbers/resource handles are deliberately excluded.");

    ImGui::Spacing();
    ImGui::TextUnformatted("Hash        First   Last   Count  P1  Pn  T1  Tn  #T  Note");

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

        ImGui::Text("0x%08X  %5llu  %5llu  %5u  %2u  %2u  %2u  %2u  %2u%s  %s",
                    hash,
                    static_cast<unsigned long long>(stat.firstDraw),
                    static_cast<unsigned long long>(stat.lastDraw),
                    stat.count,
                    stat.firstPass,
                    stat.lastPass,
                    stat.firstRT,
                    stat.lastRT,
                    stat.rtCount,
                    stat.rtOverflow ? "+" : " ",
                    note);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Per-shader / render-target breakdown");
    ImGui::TextUnformatted("Hash        RT   First   Last   Count  P1  Pn");
    for (const FHXShaderRTStat &stat : shaderRTStats) {
        ImGui::Text("0x%08X  %2u  %6llu  %6llu  %5u  %2u  %2u",
                    stat.shaderHash,
                    stat.rtId,
                    static_cast<unsigned long long>(stat.firstDraw),
                    static_cast<unsigned long long>(stat.lastDraw),
                    stat.count,
                    stat.firstPass,
                    stat.lastPass);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Render-target catalog for frozen frame");
    ImGui::TextUnformatted("ID   View/Resource handles           Size       Fmt  Samples  Format");
    for (size_t i = 0; i < rtCatalog.size(); ++i) {
        const FHXRenderTargetInfo &rt = rtCatalog[i];
        ImGui::Text("%2u   %08llX/%08llX  %4ux%-4u  %3u     %u    %s",
                    static_cast<unsigned>(i + 1),
                    static_cast<unsigned long long>(rt.viewHandle),
                    static_cast<unsigned long long>(rt.resourceHandle),
                    rt.width,
                    rt.height,
                    rt.formatValue,
                    static_cast<unsigned>(rt.samples),
                    fhxFormatName(rt.formatValue));
    }

    ImGui::Spacing();
    if (ImGui::Button("Clear observed shader list")) {
        std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
        g_fhxSeenPixelShaders.clear();
        g_fhxSeenVertexShaders.clear();
        g_fhxSeenComputeShaders.clear();
        g_fhxSelectedPixelHash = 0;
        g_fhxSelectedVertexHash = 0;
        g_fhxBlockSelectedPixel = false;
        g_fhxBlockSelectedVertex = false;
    }

    ImGui::Spacing();
    ImGui::TextWrapped(
        "Structural pre-UI boundary harness. Shader hashes are passive teachers only; no rendering "
        "occurs from draw callbacks. Injection executes only from the safe pre-vkCmdBeginRenderPass "
        "callback after a unique structural render-pass fingerprint is stable.");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(hModule))
                return FALSE;

            g_dllPath = getModulePath(hModule);

            // Pipeline creation was already verified safe in the previous build.
            reshade::register_event<reshade::addon_event::init_pipeline>(onInitPipeline);
            reshade::register_event<reshade::addon_event::destroy_pipeline>(onDestroyPipeline);

            // Minimal command-list state needed for shader hunting.
            reshade::register_event<reshade::addon_event::init_command_list>(onInitCommandList);
            reshade::register_event<reshade::addon_event::destroy_command_list>(onDestroyCommandList);
            reshade::register_event<reshade::addon_event::destroy_command_list>(onDestroyCommandListFHX);
            reshade::register_event<reshade::addon_event::reset_command_list>(onResetCommandList);
            reshade::register_event<reshade::addon_event::reset_command_list>(onResetCommandListFHX);
            reshade::register_event<reshade::addon_event::bind_pipeline>(onBindPipelineFHX);
            reshade::register_event<reshade::addon_event::init_effect_runtime>(onInitEffectRuntimeFHX);
            reshade::register_event<reshade::addon_event::destroy_effect_runtime>(onDestroyEffectRuntimeFHX);
            reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(onReshadeReloadedEffectsFHX);
            reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(onBindRenderTargetsFHX);
            reshade::register_event<reshade::addon_event::begin_render_pass>(onBeginRenderPassFHX);
            reshade::register_event<reshade::addon_event::end_render_pass>(onEndRenderPassFHX);

            // Draw suppression is still the only rendering modification in this build.
            // The additional callbacks only record render-pass timing.
            reshade::register_event<reshade::addon_event::draw>(onDrawFHX);
            reshade::register_event<reshade::addon_event::draw_indexed>(onDrawIndexedFHX);
            reshade::register_event<reshade::addon_event::reshade_present>(onReshadePresentFHX);

            // Give this build an explicit named overlay tab.
            reshade::register_overlay("REST FHX Debug", &displayFHXHuntOverlay);
            break;

        case DLL_PROCESS_DETACH:
            reshade::unregister_event<reshade::addon_event::reshade_present>(onReshadePresentFHX);
            reshade::unregister_event<reshade::addon_event::draw_indexed>(onDrawIndexedFHX);
            reshade::unregister_event<reshade::addon_event::draw>(onDrawFHX);
            reshade::unregister_event<reshade::addon_event::end_render_pass>(onEndRenderPassFHX);
            reshade::unregister_event<reshade::addon_event::begin_render_pass>(onBeginRenderPassFHX);
            reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(onBindRenderTargetsFHX);
            reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(onReshadeReloadedEffectsFHX);
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
