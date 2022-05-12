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
#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#define ImTextureID unsigned long long // Change ImGui texture ID type to that of a 'reshade::api::resource_view' handle

#include <imgui.h>
#include <reshade.hpp>
#include "crc32_hash.hpp"
#include "ShaderManager.h"
#include "CDataFile.h"
#include "ToggleGroup.h"
#include "AddonUIData.h"
#include "AddonUIDisplay.h"
#include <vector>
#include <unordered_map>
#include <set>

using namespace reshade::api;
using namespace ShaderToggler;
using namespace AddonImGui;

extern "C" __declspec(dllexport) const char *NAME = "Reshade Effect Shader Toggler";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Addon which allows you to define groups of shaders to render Reshade effects on with one key press.";

struct __declspec(uuid("222F7169-3C09-40DB-9BC9-EC53842CE537")) CommandListDataContainer {
    uint64_t activePixelShaderPipeline;
    uint64_t activeVertexShaderPipeline;
	resource_view active_rtv = resource_view{ 0 };
	atomic_bool rendered_effects = false;
	std::set <
		effect_technique,
		decltype([](const effect_technique& lhs, const effect_technique& rhs) { return lhs.handle < rhs.handle; })
	> techniquesToRender;
	std::unordered_set <
		effect_technique,
		decltype([](const effect_technique& t) { return t.handle; }),
		decltype([](const effect_technique& lhs, const effect_technique& rhs) { return lhs.handle == rhs.handle; })
	> allRenderedTechniques;
};

struct __declspec(uuid("C63E95B1-4E2F-46D6-A276-E8B4612C069A")) DeviceDataContainer {
	std::map < resource_view, resource, decltype([](const resource_view& lhs, const resource_view& rhs)
		{
			return lhs.handle < rhs.handle;
		}) > allValidRenderTargets;
	effect_runtime* current_runtime = nullptr;
	std::map<std::string, effect_technique> allEnabledTechniques;
};

#define CHAR_BUFFER_SIZE 256
#define MAX_EFFECT_HANDLES 128

static ShaderToggler::ShaderManager g_pixelShaderManager;
static ShaderToggler::ShaderManager g_vertexShaderManager;
static atomic_uint32_t g_activeCollectorFrameCounter = 0;
static AddonUIData g_addonUIData(&g_pixelShaderManager, &g_vertexShaderManager, &g_activeCollectorFrameCounter);
static std::shared_mutex s_mutex;
static char g_charBuffer[CHAR_BUFFER_SIZE];
static size_t g_charBufferSize = CHAR_BUFFER_SIZE;


/// <summary>
/// Calculates a crc32 hash from the passed in shader bytecode. The hash is used to identity the shader in future runs.
/// </summary>
/// <param name="shaderData"></param>
/// <returns></returns>
static uint32_t calculateShaderHash(void* shaderData)
{
	if(nullptr==shaderData)
	{
		return 0;
	}

	const auto shaderDesc = *static_cast<shader_desc *>(shaderData);
	return compute_crc32(static_cast<const uint8_t *>(shaderDesc.code), shaderDesc.code_size);
}


static void onInitDevice(device* device)
{
	device->create_private_data<DeviceDataContainer>();
}


static void onDestroyDevice(device* device)
{
	device->destroy_private_data<DeviceDataContainer>();
}


static void onInitCommandList(command_list *commandList)
{
	commandList->create_private_data<CommandListDataContainer>();
}


static void onDestroyCommandList(command_list *commandList)
{
	commandList->destroy_private_data<CommandListDataContainer>();
}

static void onResetCommandList(command_list *commandList)
{
	CommandListDataContainer &commandListData = commandList->get_private_data<CommandListDataContainer>();
	commandListData.activePixelShaderPipeline = -1;
	commandListData.activeVertexShaderPipeline = -1;
	commandListData.rendered_effects = false;
	commandListData.active_rtv = { 0 };
}


