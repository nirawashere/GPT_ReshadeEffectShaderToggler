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

static effect_runtime *g_fhxRuntime = nullptr;
static uint32_t g_fhxOutputWidth = 0;
static uint32_t g_fhxOutputHeight = 0;

enum class FHXBoundaryAnchor : uint32_t
{
    bars_fullres = 0,
    primary_ui,
    minimap,
    final_post,
    count
};

static constexpr uint32_t kFHXBoundaryAnchorCount = static_cast<uint32_t>(FHXBoundaryAnchor::count);
static constexpr uint32_t kFHXBoundaryStableObservations = 3;
static constexpr uint32_t kFHXWarmupSettledPresents = 3;

struct FHXBoundaryModel
{
    uint32_t learnedLocalPass[kFHXBoundaryAnchorCount] = {};
    uint32_t stableObservations[kFHXBoundaryAnchorCount] = {};
    bool seenThisRecording[kFHXBoundaryAnchorCount] = {};
    uint64_t observations[kFHXBoundaryAnchorCount] = {};
    uint64_t changes[kFHXBoundaryAnchorCount] = {};
};

static std::unordered_map<command_list *, FHXBoundaryModel> g_fhxBoundaryModels;
static std::unordered_map<command_list *, uint32_t> g_fhxLocalPassCounter;
static std::unordered_map<command_list *, uint32_t> g_fhxActiveLocalPass;

static uint32_t g_fhxSelectedBoundaryAnchor = static_cast<uint32_t>(FHXBoundaryAnchor::primary_ui);
static uint32_t g_fhxRequestedInjectionFrames = 0;
static uint32_t g_fhxRemainingInjectionFrames = 0;
static bool g_fhxContinuousInjection = false;
static bool g_fhxInjectedThisFrame = false;

static bool g_fhxWarmupIssued = false;
static bool g_fhxWarmupReady = false;
static uint32_t g_fhxWarmupQuietPresents = 0;
static uint32_t g_fhxWarmupWidth = 0;
static uint32_t g_fhxWarmupHeight = 0;
static uint32_t g_fhxWarmupFormat = 0;

static uint64_t g_fhxPresentSerial = 0;
static uint64_t g_fhxLastInjectionPresentSerial = UINT64_MAX;
static uint64_t g_fhxInjectionCount = 0;
static uint64_t g_fhxWarmupCount = 0;
static uint64_t g_fhxSkippedUnstable = 0;
static uint64_t g_fhxSkippedWrongTarget = 0;
static uint64_t g_fhxSkippedAlreadyRendered = 0;
static uint64_t g_fhxBoundaryChanges = 0;

static uint32_t g_fhxLastCandidatePass = 0;
static uint32_t g_fhxLastCandidateLocalPass = 0;
static uint64_t g_fhxLastCandidateDraw = 0;
static uint64_t g_fhxCandidateHits = 0;
static uintptr_t g_fhxLastCandidateCommandList = 0;

static FHXBoundaryInjectStatus g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::disabled;
static thread_local bool g_fhxInsideEffectRender = false;

static const char *fhxBoundaryStatusText(FHXBoundaryInjectStatus status)
{
    switch (status)
    {
        case FHXBoundaryInjectStatus::disabled: return "idle";
        case FHXBoundaryInjectStatus::waiting_for_ui_prepass: return "learning selected boundary";
        case FHXBoundaryInjectStatus::waiting_for_candidate: return "boundary found; stabilizing";
        case FHXBoundaryInjectStatus::candidate_detected: return "stable boundary ready";
        case FHXBoundaryInjectStatus::armed: return "test queued";
        case FHXBoundaryInjectStatus::no_runtime: return "no ReShade runtime";
        case FHXBoundaryInjectStatus::effects_disabled: return "ReShade effects disabled/loading";
        case FHXBoundaryInjectStatus::bad_target: return "candidate target rejected";
        case FHXBoundaryInjectStatus::injected: return "test running";
        default: return "unknown";
    }
}

