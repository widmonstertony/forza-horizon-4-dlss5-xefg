#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>
#include <reshade.hpp>

#include "ngx_scene.hpp"
#include "fh4_fg.hpp"

namespace fh4 {
namespace {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

constexpr UINT iw = 1920, ih = 1200, ow = 3840, oh = 2400;
constexpr auto color_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr auto game_format = DXGI_FORMAT_R11G11B10_FLOAT;
constexpr auto read_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto game_read = read_state | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr auto uav_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto nr_feature = static_cast<NVSDK_NGX_Feature>(0x12);
constexpr unsigned long long application_id = 0x0876232CULL;
constexpr int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
    NVSDK_NGX_DLSS_Feature_Flags_AutoExposure | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

using CoreInit = NVSDK_NGX_Result (NVSDK_CONV *)(unsigned long long, const wchar_t *, ID3D12Device *, NVSDK_NGX_Version);
using SnippetInit = NVSDK_NGX_Result (NVSDK_CONV *)(unsigned long long, const wchar_t *, ID3D12Device *,
    NVSDK_NGX_Version, const NVSDK_NGX_Parameter *);
using GetParameters = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Parameter **);
using Populate = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Parameter *);
using Create = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature,
    NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
using Evaluate = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *,
    const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback_C);
using BridgeInit = NVSDK_NGX_Result (NVSDK_CONV *)(SnippetInit, unsigned long long, const wchar_t *,
    ID3D12Device *, NVSDK_NGX_Version, const NVSDK_NGX_Parameter *);
using BridgePopulate = NVSDK_NGX_Result (NVSDK_CONV *)(Populate, NVSDK_NGX_Parameter *);
using BridgeCreate = NVSDK_NGX_Result (NVSDK_CONV *)(Create, ID3D12GraphicsCommandList *, NVSDK_NGX_Feature,
    NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
using BridgeEvaluate = NVSDK_NGX_Result (NVSDK_CONV *)(Evaluate, ID3D12GraphicsCommandList *,
    const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback_C);

std::atomic_bool active{true};
std::atomic_uint frames{0};

void log(const char *message)
{
    reshade::log::message(reshade::log::level::info, message);
}

void failure(const char *step, unsigned result)
{
    char line[224];
    std::snprintf(line, sizeof(line), "[FH4 DLSS5] %s returned 0x%08X", step, result);
    log(line);
}

void check(HRESULT result, const char *step)
{
    if (FAILED(result)) {
        failure(step, static_cast<unsigned>(result));
        throw std::runtime_error(step);
    }
}

void check_ngx(NVSDK_NGX_Result result, const char *step)
{
    if (NVSDK_NGX_FAILED(result)) {
        failure(step, static_cast<unsigned>(result));
        throw std::runtime_error(step);
    }
}

template <class Function>
Function load_export(HMODULE module, const char *name)
{
    const auto function = GetProcAddress(module, name);
    if (!function)
        throw std::runtime_error(name);
#pragma warning(suppress: 4191)
    return reinterpret_cast<Function>(function);
}

HMODULE load_module(const fs::path &path)
{
    const auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        failure("LoadLibraryExW", GetLastError());
        throw std::runtime_error("Could not open an FH4 DLSS runtime dependency");
    }
    return module;
}

fs::path get_driver_core()
{
    wchar_t system[MAX_PATH];
    if (!GetSystemDirectoryW(system, MAX_PATH))
        throw std::runtime_error("Could not locate Windows System32");
    const auto store = fs::path(system) / L"DriverStore" / L"FileRepository";
    fs::path latest;
    auto time = fs::file_time_type::min();
    for (const auto &directory : fs::directory_iterator(store, fs::directory_options::skip_permission_denied)) {
        const auto name = directory.path().filename().wstring();
        if (!name.starts_with(L"nv") || name.find(L".inf_amd64_") == std::wstring::npos)
            continue;
        const auto candidate = directory.path() / L"_nvngx.dll";
        std::error_code error;
        const auto modified = fs::last_write_time(candidate, error);
        if (!error && modified > time) {
            latest = candidate;
            time = modified;
        }
    }
    if (latest.empty())
        throw std::runtime_error("NVIDIA NGX driver runtime was not found");
    return latest;
}

