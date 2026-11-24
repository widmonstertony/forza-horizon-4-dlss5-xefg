#pragma once

#include <d3d12.h>
#include <windows.h>

namespace fh4 {

// Records the neural pass after the game's MSAA/depth resolve. The source is
// linear scene color, before the game's exposure, tone mapping and HUD.
bool process_scene(ID3D12GraphicsCommandList *commands, ID3D12Resource *scene,
                   ID3D12Resource *depth, HMODULE addon, bool &captured_depth);
void set_enabled(bool enabled);
bool enabled();
unsigned recorded_frames();

}