static const char *fhxBoundaryAnchorName(uint32_t index)
{
    switch (static_cast<FHXBoundaryAnchor>(index))
    {
        case FHXBoundaryAnchor::bars_fullres: return "Bars/full-res 0x30B96240";
        case FHXBoundaryAnchor::primary_ui: return "Primary UI 0xCF49F7D6";
        case FHXBoundaryAnchor::minimap: return "Minimap 0xAFB6F656";
        case FHXBoundaryAnchor::final_post: return "Final post 0x64787F0F";
        default: return "Unknown";
    }
}

static uint32_t fhxBoundaryAnchorHash(uint32_t index)
{
    switch (static_cast<FHXBoundaryAnchor>(index))
    {
        case FHXBoundaryAnchor::bars_fullres: return 0x30B96240u;
        case FHXBoundaryAnchor::primary_ui: return 0xCF49F7D6u;
        case FHXBoundaryAnchor::minimap: return 0xAFB6F656u;
        case FHXBoundaryAnchor::final_post: return 0x64787F0Fu;
        default: return 0;
    }
}

static bool isFullResolutionMainTargetFHXX(const FHXRenderTargetInfo &rt)
{
    if (!rt.valid || g_fhxOutputWidth == 0 || g_fhxOutputHeight == 0)
        return false;

    return rt.width == g_fhxOutputWidth &&
           rt.height == g_fhxOutputHeight &&
           static_cast<reshade::api::format>(rt.formatValue) == reshade::api::format::b8g8r8a8_unorm;
}

static bool isFullResolutionFinalTargetFHX(const FHXRenderTargetInfo &rt)
{
    if (!rt.valid || g_fhxOutputWidth == 0 || g_fhxOutputHeight == 0)
        return false;

    const auto fmt = static_cast<reshade::api::format>(rt.formatValue);
    return rt.width == g_fhxOutputWidth &&
           rt.height == g_fhxOutputHeight &&
           (fmt == reshade::api::format::b8g8r8a8_unorm_srgb ||
            fmt == reshade::api::format::b8g8r8a8_unorm);
}

static bool isExpectedBoundaryTargetFHX(uint32_t anchorIndex, const FHXRenderTargetInfo &rt)
{
    return static_cast<FHXBoundaryAnchor>(anchorIndex) == FHXBoundaryAnchor::final_post
        ? isFullResolutionFinalTargetFHX(rt)
        : isFullResolutionMainTargetFHX(rt);
}

static void resetFHXWarmup()
{
    g_fhxWarmupIssued = false;
    g_fhxWarmupReady = false;
    g_fhxWarmupQuietPresents = 0;
    g_fhxWarmupWidth = 0;
    g_fhxWarmupHeight = 0;
    g_fhxWarmupFormat = 0;
}

static bool warmupMatchesTargetFHX(const FHXRenderTargetInfo &rt)
{
    return g_fhxWarmupIssued &&
           g_fhxWarmupWidth == rt.width &&
           g_fhxWarmupHeight == rt.height &&
           g_fhxWarmupFormat == rt.formatValue;
}

static void queueFHXInjectionTest(uint32_t frameCount)
{
    g_fhxRequestedInjectionFrames = frameCount;
    g_fhxRemainingInjectionFrames = frameCount;
    g_fhxContinuousInjection = false;
    g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::armed;

    std::string msg = std::format(
        "[REST FHX] queued test: anchor='{}', frames={}",
        fhxBoundaryAnchorName(g_fhxSelectedBoundaryAnchor),
        frameCount);
    reshade::log::message(reshade::log::level::info, msg.c_str());
}

