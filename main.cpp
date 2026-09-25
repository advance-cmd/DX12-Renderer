#include "Common/D3DApp.h"

#include <vector>
#include <array>
#include <cstdint>
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

//顶点结构体
struct Vertex
{
    XMFLOAT3 Pos; //12byte
    XMFLOAT4 Color; //16byte
};

// 第 4 章的 Demo：继承框架，初始化全由 D3DApp 完成，这里只写每帧要做的事
class InitDirect3DApp : public D3DApp
{
public:
    InitDirect3DApp(HINSTANCE hInstance) : D3DApp(hInstance)
    {
    }

    bool Initialize() override
    {
        if (!D3DApp::Initialize())
        {
            return false;
        }

        // 框架初始化结束时命令列表是关着的，要录制"复制顶点数据"的命令，先把它打开
        ThrowIfFailed(mCommandList->Reset(mCommandAllocator.Get(), nullptr));

        BuildRootSignature();
        BuildShadersAndInputLayout();
        BuildBoxGeometry(); // 这里会往命令列表里录制复制命令
        BuildPSO(); // 要用到上面三个的结果，所以放在最后

        // 提交命令，等 GPU 把复制做完
        ThrowIfFailed(mCommandList->Close());
        ID3D12CommandList* cmdLists[] = {mCommandList.Get()};
        mCommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);
        FlushCommandQueue();

        // 复制已经完成，中转站可以释放了
        mVertexBufferUploader.Reset();
        mIndexBufferUploader.Reset();
        return true;
    }