constexpr char prepare_shader[] = R"hlsl(
Texture2D<float3> scene : register(t0);
Texture2D<float> gameDepth : register(t1);
RWTexture2D<float4> neuralInput : register(u0);
RWTexture2D<float> neuralDepth : register(u1);
RWTexture2D<float2> neuralMotion : register(u2);
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
    uint2 s = p * 2;
    float3 c00 = scene.Load(int3(s, 0));
    float3 c10 = scene.Load(int3(s + uint2(1, 0), 0));
    float3 c01 = scene.Load(int3(s + uint2(0, 1), 0));
    float3 c11 = scene.Load(int3(s + uint2(1, 1), 0));
    float d00 = gameDepth.Load(int3(s, 0));
    float d10 = gameDepth.Load(int3(s + uint2(1, 0), 0));
    float d01 = gameDepth.Load(int3(s + uint2(0, 1), 0));
    float d11 = gameDepth.Load(int3(s + uint2(1, 1), 0));
    // Present a bounded display-referred signal to the Movie models. The
    // engine still applies its own HDR exposure to the decoded result.
    float3 sceneColor = max(0.0, (c00 + c10 + c01 + c11) * 0.25);
    float3 mapped = sceneColor / (1.0 + sceneColor);
    float3 encoded = lerp(12.92 * mapped, 1.055 * pow(mapped, 1.0 / 2.4) - 0.055,
                          step(0.0031308, mapped));
    neuralInput[p] = float4(encoded, 1.0);
    neuralDepth[p] = max(max(d00, d10), max(d01, d11));
    neuralMotion[p] = float2(0, 0);
}
)hlsl";

constexpr char finish_shader[] = R"hlsl(
Texture2D<float4> reconstructed : register(t0);
RWTexture2D<float3> originalScene : register(u0);

float3 encode(float3 scene) {
    float3 mapped = scene / (1.0 + scene);
    return lerp(12.92 * mapped, 1.055 * pow(mapped, 1.0 / 2.4) - 0.055,
                step(0.0031308, mapped));
}

float peak(float3 value) {
    return max(value.r, max(value.g, value.b));
}

[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
    int2 at = int2(p);
    const int2 extent = int2(3839, 2399);
    float3 source = max(0.0, originalScene[p]);
    float3 base = encode(source);
    float3 center = saturate(reconstructed.Load(int3(at, 0)).rgb);
    float3 nearby = (
        saturate(reconstructed.Load(int3(clamp(at + int2(-2, 0), 0, extent), 0)).rgb) +
        saturate(reconstructed.Load(int3(clamp(at + int2( 2, 0), 0, extent), 0)).rgb) +
        saturate(reconstructed.Load(int3(clamp(at + int2(0, -2), 0, extent), 0)).rgb) +
        saturate(reconstructed.Load(int3(clamp(at + int2(0,  2), 0, extent), 0)).rgb)
    ) * 0.25;
    const float3 luminance = float3(0.2126, 0.7152, 0.0722);
    float confidence = 1.0 - smoothstep(0.02, 0.09, peak(abs(center - nearby)));
    float light = smoothstep(0.08, 0.32, dot(base, luminance));
    float headroom = 1.0 - smoothstep(0.92, 0.99, peak(base));
    float3 delta = lerp(nearby, center, confidence) - base;
    float rawTone = dot(delta, luminance);
    float tone = clamp(rawTone, -0.10, 0.10);
    // The native 4K scene keeps its edges and dark color. Apply the two-pass
    // reconstruction only to stable detail and bounded tone/chroma changes.
    float3 chroma = clamp(delta - rawTone, -0.018, 0.018);
    float3 correction = (tone + chroma * light) * confidence * light * headroom;
    if (peak(abs(correction)) < 0.0001)
        return;
    float3 encoded = saturate(base + correction);
    float3 mapped = lerp(encoded / 12.92, pow((encoded + 0.055) / 1.055, 2.4),
                         step(0.04045, encoded));
    originalScene[p] = mapped / max(1.0 - mapped, 0.0001);
}
)hlsl";

