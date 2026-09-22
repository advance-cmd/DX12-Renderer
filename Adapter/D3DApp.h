#pragma once
#include <windows.h>
#include <wrl.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <string>

class D3DApp
{
protected:
    D3DApp(HINSTANCE hInstance);
    D3DApp(const D3DApp&) = delete;              // COM 对象不该被随意拷贝
    D3DApp& operator=(const D3DApp&) = delete;
    virtual ~D3DApp();
    
public:
    static D3DApp* GetApp();                     // 给 WndProc 转发用
    bool Initialize();                           // 建窗口 + 初始化 D3D
    int  Run();                                  // PeekMessage 消息循环
    virtual LRESULT MsgProc(HWND, UINT, WPARAM, LPARAM);

protected:
    // 子类重写这三个就能做出不同的 Demo
    virtual void OnResize();
    virtual void Update(float dt) = 0;
    virtual void Draw() = 0;

    bool InitMainWindow();
    bool InitDirect3D();
    void CreateCommandObjects();                 // 队列 → 分配器 → 列表
    void CreateSwapChain();
    void CreateRtvAndDsvDescriptorHeaps();
    void FlushCommandQueue();                    // Fence 同步

    ID3D12Resource*             CurrentBackBuffer() const;
    D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView() const;
    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView() const;
    float AspectRatio() const;
    
protected:
    static D3DApp* mApp;                         // 单例指针，WndProc 靠它找到对象

    HINSTANCE mhAppInst  = nullptr;
    HWND      mhMainWnd  = nullptr;
    bool      mAppPaused = false, mMinimized = false, mMaximized = false, mResizing = false;

    Microsoft::WRL::ComPtr<IDXGIFactory4>   mdxgiFactory;
    Microsoft::WRL::ComPtr<IDXGISwapChain>  mSwapChain;
    Microsoft::WRL::ComPtr<ID3D12Device>    md3dDevice;

    Microsoft::WRL::ComPtr<ID3D12Fence>     mFence;
    UINT64 mCurrentFence = 0;

    Microsoft::WRL::ComPtr<ID3D12CommandQueue>        mCommandQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    mDirectCmdListAlloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> mCommandList;

    static const int SwapChainBufferCount = 2;
    int mCurrBackBuffer = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> mSwapChainBuffer[SwapChainBufferCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> mDepthStencilBuffer;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDsvHeap;

    D3D12_VIEWPORT mScreenViewport{};
    D3D12_RECT     mScissorRect{};

    UINT mRtvDescriptorSize       = 0;           // 描述符大小因显卡而异，必须查
    UINT mDsvDescriptorSize       = 0;
    UINT mCbvSrvUavDescriptorSize = 0;

    DXGI_FORMAT mBackBufferFormat   = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    int mClientWidth  = 1280;
    int mClientHeight = 720;
    std::wstring mMainWndCaption = L"LearnDX12";
};
