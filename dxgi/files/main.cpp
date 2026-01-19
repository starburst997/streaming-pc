// DXGI Desktop Mirror - Low-latency display mirroring
// Capture thread: captures at source refresh rate
// Render thread: presents with VSync at target refresh rate
// Supports HDR to SDR tonemapping (maxRGB Reinhard)
//
// Build: cl /O2 /EHsc main.cpp /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib winmm.lib

#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <climits>
#include <thread>
#include <atomic>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")

// Simple vertex shader - same for SDR and HDR
const char* g_VertexShader = R"(
struct VS_OUTPUT { float4 pos : SV_POSITION; float2 tex : TEXCOORD0; };
VS_OUTPUT main(float2 pos : POSITION, float2 tex : TEXCOORD0) {
    VS_OUTPUT o; o.pos = float4(pos, 0, 1); o.tex = tex; return o;
})";

// SDR pixel shader - simple passthrough
const char* g_PixelShaderSDR = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return tex.Sample(samp, uv);
})";

// SDR pixel shader with gamma correction
// Used when source monitor is HDR but gives us B8G8R8A8 (linear values in SDR container)
const char* g_PixelShaderSDRGamma = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);

float3 lin_to_srgb(float3 lin) {
    float3 srgb;
    srgb.r = lin.r <= 0.0031308 ? 12.92 * lin.r : 1.055 * pow(lin.r, 1.0/2.4) - 0.055;
    srgb.g = lin.g <= 0.0031308 ? 12.92 * lin.g : 1.055 * pow(lin.g, 1.0/2.4) - 0.055;
    srgb.b = lin.b <= 0.0031308 ? 12.92 * lin.b : 1.055 * pow(lin.b, 1.0/2.4) - 0.055;
    return srgb;
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 color = tex.Sample(samp, uv);
    color.rgb = saturate(color.rgb);  // Clamp to 0-1
    color.rgb = lin_to_srgb(color.rgb);
    return float4(color.rgb, 1.0);
})";

// HDR to SDR pixel shader with tonemapping
// Input: scRGB (linear RGB, 1.0 = 80 nits, values can exceed 1.0 for HDR)
// Output: sRGB (gamma-corrected, 0-1 range)
//
// References:
// - https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
// - https://github.com/obsproject/obs-studio/blob/master/libobs/data/color.effect
const char* g_PixelShaderHDR = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);

cbuffer Constants : register(b0) {
    float sdrWhiteNits;
    float padding1;
    float padding2;
    float padding3;
};

// sRGB OETF (linear to gamma)
float3 lin_to_srgb(float3 lin) {
    float3 srgb;
    srgb.r = lin.r <= 0.0031308 ? 12.92 * lin.r : 1.055 * pow(abs(lin.r), 1.0/2.4) - 0.055;
    srgb.g = lin.g <= 0.0031308 ? 12.92 * lin.g : 1.055 * pow(abs(lin.g), 1.0/2.4) - 0.055;
    srgb.b = lin.b <= 0.0031308 ? 12.92 * lin.b : 1.055 * pow(abs(lin.b), 1.0/2.4) - 0.055;
    return srgb;
}

// Attempt to match OBS's maxRGB Reinhard tonemapping (simpler, preserves colors better)
// This is what OBS uses with their default tonemapping
float3 reinhardMaxRGB(float3 x) {
    float maxRGB = max(max(x.r, x.g), x.b);
    if (maxRGB > 1.0) {
        float scale = 1.0 / maxRGB;  // Simple Reinhard: x / (1 + x) when maxRGB >> 1
        scale = maxRGB / (1.0 + maxRGB);  // Proper Reinhard
        scale /= maxRGB;
        x *= scale;
    }
    return x;
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 color = tex.Sample(samp, uv);

    // scRGB can have negative values for wide gamut - clamp to 0
    color.rgb = max(color.rgb, 0.0);

    // Normalize scRGB to SDR range
    // scRGB: 1.0 = 80 nits (SDR reference white per spec)
    // Windows SDR white slider typically 80-480 nits
    // We need to scale down by the ratio so that "SDR white" maps to 1.0
    float scale = 80.0 / sdrWhiteNits;
    color.rgb *= scale;

    // Apply maxRGB Reinhard tonemapping for values > 1.0
    // This preserves SDR content (values <= 1.0) perfectly
    color.rgb = reinhardMaxRGB(color.rgb);

    // Clamp to valid range
    color.rgb = saturate(color.rgb);

    // Convert linear to sRGB gamma for display
    color.rgb = lin_to_srgb(color.rgb);

    return float4(color.rgb, 1.0);
})";

// Cursor pixel shader - handles alpha blending and masked cursors
const char* g_PixelShaderCursor = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 color = tex.Sample(samp, uv);
    return color;  // Alpha blending handled by blend state
})";

struct Vertex { float x, y, u, v; };
Vertex g_Quad[] = {{-1,1,0,0}, {1,1,1,0}, {-1,-1,0,1}, {1,-1,1,1}};

// Triple buffer for lock-free producer/consumer with frame ID tracking
struct TripleBuffer {
    ID3D11Texture2D* textures[3] = {nullptr, nullptr, nullptr};
    ID3D11ShaderResourceView* srvs[3] = {nullptr, nullptr, nullptr};

    // Frame IDs for each buffer slot (for consistent frame selection)
    std::atomic<UINT64> frameIds[3] = {{0}, {0}, {0}};

    std::atomic<int> writeIdx{0};
    std::atomic<int> readyIdx{-1};
    std::atomic<int> displayIdx{-1};

    // Last displayed frame ID (for consistent frame skipping)
    UINT64 lastDisplayedFrameId = 0;

    void PublishFrame(UINT64 frameId) {
        int completed = writeIdx.load(std::memory_order_relaxed);
        frameIds[completed].store(frameId, std::memory_order_relaxed);
        int oldReady = readyIdx.exchange(completed, std::memory_order_acq_rel);

        if (oldReady >= 0 && oldReady != displayIdx.load(std::memory_order_acquire)) {
            writeIdx.store(oldReady, std::memory_order_relaxed);
        } else {
            int disp = displayIdx.load(std::memory_order_acquire);
            int ready = readyIdx.load(std::memory_order_acquire);
            for (int i = 0; i < 3; i++) {
                if (i != ready && i != disp) {
                    writeIdx.store(i, std::memory_order_relaxed);
                    break;
                }
            }
        }
    }

