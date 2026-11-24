#pragma once

#include <d3d12.h>

namespace reshade::api {
struct device;
struct command_queue;
struct swapchain;
}

namespace fh4::fg {

enum Capture : unsigned { depth = 1, velocity = 2, hudless = 4 };

void install(reshade::api::device *device);
bool capture(ID3D12GraphicsCommandList *commands, ID3D12Resource *source,
             D3D12_RESOURCE_STATES state, Capture kind);
void submitted(reshade::api::command_queue *queue, unsigned captures);
void present(reshade::api::command_queue *queue, reshade::api::swapchain *chain);
void finish_present(reshade::api::command_queue *queue, reshade::api::swapchain *chain);
void overlay();

}
