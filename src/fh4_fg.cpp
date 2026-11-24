#include <windows.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include <MinHook.h>
#include <xell/xell_d3d12.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xess_fg/xefg_swapchain_debug.h>

#include "fh4_fg.hpp"

namespace fh4::fg {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using FactoryCreate = decltype(&CreateDXGIFactory2);
using CreateForHwnd = HRESULT (STDMETHODCALLTYPE *)(IDXGIFactory2 *, IUnknown *, HWND,
    const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
constexpr auto read_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr std::array<Capture, 3> kinds{depth, velocity, hudless};
constexpr std::array<xefg_swapchain_resource_type_t, 3> types{
    XEFG_SWAPCHAIN_RES_DEPTH, XEFG_SWAPCHAIN_RES_MOTION_VECTOR, XEFG_SWAPCHAIN_RES_HUDLESS_COLOR
};
constexpr auto hdr_format = DXGI_FORMAT_R10G10B10A2_UNORM;
constexpr UINT hdr_width = 3840, hdr_height = 2400;
constexpr UINT probe_width = 60, probe_height = 38, probe_pitch = 256;

class HudLayer {
public:
    ID3D12Resource *render(ID3D12CommandQueue *queue, ID3D12Resource *backbuffer, ID3D12Resource *scene);

private:
    struct Slot {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12Resource> composed, hud, probe;
        UINT64 serial = 0;
    };
    bool initialize(ID3D12Device *device);
    std::array<Slot, 4> slots_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12DescriptorHeap> heap_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pipeline_;
    ComPtr<ID3D12GraphicsCommandList> commands_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12Resource> probe_readback_;
    UINT64 probe_serial_ = 0;
    UINT64 serial_ = 1;
    bool attempted_ = false;
    bool ready_ = false;
};

constexpr char hud_shader[] = R"hlsl(
Texture2D<float4> finalColor : register(t0);
Texture2D<float4> sceneColor : register(t1);
RWTexture2D<float4> hudColor : register(u0);
RWTexture2D<uint> hudProbe : register(u1);

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
    float3 native = finalColor.Load(int3(pixel, 0)).rgb;
    float3 scene = sceneColor.Load(int3(pixel, 0)).rgb;
    // With 10-bit PQ input, half a code value separates an unchanged pixel
    // from a pixel drawn after tone mapping. Binary alpha preserves the
    // current native UI sample without asking the interpolator to infer it.
    bool hasHud = any(abs(native - scene) > (0.5 / 1023.0));
    hudColor[pixel] = hasHud ? float4(native, 1.0) : float4(0, 0, 0, 0);
    if (all((pixel & 63) == 0))
        hudProbe[pixel >> 6] = hasHud ? 1 : 0;
}
)hlsl";

struct XeApi {
    decltype(&xefgSwapChainD3D12CreateContext) create = nullptr;
    decltype(&xefgSwapChainD3D12InitFromSwapChainDesc) init = nullptr;
    decltype(&xefgSwapChainD3D12GetSwapChainPtr) get_swapchain = nullptr;
    decltype(&xefgSwapChainSetLatencyReduction) link_xell = nullptr;
    decltype(&xefgSwapChainSetUiCompositionState) ui = nullptr;
    decltype(&xefgSwapChainSetEnabled) enable = nullptr;
    decltype(&xefgSwapChainDestroy) destroy = nullptr;
    decltype(&xefgSwapChainTagFrameConstants) constants = nullptr;
    decltype(&xefgSwapChainD3D12TagFrameResource) resource = nullptr;
    decltype(&xefgSwapChainSetPresentId) present_id = nullptr;
    decltype(&xefgSwapChainGetLastPresentStatus) status = nullptr;
    decltype(&xefgSwapChainEnableDebugFeature) debug = nullptr;
    decltype(&xefgSwapChainSetLoggingCallback) logging = nullptr;
    decltype(&xellD3D12CreateContext) create_xell = nullptr;
    decltype(&xellSetSleepMode) sleep_mode = nullptr;
    decltype(&xellAddMarkerData) marker = nullptr;
    decltype(&xellDestroyContext) destroy_xell = nullptr;