void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}

struct SceneDescriptors {
    ComPtr<ID3D12Resource> scene;
    ComPtr<ID3D12Resource> depth;
    ComPtr<ID3D12DescriptorHeap> heap;
};

class Runtime {
public:
    void initialize(ID3D12Device *device, HMODULE addon);
    bool record(ID3D12GraphicsCommandList *list, ID3D12Resource *scene, ID3D12Resource *depth,
                bool &captured_depth);
    ID3D12Device *device() const { return device_.Get(); }

private:
    ComPtr<ID3D12Resource> texture(UINT width, UINT height, DXGI_FORMAT format);
    ComPtr<ID3D12PipelineState> pipeline(const char *code, size_t size);
    ID3D12DescriptorHeap *scene_heap(ID3D12Resource *scene, ID3D12Resource *depth);
    void initialize_ngx(HMODULE addon);
    void initialize_features();
    void dimensions(bool nr);
    void settings();
    bool evaluate(ID3D12GraphicsCommandList *list, bool nr, NVSDK_NGX_Handle *feature,
        ID3D12Resource *input, ID3D12Resource *output);

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> prepare_, finish_;
    ComPtr<ID3D12Resource> input_, guide_depth_, motion_, nr_first_, nr_second_, sr_output_;
    std::vector<SceneDescriptors> descriptors_;
    HMODULE core_ = nullptr, bridge_ = nullptr, nr_module_ = nullptr, sr_module_ = nullptr;
    BridgeCreate create_ = nullptr;
    BridgeEvaluate evaluate_ = nullptr;
    BridgePopulate populate_ = nullptr;
    Populate populate_nr_ = nullptr;
    Create nr_create_ = nullptr, sr_create_ = nullptr;
    Evaluate nr_evaluate_ = nullptr, sr_evaluate_ = nullptr;
    NVSDK_NGX_Parameter *params_ = nullptr;
    NVSDK_NGX_Handle *nr1_ = nullptr, *nr2_ = nullptr, *sr_ = nullptr;
};

void Runtime::initialize_ngx(HMODULE addon)
{
    wchar_t module_path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(addon, module_path, MAX_PATH);
    if (!length || length == MAX_PATH)
        throw std::runtime_error("Could not locate FH4 add-on");
    const auto directory = fs::path(module_path).parent_path() / L"FH4Dlss5";
    const auto nr_path = directory / L"nvngx_dlssnr.dll";
    const auto sr_path = directory / L"nvngx_dlss.dll";
    const auto local_cache = fs::temp_directory_path() / L"FH4Dlss5";
    fs::create_directories(local_cache);
    core_ = load_module(get_driver_core());
    bridge_ = load_module(directory / L"dlss5_nvngx.dll");
    nr_module_ = load_module(nr_path);
    sr_module_ = load_module(sr_path);
    const auto init_core = load_export<CoreInit>(core_, "NVSDK_NGX_D3D12_Init");
    const auto init_snippet = load_export<BridgeInit>(bridge_, "NVNGXBridge_D3D12_InitExt");
    create_ = load_export<BridgeCreate>(bridge_, "NVNGXBridge_D3D12_CreateFeature");
    evaluate_ = load_export<BridgeEvaluate>(bridge_, "NVNGXBridge_D3D12_EvaluateFeature");
    populate_ = load_export<BridgePopulate>(bridge_, "NVNGXBridge_D3D12_PopulateParameters");
    populate_nr_ = load_export<Populate>(nr_module_, "NVSDK_NGX_D3D12_PopulateParameters_Impl");
    nr_create_ = load_export<Create>(nr_module_, "NVSDK_NGX_D3D12_CreateFeature");
    nr_evaluate_ = load_export<Evaluate>(nr_module_, "NVSDK_NGX_D3D12_EvaluateFeature");
    sr_create_ = load_export<Create>(sr_module_, "NVSDK_NGX_D3D12_CreateFeature");
    sr_evaluate_ = load_export<Evaluate>(sr_module_, "NVSDK_NGX_D3D12_EvaluateFeature");

    check_ngx(init_core(application_id, local_cache.c_str(), device_.Get(), NVSDK_NGX_Version_API), "driver NGX init");
    check_ngx(init_snippet(load_export<SnippetInit>(nr_module_, "NVSDK_NGX_D3D12_Init_Ext"),
        application_id, nr_path.c_str(), device_.Get(), NVSDK_NGX_Version_API, nullptr), "DLSS5 NR init");
    check_ngx(init_snippet(load_export<SnippetInit>(sr_module_, "NVSDK_NGX_D3D12_Init_Ext"),
        application_id, sr_path.c_str(), device_.Get(), NVSDK_NGX_Version_API, nullptr), "DLSS SR init");
    check_ngx(load_export<GetParameters>(core_, "NVSDK_NGX_D3D12_GetCapabilityParameters")(&params_), "NGX parameters");
    if (!params_)
        throw std::runtime_error("NVIDIA returned no parameter map");
}

