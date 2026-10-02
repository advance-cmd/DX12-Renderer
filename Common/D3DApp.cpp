#include "D3DApp.h"

#include <cassert>
#include <format>
#include <windowsx.h>
#include <dxgi1_6.h>    // IDXGIFactory6::EnumAdapterByGpuPreference：按性能挑显卡

// 窗口类只认普通函数指针，填不了成员函数，这里转发给当前 App 的 MsgProc
static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    try
    {
        return D3DApp::GetApp()->MsgProc(hWnd, msg, wParam, lParam);
    }catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK | MB_ICONERROR);
        PostQuitMessage(0);   // 出错了就退出程序
        return 0;
    }
}

D3DApp::D3DApp(HINSTANCE hInstance) : mhAppInst(hInstance)
{
    // 整个程序只允许有一个 App
    assert(mApp == nullptr);
    mApp = this;
}

D3DApp::~D3DApp()
{
    if (mCommandQueue != nullptr)
    {
        FlushCommandQueue();
    }
}

D3DApp* D3DApp::GetApp()
{
    return mApp;
}

bool D3DApp::Initialize()
{
    if (!InitMainWindow())
    {
        return false;
    }

    // 出错时 ThrowIfFailed 直接抛异常，由 WinMain 里的 catch 接住，所以这里不用返回值
    InitDirect3D();
    OnResize();

    return true;
}

int D3DApp::Run()
{
    MSG msg = {0};

    mTimer.Reset(); // 紧挨着消息循环：时间零点从这里开始

    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) // 有消息：照常翻译、分发给 WndProc
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else // 没消息：跑游戏逻辑 + 渲染一帧
        {
            mTimer.Tick();
            if (!mAppPaused)
            {
                CalculateFrameStats(); // Tick 之后，这样 TotalTime 是这一帧最新的时间
                Update(mTimer);
                Draw(mTimer);
            }
            else
            {
                Sleep(100); //暂停时让出CPU，否则这个空循环会把一个核占满
            }
        }
    }

    return (int)msg.wParam;
}

LRESULT D3DApp::MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
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
        return 0; // 点了取消就直接返回，窗口保持打开

    case WM_ACTIVATE:
        //窗口获得或失去焦点
        if (LOWORD(wParam) == WA_INACTIVE)
        {
            mAppPaused = true;
            mTimer.Stop();
        }
        else
        {
            mAppPaused = false;
            mTimer.Start();
        }
        return 0;

    case WM_SIZE:
        //窗口大小改变了
        mClientHeight = HIWORD(lParam);
        mClientWidth = LOWORD(lParam);
        if (mSwapChain == nullptr) //D3D还没初始化完，先只记下尺寸
            return 0;
        if (wParam == SIZE_MINIMIZED)
        {
            //最小化客户端
            mAppPaused = true;
            mMinmized = true;
        }
        else if (wParam == SIZE_MAXIMIZED || wParam == SIZE_RESTORED)
        {
            if (mMinmized)
            {
                //从最小化恢复
                mAppPaused = false;
                mMinmized = false;
            }
            if (!mResizing)
            {
                //正在拖边框时先不重建，松手后统一处理
                OnResize();
            }
        }
        return 0;

    case WM_ENTERSIZEMOVE:
        //开始拖动边框或标题栏
        mAppPaused = true;
        mResizing = true;
        mTimer.Stop();
        return 0;

    case WM_EXITSIZEMOVE:
        //松开鼠标
        mAppPaused = false;
        mResizing = false;
        mTimer.Start();
        OnResize();
        return 0;

    case WM_GETMINMAXINFO:
        //限制窗口最小尺寸
        ((MINMAXINFO*)lParam)->ptMinTrackSize.x = 200;
        ((MINMAXINFO*)lParam)->ptMinTrackSize.y = 200;
        return 0;
        
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
        OnMouseDown(wParam,GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam));
        return 0;
        
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
        OnMouseUp(wParam,GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam));
        return 0;
        
    case WM_MOUSEMOVE:
        OnMouseMove(wParam,GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam));
        return 0;

    case WM_MOUSEWHEEL:
        OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;

    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
}

bool D3DApp::InitMainWindow()
{
    // 通过填写WNDCLASS结构体，根据其中描述的特征来创建一个窗口
    WNDCLASS wc;

    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = mhAppInst;
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
    mhMainWnd = CreateWindow(
        L"BasicWndClass",
        mMainWndCaption.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        0,
        0,
        mhAppInst,
        0);
    if (mhMainWnd == 0)
    {
        MessageBox(0, L"CreateWindow FALLED", 0, 0);
        return false;
    }
    //调用展示和更新函数
    ShowWindow(mhMainWnd, SW_SHOW);
    UpdateWindow(mhMainWnd);

    return true;
}

//初始化 Direct3D（顺序不能乱：调试层必须在设备之前，后面的都依赖设备）
void D3DApp::InitDirect3D()
{
    EnableD3DDebugLayer();
    InitDxgiFactory();
    CreateD3DDevice();
    CreateFenceAndDescriptorSizes();
    Check4xMsaaQuality();
    CreateCommandObjects();
    CreateSwapChain();
    CreateDescHeaps();
}