static void onPresent(command_queue* queue, swapchain* swapchain, const rect*, const rect*, uint32_t, const rect*)
{
	CommandListDataContainer& commandListData = queue->get_private_data<CommandListDataContainer>();
	DeviceDataContainer& deviceData = queue->get_device()->get_private_data<DeviceDataContainer>();

	commandListData.active_rtv = { 0 };
	commandListData.rendered_effects = false;
	commandListData.techniquesToRender.clear();
	commandListData.allRenderedTechniques.clear();
}


static void onReshadeReloadedEffects(effect_runtime* runtime)
{
	DeviceDataContainer& data = runtime->get_device()->get_private_data<DeviceDataContainer>();
	data.allEnabledTechniques.clear();
	data.current_runtime->enumerate_techniques(nullptr, [&data](effect_runtime* runtime, effect_technique technique) {
		bool enabled = runtime->get_technique_state(technique);
	
		if (enabled)
		{
			g_charBufferSize = CHAR_BUFFER_SIZE;
			runtime->get_technique_name(technique, g_charBuffer, &g_charBufferSize);
			data.allEnabledTechniques.emplace(std::string(g_charBuffer), technique.handle);
		}
		});
}


static bool onReshadeSetTechniqueState(effect_runtime* runtime, effect_technique technique, bool enabled)
{
	DeviceDataContainer& data = runtime->get_device()->get_private_data<DeviceDataContainer>();
	if (!enabled)
	{
		(void)std::erase_if(data.allEnabledTechniques, [&data, &technique](const auto& tech) {
			auto const& [key, value] = tech;
			return value.handle == technique.handle;
			});
	}
	else
	{
		g_charBufferSize = CHAR_BUFFER_SIZE;
		runtime->get_technique_name(technique, g_charBuffer, &g_charBufferSize);
		string techName(g_charBuffer);
		if (data.allEnabledTechniques.find(techName) == data.allEnabledTechniques.end())
		{
			data.allEnabledTechniques.emplace(techName, technique);
		}
	}

	return false;
}


static void onInitEffectRuntime(effect_runtime* runtime)
{
	DeviceDataContainer& data = runtime->get_device()->get_private_data<DeviceDataContainer>();
	data.current_runtime = runtime;
}


static void onDestroyEffectRuntime(effect_runtime* runtime)
{
	DeviceDataContainer& data = runtime->get_device()->get_private_data<DeviceDataContainer>();
	data.current_runtime = nullptr;
}


static void onInitResourceView(device* device, resource resource, resource_usage usage_type, const resource_view_desc& desc, resource_view view)
{
	if (device == nullptr)
	{
		return;
	}

	DeviceDataContainer& data = device->get_private_data<DeviceDataContainer>();

	const resource_desc texture_desc = device->get_resource_desc(resource);

	if (data.current_runtime != nullptr)
	{
		uint32_t frame_width, frame_height;
		data.current_runtime->get_screenshot_width_and_height(&frame_width, &frame_height);

		if (texture_desc.texture.height != frame_height || texture_desc.texture.width != frame_width)
		{
			return;
		}

		data.allValidRenderTargets.insert(make_pair(view, resource));
	}
}


static void onDestroyResourceView(device* device, resource_view view)
{
	if (device == nullptr) {
		return;
	}

	DeviceDataContainer& data = device->get_private_data<DeviceDataContainer>();

	std::unique_lock<std::shared_mutex> lock(s_mutex);
	(void)std::erase_if(data.allValidRenderTargets, [&view](const auto& item) {
		auto const& [key, value] = item;
		return key.handle == view.handle;
		});
}


static void onDestroyResource(device* device, resource res)
{
	if (device == nullptr) {
		return;
	}

	DeviceDataContainer& data = device->get_private_data<DeviceDataContainer>();

	std::unique_lock<std::shared_mutex> lock(s_mutex);
	(void)std::erase_if(data.allValidRenderTargets, [&res](const auto& item) {
		auto const& [key, value] = item;
		return value.handle == res.handle;
		});
}