void Runtime::dimensions(bool nr)
{
    const UINT width = nr ? iw : ow;
    const UINT height = nr ? ih : oh;
    params_->Set("CreationNodeMask", 1u);
    params_->Set("VisibilityNodeMask", 1u);
    params_->Set("Width", iw);
    params_->Set("Height", ih);
    params_->Set("OutWidth", width);
    params_->Set("OutHeight", height);
    params_->Set("DLSS.Feature.Create.Flags", flags);
    params_->Set("DLSS.Enable.Output.Subrects", 0);
    params_->Set("PerfQualityValue", static_cast<int>(nr ? NVSDK_NGX_PerfQuality_Value_UltraQuality
        : NVSDK_NGX_PerfQuality_Value_MaxPerf));
    if (!nr)
        return;
    params_->Set("ResourceWidth", iw);
    params_->Set("ResourceHeight", ih);
    params_->Set("ResourceOutWidth", width);
    params_->Set("ResourceOutHeight", height);
    params_->Set("DLSS.Use.HW.Depth", 1u);
    params_->Set("DLSS.Denoise.Mode", 1);
    params_->Set("DLSS.Roughness.Mode", 0u);
    params_->Set("Output.Width", width);
    params_->Set("Output.Height", height);
    settings();
}

void Runtime::settings()
{
    params_->Set("DLSSNR.Enabled", 1u);
    params_->Set("DLSSNR.InputWidth", iw);
    params_->Set("DLSSNR.InputHeight", ih);
    params_->Set("DLSSNR.Width", iw);
    params_->Set("DLSSNR.Height", ih);
    params_->Set("DLSSNR.OutputWidth", iw);
    params_->Set("DLSSNR.OutputHeight", ih);
    params_->Set("DLSSNR.Upscaling", 1u);
    params_->Set("DLSSNR.ScalingRatio", 1.0f);
    params_->Set("DLSSNR.Scale", 1.0f);
    params_->Set("DLSSNR.Hint.Render.Preset", 3);
    params_->Set("DLSSNR.Style", 2u);
    params_->Set("DLSSNR.Intensity", 1.0f);
    params_->Set("DLSSNR.LocalToneStrength", 1.0f);
    params_->Set("DLSSNR.LocalStructureStrength", 1.0f);
    params_->Set("DLSSNR.SkinStructureStrength", -1.0f);
    params_->Set("DLSSNR.UseAutoMask", 1u);
    params_->Set("DLSSNR.UICorrection", 0u);
}

void Runtime::initialize_features()
{
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    check(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "NGX init queue");
    check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "NGX init allocator");
    check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
          IID_PPV_ARGS(&list)), "NGX init list");
    check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "NGX init fence");

    for (auto **feature : {&nr1_, &nr2_, &sr_}) {
        const bool nr = feature != &sr_;
        params_->Reset();
        if (nr)
            check_ngx(populate_(populate_nr_, params_), "DLSS5 NR populate");
        dimensions(nr);
        check_ngx(create_(nr ? nr_create_ : sr_create_, list.Get(),
            nr ? nr_feature : NVSDK_NGX_Feature_SuperSampling, params_, feature),
            nr ? "CreateFeature DLSS5 NR" : "CreateFeature DLSS SR");
        if (!*feature)
            throw std::runtime_error("NVIDIA returned no feature handle");
    }

    check(list->Close(), "close NGX init");
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), 1), "signal NGX init");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        throw std::runtime_error("NGX init event");
    const HRESULT completion = fence->SetEventOnCompletion(1, event);
    const DWORD wait = SUCCEEDED(completion) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
    CloseHandle(event);
    if (wait != WAIT_OBJECT_0)
        throw std::runtime_error("NGX initialization GPU wait did not finish");
    log("[FH4 DLSS5] Created NVIDIA feature 18 twice and NVIDIA DLSS Super Resolution.");
}