// 和窗口尺寸有关的资源：RTV、深度缓冲、视口/裁剪矩形
// 初始化时调用一次，之后窗口大小一变（WM_SIZE / WM_EXITSIZEMOVE）就再调用一次，全部重建
void D3DApp::OnResize()
{
    //等GPU用完旧缓冲
    FlushCommandQueue();

    //释放对旧缓冲有用的所有引用
    for (UINT i = 0; i < SwapChainBufferCount; i++)
    {
        mSwapChainBuffer[i].Reset();
    }
    mDepthStencilBuffer.Reset();

    //让交换链按新尺寸重新分配缓冲
    ThrowIfFailed(mSwapChain->ResizeBuffers(
        SwapChainBufferCount,mClientWidth,mClientHeight,mBackBufferFormat,0));

    //重建
    CreateSwapChainRTV();
    CreateDepthAndStencil();
    UpdateViewportAndScissor();
}

//启用D3D12的调试层（只在 Debug 下生效，必须在创建设备之前调用）
void D3DApp::EnableD3DDebugLayer()
{
#if defined(DEBUG) || defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)));
    debugController->EnableDebugLayer();
#endif
}

//创建DXGI工厂（Debug 下带调试标志，DXGI 层的错误也能被捕获）
void D3DApp::InitDxgiFactory()
{
    UINT flags = 0;
#if defined(DEBUG) || defined(_DEBUG)
    flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    ThrowIfFailed(CreateDXGIFactory2(flags, IID_PPV_ARGS(&mDxgiFactory)));
}

//创建设备：优先用高性能显卡（独显），都不行再用默认显卡，最后回退到 WARP
void D3DApp::CreateD3DDevice()
{
    //传 nullptr 用的是"默认显卡"（第 0 块），双显卡笔记本上通常是集显。
    //所以先按"高性能优先"的顺序把显卡排一遍，挑第一块能建出 D3D12 设备的硬件显卡
    HRESULT hardwareResult = E_FAIL;
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(mDxgiFactory.As(&factory6))) //IDXGIFactory6 要 Win10 1803 以上，拿不到就跳过
    {
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;
             factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
             i++)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) //软件显卡（WARP）留到最后再说
            {
                continue;
            }
            hardwareResult = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&mDevice));
            if (SUCCEEDED(hardwareResult))
            {
                mAdapterName = desc.Description;
                break;
            }
        }
    }

    //上面没成功：退回默认显卡
    //这里不用 ThrowIfFailed：硬件设备失败是预期情况，要回退到 WARP，而不是直接报错退出
    if (FAILED(hardwareResult))
    {
        hardwareResult = D3D12CreateDevice(
            nullptr,
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&mDevice));
        mAdapterName = L"默认显卡";
    }

    //回退WARP
    if (FAILED(hardwareResult))
    {
        mAdapterName = L"WARP（软件渲染）";
        //拿到软件适配器
        ComPtr<IDXGIAdapter> adapter;
        ThrowIfFailed(mDxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));

        //创建WARP设备
        ThrowIfFailed(D3D12CreateDevice(
            adapter.Get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&mDevice)));
    }
}

//创建围栏，并查询三种描述符的大小（大小因显卡而异，只能查，不能写死）
void D3DApp::CreateFenceAndDescriptorSizes()
{
    ThrowIfFailed(mDevice->CreateFence(
        0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&mFence)));

    mRTVDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    mDSVDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    mCSUDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

//检测对 4X MSAA 质量级别的支持
void D3DApp::Check4xMsaaQuality()
{
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels;
    msQualityLevels.Format = mBackBufferFormat;
    msQualityLevels.SampleCount = 4;
    msQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
    msQualityLevels.NumQualityLevels = 0;

    ThrowIfFailed(mDevice->CheckFeatureSupport(
        D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
        &msQualityLevels,
        sizeof(msQualityLevels)));

    m4xMsaaQuality = msQualityLevels.NumQualityLevels;
    assert(m4xMsaaQuality > 0 && "Unexpected MSAA quality level");
}

//创建命令队列、命令分配器和命令列表
void D3DApp::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateCommandQueue(
        &desc,IID_PPV_ARGS(&mCommandQueue)));
    ThrowIfFailed(mDevice->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(mCommandAllocator.GetAddressOf())));
    ThrowIfFailed(mDevice->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        mCommandAllocator.Get(),
        nullptr, //初始化流水线状态对象
        IID_PPV_ARGS(mCommandList.GetAddressOf())));
    ThrowIfFailed(mCommandList->Close());
}

void D3DApp::CreateSwapChain()
{
    mSwapChain.Reset();

    DXGI_SWAP_CHAIN_DESC1 desc = {
        .Width = 0,
        .Height = 0,
        .Format = mBackBufferFormat,
        .Stereo = false,
        .SampleDesc = {
            .Count = 1,
            .Quality = 0
        },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = SwapChainBufferCount,
        .Scaling = DXGI_SCALING_STRETCH,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
        .Flags = 0
    };
    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(mDxgiFactory->CreateSwapChainForHwnd(
        mCommandQueue.Get(),
        mhMainWnd,&desc,
        nullptr,
        nullptr,
        &swapChain1));

    ThrowIfFailed(swapChain1.As(&mSwapChain));

    DXGI_SWAP_CHAIN_DESC1 actual = {};
    ThrowIfFailed(mSwapChain->GetDesc1(&actual));
    mClientWidth = actual.Width;
    mClientHeight = actual.Height;

    ThrowIfFailed(mDxgiFactory->MakeWindowAssociation(mhMainWnd, DXGI_MWA_NO_ALT_ENTER));
}