    int AcquireFrame(UINT64* outFrameId = nullptr) {
        int ready = readyIdx.exchange(-1, std::memory_order_acq_rel);
        if (ready >= 0) {
            displayIdx.store(ready, std::memory_order_release);
        }
        int idx = displayIdx.load(std::memory_order_acquire);
        if (idx >= 0 && outFrameId) {
            *outFrameId = frameIds[idx].load(std::memory_order_relaxed);
        }
        return idx;
    }

    int GetWriteIndex() { return writeIdx.load(std::memory_order_relaxed); }

    UINT64 GetReadyFrameId() {
        int ready = readyIdx.load(std::memory_order_acquire);
        if (ready >= 0) {
            return frameIds[ready].load(std::memory_order_relaxed);
        }
        return 0;
    }
};

// Cursor info shared between capture and render threads
struct CursorInfo {
    std::atomic<bool> visible{true};   // Default to visible (will be updated on first cursor event)
    std::atomic<bool> hasShape{false};  // True once we've captured a cursor shape
    std::atomic<int> x{0};
    std::atomic<int> y{0};
    std::atomic<int> width{0};
    std::atomic<int> height{0};
    std::atomic<bool> shapeUpdated{false};

    // Shape data (protected by shapeUpdated flag)
    BYTE* shapeBuffer = nullptr;
    UINT shapeBufferSize = 0;
    UINT shapeWidth = 0;
    UINT shapeHeight = 0;
    UINT shapePitch = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_TYPE shapeType = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
};

struct {
    int sourceMonitor = 0;
    int targetMonitor = 1;
    bool preserveAspect = true;
    bool tonemap = true;  // HDR to SDR tonemapping (can be disabled with --no-tonemap)
    float sdrWhiteNits = 240.0f;  // SDR white level in nits (matches OBS default)
    bool showCursor = true;  // Show cursor (can be disabled with --no-cursor)
    bool useWaitableSwapChain = true;  // Use waitable swap chain for frame pacing
    bool useFrameDelay = true;  // Add small delay after waitable for consistent frame selection
    int frameDelayUs = 1000;  // Frame delay in microseconds (default 1000µs = 1ms)
    bool debug = false;   // Debug output
    std::atomic<bool> running{true};

    HWND hwnd = nullptr;
    int windowWidth = 0, windowHeight = 0;

    // Render thread resources
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain1* swapChain = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* psSDR = nullptr;
    ID3D11PixelShader* psSDRGamma = nullptr;  // For HDR monitor giving SDR format
    ID3D11PixelShader* psHDR = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* cbHDR = nullptr;  // Constant buffer for HDR shader
    ID3D11SamplerState* sampler = nullptr;

    // Cursor resources
    ID3D11PixelShader* psCursor = nullptr;
    ID3D11Texture2D* cursorTex = nullptr;
    ID3D11ShaderResourceView* cursorSrv = nullptr;
    ID3D11Buffer* cursorVb = nullptr;
    ID3D11BlendState* blendState = nullptr;
    CursorInfo cursor;

    // Capture thread resources
    ID3D11Device* capDevice = nullptr;
    ID3D11DeviceContext* capContext = nullptr;
    IDXGIOutputDuplication* duplication = nullptr;

    TripleBuffer buffer;

    RECT sourceRect = {}, targetRect = {};
    D3D11_VIEWPORT viewport = {};

    // Source format info (detected from first captured frame)
    bool sourceIsHDR = false;           // True if actual captured format is HDR (R16G16B16A16_FLOAT)
    bool sourceReportedHDR = false;     // True if monitor reported HDR capability
    DXGI_FORMAT sourceFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    std::atomic<bool> bufferInitialized{false};

    // Refresh rates (detected during initialization)
    float sourceRefreshRate = 60.0f;
    float targetRefreshRate = 60.0f;

    // Stats
    std::atomic<int> captureCount{0};
    std::atomic<UINT64> captureFrameId{0};
    UINT64 lastRenderedId = 0;
    UINT64 lastCaptureCheckId = 0;  // For detecting idle desktop

    // Frame pacing stats
    int frameSkipMin = INT_MAX;
    int frameSkipMax = 0;
    int frameSkipTotal = 0;
    int frameSkipCount = 0;

    // Frame pacing
    HANDLE frameLatencyWaitable = nullptr;
    int targetFrameSkip = 0;  // Auto-detected: sourceHz / targetHz (e.g., 2 for 120→60)
    bool useSmartFrameSelection = true;  // Wait for correct frame ID instead of fixed delay

    std::thread captureThread;
} g;

void Cleanup();

// High-precision microsecond delay using spin-wait
// Sleep() only has ~1ms precision, this gives µs precision
void DelayMicroseconds(int us) {
    if (us <= 0) return;

    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    double targetTicks = (double)us * freq.QuadPart / 1000000.0;
    LONGLONG endTicks = start.QuadPart + (LONGLONG)targetTicks;

    // Spin-wait for precise timing
    do {
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < endTicks);
}

void Fatal(const char* msg, HRESULT hr = 0) {
    if (hr) fprintf(stderr, "FATAL: %s (0x%08X)\n", msg, (unsigned)hr);
    else fprintf(stderr, "FATAL: %s\n", msg);
    Cleanup();
    exit(1);
}

// Console control handler for graceful CTRL+C shutdown
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            printf("\nReceived shutdown signal...\n");
            g.running = false;
            // Give threads time to clean up
            Sleep(200);
            return TRUE;
    }
    return FALSE;
}