ComPtr<ID3D12Resource> Runtime::texture(UINT width, UINT height, DXGI_FORMAT format)
{
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC desc = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format,
        {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    ComPtr<ID3D12Resource> resource;
    check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, uav_state,
          nullptr, IID_PPV_ARGS(&resource)), "allocate FH4 neural texture");
    return resource;
}

ComPtr<ID3D12PipelineState> Runtime::pipeline(const char *code, size_t size)
{
    ComPtr<ID3DBlob> shader, error;
    check(D3DCompile(code, size, "FH4 scene processing", nullptr, nullptr, "main", "cs_5_0",
          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &error), "compile FH4 compute shader");
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root_.Get();
    desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    ComPtr<ID3D12PipelineState> result;
    check(device_->CreateComputePipelineState(&desc, IID_PPV_ARGS(&result)), "create FH4 compute pipeline");
    return result;
}

void Runtime::initialize(ID3D12Device *device, HMODULE addon)
{
    device_ = device;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT format = {game_format};
    check(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format, sizeof(format)), "query R11 format");
    constexpr auto required = D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
    if ((format.Support2 & required) != required)
        throw std::runtime_error("R11G11B10 typed UAV load/store is unavailable");

    D3D12_DESCRIPTOR_RANGE ranges[] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 2}
    };
    D3D12_ROOT_PARAMETER binding = {};
    binding.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    binding.DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC root_desc = {1, &binding, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> signature, error;
    check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error),
          "serialize FH4 compute bindings");
    check(device_->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
          IID_PPV_ARGS(&root_)), "create FH4 compute bindings");
    prepare_ = pipeline(prepare_shader, sizeof(prepare_shader) - 1);
    finish_ = pipeline(finish_shader, sizeof(finish_shader) - 1);
    input_ = texture(iw, ih, color_format);
    guide_depth_ = texture(iw, ih, DXGI_FORMAT_R32_FLOAT);
    motion_ = texture(iw, ih, DXGI_FORMAT_R16G16_FLOAT);
    nr_first_ = texture(iw, ih, color_format);
    nr_second_ = texture(iw, ih, color_format);
    sr_output_ = texture(ow, oh, color_format);
    initialize_ngx(addon);
    initialize_features();
}

ID3D12DescriptorHeap *Runtime::scene_heap(ID3D12Resource *scene, ID3D12Resource *depth)
{
    for (const auto &entry : descriptors_) {
        if (entry.scene.Get() == scene && entry.depth.Get() == depth)
            return entry.heap.Get();
    }
    if (descriptors_.size() == 8)
        return nullptr;
    SceneDescriptors entry;
    entry.scene = scene;
    entry.depth = depth;
    const D3D12_DESCRIPTOR_HEAP_DESC desc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 10,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    check(device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&entry.heap)), "create FH4 scene descriptors");
    const auto stride = device_->GetDescriptorHandleIncrementSize(desc.Type);
    auto handle = entry.heap->GetCPUDescriptorHandleForHeapStart();
    const auto srv = [&](ID3D12Resource *resource, DXGI_FORMAT format) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
        view.Format = format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(resource, &view, handle);
        handle.ptr += stride;
    };
    const auto uav = [&](ID3D12Resource *resource, DXGI_FORMAT format) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
        view.Format = format;
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(resource, nullptr, &view, handle);
        handle.ptr += stride;
    };
    srv(scene, game_format);
    srv(depth, DXGI_FORMAT_R32_FLOAT);
    uav(input_.Get(), color_format);
    uav(guide_depth_.Get(), DXGI_FORMAT_R32_FLOAT);
    uav(motion_.Get(), DXGI_FORMAT_R16G16_FLOAT);
    srv(sr_output_.Get(), color_format);
    srv(nullptr, DXGI_FORMAT_R32_FLOAT);
    uav(scene, game_format);
    uav(nullptr, DXGI_FORMAT_R32_FLOAT);
    uav(nullptr, DXGI_FORMAT_R16G16_FLOAT);
    descriptors_.push_back(std::move(entry));
    return descriptors_.back().heap.Get();
}