private:
    void Update(const GameTimer& gt) override
    {
    }

    void Draw(const GameTimer& gt) override
    {
        //准备录制
        ThrowIfFailed(mCommandAllocator->Reset());
        ThrowIfFailed(mCommandList->Reset(mCommandAllocator.Get(),mPSO.Get())); //相当于 Reset 之后立刻设置好这条流水线。
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

        mCommandList->SetGraphicsRootSignature(mRootSignature.Get()); // 着色器的"参数列表"
        mCommandList->IASetVertexBuffers(0, 1, &mVBV); // 顶点从哪读（输入槽 0）
        mCommandList->IASetIndexBuffer(&mIBV); //索引
        mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); // 每 3 个顶点组成一个三角形
        mCommandList->DrawIndexedInstanced(36, 1, 0, 0, 0);

        //屏障：Render_Target -> Prsent
        auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
            CurrentBackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        mCommandList->ResourceBarrier(1, &toPresent);
        //结束录制，提交
        ThrowIfFailed(mCommandList->Close());
        ID3D12CommandList* cmdLists[] = {mCommandList.Get()};
        mCommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);
        //显示，然后等GPU
        ThrowIfFailed(mSwapChain->Present(0,0));
        FlushCommandQueue();
    }

    ComPtr<ID3D12Resource> mVertexBufferGPU; //默认堆里的顶点缓冲区（真正用来画的）
    ComPtr<ID3D12Resource> mIndexBufferGPU; //索引缓冲
    ComPtr<ID3D12Resource> mVertexBufferUploader; //上传缓冲区
    ComPtr<ID3D12Resource> mIndexBufferUploader; //索引上传缓冲区
    D3D12_VERTEX_BUFFER_VIEW mVBV = {}; //顶点缓冲区视图
    D3D12_INDEX_BUFFER_VIEW mIBV = {}; //索引缓冲区视图
    ComPtr<ID3D12RootSignature> mRootSignature; //根签名
    ComPtr<ID3DBlob> mVSByteCode; // 顶点着色器字节码
    ComPtr<ID3DBlob> mPSByteCode; // 像素着色器字节码
    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout; // 输入布局
    ComPtr<ID3D12PipelineState> mPSO;

    void BuildBoxGeometry()
    {
        std::array<Vertex, 8> vertices = {
            Vertex({XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT4(Colors::White)}),
            Vertex({XMFLOAT3(-1.0f, 1.0f, -1.0f), XMFLOAT4(Colors::Black)}),
            Vertex({XMFLOAT3(1.0f, 1.0f, -1.0f), XMFLOAT4(Colors::Red)}),
            Vertex({XMFLOAT3(1.0f, -1.0f, -1.0f), XMFLOAT4(Colors::Green)}),
            Vertex({XMFLOAT3(-1.0f, -1.0f, 1.0f), XMFLOAT4(Colors::Blue)}),
            Vertex({XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT4(Colors::Yellow)}),
            Vertex({XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT4(Colors::Cyan)}),
            Vertex({XMFLOAT3(1.0f, -1.0f, 1.0f), XMFLOAT4(Colors::Magenta)}),
        };
        std::array<std::uint16_t, 36> indices = {
            //前表面
            0, 1, 2,
            0, 2, 3,
            //后表面
            4, 6, 5,
            4, 7, 6,
            //左表面
            4, 5, 1,
            4, 1, 0,
            //右表面
            3, 2, 6,
            3, 6, 7,
            //上表面
            1, 5, 6,
            1, 6, 2,
            //下表面
            4, 0, 3,
            4, 3, 7
        };
        const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
        mVertexBufferGPU = CreateDefaultBuffer(mDevice.Get(), mCommandList.Get(),
                                               vertices.data(), vbByteSize, mVertexBufferUploader);
        mVBV.BufferLocation = mVertexBufferGPU->GetGPUVirtualAddress();
        mVBV.SizeInBytes = vbByteSize;
        mVBV.StrideInBytes = sizeof(Vertex);
        const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);   //它和数组的元素类型、mIBV.Format = DXGI_FORMAT_R16_UINT 三者一致
        mIndexBufferGPU = CreateDefaultBuffer(mDevice.Get(), mCommandList.Get(),
                                              indices.data(), ibByteSize, mIndexBufferUploader);
        mIBV.BufferLocation = mIndexBufferGPU->GetGPUVirtualAddress();
        mIBV.SizeInBytes = ibByteSize;
        mIBV.Format = DXGI_FORMAT_R16_UINT;
    }

    void BuildRootSignature()
    {
        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
            0,
            nullptr,
            0,
            nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        //第一步：把描述“序列化”成一段二进制数据
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> error;

        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
        if (error != nullptr)
        {
            OutputDebugStringA((char*)error->GetBufferPointer());
        }
        ThrowIfFailed(hr);

        //第二步：用这段二进制数据创建根签名对象
        ThrowIfFailed(
            mDevice->CreateRootSignature(
                0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&mRootSignature)));
    }

    void BuildShadersAndInputLayout()
    {
        mVSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", nullptr, "VS", "vs_5_0");
        mPSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", nullptr, "PS", "ps_5_0");

        mInputLayout = {
            {
                "POSITION",
                0,
                DXGI_FORMAT_R32G32B32_FLOAT,
                0,
                0,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                0
            },
            {
                "COLOR",
                0,
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                0,
                12,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                0
            },
        };
    }

    void BuildPSO()
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        // 【输入】顶点长什么样，着色器能用哪些资源
        psoDesc.InputLayout = {mInputLayout.data(), (UINT)mInputLayout.size()};
        psoDesc.pRootSignature = mRootSignature.Get();

        // 【可编程阶段】顶点着色器、像素着色器
        psoDesc.VS = {mVSByteCode->GetBufferPointer(), mVSByteCode->GetBufferSize()};
        psoDesc.PS = {mPSByteCode->GetBufferPointer(), mPSByteCode->GetBufferSize()};

        // 【固定功能阶段】光栅化、混合、深度模板，全部用默认值
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.SampleMask = UINT_MAX;

        // 【图元类型】
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

        // 【输出】画到什么格式的缓冲区上，必须和第 4 章建的缓冲区一致
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = mBackBufferFormat;
        psoDesc.DSVFormat = mDepthStencilFormat;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.SampleDesc.Quality = 0;

        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSO)));
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