void D3DApp::CreateDescHeaps()
{
    /*
     * 渲染缓冲区视图
     */
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = SwapChainBufferCount,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        .NodeMask = 0
    };
    ThrowIfFailed(mDevice->CreateDescriptorHeap(
        &rtvHeapDesc,IID_PPV_ARGS(mRTVDescriptorHeap.GetAddressOf())));
    /*
     * 深度/模板视图
     */
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
        .NumDescriptors = 1,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        .NodeMask = 0
    };
    ThrowIfFailed(mDevice->CreateDescriptorHeap(
        &dsvHeapDesc,IID_PPV_ARGS(mDSVDescriptorHeap.GetAddressOf())));
}

D3D12_CPU_DESCRIPTOR_HANDLE D3DApp::CurrentBackBufferView() const
{
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(
        mRTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
        mSwapChain->GetCurrentBackBufferIndex(),
        mRTVDescriptorSize);
}

ID3D12Resource* D3DApp::CurrentBackBuffer() const
{
    return mSwapChainBuffer[mSwapChain->GetCurrentBackBufferIndex()].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE D3DApp::DepthStencilView() const
{
    return mDSVDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
}

void D3DApp::FlushCommandQueue()
{
    mCurrentFence++;
    ThrowIfFailed(mCommandQueue->Signal(mFence.Get(), mCurrentFence));
    if (mFence->GetCompletedValue() < mCurrentFence)
    {
        HANDLE eventHandle = CreateEventEx(nullptr, nullptr, 0,EVENT_ALL_ACCESS);

        // 预约：围栏值到了 mCurrentFence 时，把这个事件设为"有信号"
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrentFence,eventHandle));
        // 线程在这里睡下，直到事件有信号。睡着的时候不占用CPU
        WaitForSingleObject(eventHandle,INFINITE);
        CloseHandle(eventHandle);
    }
}

void D3DApp::CalculateFrameStats()
{
    static int frameCnt = 0; //这一秒内花了几帧
    static float timeElapsed = 0.0f; //上一次更新标题时的时间点

    frameCnt++;
    if (mTimer.TotalTime() - timeElapsed >= 1.0f)
    {
        float fps = (float)frameCnt; //一秒内画了几帧，就是帧率
        float mspf = 1000.0f / fps; //平均每帧用了多少毫秒

        std::wstring windowText = std::format(L"{}    fps: {:.0f}    mspf: {:.3f}    {}",
                                              mMainWndCaption, fps, mspf, mAdapterName);
        SetWindowText(mhMainWnd, windowText.c_str());
        // 为下一秒做准备
        frameCnt = 0;
        timeElapsed += 1.0f;
    }
}

float D3DApp::AspectRatio() const
{
    return static_cast<float>(mClientWidth)/mClientHeight;
}

void D3DApp::CreateSwapChainRTV()
{
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle(mRTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT i = 0; i < SwapChainBufferCount; i++)
    {
        //获取交换链内的第i个缓冲区
        ThrowIfFailed(mSwapChain->GetBuffer(i,IID_PPV_ARGS(&mSwapChainBuffer[i])));

        //创建RTV
        mDevice->CreateRenderTargetView(mSwapChainBuffer[i].Get(), nullptr, rtvHeapHandle);

        //偏移到描述符堆中的下一个缓冲区
        rtvHeapHandle.Offset(1, mRTVDescriptorSize);
    }
}

void D3DApp::CreateDepthAndStencil()
{
    //创建深度/模板缓冲区及其视图
    D3D12_RESOURCE_DESC depthStencilDesc{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = mClientWidth,
        .Height = mClientHeight,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = mDepthStencilFormat,
        .SampleDesc = {
            .Count = 1,
            .Quality = 0
        },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
    };
    D3D12_CLEAR_VALUE optClear{
        .Format = mDepthStencilFormat,
        .DepthStencil = {
            .Depth = 1.0f,
            .Stencil = 0
        }
    };
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(mDevice->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &depthStencilDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &optClear,
        IID_PPV_ARGS(mDepthStencilBuffer.GetAddressOf())));

    //利用此资源的格式，为整个资源的第0mip层创建描述符
    mDevice->CreateDepthStencilView(
        mDepthStencilBuffer.Get(),
        nullptr,
        DepthStencilView());
}

// 初始化时：只算值，不调 RSSet
void D3DApp::UpdateViewportAndScissor()
{
    mScreenViewport = {0.0f, 0.0f, (float)mClientWidth, (float)mClientHeight, 0.0f, 1.0f};
    mScissorRect = {0, 0, (LONG)mClientWidth, (LONG)mClientHeight};
}
