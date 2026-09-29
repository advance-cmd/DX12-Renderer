// LearnDX12 · 第 7 章：帧资源 + 渲染项，画 10×10 个带纹理的木箱
//
// 分工：D3DApp 负责窗口、设备、交换链、深度缓冲和消息循环；
//       CrateApp 只管自己的资源（纹理、几何体、渲染项、帧资源、根签名、PSO）以及每帧的更新和绘制。

#include "Common/D3DApp.h"
#include "Common/UploadBuffer.h"
#include "Common/DDSTextureLoader.h"
#include "Common/GeometryGenerator.h"
#include "FrameResource.h"
#include "Waves.h"

#include <algorithm>        // std::clamp
#include <cstdint>
#include <memory>
#include <vector>
#include <random>
#include <DirectXMath.h>
#include <DirectXColors.h>  // Colors::LightSteelBlue 之类的预定义颜色
#include <d3dcompiler.h>    // D3DCompileFromFile：运行时编译 HLSL

// Agility SDK：告诉系统用 exe 旁边 D3D12\ 目录里的新版 D3D12 运行时
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

// 顶点格式，必须和 BuildShadersAndInputLayout 里的输入布局、HLSL 里的 VertexIn 一致
struct Vertex
{
    XMFLOAT3 Pos;
    XMFLOAT3 Normal;
    XMFLOAT2 TexC;
};

// 渲染项：画一个物体需要的全部信息
struct RenderItem
{
    XMFLOAT4X4 World = XMFLOAT4X4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1); // 局部 → 世界

    // 还有几份帧资源的 ObjectCB 没写入最新的 World。物体一动就要重新设成 gNumFrameResources
    int NumFramesDirty = gNumFrameResources;

    // 这个物体的常量在 ObjectCB 里是第几个。-1（转成 UINT 是一个极大的数）表示"还没分配"，忘了设会立刻出错
    UINT ObjCBIndex = -1;

    DX_Utils::MeshGeometry* Geo = nullptr; // 用哪一对顶点/索引缓冲（多个渲染项可以共用）
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // DrawIndexedInstanced 的参数：画缓冲区里的哪一段
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int BaseVertexLocation = 0;

    // 画这个物体用哪个 PSO。顶点格式和着色器不同，PSO 就不能共用，
    // 所以画之前要按物体切
    ComPtr<ID3D12PipelineState> PSO;

    Material* Mat = nullptr;
};

class CrateApp : public D3DApp
{
public:
    CrateApp(HINSTANCE hInstance) : D3DApp(hInstance)
    {
    }

    // 子类成员（帧资源、几何体、纹理）比基类析构函数先销毁，
    // 所以必须在这里先等 GPU 用完它们，不能只靠 ~D3DApp 里的 Flush
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
        BuildDescriptorHeaps();
        BuildShapeGeometry(); // 录制复制命令（木箱）
        BuildLandGeometry(); // 录制复制命令（陆地的顶点/索引）
        BuildWaves(); // 波浪模拟（这一步只建模拟本体，第 5 步再建水面几何）
        BuildShaderResourceViews(); // 要用到纹理和堆
        BuildRootSignature(); // PSO 要用根签名
        BuildShadersAndInputLayout(); // PSO 要用着色器和输入布局
        BuildPSO(); // 上面两样都有了才能建 PSO；渲染项要记住用哪个 PSO，所以再排在它前面
        BuildMaterials(); //FrameResources要用到这个
        BuildRenderItems(); // 要用到几何体 和 PSO
        BuildFrameResources(); // ObjectCB 按渲染项个数、WavesVB 按水面顶点数，所以排在最后

        // 提交复制命令，等 GPU 做完
        ThrowIfFailed(mCommandList->Close());
        ID3D12CommandList* cmdLists[] = {mCommandList.Get()};
        mCommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);
        FlushCommandQueue();

        // 复制已经完成，中转站（上传堆）可以释放了
        mShapeGeo->DisposeUploaders();
        mLandGeo->DisposeUploaders();
        mWaterGeo->DisposeUploaders();
        for (auto& [name, tex] : mTextures)
        {
            tex->UploadHeap.Reset();
        }
        return true;
    }