static void onInitPipeline(device *device, pipeline_layout, uint32_t subobjectCount, const pipeline_subobject *subobjects, pipeline pipelineHandle)
{
	// shader has been created, we will now create a hash and store it with the handle we got.
	for (uint32_t i = 0; i < subobjectCount; ++i)
	{
		switch (subobjects[i].type)
		{
		case pipeline_subobject_type::vertex_shader:
			{
				g_vertexShaderManager.addHashHandlePair(calculateShaderHash(subobjects[i].data), pipelineHandle.handle);
			}
			break;
		case pipeline_subobject_type::pixel_shader:
			{
				g_pixelShaderManager.addHashHandlePair(calculateShaderHash(subobjects[i].data), pipelineHandle.handle);
			}
			break;
		}
	}
}


static void onDestroyPipeline(device *device, pipeline pipelineHandle)
{
	g_pixelShaderManager.removeHandle(pipelineHandle.handle);
	g_vertexShaderManager.removeHandle(pipelineHandle.handle);
}


/// <summary>
/// This function will return true if the command list specified has one or more shader hashes which are currently marked. Otherwise false.
/// </summary>
/// <param name="commandList"></param>
/// <returns>true if the draw call has to be blocked</returns>
bool checkDrawCallForCommandList(command_list* commandList)
{
	if (nullptr == commandList)
	{
		return false;
	}

	CommandListDataContainer& commandListData = commandList->get_private_data<CommandListDataContainer>();
	DeviceDataContainer& deviceData = commandList->get_device()->get_private_data<DeviceDataContainer>();

	if (deviceData.allEnabledTechniques.size() == commandListData.allRenderedTechniques.size())
	{
		return false;
	}

	uint32_t psShaderHash = g_pixelShaderManager.getShaderHash(commandListData.activePixelShaderPipeline);
	uint32_t vsShaderHash = g_vertexShaderManager.getShaderHash(commandListData.activeVertexShaderPipeline);

	vector<const ToggleGroup*> tGroups;

	if ((g_pixelShaderManager.isBlockedShader(psShaderHash) || g_vertexShaderManager.isBlockedShader(vsShaderHash)) &&
		(g_pixelShaderManager.isInHuntingMode() || g_vertexShaderManager.isInHuntingMode()))
	{
		tGroups.push_back(&g_addonUIData.GetToggleGroups()[g_addonUIData.GetToggleGroupIdShaderEditing()]);
	}

	for (auto& group : g_addonUIData.GetToggleGroups())
	{
		if (group.second.isBlockedPixelShader(psShaderHash) || group.second.isBlockedVertexShader(vsShaderHash))
		{
			tGroups.push_back(&group.second);
		}
	}

	for (auto tGroup : tGroups)
	{
		if (tGroup->preferredTechniques().size() > 0) {
			for (auto& techName : tGroup->preferredTechniques())
			{
				if (deviceData.allEnabledTechniques.contains(techName))
				{
					commandListData.techniquesToRender.insert(deviceData.allEnabledTechniques[techName]);
				}
			}
		}
		else
		{
			for (const auto& tech : deviceData.allEnabledTechniques)
			{
				commandListData.techniquesToRender.insert(tech.second);
			}
		}
	}

	return commandListData.techniquesToRender.size() > 0;
}


static void RenderEffects(command_list* cmd_list)
{
	if (cmd_list == nullptr || cmd_list->get_device() == nullptr)
	{
		return;
	}

	device* device = cmd_list->get_device();
	CommandListDataContainer& commandListData = cmd_list->get_private_data<CommandListDataContainer>();
	DeviceDataContainer& deviceData = device->get_private_data<DeviceDataContainer>();

	if (deviceData.current_runtime == nullptr || commandListData.active_rtv == 0) {
		return;
	}

	resource res = device->get_resource_from_view(commandListData.active_rtv);
	resource_usage oldUsage = device->get_resource_desc(res).usage;

	vector<effect_technique> toRender;

	for (auto& tech : commandListData.techniquesToRender)
	{
		if (commandListData.allRenderedTechniques.find(tech) == commandListData.allRenderedTechniques.end())
		{
			toRender.push_back(tech);
		}
	}

	if (toRender.size() > 0) {
		cmd_list->barrier(res, oldUsage, resource_usage::render_target);

		// dummy render call to prevent reshade from rendering effects on top if we're rendering too
		if (!commandListData.rendered_effects)
		{
			for (auto& tech : deviceData.allEnabledTechniques)
			{
				deviceData.current_runtime->set_technique_state(tech.second, false);
			}

			deviceData.current_runtime->render_effects(cmd_list, commandListData.active_rtv);

			for (auto& tech : deviceData.allEnabledTechniques)
			{
				deviceData.current_runtime->set_technique_state(tech.second, true);
			}

			commandListData.rendered_effects = true;
		}

		for (auto& tech : toRender)
		{
			deviceData.current_runtime->render_technique(tech, cmd_list, commandListData.active_rtv);
		}

		cmd_list->barrier(res, resource_usage::render_target, oldUsage);

		for (auto& tech : commandListData.techniquesToRender)
		{
			commandListData.allRenderedTechniques.insert(tech);
		}

		commandListData.techniquesToRender.clear();
	}
}