static void logFHXInjectionHeartbeat(const char *phase, command_list *commandList, uint32_t globalPass, uint32_t localPass, const FHXRenderTargetInfo &rt)
{
    if (g_fhxInjectionCount < 10 || (g_fhxInjectionCount % 60) == 0 || std::strcmp(phase, "warmup") == 0)
    {
        std::string msg = std::format(
            "[REST FHX] {} serial={} anchor='{}' injection={} cmd=0x{:X} globalPass={} localPass={} target={}x{} fmt={}",
            phase,
            g_fhxPresentSerial,
            fhxBoundaryAnchorName(g_fhxSelectedBoundaryAnchor),
            g_fhxInjectionCount,
            reinterpret_cast<uintptr_t>(commandList),
            globalPass,
            localPass,
            rt.width,
            rt.height,
            rt.formatValue);
        reshade::log::message(reshade::log::level::info, msg.c_str());
    }
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
                                 const render_pass_depth_stencil_desc *)
{
    if (g_fhxInsideEffectRender || commandList == nullptr)
        return;

    const resource_view targetView =
        (count != 0 && rts != nullptr) ? rts[0].view : resource_view {};
    const FHXRenderTargetInfo rt =
        (targetView.handle != 0) ? inspectRenderTargetFHX(commandList, targetView) : FHXRenderTargetInfo {};

    effect_runtime *runtime = nullptr;
    bool performWarmup = false;
    bool performInjection = false;
    uint32_t passIndex = 0;
    uint32_t localPassIndex = 0;

    {
        std::lock_guard<std::mutex> lock(g_fhxHuntMutex);

        passIndex = ++g_fhxNextRenderPass;
        localPassIndex = ++g_fhxLocalPassCounter[commandList];

        g_fhxActiveRenderPass[commandList] = passIndex;
        g_fhxActiveLocalPass[commandList] = localPassIndex;
        g_fhxCurrentRenderTarget[commandList] = rt;

        auto modelIt = g_fhxBoundaryModels.find(commandList);
        if (modelIt == g_fhxBoundaryModels.end())
            return;

        FHXBoundaryModel &model = modelIt->second;
        const uint32_t anchorIndex = g_fhxSelectedBoundaryAnchor;

        const bool stableBoundary =
            anchorIndex < kFHXBoundaryAnchorCount &&
            model.learnedLocalPass[anchorIndex] != 0 &&
            model.stableObservations[anchorIndex] >= kFHXBoundaryStableObservations;

        if (!stableBoundary)
        {
            if (g_fhxRemainingInjectionFrames != 0 || g_fhxContinuousInjection)
                ++g_fhxSkippedUnstable;
            return;
        }

        if (localPassIndex != model.learnedLocalPass[anchorIndex])
            return;

        ++g_fhxCandidateHits;
        g_fhxLastCandidatePass = passIndex;
        g_fhxLastCandidateLocalPass = localPassIndex;
        g_fhxLastCandidateDraw = g_fhxCurrentDrawIndex + 1;
        g_fhxLastCandidateCommandList = reinterpret_cast<uintptr_t>(commandList);

        if (!isExpectedBoundaryTargetFHX(anchorIndex, rt))
        {
            ++g_fhxSkippedWrongTarget;
            g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::bad_target;
            return;
        }

        const bool testRequested = g_fhxRemainingInjectionFrames != 0 || g_fhxContinuousInjection;
        if (!testRequested)
        {
            g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::candidate_detected;
            return;
        }

        if (g_fhxLastInjectionPresentSerial == g_fhxPresentSerial)
        {
            ++g_fhxSkippedAlreadyRendered;
            return;
        }

        runtime = g_fhxRuntime;
        if (runtime == nullptr)
        {
            g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::no_runtime;
            return;
        }

        if (!warmupMatchesTargetFHX(rt))
            resetFHXWarmup();

        if (!g_fhxWarmupIssued)
        {
            g_fhxWarmupIssued = true;
            g_fhxWarmupReady = false;
            g_fhxWarmupQuietPresents = 0;
            g_fhxWarmupWidth = rt.width;
            g_fhxWarmupHeight = rt.height;
            g_fhxWarmupFormat = rt.formatValue;
            g_fhxLastInjectionPresentSerial = g_fhxPresentSerial;
            performWarmup = true;
        }
        else if (!g_fhxWarmupReady)
        {
            g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::effects_disabled;
            return;
        }
        else
        {
            g_fhxLastInjectionPresentSerial = g_fhxPresentSerial;
            g_fhxInjectedThisFrame = true;
            performInjection = true;
        }
    }

    if (runtime == nullptr)
        return;

    if (performWarmup)
    {
        logFHXInjectionHeartbeat("warmup", commandList, passIndex, localPassIndex, rt);
        g_fhxInsideEffectRender = true;
        runtime->render_effects(commandList, targetView, targetView);
        g_fhxInsideEffectRender = false;

        std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
        ++g_fhxWarmupCount;
        g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::waiting_for_candidate;
        return;
    }

    if (!performInjection)
        return;

    if (runtime->is_loading() || !runtime->get_effects_state())
    {
        std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
        g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::effects_disabled;
        return;
    }

    logFHXInjectionHeartbeat("begin", commandList, passIndex, localPassIndex, rt);

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
            reshade::log::message(reshade::log::level::info, "[REST FHX] finite injection test complete");
        }
        else
        {
            g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::injected;
        }
    }

    logFHXInjectionHeartbeat("end", commandList, passIndex, localPassIndex, rt);
}