    bool load();
};

XeApi api;
CreateForHwnd original_create = nullptr;
thread_local bool creating_proxy = false;
std::atomic_bool install_attempted{false}, adopted{false}, wanted{true}, only_generated{false};
xefg_swapchain_handle_t context = nullptr;
xell_context_handle_t xell = nullptr;
ID3D12CommandQueue *target_queue = nullptr;
HWND target_window = nullptr;
std::mutex captures_mutex;
std::array<ComPtr<ID3D12Resource>, 3> captures;
HudLayer hud_layer;
std::atomic_uint submitted_mask{0};
bool enabled = false, debug_enabled = false;
uint32_t frame_id = 0;
unsigned real_frames = 0, generated_frames = 0, batches = 0, hdr_frames = 0;
std::atomic_uint overlay_generated{0}, overlay_real{0};
Clock::time_point last_frame{}, measure_start{};
std::filesystem::path control_path;
std::filesystem::file_time_type control_time{};
Clock::time_point next_control{};

void log(const char *message)
{
    reshade::log::message(reshade::log::level::info, message);
}

bool success(int result, const char *step)
{
    if (result >= 0)
        return true;
    char message[160];
    std::snprintf(message, sizeof(message), "[FH4 FG] %s returned %d.", step, result);
    log(message);
    return false;
}

void xe_log(const char *message, xefg_swapchain_logging_level_t, void *)
{
    log(message);
}

