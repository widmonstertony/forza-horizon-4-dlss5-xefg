#include <windows.h>

#include <array>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "ngx_scene.hpp"
#include "fh4_fg.hpp"

extern "C" __declspec(dllexport) const char *NAME = "FH4 DLSS5";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "NVIDIA neural rendering at FH4's native scene resolve.";

namespace {
using namespace reshade::api;

enum class Pass { none, resolve, velocity, composite };

constexpr struct {
    std::array<unsigned char, 16> checksum;
    Pass pass;
} signatures[] = {
    {{0x88, 0x34, 0xb7, 0x66, 0xb9, 0xfa, 0x27, 0xae, 0x4d, 0xf6, 0x68, 0x35, 0xea, 0xd4, 0x87, 0xfd}, Pass::resolve},
    {{0xc0, 0x0b, 0xd3, 0xd5, 0x52, 0x7d, 0x63, 0x4b, 0xd9, 0xbf, 0xc5, 0x53, 0x5e, 0x93, 0xb9, 0x73}, Pass::velocity},
    {{0x8a, 0xc3, 0xd3, 0x43, 0xfc, 0xb6, 0x85, 0xc2, 0x96, 0x1d, 0x19, 0x70, 0x27, 0x6a, 0xc0, 0xba}, Pass::composite}
};

struct __declspec(uuid("c364468c-8d53-40ed-b490-6b6eaa1a60a2")) PassState {
    Pass bound = Pass::none;
    Pass pending_capture = Pass::none;
    resource capture_target = {};
    unsigned captures = 0;
    resource_view rtv = {}, dsv = {};
    viewport area = {};
    resource scene = {}, depth = {};
    resource_usage scene_state = resource_usage::undefined;
    resource_usage depth_state = resource_usage::undefined;
};

std::shared_mutex pipelines_mutex;
std::unordered_map<uint64_t, Pass> known_pipelines;
HMODULE addon_module = nullptr;

void on_pipeline(device *dev, pipeline_layout, uint32_t count, const pipeline_subobject *data, pipeline handle)
{
    if (dev->get_api() != device_api::d3d12)
        return;
    for (uint32_t i = 0; i < count; ++i) {
        if (data[i].type != pipeline_subobject_type::pixel_shader || !data[i].data)
            continue;
        const auto &shader = *static_cast<const shader_desc *>(data[i].data);
        const auto *bytes = static_cast<const unsigned char *>(shader.code);
        if (!bytes || shader.code_size < 20 || std::memcmp(bytes, "DXBC", 4) != 0)
            continue;
        for (const auto &signature : signatures) {
            if (std::memcmp(bytes + 4, signature.checksum.data(), signature.checksum.size()) == 0) {
                std::unique_lock lock(pipelines_mutex);
                known_pipelines.insert_or_assign(handle.handle, signature.pass);
                return;
            }
        }
    }
}

void on_destroy_pipeline(device *, pipeline handle)
{
    std::unique_lock lock(pipelines_mutex);
    known_pipelines.erase(handle.handle);
}

void on_bind_pipeline(command_list *list, pipeline_stage stage, pipeline handle)
{
    if (static_cast<uint32_t>(stage & pipeline_stage::pixel_shader) != 0) {
        if (auto *state = list->get_private_data<PassState>()) {
            std::shared_lock lock(pipelines_mutex);
            const auto entry = known_pipelines.find(handle.handle);
            state->bound = entry == known_pipelines.end() ? Pass::none : entry->second;
        }
    }
}

void on_init_list(command_list *list)
{
    if (list->get_device()->get_api() == device_api::d3d12)
        list->create_private_data<PassState>();
}

void on_destroy_list(command_list *list)
{
    if (list->get_device()->get_api() == device_api::d3d12)
        list->destroy_private_data<PassState>();
}

void on_reset(command_list *list)
{
    if (auto *state = list->get_private_data<PassState>())
        *state = {};
}

void on_bind_targets(command_list *list, uint32_t count, const resource_view *targets, resource_view depth)
{
    if (auto *state = list->get_private_data<PassState>()) {
        state->rtv = count == 1 ? targets[0] : resource_view{};
        state->dsv = depth;
    }
}

void on_viewport(command_list *list, uint32_t first, uint32_t count, const viewport *views)
{
    if (first == 0 && count) {
        if (auto *state = list->get_private_data<PassState>())
            state->area = views[0];
    }
}

bool on_draw(command_list *list, uint32_t vertices, uint32_t instances, uint32_t, uint32_t)
{
    auto *state = list->get_private_data<PassState>();
    if (!state || state->bound == Pass::none || vertices != 3 || instances != 1 || !state->rtv.handle)
        return false;
    const auto dev = list->get_device();
    const bool is_velocity = state->bound == Pass::velocity;
    if (state->area.width != (is_velocity ? 1920 : 3840) ||
        state->area.height != (is_velocity ? 1200 : 2400))
        return false;
    if (state->bound != Pass::resolve) {
        state->pending_capture = state->bound;
        state->capture_target = dev->get_resource_from_view(state->rtv);
        return false;
    }
    if (!fh4::enabled() || !state->dsv.handle)
        return false;
    state->scene = dev->get_resource_from_view(state->rtv);
    state->depth = dev->get_resource_from_view(state->dsv);
    state->scene_state = resource_usage::render_target;
    state->depth_state = resource_usage::depth_stencil_write;
    return false;
}

void on_barrier(command_list *list, uint32_t count, const resource *resources,
                const resource_usage *before, const resource_usage *after)
{
    auto *state = list->get_private_data<PassState>();
    if (!state)
        return;
    for (uint32_t i = 0; i < count; ++i) {
        if (state->pending_capture != Pass::none && resources[i].handle == state->capture_target.handle &&
            before[i] == resource_usage::render_target && after[i] == resource_usage::shader_resource) {
            const auto kind = state->pending_capture == Pass::velocity ? fh4::fg::velocity : fh4::fg::hudless;
            auto *commands = reinterpret_cast<ID3D12GraphicsCommandList *>(list->get_native());
            auto *source = reinterpret_cast<ID3D12Resource *>(resources[i].handle);
            constexpr auto shader_read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            if (fh4::fg::capture(commands, source, shader_read, kind))
                state->captures |= kind;
            state->pending_capture = Pass::none;
        }
        if (resources[i].handle == state->scene.handle)
            state->scene_state = after[i];
        if (resources[i].handle == state->depth.handle)
            state->depth_state = after[i];
    }
}

void on_close(command_list *list)
{
    auto *state = list->get_private_data<PassState>();
    if (!state || !state->scene.handle || !state->depth.handle ||
        state->scene_state != resource_usage::shader_resource || state->depth_state != resource_usage::shader_resource)
        return;
    auto *commands = reinterpret_cast<ID3D12GraphicsCommandList *>(list->get_native());
    if (commands && commands->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        bool copied_depth = false;
        fh4::process_scene(commands, reinterpret_cast<ID3D12Resource *>(state->scene.handle),
            reinterpret_cast<ID3D12Resource *>(state->depth.handle), addon_module, copied_depth);
        if (copied_depth)
            state->captures |= fh4::fg::depth;
    }
    state->scene = {};
    state->depth = {};
}

void on_execute(command_queue *queue, command_list *list)
{
    if (auto *state = list->get_private_data<PassState>(); state && state->captures)
        fh4::fg::submitted(queue, state->captures);
}

void on_present(command_queue *queue, swapchain *chain, const rect *, const rect *, uint32_t, const rect *)
{
    fh4::fg::present(queue, chain);
}

void draw_overlay(effect_runtime *)
{
    bool enabled = fh4::enabled();
    if (ImGui::Checkbox("DLSS5 Movie + DLSS Super Resolution", &enabled))
        fh4::set_enabled(enabled);
    ImGui::Text("2 neural passes, 1920 x 1200 to 3840 x 2400");
    ImGui::Text("Scene frames recorded: %u", fh4::recorded_frames());
    fh4::fg::overlay();
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    using namespace reshade;
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        const auto *filename = std::wcsrchr(path, L'\\');
        if (!filename || _wcsicmp(filename + 1, L"ForzaHorizon4.exe") != 0)
            return FALSE;
        if (!register_addon(instance))
            return FALSE;
        addon_module = instance;
        register_event<addon_event::init_device>(fh4::fg::install);
        register_event<addon_event::init_pipeline>(on_pipeline);
        register_event<addon_event::destroy_pipeline>(on_destroy_pipeline);
        register_event<addon_event::bind_pipeline>(on_bind_pipeline);
        register_event<addon_event::init_command_list>(on_init_list);
        register_event<addon_event::destroy_command_list>(on_destroy_list);
        register_event<addon_event::reset_command_list>(on_reset);
        register_event<addon_event::bind_render_targets_and_depth_stencil>(on_bind_targets);
        register_event<addon_event::bind_viewports>(on_viewport);
        register_event<addon_event::draw>(on_draw);
        register_event<addon_event::barrier>(on_barrier);
        register_event<addon_event::close_command_list>(on_close);
        register_event<addon_event::execute_command_list>(on_execute);
        register_event<addon_event::present>(on_present);
        register_event<addon_event::finish_present>(fh4::fg::finish_present);
        register_overlay("FH4 DLSS5", draw_overlay);
        log::message(log::level::info, "[FH4 DLSS5] Waiting for the 3840x2400 scene resolve.");
    } else if (reason == DLL_PROCESS_DETACH) {
        unregister_addon(instance);
    }
    return TRUE;
}
