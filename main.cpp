// LearnDX12 · 第 7 章：帧资源 + 渲染项，画 10×10 个带纹理的木箱
//
// 分工：D3DApp 负责窗口、设备、交换链、深度缓冲和消息循环；
//       CrateApp 只管自己的资源（纹理、几何体、渲染项、帧资源、根签名、PSO）以及每帧的更新和绘制。

#include "Common/D3DApp.h"
#include "Common/UploadBuffer.h"
#include "Common/DDSTextureLoader.h"
#include "Common/GeometryGenerator.h"
#include "Common/Camera.h"
#include "FrameResource.h"
#include "Waves.h"

#include <algorithm>        // std::clamp
#include <cmath>            // powf
#include <format>           // std::format：窗口标题里显示移速
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

// 公告牌树的顶点：一棵树只有一个顶点，GS 再把它扩成四边形。
// 必须和树的输入布局（POSITION 偏移 0、SIZE 偏移 12）、TreeSprite.hlsl 里的 VertexIn 一致
struct TreeSpriteVertex
{
    XMFLOAT3 Pos; // 树的中心，直接是世界坐标
    XMFLOAT2 Size; // 宽、高
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

// 类外面（RenderItem 结构体下面）
enum class RenderLayer : int
{
    Opaque = 0,
    Mirrors,
    MirrorClear,
    Reflected,
    Shadow,
    Transparent,
    Count
};

static constexpr UINT kTimestampCount = 6;

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
        mCamera.SetMoveSpeed(5.f);
        // 初始位置：站在换摄像机之前的老位置（离原点 50、略高于水面），看向原点
        mCamera.LookAt(XMFLOAT3(0.0f, 4.99f, -49.75f), XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 1.0f, 0.0f));
        LoadTextures(); // 录制复制命令（纹理）
        BuildDescriptorHeaps();
        BuildBlurResources(); // 模糊用的两张纹理和它们的 4 个描述符（要用到堆）
        BuildTimestampQueries(); //Pass时间记录
        BuildShapeGeometry(); // 录制复制命令（木箱）
        BuildLandGeometry(); // 录制复制命令（陆地的顶点/索引）
        BuildWaves(); // 波浪模拟（这一步只建模拟本体，第 5 步再建水面几何）
        BuildTreeSpritesGeometry(); // 录制复制命令（树的点）
        BuildShaderResourceViews(); // 要用到纹理和堆
        BuildRootSignature(); // PSO 要用根签名
        BuildBlurRootSignature(); //模糊专用的根签名
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
        mTreeGeo->DisposeUploaders();
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
        mCamera.SetLens(0.25f * XM_PI, AspectRatio(), 1.0f, 1000.f);

        // 模糊纹理必须和后台缓冲一样大（要互相 CopyResource）。
        // Initialize 里第一次调用到这里时描述符堆还没建，跳过，留给 Initialize 自己建
        if (mHeap)
        {
            BuildBlurResources();
        }
    }

    //构建Blur View
    // 后期模糊用的两张纹理 A = mBlurMap[0]、B = mBlurMap[1]：大小、格式都和后台缓冲一样，
    // 带 ALLOW_UNORDERED_ACCESS 标志（计算着色器要往里写，没有这个标志建不了 UAV）。
    // 约定：每帧开始和结束时两张都在 COMMON 状态。
    // 描述符放在堆的第 5～8 格：5 = A 的 SRV，6 = A 的 UAV，7 = B 的 SRV，8 = B 的 UAV
    void BuildBlurResources()
    {
        D3D12_RESOURCE_DESC texDesc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            .Alignment = 0,
            .Width = mClientWidth,
            .Height = mClientHeight,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = mBackBufferFormat,
            .SampleDesc = {.Count = 1, .Quality = 0},
            .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
            .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        };
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        // 先存成变量再取地址（直接 &CD3DX12_HEAP_PROPERTIES(...) 会报 C2102）
        for (int i = 0; i < 2; i++)
        {
            mBlurMap[i].Reset(); // 窗口变大小时先释放旧的（D3DApp::OnResize 开头 Flush 过，GPU 已经不用它了）
            ThrowIfFailed(mDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
                D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(&mBlurMap[i])));
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {
            .Format = mBackBufferFormat,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = {.MostDetailedMip = 0, .MipLevels = 1},
        };
        // UAV 描述：只能指定一个 mip 级别（MipSlice），计算着色器就往这一级里写
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
            .Format = mBackBufferFormat,
            .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D,
            .Texture2D = {.MipSlice = 0},
        };
        for (int i = 0; i < 2; i++)
        {
            CD3DX12_CPU_DESCRIPTOR_HANDLE srv(mHeap->GetCPUDescriptorHandleForHeapStart(), 5 + 2 * i,
                                              mCSUDescriptorSize);
            CD3DX12_CPU_DESCRIPTOR_HANDLE uav(mHeap->GetCPUDescriptorHandleForHeapStart(), 6 + 2 * i,
                                              mCSUDescriptorSize);
            mDevice->CreateShaderResourceView(mBlurMap[i].Get(), &srvDesc, srv);
            mDevice->CreateUnorderedAccessView(mBlurMap[i].Get(), nullptr, &uavDesc, uav); // 第二个参数是计数器资源，用不到
        }
    }

    void BuildTimestampQueries()
    {
        //查询堆：用来存放时间戳读数的一块显存，有点像描述符堆，只不过每格放的是一个查询结果。
        D3D12_QUERY_HEAP_DESC query_heap_desc = {
            .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
            .Count = kTimestampCount * gNumFrameResources,
            .NodeMask = 0,
        };
        ThrowIfFailed(mDevice->CreateQueryHeap(&query_heap_desc,IID_PPV_ARGS(&mTimestampHeap)));
        //回读缓冲：GPU 把查询堆里的读数复制到这里，CPU 再从这里读。和截图用的回读缓冲是同一种东西。
        CD3DX12_HEAP_PROPERTIES readBackHeap(D3D12_HEAP_TYPE_READBACK);
        CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(query_heap_desc.Count * sizeof(UINT64));
        ThrowIfFailed(mDevice->CreateCommittedResource(
            &readBackHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&mTimestampReadback)));
        ThrowIfFailed(mCommandQueue->GetTimestampFrequency(&mTimestampFreq)); // 每秒多少个"嘀嗒"
    }

    // 后期模糊：进来时后台缓冲是 RENDER_TARGET，出去时还是；A、B 进出都是 COMMON
    void DoBlur()
    {
        // 0. 权重和半径，作为根常量传进去
        std::vector<float> weights = CalcGaussWeights(2.5f);
        const int blurRadius = (int)weights.size() / 2;
        mCommandList->SetComputeRootSignature(mBlurRootSignature.Get());
        mCommandList->SetComputeRoot32BitConstants(0, 1, &blurRadius, 0); // 第 0 格：半径
        mCommandList->SetComputeRoot32BitConstants(0, (UINT)weights.size(), weights.data(), 1); // 第 1～11 格：权重

        ID3D12Resource* back = CurrentBackBuffer();
        ID3D12Resource* A = mBlurMap[0].Get();
        ID3D12Resource* B = mBlurMap[1].Get();

        // ① 场景复制进 A
        D3D12_RESOURCE_BARRIER b1[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(back, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(A, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        mCommandList->ResourceBarrier(2, b1);
        mCommandList->CopyResource(A, back); // 参数顺序：(目标, 来源)

        D3D12_RESOURCE_BARRIER b2[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(A, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(B, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        };
        mCommandList->ResourceBarrier(2, b2);

        const UINT groupsX = (UINT)ceilf(mClientWidth / 256.0f);
        const UINT groupsY = (UINT)ceilf(mClientHeight / 256.0f);
        for (int i = 0; i < mBlurCount; i++)
        {
            // ③a 横向：读 A，写 B
            mCommandList->SetPipelineState(mHorzBlurPSO.Get());
            mCommandList->SetComputeRootDescriptorTable(1, HeapGpuHandle(5)); // 根参数 1（输入）= 堆第 5 格：A 的 SRV
            mCommandList->SetComputeRootDescriptorTable(2, HeapGpuHandle(8)); // 空 2：根参数 2（输出）= B 的 UAV 在第几格？
            mCommandList->Dispatch(groupsX, mClientHeight, 1); // 每个线程组管一行里的 256 个像素

            // 交换：A 变成可写，B 变成可读
            D3D12_RESOURCE_BARRIER s1[] = {
                CD3DX12_RESOURCE_BARRIER::Transition(A, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                CD3DX12_RESOURCE_BARRIER::Transition(B, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), // 空 3
            };
            mCommandList->ResourceBarrier(2, s1);

            // ③b 纵向：读 B，写 A
            mCommandList->SetPipelineState(mVertBlurPSO.Get());
            mCommandList->SetComputeRootDescriptorTable(1, HeapGpuHandle(7)); // 空 4：B 的 SRV
            mCommandList->SetComputeRootDescriptorTable(2, HeapGpuHandle(6)); // 空 4：A 的 UAV
            mCommandList->Dispatch(mClientWidth, groupsY, 1); // 空 5：每个线程组管一列里的 256 个像素

            // 交换回来，下一轮又从"读 A、写 B"开始
            D3D12_RESOURCE_BARRIER s2[] = {
                CD3DX12_RESOURCE_BARRIER::Transition(A, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(B, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            mCommandList->ResourceBarrier(2, s2);
        }

        // ④ 结果在 A 里，复制回后台缓冲
        D3D12_RESOURCE_BARRIER b4[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(back, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                 D3D12_RESOURCE_STATE_COPY_DEST),
            CD3DX12_RESOURCE_BARRIER::Transition(A, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        mCommandList->ResourceBarrier(2, b4);
        mCommandList->CopyResource(back, A); // 空 6

        // ⑤ 收尾：全部恢复成进来时的样子
        D3D12_RESOURCE_BARRIER b5[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(back, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(A, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
            CD3DX12_RESOURCE_BARRIER::Transition(B, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
        };
        mCommandList->ResourceBarrier(3, b5);
    }

    // 堆里第 slot 格的 GPU 句柄（SetComputeRootDescriptorTable 要用）
    CD3DX12_GPU_DESCRIPTOR_HANDLE HeapGpuHandle(int slot) const
    {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(mHeap->GetGPUDescriptorHandleForHeapStart(), slot, mCSUDescriptorSize);
    }

    // WASD 前后左右，Q 下降、E 上升（沿世界 y 轴，和朝向无关）
    void OnKeyboardInput(const GameTimer& gt)
    {
        auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };

        // B 键开关后期模糊。按住不放时 GetAsyncKeyState 每帧都返回"按下"，
        // 所以只在"上一帧没按、这一帧按了"的那一帧切换一次
        const bool blurKey = down('B');
        if (blurKey && !mBlurKeyWasDown)
        {
            mBlurEnabled = !mBlurEnabled;
        }
        mBlurKeyWasDown = blurKey;

        // 每个键单独判断、各记一个方向：W + D 同时按就是斜着走，W + S 同时按互相抵消
        float forward = 0.0f, right = 0.0f, up = 0.0f;
        if (down('W')) forward += 1.0f;
        if (down('S')) forward -= 1.0f;
        if (down('D')) right += 1.0f;
        if (down('A')) right -= 1.0f;
        if (down('E')) up += 1.0f;
        if (down('Q')) up -= 1.0f;

        // 斜着走时 (1, 1) 的长度是 √2，不处理的话斜走比直走快 41%。把水平方向缩回长度 1
        float len = sqrtf(forward * forward + right * right);
        if (len > 1.0f)
        {
            forward /= len;
            right /= len;
        }

        // 没按的方向不调用：Walk / Strafe / Ascend 都会把观察矩阵标成"脏"，不动就别让它每帧重建
        const float dt = gt.DeltaTime();
        if (forward != 0.0f) mCamera.Walk(forward * dt);
        if (right != 0.0f) mCamera.Strafe(right * dt);
        if (up != 0.0f) mCamera.Ascend(up * dt);
    }

    void Update(const GameTimer& gt) override
    {
        // 先处理输入、移动摄像机，这一帧的 ViewProj 才是移动后的（放在最后会晚一帧）
        OnKeyboardInput(gt);

        // 轮换到下一份帧资源；GPU 如果还在用它，就先等
        mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
        mCurrentFrame = mFrameResources[mCurrFrameResourceIndex].get();
        WaitForCurrentFrameResource();  //执行完，说明上一次用这份帧资源的那一帧，GPU 已经全部做完了。
        
        if (mCurrentFrame->Fence !=0)
        {
            const UINT tsBase = mCurrFrameResourceIndex * kTimestampCount;
            D3D12_RANGE readRange{tsBase * sizeof(UINT64), (tsBase + kTimestampCount) * sizeof(UINT64)};  // 告诉驱动：CPU 要读哪一段
            UINT64* data = nullptr;
            ThrowIfFailed(mTimestampReadback->Map(0, &readRange, reinterpret_cast<void**>(&data)));
            const UINT64* t = data + tsBase; // Map 返回的是整个缓冲的开头，要自己加偏移才是这一帧的 T0
            // 相邻两个时间戳之差 = 这一段的耗时（嘀嗒数 → 毫秒）。必须在 Unmap 之前读完
            for (UINT k = 0; k < kTimestampCount - 1; k++)
            {
                mPassMsSum[k] += (t[k + 1] - t[k]) * 1000.0 / mTimestampFreq;
            }
            D3D12_RANGE noWrite{0, 0}; // CPU 一个字节都没写
            mTimestampReadback->Unmap(0, &noWrite);

            // 每帧的数会跳，攒 60 帧取平均再显示，然后清零重新攒
            if (++mPassSamples == 60)
            {
                double avg[kTimestampCount - 1];
                for (UINT k = 0; k < kTimestampCount - 1; k++)
                {
                    avg[k] = mPassMsSum[k] / mPassSamples;
                    mPassMsSum[k] = 0.0;
                }
                mPassSamples = 0;
                // 基类每秒刷新一次标题（CalculateFrameStats），会把这段文字带上
                mMainWndCaption = std::format(
                    L"LearnDX12    移速 {:.1f}    GPU：不透明 {:.3f} | 镜子 {:.3f} | 影子 {:.3f} | 透明 {:.3f} | 模糊 {:.3f} ms",
                    mCamera.GetMoveSpeed(), avg[0], avg[1], avg[2], avg[3], avg[4]);
            }
        }
        
        UpdateWaves(gt);

        UpdateMaterialCBs();
        UpdateObjectCBs();
        UpdateMainPassCB();
    }

    void Draw(const GameTimer& gt) override
    {
        const UINT tsBase = mCurrFrameResourceIndex * kTimestampCount; // 这份帧资源的 6 格从这里开始
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

        mCommandList->EndQuery(mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 0); // T0
        DrawRenderItems(mRitemLayer[(int)RenderLayer::Opaque]);
        mCommandList->EndQuery(mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 1); // T1
        mCommandList->OMSetStencilRef(1); // 章上刻"1"
        DrawRenderItems(mRitemLayer[(int)RenderLayer::Mirrors]); // ③ 看得见的镜子像素盖章
        DrawRenderItems(mRitemLayer[(int)RenderLayer::MirrorClear]); // ③b 盖了章的地方：深度推远、刷成天空色
        // ④ 镜像：Ref 还是 1，只画在模板 = 1 的像素；Pass 常量换成第 1 份（反射后的灯）
        const UINT passCBByteSize = CalcConstantBufferByteSize(sizeof(PassConstants));
        const D3D12_GPU_VIRTUAL_ADDRESS passCB = mCurrentFrame->PassCB->Resource()->GetGPUVirtualAddress();
        mCommandList->SetGraphicsRootConstantBufferView(2, passCB + passCBByteSize);
        DrawRenderItems(mRitemLayer[(int)RenderLayer::Reflected]);
        mCommandList->SetGraphicsRootConstantBufferView(2, passCB); // 换回主 Pass 常量，后面的镜子和水要用
        mCommandList->OMSetStencilRef(0); // 用完恢复
        mCommandList->EndQuery(mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 2); // T2
        DrawRenderItems(mRitemLayer[(int)RenderLayer::Shadow]);
        mCommandList->EndQuery(mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 3); // T3
        DrawRenderItems(mRitemLayer[(int)RenderLayer::Transparent]);
        mCommandList->EndQuery(mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 4); // T4
        if (mBlurEnabled)
        {
            DoBlur();
        }
        mCommandList->EndQuery(mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 5); // T5
        mCommandList->ResolveQueryData(
            mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
            tsBase, kTimestampCount, // 空 1：从查询堆第几格开始、复制几格
            mTimestampReadback.Get(),
            tsBase*sizeof(UINT64)); // 空 2：复制到回读缓冲的第几个字节

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
            mCamera.Pitch(dy);
            mCamera.RotateY(dx);
        }
        // 必须放在最后：下一次 MouseMove 要用这次的位置算差值
        mLastMousePos.x = x;
        mLastMousePos.y = y;
    }

    // 滚轮调移速：往前滚一格 ×1.25，往后滚一格 ÷1.25。
    // 按倍数而不是按加减调：慢的时候每格变化小、调得细，快的时候每格变化大、调得快
    void OnMouseWheel(int delta) override
    {
        float speed = mCamera.GetMoveSpeed() * powf(1.25f, delta / static_cast<float>(WHEEL_DELTA));
        speed = std::clamp(speed, 0.5f, 200.0f);
        mCamera.SetMoveSpeed(speed);
        // 基类每秒刷新一次标题（CalculateFrameStats），会把这段文字带上
        mMainWndCaption = std::format(L"LearnDX12    移速 {:.1f}", speed);
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
            Material* mat = e.second.get(); // e.first 是名字 "crate"，e.second 才是 unique_ptr<Material>
            if (mat->NumFramesDirty > 0)
            {
                MaterialConstants mc;
                // 1) 把 mat 的 3 个字段抄进 mc
                mc.DiffuseAlbedo = mat->DiffuseAlbedo;
                mc.FresnelR0 = mat->FresnelR0;
                mc.Roughness = mat->Roughness;
                // 2) CopyData：第几格用 mat->MatCBIndex
                mCurrentFrame->MaterialCB->CopyData(mat->MatCBIndex, mc);
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
                XMStoreFloat4x4(&oc.WorldInvTranspose, XMMatrixInverse(nullptr, XMLoadFloat4x4(&ri->World)));
                // 只存逆矩阵：逆转置 + 上传前的转置，两次转置抵消
                mCurrentFrame->ObjectCB->CopyData(ri->ObjCBIndex, oc);
                ri->NumFramesDirty--;
            }
        }
    }

    // Pass 常量：相机每帧都可能动，所以每帧都写
    void UpdateMainPassCB()
    {
        mCamera.UpdateViewMatrix();

        PassConstants pc = {};
        XMStoreFloat4x4(&pc.ViewProj, XMMatrixTranspose(mCamera.GetView() * mCamera.GetProj()));
        pc.EyePosW = mCamera.GetPosition3f();
        pc.AmbientLight = {0.25f, 0.25f, 0.35f, 1.0f};
        pc.FogColor = XMFLOAT4(Colors::LightSteelBlue.f);
        pc.FogStart = 70.f;
        pc.FogRange = 120.f;
        /*struct Light
        {
        XMFLOAT3 Strength;
        float FalloffStart;

        XMFLOAT3 Direction;
        float FalloffEnd;

        XMFLOAT3 Position;
        float SpotPower;
        };*/
        pc.Lights[0].Strength = {1, 1, 1};
        pc.Lights[0].FalloffStart = 5;
        pc.Lights[0].Direction = mSunDir; // 太阳在场景后方、压得很低，朝镜头斜着照过来，水面的镜面反射点才会进入画面
        pc.Lights[0].FalloffEnd = 100;
        pc.Lights[0].Position = {0, 50, 0};
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

        PassConstants reflectedPass = pc; // 其余都一样，只把灯反射一下
        const XMMATRIX R = XMMatrixReflect(XMLoadFloat4(&mMirrorPlane));
        for (int i = 0; i < 3; ++i)
        {
            // 方向：只转不平移（w = 0）；位置：要平移（w = 1）
            XMStoreFloat3(&reflectedPass.Lights[i].Direction,
                          XMVector3TransformNormal(XMLoadFloat3(&pc.Lights[i].Direction), R));
            XMStoreFloat3(&reflectedPass.Lights[i].Position,
                          XMVector3TransformCoord(XMLoadFloat3(&pc.Lights[i].Position), R));
        }
        mCurrentFrame->PassCB->CopyData(1, reflectedPass);
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
    void DrawRenderItems(const std::vector<RenderItem*>& items)
    {
        const UINT objCBByteSize = CalcConstantBufferByteSize(sizeof(ObjectConstants)); // 256 字节对齐
        const D3D12_GPU_VIRTUAL_ADDRESS objCBStart = mCurrentFrame->ObjectCB->Resource()->GetGPUVirtualAddress();

        const UINT MatCBByteSize = CalcConstantBufferByteSize(sizeof(MaterialConstants));
        const D3D12_GPU_VIRTUAL_ADDRESS MatCBStart = mCurrentFrame->MaterialCB->Resource()->GetGPUVirtualAddress();

        for (auto& ri : items)
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

            // 算出"堆起点往后数 N 格"的 GPU 地址：N 从材质里取，每格的字节数是 mCSUDescriptorSize
            CD3DX12_GPU_DESCRIPTOR_HANDLE tex(mHeap->GetGPUDescriptorHandleForHeapStart(), ri->Mat->DiffuseSrvHeapIndex,
                                              mCSUDescriptorSize);
            // 把书签夹到这里
            mCommandList->SetGraphicsRootDescriptorTable(1, tex);

            mCommandList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
        }
    }

    // ======================== 初始化（按 Initialize 里的调用顺序） ========================

    void LoadTextureHelper(std::string Name, std::wstring FileName)
    {
        auto Tex = std::make_unique<DX_Utils::Texture>();
        Tex->Name = Name;
        Tex->Filename = FileName;
        ThrowIfFailed(CreateDDSTextureFromFile12(mDevice.Get(), mCommandList.Get(), Tex->Filename.c_str(),
            Tex->Resource, Tex->UploadHeap));
        mTextures[Tex->Name] = std::move(Tex);
    }

    //加载材质到mTextures成员变量
    void LoadTextures()
    {
        LoadTextureHelper("woodCrateTex", L"Textures\\WoodCrate01.dds");
        LoadTextureHelper("WireFenceTex", L"Textures\\WireFence.dds");
        LoadTextureHelper("iceTex", L"Textures\\ice.dds");
        LoadTextureHelper("tileTex", L"Textures\\tile.dds");
        LoadTextureHelper("treeArrayTex", L"Textures\\treeArray2.dds");
        //书上的 DDS 加载函数认得纹理数组，建出来的资源 DepthOrArraySize 会是 3。
    }

    // 构建描述符堆
    void BuildDescriptorHeaps()
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {
            .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            .NumDescriptors = 9, // 0～4 纹理，5～8 模糊用的两张纹理（各一个 SRV、一个 UAV）
            .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, // 着色器要从这个堆里读（RTV/DSV 堆不用，所以是 NONE）
            .NodeMask = 0,
        };
        ThrowIfFailed(mDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(mHeap.GetAddressOf())));
    }

    // 构建SRV
    void BuildShaderResourceViews()
    {
        auto& woodTex = mTextures.at("woodCrateTex")->Resource;
        D3D12_SHADER_RESOURCE_VIEW_DESC woodDesc = {
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
        mDevice->CreateShaderResourceView(woodTex.Get(), &woodDesc, mHeap->GetCPUDescriptorHandleForHeapStart());

        auto& WireFenceTex = mTextures.at("WireFenceTex")->Resource;
        D3D12_SHADER_RESOURCE_VIEW_DESC WireFenceDESC = {
            .Format = WireFenceTex->GetDesc().Format,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = {
                .MostDetailedMip = 0,
                .MipLevels = WireFenceTex->GetDesc().MipLevels,
                .PlaneSlice = 0,
                .ResourceMinLODClamp = 0.0f,
            },
        };
        CD3DX12_CPU_DESCRIPTOR_HANDLE fenceHandle(mHeap->GetCPUDescriptorHandleForHeapStart(), 1, mCSUDescriptorSize);
        mDevice->CreateShaderResourceView(WireFenceTex.Get(), &WireFenceDESC, fenceHandle);

        auto& iceTex = mTextures.at("iceTex")->Resource;
        D3D12_SHADER_RESOURCE_VIEW_DESC iceDesc = {
            .Format = iceTex->GetDesc().Format,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = {
                .MostDetailedMip = 0,
                .MipLevels = iceTex->GetDesc().MipLevels,
                .PlaneSlice = 0,
                .ResourceMinLODClamp = 0.0f,
            }
        };
        CD3DX12_CPU_DESCRIPTOR_HANDLE iceHandle(mHeap->GetCPUDescriptorHandleForHeapStart(), 2, mCSUDescriptorSize);
        mDevice->CreateShaderResourceView(iceTex.Get(), &iceDesc, iceHandle);

        auto& tileTex = mTextures.at("tileTex")->Resource;
        D3D12_SHADER_RESOURCE_VIEW_DESC tileDesc = {
            .Format = tileTex->GetDesc().Format,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = {
                .MostDetailedMip = 0,
                .MipLevels = tileTex->GetDesc().MipLevels,
                .PlaneSlice = 0,
                .ResourceMinLODClamp = 0.0f,
            }
        };
        CD3DX12_CPU_DESCRIPTOR_HANDLE tileHandle(mHeap->GetCPUDescriptorHandleForHeapStart(), 3, mCSUDescriptorSize);
        mDevice->CreateShaderResourceView(tileTex.Get(), &tileDesc, tileHandle);

        auto& treeArrayTex = mTextures.at("treeArrayTex")->Resource;
        D3D12_SHADER_RESOURCE_VIEW_DESC treeArrayDesc = {
            .Format = treeArrayTex->GetDesc().Format,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2DArray = {
                .MostDetailedMip = 0, //从最清晰的那级 mip 开始
                .MipLevels = treeArrayTex->GetDesc().MipLevels, //用几级 mip
                .FirstArraySlice = 0, //从第几张图开始
                .ArraySize = treeArrayTex->GetDesc().DepthOrArraySize, //一共几张（3）
                .PlaneSlice = 0,
                .ResourceMinLODClamp = 0.0f
            }
        };
        CD3DX12_CPU_DESCRIPTOR_HANDLE treeHandle(mHeap->GetCPUDescriptorHandleForHeapStart(), 4, mCSUDescriptorSize);
        mDevice->CreateShaderResourceView(treeArrayTex.Get(), &treeArrayDesc, treeHandle);
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

        // 镜子：12×10 的竖直矩形，中心在原点，正面朝 -z（从 -z 看过去四个角是顺时针）。
        // 接在盒子后面放进同一对缓冲，所以要记下盒子占了多少顶点、多少索引
        const UINT boxVertexCount = (UINT)vertices.size();
        const UINT boxIndexCount = (UINT)indices.size();
        vertices.push_back(Vertex{XMFLOAT3(-6.0f, 5.0f, 0.0f), XMFLOAT3(0.0f, 0.0f, -1.0f), XMFLOAT2(0.0f, 0.0f)});
        vertices.push_back(Vertex{XMFLOAT3(6.0f, 5.0f, 0.0f), XMFLOAT3(0.0f, 0.0f, -1.0f), XMFLOAT2(1.0f, 0.0f)});
        vertices.push_back(Vertex{XMFLOAT3(6.0f, -5.0f, 0.0f), XMFLOAT3(0.0f, 0.0f, -1.0f), XMFLOAT2(1.0f, 1.0f)});
        vertices.push_back(Vertex{XMFLOAT3(-6.0f, -5.0f, 0.0f), XMFLOAT3(0.0f, 0.0f, -1.0f), XMFLOAT2(0.0f, 1.0f)});
        for (int i : {0, 1, 2, 0, 2, 3}) // 左上 → 右上 → 右下，左上 → 右下 → 左下
        {
            indices.push_back((std::uint16_t)i); // 从 0 数，画的时候靠 BaseVertexLocation 偏到镜子的顶点
        }

        // 地板：22×26 的水平矩形，中心在原点，正面朝 +y（从上往下看四个角是顺时针）。
        // uv 超出 0～1，配合 Wrap 采样器让瓷砖纹理横向铺 4 遍、纵向铺 5 遍
        const UINT floorVertexStart = (UINT)vertices.size();
        const UINT floorIndexStart = (UINT)indices.size();
        vertices.push_back(Vertex{XMFLOAT3(-11.0f, 0.0f, 13.0f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 0.0f)});
        vertices.push_back(Vertex{XMFLOAT3(11.0f, 0.0f, 13.0f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(4.0f, 0.0f)});
        vertices.push_back(Vertex{XMFLOAT3(11.0f, 0.0f, -13.0f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(4.0f, 5.0f)});
        vertices.push_back(Vertex{XMFLOAT3(-11.0f, 0.0f, -13.0f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 5.0f)});
        for (int i : {0, 1, 2, 0, 2, 3})
        {
            indices.push_back((std::uint16_t)i);
        }

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

        DX_Utils::SubmeshGeometry submesh_Mirror;
        submesh_Mirror.IndexCount = 6;
        submesh_Mirror.StartIndexLocation = boxIndexCount; // 索引接在盒子的 36 个后面
        submesh_Mirror.BaseVertexLocation = (int)boxVertexCount; // 顶点接在盒子的 24 个后面
        mShapeGeo->DrawArgs["mirror"] = submesh_Mirror;

        DX_Utils::SubmeshGeometry submesh_Floor;
        submesh_Floor.IndexCount = 6;
        submesh_Floor.StartIndexLocation = floorIndexStart; // 接在镜子的 6 个索引后面
        submesh_Floor.BaseVertexLocation = (int)floorVertexStart; // 接在镜子的 4 个顶点后面
        mShapeGeo->DrawArgs["floor"] = submesh_Floor;
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

    // 公告牌树：24 棵，每棵只存一个点（中心 + 宽高）。顶点格式和木箱、陆地都不一样，所以单独一份几何体。
    // 位置挑在山坡上（地面比水面高出 2.5 以上，水会涨到 0.5～0.8），避开地板、铁丝网、镜子和那 100 个木箱；
    // 前 12 棵从初始机位能整棵看到，后 12 棵散在别处，走动时能看到
    void BuildTreeSpritesGeometry()
    {
        const XMFLOAT2 treeXZ[] = {
            {17.7f, 33.7f}, {63.9f, 63.4f}, {3.2f, 67.5f}, {9.4f, 48.6f}, {14.5f, 17.6f}, {12.5f, 63.1f},
            {25.6f, 55.2f}, {22.8f, 10.5f}, {70.6f, 50.2f}, {20.6f, 46.0f}, {4.3f, 31.9f}, {25.3f, 65.5f},
            {-34.7f, -38.3f}, {71.4f, -4.3f}, {48.5f, -3.4f}, {58.3f, 10.0f}, {-59.3f, 43.3f}, {-29.7f, 38.7f},
            {18.3f, -4.9f}, {-19.6f, -27.0f}, {51.2f, -47.6f}, {-53.3f, -30.0f}, {-21.3f, -43.6f}, {-71.3f, -51.3f},
        };
        const XMFLOAT2 treeSize = {13.0f, 16.0f}; // 树的图片是 208×256，宽高比 0.8125

        std::vector<TreeSpriteVertex> vertices;
        std::vector<std::uint16_t> indices;
        for (const XMFLOAT2& xz : treeXZ)
        {
            // 中心 = 地面高度 + 半高，再往下埋 0.5：坡上的树底边是水平的，不埋一点会有一角悬空
            const float y = GetHillsHeight(xz.x, xz.y) + 0.5f * treeSize.y - 0.5f;
            indices.push_back((std::uint16_t)vertices.size()); // 0, 1, 2, ……：DrawRenderItems 用的是带索引的画法
            vertices.push_back({XMFLOAT3(xz.x, y, xz.y), treeSize});
        }

        const UINT vbByteSize = (UINT)vertices.size() * sizeof(TreeSpriteVertex);
        const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

        mTreeGeo = std::make_unique<DX_Utils::MeshGeometry>();
        mTreeGeo->Name = "treeSpritesGeo";

        ThrowIfFailed(D3DCreateBlob(vbByteSize, &mTreeGeo->VertexBufferCPU));
        CopyMemory(mTreeGeo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);
        ThrowIfFailed(D3DCreateBlob(ibByteSize, &mTreeGeo->IndexBufferCPU));
        CopyMemory(mTreeGeo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

        mTreeGeo->VertexBufferGPU = CreateDefaultBuffer(mDevice.Get(), mCommandList.Get(),
                                                        vertices.data(), vbByteSize, mTreeGeo->VertexBufferUploader);
        mTreeGeo->IndexBufferGPU = CreateDefaultBuffer(mDevice.Get(), mCommandList.Get(),
                                                       indices.data(), ibByteSize, mTreeGeo->IndexBufferUploader);

        mTreeGeo->VertexByteStride = sizeof(TreeSpriteVertex);
        mTreeGeo->VertexBufferByteSize = vbByteSize;
        mTreeGeo->IndexFormat = DXGI_FORMAT_R16_UINT;
        mTreeGeo->IndexBufferByteSize = ibByteSize;

        DX_Utils::SubmeshGeometry submesh;
        submesh.IndexCount = (UINT)indices.size();
        submesh.StartIndexLocation = 0;
        submesh.BaseVertexLocation = 0;
        mTreeGeo->DrawArgs["points"] = submesh;
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

    //构建材质
    void BuildMaterials()
    {
        std::unique_ptr<Material> LMaterial_crate = std::make_unique<Material>();
        std::unique_ptr<Material> LMaterial_grass = std::make_unique<Material>();
        std::unique_ptr<Material> LMaterial_water = std::make_unique<Material>();
        std::unique_ptr<Material> LMaterial_fence = std::make_unique<Material>();
        std::unique_ptr<Material> LMaterial_mirror = std::make_unique<Material>();
        std::unique_ptr<Material> LMaterial_floor = std::make_unique<Material>();
        std::unique_ptr<Material> LMaterial_shadow = std::make_unique<Material>();
        std::unique_ptr<Material> LMaterial_fenceShadow = std::make_unique<Material>();
        std::unique_ptr<Material> LMaterial_tree = std::make_unique<Material>();

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
        LMaterial_water->DiffuseAlbedo = {0.0f, 0.2f, 0.6f, .5f};
        LMaterial_water->FresnelR0 = {0.1f, 0.1f, 0.1f};
        LMaterial_water->Roughness = 0;

        LMaterial_fence->Name = "fence";
        LMaterial_fence->MatCBIndex = 3;
        LMaterial_fence->DiffuseAlbedo = {1.0f, 1.0f, 1.0f, 1.0f};
        LMaterial_fence->FresnelR0 = {0.1f, 0.1f, 0.1f};
        LMaterial_fence->Roughness = 0.25f;
        LMaterial_fence->DiffuseSrvHeapIndex = 1; //材质里的一个编号，意思是"我这个材质的贴图，放在 SRV 堆的第几格"。

        LMaterial_mirror->Name = "mirror";
        LMaterial_mirror->MatCBIndex = 4;
        LMaterial_mirror->DiffuseAlbedo = {1.0f, 1.0f, 1.0f, .3f};
        LMaterial_mirror->FresnelR0 = {0.1f, 0.1f, 0.1f};
        LMaterial_mirror->Roughness = 0.5f;
        LMaterial_mirror->DiffuseSrvHeapIndex = 2;

        LMaterial_floor->Name = "floor";
        LMaterial_floor->MatCBIndex = 5;
        LMaterial_floor->DiffuseAlbedo = {.9f, .9f, .9f, 1.f};
        LMaterial_floor->FresnelR0 = {0.02f, 0.02f, 0.02f};
        LMaterial_floor->Roughness = 0.7f;
        LMaterial_floor->DiffuseSrvHeapIndex = 3;

        LMaterial_shadow->Name = "shadow";
        LMaterial_shadow->MatCBIndex = 6;
        LMaterial_shadow->DiffuseAlbedo = {.0f, .0f, .0f, .5f};
        LMaterial_shadow->FresnelR0 = {0.001f, 0.001f, 0.001f};
        LMaterial_shadow->Roughness = 0.f;
        LMaterial_shadow->DiffuseSrvHeapIndex = 0;

        LMaterial_fenceShadow->Name = "fenceShadow";
        LMaterial_fenceShadow->MatCBIndex = 7;
        LMaterial_fenceShadow->DiffuseAlbedo = {.0f, .0f, .0f, .5f};
        LMaterial_fenceShadow->FresnelR0 = {0.001f, 0.001f, 0.001f};
        LMaterial_fenceShadow->Roughness = 0.f;
        LMaterial_fenceShadow->DiffuseSrvHeapIndex = 1;

        LMaterial_tree->Name = "tree";
        LMaterial_tree->MatCBIndex = 8;
        LMaterial_tree->DiffuseAlbedo = {1.0f, 1.0f, 1.0f, 1.0f};
        LMaterial_tree->FresnelR0 = {0.01f, 0.01f, 0.01f};
        LMaterial_tree->Roughness = 0.125f;
        LMaterial_tree->DiffuseSrvHeapIndex = 4;

        mMaterials[LMaterial_crate->Name] = std::move(LMaterial_crate);
        mMaterials[LMaterial_grass->Name] = std::move(LMaterial_grass);
        mMaterials[LMaterial_water->Name] = std::move(LMaterial_water);
        mMaterials[LMaterial_fence->Name] = std::move(LMaterial_fence);
        mMaterials[LMaterial_mirror->Name] = std::move(LMaterial_mirror);
        mMaterials[LMaterial_floor->Name] = std::move(LMaterial_floor);
        mMaterials[LMaterial_shadow->Name] = std::move(LMaterial_shadow);
        mMaterials[LMaterial_fenceShadow->Name] = std::move(LMaterial_fenceShadow);
        mMaterials[LMaterial_tree->Name] = std::move(LMaterial_tree);
    }

    //构建mAllItems
    void BuildRenderItems()
    {
        UINT objCBIndex = 0;

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
        mRitemLayer[(int)RenderLayer::Opaque].push_back(land.get());
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
                mRitemLayer[(int)RenderLayer::Opaque].push_back(crate.get());
                mAllItems.push_back(std::move(crate));
            }
        }

        auto Lfence1 = std::make_unique<RenderItem>();
        Lfence1->PSO = mAlphaTestedPSO;
        Lfence1->ObjCBIndex = objCBIndex++;
        Lfence1->Geo = mShapeGeo.get();
        Lfence1->IndexCount = box.IndexCount;
        Lfence1->StartIndexLocation = mShapeGeo->DrawArgs["box"].StartIndexLocation;
        Lfence1->BaseVertexLocation = mShapeGeo->DrawArgs["box"].BaseVertexLocation;
        Lfence1->Mat = mMaterials.at("fence").get();
        XMStoreFloat4x4(&Lfence1->World, XMMatrixScaling(4, 4, 4) * XMMatrixTranslation(0, 4, -20));
        mRitemLayer[(int)RenderLayer::Opaque].push_back(Lfence1.get());
        mAllItems.push_back(std::move(Lfence1));

        // 镜子用的两个新木箱（边长 3），都在镜子前面：
        //   镜前木箱 —— 第 3 步让镜子照它；挡板木箱 —— 挡住镜子的左下角，验证"被挡住的那块镜子不标记"
        const XMMATRIX extraCrateWorlds[] = {
            XMMatrixScaling(1.5f, 1.5f, 1.5f) * XMMatrixTranslation(5.0f, 1.5f, -22.0f), // 镜前木箱
            XMMatrixScaling(1.5f, 1.5f, 1.5f) * XMMatrixTranslation(6.75f, 2.5f, -25.25f), // 挡板木箱
        };
        for (const XMMATRIX& world : extraCrateWorlds)
        {
            auto crate = std::make_unique<RenderItem>();
            crate->PSO = mCratePSO;
            XMStoreFloat4x4(&crate->World, world);
            crate->ObjCBIndex = objCBIndex++;
            crate->Geo = mShapeGeo.get();
            crate->IndexCount = box.IndexCount;
            crate->StartIndexLocation = box.StartIndexLocation;
            crate->BaseVertexLocation = box.BaseVertexLocation;
            crate->Mat = mMaterials.at("crate").get();
            mRitemLayer[(int)RenderLayer::Opaque].push_back(crate.get());
            mAllItems.push_back(std::move(crate));
        }

        //镜面

        const float yaw = 0.9608f;
        const float nx = -sinf(yaw), nz = -cosf(yaw);
        mMirrorPlane = XMFLOAT4(nx, 0.0f, nz, -(nx * 11.0f + nz * (-20.0f)));
        const XMMATRIX R = XMMatrixReflect(XMLoadFloat4(&mMirrorPlane));

        const auto& mirrorArgs = mShapeGeo->DrawArgs.at("mirror");
        auto mMarkMirror = std::make_unique<RenderItem>();
        XMStoreFloat4x4(&mMarkMirror->World, XMMatrixRotationY(0.9608f) * XMMatrixTranslation(11.0f, 5.0f, -20.0f));
        // 转 55°，面朝相机和铁丝网
        mMarkMirror->PSO = mMarkMirrorsPSO;
        mMarkMirror->ObjCBIndex = objCBIndex++;
        mMarkMirror->Geo = mShapeGeo.get();
        mMarkMirror->IndexCount = mirrorArgs.IndexCount;
        mMarkMirror->StartIndexLocation = mirrorArgs.StartIndexLocation;
        mMarkMirror->BaseVertexLocation = mirrorArgs.BaseVertexLocation;
        mMarkMirror->Mat = mMaterials.at("mirror").get();
        mRitemLayer[(int)RenderLayer::Mirrors].push_back(mMarkMirror.get());
        mAllItems.push_back(std::move(mMarkMirror));

        auto mMirrorClear = std::make_unique<RenderItem>();
        XMStoreFloat4x4(&mMirrorClear->World, XMMatrixRotationY(0.9608f) * XMMatrixTranslation(11.0f, 5.0f, -20.0f));
        mMirrorClear->PSO = mMirrorClearPSO;
        mMirrorClear->ObjCBIndex = objCBIndex++;
        mMirrorClear->Geo = mShapeGeo.get();
        mMirrorClear->IndexCount = mirrorArgs.IndexCount;
        mMirrorClear->StartIndexLocation = mirrorArgs.StartIndexLocation;
        mMirrorClear->BaseVertexLocation = mirrorArgs.BaseVertexLocation;
        mMirrorClear->Mat = mMaterials.at("mirror").get();
        mRitemLayer[(int)RenderLayer::MirrorClear].push_back(mMirrorClear.get());
        mAllItems.push_back(std::move(mMirrorClear));

        auto mMirror = std::make_unique<RenderItem>();
        XMStoreFloat4x4(&mMirror->World, XMMatrixRotationY(0.9608f) * XMMatrixTranslation(11.0f, 5.0f, -20.0f));
        mMirror->PSO = mMirrorPSO;
        mMirror->ObjCBIndex = objCBIndex++;
        mMirror->Geo = mShapeGeo.get();
        mMirror->IndexCount = mirrorArgs.IndexCount;
        mMirror->StartIndexLocation = mirrorArgs.StartIndexLocation;
        mMirror->BaseVertexLocation = mirrorArgs.BaseVertexLocation;
        mMirror->Mat = mMaterials.at("mirror").get();
        mRitemLayer[(int)RenderLayer::Transparent].push_back(mMirror.get());
        mAllItems.push_back(std::move(mMirror));

        auto LRefectFence1 = std::make_unique<RenderItem>();
        LRefectFence1->PSO = mReflectedFencePSO;
        LRefectFence1->ObjCBIndex = objCBIndex++;
        LRefectFence1->Geo = mShapeGeo.get();
        LRefectFence1->IndexCount = box.IndexCount;
        LRefectFence1->StartIndexLocation = mShapeGeo->DrawArgs["box"].StartIndexLocation;
        LRefectFence1->BaseVertexLocation = mShapeGeo->DrawArgs["box"].BaseVertexLocation;
        LRefectFence1->Mat = mMaterials.at("fence").get();
        XMStoreFloat4x4(&LRefectFence1->World, XMMatrixScaling(4, 4, 4) * XMMatrixTranslation(0, 4, -20) * R);
        mRitemLayer[(int)RenderLayer::Reflected].push_back(LRefectFence1.get());
        mAllItems.push_back(std::move(LRefectFence1));

        auto LReflectCrate1 = std::make_unique<RenderItem>();
        LReflectCrate1->PSO = mReflectedCratePSO;
        LReflectCrate1->ObjCBIndex = objCBIndex++;
        LReflectCrate1->Geo = mShapeGeo.get();
        LReflectCrate1->IndexCount = box.IndexCount;
        LReflectCrate1->StartIndexLocation = box.StartIndexLocation;
        LReflectCrate1->BaseVertexLocation = box.BaseVertexLocation;
        LReflectCrate1->Mat = mMaterials.at("crate").get();
        XMStoreFloat4x4(&LReflectCrate1->World, extraCrateWorlds[0] * R);
        mRitemLayer[(int)RenderLayer::Reflected].push_back(LReflectCrate1.get());
        mAllItems.push_back(std::move(LReflectCrate1));

        auto LReflectCrate2 = std::make_unique<RenderItem>();
        LReflectCrate2->PSO = mReflectedCratePSO;
        LReflectCrate2->ObjCBIndex = objCBIndex++;
        LReflectCrate2->Geo = mShapeGeo.get();
        LReflectCrate2->IndexCount = box.IndexCount;
        LReflectCrate2->StartIndexLocation = box.StartIndexLocation;
        LReflectCrate2->BaseVertexLocation = box.BaseVertexLocation;
        LReflectCrate2->Mat = mMaterials.at("crate").get();
        XMStoreFloat4x4(&LReflectCrate2->World, extraCrateWorlds[1] * R);
        mRitemLayer[(int)RenderLayer::Reflected].push_back(LReflectCrate2.get());
        mAllItems.push_back(std::move(LReflectCrate2));

        //水面
        auto water = std::make_unique<RenderItem>();
        XMStoreFloat4x4(&water->World, XMMatrixIdentity());
        water->PSO = mBlendPSO;
        water->ObjCBIndex = objCBIndex++;
        water->Geo = mWaterGeo.get();
        water->IndexCount = mWaterGeo->DrawArgs["grid"].IndexCount;
        water->StartIndexLocation = mWaterGeo->DrawArgs["grid"].StartIndexLocation;
        water->BaseVertexLocation = mWaterGeo->DrawArgs["grid"].BaseVertexLocation;
        water->Mat = mMaterials.at("water").get();
        mWavesItem = water.get();
        mRitemLayer[(int)RenderLayer::Transparent].push_back(mWavesItem);
        mAllItems.push_back(std::move(water));

        //地板瓷砖
        auto floor = std::make_unique<RenderItem>();
        auto propertyFloor = mShapeGeo->DrawArgs.at("floor");
        XMStoreFloat4x4(&floor->World, XMMatrixTranslation(1.0f, 0.05f, -27.0f));
        floor->PSO = mCratePSO;
        floor->ObjCBIndex = objCBIndex++;
        floor->Geo = mShapeGeo.get();
        floor->IndexCount = propertyFloor.IndexCount;
        floor->StartIndexLocation = propertyFloor.StartIndexLocation;
        floor->BaseVertexLocation = propertyFloor.BaseVertexLocation;
        floor->Mat = mMaterials.at("floor").get();
        mRitemLayer[(int)RenderLayer::Opaque].push_back(floor.get());
        mAllItems.push_back(std::move(floor));

        // 影子落在地板所在的平面 y = 0.05 上：n = (0,1,0)，d = -0.05（平面方程 n·p + d = 0）
        XMVECTOR shadowPlane = XMVectorSet(0.0f, 1.0f, 0.0f, -0.05f);
        // 第二个参数要的是"指向光源"的方向，正好和 mSunDir 反过来。XMLoadFloat3 读进来 w = 0，表示平行光
        XMVECTOR toLight = XMVectorNegate(XMLoadFloat3(&mSunDir));
        XMMATRIX S = XMMatrixShadow(shadowPlane, toLight);
        XMMATRIX lift = XMMatrixTranslation(0.0f, 0.001f, 0.0f); // 抬高一点点，防止和地板深度打架

        auto FenceShadow = std::make_unique<RenderItem>();
        XMStoreFloat4x4(&FenceShadow->World, XMMatrixScaling(4, 4, 4) * XMMatrixTranslation(0, 4, -20) * S * lift);
        FenceShadow->PSO = mFenceShadowPSO.Get();
        FenceShadow->ObjCBIndex = objCBIndex++;
        FenceShadow->Geo = mShapeGeo.get();
        FenceShadow->IndexCount = box.IndexCount;
        FenceShadow->StartIndexLocation = box.StartIndexLocation;
        FenceShadow->BaseVertexLocation = box.BaseVertexLocation;
        FenceShadow->Mat = mMaterials.at("fenceShadow").get();
        mRitemLayer[(int)RenderLayer::Shadow].push_back(FenceShadow.get());
        mAllItems.push_back(std::move(FenceShadow));

        auto boxShadow1 = std::make_unique<RenderItem>();
        XMStoreFloat4x4(&boxShadow1->World, extraCrateWorlds[0] * S * lift);
        boxShadow1->PSO = mShadowPSO;
        boxShadow1->ObjCBIndex = objCBIndex++;
        boxShadow1->Geo = mShapeGeo.get();
        boxShadow1->IndexCount = box.IndexCount;
        boxShadow1->StartIndexLocation = box.StartIndexLocation;
        boxShadow1->BaseVertexLocation = box.BaseVertexLocation;
        boxShadow1->Mat = mMaterials.at("shadow").get();
        mRitemLayer[(int)RenderLayer::Shadow].push_back(boxShadow1.get());
        mAllItems.push_back(std::move(boxShadow1));

        auto boxShadow2 = std::make_unique<RenderItem>();
        XMStoreFloat4x4(&boxShadow2->World, extraCrateWorlds[1] * S * lift);
        boxShadow2->PSO = mShadowPSO;
        boxShadow2->ObjCBIndex = objCBIndex++;
        boxShadow2->Geo = mShapeGeo.get();
        boxShadow2->IndexCount = box.IndexCount;
        boxShadow2->StartIndexLocation = box.StartIndexLocation;
        boxShadow2->BaseVertexLocation = box.BaseVertexLocation;
        boxShadow2->Mat = mMaterials.at("shadow").get();
        mRitemLayer[(int)RenderLayer::Shadow].push_back(boxShadow2.get());
        mAllItems.push_back(std::move(boxShadow2));

        auto tree = std::make_unique<RenderItem>();
        auto LGeo = mTreeGeo.get();
        tree->PSO = mTreePSO;
        tree->ObjCBIndex = objCBIndex++;
        tree->Geo = LGeo;
        tree->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        tree->IndexCount = LGeo->DrawArgs.at("points").IndexCount;
        tree->StartIndexLocation = LGeo->DrawArgs.at("points").StartIndexLocation;
        tree->BaseVertexLocation = LGeo->DrawArgs.at("points").BaseVertexLocation;
        tree->Mat = mMaterials.at("tree").get();
        mRitemLayer[(int)RenderLayer::Opaque].push_back(tree.get());
        mAllItems.push_back(std::move(tree));
    }

    // 3 份帧资源，每份：1 个 Pass 常量 + 每个渲染项 1 个物体常量
    void BuildFrameResources()
    {
        for (int i = 0; i < gNumFrameResources; i++)
        {
            mFrameResources.push_back(
                std::make_unique<FrameResource>(
                    mDevice.Get(),
                    2,
                    (UINT)mAllItems.size(),
                    (UINT)mWaves->VertexCount(),
                    (UINT)mMaterials.size()));
        }
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

    //构建 Blur根签名
    void BuildBlurRootSignature()
    {
        CD3DX12_ROOT_PARAMETER slotRootParameter[3];
        slotRootParameter[0].InitAsConstants(12, 0); //InitAsConstants(12, 0)：12 个 32 位根常量	BlurSettings : register(b0)
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); //描述符表，里面 1 个 SRV，从 t0 开始	gInput : register(t0)
        slotRootParameter[1].InitAsDescriptorTable(1, &srvRange);

        CD3DX12_DESCRIPTOR_RANGE uavRange;
        uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0); //描述符表，里面 1 个 UAV，从 u0 开始	gOutput : register(u0)
        slotRootParameter[2].InitAsDescriptorTable(1, &uavRange);

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
            _countof(slotRootParameter), slotRootParameter,
            0, nullptr, // 不用采样器：着色器里是按下标直接读像素
            D3D12_ROOT_SIGNATURE_FLAG_NONE); // 计算着色器不经过输入装配器

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
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mBlurRootSignature)));
    }

    //编译着色器
    void BuildShadersAndInputLayout()
    {
        // 灯的数量用宏传给着色器，顺序必须和 UpdateMainPassCB 里填灯的顺序一致：先平行光，再点光源，最后聚光灯
        const D3D_SHADER_MACRO lightDefines[] = {
            {"NUM_DIR_LIGHTS", "1"},
            {"NUM_POINT_LIGHTS", "1"},
            {"NUM_SPOT_LIGHTS", "1"},
            {"FOG", "1"},
            {nullptr, nullptr}
        };

        const D3D_SHADER_MACRO alphaTestDefines[] = {
            {"NUM_DIR_LIGHTS", "1"},
            {"NUM_POINT_LIGHTS", "1"},
            {"NUM_SPOT_LIGHTS", "1"},
            {"FOG", "1"},
            {"ALPHA_TEST", "1"},
            {nullptr, nullptr}
        };

        // 用到了 ConstantBuffer<T>，需要 Shader Model 5.1
        mCrateVSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", lightDefines, "VS", "vs_5_1");
        mCratePSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", lightDefines, "PS", "ps_5_1");
        mAlphaTestedPSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", alphaTestDefines, "PS", "ps_5_1");
        mMirrorVSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", lightDefines, "VS_MirrorClear", "vs_5_1");
        mMirrorPSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", lightDefines, "PS_MirrorClear", "ps_5_1");
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

        mTreeVSByteCode = CompileShader(L"HLSL\\TreeSprite.hlsl", alphaTestDefines, "VS", "vs_5_1");
        mTreePSByteCode = CompileShader(L"HLSL\\TreeSprite.hlsl", alphaTestDefines, "PS", "ps_5_1");
        mTreeGSByteCode = CompileShader(L"HLSL\\TreeSprite.hlsl", alphaTestDefines, "GS", "gs_5_1");

        mTreeInputLayout = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"SIZE", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        mHorzBlurCSByteCode = CompileShader(L"HLSL\\Blur.hlsl", nullptr, "HorzBlurCS", "cs_5_1");
        mVertBlurCSByteCode = CompileShader(L"HLSL\\Blur.hlsl", nullptr, "VertBlurCS", "cs_5_1");
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

        D3D12_RENDER_TARGET_BLEND_DESC alpha = {};
        alpha.BlendEnable = true; // 开常规混合（和 LogicOpEnable 只能开一个）
        alpha.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        alpha.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        alpha.BlendOp = D3D12_BLEND_OP_ADD;
        alpha.SrcBlendAlpha = D3D12_BLEND_ONE;
        alpha.DestBlendAlpha = D3D12_BLEND_ZERO;
        alpha.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        alpha.LogicOp = D3D12_LOGIC_OP_NOOP;
        alpha.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        auto LMarkMirrorsPSOdesc = psoDesc; //标记镜子
        auto LMirrorClearPSOdesc = psoDesc; //镜子重置
        auto LMirrorPSOdesc = psoDesc; //半透明镜子
        LMarkMirrorsPSOdesc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
        LMarkMirrorsPSOdesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        LMarkMirrorsPSOdesc.DepthStencilState.StencilEnable = true;
        LMarkMirrorsPSOdesc.DepthStencilState.FrontFace = {
            .StencilFailOp = D3D12_STENCIL_OP_KEEP,
            .StencilDepthFailOp = D3D12_STENCIL_OP_KEEP,
            .StencilPassOp = D3D12_STENCIL_OP_REPLACE,
            .StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS
        };
        LMarkMirrorsPSOdesc.DepthStencilState.BackFace = LMarkMirrorsPSOdesc.DepthStencilState.FrontFace;
        LMirrorClearPSOdesc.VS = {mMirrorVSByteCode->GetBufferPointer(), mMirrorVSByteCode->GetBufferSize()};
        LMirrorClearPSOdesc.PS = {mMirrorPSByteCode->GetBufferPointer(), mMirrorPSByteCode->GetBufferSize()};
        LMirrorClearPSOdesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        LMirrorClearPSOdesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        LMirrorClearPSOdesc.DepthStencilState.StencilEnable = true;
        LMirrorClearPSOdesc.DepthStencilState.FrontFace = {
            .StencilFailOp = D3D12_STENCIL_OP_KEEP,
            .StencilDepthFailOp = D3D12_STENCIL_OP_KEEP,
            .StencilPassOp = D3D12_STENCIL_OP_KEEP,
            .StencilFunc = D3D12_COMPARISON_FUNC_EQUAL
        };
        LMirrorClearPSOdesc.DepthStencilState.BackFace = LMirrorClearPSOdesc.DepthStencilState.FrontFace;
        LMirrorPSOdesc.BlendState.RenderTarget[0] = alpha;
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&LMarkMirrorsPSOdesc,IID_PPV_ARGS(&mMarkMirrorsPSO)));
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&LMirrorClearPSOdesc,IID_PPV_ARGS(&mMirrorClearPSO)));
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&LMirrorPSOdesc,IID_PPV_ARGS(&mMirrorPSO)));

        //AlphaTest
        auto fenceDesc = psoDesc;
        fenceDesc.PS = {mAlphaTestedPSByteCode->GetBufferPointer(), mAlphaTestedPSByteCode->GetBufferSize()};
        fenceDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&fenceDesc,IID_PPV_ARGS(&mAlphaTestedPSO)));

        auto mShadowPSODesc = psoDesc;
        auto mFenceShadowPSODesc = fenceDesc;

        mShadowPSODesc.BlendState.RenderTarget[0] = alpha;
        mShadowPSODesc.DepthStencilState.StencilEnable = true;
        mShadowPSODesc.DepthStencilState.FrontFace = {
            .StencilFailOp = D3D12_STENCIL_OP_KEEP,
            .StencilDepthFailOp = D3D12_STENCIL_OP_KEEP,
            .StencilPassOp = D3D12_STENCIL_OP_INCR,
            .StencilFunc = D3D12_COMPARISON_FUNC_EQUAL
        };
        mShadowPSODesc.DepthStencilState.BackFace = mShadowPSODesc.DepthStencilState.FrontFace;

        mFenceShadowPSODesc.BlendState.RenderTarget[0] = alpha;
        mFenceShadowPSODesc.DepthStencilState.StencilEnable = true;
        mFenceShadowPSODesc.DepthStencilState.FrontFace = {
            .StencilFailOp = D3D12_STENCIL_OP_KEEP,
            .StencilDepthFailOp = D3D12_STENCIL_OP_KEEP,
            .StencilPassOp = D3D12_STENCIL_OP_INCR,
            .StencilFunc = D3D12_COMPARISON_FUNC_EQUAL
        };
        mFenceShadowPSODesc.DepthStencilState.BackFace = mFenceShadowPSODesc.DepthStencilState.FrontFace;

        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&mShadowPSODesc,IID_PPV_ARGS(&mShadowPSO)));
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&mFenceShadowPSODesc,IID_PPV_ARGS(&mFenceShadowPSO)));

        auto LReflectedCratePSOdesc = psoDesc;
        auto LReflectedFencePSOdesc = fenceDesc;

        LReflectedCratePSOdesc.DepthStencilState.StencilEnable = true;
        LReflectedCratePSOdesc.DepthStencilState.FrontFace = {
            .StencilFailOp = D3D12_STENCIL_OP_KEEP,
            .StencilDepthFailOp = D3D12_STENCIL_OP_KEEP,
            .StencilPassOp = D3D12_STENCIL_OP_KEEP,
            .StencilFunc = D3D12_COMPARISON_FUNC_EQUAL
        };
        LReflectedCratePSOdesc.DepthStencilState.BackFace = LReflectedCratePSOdesc.DepthStencilState.FrontFace;
        LReflectedCratePSOdesc.RasterizerState.FrontCounterClockwise = true; // 照镜子后顺时针变逆时针

        LReflectedFencePSOdesc.DepthStencilState.StencilEnable = true;
        LReflectedFencePSOdesc.DepthStencilState.FrontFace = {
            .StencilFailOp = D3D12_STENCIL_OP_KEEP,
            .StencilDepthFailOp = D3D12_STENCIL_OP_KEEP,
            .StencilPassOp = D3D12_STENCIL_OP_KEEP,
            .StencilFunc = D3D12_COMPARISON_FUNC_EQUAL
        };
        LReflectedFencePSOdesc.DepthStencilState.BackFace = LReflectedFencePSOdesc.DepthStencilState.FrontFace;
        LReflectedFencePSOdesc.RasterizerState.FrontCounterClockwise = true;
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&LReflectedCratePSOdesc,IID_PPV_ARGS(&mReflectedCratePSO)));
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&LReflectedFencePSOdesc,IID_PPV_ARGS(&mReflectedFencePSO)));

        auto LTreePSODesc = psoDesc;
        LTreePSODesc.InputLayout = {mTreeInputLayout.data(), (UINT)mTreeInputLayout.size()};
        LTreePSODesc.VS = {mTreeVSByteCode->GetBufferPointer(), mTreeVSByteCode->GetBufferSize()};
        LTreePSODesc.PS = {mTreePSByteCode->GetBufferPointer(), mTreePSByteCode->GetBufferSize()};
        LTreePSODesc.GS = {mTreeGSByteCode->GetBufferPointer(), mTreeGSByteCode->GetBufferSize()};
        LTreePSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        LTreePSODesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&LTreePSODesc,IID_PPV_ARGS(&mTreePSO)));

        // 陆地和水面：位置 + 顶点色，PS 直接返回颜色
        psoDesc.InputLayout = {mColorInputLayout.data(), (UINT)mColorInputLayout.size()};
        psoDesc.VS = {mColorVSByteCode->GetBufferPointer(), mColorVSByteCode->GetBufferSize()};
        psoDesc.PS = {mColorPSByteCode->GetBufferPointer(), mColorPSByteCode->GetBufferSize()};
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&psoDesc,IID_PPV_ARGS(&mColorPSO)));

        psoDesc.BlendState.RenderTarget[0] = alpha;
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&psoDesc,IID_PPV_ARGS(&mBlendPSO)));

        //Horz
        D3D12_COMPUTE_PIPELINE_STATE_DESC GusBlurDesc = {
            .pRootSignature = mBlurRootSignature.Get(),
            .CS = {mHorzBlurCSByteCode->GetBufferPointer(), mHorzBlurCSByteCode->GetBufferSize()},
            .Flags = D3D12_PIPELINE_STATE_FLAG_NONE
        };
        ThrowIfFailed(mDevice->CreateComputePipelineState(&GusBlurDesc,IID_PPV_ARGS(&mHorzBlurPSO)));
        //Vert
        GusBlurDesc.CS = {mVertBlurCSByteCode->GetBufferPointer(), mVertBlurCSByteCode->GetBufferSize()};
        ThrowIfFailed(mDevice->CreateComputePipelineState(&GusBlurDesc,IID_PPV_ARGS(&mVertBlurPSO)));
    }

    // 高斯权重：G(x) = exp(-x² / 2σ²)，x 从 -半径 到 +半径，最后除以总和，让它们加起来等于 1。
    // 半径取 ceil(2σ)，再往外的权重已经很小了。Blur.hlsl 最多支持半径 5（11 个权重），所以 σ 不能超过 2.5
    std::vector<float> CalcGaussWeights(float sigma)
    {
        const int blurRadius = (int)ceilf(2.0f * sigma);
        std::vector<float> weights(2 * blurRadius + 1);

        float sum = 0.0f;
        for (int i = -blurRadius; i <= blurRadius; i++)
        {
            weights[i + blurRadius] = expf(-(float)(i * i) / (2.0f * sigma * sigma));
            sum += weights[i + blurRadius];
        }
        for (float& w : weights)
        {
            w /= sum; // 不除的话总和是 2.48（σ = 1）这种数，画面会整体变亮
        }
        return weights;
    }

    // ======================== 成员变量 ========================

    // 太阳光前进的方向：从后上方斜着照下来，抬高约 33°（原来约 8.5°，影子会拖到相机后面）
    XMFLOAT3 mSunDir = {0.2494f, -0.5486f, -0.7980f};

    // 资源：纹理、几何体、渲染项
    std::unordered_map<std::string, std::unique_ptr<DX_Utils::Texture>> mTextures;
    // 材质
    std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
    // 木箱：位置 + 贴图坐标，静态数据，放在默认堆里
    std::unique_ptr<DX_Utils::MeshGeometry> mShapeGeo;
    // 陆地：位置 + 顶点色，静态数据，放在默认堆里
    std::unique_ptr<DX_Utils::MeshGeometry> mLandGeo;
    std::unique_ptr<DX_Utils::MeshGeometry> mWaterGeo;
    // 公告牌树：每棵一个点（中心 + 宽高），静态数据，放在默认堆里
    std::unique_ptr<DX_Utils::MeshGeometry> mTreeGeo;
    // 后期模糊：A = mBlurMap[0]（场景复制进来，最终结果也在它里面），B = mBlurMap[1]（存横向模糊的结果）
    ComPtr<ID3D12Resource> mBlurMap[2];
    bool mBlurEnabled = true; // B 键开关
    bool mBlurKeyWasDown = false;
    int mBlurCount = 4; // "横向 + 纵向"算一次，重复几次。越多越糊
    // 波浪：CPU 端的模拟。只算高度和法线，不碰任何 D3D 资源
    std::unique_ptr<Waves> mWaves;
    std::vector<std::unique_ptr<RenderItem>> mAllItems;
    std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];
    RenderItem* mWavesItem = nullptr;

    // 帧资源：3 份轮流用，CPU 最多领先 GPU 3 帧
    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrentFrame = nullptr; // 当前这一帧用的那一份
    //这一帧用的是第几份帧资源
    int mCurrFrameResourceIndex = 0;

    // 管线：描述符堆、根签名、着色器、输入布局、PSO（两套：木箱一套，陆地/水面共用一套）
    ComPtr<ID3D12DescriptorHeap> mHeap; // 着色器可见的 CBV/SRV/UAV 堆（现在只有 1 个 SRV）
    ComPtr<ID3D12RootSignature> mRootSignature;
    ComPtr<ID3D12PipelineState> mCratePSO;
    ComPtr<ID3D12PipelineState> mAlphaTestedPSO;

    ComPtr<ID3DBlob> mCrateVSByteCode;
    ComPtr<ID3DBlob> mCratePSByteCode;
    ComPtr<ID3DBlob> mAlphaTestedPSByteCode;
    ComPtr<ID3DBlob> mMirrorVSByteCode;
    ComPtr<ID3DBlob> mMirrorPSByteCode;
    std::vector<D3D12_INPUT_ELEMENT_DESC> mCrateInputLayout;
    ComPtr<ID3D12PipelineState> mColorPSO;
    ComPtr<ID3DBlob> mColorVSByteCode;
    ComPtr<ID3DBlob> mColorPSByteCode;
    std::vector<D3D12_INPUT_ELEMENT_DESC> mColorInputLayout;
    ComPtr<ID3D12PipelineState> mBlendPSO;
    ComPtr<ID3D12PipelineState> mMarkMirrorsPSO;
    ComPtr<ID3D12PipelineState> mMirrorClearPSO;
    ComPtr<ID3D12PipelineState> mMirrorPSO;
    ComPtr<ID3D12PipelineState> mReflectedCratePSO;
    ComPtr<ID3D12PipelineState> mReflectedFencePSO;
    ComPtr<ID3D12PipelineState> mShadowPSO;
    ComPtr<ID3D12PipelineState> mFenceShadowPSO;

    ComPtr<ID3DBlob> mTreeVSByteCode;
    ComPtr<ID3DBlob> mTreePSByteCode;
    ComPtr<ID3DBlob> mTreeGSByteCode;
    std::vector<D3D12_INPUT_ELEMENT_DESC> mTreeInputLayout;
    ComPtr<ID3D12PipelineState> mTreePSO;
    XMFLOAT4 mMirrorPlane;

    // 相机：绕原点转的球坐标 (θ, φ, r)
    Camera mCamera;
    POINT mLastMousePos = {};

    // 后期模糊的根签名、两个计算着色器和两个计算 PSO
    ComPtr<ID3D12RootSignature> mBlurRootSignature;
    ComPtr<ID3DBlob> mHorzBlurCSByteCode;
    ComPtr<ID3DBlob> mVertBlurCSByteCode;
    ComPtr<ID3D12PipelineState> mHorzBlurPSO;
    ComPtr<ID3D12PipelineState> mVertBlurPSO;

    //时间戳
    ComPtr<ID3D12QueryHeap> mTimestampHeap;
    ComPtr<ID3D12Resource> mTimestampReadback;
    UINT64 mTimestampFreq = 1; // 初值给 1，免得读数之前不小心除以 0
    double mPassMsSum[kTimestampCount - 1] = {}; // 5 段各自累加的毫秒数（不透明、镜子、影子、透明、模糊）
    int mPassSamples = 0;                        // 已经攒了几帧
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