static void onBindPipeline(command_list* commandList, pipeline_stage stages, pipeline pipelineHandle)
{
	if(nullptr!=commandList && pipelineHandle.handle!=0)
	{
		const bool handleHasPixelShaderAttached = g_pixelShaderManager.isKnownHandle(pipelineHandle.handle);
		const bool handleHasVertexShaderAttached = g_vertexShaderManager.isKnownHandle(pipelineHandle.handle);
		if(!handleHasPixelShaderAttached && !handleHasVertexShaderAttached)
		{
			// draw call with unknown handle, don't collect it
			return;
		}
		CommandListDataContainer &commandListData = commandList->get_private_data<CommandListDataContainer>();
		switch(stages)
		{
			case pipeline_stage::all:
				if(g_activeCollectorFrameCounter>0)
				{
					// in collection mode
					if(handleHasPixelShaderAttached)
					{
						g_pixelShaderManager.addActivePipelineHandle(pipelineHandle.handle);
					}
					if(handleHasVertexShaderAttached)
					{
						g_vertexShaderManager.addActivePipelineHandle(pipelineHandle.handle);
					}
				}
				else
				{
					commandListData.activePixelShaderPipeline = handleHasPixelShaderAttached ? pipelineHandle.handle : -1;
					commandListData.activeVertexShaderPipeline = handleHasVertexShaderAttached ? pipelineHandle.handle : -1;
				}
				break;	
			case pipeline_stage::pixel_shader:
				if(handleHasPixelShaderAttached)
				{
					if(g_activeCollectorFrameCounter>0)
					{
						// in collection mode
						g_pixelShaderManager.addActivePipelineHandle(pipelineHandle.handle);
					}
					commandListData.activePixelShaderPipeline = pipelineHandle.handle;
				}
				break;
			case pipeline_stage::vertex_shader:
				if(handleHasVertexShaderAttached)
				{
					if(g_activeCollectorFrameCounter>0)
					{
						// in collection mode
						g_vertexShaderManager.addActivePipelineHandle(pipelineHandle.handle);
					}
					commandListData.activeVertexShaderPipeline = pipelineHandle.handle;
				}
				break;
		}

		if(checkDrawCallForCommandList(commandList))
		{
			RenderEffects(commandList);
		}
	}
}


static void onBindRenderTargetsAndDepthStencil(command_list* cmd_list, uint32_t count, const resource_view* rtvs, resource_view dsv)
{
	if (cmd_list == nullptr || cmd_list->get_device() == nullptr)
	{
		return;
	}

	device* device = cmd_list->get_device();
	CommandListDataContainer& commandListData = cmd_list->get_private_data<CommandListDataContainer>();
	DeviceDataContainer& deviceData = device->get_private_data<DeviceDataContainer>();

	resource_view new_view = { 0 };

	{
		std::unique_lock<std::shared_mutex> lock(s_mutex);
		for (int i = 0; i < count; i++)
		{
			if (deviceData.allValidRenderTargets.find(rtvs[i]) != deviceData.allValidRenderTargets.end()) {
				new_view = rtvs[i];
				break;
			}
		}
	}
	
	if(new_view != commandListData.active_rtv)
	{
		commandListData.active_rtv = new_view;
	}
}