template <class Function>
Function entry(HMODULE module, const char *name)
{
#pragma warning(suppress: 4191)
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

bool XeApi::load()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const auto local = std::filesystem::path(path).parent_path() / L"FH4Dlss5";
    control_path = local / L"frame-generation.ini";
    const auto ll = LoadLibraryExW((local / L"libxell.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    const auto fg = LoadLibraryExW((local / L"libxess_fg.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!ll || !fg)
        return false;

    create = entry<decltype(create)>(fg, "xefgSwapChainD3D12CreateContext");
    init = entry<decltype(init)>(fg, "xefgSwapChainD3D12InitFromSwapChainDesc");
    get_swapchain = entry<decltype(get_swapchain)>(fg, "xefgSwapChainD3D12GetSwapChainPtr");
    link_xell = entry<decltype(link_xell)>(fg, "xefgSwapChainSetLatencyReduction");
    ui = entry<decltype(ui)>(fg, "xefgSwapChainSetUiCompositionState");
    enable = entry<decltype(enable)>(fg, "xefgSwapChainSetEnabled");
    destroy = entry<decltype(destroy)>(fg, "xefgSwapChainDestroy");
    constants = entry<decltype(constants)>(fg, "xefgSwapChainTagFrameConstants");
    resource = entry<decltype(resource)>(fg, "xefgSwapChainD3D12TagFrameResource");
    present_id = entry<decltype(present_id)>(fg, "xefgSwapChainSetPresentId");
    status = entry<decltype(status)>(fg, "xefgSwapChainGetLastPresentStatus");
    debug = entry<decltype(debug)>(fg, "xefgSwapChainEnableDebugFeature");
    logging = entry<decltype(logging)>(fg, "xefgSwapChainSetLoggingCallback");
    create_xell = entry<decltype(create_xell)>(ll, "xellD3D12CreateContext");
    sleep_mode = entry<decltype(sleep_mode)>(ll, "xellSetSleepMode");
    marker = entry<decltype(marker)>(ll, "xellAddMarkerData");
    destroy_xell = entry<decltype(destroy_xell)>(ll, "xellDestroyContext");
    return create && init && get_swapchain && link_xell && ui && enable && destroy && constants &&
        resource && present_id && status && create_xell && sleep_mode && marker && destroy_xell;
}

HRESULT STDMETHODCALLTYPE create_for_hwnd(IDXGIFactory2 *factory, IUnknown *device, HWND window,
    const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen,
    IDXGIOutput *output, IDXGISwapChain1 **swapchain)
{
    const auto native = [&]() { return original_create(factory, device, window, desc, fullscreen, output, swapchain); };
    if (!swapchain || !desc || !device || !window || adopted.load() || creating_proxy ||
        desc->Format != hdr_format || desc->Width < 1280 ||
        desc->SampleDesc.Count != 1 || (fullscreen && !fullscreen->Windowed))
        return native();
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != GetCurrentProcessId() || GetAncestor(window, GA_ROOT) != window)
        return native();

    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Device> d3d_device;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) ||
        FAILED(queue->GetDevice(IID_PPV_ARGS(&d3d_device))))
        return native();

    xefg_swapchain_handle_t local_context = nullptr;
    xell_context_handle_t local_xell = nullptr;
    ComPtr<IDXGISwapChain1> proxy;
    xell_sleep_params_t sleep = {};
    sleep.bLowLatencyMode = 1;
    xefg_swapchain_d3d12_init_params_t config = {};
    config.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;
    config.maxInterpolatedFrames = 1;
    config.creationNodeMask = 1;
    config.visibleNodeMask = 1;
    config.uiMode = XEFG_SWAPCHAIN_UI_MODE_HUDLESS_UITEXTURE;
    creating_proxy = true;
    log("[FH4 FG] Creating Intel XeSS-FG context.");
    const bool initialized = success(api.create(d3d_device.Get(), &local_context), "create XeSS-FG") &&
        (!api.logging || success(api.logging(local_context, XEFG_SWAPCHAIN_LOGGING_LEVEL_INFO, xe_log, nullptr),
                                 "set graphics logging")) &&
        (log("[FH4 FG] Creating XeLL context."), true) &&
        success(api.create_xell(d3d_device.Get(), &local_xell), "create XeLL") &&
        success(api.sleep_mode(local_xell, &sleep), "enable XeLL") &&
        success(api.link_xell(local_context, local_xell), "attach XeLL") &&
        (log("[FH4 FG] Initializing XeSS-FG swapchain from the game descriptor."), true) &&
        success(api.init(local_context, window, desc, fullscreen, queue.Get(), factory, &config), "create swapchain") &&
        success(api.get_swapchain(local_context, IID_PPV_ARGS(&proxy)), "get XeSS-FG swapchain") &&
        success(api.ui(local_context, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED), "enable HUD composition");
    creating_proxy = false;
    if (!initialized) {
        proxy.Reset();
        if (local_context)
            api.destroy(local_context);
        if (local_xell)
            api.destroy_xell(local_xell);
        return native();
    }

    *swapchain = proxy.Detach();
    context = local_context;
    xell = local_xell;
    target_queue = queue.Get();
    target_window = window;
    adopted.store(true, std::memory_order_release);
    log("[FH4 FG] Adopted FH4 HDR swapchain. XeSS-FG 2x, HUDLESS_UITEXTURE with native HDR UI pixels.");
    return S_OK;
}

void transition(ID3D12GraphicsCommandList *commands, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    commands->ResourceBarrier(1, &barrier);
}