bool Runtime::evaluate(ID3D12GraphicsCommandList *list, bool nr, NVSDK_NGX_Handle *feature,
                       ID3D12Resource *input, ID3D12Resource *output)
{
    params_->Set("Color", input);
    params_->Set("Output", output);
    params_->Set("Depth", guide_depth_.Get());
    params_->Set("MotionVectors", motion_.Get());
    // The stock FH4 motion-blur texture is not aligned with this insertion
    // point. Reset temporal history until a same-frame guide is available.
    params_->Set("Reset", 1);
    params_->Set("Jitter.Offset.X", 0.0f);
    params_->Set("Jitter.Offset.Y", 0.0f);
    params_->Set("MV.Scale.X", 1.0f);
    params_->Set("MV.Scale.Y", 1.0f);
    params_->Set("DLSS.Pre.Exposure", 1.0f);
    params_->Set("DLSS.Exposure.Scale", 1.0f);
    params_->Set("DLSS.Render.Subrect.Dimensions.Width", iw);
    params_->Set("DLSS.Render.Subrect.Dimensions.Height", ih);
    for (const char *key : {"DLSS.Input.Color.Subrect.Base.X", "DLSS.Input.Color.Subrect.Base.Y",
        "DLSS.Input.Depth.Subrect.Base.X", "DLSS.Input.Depth.Subrect.Base.Y",
        "DLSS.Input.MV.Subrect.Base.X", "DLSS.Input.MV.Subrect.Base.Y",
        "DLSS.Output.Subrect.Base.X", "DLSS.Output.Subrect.Base.Y"})
        params_->Set(key, 0u);
    if (nr) {
        params_->Set("DLSSNR.Color", input);
        params_->Set("DLSSNR.Output", output);
        params_->Set("DLSSNR.Depth", guide_depth_.Get());
        params_->Set("DLSSNR.MVec", motion_.Get());
        params_->Set("DLSSNR.Reset", 1);
        params_->Set("DLSSNR.MVecScaleX", 1.0f);
        params_->Set("DLSSNR.MVecScaleY", 1.0f);
        params_->Set("DLSSNR.JitterOffsetX", 0.0f);
        params_->Set("DLSSNR.JitterOffsetY", 0.0f);
        params_->Set("DLSSNR.DepthInverted", 1u);
        for (const char *key : {"DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY",
            "DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY",
            "DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY",
            "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY"})
            params_->Set(key, 0);
        for (const char *key : {"DLSSNR.ColorSubrectWidth", "DLSSNR.MVecSubrectWidth",
            "DLSSNR.DepthSubrectWidth", "DLSSNR.OutputSubrectWidth"})
            params_->Set(key, static_cast<int>(iw));
        for (const char *key : {"DLSSNR.ColorSubrectHeight", "DLSSNR.MVecSubrectHeight",
            "DLSSNR.DepthSubrectHeight", "DLSSNR.OutputSubrectHeight"})
            params_->Set(key, static_cast<int>(ih));
        settings();
    }
    const auto result = evaluate_(nr ? nr_evaluate_ : sr_evaluate_, list, feature, params_, nullptr);
    if (NVSDK_NGX_FAILED(result)) {
        failure(nr ? "EvaluateFeature DLSS5 NR" : "EvaluateFeature DLSS SR", static_cast<unsigned>(result));
        return false;
    }
    return true;
}