static void onReshadeOverlay(effect_runtime* runtime)
{
	DisplayOverlay(g_addonUIData);
}


static void onReshadePresent(effect_runtime* runtime)
{
	CheckHotkeys(g_addonUIData, runtime);
}


static void displaySettings(effect_runtime* runtime)
{
	DisplaySettings(g_addonUIData, runtime);
}


BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
		{
			return FALSE;
		}
		reshade::register_event<reshade::addon_event::init_pipeline>(onInitPipeline);
		reshade::register_event<reshade::addon_event::init_command_list>(onInitCommandList);
		reshade::register_event<reshade::addon_event::destroy_command_list>(onDestroyCommandList);
		reshade::register_event<reshade::addon_event::reset_command_list>(onResetCommandList);
		reshade::register_event<reshade::addon_event::destroy_pipeline>(onDestroyPipeline);
		reshade::register_event<reshade::addon_event::reshade_overlay>(onReshadeOverlay);
		reshade::register_event<reshade::addon_event::reshade_present>(onReshadePresent);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(onReshadeReloadedEffects);
		reshade::register_event<reshade::addon_event::reshade_set_technique_state>(onReshadeSetTechniqueState);
		reshade::register_event<reshade::addon_event::bind_pipeline>(onBindPipeline);
		reshade::register_event<reshade::addon_event::init_device>(onInitDevice);
		reshade::register_event<reshade::addon_event::destroy_device>(onDestroyDevice);
		reshade::register_event<reshade::addon_event::present>(onPresent);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(onBindRenderTargetsAndDepthStencil);
		reshade::register_event<reshade::addon_event::init_effect_runtime>(onInitEffectRuntime);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(onDestroyEffectRuntime);
		reshade::register_event<reshade::addon_event::destroy_resource_view>(onDestroyResourceView);
		reshade::register_event<reshade::addon_event::init_resource_view>(onInitResourceView);
		reshade::register_event<reshade::addon_event::destroy_resource>(onDestroyResource);
		reshade::register_overlay(nullptr, &displaySettings);
		g_addonUIData.LoadShaderTogglerIniFile();
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_event<reshade::addon_event::reshade_present>(onReshadePresent);
		reshade::unregister_event<reshade::addon_event::destroy_pipeline>(onDestroyPipeline);
		reshade::unregister_event<reshade::addon_event::init_pipeline>(onInitPipeline);
		reshade::unregister_event<reshade::addon_event::reshade_overlay>(onReshadeOverlay);
		reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(onReshadeReloadedEffects);
		reshade::unregister_event<reshade::addon_event::reshade_set_technique_state>(onReshadeSetTechniqueState);
		reshade::unregister_event<reshade::addon_event::bind_pipeline>(onBindPipeline);
		reshade::unregister_event<reshade::addon_event::init_command_list>(onInitCommandList);
		reshade::unregister_event<reshade::addon_event::destroy_command_list>(onDestroyCommandList);
		reshade::unregister_event<reshade::addon_event::reset_command_list>(onResetCommandList);
		reshade::unregister_event<reshade::addon_event::init_device>(onInitDevice);
		reshade::unregister_event<reshade::addon_event::destroy_device>(onDestroyDevice);
		reshade::unregister_event<reshade::addon_event::present>(onPresent);
		reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(onBindRenderTargetsAndDepthStencil);
		reshade::unregister_event<reshade::addon_event::init_effect_runtime>(onInitEffectRuntime);
		reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(onDestroyEffectRuntime);
		reshade::unregister_event<reshade::addon_event::destroy_resource_view>(onDestroyResourceView);
		reshade::unregister_event<reshade::addon_event::init_resource_view>(onInitResourceView);
		reshade::unregister_event<reshade::addon_event::destroy_resource>(onDestroyResource);
		reshade::unregister_overlay(nullptr, &displaySettings);
		reshade::unregister_addon(hModule);
		break;
	}

	return TRUE;
}