bool HudLayer::initialize(ID3D12Device *device)
{
    device_ = device;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT format = {hdr_format};
    if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format, sizeof(format))) ||
        !(format.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
        return false;

    D3D12_DESCRIPTOR_RANGE ranges[] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 2},
    };
    D3D12_ROOT_PARAMETER binding = {};
    binding.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    binding.DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC desc = {1, &binding, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> signature, shader;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, nullptr)) ||
        FAILED(device_->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                            IID_PPV_ARGS(&root_))) ||
        FAILED(D3DCompile(hud_shader, sizeof(hud_shader) - 1, "FH4 HUD", nullptr, nullptr,
                          "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, nullptr)))
        return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
    pipeline.pRootSignature = root_.Get();
    pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    D3D12_DESCRIPTOR_HEAP_DESC heap = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        4 * static_cast<UINT>(slots_.size()), D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device_->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pipeline_))) ||
        FAILED(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_))) ||
        FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
        return false;

    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    properties.CreationNodeMask = properties.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC texture = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, hdr_width,
        hdr_height, 1, 1, hdr_format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    for (auto &slot : slots_) {
        texture.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
            FAILED(device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &texture, read_state,
                                                     nullptr, IID_PPV_ARGS(&slot.composed))))
            return false;
        texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &texture, read_state,
                                                     nullptr, IID_PPV_ARGS(&slot.hud))))
            return false;
        auto probe = texture;
        probe.Width = probe_width;
        probe.Height = probe_height;
        probe.Format = DXGI_FORMAT_R32_UINT;
        if (FAILED(device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &probe,
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&slot.probe))))
            return false;
    }
    D3D12_RESOURCE_DESC buffer = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, probe_pitch * probe_height,
        1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    properties.Type = D3D12_HEAP_TYPE_READBACK;
    if (FAILED(device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&probe_readback_))))
        return false;
    return SUCCEEDED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                         slots_[0].allocator.Get(), nullptr, IID_PPV_ARGS(&commands_))) &&
           SUCCEEDED(commands_->Close());
}