protected:
    // ======================== D3DApp 的虚函数 ========================

    void OnResize() override
    {
        D3DApp::OnResize(); // 先让基类重建交换链缓冲、RTV、深度缓冲、视口

        // 宽高比变了，投影矩阵要跟着重算
        XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f * XM_PI, AspectRatio(), 1.0f, 1000.0f);
        XMStoreFloat4x4(&mProj, P);
    }

    void Update(const GameTimer& gt) override
    {
        // 轮换到下一份帧资源；GPU 如果还在用它，就先等
        mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
        mCurrentFrame = mFrameResources[mCurrFrameResourceIndex].get();
        WaitForCurrentFrameResource();
        UpdateWaves(gt);

        UpdateMaterialCBs();
        UpdateObjectCBs();
        UpdateMainPassCB();
    }

    void Draw(const GameTimer& gt) override
    {
        // 用这份帧资源自己的分配器录制（Update 里已经确认 GPU 用完了它）
        ThrowIfFailed(mCurrentFrame->CmdListAlloc->Reset());
        ThrowIfFailed(mCommandList->Reset(mCurrentFrame->CmdListAlloc.Get(), nullptr));

        // 后台缓冲：PRESENT → RENDER_TARGET
        auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(
            CurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        mCommandList->ResourceBarrier(1, &toRenderTarget);

        mCommandList->RSSetViewports(1, &mScreenViewport);
        mCommandList->RSSetScissorRects(1, &mScissorRect);

        // 清屏，绑定渲染目标
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = CurrentBackBufferView();
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = DepthStencilView();
        mCommandList->ClearRenderTargetView(rtv, Colors::LightSteelBlue, 0, nullptr);
        mCommandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0,
                                            nullptr);
        mCommandList->OMSetRenderTargets(1, &rtv, true, &dsv);

        // 每帧只需设一次的：堆 → 根签名 → 参数 1（纹理表）、参数 2（Pass 常量）
        // 参数 1 是描述符表，必须先 SetDescriptorHeaps；参数 0（物体常量）在 DrawRenderItems 里每个物体设一次
        ID3D12DescriptorHeap* heaps[] = {mHeap.Get()};
        mCommandList->SetDescriptorHeaps(_countof(heaps), heaps);
        mCommandList->SetGraphicsRootSignature(mRootSignature.Get());
        mCommandList->SetGraphicsRootConstantBufferView(2, mCurrentFrame->PassCB->Resource()->GetGPUVirtualAddress());
        mCommandList->SetGraphicsRootDescriptorTable(1, mHeap->GetGPUDescriptorHandleForHeapStart()); // SRV 在第 0 格

        DrawRenderItems();

        // 后台缓冲：RENDER_TARGET → PRESENT
        auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
            CurrentBackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        mCommandList->ResourceBarrier(1, &toPresent);

        // 结束录制，提交
        ThrowIfFailed(mCommandList->Close());
        ID3D12CommandList* cmdLists[] = {mCommandList.Get()};
        mCommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);
        ThrowIfFailed(mSwapChain->Present(0, 0));

        // 不再每帧 Flush：给这份帧资源记下 Fence 值。GPU 执行到这条 Signal 时，Fence 才会变成这个值，
        // 3 帧后轮回到这份帧资源时，WaitForCurrentFrameResource 就靠它判断 GPU 用完没有
        mCurrentFrame->Fence = ++mCurrentFence;
        ThrowIfFailed(mCommandQueue->Signal(mFence.Get(), mCurrentFence));
    }

    // 鼠标：左键拖动旋转，右键拖动缩放。
    // 正负号故意和书上相反："抓着物体转"——物体跟着鼠标走，往上/往右拖是拉近
    void OnMouseDown(WPARAM btnState, int x, int y) override
    {
        mLastMousePos.x = x;
        mLastMousePos.y = y;
        SetCapture(mhMainWnd); // 按着键拖出窗口以后，鼠标消息仍然发给这个窗口
    }

    void OnMouseUp(WPARAM btnState, int x, int y) override
    {
        ReleaseCapture();
    }

    void OnMouseMove(WPARAM btnState, int x, int y) override
    {
        if ((btnState & MK_LBUTTON) != 0) // 左键：旋转，每像素 0.25°
        {
            float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
            float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));
            mTheta -= dx;
            mPhi -= dy;
            mPhi = std::clamp(mPhi, 0.1f, XM_PI - 0.1f); // 不能转到正上方/正下方，否则 LookAt 的 up 向量失效
        }
        else if ((btnState & MK_RBUTTON) != 0) // 右键：缩放，每像素 0.05 个单位
        {
            float dx = 0.05f * static_cast<float>(x - mLastMousePos.x);
            float dy = 0.05f * static_cast<float>(y - mLastMousePos.y);
            mRadius -= dx - dy;
            mRadius = std::clamp(mRadius, 3.0f, 80.0f);
        }
        // 必须放在最后：下一次 MouseMove 要用这次的位置算差值
        mLastMousePos.x = x;
        mLastMousePos.y = y;
    }

