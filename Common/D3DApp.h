#pragma once
// 仿照书中 d3dApp.h 的框架：
//   原来 main.h 里的全局变量  → 成员变量（g 前缀改成 m）
//   原来 main.cpp 里的初始化函数 → 成员函数
//   每个 Demo 继承 D3DApp，只需要实现 Update / Draw

#include <windows.h>
#include <wrl/client.h>
#include <string>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dx12/d3dx12.h>

#include "DxUtil.h"     // ThrowIfFailed
#include "GameTimer.h"

using Microsoft::WRL::ComPtr;

class D3DApp
{
protected:
    D3DApp(HINSTANCE hInstance);
    D3DApp(const D3DApp&) = delete;
    D3DApp& operator=(const D3DApp&) = delete;
    virtual ~D3DApp();

public:
    // 窗口过程是普通函数，拿不到 this，只能通过这个静态指针找到当前的 App
    static D3DApp* GetApp();

    // 创建窗口 + 初始化 Direct3D
    virtual bool Initialize();

    //封装消息循环代码
    int Run();

    // 窗口过程会处理窗口所接收到的消息
    virtual LRESULT MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

protected:
    // 和窗口尺寸有关的资源都在这里创建
    virtual void OnResize();

    // 纯虚函数：每个 Demo 自己决定每帧更新什么、画什么
    virtual void Update(const GameTimer& gt) = 0;
    virtual void Draw(const GameTimer& gt) = 0;
    
    //鼠标移动
    virtual void OnMouseDown(WPARAM btnState,int x,int y){}
    virtual void OnMouseUp(WPARAM btnState,int x,int y){}
    virtual void OnMouseMove(WPARAM btnState,int x,int y){}

protected:
    //封装初始化Windows应用程序所需的代码
    bool InitMainWindow();
    void InitDirect3D();

    // Direct3D 初始化的各个步骤，调用顺序见 InitDirect3D / OnResize
    void EnableD3DDebugLayer();
    void InitDxgiFactory();
    void CreateD3DDevice();
    void CreateFenceAndDescriptorSizes();
    void Check4xMsaaQuality();
    void CreateCommandObjects();
    void CreateSwapChain();
    void CreateDescHeaps();
    void CreateSwapChainRTV();
    void CreateDepthAndStencil();
    void UpdateViewportAndScissor();

    D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView() const;
    ID3D12Resource* CurrentBackBuffer() const;
    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView() const;
    
    void FlushCommandQueue();
    
    void CalculateFrameStats();
    float AspectRatio() const;

protected:
    inline static D3DApp* mApp = nullptr;

    HINSTANCE mhAppInst = nullptr;
    //用于指认所创建主窗口的句柄
    HWND mhMainWnd = nullptr;
    std::wstring mMainWndCaption = L"LearnDX12";   // 窗口标题的基础文字

    GameTimer mTimer;
    bool mAppPaused = false;    //暂停：失去焦点、最小化、拖动边框时
    bool mMinmized = false;     //当前是否处于最小化状态
    bool mResizing = false;     //是否正在拖动边框

    ComPtr<IDXGIFactory4> mDxgiFactory;
    ComPtr<ID3D12Device> mDevice;
    ComPtr<ID3D12Fence1> mFence;
    UINT64 mCurrentFence = 0;
    UINT mRTVDescriptorSize = 0;
    UINT mDSVDescriptorSize = 0;
    UINT mCSUDescriptorSize = 0;
    DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    UINT m4xMsaaQuality = 0;

    ComPtr<ID3D12CommandQueue> mCommandQueue;
    ComPtr<ID3D12CommandAllocator> mCommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> mCommandList;

    ComPtr<IDXGISwapChain3> mSwapChain;
    static constexpr UINT SwapChainBufferCount = 2;   // 双缓冲
    UINT mClientWidth = 0;
    UINT mClientHeight = 0;
    ComPtr<ID3D12Resource> mSwapChainBuffer[SwapChainBufferCount];

    //描述符堆
    ComPtr<ID3D12DescriptorHeap> mRTVDescriptorHeap;
    ComPtr<ID3D12DescriptorHeap> mDSVDescriptorHeap;

    //深度/模版缓冲资源
    ComPtr<ID3D12Resource> mDepthStencilBuffer;

    D3D12_VIEWPORT mScreenViewport = {};
    D3D12_RECT     mScissorRect = {};
    
};
