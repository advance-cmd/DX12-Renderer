#include "Common/D3DApp.h"

#include <vector>
#include <array>
#include <cstdint>
#include <memory>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <d3dcompiler.h>    // D3DCompileFromFile，第 6 章编译 HLSL 时才用
#include <DirectXColors.h>  // Colors::LightSteelBlue 之类的预定义颜色

#include "Common/UploadBuffer.h"

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

//HLSL常量
struct ObjectConstants
{
    XMFLOAT4X4 MVP; //需要和HLSL的Cbuffer对应
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
        BuildDescriptorHeaps(); //先有堆
        BuildConstantBuffers(); //建UploadBuffer，并把CBV写进堆里
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
        mBoxGeo->DisposeUploaders();
        return true;
    }

private:
    void Update(const GameTimer& gt) override
    {
        // 球坐标转成 xyz（6.11 笔记里有这个公式）
        float x = mRadius * sinf(mPhi) * cosf(mTheta);
        float z = mRadius * sinf(mPhi) * sinf(mTheta);
        float y = mRadius * cosf(mPhi);

        XMStoreFloat4x4(&mView,XMMatrixLookAtLH(XMVectorSet(x, y, z, 1.0f), XMVectorZero(), XMVectorSet(0, 1, 0, 0)));
        XMStoreFloat4x4(&oc.MVP, XMMatrixTranspose(
            XMLoadFloat4x4(&mWorld)*
            XMLoadFloat4x4(&mView)*
            XMLoadFloat4x4(&mProj))); //把矩阵转置后存进 oc.MVP
        mObjectCB->CopyData(0, oc); //把整个 oc 复制进常量缓冲区的第 0 个元素
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

        ID3D12DescriptorHeap* heaps[] = {mCbvHeap.Get()};
        mCommandList->SetDescriptorHeaps(_countof(heaps), heaps); //先设置描述符堆
        mCommandList->SetGraphicsRootSignature(mRootSignature.Get()); // 着色器的"参数列表"
        mCommandList->SetGraphicsRootDescriptorTable(0, mCbvHeap->GetGPUDescriptorHandleForHeapStart()); //再设置描述符表
        // 视图是函数返回的临时对象，先存成变量才能取地址（直接 &mBoxGeo->VertexBufferView() 会报 C2102）
        D3D12_VERTEX_BUFFER_VIEW vbv = mBoxGeo->VertexBufferView();
        D3D12_INDEX_BUFFER_VIEW ibv = mBoxGeo->IndexBufferView();
        mCommandList->IASetVertexBuffers(0, 1, &vbv); // 顶点从哪读（输入槽 0）
        mCommandList->IASetIndexBuffer(&ibv); //索引
        mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); // 每 3 个顶点组成一个三角形

        // 按名字取出立方体在缓冲区里的那一段
        const DX_Utils::SubmeshGeometry& box = mBoxGeo->DrawArgs["box"];
        mCommandList->DrawIndexedInstanced(box.IndexCount, 1, box.StartIndexLocation, box.BaseVertexLocation, 0);

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

    std::unique_ptr<DX_Utils::MeshGeometry> mBoxGeo; //立方体的顶点/索引缓冲、视图参数和 DrawArgs 都在这里面
    ComPtr<ID3D12RootSignature> mRootSignature; //根签名
    ComPtr<ID3DBlob> mVSByteCode; // 顶点着色器字节码
    ComPtr<ID3DBlob> mPSByteCode; // 像素着色器字节码
    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout; // 输入布局
    ComPtr<ID3D12PipelineState> mPSO;

    ComPtr<ID3D12DescriptorHeap> mCbvHeap;
    std::unique_ptr<UploadBuffer<ObjectConstants>> mObjectCB;
    ObjectConstants oc;

    //M
    XMFLOAT4X4 mWorld = XMFLOAT4X4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    //V
    XMFLOAT4X4 mView = XMFLOAT4X4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    //P
    XMFLOAT4X4 mProj = XMFLOAT4X4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1); 
    float mTheta = 1.5f * XM_PI; // 绕 y 轴转的角度
    float mPhi = XM_PIDIV4; // 和 +y 轴的夹角
    float mRadius = 5.0f; // 离原点多远

    //建一个能放 CBV 的堆
    void BuildDescriptorHeaps()
    {
        D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc = {
            .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            .NumDescriptors = 1,
            .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, //着色器要从这个堆里读。RTV/DSV 堆不需要着色器访问，所以之前写的是 NONE
            .NodeMask = 0,
        };
        ThrowIfFailed(mDevice->CreateDescriptorHeap(&cbvHeapDesc,IID_PPV_ARGS(mCbvHeap.GetAddressOf())));
    }

    //创建缓冲区，在堆里放 CBV
    void BuildConstantBuffers()
    {
        mObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(mDevice.Get(), 1, true);
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvViewDesc = {
            .BufferLocation = mObjectCB->Resource()->GetGPUVirtualAddress(),
            .SizeInBytes = CalcConstantBufferByteSize(sizeof(ObjectConstants)),
        };
        mDevice->CreateConstantBufferView(&cbvViewDesc, mCbvHeap->GetCPUDescriptorHandleForHeapStart());
    }

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
        const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);
        //它和数组的元素类型、IndexFormat = DXGI_FORMAT_R16_UINT 三者一致

        mBoxGeo = std::make_unique<DX_Utils::MeshGeometry>();
        mBoxGeo->Name = "boxGeo";

        // 在内存里留一份副本（以后拾取、碰撞检测用）
        ThrowIfFailed(D3DCreateBlob(vbByteSize, &mBoxGeo->VertexBufferCPU));
        CopyMemory(mBoxGeo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);
        ThrowIfFailed(D3DCreateBlob(ibByteSize, &mBoxGeo->IndexBufferCPU));
        CopyMemory(mBoxGeo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

        // 上传到默认堆；上传缓冲区存在 mBoxGeo 里，Initialize 末尾 Flush 之后再 DisposeUploaders
        mBoxGeo->VertexBufferGPU = CreateDefaultBuffer(mDevice.Get(), mCommandList.Get(),
                                                       vertices.data(), vbByteSize, mBoxGeo->VertexBufferUploader);
        mBoxGeo->IndexBufferGPU = CreateDefaultBuffer(mDevice.Get(), mCommandList.Get(),
                                                      indices.data(), ibByteSize, mBoxGeo->IndexBufferUploader);

        // 视图用到的参数，VertexBufferView() / IndexBufferView() 每次调用时现算
        mBoxGeo->VertexByteStride = sizeof(Vertex);
        mBoxGeo->VertexBufferByteSize = vbByteSize;
        mBoxGeo->IndexFormat = DXGI_FORMAT_R16_UINT;
        mBoxGeo->IndexBufferByteSize = ibByteSize;

        // 整个缓冲区只有立方体一个物体，所以从第 0 个索引、第 0 个顶点开始
        DX_Utils::SubmeshGeometry submesh;
        submesh.IndexCount = (UINT)indices.size();
        submesh.StartIndexLocation = 0;
        submesh.BaseVertexLocation = 0;
        mBoxGeo->DrawArgs["box"] = submesh;
    }

    void BuildRootSignature()
    {
        CD3DX12_DESCRIPTOR_RANGE cbvTable;
        cbvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0); // 1 个 CBV，从 b0 开始
        CD3DX12_ROOT_PARAMETER slotRootParameter[1];
        slotRootParameter[0].InitAsDescriptorTable(1, &cbvTable);

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
            1,
            slotRootParameter,
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
        mVSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", nullptr, "VS", "vs_5_1");
        mPSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", nullptr, "PS", "ps_5_1");

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

protected:
    void OnResize() override
    {
        D3DApp::OnResize(); // 先让基类重建交换链缓冲、RTV、深度缓冲、视口
        XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f * XM_PI, AspectRatio(), 1.0f, 1000.0f);
        XMStoreFloat4x4(&mProj, P);
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