ID3D12Resource *HudLayer::render(ID3D12CommandQueue *queue, ID3D12Resource *backbuffer, ID3D12Resource *scene)
{
    if (!attempted_) {
        attempted_ = true;
        ComPtr<ID3D12Device> device;
        ready_ = SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && initialize(device.Get());
        log(ready_ ? "[FH4 FG] Native HDR UI layer initialized."
                   : "[FH4 FG] Native HDR UI layer initialization failed.");
    }
    if (!ready_ || !backbuffer || !scene || backbuffer->GetDesc().Format != hdr_format)
        return nullptr;
    if (probe_serial_ && fence_->GetCompletedValue() >= probe_serial_) {
        void *pixels = nullptr;
        D3D12_RANGE range = {0, probe_pitch * probe_height};
        if (SUCCEEDED(probe_readback_->Map(0, &range, &pixels))) {
            unsigned hud_pixels = 0;
            const auto *words = static_cast<const uint32_t *>(pixels);
            for (UINT y = 0; y < probe_height; ++y) {
                for (UINT x = 0; x < probe_width; ++x)
                    hud_pixels += words[y * (probe_pitch / sizeof(uint32_t)) + x] != 0;
            }
            range = {0, 0};
            probe_readback_->Unmap(0, &range);
            char line[120];
            std::snprintf(line, sizeof(line), "[FH4 FG] HUD coverage sample: %.1f%% of output pixels.",
                          100.0 * hud_pixels / (probe_width * probe_height));
            log(line);
        }
        probe_serial_ = 0;
    }
    const auto index = static_cast<UINT>(serial_ % slots_.size());
    auto &slot = slots_[index];
    if (fence_->GetCompletedValue() < slot.serial ||
        FAILED(slot.allocator->Reset()) || FAILED(commands_->Reset(slot.allocator.Get(), pipeline_.Get())))
        return nullptr;

    const UINT stride = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = heap_->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(index) * 4 * stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = hdr_format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(slot.composed.Get(), &srv, cpu);
    cpu.ptr += stride;
    device_->CreateShaderResourceView(scene, &srv, cpu);
    cpu.ptr += stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = hdr_format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(slot.hud.Get(), nullptr, &uav, cpu);
    cpu.ptr += stride;
    uav.Format = DXGI_FORMAT_R32_UINT;
    device_->CreateUnorderedAccessView(slot.probe.Get(), nullptr, &uav, cpu);

    transition(commands_.Get(), backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(commands_.Get(), slot.composed.Get(), read_state, D3D12_RESOURCE_STATE_COPY_DEST);
    commands_->CopyResource(slot.composed.Get(), backbuffer);
    transition(commands_.Get(), slot.composed.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read_state);
    transition(commands_.Get(), backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    transition(commands_.Get(), slot.hud.Get(), read_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap *heaps[] = {heap_.Get()};
    commands_->SetDescriptorHeaps(1, heaps);
    commands_->SetComputeRootSignature(root_.Get());
    auto gpu = heap_->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(index) * 4 * stride;
    commands_->SetComputeRootDescriptorTable(0, gpu);
    commands_->Dispatch(hdr_width / 8, hdr_height / 8, 1);
    transition(commands_.Get(), slot.hud.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read_state);
    const bool measure = !probe_serial_ && (serial_ == 1 || serial_ % 240 == 0);
    if (measure) {
        D3D12_TEXTURE_COPY_LOCATION from = {};
        from.pResource = slot.probe.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to = {};
        to.pResource = probe_readback_.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_UINT, probe_width, probe_height, 1, probe_pitch};
        transition(commands_.Get(), slot.probe.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(commands_.Get(), slot.probe.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (FAILED(commands_->Close()))
        return nullptr;

    ID3D12CommandList *lists[] = {commands_.Get()};
    queue->ExecuteCommandLists(1, lists);
    slot.serial = serial_++;
    ready_ = SUCCEEDED(queue->Signal(fence_.Get(), slot.serial));
    if (ready_ && measure)
        probe_serial_ = slot.serial;
    return ready_ ? slot.hud.Get() : nullptr;
}

bool is_target(reshade::api::command_queue *queue, reshade::api::swapchain *chain)
{
    if (!adopted.load(std::memory_order_acquire) ||
        queue->get_native() != reinterpret_cast<uint64_t>(target_queue))
        return false;
    const auto native = reinterpret_cast<IDXGISwapChain1 *>(chain->get_native());
    HWND window = nullptr;
    return native && SUCCEEDED(native->GetHwnd(&window)) && window == target_window;
}

void read_controls(Clock::time_point now)
{
    if (now < next_control)
        return;
    next_control = now + std::chrono::milliseconds(500);
    std::error_code error;
    const auto write_time = std::filesystem::last_write_time(control_path, error);
    if (error || write_time == control_time)
        return;
    control_time = write_time;
    wanted.store(GetPrivateProfileIntW(L"FrameGeneration", L"Enabled", 1, control_path.c_str()) != 0,
                 std::memory_order_relaxed);
    only_generated.store(GetPrivateProfileIntW(L"FrameGeneration", L"PreviewGenerated", 0, control_path.c_str()) != 0,
                         std::memory_order_relaxed);
}

} // namespace

void install(reshade::api::device *device)
{
    if (device->get_api() != reshade::api::device_api::d3d12 || install_attempted.exchange(true))
        return;
    if (!api.load()) {
        log("[FH4 FG] Intel runtime could not be loaded.");
        return;
    }
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW(system, MAX_PATH);
    const auto path = std::filesystem::path(system) / L"dxgi.dll";
    const auto dxgi = GetModuleHandleW(path.c_str());
    const auto create = dxgi ? entry<FactoryCreate>(dxgi, "CreateDXGIFactory2") : nullptr;
    ComPtr<IDXGIFactory2> factory, native_factory;
    ComPtr<IDXGIAdapter> adapter;
    if (!create || FAILED(create(0, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapters(0, &adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&native_factory)))) {
        log("[FH4 FG] Could not acquire native DXGI factory.");
        return;
    }
    void *method = (*reinterpret_cast<void ***>(native_factory.Get()))[15];
    HMODULE code_module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(method), &code_module);
    if (code_module != dxgi) {
        log("[FH4 FG] Native DXGI factory target was unavailable.");
        return;
    }
    const auto state = MH_Initialize();
    if ((state != MH_OK && state != MH_ERROR_ALREADY_INITIALIZED) ||
        MH_CreateHook(method, reinterpret_cast<LPVOID>(&create_for_hwnd),
                      reinterpret_cast<LPVOID *>(&original_create)) != MH_OK ||
        MH_EnableHook(method) != MH_OK) {
        log("[FH4 FG] Failed to register the DXGI swapchain extension.");
        return;
    }
    log("[FH4 FG] Waiting for the native FH4 HDR swapchain.");
}

bool capture(ID3D12GraphicsCommandList *commands, ID3D12Resource *source,
             D3D12_RESOURCE_STATES state, Capture kind)
{
    if (!adopted.load(std::memory_order_acquire) || !wanted.load(std::memory_order_relaxed) || !source)
        return false;
    const auto desc = source->GetDesc();
    const auto index = kind == depth ? 0 : kind == velocity ? 1 : 2;
    constexpr DXGI_FORMAT formats[] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM};
    if (desc.Format != formats[index] || desc.SampleDesc.Count != 1 ||
        desc.Width != (kind == hudless ? 3840u : 1920u) || desc.Height != (kind == hudless ? 2400u : 1200u))
        return false;

    std::lock_guard lock(captures_mutex);
    auto &target = captures[index];
    if (!target) {
        ComPtr<ID3D12Device> device;
        if (FAILED(commands->GetDevice(IID_PPV_ARGS(&device))))
            return false;
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        auto texture = desc;
        texture.Flags = D3D12_RESOURCE_FLAG_NONE;
        texture.Alignment = 0;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture, read_state,
                                                   nullptr, IID_PPV_ARGS(&target))))
            return false;
    }
    transition(commands, source, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(commands, target.Get(), read_state, D3D12_RESOURCE_STATE_COPY_DEST);
    commands->CopyResource(target.Get(), source);
    transition(commands, target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read_state);
    transition(commands, source, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    return true;
}

void submitted(reshade::api::command_queue *queue, unsigned flags)
{
    if (adopted.load(std::memory_order_acquire) &&
        queue->get_native() == reinterpret_cast<uint64_t>(target_queue))
        submitted_mask.fetch_or(flags, std::memory_order_relaxed);
}

void present(reshade::api::command_queue *queue, reshade::api::swapchain *chain)
{
    if (!is_target(queue, chain))
        return;
    const auto flags = submitted_mask.exchange(0, std::memory_order_relaxed);
    const auto now = Clock::now();
    read_controls(now);
    const float dt = std::chrono::duration<float, std::milli>(now - last_frame).count();
    last_frame = now;
    const auto backbuffer_handle = chain->get_current_back_buffer();
    const auto backbuffer = chain->get_device()->get_resource_desc(backbuffer_handle);
    const bool full_size = backbuffer.texture.width == hdr_width && backbuffer.texture.height == hdr_height;
    const bool ready = full_size && wanted.load(std::memory_order_relaxed) && flags == (depth | velocity | hudless);
    ++frame_id;
    bool tagged = false;
    if (ready) {
        std::lock_guard lock(captures_mutex);
        auto *native_buffer = reinterpret_cast<ID3D12Resource *>(backbuffer_handle.handle);
        const auto ui = hud_layer.render(target_queue, native_buffer, captures[2].Get());
        xefg_swapchain_frame_constant_data_t constants = {};
        for (const auto index : {0, 5, 10, 15}) {
            constants.viewMatrix[index] = 1.0f;
            constants.projectionMatrix[index] = 1.0f;
        }
        constants.motionVectorScaleX = 960.0f;
        constants.motionVectorScaleY = 600.0f;
        constants.resetHistory = !enabled || dt > 100.0f;
        constants.frameRenderTime = dt > 0.0f && dt < 100.0f ? dt : 22.0f;
        tagged = ui && success(api.constants(context, frame_id, &constants), "tag frame constants");
        for (size_t i = 0; tagged && i < kinds.size(); ++i) {
            const unsigned width = kinds[i] == hudless ? 3840u : 1920u;
            const unsigned height = kinds[i] == hudless ? 2400u : 1200u;
            const xefg_swapchain_d3d12_resource_data_t entry{
                types[i], XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT, {}, {width, height},
                captures[i].Get(), read_state
            };
            tagged = success(api.resource(context, nullptr, frame_id, &entry), "tag captured resource");
        }
        if (tagged) {
            const xefg_swapchain_d3d12_resource_data_t entry{
                XEFG_SWAPCHAIN_RES_UI, XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT, {},
                {hdr_width, hdr_height}, ui, read_state
            };
            tagged = success(api.resource(context, nullptr, frame_id, &entry), "tag native HDR UI");
        }
    }
    if (enabled != tagged && success(api.enable(context, tagged), "change frame generation")) {
        enabled = tagged;
        log(enabled ? "[FH4 FG] Enabled with FH4 depth, velocity, pre-HUD HDR image and native HUD."
                    : "[FH4 FG] Disabled while a matching scene is unavailable.");
    }
    const bool debug = only_generated.load(std::memory_order_relaxed);
    if (api.debug && debug != debug_enabled &&
        success(api.debug(context, XEFG_SWAPCHAIN_DEBUG_FEATURE_SHOW_ONLY_INTERPOLATION, debug, nullptr),
                "change generated-frame preview"))
    {
        debug_enabled = debug;
        log(debug ? "[FH4 FG] Preview shows generated frames only."
                  : "[FH4 FG] Preview disabled. Presenting real and generated frames.");
    }
    if (enabled) {
        api.present_id(context, frame_id);
        for (const auto marker : {XELL_SIMULATION_START, XELL_SIMULATION_END, XELL_RENDERSUBMIT_START,
                                  XELL_RENDERSUBMIT_END, XELL_PRESENT_START})
            api.marker(xell, frame_id, marker);
    }
}

void finish_present(reshade::api::command_queue *queue, reshade::api::swapchain *chain)
{
    if (!is_target(queue, chain) || !enabled)
        return;
    api.marker(xell, frame_id, XELL_PRESENT_END);
    xefg_swapchain_present_status_t status = {};
    if (api.status(context, &status) != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        return;
    const auto now = Clock::now();
    if (!batches)
        measure_start = now;
    ++batches;
    ++real_frames;
    ++hdr_frames;
    if (status.framesPresented > 1 && status.frameGenResult == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        generated_frames += status.framesPresented - 1;
    overlay_generated.store(generated_frames, std::memory_order_relaxed);
    overlay_real.store(real_frames, std::memory_order_relaxed);
    if (batches == 1 || batches % 120 == 0) {
        const double elapsed = std::chrono::duration<double>(now - measure_start).count();
        char line[260];
        std::snprintf(line, sizeof(line),
            "[FH4 FG] real=%u generated=%u hdr=%u last_present=%u result=%d mean_output_fps=%.1f preview=%u",
            real_frames, generated_frames, hdr_frames, status.framesPresented, status.frameGenResult,
            elapsed > 0.0 ? (real_frames + generated_frames - 1) / elapsed : 0.0, debug_enabled);
        log(line);
    }
}

void overlay()
{
    bool state = wanted.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("XeSS Frame Generation 2x (HUD separated)", &state))
        wanted.store(state, std::memory_order_relaxed);
    ImGui::Text("Real frames: %u  Generated frames: %u", overlay_real.load(), overlay_generated.load());
    if (api.debug) {
        bool debug = only_generated.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Preview generated frames only", &debug))
            only_generated.store(debug, std::memory_order_relaxed);
    }
}

}
