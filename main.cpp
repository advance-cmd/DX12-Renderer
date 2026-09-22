#include "main.h"
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <string>
#include <wrl.h>

#include <d3d12.h>          // 所有 ID3D12xxx 接口、D3D12_xxx 结构体和枚举
#include <dxgi1_4.h>        // IDXGIFactory4、IDXGISwapChain、适配器枚举
#include <d3dcompiler.h>    // D3DCompileFromFile，第 6 章编译 HLSL 时才用
#include <DirectXColors.h>  // Colors::LightSteelBlue 之类的预定义颜色

extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 619; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

using namespace std;
using namespace DirectX;
using namespace DirectX::PackedVector;

//启用D3D12的调试层（只在 Debug 下生效，必须在创建设备之前调用）
void EnableD3DDebugLayer()
{
#if defined(DEBUG) || defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)));
    debugController->EnableDebugLayer();
#endif
}

//创建DXGI工厂（Debug 下带调试标志，DXGI 层的错误也能被捕获）
void InitDxgiFactory()
{
    UINT flags = 0;
#if defined(DEBUG) || defined(_DEBUG)
    flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    ThrowIfFailed(CreateDXGIFactory2(flags, IID_PPV_ARGS(&gDxgiFactory)));
}

//创建设备：先尝试硬件设备，失败则回退到WARP
void CreateD3DDevice()
{
    //尝试创建硬件设备
    //这里不用 ThrowIfFailed：硬件设备失败是预期情况，要回退到 WARP，而不是直接报错退出
    HRESULT hardwareResult = D3D12CreateDevice(
        nullptr,
        D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&gDevice));

    //回退WARP
    if (FAILED(hardwareResult))
    {
        //拿到软件适配器
        ComPtr<IDXGIAdapter> adapter;
        ThrowIfFailed(gDxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));

        //创建WARP设备
        ThrowIfFailed(D3D12CreateDevice(
            adapter.Get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&gDevice)));
    }
}

//创建围栏，并查询三种描述符的大小（大小因显卡而异，只能查，不能写死）
void CreateFenceAndDescriptorSizes()
{
    ThrowIfFailed(gDevice->CreateFence(
        0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gFence)));

    gRTVDescriptorSize = gDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    gDSVDescriptorSize = gDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    gCSUDescriptorSize = gDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

//检测对 4X MSAA 质量级别的支持
void Check4xMsaaQuality()
{
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels;
    msQualityLevels.Format = gBackBufferFormat;
    msQualityLevels.SampleCount = 4;
    msQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
    msQualityLevels.NumQualityLevels = 0;

    ThrowIfFailed(gDevice->CheckFeatureSupport(
        D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
        &msQualityLevels,
        sizeof(msQualityLevels)));

    g4xMsaaQuality = msQualityLevels.NumQualityLevels;
    assert(g4xMsaaQuality > 0 && "Unexpected MSAA quality level");
}

//创建命令队列、命令分配器和命令列表
void CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(gDevice->CreateCommandQueue(
        &desc,IID_PPV_ARGS(&gCommandQueue)));
    ThrowIfFailed(gDevice->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(gCommandAllocator.GetAddressOf())));
    ThrowIfFailed(gDevice->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        gCommandAllocator.Get(),
        nullptr,    //初始化流水线状态对象
        IID_PPV_ARGS(gCommandList.GetAddressOf())));
    ThrowIfFailed(gCommandList->Close());
}

void CreateSwapChain()
{
    
    gSwapChain.Reset();
    
    DXGI_SWAP_CHAIN_DESC1 desc = {
        .Width = 0,
        .Height = 0,
        .Format = gBackBufferFormat,
        .Stereo = false,
        .SampleDesc = {
            .Count = 1,
            .Quality = 0
        },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = gSwapChainBufferCount,
        .Scaling = DXGI_SCALING_STRETCH,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
        .Flags = 0
    };
    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(gDxgiFactory->CreateSwapChainForHwnd(
        gCommandQueue.Get(),
        ghMainWnd,&desc,
        nullptr,
        nullptr,
        &swapChain1));
    
    ThrowIfFailed(swapChain1.As(&gSwapChain));

    DXGI_SWAP_CHAIN_DESC1 actual = {};
    ThrowIfFailed(gSwapChain->GetDesc1(&actual));
    gClientWidth  = actual.Width;
    gClientHeight = actual.Height;
    
    ThrowIfFailed(gDxgiFactory->MakeWindowAssociation(ghMainWnd, DXGI_MWA_NO_ALT_ENTER));
}


bool InitWindowsApp(HINSTANCE instanceHandle, int show)
{
    // 通过填写WNDCLASS结构体，根据其中描述的特征来创建一个窗口
    WNDCLASS wc;

    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = instanceHandle;
    wc.hIcon = LoadIcon(0, IDI_APPLICATION);
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszMenuName = 0;
    wc.lpszClassName = L"BasicWndClass";

    //下一步，我们要在Windows系统中为上述WNDCLASS注册一个实例，这样一来，即可据此创建窗口
    if (!RegisterClass(&wc))
    {
        MessageBox(0, L"RegisterClass FALLED", 0, 0);
        return false;
    }
    //注册过WNDCLASS实例之后，我们就能用CreateWindow函数来创建一个窗口了。此函数返回的就
    //是所建窗口的句柄(一个HWND类型的数值)。如果创建失败，返回的句柄数值为0。窗口句柄是一种
    //窗口的引用方式， Windows系统会在内部对此进行管理。处理窗口的大量Win32 API函数都需要
    //把HWND类型作为参数，以此来确定要对哪一个窗口执行相应的操作
    ghMainWnd = CreateWindow(
        L"BasicWndClass",
        L"Win32Basic",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        0,
        0,
        instanceHandle,
        0);
    if (ghMainWnd == 0)
    {
        MessageBox(0, L"CreateWindow FALLED", 0, 0);
        return false;
    }
    //调用展示和更新函数
    ShowWindow(ghMainWnd, show);
    UpdateWindow(ghMainWnd);

    return true;
}

int Run()
{
    MSG msg = {0};

    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))   // 有消息：照常翻译、分发给 WndProc
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else                                              // 没消息：跑游戏逻辑 + 渲染一帧
        {
            // Update(); Draw();
        }
    }

    return (int)msg.wParam;
}

LRESULT WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            SendMessage(hWnd, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
        if (MessageBox(hWnd, L"确定关闭？", L"提醒", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
        {
            DestroyWindow(hWnd);
        }
        return 0;   // 点了取消就直接返回，窗口保持打开
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
}


int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
    // ThrowIfFailed 失败时会抛 DxException，在这里统一接住，弹窗显示是哪句代码、第几行出的错
    try
    {
        //将hInstance和nShowCmd作为参数来调用封装函数InitWindowsApp，用来创建和初始化应用程序的主窗口
        if (!InitWindowsApp(hInstance, nShowCmd)) {
            return 0;
        }

        //初始化 Direct3D（顺序不能乱：调试层必须在设备之前，后面的都依赖设备）
        EnableD3DDebugLayer();
        InitDxgiFactory();
        CreateD3DDevice();
        CreateFenceAndDescriptorSizes();
        Check4xMsaaQuality();
        CreateCommandObjects();
        CreateSwapChain();

        //初始化完毕后就可以开启消息循环了
        return Run();
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK | MB_ICONERROR);
        return 0;
    }
}