static void onEndRenderPassFHX(command_list *commandList)
{
    if (g_fhxInsideEffectRender || commandList == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
    g_fhxActiveRenderPass[commandList] = 0;
    g_fhxActiveLocalPass[commandList] = 0;
    g_fhxCurrentRenderTarget[commandList] = FHXRenderTargetInfo {};
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
}

static void onResetCommandListFHX(command_list *commandList)
{
    if (commandList == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
    g_fhxActiveRenderPass.erase(commandList);
    g_fhxActiveLocalPass.erase(commandList);
    g_fhxLocalPassCounter[commandList] = 0;
    g_fhxCurrentRenderTarget.erase(commandList);

    FHXBoundaryModel &model = g_fhxBoundaryModels[commandList];
    for (uint32_t i = 0; i < kFHXBoundaryAnchorCount; ++i)
        model.seenThisRecording[i] = false;
}

static void onDestroyCommandListFHX(command_list *commandList)
{
    if (commandList == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
    g_fhxActiveRenderPass.erase(commandList);
    g_fhxActiveLocalPass.erase(commandList);
    g_fhxLocalPassCounter.erase(commandList);
    g_fhxBoundaryModels.erase(commandList);
    g_fhxCurrentRenderTarget.erase(commandList);
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

    // Learn each candidate boundary on the command list where that shader
    // actually appears. This is intentionally command-list-local, because global
    // pass numbers can interleave across Vulkan command buffers.
    const auto localPassIt = g_fhxActiveLocalPass.find(commandList);
    const uint32_t localPassIndex =
        (localPassIt != g_fhxActiveLocalPass.end()) ? localPassIt->second : 0;

    if (localPassIndex != 0)
    {
        FHXBoundaryModel &model = g_fhxBoundaryModels[commandList];

        for (uint32_t anchorIndex = 0; anchorIndex < kFHXBoundaryAnchorCount; ++anchorIndex)
        {
            if (pixelHash != fhxBoundaryAnchorHash(anchorIndex) ||
                model.seenThisRecording[anchorIndex] ||
                !isExpectedBoundaryTargetFHX(anchorIndex, rt))
                continue;

            model.seenThisRecording[anchorIndex] = true;
            ++model.observations[anchorIndex];

            if (model.learnedLocalPass[anchorIndex] == localPassIndex)
            {
                if (model.stableObservations[anchorIndex] < 0xFFFFFFFFu)
                    ++model.stableObservations[anchorIndex];
            }
            else
            {
                if (model.learnedLocalPass[anchorIndex] != 0)
                {
                    ++model.changes[anchorIndex];
                    ++g_fhxBoundaryChanges;

                    std::string msg = std::format(
                        "[REST FHX] boundary changed: anchor='{}' cmd=0x{:X} oldLocalPass={} newLocalPass={}",
                        fhxBoundaryAnchorName(anchorIndex),
                        reinterpret_cast<uintptr_t>(commandList),
                        model.learnedLocalPass[anchorIndex],
                        localPassIndex);
                    reshade::log::message(reshade::log::level::warning, msg.c_str());
                }

                model.learnedLocalPass[anchorIndex] = localPassIndex;
                model.stableObservations[anchorIndex] = 1;
            }

            if (anchorIndex == g_fhxSelectedBoundaryAnchor)
            {
                g_fhxLastCandidatePass = passIndex;
                g_fhxLastCandidateLocalPass = localPassIndex;
                g_fhxLastCandidateDraw = drawIndex;
                g_fhxLastCandidateCommandList = reinterpret_cast<uintptr_t>(commandList);

                if (model.stableObservations[anchorIndex] >= kFHXBoundaryStableObservations)
                    g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::candidate_detected;
                else
                    g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::waiting_for_candidate;
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

    if (runtime != nullptr) {
        g_fhxRuntime = runtime;
        g_fhxOutputWidth = width;
        g_fhxOutputHeight = height;
    }

    if (!g_fhxFreezeProfiler) {
        g_fhxLastPixelFrame = g_fhxCurrentPixelFrame;
        g_fhxLastShaderRTFrame = g_fhxCurrentShaderRTFrame;
        g_fhxLastRTCatalog = g_fhxCurrentRTCatalog;
        ++g_fhxProfiledFrame;
    }

    // A custom-target render can cause ReShade to create and asynchronously
    // compile a new effect permutation. Treat that first render as warm-up only
    // and do not start the requested test until loading is finished and several
    // clean presents have elapsed.
    if (g_fhxWarmupIssued && !g_fhxWarmupReady)
    {
        if (runtime != nullptr && runtime->is_loading())
        {
            g_fhxWarmupQuietPresents = 0;
        }
        else
        {
            if (g_fhxWarmupQuietPresents < kFHXWarmupSettledPresents)
                ++g_fhxWarmupQuietPresents;

            if (g_fhxWarmupQuietPresents >= kFHXWarmupSettledPresents)
            {
                g_fhxWarmupReady = true;
                reshade::log::message(reshade::log::level::info, "[REST FHX] custom-target permutation warm-up settled; requested test may begin");
            }
        }
    }

    g_fhxCurrentPixelFrame.clear();
    g_fhxCurrentShaderRTFrame.clear();
    g_fhxCurrentRTCatalog.clear();
    g_fhxCurrentDrawIndex = 0;
    g_fhxNextRenderPass = 0;
    g_fhxInjectedThisFrame = false;

    const bool testRequested = g_fhxRemainingInjectionFrames != 0 || g_fhxContinuousInjection;
    if (testRequested)
        g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::armed;
    else if (g_fhxBoundaryInjectStatus == FHXBoundaryInjectStatus::injected)
        g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::candidate_detected;
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
    ImGui::TextUnformatted("FHX multi-boundary injection test harness");
    ImGui::Text("Output size: %ux%u", g_fhxOutputWidth, g_fhxOutputHeight);

    const char *anchorItems[] = {
        "Bars/full-res 0x30B96240",
        "Primary UI 0xCF49F7D6",
        "Minimap 0xAFB6F656",
        "Final post 0x64787F0F"
    };

    int selectedAnchor = static_cast<int>(g_fhxSelectedBoundaryAnchor);
    if (ImGui::Combo("Boundary anchor", &selectedAnchor, anchorItems, 4))
    {
        std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
        g_fhxSelectedBoundaryAnchor = static_cast<uint32_t>(selectedAnchor);
        g_fhxRequestedInjectionFrames = 0;
        g_fhxRemainingInjectionFrames = 0;
        g_fhxContinuousInjection = false;
        resetFHXWarmup();
        g_fhxBoundaryInjectStatus = FHXBoundaryInjectStatus::waiting_for_ui_prepass;
    }

    uint32_t readyModels = 0;
    uint32_t totalModels = 0;
    for (const auto &entry : g_fhxBoundaryModels)
    {
        const FHXBoundaryModel &model = entry.second;
        ++totalModels;
        if (model.learnedLocalPass[g_fhxSelectedBoundaryAnchor] != 0 &&
            model.stableObservations[g_fhxSelectedBoundaryAnchor] >= kFHXBoundaryStableObservations)
            ++readyModels;
    }

    ImGui::Text("Boundary models ready: %u / %u command lists", readyModels, totalModels);
    ImGui::Text("Last anchor observation: cmd 0x%llX, global pass %u, local pass %u, draw %llu",
                static_cast<unsigned long long>(g_fhxLastCandidateCommandList),
                g_fhxLastCandidatePass,
                g_fhxLastCandidateLocalPass,
                static_cast<unsigned long long>(g_fhxLastCandidateDraw));
    ImGui::Text("Status: %s", fhxBoundaryStatusText(g_fhxBoundaryInjectStatus));

    ImGui::Text("Warm-up: %s | quiet presents %u/%u | warmups %llu",
                g_fhxWarmupReady ? "READY" : (g_fhxWarmupIssued ? "WAITING" : "NOT STARTED"),
                g_fhxWarmupQuietPresents,
                kFHXWarmupSettledPresents,
                static_cast<unsigned long long>(g_fhxWarmupCount));

    ImGui::Text("Requested/remaining finite injections: %u / %u",
                g_fhxRequestedInjectionFrames,
                g_fhxRemainingInjectionFrames);
    ImGui::Text("Completed injections: %llu | present serial: %llu",
                static_cast<unsigned long long>(g_fhxInjectionCount),
                static_cast<unsigned long long>(g_fhxPresentSerial));

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

    if (ImGui::Checkbox("Continuous injection", &g_fhxContinuousInjection))
    {
        std::lock_guard<std::mutex> lock(g_fhxHuntMutex);
        g_fhxRequestedInjectionFrames = 0;
        g_fhxRemainingInjectionFrames = 0;
        g_fhxBoundaryInjectStatus = g_fhxContinuousInjection
            ? FHXBoundaryInjectStatus::armed
            : FHXBoundaryInjectStatus::candidate_detected;

        reshade::log::message(
            reshade::log::level::info,
            g_fhxContinuousInjection ? "[REST FHX] continuous test enabled" : "[REST FHX] continuous test disabled");
    }

    ImGui::Text("Skipped: unstable=%llu wrong-target=%llu already-rendered=%llu | boundary changes=%llu",
                static_cast<unsigned long long>(g_fhxSkippedUnstable),
                static_cast<unsigned long long>(g_fhxSkippedWrongTarget),
                static_cast<unsigned long long>(g_fhxSkippedAlreadyRendered),
                static_cast<unsigned long long>(g_fhxBoundaryChanges));

    ImGui::TextWrapped(
        "Each Vulkan command list learns the LOCAL pass containing the selected verified shader. "
        "A boundary must repeat at least 3 times on that command list before it is trusted. "
        "The first custom-target ReShade render is treated only as a permutation warm-up; the "
        "actual test waits until ReShade stops loading and 3 additional presents pass. "
        "Injection remains in begin_render_pass, before the game's Vulkan render pass begins. "
        "Use Final post as a control: if it is stable while earlier anchors flicker, boundary "
        "timing is the problem rather than the custom render_effects call itself.");

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
        "Multi-boundary FHX diagnostic harness. Injection is OFF until a finite or continuous test "
        "is selected. Candidate boundaries are learned per Vulkan command list/local pass from four "
        "verified shader anchors, with automatic custom-target permutation warm-up before real test "
        "frames. Injection remains in begin_render_pass before Vulkan enters the game render pass. "
        "Descriptor, texture-binding, constant-copy and full upstream REST systems remain disabled.");
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