struct MonitorInfo { int index; RECT rect; };
BOOL CALLBACK MonitorEnumProc(HMONITOR, HDC, LPRECT rect, LPARAM data) {
    auto* info = (MonitorInfo*)data;
    if (info->index == 0) { info->rect = *rect; return FALSE; }
    info->index--; return TRUE;
}
bool GetMonitorRect(int idx, RECT* rect) {
    MonitorInfo info = {idx, {}};
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, (LPARAM)&info);
    *rect = info.rect;
    return info.index == 0 || (rect->right - rect->left) > 0;
}
int GetMonitorCount() { return GetSystemMetrics(SM_CMONITORS); }
void PrintMonitors() {
    printf("Available monitors:\n");
    for (int i = 0; i < GetMonitorCount(); i++) {
        RECT r; GetMonitorRect(i, &r);
        printf("  %d: %dx%d at (%d,%d)\n", i, r.right-r.left, r.bottom-r.top, r.left, r.top);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { g.running = false; return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hwnd, msg, wp, lp);
}

void CreateWindow_() {
    WNDCLASS wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "DXGIMirror"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&wc);

    g.windowWidth = g.targetRect.right - g.targetRect.left;
    g.windowHeight = g.targetRect.bottom - g.targetRect.top;

    g.hwnd = CreateWindowEx(WS_EX_TOPMOST, "DXGIMirror", "DXGI Mirror",
        WS_POPUP | WS_VISIBLE, g.targetRect.left, g.targetRect.top,
        g.windowWidth, g.windowHeight, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

    float srcW = (float)(g.sourceRect.right - g.sourceRect.left);
    float srcH = (float)(g.sourceRect.bottom - g.sourceRect.top);
    float dstW = (float)g.windowWidth, dstH = (float)g.windowHeight;

    if (g.preserveAspect) {
        float srcAspect = srcW / srcH, dstAspect = dstW / dstH;
        if (srcAspect > dstAspect) {
            float h = dstW / srcAspect;
            g.viewport = {0, (dstH-h)/2, dstW, h, 0, 1};
        } else {
            float w = dstH * srcAspect;
            g.viewport = {(dstW-w)/2, 0, w, dstH, 0, 1};
        }
    } else {
        g.viewport = {0, 0, dstW, dstH, 0, 1};
    }
}

void InitD3D() {
    HRESULT hr;
    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL flOut;

    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2,
        D3D11_SDK_VERSION, &g.device, &flOut, &g.context);
    if (FAILED(hr)) Fatal("D3D11CreateDevice (render)", hr);

    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2,
        D3D11_SDK_VERSION, &g.capDevice, &flOut, &g.capContext);
    if (FAILED(hr)) Fatal("D3D11CreateDevice (capture)", hr);

    IDXGIDevice* dxgiDev; g.device->QueryInterface(&dxgiDev);
    IDXGIAdapter* adapter; dxgiDev->GetAdapter(&adapter); dxgiDev->Release();
    IDXGIFactory2* factory; adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    adapter->Release();

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = g.windowWidth; scd.Height = g.windowHeight;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (g.useWaitableSwapChain) {
        scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }

    hr = factory->CreateSwapChainForHwnd(g.device, g.hwnd, &scd, nullptr, nullptr, &g.swapChain);
    factory->Release();
    if (FAILED(hr)) Fatal("CreateSwapChain", hr);

    // Set max frame latency to 1 for tighter timing control
    if (g.useWaitableSwapChain) {
        IDXGISwapChain2* swapChain2 = nullptr;
        hr = g.swapChain->QueryInterface(&swapChain2);
        if (SUCCEEDED(hr)) {
            swapChain2->SetMaximumFrameLatency(1);
            g.frameLatencyWaitable = swapChain2->GetFrameLatencyWaitableObject();
            swapChain2->Release();
        }
    }

    ID3D11Texture2D* bb;
    g.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    g.device->CreateRenderTargetView(bb, nullptr, &g.rtv);
    bb->Release();
}

void InitShaders() {
    HRESULT hr; ID3DBlob *blob, *err;

    // Vertex shader (shared)
    hr = D3DCompile(g_VertexShader, strlen(g_VertexShader), "VS", 0, 0, "main", "vs_5_0", 0, 0, &blob, &err);
    if (FAILED(hr)) Fatal("VS compile");
    g.device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), 0, &g.vs);

    D3D11_INPUT_ELEMENT_DESC ied[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };
    g.device->CreateInputLayout(ied, 2, blob->GetBufferPointer(), blob->GetBufferSize(), &g.layout);
    blob->Release();

    // SDR pixel shader (passthrough)
    hr = D3DCompile(g_PixelShaderSDR, strlen(g_PixelShaderSDR), "PS_SDR", 0, 0, "main", "ps_5_0", 0, 0, &blob, &err);
    if (FAILED(hr)) Fatal("PS SDR compile");
    g.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), 0, &g.psSDR);
    blob->Release();

    // SDR pixel shader with gamma correction (for HDR monitor giving SDR format)
    hr = D3DCompile(g_PixelShaderSDRGamma, strlen(g_PixelShaderSDRGamma), "PS_SDR_Gamma", 0, 0, "main", "ps_5_0", 0, 0, &blob, &err);
    if (FAILED(hr)) {
        if (err) fprintf(stderr, "PS SDR Gamma compile error: %s\n", (char*)err->GetBufferPointer());
        Fatal("PS SDR Gamma compile");
    }
    g.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), 0, &g.psSDRGamma);
    blob->Release();

    // HDR pixel shader (with tonemapping)
    hr = D3DCompile(g_PixelShaderHDR, strlen(g_PixelShaderHDR), "PS_HDR", 0, 0, "main", "ps_5_0", 0, 0, &blob, &err);
    if (FAILED(hr)) {
        if (err) fprintf(stderr, "PS HDR compile error: %s\n", (char*)err->GetBufferPointer());
        Fatal("PS HDR compile");
    }
    g.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), 0, &g.psHDR);
    blob->Release();

    D3D11_BUFFER_DESC bd = {}; bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = sizeof(g_Quad); bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = {g_Quad};
    g.device->CreateBuffer(&bd, &sd, &g.vb);

    D3D11_SAMPLER_DESC sampd = {}; sampd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampd.AddressU = sampd.AddressV = sampd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    g.device->CreateSamplerState(&sampd, &g.sampler);

    // Constant buffer for HDR shader (sdrWhiteNits value)
    D3D11_BUFFER_DESC cbd = {};
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.ByteWidth = 16;  // 4 floats (16 bytes, minimum cbuffer size)
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g.device->CreateBuffer(&cbd, nullptr, &g.cbHDR);

    // Cursor pixel shader
    hr = D3DCompile(g_PixelShaderCursor, strlen(g_PixelShaderCursor), "PS_Cursor", 0, 0, "main", "ps_5_0", 0, 0, &blob, &err);
    if (FAILED(hr)) Fatal("PS Cursor compile");
    g.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), 0, &g.psCursor);
    blob->Release();

    // Blend state for cursor alpha blending (straight alpha)
    // Using straight alpha blend as it's more compatible with various cursor formats
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g.device->CreateBlendState(&blendDesc, &g.blendState);

    // Dynamic vertex buffer for cursor (updated each frame)
    D3D11_BUFFER_DESC cvbd = {};
    cvbd.Usage = D3D11_USAGE_DYNAMIC;
    cvbd.ByteWidth = sizeof(Vertex) * 4;
    cvbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    cvbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g.device->CreateBuffer(&cvbd, nullptr, &g.cursorVb);
}