private:
    // ======================== 每帧 ========================

    // 当前帧资源上次提交后记下的 Fence 值，GPU 还没执行到，就让 CPU 睡着等
    // （Fence == 0 表示这份帧资源还没提交过命令，不用等）
    void WaitForCurrentFrameResource()
    {
        if (mCurrentFrame->Fence == 0 || mFence->GetCompletedValue() >= mCurrentFrame->Fence)
        {
            return;
        }

        HANDLE eventHandle = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        if (eventHandle == nullptr)
        {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrentFrame->Fence, eventHandle));
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }

    //材质
    void UpdateMaterialCBs()
    {
        for (auto& e : mMaterials)
        {
            Material* mat = e.second.get();   // e.first 是名字 "crate"，e.second 才是 unique_ptr<Material>
            if (mat->NumFramesDirty > 0)
            {
                MaterialConstants mc;
                // 1) 把 mat 的 3 个字段抄进 mc
                mc.DiffuseAlbedo = mat->DiffuseAlbedo;
                mc.FresnelR0 = mat->FresnelR0;
                mc.Roughness = mat->Roughness;
                // 2) CopyData：第几格用 mat->MatCBIndex
                mCurrentFrame->MaterialCB->CopyData(mat->MatCBIndex,mc);
                // 3) NumFramesDirty--
                mat->NumFramesDirty--;
            }
        }
    }

    // 物体常量：只写还"脏"的渲染项，每写一份帧资源减 1，减到 0 就不再写
    void UpdateObjectCBs()
    {
        for (auto& ri : mAllItems)
        {
            if (ri->NumFramesDirty > 0)
            {
                ObjectConstants oc;
                XMStoreFloat4x4(&oc.World, XMMatrixTranspose(XMLoadFloat4x4(&ri->World))); // HLSL 按列主序读，要转置
                XMStoreFloat4x4(&oc.WorldInvTranspose, XMMatrixInverse(nullptr, XMLoadFloat4x4(&ri->World))); // 只存逆矩阵：逆转置 + 上传前的转置，两次转置抵消
                mCurrentFrame->ObjectCB->CopyData(ri->ObjCBIndex, oc);
                ri->NumFramesDirty--;
            }
        }
    }

    // Pass 常量：相机每帧都可能动，所以每帧都写
    void UpdateMainPassCB()
    {
        // 球坐标 → 相机位置（6.11 笔记里有这个公式），看向原点
        float x = mRadius * sinf(mPhi) * cosf(mTheta);
        float z = mRadius * sinf(mPhi) * sinf(mTheta);
        float y = mRadius * cosf(mPhi);
        XMStoreFloat4x4(&mView, XMMatrixLookAtLH(XMVectorSet(x, y, z, 1.0f), XMVectorZero(), XMVectorSet(0, 1, 0, 0)));

        PassConstants pc = {};
        XMStoreFloat4x4(&pc.ViewProj, XMMatrixTranspose(XMLoadFloat4x4(&mView) * XMLoadFloat4x4(&mProj)));
        pc.EyePosW = {x, y, z};
        pc.AmbientLight = {0.25f, 0.25f, 0.35f, 1.0f};
        /*struct Light
        {
        XMFLOAT3 Strength;
        float FalloffStart;

        XMFLOAT3 Direction;
        float FalloffEnd;

        XMFLOAT3 Position;
        float SpotPower;
        };*/
        pc.Lights[0].Strength = {1,1,1};
        pc.Lights[0].FalloffStart = 5;
        pc.Lights[0].Direction = {0.0f, -0.14834f, -0.98893f}; // 太阳在场景后方、压得很低，朝镜头斜着照过来，水面的镜面反射点才会进入画面
        pc.Lights[0].FalloffEnd = 100;
        pc.Lights[0].Position = {0,50,0};
        pc.Lights[0].SpotPower = 0.2f;

        // Lights[1]：点光源，悬在水面上方，暖白色。FalloffStart 以内全亮，FalloffEnd 以外照不到
        pc.Lights[1].Strength = {1.0f, 0.9f, 0.6f};
        pc.Lights[1].FalloffStart = 3.0f;
        pc.Lights[1].FalloffEnd = 14.0f;
        pc.Lights[1].Position = {6.0f, 2.0f, -34.0f};

        // Lights[2]：聚光灯，挂在左边山坡上方 8 个单位，竖直朝下照，冷白色
        pc.Lights[2].Strength = {0.7f, 0.8f, 1.0f};
        pc.Lights[2].FalloffStart = 8.0f;
        pc.Lights[2].FalloffEnd = 16.0f;
        pc.Lights[2].Position = {-2.8f, 11.9f, -40.0f};
        pc.Lights[2].Direction = {0.0f, -1.0f, 0.0f};
        pc.Lights[2].SpotPower = 32.0f;
        mCurrentFrame->PassCB->CopyData(0, pc);
    }

    void UpdateWaves(const GameTimer& gt)
    {
        static float tBase = 0.0f;
        if (gt.TotalTime() - tBase >= 0.25f)
        {
            tBase += 0.25f;

            static std::mt19937 rng{std::random_device{}()};
            std::uniform_int_distribution<int> row(4, mWaves->RowCount() - 5);
            std::uniform_int_distribution<int> col(4, mWaves->ColumnCount() - 5);
            std::uniform_real_distribution<float> magnitude(0.2f, 0.5f);
            mWaves->Disturb(row(rng), col(rng), magnitude(rng));
        }

        mWaves->Update(gt.DeltaTime());

        for (int i = 0; i < mWaves->VertexCount(); ++i)
        {
            ColorVertex vertex;
            vertex.Pos = mWaves->Position(i);
            vertex.Normal = mWaves->Normal(i);
            vertex.Color = XMFLOAT4(0.0f, 0.25f, 0.6f, 1.0f);
            mCurrentFrame->WavesVB->CopyData(i, vertex);
        }

        mWavesItem->Geo->VertexBufferGPU =
            mCurrentFrame->WavesVB->Resource();
    }

    // 逐个画渲染项：设 IA → 把根参数 0 指到这个物体的常量 → Draw
    void DrawRenderItems()
    {
        const UINT objCBByteSize = CalcConstantBufferByteSize(sizeof(ObjectConstants)); // 256 字节对齐
        const D3D12_GPU_VIRTUAL_ADDRESS objCBStart = mCurrentFrame->ObjectCB->Resource()->GetGPUVirtualAddress();
        
        const UINT MatCBByteSize = CalcConstantBufferByteSize(sizeof(MaterialConstants));
        const D3D12_GPU_VIRTUAL_ADDRESS MatCBStart = mCurrentFrame->MaterialCB->Resource()->GetGPUVirtualAddress();

        for (auto& ri : mAllItems)
        {
            // PSO 是一整包状态，只能整个换。木箱用木箱的，陆地和波浪用另一套
            mCommandList->SetPipelineState(ri->PSO.Get());
            // 视图是函数返回的临时对象，先存成变量才能取地址（直接 &ri->Geo->VertexBufferView() 会报 C2102）
            D3D12_VERTEX_BUFFER_VIEW vbv = ri->Geo->VertexBufferView();
            D3D12_INDEX_BUFFER_VIEW ibv = ri->Geo->IndexBufferView();
            mCommandList->IASetVertexBuffers(0, 1, &vbv);
            mCommandList->IASetIndexBuffer(&ibv);
            mCommandList->IASetPrimitiveTopology(ri->PrimitiveType);

            // 根 CBV 直接给 GPU 地址：ObjectCB 起始地址 + 第几个 × 每个的大小
            mCommandList->SetGraphicsRootConstantBufferView(0, objCBStart + ri->ObjCBIndex * objCBByteSize);
            //材质CBV绑定
            mCommandList->SetGraphicsRootConstantBufferView(3, MatCBStart + ri->Mat->MatCBIndex * MatCBByteSize);

            mCommandList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
        }
    }

    // ======================== 初始化（按 Initialize 里的调用顺序） ========================

    void LoadTextures()
    {
        auto tex = std::make_unique<DX_Utils::Texture>();
        tex->Name = "woodCrateTex";
        tex->Filename = L"Textures\\WoodCrate01.dds";
        ThrowIfFailed(CreateDDSTextureFromFile12(mDevice.Get(), mCommandList.Get(), tex->Filename.c_str(),
            tex->Resource, tex->UploadHeap));
        mTextures[tex->Name] = std::move(tex);
    }

    // 着色器可见的 CBV/SRV/UAV 堆。常量走根 CBV，不需要 CBV 描述符，所以堆里只有纹理的 1 个 SRV
    void BuildDescriptorHeaps()
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {
            .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            .NumDescriptors = 1,
            .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, // 着色器要从这个堆里读（RTV/DSV 堆不用，所以是 NONE）
            .NodeMask = 0,
        };
        ThrowIfFailed(mDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(mHeap.GetAddressOf())));
    }

    void BuildShapeGeometry()
    {
        // 盒子：边长 2、中心在原点，24 个顶点、36 个索引。
        // 地板交给陆地了（BuildLandGeometry），这里只管木箱
        GeometryGenerator geoGen;
        GeometryGenerator::MeshData box = geoGen.CreateBox(2.0f, 2.0f, 2.0f);

        // GeometryGenerator 的顶点有位置、法线、uv，我们的 Vertex 也一样，逐个字段拷过来
        std::vector<Vertex> vertices(box.Vertices.size());
        for (size_t i = 0; i < box.Vertices.size(); i++)
        {
            vertices[i].Pos = box.Vertices[i].Position;
            vertices[i].Normal = box.Vertices[i].Normal;
            vertices[i].TexC = box.Vertices[i].TexC;
        }

        // 每个形状各自的顶点都不到 65536 个，16 位索引够用
        std::vector<std::uint16_t> indices = box.GetIndices16();

        // 索引的元素类型、这里的 sizeof、IndexFormat = R16_UINT 三者必须一致
        const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
        const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

        mShapeGeo = std::make_unique<DX_Utils::MeshGeometry>();
        mShapeGeo->Name = "shapeGeo";

        // 内存里留一份副本（以后拾取、碰撞检测用）
        ThrowIfFailed(D3DCreateBlob(vbByteSize, &mShapeGeo->VertexBufferCPU));
        CopyMemory(mShapeGeo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);
        ThrowIfFailed(D3DCreateBlob(ibByteSize, &mShapeGeo->IndexBufferCPU));
        CopyMemory(mShapeGeo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

        // 经上传堆复制到默认堆；中转站存在 mShapeGeo 里，Initialize 末尾 Flush 之后再 DisposeUploaders
        mShapeGeo->VertexBufferGPU = CreateDefaultBuffer(mDevice.Get(), mCommandList.Get(),
                                                         vertices.data(), vbByteSize, mShapeGeo->VertexBufferUploader);
        mShapeGeo->IndexBufferGPU = CreateDefaultBuffer(mDevice.Get(), mCommandList.Get(),
                                                        indices.data(), ibByteSize, mShapeGeo->IndexBufferUploader);

        // 算视图要用的参数，VertexBufferView() / IndexBufferView() 每次调用时现算
        mShapeGeo->VertexByteStride = sizeof(Vertex);
        mShapeGeo->VertexBufferByteSize = vbByteSize;
        mShapeGeo->IndexFormat = DXGI_FORMAT_R16_UINT;
        mShapeGeo->IndexBufferByteSize = ibByteSize;

        // 每个形状在大缓冲里的位置：画几个索引、从第几个索引开始读、读到的索引再加多少
        DX_Utils::SubmeshGeometry submesh_Box;
        submesh_Box.IndexCount = (UINT)box.Indices32.size();
        submesh_Box.StartIndexLocation = 0;
        submesh_Box.BaseVertexLocation = 0;
        mShapeGeo->DrawArgs["box"] = submesh_Box;
    }

    // 陆地：一张 160×160 的大网格，按高度函数把 y 抬起来，再按高度上色。
    // 顶点格式是位置 + 顶点色，和木箱那套不一样，所以单独一份几何体
    void BuildLandGeometry()
    {
        GeometryGenerator geoGen;
        GeometryGenerator::MeshData grid = geoGen.CreateGrid(160.0f, 160.0f, 50, 50);

        std::vector<ColorVertex> vertices(grid.Vertices.size());
        for (size_t i = 0; i < grid.Vertices.size(); i++)
        {
            XMFLOAT3 p = grid.Vertices[i].Position;
            p.y = GetHillsHeight(p.x, p.z); // 把平的网格变成起伏的丘陵

            vertices[i].Pos = p;
            vertices[i].Normal = GetHillsNormal(vertices[i].Pos.x, vertices[i].Pos.z);
            // 按高度分带上色：沙滩 → 草绿 → 深绿 → 土黄 → 雪白
            if (p.y < -10.0f) vertices[i].Color = XMFLOAT4(1.0f, 0.96f, 0.62f, 1.0f);
            else if (p.y < 5.0f) vertices[i].Color = XMFLOAT4(0.48f, 0.77f, 0.46f, 1.0f);
            else if (p.y < 12.0f) vertices[i].Color = XMFLOAT4(0.1f, 0.48f, 0.19f, 1.0f);
            else if (p.y < 20.0f) vertices[i].Color = XMFLOAT4(0.45f, 0.39f, 0.34f, 1.0f);
            else vertices[i].Color = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        }

        std::vector<std::uint16_t> indices = grid.GetIndices16();
        const UINT vbByteSize = (UINT)vertices.size() * sizeof(ColorVertex);
        const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

        mLandGeo = std::make_unique<DX_Utils::MeshGeometry>();
        mLandGeo->Name = "landGeo";

        ThrowIfFailed(D3DCreateBlob(vbByteSize, &mLandGeo->VertexBufferCPU));
        CopyMemory(mLandGeo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);
        ThrowIfFailed(D3DCreateBlob(ibByteSize, &mLandGeo->IndexBufferCPU));
        CopyMemory(mLandGeo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

        mLandGeo->VertexBufferGPU = CreateDefaultBuffer(mDevice.Get(), mCommandList.Get(),
                                                        vertices.data(), vbByteSize, mLandGeo->VertexBufferUploader);
        mLandGeo->IndexBufferGPU = CreateDefaultBuffer(mDevice.Get(), mCommandList.Get(),
                                                       indices.data(), ibByteSize, mLandGeo->IndexBufferUploader);

        mLandGeo->VertexByteStride = sizeof(ColorVertex);
        mLandGeo->VertexBufferByteSize = vbByteSize;
        mLandGeo->IndexFormat = DXGI_FORMAT_R16_UINT;
        mLandGeo->IndexBufferByteSize = ibByteSize;

        DX_Utils::SubmeshGeometry submesh;
        submesh.IndexCount = (UINT)indices.size();
        submesh.StartIndexLocation = 0;
        submesh.BaseVertexLocation = 0;
        mLandGeo->DrawArgs["grid"] = submesh;
    }

    // 波浪模拟本体。第 5 步这里还会建水面的索引缓冲
    void BuildWaves()
    {
        mWaves = std::make_unique<Waves>(128, 128, 1.0f, 0.03f, 4.0f, 0.2f);

        const int m = mWaves->RowCount();
        const int n = mWaves->ColumnCount();
        std::vector<std::uint16_t> indices(
            3 * static_cast<size_t>(mWaves->TriangleCount()));

        int k = 0;
        for (int i = 0; i < m - 1; ++i)
        {
            for (int j = 0; j < n - 1; ++j)
            {
                indices[k + 0] = static_cast<std::uint16_t>(i * n + j);
                indices[k + 1] = static_cast<std::uint16_t>(i * n + j + 1);
                indices[k + 2] = static_cast<std::uint16_t>((i + 1) * n + j);
                indices[k + 3] = static_cast<std::uint16_t>((i + 1) * n + j);
                indices[k + 4] = static_cast<std::uint16_t>(i * n + j + 1);
                indices[k + 5] = static_cast<std::uint16_t>((i + 1) * n + j + 1);
                k += 6;
            }
        }

        const UINT vbByteSize = static_cast<UINT>(
            mWaves->VertexCount() * sizeof(ColorVertex));
        const UINT ibByteSize = static_cast<UINT>(
            indices.size() * sizeof(std::uint16_t));

        mWaterGeo = std::make_unique<DX_Utils::MeshGeometry>();
        mWaterGeo->Name = "waterGeo";

        // 顶点由每帧的 WavesVB 提供，这里只创建固定的索引缓冲。
        ThrowIfFailed(D3DCreateBlob(ibByteSize, &mWaterGeo->IndexBufferCPU));
        CopyMemory(mWaterGeo->IndexBufferCPU->GetBufferPointer(),
                   indices.data(), ibByteSize);
        mWaterGeo->IndexBufferGPU = CreateDefaultBuffer(
            mDevice.Get(), mCommandList.Get(),
            indices.data(), ibByteSize, mWaterGeo->IndexBufferUploader);

        mWaterGeo->VertexByteStride = sizeof(ColorVertex);
        mWaterGeo->VertexBufferByteSize = vbByteSize;
        mWaterGeo->IndexFormat = DXGI_FORMAT_R16_UINT;
        mWaterGeo->IndexBufferByteSize = ibByteSize;

        DX_Utils::SubmeshGeometry submesh;
        submesh.IndexCount = static_cast<UINT>(indices.size());
        submesh.StartIndexLocation = 0;
        submesh.BaseVertexLocation = 0;
        mWaterGeo->DrawArgs["grid"] = submesh;
    }

    // 陆地高度函数（书上 7.7）：x、z 决定 y，几个正弦余弦叠一叠就有起伏和平地
    float GetHillsHeight(float x, float z) const
    {
        return 0.3f * (z * sinf(0.1f * x) + x * cosf(0.1f * z));
    }

    // 地形表面的法线：n = (-df/dx, 1, -df/dz)，第 8 章做光照时直接能用
    XMFLOAT3 GetHillsNormal(float x, float z) const
    {
        XMFLOAT3 n(
            -0.03f * z * cosf(0.1f * x) - 0.3f * cosf(0.1f * z),
            1.0f,
            -0.3f * sinf(0.1f * x) + 0.03f * x * sinf(0.1f * z));
        XMStoreFloat3(&n, XMVector3Normalize(XMLoadFloat3(&n)));
        return n;
    }

    void BuildMaterials()
    {
        std::unique_ptr<Material> LMaterial_crate = std::make_unique<Material>();
        std::unique_ptr<Material> LMaterial_grass = std::make_unique<Material>();
        std::unique_ptr<Material> LMaterial_water = std::make_unique<Material>();

        LMaterial_crate->Name = "crate";
        LMaterial_crate->MatCBIndex = 0;
        LMaterial_crate->DiffuseAlbedo = {1, 1, 1, 1};
        LMaterial_crate->FresnelR0 = {0.05f, 0.05f, 0.05f};
        LMaterial_crate->Roughness = 0.2f;

        LMaterial_grass->Name = "grass";
        LMaterial_grass->MatCBIndex = 1;
        LMaterial_grass->DiffuseAlbedo = {0.2f, 0.6f, 0.2f, 1};
        LMaterial_grass->FresnelR0 = {0.01f, 0.01f, 0.01f};
        LMaterial_grass->Roughness = 0.125f;

        LMaterial_water->Name = "water";
        LMaterial_water->MatCBIndex = 2;
        LMaterial_water->DiffuseAlbedo = {0.0f, 0.2f, 0.6f, 1};
        LMaterial_water->FresnelR0 = {0.1f, 0.1f, 0.1f};
        LMaterial_water->Roughness = 0;

        mMaterials[LMaterial_crate->Name] = std::move(LMaterial_crate);
        mMaterials[LMaterial_grass->Name] = std::move(LMaterial_grass);
        mMaterials[LMaterial_water->Name] = std::move(LMaterial_water);
    }

    // 只放木箱和陆地两种渲染项（水面在第 5 步加进来）。
    // 木箱和陆地各用一对顶点/索引缓冲、各用一套 PSO
    void BuildRenderItems()
    {
        UINT objCBIndex = 0;

        //水面
        auto water = std::make_unique<RenderItem>();
        XMStoreFloat4x4(&water->World, XMMatrixIdentity());
        water->PSO = mColorPSO;
        water->ObjCBIndex = objCBIndex++;
        water->Geo = mWaterGeo.get();
        water->IndexCount = mWaterGeo->DrawArgs["grid"].IndexCount;
        water->StartIndexLocation = mWaterGeo->DrawArgs["grid"].StartIndexLocation;
        water->BaseVertexLocation = mWaterGeo->DrawArgs["grid"].BaseVertexLocation;
        water->Mat = mMaterials.at("water").get();

        mWavesItem = water.get();
        mAllItems.push_back(std::move(water));

        // 陆地：顶点里已经把高度算进去了（BuildLandGeometry），世界矩阵用单位阵。
        // 顶点格式是位置 + 顶点色，所以用 mColorPSO
        auto land = std::make_unique<RenderItem>();
        XMStoreFloat4x4(&land->World, XMMatrixIdentity());
        land->PSO = mColorPSO;
        land->ObjCBIndex = objCBIndex++;
        land->Geo = mLandGeo.get();
        land->IndexCount = mLandGeo->DrawArgs["grid"].IndexCount;
        land->StartIndexLocation = mLandGeo->DrawArgs["grid"].StartIndexLocation;
        land->BaseVertexLocation = mLandGeo->DrawArgs["grid"].BaseVertexLocation;
        land->Mat = mMaterials.at("grass").get();
        mAllItems.push_back(std::move(land));

        // 10×10 个木箱铺在 xz 平面上，顺带当尺度参照物
        const int gridCount = 10; // 每行、每列几个
        const float spacing = 3.0f; // 相邻两个木箱中心的距离（木箱边长是 2）
        const DX_Utils::SubmeshGeometry& box = mShapeGeo->DrawArgs["box"];

        for (int i = 0; i < gridCount; i++)
        {
            for (int j = 0; j < gridCount; j++)
            {
                auto crate = std::make_unique<RenderItem>();
                crate->PSO = mCratePSO;

                // 减去 (gridCount - 1) / 2，让整片木箱以原点为中心
                float x = (i - (gridCount - 1) * 0.5f) * spacing;
                float z = (j - (gridCount - 1) * 0.5f) * spacing;

                XMStoreFloat4x4(&crate->World, XMMatrixTranslation(x, 0.0f, z));
                crate->ObjCBIndex = objCBIndex++; // 每个木箱在 ObjectCB 里占自己的一格
                crate->Geo = mShapeGeo.get();
                crate->IndexCount = box.IndexCount;
                crate->StartIndexLocation = box.StartIndexLocation;
                crate->BaseVertexLocation = box.BaseVertexLocation;
                crate->Mat = mMaterials.at("crate").get();
                mAllItems.push_back(std::move(crate));
            }
        }
    }

    // 3 份帧资源，每份：1 个 Pass 常量 + 每个渲染项 1 个物体常量
    void BuildFrameResources()
    {
        for (int i = 0; i < gNumFrameResources; i++)
        {
            mFrameResources.push_back(
                std::make_unique<FrameResource>(
                    mDevice.Get(),
                    1,
                    (UINT)mAllItems.size(),
                    (UINT)mWaves->VertexCount(),
                    (UINT)mMaterials.size()));
        }
    }

    // 在堆的第 0 格为木箱纹理建 SRV。格式和 mip 数必须取自纹理本身（不是上传堆）
    void BuildShaderResourceViews()
    {
        auto& woodTex = mTextures.at("woodCrateTex")->Resource;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {
            .Format = woodTex->GetDesc().Format,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = {
                .MostDetailedMip = 0,
                .MipLevels = woodTex->GetDesc().MipLevels,
                .PlaneSlice = 0,
                .ResourceMinLODClamp = 0.0f,
            },
        };
        mDevice->CreateShaderResourceView(woodTex.Get(), &srvDesc, mHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // 根签名 = 着色器的"参数列表"：
    //   参数 0：根 CBV → b0，物体常量（每个物体设一次）
    //   参数 1：描述符表 → t0，木箱纹理（只有像素着色器用）
    //   参数 2：根 CBV → b1，Pass 常量（每帧设一次）
    //   静态采样器 s0：三线性过滤 + WRAP（CD3DX12_STATIC_SAMPLER_DESC 默认的寻址模式）
    void BuildRootSignature()
    {
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // 1 个 SRV，从 t0 开始

        CD3DX12_ROOT_PARAMETER slotRootParameter[4];
        slotRootParameter[0].InitAsConstantBufferView(0);
        slotRootParameter[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        slotRootParameter[2].InitAsConstantBufferView(1);
        slotRootParameter[3].InitAsConstantBufferView(2);

        CD3DX12_STATIC_SAMPLER_DESC linearWrap(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
            _countof(slotRootParameter), slotRootParameter,
            1, &linearWrap,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        // 第一步：把描述序列化成一段二进制数据；出错时错误信息在 error 里
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> error;
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
        if (error != nullptr)
        {
            OutputDebugStringA((char*)error->GetBufferPointer());
        }
        ThrowIfFailed(hr);

        // 第二步：用这段二进制数据创建根签名对象
        ThrowIfFailed(mDevice->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)));
    }

    void BuildShadersAndInputLayout()
    {
        // 灯的数量用宏传给着色器，顺序必须和 UpdateMainPassCB 里填灯的顺序一致：先平行光，再点光源，最后聚光灯
        const D3D_SHADER_MACRO lightDefines[] = {
            {"NUM_DIR_LIGHTS", "1"},
            {"NUM_POINT_LIGHTS", "1"},
            {"NUM_SPOT_LIGHTS", "1"},
            {nullptr, nullptr}};
        // 用到了 ConstantBuffer<T>，需要 Shader Model 5.1
        mCrateVSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", lightDefines, "VS", "vs_5_1");
        mCratePSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", lightDefines, "PS", "ps_5_1");
        // 语义名, 语义索引, 格式, 输入槽, 在顶点里的字节偏移, 按顶点/按实例, 实例步进
        mCrateInputLayout = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        // 陆地和水面用另一对着色器：除了变换，直接把顶点色输出
        mColorVSByteCode = CompileShader(L"HLSL\\Color.hlsl", lightDefines, "VS", "vs_5_1");
        mColorPSByteCode = CompileShader(L"HLSL\\Color.hlsl", lightDefines, "PS", "ps_5_1");
        // 陆地和水面：位置（12 字节）+ 顶点色（16 字节），和 struct ColorVertex 一致。
        // 输入布局必须和顶点结构体、以及对应的 HLSL VertexIn 三者对齐
        mColorInputLayout = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
    }

    void BuildPSO()
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = mRootSignature.Get();

        // 固定功能阶段和输出合并，两个 PSO 完全一样
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = mBackBufferFormat;
        psoDesc.DSVFormat = mDepthStencilFormat;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.SampleDesc.Quality = 0;

        // 木箱：位置 + 贴图坐标，PS 采样纹理
        psoDesc.InputLayout = {mCrateInputLayout.data(), (UINT)mCrateInputLayout.size()};
        psoDesc.VS = {mCrateVSByteCode->GetBufferPointer(), mCrateVSByteCode->GetBufferSize()};
        psoDesc.PS = {mCratePSByteCode->GetBufferPointer(), mCratePSByteCode->GetBufferSize()};
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&psoDesc,IID_PPV_ARGS(&mCratePSO)));

        // 陆地和水面：位置 + 顶点色，PS 直接返回颜色
        psoDesc.InputLayout = {mColorInputLayout.data(), (UINT)mColorInputLayout.size()};
        psoDesc.VS = {mColorVSByteCode->GetBufferPointer(), mColorVSByteCode->GetBufferSize()};
        psoDesc.PS = {mColorPSByteCode->GetBufferPointer(), mColorPSByteCode->GetBufferSize()};
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&psoDesc,IID_PPV_ARGS(&mColorPSO)));
    }

    // ======================== 成员变量 ========================

    // 资源：纹理、几何体、渲染项
    std::unordered_map<std::string, std::unique_ptr<DX_Utils::Texture>> mTextures;
    // 材质
    std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
    // 木箱：位置 + 贴图坐标，静态数据，放在默认堆里
    std::unique_ptr<DX_Utils::MeshGeometry> mShapeGeo;
    // 陆地：位置 + 顶点色，静态数据，放在默认堆里
    std::unique_ptr<DX_Utils::MeshGeometry> mLandGeo;
    std::unique_ptr<DX_Utils::MeshGeometry> mWaterGeo;
    // 波浪：CPU 端的模拟。只算高度和法线，不碰任何 D3D 资源
    std::unique_ptr<Waves> mWaves;
    std::vector<std::unique_ptr<RenderItem>> mAllItems;
    RenderItem* mWavesItem = nullptr;

    // 帧资源：3 份轮流用，CPU 最多领先 GPU 3 帧
    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrentFrame = nullptr; // 当前这一帧用的那一份
    int mCurrFrameResourceIndex = 0;

    // 管线：描述符堆、根签名、着色器、输入布局、PSO（两套：木箱一套，陆地/水面共用一套）
    ComPtr<ID3D12DescriptorHeap> mHeap; // 着色器可见的 CBV/SRV/UAV 堆（现在只有 1 个 SRV）
    ComPtr<ID3D12RootSignature> mRootSignature;
    ComPtr<ID3DBlob> mCrateVSByteCode;
    ComPtr<ID3DBlob> mCratePSByteCode;
    ComPtr<ID3DBlob> mColorVSByteCode;
    ComPtr<ID3DBlob> mColorPSByteCode;
    std::vector<D3D12_INPUT_ELEMENT_DESC> mCrateInputLayout;
    std::vector<D3D12_INPUT_ELEMENT_DESC> mColorInputLayout;
    ComPtr<ID3D12PipelineState> mCratePSO;
    ComPtr<ID3D12PipelineState> mColorPSO;

    // 相机：绕原点转的球坐标 (θ, φ, r)
    XMFLOAT4X4 mView = XMFLOAT4X4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    XMFLOAT4X4 mProj = XMFLOAT4X4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    float mTheta = 1.5f * XM_PI; // 绕 y 轴转的角度
    float mPhi = XM_PIDIV2 - 0.1f; // 和 +y 轴的夹角：快贴着地平线看，才看得出地形起伏
    float mRadius = 50.0f; // 离原点多远（地形 160 宽、水面 128 宽，站远了才看得全）
    POINT mLastMousePos = {};
};

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
        return theApp.Run(); // 消息循环
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK | MB_ICONERROR);
        return 0;
    }
    catch (const std::exception& e)
    {
        MessageBoxA(nullptr, e.what(), "Exception", MB_OK);
        return 0;
    }
}