bool Runtime::record(ID3D12GraphicsCommandList *list, ID3D12Resource *scene, ID3D12Resource *depth,
                     bool &captured_depth)
{
    const auto heap = scene_heap(scene, depth);
    if (!heap)
        return false;
    ID3D12DescriptorHeap *heaps[] = {heap};
    auto table = heap->GetGPUDescriptorHandleForHeapStart();
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_.Get());
    list->SetPipelineState(prepare_.Get());
    list->SetComputeRootDescriptorTable(0, table);
    list->Dispatch(iw / 8, ih / 8, 1);
    for (const auto resource : {input_.Get(), guide_depth_.Get(), motion_.Get()})
        transition(list, resource, uav_state, read_state);

    bool first = evaluate(list, true, nr1_, input_.Get(), nr_first_.Get());
    bool second = false, upscale = false;
    if (first) {
        transition(list, nr_first_.Get(), uav_state, read_state);
        second = evaluate(list, true, nr2_, nr_first_.Get(), nr_second_.Get());
        transition(list, nr_first_.Get(), read_state, uav_state);
    }
    if (second) {
        transition(list, nr_second_.Get(), uav_state, read_state);
        upscale = evaluate(list, false, sr_, nr_second_.Get(), sr_output_.Get());
        transition(list, nr_second_.Get(), read_state, uav_state);
    }
    if (upscale) {
        transition(list, sr_output_.Get(), uav_state, read_state);
        transition(list, scene, game_read, uav_state);
        table.ptr += 5ULL * device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root_.Get());
        list->SetPipelineState(finish_.Get());
        list->SetComputeRootDescriptorTable(0, table);
        list->Dispatch(ow / 8, oh / 8, 1);
        transition(list, scene, uav_state, game_read);
        transition(list, sr_output_.Get(), read_state, uav_state);
        captured_depth = fg::capture(list, guide_depth_.Get(), read_state, fg::depth);
    }
    for (const auto resource : {input_.Get(), guide_depth_.Get(), motion_.Get()})
        transition(list, resource, read_state, uav_state);
    return upscale;
}

std::mutex runtime_mutex;
Runtime *runtime = nullptr;
bool failed = false;

} // namespace

void set_enabled(bool value) { active.store(value, std::memory_order_relaxed); }
bool enabled() { return active.load(std::memory_order_relaxed); }
unsigned recorded_frames() { return frames.load(std::memory_order_relaxed); }

bool process_scene(ID3D12GraphicsCommandList *commands, ID3D12Resource *scene,
                   ID3D12Resource *depth, HMODULE addon, bool &captured_depth)
{
    if (!enabled())
        return false;
    const auto image = scene->GetDesc();
    const auto guide = depth->GetDesc();
    if (image.Width != ow || image.Height != oh || image.Format != game_format ||
        image.SampleDesc.Count != 1 || !(image.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
        guide.Width != ow || guide.Height != oh || guide.Format != DXGI_FORMAT_R32_TYPELESS ||
        guide.SampleDesc.Count != 1 || (guide.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
        return false;
    std::lock_guard lock(runtime_mutex);
    if (failed)
        return false;
    try {
        ComPtr<ID3D12Device> device;
        check(commands->GetDevice(IID_PPV_ARGS(&device)), "get FH4 D3D12 device");
        if (!runtime) {
            // The game owns the queue executing these resources. Keep feature
            // handles and resources alive through process exit rather than
            // releasing them during a loader-lock or device-destroy callback.
            runtime = new Runtime;
            runtime->initialize(device.Get(), addon);
        }
        if (runtime->device() != device.Get())
            return false;
        if (!runtime->record(commands, scene, depth, captured_depth)) {
            failed = true;
            log("[FH4 DLSS5] Processing skipped; game scene remains unchanged.");
            return false;
        }
        const unsigned count = frames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 240 == 0) {
            char line[186];
            std::snprintf(line, sizeof(line),
                "[FH4 DLSS5] recorded=%u; Movie 2 passes + DLSS SR; scene=1920x1200 -> 3840x2400 before HUD", count);
            log(line);
        }
        return true;
    } catch (const std::exception &error) {
        failed = true;
        log(error.what());
        return false;
    }
}

}