void InitDuplication() {
    HRESULT hr;
    IDXGIDevice* dxgiDev; g.capDevice->QueryInterface(&dxgiDev);
    IDXGIAdapter* adapter; dxgiDev->GetAdapter(&adapter); dxgiDev->Release();

    IDXGIOutput* output = nullptr;
    for (UINT i = 0; ; i++) {
        IDXGIOutput* out;
        if (adapter->EnumOutputs(i, &out) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC desc; out->GetDesc(&desc);
        if (desc.DesktopCoordinates.left == g.sourceRect.left &&
            desc.DesktopCoordinates.top == g.sourceRect.top) { output = out; break; }
        out->Release();
    }
    adapter->Release();
    if (!output) Fatal("Source monitor not found");

    // Try IDXGIOutput6 first (Windows 10 1803+), then IDXGIOutput5, then fall back to IDXGIOutput1
    // DuplicateOutput1 allows us to request HDR format (R16G16B16A16_FLOAT)
    IDXGIOutput6* out6 = nullptr;
    IDXGIOutput5* out5 = nullptr;
    IDXGIOutput1* out1 = nullptr;

    // Formats we support, in order of preference
    DXGI_FORMAT supportedFormats[] = {
        DXGI_FORMAT_R16G16B16A16_FLOAT,  // HDR
        DXGI_FORMAT_B8G8R8A8_UNORM,      // SDR
    };

    hr = output->QueryInterface(&out6);
    if (SUCCEEDED(hr)) {
        hr = out6->DuplicateOutput1(g.capDevice, 0, _countof(supportedFormats), supportedFormats, &g.duplication);
        out6->Release();
        if (SUCCEEDED(hr)) {
            if (g.debug) printf("[DEBUG] Using IDXGIOutput6::DuplicateOutput1 (HDR supported)\n");
        }
    }

    if (!g.duplication) {
        hr = output->QueryInterface(&out5);
        if (SUCCEEDED(hr)) {
            hr = out5->DuplicateOutput1(g.capDevice, 0, _countof(supportedFormats), supportedFormats, &g.duplication);
            out5->Release();
            if (SUCCEEDED(hr)) {
                if (g.debug) printf("[DEBUG] Using IDXGIOutput5::DuplicateOutput1 (HDR supported)\n");
            }
        }
    }

    if (!g.duplication) {
        // Fall back to old method (no HDR support)
        hr = output->QueryInterface(&out1);
        if (SUCCEEDED(hr)) {
            hr = out1->DuplicateOutput(g.capDevice, &g.duplication);
            out1->Release();
            if (SUCCEEDED(hr)) {
                if (g.debug) printf("[DEBUG] Using IDXGIOutput1::DuplicateOutput (no HDR support)\n");
            }
        }
    }

    output->Release();
    if (FAILED(hr) || !g.duplication) Fatal("DuplicateOutput", hr);

    DXGI_OUTDUPL_DESC dd; g.duplication->GetDesc(&dd);

    g.sourceReportedHDR = (dd.ModeDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    g.sourceRefreshRate = (float)dd.ModeDesc.RefreshRate.Numerator / dd.ModeDesc.RefreshRate.Denominator;

    printf("  Reported format: %s (DXGI_FORMAT=%d)\n",
           g.sourceReportedHDR ? "HDR" : "SDR", (int)dd.ModeDesc.Format);
    printf("  Resolution: %ux%u @ %.2fHz\n",
           dd.ModeDesc.Width, dd.ModeDesc.Height, g.sourceRefreshRate);
}

void InitTripleBuffer(DXGI_FORMAT format, UINT width, UINT height) {
    if (g.debug) {
        printf("[DEBUG] InitTripleBuffer: %ux%u, Format=%d\n", width, height, (int)format);
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    DXGI_FORMAT srvFormat = format;

    for (int i = 0; i < 3; i++) {
        HRESULT hr = g.device->CreateTexture2D(&td, nullptr, &g.buffer.textures[i]);
        if (FAILED(hr)) Fatal("CreateTexture2D (triple buffer)", hr);

        D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
        srvd.Format = srvFormat;
        srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels = 1;
        hr = g.device->CreateShaderResourceView(g.buffer.textures[i], &srvd, &g.buffer.srvs[i]);
        if (FAILED(hr)) Fatal("CreateSRV (triple buffer)", hr);
    }

    if (g.debug) {
        printf("[DEBUG] Triple buffer created successfully\n");
    }
}

// Capture thread
void CaptureThreadFunc() {
    ID3D11Texture2D* sharedTex[3] = {nullptr, nullptr, nullptr};
    bool buffersOpened = false;
    int debugCounter = 0;

    while (g.running) {
        DXGI_OUTDUPL_FRAME_INFO info;
        IDXGIResource* res = nullptr;

        HRESULT hr = g.duplication->AcquireNextFrame(100, &info, &res);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            if (g.debug && (++debugCounter % 10 == 0)) {
                printf("[DEBUG] AcquireNextFrame timeout\n");
            }
            continue;
        }

        if (hr == DXGI_ERROR_ACCESS_LOST) {
            if (g.debug) printf("[DEBUG] Access lost, reinitializing...\n");
            if (g.duplication) { g.duplication->Release(); g.duplication = nullptr; }
            Sleep(100);
            InitDuplication();
            continue;
        }

        if (FAILED(hr)) {
            if (g.debug) printf("[DEBUG] AcquireNextFrame failed: 0x%08X\n", (unsigned)hr);
            continue;
        }

        // Capture cursor info (always, not just on new content)
        if (g.showCursor) {
            // Update cursor position only if we got a mouse update
            // (LastMouseUpdateTime is non-zero when there's a cursor update)
            if (info.LastMouseUpdateTime.QuadPart != 0) {
                g.cursor.visible.store(info.PointerPosition.Visible != 0, std::memory_order_relaxed);
                g.cursor.x.store(info.PointerPosition.Position.x, std::memory_order_relaxed);
                g.cursor.y.store(info.PointerPosition.Position.y, std::memory_order_relaxed);
            }

            // Get cursor shape if it changed
            if (info.PointerShapeBufferSize > 0) {
                if (g.cursor.shapeBufferSize < info.PointerShapeBufferSize) {
                    delete[] g.cursor.shapeBuffer;
                    g.cursor.shapeBuffer = new BYTE[info.PointerShapeBufferSize];
                    g.cursor.shapeBufferSize = info.PointerShapeBufferSize;
                }

                UINT bufferSizeRequired;
                DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo;
                hr = g.duplication->GetFramePointerShape(
                    g.cursor.shapeBufferSize, g.cursor.shapeBuffer,
                    &bufferSizeRequired, &shapeInfo);

                if (SUCCEEDED(hr)) {
                    g.cursor.shapeWidth = shapeInfo.Width;
                    g.cursor.shapeHeight = shapeInfo.Height;
                    g.cursor.shapePitch = shapeInfo.Pitch;
                    g.cursor.shapeType = (DXGI_OUTDUPL_POINTER_SHAPE_TYPE)shapeInfo.Type;
                    g.cursor.width.store(shapeInfo.Width, std::memory_order_relaxed);
                    g.cursor.height.store(
                        shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ?
                        shapeInfo.Height / 2 : shapeInfo.Height,
                        std::memory_order_relaxed);
                    g.cursor.hasShape.store(true, std::memory_order_relaxed);
                    g.cursor.shapeUpdated.store(true, std::memory_order_release);

                    if (g.debug) {
                        const char* typeStr = "UNKNOWN";
                        switch (shapeInfo.Type) {
                            case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME: typeStr = "MONOCHROME"; break;
                            case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR: typeStr = "COLOR"; break;
                            case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: typeStr = "MASKED_COLOR"; break;
                        }
                        printf("[DEBUG] Cursor shape: %s %ux%u pitch=%u\n",
                               typeStr, shapeInfo.Width, shapeInfo.Height, shapeInfo.Pitch);
                    }
                }
            }
        }

        // Check for new frame content
        bool hasNewContent = (info.LastPresentTime.QuadPart != 0) ||
                             (info.AccumulatedFrames > 0) ||
                             !buffersOpened;  // Always process first frame

        if (hasNewContent) {
            ID3D11Texture2D* tex;
            hr = res->QueryInterface(&tex);

            if (SUCCEEDED(hr)) {
                // On first frame, detect actual format and initialize buffers
                if (!buffersOpened) {
                    D3D11_TEXTURE2D_DESC td;
                    tex->GetDesc(&td);

                    printf("  Actual format: %s (DXGI_FORMAT=%d)\n",
                           td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? "HDR (R16G16B16A16_FLOAT)" :
                           td.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? "SDR (B8G8R8A8_UNORM)" : "Other",
                           (int)td.Format);

                    // Update global format info
                    g.sourceFormat = td.Format;
                    g.sourceIsHDR = (td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);

                    if (g.sourceIsHDR) {
                        if (g.tonemap) {
                            printf("  Processing: maxRGB Reinhard tonemapping (HDR to SDR, sdrWhite=%.0f nits)\n", g.sdrWhiteNits);
                        } else {
                            printf("  Processing: None (--no-tonemap, HDR values may clip)\n");
                        }
                    } else {
                        printf("  Processing: Passthrough (SDR)\n");
                    }

                    // Initialize triple buffer with actual format
                    InitTripleBuffer(td.Format, td.Width, td.Height);

                    // Open shared handles
                    for (int i = 0; i < 3; i++) {
                        IDXGIResource* bufRes;
                        g.buffer.textures[i]->QueryInterface(&bufRes);
                        HANDLE sharedHandle;
                        bufRes->GetSharedHandle(&sharedHandle);
                        bufRes->Release();

                        hr = g.capDevice->OpenSharedResource(sharedHandle,
                            __uuidof(ID3D11Texture2D), (void**)&sharedTex[i]);
                        if (FAILED(hr)) Fatal("OpenSharedResource", hr);
                    }

                    buffersOpened = true;

                    if (g.debug) {
                        printf("[DEBUG] Buffers initialized with actual format\n");
                    }
                }

                int writeIdx = g.buffer.GetWriteIndex();
                g.capContext->CopyResource(sharedTex[writeIdx], tex);
                g.capContext->Flush();

                UINT64 frameId = g.captureFrameId.fetch_add(1, std::memory_order_relaxed) + 1;
                g.buffer.PublishFrame(frameId);
                g.captureCount.fetch_add(1, std::memory_order_relaxed);

                // Signal buffer ready AFTER first frame is copied and published
                if (!g.bufferInitialized.load(std::memory_order_relaxed)) {
                    g.bufferInitialized.store(true, std::memory_order_release);
                }

                tex->Release();
            } else {
                if (g.debug) printf("[DEBUG] QueryInterface for texture failed: 0x%08X\n", (unsigned)hr);
            }
        }

        res->Release();
        g.duplication->ReleaseFrame();
    }

    for (int i = 0; i < 3; i++) {
        if (sharedTex[i]) sharedTex[i]->Release();
    }
}

static int s_renderDebugCounter = 0;
static bool s_firstRenderDone = false;

void Render() {
    if (!g.bufferInitialized.load(std::memory_order_acquire)) {
        if (g.debug && (++s_renderDebugCounter % 60 == 0)) {
            printf("[DEBUG] Render: buffer not initialized\n");
        }
        return;
    }

    int readIdx = g.buffer.AcquireFrame();
    if (readIdx < 0) {
        if (g.debug && (++s_renderDebugCounter % 60 == 0)) {
            printf("[DEBUG] Render: no frame available (readIdx=%d, writeIdx=%d, readyIdx=%d, displayIdx=%d)\n",
                   readIdx,
                   g.buffer.writeIdx.load(),
                   g.buffer.readyIdx.load(),
                   g.buffer.displayIdx.load());
        }
        return;
    }

    ID3D11ShaderResourceView* srv = g.buffer.srvs[readIdx];
    if (!srv) {
        if (g.debug) printf("[DEBUG] Render: SRV is null for readIdx=%d\n", readIdx);
        return;
    }

    if (g.debug && !s_firstRenderDone) {
        printf("[DEBUG] First render: readIdx=%d, sourceIsHDR=%d, tonemap=%d\n",
               readIdx, g.sourceIsHDR, g.tonemap);
        s_firstRenderDone = true;
    }

    float black[] = {0,0,0,1};
    g.context->OMSetRenderTargets(1, &g.rtv, nullptr);
    g.context->ClearRenderTargetView(g.rtv, black);
    g.context->RSSetViewports(1, &g.viewport);

    g.context->VSSetShader(g.vs, 0, 0);

    // Select pixel shader based on source format:
    // - sourceIsHDR (actual R16G16B16A16_FLOAT): use HDR tonemapping shader
    // - SDR source: use passthrough shader
    if (g.sourceIsHDR && g.tonemap) {
        // Update HDR constant buffer with sdrWhiteNits value
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(g.context->Map(g.cbHDR, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            float* data = (float*)mapped.pData;
            data[0] = g.sdrWhiteNits;
            data[1] = 0.0f;  // padding
            data[2] = 0.0f;
            data[3] = 0.0f;
            g.context->Unmap(g.cbHDR, 0);
        }
        g.context->PSSetConstantBuffers(0, 1, &g.cbHDR);
        g.context->PSSetShader(g.psHDR, 0, 0);
    } else {
        g.context->PSSetShader(g.psSDR, 0, 0);
    }

    g.context->PSSetShaderResources(0, 1, &srv);
    g.context->PSSetSamplers(0, 1, &g.sampler);

    UINT stride = sizeof(Vertex), offset = 0;
    g.context->IASetVertexBuffers(0, 1, &g.vb, &stride, &offset);
    g.context->IASetInputLayout(g.layout);
    g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    g.context->Draw(4, 0);

    // Render cursor if visible and we have a shape
    if (g.showCursor && g.cursor.hasShape.load(std::memory_order_relaxed) &&
        g.cursor.visible.load(std::memory_order_relaxed)) {
        // Update cursor texture if shape changed
        if (g.cursor.shapeUpdated.exchange(false, std::memory_order_acquire)) {
            // Release old texture
            if (g.cursorSrv) { g.cursorSrv->Release(); g.cursorSrv = nullptr; }
            if (g.cursorTex) { g.cursorTex->Release(); g.cursorTex = nullptr; }

            UINT width = g.cursor.shapeWidth;
            UINT height = g.cursor.shapeHeight;
            if (g.cursor.shapeType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                height /= 2;  // Monochrome cursors are AND mask + XOR mask
            }

            // Create cursor texture
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = width;
            td.Height = height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            // Convert cursor data to BGRA with proper alpha
            UINT* pixels = new UINT[width * height];

            if (g.cursor.shapeType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                // Monochrome: AND mask then XOR mask, each 1 bit per pixel
                UINT pitch = g.cursor.shapePitch;
                for (UINT y = 0; y < height; y++) {
                    for (UINT x = 0; x < width; x++) {
                        UINT byteIdx = x / 8;
                        UINT bitIdx = 7 - (x % 8);
                        BYTE andMask = (g.cursor.shapeBuffer[y * pitch + byteIdx] >> bitIdx) & 1;
                        BYTE xorMask = (g.cursor.shapeBuffer[(y + height) * pitch + byteIdx] >> bitIdx) & 1;

                        if (andMask == 0 && xorMask == 0) {
                            pixels[y * width + x] = 0xFF000000;  // Black, opaque
                        } else if (andMask == 0 && xorMask == 1) {
                            pixels[y * width + x] = 0xFFFFFFFF;  // White, opaque
                        } else if (andMask == 1 && xorMask == 0) {
                            pixels[y * width + x] = 0x00000000;  // Transparent
                        } else {
                            // XOR (invert) - render as semi-transparent white
                            pixels[y * width + x] = 0x80FFFFFF;
                        }
                    }
                }
            } else if (g.cursor.shapeType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                // Color cursor: BGRA format with proper alpha channel
                // These cursors use standard alpha blending:
                // - Alpha = 0: fully transparent
                // - Alpha = 255: fully opaque
                // - Alpha values in between: semi-transparent
                // Note: Don't treat black as transparent - that breaks I-beam cursors!
                for (UINT y = 0; y < height; y++) {
                    for (UINT x = 0; x < width; x++) {
                        BYTE* src = &g.cursor.shapeBuffer[y * g.cursor.shapePitch + x * 4];
                        BYTE b = src[0], gc = src[1], r = src[2], a = src[3];
                        // Use alpha channel directly - this is the standard format
                        pixels[y * width + x] = (a << 24) | (r << 16) | (gc << 8) | b;
                    }
                }
            } else if (g.cursor.shapeType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                // Masked color cursor format:
                // - Alpha = 0x00: XOR pixel (invert) - we approximate with semi-transparent
                // - Alpha = 0xFF: Replace pixel with RGB color (solid cursor pixel)
                for (UINT y = 0; y < height; y++) {
                    for (UINT x = 0; x < width; x++) {
                        BYTE* src = &g.cursor.shapeBuffer[y * g.cursor.shapePitch + x * 4];
                        BYTE b = src[0], gc = src[1], r = src[2], a = src[3];

                        if (a == 0xFF) {
                            // Solid pixel - use the color with full opacity
                            pixels[y * width + x] = 0xFF000000 | (r << 16) | (gc << 8) | b;
                        } else if (a == 0 && (r | gc | b) != 0) {
                            // XOR pixel with color - approximate as semi-transparent
                            pixels[y * width + x] = 0x80000000 | (r << 16) | (gc << 8) | b;
                        } else {
                            // Transparent
                            pixels[y * width + x] = 0x00000000;
                        }
                    }
                }
            }

            D3D11_SUBRESOURCE_DATA initData = {};
            initData.pSysMem = pixels;
            initData.SysMemPitch = width * 4;

            g.device->CreateTexture2D(&td, &initData, &g.cursorTex);
            delete[] pixels;

            if (g.cursorTex) {
                D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
                srvd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvd.Texture2D.MipLevels = 1;
                g.device->CreateShaderResourceView(g.cursorTex, &srvd, &g.cursorSrv);
            }
        }

        // Draw cursor if we have a texture
        if (g.cursorSrv) {
            int cursorX = g.cursor.x.load(std::memory_order_relaxed);
            int cursorY = g.cursor.y.load(std::memory_order_relaxed);
            int cursorW = g.cursor.width.load(std::memory_order_relaxed);
            int cursorH = g.cursor.height.load(std::memory_order_relaxed);

            // Calculate source dimensions
            float srcW = (float)(g.sourceRect.right - g.sourceRect.left);
            float srcH = (float)(g.sourceRect.bottom - g.sourceRect.top);

            // Convert cursor position to viewport coordinates
            float vpX = g.viewport.TopLeftX;
            float vpY = g.viewport.TopLeftY;
            float vpW = g.viewport.Width;
            float vpH = g.viewport.Height;

            // Scale cursor position and size
            float scaleX = vpW / srcW;
            float scaleY = vpH / srcH;

            float cx = vpX + cursorX * scaleX;
            float cy = vpY + cursorY * scaleY;
            float cw = cursorW * scaleX;
            float ch = cursorH * scaleY;

            // Convert to NDC (-1 to 1)
            float ndcX1 = (cx / g.windowWidth) * 2.0f - 1.0f;
            float ndcY1 = 1.0f - (cy / g.windowHeight) * 2.0f;
            float ndcX2 = ((cx + cw) / g.windowWidth) * 2.0f - 1.0f;
            float ndcY2 = 1.0f - ((cy + ch) / g.windowHeight) * 2.0f;

            // Update cursor vertex buffer
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(g.context->Map(g.cursorVb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                Vertex* verts = (Vertex*)mapped.pData;
                verts[0] = {ndcX1, ndcY1, 0, 0};  // Top-left
                verts[1] = {ndcX2, ndcY1, 1, 0};  // Top-right
                verts[2] = {ndcX1, ndcY2, 0, 1};  // Bottom-left
                verts[3] = {ndcX2, ndcY2, 1, 1};  // Bottom-right
                g.context->Unmap(g.cursorVb, 0);
            }

            // Enable alpha blending
            float blendFactor[4] = {0, 0, 0, 0};
            g.context->OMSetBlendState(g.blendState, blendFactor, 0xFFFFFFFF);

            // Draw cursor
            g.context->PSSetShader(g.psCursor, 0, 0);
            g.context->PSSetShaderResources(0, 1, &g.cursorSrv);
            UINT stride = sizeof(Vertex), offset = 0;
            g.context->IASetVertexBuffers(0, 1, &g.cursorVb, &stride, &offset);
            g.context->Draw(4, 0);

            // Disable blending
            g.context->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
        }
    }

    ID3D11ShaderResourceView* null = nullptr;
    g.context->PSSetShaderResources(0, 1, &null);
}

void Cleanup() {
    g.running = false;

    // Wait for capture thread
    if (g.captureThread.joinable()) {
        g.captureThread.join();
    }

    // Release triple buffer (may not be initialized if we exit early)
    if (g.bufferInitialized.load(std::memory_order_acquire)) {
        for (int i = 0; i < 3; i++) {
            if (g.buffer.srvs[i]) { g.buffer.srvs[i]->Release(); g.buffer.srvs[i] = nullptr; }
            if (g.buffer.textures[i]) { g.buffer.textures[i]->Release(); g.buffer.textures[i] = nullptr; }
        }
    }

    // Release capture resources
    if (g.duplication) { g.duplication->Release(); g.duplication = nullptr; }
    if (g.capContext) { g.capContext->Release(); g.capContext = nullptr; }
    if (g.capDevice) { g.capDevice->Release(); g.capDevice = nullptr; }

    // Release cursor resources
    if (g.cursorSrv) { g.cursorSrv->Release(); g.cursorSrv = nullptr; }
    if (g.cursorTex) { g.cursorTex->Release(); g.cursorTex = nullptr; }
    if (g.cursorVb) { g.cursorVb->Release(); g.cursorVb = nullptr; }
    if (g.blendState) { g.blendState->Release(); g.blendState = nullptr; }
    if (g.psCursor) { g.psCursor->Release(); g.psCursor = nullptr; }
    delete[] g.cursor.shapeBuffer; g.cursor.shapeBuffer = nullptr;

    // Release render resources
    if (g.sampler) { g.sampler->Release(); g.sampler = nullptr; }
    if (g.cbHDR) { g.cbHDR->Release(); g.cbHDR = nullptr; }
    if (g.vb) { g.vb->Release(); g.vb = nullptr; }
    if (g.layout) { g.layout->Release(); g.layout = nullptr; }
    if (g.psHDR) { g.psHDR->Release(); g.psHDR = nullptr; }
    if (g.psSDRGamma) { g.psSDRGamma->Release(); g.psSDRGamma = nullptr; }
    if (g.psSDR) { g.psSDR->Release(); g.psSDR = nullptr; }
    if (g.vs) { g.vs->Release(); g.vs = nullptr; }
    if (g.rtv) { g.rtv->Release(); g.rtv = nullptr; }
    if (g.frameLatencyWaitable) { CloseHandle(g.frameLatencyWaitable); g.frameLatencyWaitable = nullptr; }
    if (g.swapChain) { g.swapChain->Release(); g.swapChain = nullptr; }
    if (g.context) { g.context->Release(); g.context = nullptr; }
    if (g.device) { g.device->Release(); g.device = nullptr; }
    if (g.hwnd) { DestroyWindow(g.hwnd); g.hwnd = nullptr; }
}

void PrintUsage(const char* prog) {
    printf("DXGI Desktop Mirror\n\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("  --source N       Source monitor (default: 0)\n");
    printf("  --target N       Target monitor (default: 1)\n");
    printf("  --stretch        Stretch to fill (ignore aspect ratio)\n");
    printf("  --no-tonemap     Disable HDR to SDR tonemapping\n");
    printf("  --sdr-white N    SDR white level in nits for HDR tonemapping (default: 240)\n");
    printf("  --no-cursor      Hide the mouse cursor\n");
    printf("  --no-waitable    Disable waitable swap chain (frame pacing)\n");
    printf("  --no-smart-select Disable smart frame selection (use fixed delay)\n");
    printf("  --no-frame-delay Disable frame delay (frame pacing fallback)\n");
    printf("  --frame-delay N  Frame delay in microseconds (default: 1000 = 1ms)\n");
    printf("  --debug          Enable debug output\n");
    printf("  --list           List monitors\n");
}

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Install console control handler for graceful CTRL+C shutdown
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--source") && i+1 < argc) g.sourceMonitor = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--target") && i+1 < argc) g.targetMonitor = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stretch")) g.preserveAspect = false;
        else if (!strcmp(argv[i], "--no-tonemap")) g.tonemap = false;
        else if (!strcmp(argv[i], "--sdr-white") && i+1 < argc) g.sdrWhiteNits = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--no-cursor")) g.showCursor = false;
        else if (!strcmp(argv[i], "--no-waitable")) g.useWaitableSwapChain = false;
        else if (!strcmp(argv[i], "--no-smart-select")) g.useSmartFrameSelection = false;
        else if (!strcmp(argv[i], "--no-frame-delay")) g.useFrameDelay = false;
        else if (!strcmp(argv[i], "--frame-delay") && i+1 < argc) g.frameDelayUs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--debug")) g.debug = true;
        else if (!strcmp(argv[i], "--list")) { PrintMonitors(); return 0; }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { PrintUsage(argv[0]); return 0; }
        else { fprintf(stderr, "Unknown: %s\n", argv[i]); return 1; }
    }

    int mc = GetMonitorCount();
    if (g.sourceMonitor < 0 || g.sourceMonitor >= mc) { fprintf(stderr, "Invalid source\n"); return 1; }
    if (g.targetMonitor < 0 || g.targetMonitor >= mc) { fprintf(stderr, "Invalid target\n"); return 1; }
    if (g.sourceMonitor == g.targetMonitor) { fprintf(stderr, "Source == target\n"); return 1; }

    GetMonitorRect(g.sourceMonitor, &g.sourceRect);
    GetMonitorRect(g.targetMonitor, &g.targetRect);

    printf("DXGI Desktop Mirror\n");
    printf("  Source: %d (%dx%d)\n", g.sourceMonitor,
           g.sourceRect.right-g.sourceRect.left, g.sourceRect.bottom-g.sourceRect.top);
    printf("  Target: %d (%dx%d)\n", g.targetMonitor,
           g.targetRect.right-g.targetRect.left, g.targetRect.bottom-g.targetRect.top);
    printf("  Output: VSync\n");

    CreateWindow_();
    InitD3D();
    InitDuplication();
    InitShaders();

    // Detect target refresh rate from the swap chain
    {
        IDXGIOutput* output = nullptr;
        if (SUCCEEDED(g.swapChain->GetContainingOutput(&output))) {
            DXGI_OUTPUT_DESC outputDesc;
            output->GetDesc(&outputDesc);

            DXGI_MODE_DESC modeDesc = {};
            modeDesc.Width = g.windowWidth;
            modeDesc.Height = g.windowHeight;
            modeDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

            DXGI_MODE_DESC closestMatch;
            if (SUCCEEDED(output->FindClosestMatchingMode(&modeDesc, &closestMatch, g.device))) {
                g.targetRefreshRate = (float)closestMatch.RefreshRate.Numerator / closestMatch.RefreshRate.Denominator;
            }
            output->Release();
        }

        // Calculate target frame skip for smart frame selection
        // e.g., 120Hz source / 60Hz target = 2 (show every 2nd frame)
        if (g.sourceRefreshRate > 0 && g.targetRefreshRate > 0) {
            g.targetFrameSkip = (int)round(g.sourceRefreshRate / g.targetRefreshRate);
            if (g.targetFrameSkip < 1) g.targetFrameSkip = 1;
        }

        printf("  Target: %.2fHz (frame skip: %d)\n", g.targetRefreshRate, g.targetFrameSkip);

        // Print frame pacing strategy
        if (g.targetFrameSkip > 1 && g.useSmartFrameSelection) {
            printf("  Frame pacing: Smart selection (wait for frame N+%d)\n", g.targetFrameSkip);
        } else if (g.useFrameDelay && g.frameDelayUs > 0) {
            printf("  Frame pacing: Fixed delay (%d µs)\n", g.frameDelayUs);
        } else {
            printf("  Frame pacing: None (immediate)\n");
        }
    }

    // Triple buffer is initialized by capture thread on first frame
    // (to detect actual format, which may differ from reported format)

    timeBeginPeriod(1);

    g.captureThread = std::thread(CaptureThreadFunc);

    // Wait for first frame to initialize buffers (with timeout)
    printf("  Waiting for first frame...\n");
    int waitCount = 0;
    while (g.running && !g.bufferInitialized.load(std::memory_order_acquire)) {
        Sleep(10);
        waitCount++;
        if (waitCount > 500) {  // 5 second timeout
            fprintf(stderr, "ERROR: Timeout waiting for first frame. Is the source monitor active?\n");
            fprintf(stderr, "       Try moving your mouse on the source monitor to trigger an update.\n");
            Cleanup();
            return 1;
        }
        if (g.debug && waitCount % 100 == 0) {
            printf("[DEBUG] Still waiting for first frame... (%d ms)\n", waitCount * 10);
        }
    }

    if (!g.running) {
        Cleanup();
        return 0;
    }

    printf("\nPress ESC to exit (or CTRL+C).\n\n");

    LARGE_INTEGER freq, lastStat, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&lastStat);

    int outCount = 0, uniqCount = 0, dupCount = 0;

    MSG msg;
    while (g.running) {
        // Wait for VSync timing signal (if waitable swap chain is available)
        // This ensures we acquire the freshest frame right after VSync
        if (g.frameLatencyWaitable) {
            WaitForSingleObjectEx(g.frameLatencyWaitable, 100, TRUE);
        }

        // Smart frame selection: only wait for next frame if desktop is active
        // This ensures consistent Skip:2-2 for 120Hz→60Hz while maintaining
        // 60 FPS output when desktop is idle (showing duplicate frames)
        if (g.useSmartFrameSelection && g.targetFrameSkip > 1) {
            UINT64 currentCapture = g.captureFrameId.load(std::memory_order_relaxed);

            // Check if new frames are coming in (desktop is active)
            if (currentCapture > g.lastCaptureCheckId) {
                // Desktop is active - wait for the right frame
                UINT64 targetId = g.lastRenderedId + g.targetFrameSkip;

                // Only wait if we haven't reached target yet
                if (currentCapture < targetId) {
                    // Use frame delay to wait for the next frame
                    if (g.useFrameDelay && g.frameDelayUs > 0) {
                        DelayMicroseconds(g.frameDelayUs);
                    }
                }
            }
            // Update check ID for next iteration
            g.lastCaptureCheckId = currentCapture;
        } else if (g.useFrameDelay && g.frameDelayUs > 0) {
            // Fallback: simple fixed delay
            DelayMicroseconds(g.frameDelayUs);
        }

        while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g.running = false; break; }
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        if (!g.running) break;

        Render();
        g.swapChain->Present(1, 0);

        outCount++;

        UINT64 currentFrameId = g.captureFrameId.load(std::memory_order_relaxed);
        if (currentFrameId != g.lastRenderedId) {
            // Track frame skip delta for pacing analysis
            if (g.lastRenderedId > 0) {
                int skipDelta = (int)(currentFrameId - g.lastRenderedId);
                if (skipDelta < g.frameSkipMin) g.frameSkipMin = skipDelta;
                if (skipDelta > g.frameSkipMax) g.frameSkipMax = skipDelta;
                g.frameSkipTotal += skipDelta;
                g.frameSkipCount++;
            }
            uniqCount++;
            g.lastRenderedId = currentFrameId;
        } else {
            dupCount++;
        }

        QueryPerformanceCounter(&now);
        double statElapsed = (double)(now.QuadPart - lastStat.QuadPart) / freq.QuadPart;
        if (statElapsed >= 1.0) {
            int capCount = g.captureCount.exchange(0, std::memory_order_relaxed);
            int dropCount = capCount > outCount ? capCount - outCount : 0;

            // Calculate average frame skip
            float avgSkip = g.frameSkipCount > 0 ? (float)g.frameSkipTotal / g.frameSkipCount : 0;

            printf("\rOut:%3d Cap:%3d Uniq:%3d Dup:%3d Drop:%3d Skip:%d-%d(%.1f)   ",
                   outCount, capCount, uniqCount, dupCount, dropCount,
                   g.frameSkipMin == INT_MAX ? 0 : g.frameSkipMin, g.frameSkipMax, avgSkip);
            fflush(stdout);

            // Reset stats
            outCount = uniqCount = dupCount = 0;
            g.frameSkipMin = INT_MAX;
            g.frameSkipMax = 0;
            g.frameSkipTotal = 0;
            g.frameSkipCount = 0;
            lastStat = now;
        }
    }

    timeEndPeriod(1);
    printf("\nShutting down...\n");
    Cleanup();
    printf("Done.\n");
    return 0;
}
