#include "Common/D3DApp.h"

#include <algorithm>
#include <vector>
#include <array>
#include <cstdint>
#include <memory>
#include <DirectXMath.h>
#include <d3dcompiler.h>    // D3DCompileFromFile：运行时编译 HLSL
#include <DirectXColors.h>  // Colors::LightSteelBlue 之类的预定义颜色

#include "Common/UploadBuffer.h"
#include "Common/DDSTextureLoader.h"
#include "FrameResource.h"

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
}

extern "C" {
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

using namespace DirectX;

//顶点结构体
struct Vertex
{
    XMFLOAT3 Pos; //12byte
    XMFLOAT2 TexC; //8byte
};



// 带纹理的木箱：鼠标拖动绕着它转（左键旋转，右键缩放）
// 窗口、设备、交换链由 D3DApp 负责，这里只管木箱自己的资源、每帧更新和绘制
class CrateApp : public D3DApp
{
public:
    CrateApp(HINSTANCE hInstance) : D3DApp(hInstance)
    {
    }
    
    ~CrateApp() override
    {
        if (mCommandQueue != nullptr)
        {
            FlushCommandQueue();
        }
    }
    
    bool Initialize() override
    {
        if (!D3DApp::Initialize())
        {
            return false;
        }

        // 框架初始化结束时命令列表是关着的；上传纹理和顶点/索引数据要录制复制命令，先把它打开
        ThrowIfFailed(mCommandList->Reset(mCommandAllocator.Get(), nullptr));
        LoadTextures(); // 录制复制命令（纹理）
        BuildDescriptorHeaps(); //先有堆
        BuildFrameResources();
        BuildShaderResourceViews();
        BuildRootSignature();
        BuildShadersAndInputLayout();
        BuildBoxGeometry(); // 录制复制命令（顶点/索引）
        BuildPSO(); // 要用到上面三个的结果，所以放在最后

        // 提交命令，等 GPU 把复制做完
        ThrowIfFailed(mCommandList->Close());
        ID3D12CommandList* cmdLists[] = {mCommandList.Get()};
        mCommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);
        FlushCommandQueue();

        // 复制已经完成，中转站可以释放了
        mBoxGeo->DisposeUploaders();
        for (auto & it :mTextures)
        {
            it.second->UploadHeap.Reset();
        }
        return true;
    }

private:
    void Update(const GameTimer& gt) override
    {
        mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
        mCurrentFrame = mFrameResources[mCurrFrameResourceIndex].get();
        
        if (mCurrentFrame->Fence != 0 && mFence->GetCompletedValue() < mCurrentFrame->Fence)
        {
            HANDLE eventHandle = CreateEventEx(nullptr, nullptr, 0,EVENT_ALL_ACCESS);
            if (eventHandle == nullptr)
            {
                ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
            }
            ThrowIfFailed(mFence->SetEventOnCompletion(mCurrentFrame->Fence,eventHandle));
            WaitForSingleObject(eventHandle, INFINITE);
            CloseHandle(eventHandle);
        }
        
        ObjectConstants oc;
        // 球坐标转成 xyz（6.11 笔记里有这个公式）
        float x = mRadius * sinf(mPhi) * cosf(mTheta);
        float z = mRadius * sinf(mPhi) * sinf(mTheta);
        float y = mRadius * cosf(mPhi);

        XMStoreFloat4x4(&mView, XMMatrixLookAtLH(XMVectorSet(x, y, z, 1.0f), XMVectorZero(), XMVectorSet(0, 1, 0, 0)));
        XMStoreFloat4x4(&oc.MVP, XMMatrixTranspose(
                            XMLoadFloat4x4(&mWorld) *
                            XMLoadFloat4x4(&mView) *
                            XMLoadFloat4x4(&mProj))); //把矩阵转置后存进 oc.MVP
        mCurrentFrame->ObjectCB->CopyData(0, oc);
    }

    void Draw(const GameTimer& gt) override
    {
        //准备录制
        ThrowIfFailed(mCurrentFrame->CmdListAlloc->Reset());
        ThrowIfFailed(
            mCommandList->Reset(mCurrentFrame->CmdListAlloc.Get(), mPSO.Get()));
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

        ID3D12DescriptorHeap* heaps[] = {mHeap.Get()};
        mCommandList->SetDescriptorHeaps(_countof(heaps), heaps); //先设置描述符堆
        mCommandList->SetGraphicsRootSignature(mRootSignature.Get()); // 着色器的"参数列表"
        mCommandList->SetGraphicsRootConstantBufferView(0, mCurrentFrame->ObjectCB->Resource()->GetGPUVirtualAddress());

        CD3DX12_GPU_DESCRIPTOR_HANDLE TexGPUHandle(mHeap->GetGPUDescriptorHandleForHeapStart());
        mCommandList->SetGraphicsRootDescriptorTable(1, TexGPUHandle); //设置描述符表srv (Tex/Sample)

        // 视图是函数返回的临时对象，先存成变量才能取地址（直接 &mBoxGeo->VertexBufferView() 会报 C2102）
        D3D12_VERTEX_BUFFER_VIEW vbv = mBoxGeo->VertexBufferView();
        D3D12_INDEX_BUFFER_VIEW ibv = mBoxGeo->IndexBufferView();
        mCommandList->IASetVertexBuffers(0, 1, &vbv); // 顶点从哪读（输入槽 0）
        mCommandList->IASetIndexBuffer(&ibv); //索引
        mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); // 每 3 个顶点组成一个三角形

        // 按名字取出立方体在缓冲区里的那一段
        const DX_Utils::SubmeshGeometry& box = mBoxGeo->DrawArgs["box"];
        mCommandList->DrawIndexedInstanced(box.IndexCount, 1, box.StartIndexLocation, box.BaseVertexLocation, 0);

        //屏障：Render_Target -> Present
        auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
            CurrentBackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        mCommandList->ResourceBarrier(1, &toPresent);
        //结束录制，提交
        ThrowIfFailed(mCommandList->Close());
        ID3D12CommandList* cmdLists[] = {mCommandList.Get()};
        mCommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);
        //显示；给这份帧资源记下 Fence 值，GPU 执行到这里时 Fence 会变成这个值
        ThrowIfFailed(mSwapChain->Present(0,0));
        ++mCurrentFence;
        mCurrentFrame->Fence = mCurrentFence;

        ThrowIfFailed(
            mCommandQueue->Signal(mFence.Get(), mCurrentFence));
    }

    std::unique_ptr<DX_Utils::MeshGeometry> mBoxGeo; //立方体的顶点/索引缓冲、视图参数和 DrawArgs 都在这里面
    ComPtr<ID3D12RootSignature> mRootSignature; //根签名
    ComPtr<ID3DBlob> mVSByteCode; // 顶点着色器字节码
    ComPtr<ID3DBlob> mPSByteCode; // 像素着色器字节码
    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout; // 输入布局
    ComPtr<ID3D12PipelineState> mPSO;

    ComPtr<ID3D12DescriptorHeap> mHeap;

    //M
    XMFLOAT4X4 mWorld = XMFLOAT4X4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    //V
    XMFLOAT4X4 mView = XMFLOAT4X4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    //P
    XMFLOAT4X4 mProj = XMFLOAT4X4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    float mTheta = 1.5f * XM_PI; // 绕 y 轴转的角度
    float mPhi = XM_PIDIV4; // 和 +y 轴的夹角
    float mRadius = 5.0f; // 离原点多远

    POINT mLastMousePos = {};

    std::unordered_map<std::string, std::unique_ptr<DX_Utils::Texture>> mTextures;
    
    static constexpr int gNumFrameResources = 3;    //帧资源的份数
    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrentFrame = nullptr; //当前这一帧用的那一份
    int mCurrFrameResourceIndex = 0;
    
    void BuildFrameResources()
    {
        for (int i = 0; i < gNumFrameResources; i++)
        {
            mFrameResources.push_back(std::make_unique<FrameResource>(mDevice.Get(),1));    //第二个参数是 1，因为现在只有一个木箱，每份帧资源只需要 1 个常量。
        }
    }

    void LoadTextures()
    {
        auto Tex = std::make_unique<DX_Utils::Texture>();
        Tex->Name = "woodCrateTex";
        Tex->Filename = L"Textures\\WoodCrate01.dds";
        ThrowIfFailed(CreateDDSTextureFromFile12(mDevice.Get(), mCommandList.Get(), Tex->Filename.c_str(), Tex->Resource,
                                   Tex->UploadHeap));
        mTextures[Tex->Name] = std::move(Tex);
    }

    //建堆
    void BuildDescriptorHeaps()
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {
            .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            .NumDescriptors = 1,
            .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, //着色器要从这个堆里读。RTV/DSV 堆不需要着色器访问，所以之前写的是 NONE
            .NodeMask = 0,
        };
        ThrowIfFailed(mDevice->CreateDescriptorHeap(&HeapDesc,IID_PPV_ARGS(mHeap.GetAddressOf())));
    }

    void BuildShaderResourceViews()
    {
        auto& woodTex = mTextures.at("woodCrateTex")->Resource;
        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {
            .Format = woodTex->GetDesc().Format,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = {
                .MostDetailedMip = 0,
                .MipLevels = woodTex->GetDesc().MipLevels,
                .PlaneSlice = 0,
                .ResourceMinLODClamp = 0.0f,
            }
        };
        CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mHeap->GetCPUDescriptorHandleForHeapStart());
        mDevice->CreateShaderResourceView(woodTex.Get(), &SRVDesc, hDescriptor);
    }

    void BuildBoxGeometry()
    {
        // 24 个顶点：每个面 4 个，各自带 uv（一个角在 3 个面上的 uv 不同，所以不能共用）
        // 数据来自书上 GeometryGenerator::CreateBox，w2 = h2 = d2 = 1；uv 的 v 轴朝下，左上角是 (0, 0)
        std::array<Vertex, 24> vertices = {
            //前表面（z = -1）
            Vertex({XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT2(0.0f, 1.0f)}), // 0 左下
            Vertex({XMFLOAT3(-1.0f, +1.0f, -1.0f), XMFLOAT2(0.0f, 0.0f)}), // 1 左上
            Vertex({XMFLOAT3(+1.0f, +1.0f, -1.0f), XMFLOAT2(1.0f, 0.0f)}), // 2 右上
            Vertex({XMFLOAT3(+1.0f, -1.0f, -1.0f), XMFLOAT2(1.0f, 1.0f)}), // 3 右下
            //后表面（z = +1）
            Vertex({XMFLOAT3(-1.0f, -1.0f, +1.0f), XMFLOAT2(1.0f, 1.0f)}),
            Vertex({XMFLOAT3(+1.0f, -1.0f, +1.0f), XMFLOAT2(0.0f, 1.0f)}),
            Vertex({XMFLOAT3(+1.0f, +1.0f, +1.0f), XMFLOAT2(0.0f, 0.0f)}),
            Vertex({XMFLOAT3(-1.0f, +1.0f, +1.0f), XMFLOAT2(1.0f, 0.0f)}),
            //上表面（y = +1）
            Vertex({XMFLOAT3(-1.0f, +1.0f, -1.0f), XMFLOAT2(0.0f, 1.0f)}),
            Vertex({XMFLOAT3(-1.0f, +1.0f, +1.0f), XMFLOAT2(0.0f, 0.0f)}),
            Vertex({XMFLOAT3(+1.0f, +1.0f, +1.0f), XMFLOAT2(1.0f, 0.0f)}),
            Vertex({XMFLOAT3(+1.0f, +1.0f, -1.0f), XMFLOAT2(1.0f, 1.0f)}),
            //下表面（y = -1）
            Vertex({XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT2(1.0f, 1.0f)}),
            Vertex({XMFLOAT3(+1.0f, -1.0f, -1.0f), XMFLOAT2(0.0f, 1.0f)}),
            Vertex({XMFLOAT3(+1.0f, -1.0f, +1.0f), XMFLOAT2(0.0f, 0.0f)}),
            Vertex({XMFLOAT3(-1.0f, -1.0f, +1.0f), XMFLOAT2(1.0f, 0.0f)}),
            //左表面（x = -1）
            Vertex({XMFLOAT3(-1.0f, -1.0f, +1.0f), XMFLOAT2(0.0f, 1.0f)}),
            Vertex({XMFLOAT3(-1.0f, +1.0f, +1.0f), XMFLOAT2(0.0f, 0.0f)}),
            Vertex({XMFLOAT3(-1.0f, +1.0f, -1.0f), XMFLOAT2(1.0f, 0.0f)}),
            Vertex({XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT2(1.0f, 1.0f)}),
            //右表面（x = +1）
            Vertex({XMFLOAT3(+1.0f, -1.0f, -1.0f), XMFLOAT2(0.0f, 1.0f)}),
            Vertex({XMFLOAT3(+1.0f, +1.0f, -1.0f), XMFLOAT2(0.0f, 0.0f)}),
            Vertex({XMFLOAT3(+1.0f, +1.0f, +1.0f), XMFLOAT2(1.0f, 0.0f)}),
            Vertex({XMFLOAT3(+1.0f, -1.0f, +1.0f), XMFLOAT2(1.0f, 1.0f)}),
        };
        // 每个面的 4 个顶点从外面看都是"左下、左上、右上、右下"，所以索引规律都一样：
        // 4f, 4f+1, 4f+2,  4f, 4f+2, 4f+3（两个三角形都是顺时针）
        std::array<std::uint16_t, 36> indices = {
            0, 1, 2, 0, 2, 3, //前表面
            4, 5, 6, 4, 6, 7, //后表面
            8, 9, 10, 8, 10, 11, //上表面
            12, 13, 14, 12, 14, 15, //下表面
            16, 17, 18, 16, 18, 19, //左表面
            20, 21, 22, 20, 22, 23 //右表面
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
        CD3DX12_ROOT_PARAMETER slotRootParameter[2];
        slotRootParameter[0].InitAsConstantBufferView(0);
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        slotRootParameter[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC sampler_desc(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
            2,
            slotRootParameter,
            1,
            &sampler_desc,
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
                "TEXCOORD",
                0,
                DXGI_FORMAT_R32G32_FLOAT,
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

    void OnMouseDown(WPARAM btnState, int x, int y) override
    {
        mLastMousePos.x = x;
        mLastMousePos.y = y;
        SetCapture(mhMainWnd); //按着键拖出窗口以后，鼠标消息仍然发给这个窗口
    }

    void OnMouseUp(WPARAM btnState, int x, int y) override
    {
        ReleaseCapture(); //把鼠标放开
    }

    void OnMouseMove(WPARAM btnState, int x, int y) override
    {
        if ((btnState & MK_LBUTTON) != 0) // 左键：旋转
        {
            float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x)); // 每像素 0.25°
            float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));
            mTheta -= dx;
            mPhi -= dy;
            mPhi = std::clamp(mPhi, 0.1f, XM_PI - 0.1f);
        }
        else if ((btnState & MK_RBUTTON) != 0) // 右键：缩放
        {
            float dx = 0.005f * static_cast<float>(x - mLastMousePos.x); // 每像素 0.005 个单位
            float dy = 0.005f * static_cast<float>(y - mLastMousePos.y);
            mRadius -= dx - dy;
            mRadius = std::clamp(mRadius, 3.0f, 15.0f);
        }
        mLastMousePos.x = x; // 这两行必须放在最后
        mLastMousePos.y = y;
    }
};

//在 Windows 应用程序中WinMain函数就相当于大多数语言中的main()函数
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR pCmdLine, int nShowCmd)
{
    // ThrowIfFailed 失败时会抛 DxException，在这里统一接住，弹窗显示是哪句代码、第几行出的错
    try
    {
        CrateApp theApp(hInstance);
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
