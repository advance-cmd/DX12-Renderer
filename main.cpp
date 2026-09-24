#include "Common/D3DApp.h"

#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <d3dcompiler.h>    // D3DCompileFromFile，第 6 章编译 HLSL 时才用
#include <DirectXColors.h>  // Colors::LightSteelBlue 之类的预定义颜色

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
}

extern "C" {
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

using namespace DirectX;
using namespace DirectX::PackedVector;

// 第 4 章的 Demo：继承框架，初始化全由 D3DApp 完成，这里只写每帧要做的事
class InitDirect3DApp : public D3DApp
{
public:
    InitDirect3DApp(HINSTANCE hInstance) : D3DApp(hInstance)
    {
    }

private:
    void Update(const GameTimer& gt) override
    {
    }

    void Draw(const GameTimer& gt) override
    {
        //准备录制
        ThrowIfFailed(mCommandAllocator->Reset());
        ThrowIfFailed(mCommandList->Reset(mCommandAllocator.Get(),nullptr));
        //屏障 Present -> Render_Target
        auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(
            CurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        mCommandList->ResourceBarrier(1, &toRenderTarget); //                   有几个屏障          屏障数组的首地址
        //视口和裁剪矩形
        mCommandList->RSSetViewports(1, &mScreenViewport);
        mCommandList->RSSetScissorRects(1, &mScissorRect);
        //清屏，绑定渲染目标
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = CurrentBackBufferView();
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = DepthStencilView();

        mCommandList->ClearRenderTargetView(rtv, Colors::LightSteelBlue, 0, nullptr);
        mCommandList->ClearDepthStencilView(
            dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0,
            nullptr);
        mCommandList->OMSetRenderTargets(1, &rtv, true, &dsv);
        //屏障：Render_Target -> Prsent
        auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
            CurrentBackBuffer(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
        mCommandList->ResourceBarrier(1, &toPresent);
        //结束录制，提交
        ThrowIfFailed(mCommandList->Close());
        ID3D12CommandList* cmdLists[]={mCommandList.Get()};
        mCommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);
        //显示，然后等GPU
        ThrowIfFailed(mSwapChain->Present(0,0));
        FlushCommandQueue();
        
    }
};

//在 Windows 应用程序中WinMain函数就相当于大多数语言中的main()函数
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR pCmdLine, int nShowCmd)
{
    // ThrowIfFailed 失败时会抛 DxException，在这里统一接住，弹窗显示是哪句代码、第几行出的错
    try
    {
        InitDirect3DApp theApp(hInstance);
        if (!theApp.Initialize())
        {
            return 0;
        }

        //初始化完毕后就可以开启消息循环了
        return theApp.Run();
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK | MB_ICONERROR);
        return 0;
    }
}
