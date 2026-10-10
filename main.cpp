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
#include <filesystem>       // 热重载：查 HLSL 文件的修改时间

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

// imgui_impl_win32.h 里故意没声明这个函数（免得把 windows.h 拖进来），要自己声明
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

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
    XMFLOAT3 TangentU; // 切线 T：沿纹理 U 增大的方向（物体空间）
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

    // 指向 mXxxPSO 里那根裸指针：热重载换的是槽位里的 PSO，渲染项不用改就跟着换
    ID3D12PipelineState* const* PSO = nullptr;
    // 画阴影图时用的 PSO（只写深度）。空 = 这个物体不投影子
    ID3D12PipelineState* const* ShadowPSO = nullptr;
    // 延迟渲染 G-Buffer Pass 用的 PSO（只写材质和法线）。空 = 不走延迟（透明物体、天空、镜子……）
    ID3D12PipelineState* const* GbufferPSO = nullptr;

    Material* Mat = nullptr;
};

enum class RenderLayer : int
{
    Opaque = 0,
    Mirrors,
    MirrorClear,
    Reflected,
    Shadow,
    Transparent,
    Sky,
    TestSpheres, // PBR 测试球：面板上勾选才画
    Count
};

// DrawRenderItems 这一趟画的是哪个 Pass：决定用渲染项的哪个 PSO
enum class DrawPass
{
    Main, // 前向：边画边算光
    Shadow, // 阴影图：只写深度
    GBuffer // 延迟：只写材质和法线
};

static constexpr UINT kTimestampCount = 10;
static constexpr UINT kShadowMapSize = 4096;

//场景先画进这张 HDR 纹理，每个通道 16 位浮点，亮度可以超过 1，最后再压到后台缓冲
static constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
// 0：基础色 + 金属度；1：世界法线 + 粗糙度
static constexpr DXGI_FORMAT kGBuffer0Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; //RGB：基础色，A：金属度
static constexpr DXGI_FORMAT kGBuffer1Format = DXGI_FORMAT_R16G16B16A16_FLOAT; //XYZ：世界法线，W：粗糙度
// SSAO 的遮蔽图：每个像素只存一个 0～1 的数，一个 8 位通道就够（1 字节）
static constexpr DXGI_FORMAT kAoFormat = DXGI_FORMAT_R8_UNORM;
// 延迟第③步：面板勾"64 盏小灯"时多出来的点光源，排在原来 3 盏后面（Lights[3..66]，MaxLights = 100 够用）
static constexpr UINT kExtraLights = 64;

// 录 demo 用的 6 个机位（Day18 的参考镜头），ImGui 面板里的下拉框选
struct CameraPreset
{
    const char* Name;
    XMFLOAT3 Eye; // 眼睛
    XMFLOAT3 Target; // 看向
};

static const CameraPreset kCameraPresets[] = {
    {"1 开场全景", {34.0f, 5.0f, -44.0f}, {-4.0f, 2.0f, -20.0f}},
    {"2 水面与倒影", {35.0f, 14.0f, -62.0f}, {0.0f, 2.0f, -22.0f}},
    {"3 铁丝网、镜子和木箱", {-14.0f, 4.0f, -46.0f}, {12.0f, 3.0f, -18.0f}},
    {"4 镜子特写", {-2.0f, 6.0f, -32.0f}, {11.0f, 4.5f, -20.0f}},
    {"5 山坡、树影和雾", {-30.0f, 22.0f, -75.0f}, {8.0f, 2.0f, -20.0f}},
    {"6 法线贴图地板", {6.0f, 2.5f, -42.0f}, {-2.0f, 0.0f, -18.0f}},
    {"7 PBR 测试球", {19.0f, 4.0f, -19.0f}, {19.0f, 1.0f, -34.0f}},
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
            if (ImGui::GetCurrentContext() != nullptr)
            {
                ImGui_ImplDX12_Shutdown();
                ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
            }
        }
    }

    // 按 DPI 缩放 UI。ScaleAllSizes 是"乘上去"的，所以先恢复成默认样式再乘，
    // 否则每换一次屏幕就叠乘一次（200% → 100% → 200% 来回一趟，边距从 16 变成 32）
    void ScaleImGui()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style = ImGuiStyle(); // 默认样式（构造函数里就是 Dark 配色）
        style.ScaleAllSizes(mDpiScale); // 边距、圆角、滚动条这些尺寸 × 缩放
        style.FontScaleDpi = mDpiScale; // 字号 × 缩放（1.92 的字体按需要的字号现烤，放大了也清楚）
    }

    void OnDpiChanged() override
    {
        ScaleImGui();
    }

    void InitImGui()
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        ScaleImGui();
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 16.0f);
        ImGui_ImplWin32_Init(mhMainWnd);

        for (int i = kImGuiFirstSlot + kImGuiSlotCount - 1; i >= kImGuiFirstSlot; i--)
        {
            mImGuiFreeSlots.push_back(i);
        }
        ImGui_ImplDX12_InitInfo init;
        init.Device = mDevice.Get();
        init.CommandQueue = mCommandQueue.Get(); // 上传字体纹理时用
        init.NumFramesInFlight = gNumFrameResources; // ImGui 自己的顶点/索引缓冲也要轮换几份
        init.RTVFormat = mBackBufferFormat;
        init.DSVFormat = DXGI_FORMAT_UNKNOWN; //UI 不用深度
        init.SrvDescriptorHeap = mHeap.Get();
        init.UserData = this; // 回调是普通函数指针，拿不到 this，借 UserData 传进去
        init.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                       D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
        {
            auto* app = static_cast<CrateApp*>(info->UserData);
            const int slot = app->mImGuiFreeSlots.back();
            app->mImGuiFreeSlots.pop_back();
            *cpu = app->HeapCpuHandle(slot);
            *gpu = app->HeapGpuHandle(slot);
        };
        init.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                      D3D12_GPU_DESCRIPTOR_HANDLE)
        {
            auto* app = static_cast<CrateApp*>(info->UserData);
            const int slot = (int)((cpu.ptr - app->mHeap->GetCPUDescriptorHandleForHeapStart().ptr)
                / app->mCSUDescriptorSize);
            app->mImGuiFreeSlots.push_back(slot);
        };
        ImGui_ImplDX12_Init(&init);

        // 滑块初值从现在的 mSunDir 反算（指向太阳 = -mSunDir）
        const XMFLOAT3 toSun = {-mSunDir.x, -mSunDir.y, -mSunDir.z};
        mSunElevation = XMConvertToDegrees(asinf(toSun.y)); // 仰角：y = sin(仰角)
        mSunAzimuth = XMConvertToDegrees(atan2f(toSun.x, toSun.z)); // 方位：从 +z 往 +x 转
    }

    bool Initialize() override
    {
        if (!D3DApp::Initialize())
        {
            return false;
        }

        D3D12_FEATURE_DATA_SHADER_MODEL sm = {D3D_SHADER_MODEL_6_6};
        ThrowIfFailed(mDevice->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)));
        if (sm.HighestShaderModel < D3D_SHADER_MODEL_6_6)
        {
            MessageBox(nullptr, L"显卡不支持 Shader Model 6.6", L"错误", MB_OK);
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
        BuildHdrResources();
        BuildBloomResources();
        BuildGBufferResources();
        BuildTimestampQueries(); //Pass时间记录
        BuildShadowMap(); //构建阴影贴图资源和View
        BuildShapeGeometry(); // 录制复制命令（木箱）
        BuildLandGeometry(); // 录制复制命令（陆地的顶点/索引）
        BuildWaves(); // 波浪模拟（这一步只建模拟本体，第 5 步再建水面几何）
        BuildTreeSpritesGeometry(); // 录制复制命令（树的点）
        BuildShaderResourceViews(); // 要用到纹理和堆
        BuildRootSignature(); // PSO 要用根签名
        BuildBlurRootSignature(); //模糊专用的根签名
        BuildTonemapRootSignature();
        BuildBloomRootSignature();
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

        InitImGui();

        return true;
    }

protected:
    // ======================== D3DApp 的虚函数 ========================

    // 消息先给 ImGui：鼠标在 UI 上、或者正在往输入框里打字时，ImGui 要先拿到这条消息
    LRESULT MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override
    {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        {
            return true;
        }
        return D3DApp::MsgProc(hWnd, msg, wParam, lParam); // 剩下的照旧交给框架：缩放、暂停、鼠标转视角……
    }

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
            BuildHdrResources();
            BuildBloomResources();
            BuildGBufferResources();
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

    // 阴影图：阴影 Pass 当深度缓冲写（DSV），主 Pass 当纹理读（SRV）。约定：帧与帧之间停在 PIXEL_SHADER_RESOURCE
    void BuildShadowMap()
    {
        // 资源：一份资源两种看法（DSV 按 D24S8 写，SRV 按 R24 读），所以用"无类型"的 R24G8_TYPELESS
        D3D12_RESOURCE_DESC ShadowPassDesc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            .Alignment = 0,
            .Width = kShadowMapSize,
            .Height = kShadowMapSize,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = DXGI_FORMAT_R24G8_TYPELESS,
            .SampleDesc = {.Count = 1, .Quality = 0},
            .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
            .Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
        };
        // 优化清除值：每帧都按这个值清（深度 1.0），格式写 DSV 的格式
        D3D12_CLEAR_VALUE optClear = {
            .Format = DXGI_FORMAT_D24_UNORM_S8_UINT,
            .DepthStencil = {
                .Depth = 1.0f,
                .Stencil = 0
            }
        };
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(mDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &ShadowPassDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &optClear,
            IID_PPV_ARGS(&mShadowMap)));

        // DSV：框架的 DSV 堆只有 1 格（屏幕的深度缓冲占着），自己建一个 1 格的小堆
        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {
            .Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
            .NumDescriptors = 1,
            .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
            .NodeMask = 0,
        };
        ThrowIfFailed(mDevice->CreateDescriptorHeap(&dsvHeapDesc,IID_PPV_ARGS(&mShadowDsvHeap)));
        // D3DApp 建屏幕深度缓冲的 DSV 时传的是 nullptr：那块资源本身就是有类型的 D24S8。
        // 阴影图是无类型的，必须告诉 DSV "按什么格式写"
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {
            .Format = DXGI_FORMAT_D24_UNORM_S8_UINT,
            .ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D,
            .Flags = D3D12_DSV_FLAG_NONE,
            .Texture2D = {.MipSlice = 0},
        };
        mDevice->CreateDepthStencilView(mShadowMap.Get(), &dsvDesc,
                                        mShadowDsvHeap->GetCPUDescriptorHandleForHeapStart());
        // SRV：放在大堆第 9 格，只读前 24 位深度，后 8 位模板不要
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {
            .Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = {.MostDetailedMip = 0, .MipLevels = 1},
        };
        CD3DX12_CPU_DESCRIPTOR_HANDLE srv(mHeap->GetCPUDescriptorHandleForHeapStart(), 9, mCSUDescriptorSize);
        mDevice->CreateShaderResourceView(mShadowMap.Get(), &srvDesc, srv);
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
            mCommandList->SetComputeRootDescriptorTable(2, HeapGpuHandle(8)); // 根参数 2（输出）= 堆第 8 格：B 的 UAV
            mCommandList->Dispatch(groupsX, mClientHeight, 1); // 每个线程组管一行里的 256 个像素

            // 交换：A 变成可写，B 变成可读
            D3D12_RESOURCE_BARRIER s1[] = {
                CD3DX12_RESOURCE_BARRIER::Transition(A, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                CD3DX12_RESOURCE_BARRIER::Transition(B, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            };
            mCommandList->ResourceBarrier(2, s1);

            // ③b 纵向：读 B，写 A
            mCommandList->SetPipelineState(mVertBlurPSO.Get());
            mCommandList->SetComputeRootDescriptorTable(1, HeapGpuHandle(7)); // 输入 = 堆第 7 格：B 的 SRV
            mCommandList->SetComputeRootDescriptorTable(2, HeapGpuHandle(6)); // 输出 = 堆第 6 格：A 的 UAV
            mCommandList->Dispatch(mClientWidth, groupsY, 1); // 每个线程组管一列里的 256 个像素

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
        mCommandList->CopyResource(back, A); // 参数顺序：(目标, 来源)

        // ⑤ 收尾：全部恢复成进来时的样子
        D3D12_RESOURCE_BARRIER b5[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(back, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(A, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
            CD3DX12_RESOURCE_BARRIER::Transition(B, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
        };
        mCommandList->ResourceBarrier(3, b5);
    }

    // 堆里第 slot 格的 GPU 句柄
    CD3DX12_GPU_DESCRIPTOR_HANDLE HeapGpuHandle(int slot) const
    {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(mHeap->GetGPUDescriptorHandleForHeapStart(), slot, mCSUDescriptorSize);
    }

    // 堆里第 slot 格的 CPU 句柄
    CD3DX12_CPU_DESCRIPTOR_HANDLE HeapCpuHandle(int slot) const
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(mHeap->GetCPUDescriptorHandleForHeapStart(), slot, mCSUDescriptorSize);
    }

    // WASD 前后左右，Q 下降、E 上升（沿世界 y 轴，和朝向无关）
    void OnKeyboardInput(const GameTimer& gt)
    {
        if (ImGui::GetIO().WantCaptureKeyboard) return;
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
        if (forward != 0.0f || right != 0.0f || up != 0.0f) mCameraPreset = -1; // 离开了预设机位
        const float dt = gt.DeltaTime();
        if (forward != 0.0f) mCamera.Walk(forward * dt);
        if (right != 0.0f) mCamera.Strafe(right * dt);
        if (up != 0.0f) mCamera.Ascend(up * dt);
    }

    // HLSL 目录里所有 .hlsl 中最新的修改时间。查整个目录：改了被 include 的 LightingUtil.hlsl 也要算
    static std::filesystem::file_time_type NewestShaderTime()
    {
        std::filesystem::file_time_type newest{};
        std::error_code ec; // 用 error_code 版本：文件正被编辑器写入时不抛异常
        for (const auto& entry : std::filesystem::directory_iterator(L"HLSL", ec))
        {
            if (entry.path().extension() == L".hlsl")
            {
                newest = (std::max)(newest, entry.last_write_time(ec)); // 加括号：躲开 windows.h 的 max 宏
            }
        }
        return newest;
    }

    void CheckShaderReload(const GameTimer& gt)
    {
        if (gt.TotalTime() - mLastShaderPoll < 0.25f) return; // 每 0.25 秒查一次
        mLastShaderPoll = gt.TotalTime();

        const auto newest = NewestShaderTime();
        if (newest == mShaderTime) return; // 没改过
        // 编辑器保存时可能分几次写完：第一次看到新时间先记下，0.25 秒后还是它，才算写完
        if (newest != mPendingShaderTime)
        {
            mPendingShaderTime = newest;
            return;
        }
        mShaderTime = newest;
        ReloadShaders();
    }

    void ReloadShaders()
    {
        try
        {
            BuildShadersAndInputLayout(); // 有一个编不过就抛异常，后面两步不执行，旧 PSO 照常用
            FlushCommandQueue(); // CPU 最多领先 3 帧，GPU 可能还在用旧 PSO，等它画完
            BuildPSO(); // IID_PPV_ARGS(&mXxxPSO) 先放掉旧的，再把新的建在同一个槽里，渲染项自动跟着换
            OutputDebugStringW(L"[热重载] 成功\n");
        }
        catch (DxException& e)
        {
            // 弹窗抢走焦点时框架会暂停、停住计时器，关掉后接着跑，时间不会跳
            MessageBox(mhMainWnd, e.ToString().c_str(), L"着色器热重载失败", MB_OK);
        }
    }

    void BuildImGui()
    {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // 面板大小跟着内容走：imgui.ini 记的是像素大小，UI 按 DPI 放大以后旧尺寸装不下，内容会被截掉
        ImGui::Begin("调试", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("帧率 %.0f", ImGui::GetIO().Framerate);
        ImGui::Checkbox("垂直同步", &mVsync); // 默认关：帧率不锁在屏幕刷新率上，看得出真实的快慢
        ImGui::Checkbox("后期模糊", &mBlurEnabled);
        ImGui::SliderFloat("曝光", &mExposureEV, -4.0f, 4.0f, "%+.1f EV");
        const char* curves[] = {"截断", "Reinhard", "ACES", "AgX"};
        ImGui::Combo("色调曲线", &mToneCurve, curves, IM_ARRAYSIZE(curves));
        ImGui::SliderFloat("太阳强度", &mSunIntensity, 0.5f, 4.0f, "×%.2f");
        ImGui::Checkbox("Bloom", &mBloomEnabled);
        ImGui::SliderFloat("Bloom 强度", &mBloomIntensity, 0.0f, 0.5f, "%.3f");
        ImGui::SliderFloat("Bloom 阈值", &mBloomThreshold, 0.0f, 4.0f, "%.2f");
        ImGui::Checkbox("PBR 测试球", &mShowTestSpheres);
        ImGui::Checkbox("延迟渲染", &mDeferred);
        if (mDeferred) // 调试视图只有延迟模式才有（要读 G-Buffer）
        {
            // 第 7 个"环境光遮蔽"要等 SSAO 第①步写完、把 kGBufferViews 改成 7 才会出现在下拉框里
            const char* gbufferViews[] = {"关", "基础色", "法线", "粗糙度", "金属度", "深度", "世界坐标", "环境光遮蔽"};
            ImGui::Combo("G-Buffer 视图", &mGBufferView, gbufferViews, 1 + kGBufferViews);
            ImGui::Checkbox("SSAO", &mSsao); // 关掉 = 遮蔽图清成全白（1 = 没被挡），环境光不变暗
            ImGui::SliderInt("AO 模糊次数", &mSsaoBlurCount, 0, 4); // "横 + 竖"算一次；0 = 看没模糊的噪点
        }
        if (ImGui::Checkbox("64 盏小灯", &mManyLights))
        {
            ReloadShaders(); // 小灯的数量是编译着色器时传的宏（NUM_EXTRA_LIGHTS），换了要重编
        }
        if (mDeferred && mManyLights) // 只有延迟 + 小灯时才有两种画法可选
        {
            ImGui::Checkbox("光体积", &mLightVolumes); // 勾上：每盏小灯画一个球；不勾：全屏 Pass 里循环 64 盏
        }
        bool sunChanged = ImGui::SliderFloat("太阳方位角", &mSunAzimuth, -180.0f, 180.0f, "%.1f°");
        sunChanged |= ImGui::SliderFloat("太阳仰角", &mSunElevation, 5.0f, 85.0f, "%.1f°");
        if (sunChanged)
        {
            const float az = XMConvertToRadians(mSunAzimuth), el = XMConvertToRadians(mSunElevation);
            mSunDir = XMFLOAT3(-cosf(el) * sinf(az), -sinf(el), -cosf(el) * cosf(az));
        }

        // 机位：选一个就把摄像机放过去，之后照样能用鼠标键盘挪（一挪就显示"自由视角"）
        const char* preview = mCameraPreset >= 0 ? kCameraPresets[mCameraPreset].Name : "自由视角";
        if (ImGui::BeginCombo("机位", preview))
        {
            for (int i = 0; i < (int)_countof(kCameraPresets); i++)
            {
                if (ImGui::Selectable(kCameraPresets[i].Name, i == mCameraPreset))
                {
                    mCameraPreset = i;
                    mCamera.LookAt(kCameraPresets[i].Eye, kCameraPresets[i].Target, XMFLOAT3(0.0f, 1.0f, 0.0f));
                }
            }
            ImGui::EndCombo();
        }
        ImGui::End();

        ImGui::Render();
    }

    void Update(const GameTimer& gt) override
    {
        // 先处理输入、移动摄像机，这一帧的 ViewProj 才是移动后的（放在最后会晚一帧）
        OnKeyboardInput(gt);
        CheckShaderReload(gt); // HLSL 文件改过就重新编译、重建 PSO
        BuildImGui();

        // 轮换到下一份帧资源；GPU 如果还在用它，就先等
        mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
        mCurrentFrame = mFrameResources[mCurrFrameResourceIndex].get();
        WaitForCurrentFrameResource(); //执行完，说明上一次用这份帧资源的那一帧，GPU 已经全部做完了。

        if (mCurrentFrame->Fence != 0)
        {
            const UINT tsBase = mCurrFrameResourceIndex * kTimestampCount;
            D3D12_RANGE readRange{
                tsBase * sizeof(UINT64), (tsBase + kTimestampCount) * sizeof(UINT64)
            }; // 告诉驱动：CPU 要读哪一段
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
                    L"LearnDX12    移速 {:.1f}    GPU：阴影图 {:.3f} | 不透明/G-Buffer {:.3f} | SSAO {:.3f} | 光照 {:.3f} | 天空+镜子 {:.3f} | 透明 {:.3f} | Bloom {:.3f} | 色调映射 {:.3f} | 模糊 {:.3f} ms",
                    mCamera.GetMoveSpeed(), avg[0], avg[1], avg[2], avg[3], avg[4], avg[5], avg[6], avg[7], avg[8]);
            }
        }

        UpdateWaves(gt);

        UpdateMaterialCBs();
        UpdateObjectCBs();
        UpdateShadowTransform();
        UpdateMainPassCB();
    }

    /*
     * 一帧的 Pass 地图。PSO 本身不知道自己属于哪一段：它在哪个 Draw 前被 SetPipelineState，就属于哪一段；
     * 创建 PSO 时的 RTVFormats / DSVFormat / 根签名，必须和那一段绑着的渲染目标、深度缓冲、根签名对上。
     * 下面的 rtv 就是 HDR 纹理的 RTV（传进 DrawDeferred 后叫 hdrRtv），dsv 是主深度缓冲。
     *
     *   ① 阴影图        DrawShadowMap
     *        渲染目标：无 | 深度：阴影图 | PSO：各物体的 ShadowPSO | 根签名：主
     *   ② 不透明物体
     *        前向：渲染目标：HDR | 深度：主深度 | PSO：各物体的 PSO | 根签名：主
     *        延迟：DrawDeferred，分成两段 ↓
     *     ②a G-Buffer   渲染目标：GBuffer0 + GBuffer1 | 深度：主深度 | PSO：各物体的 GbufferPSO | 根签名：主
     *     ②b 光照       全屏三角形
     *        渲染目标：HDR | 深度：无（深度这时当 SRV 读）| PSO：mDeferredLightPSO，选了 G-Buffer 视图时换 mGBufferViewPSO[v]
     *        根签名：主（根参数 8 = G-Buffer + 深度）
     *        勾了"64 盏小灯"+"光体积"时：全屏这次换 mDeferredLightBasePSO（只算原来 3 盏），接着 ↓
     *     ②c 光体积     64 个球，一次实例化画完
     *        渲染目标：HDR（加法混合，叠在 ②b 上）| 深度：无 | PSO：mLightVolumePSO | 根签名：主
     *   ③ 天空、镜子、倒影、透明
     *        渲染目标：HDR | 深度：主深度 | PSO：各物体的 PSO | 根签名：主
     *   ④ Bloom         DoBloom，全屏三角形
     *        渲染目标：Bloom 纹理链（kBloomFormat）| 深度：无 | PSO：mBloomPrefilter/Down/UpPSO | 根签名：Bloom
     *   ⑤ 色调映射      全屏三角形，HDR 纹理 → 屏幕
     *        渲染目标：后台缓冲（mBackBufferFormat）| 深度：无 | PSO：mTonemapPSO | 根签名：Tonemap
     *   ⑥ 模糊          DoBlur，计算着色器（Dispatch，不绑渲染目标）
     *        读写：后台缓冲 ↔ 模糊纹理 | PSO：mHorzBlurPSO / mVertBlurPSO | 根签名：Blur（计算）
     *   ⑦ UI            ImGui
     *        渲染目标：后台缓冲 | 深度：无 | PSO、根签名：ImGui 自己的
     *
     * 两个 PSO 长得像（比如光照 PSO 和 tonemapDesc 都是全屏三角形），但在表里不同行，输出格式就不同
     */
    void Draw(const GameTimer& gt) override
    {
        const UINT tsBase = mCurrFrameResourceIndex * kTimestampCount; // 这份帧资源的 6 格从这里开始
        // 用这份帧资源自己的分配器录制（Update 里已经确认 GPU 用完了它）
        ThrowIfFailed(mCurrentFrame->CmdListAlloc->Reset());
        ThrowIfFailed(mCommandList->Reset(mCurrentFrame->CmdListAlloc.Get(), nullptr));

        // 后台缓冲：PRESENT → RENDER_TARGET；HDR 纹理：PIXEL_SHADER_RESOURCE → RENDER_TARGET（场景要往里画）
        D3D12_RESOURCE_BARRIER toRenderTarget[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(
                CurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(mHdrMap.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET)
        };
        mCommandList->ResourceBarrier(2, toRenderTarget);

        // 每帧只需设一次的：堆 → 根签名。阴影 Pass 和主 Pass 共用，所以放在最前面
        // 参数 1 是描述符表，必须先 SetDescriptorHeaps；参数 0（物体常量）在 DrawRenderItems 里每个物体设一次
        ID3D12DescriptorHeap* heaps[] = {mHeap.Get()};
        mCommandList->SetDescriptorHeaps(_countof(heaps), heaps);
        mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

        // 3 份 Pass 常量挨着放：passCB = 第 0 份（主 Pass），+1 份 = 镜像，+2 份 = 阴影 Pass
        const UINT passCBByteSize = CalcConstantBufferByteSize(sizeof(PassConstants));
        const D3D12_GPU_VIRTUAL_ADDRESS passCB = mCurrentFrame->PassCB->Resource()->GetGPUVirtualAddress();

        // ① 阴影 Pass：从太阳那里把场景的深度画进阴影图
        mCommandList->EndQuery(mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 0); // T0
        DrawShadowMap(passCB + 2 * passCBByteSize);
        mCommandList->EndQuery(mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 1); // T1
        mCommandList->SetGraphicsRootDescriptorTable(5, HeapGpuHandle(10));

        // ② 主 Pass。阴影 Pass 把视口改成了阴影图的大小，这里换回屏幕的
        mCommandList->RSSetViewports(1, &mScreenViewport);
        mCommandList->RSSetScissorRects(1, &mScissorRect);

        // 清屏：场景画进 HDR 纹理（后台缓冲不用清了，色调映射会把每个像素写一遍）
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = mHdrRtvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = DepthStencilView();
        const float hdrClear[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // 必须和优化清除值一样
        mCommandList->ClearRenderTargetView(rtv, hdrClear, 0, nullptr);
        mCommandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0,
                                            nullptr);

        mCommandList->SetGraphicsRootConstantBufferView(2, passCB); // 阴影 Pass 用的是第 2 份，换回主 Pass 的
        mCommandList->SetGraphicsRootDescriptorTable(4, HeapGpuHandle(9));
        mCommandList->SetGraphicsRootDescriptorTable(7, HeapGpuHandle(16));

        // 不透明物体。延迟 = G-Buffer Pass + 光照 Pass（选了 G-Buffer 视图时换成调试视图）；前向 = 边画边算光。
        // 两条路出来时一样：HDR 纹理 + 深度缓冲绑着，深度里是不透明物体的深度
        if (mDeferred)
        {
            DrawDeferred(rtv, dsv, tsBase); // T2、T3、T4 在里面
        }
        else
        {
            mCommandList->OMSetRenderTargets(1, &rtv, true, &dsv);
            DrawRenderItems(mRitemLayer[(int)RenderLayer::Opaque]);
            if (mShowTestSpheres)
            {
                DrawRenderItems(mRitemLayer[(int)RenderLayer::TestSpheres]);
            }
            mCommandList->EndQuery(mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 2); // T2
            mCommandList->EndQuery(mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 3); // T3：前向没有 SSAO
            mCommandList->EndQuery(mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 4); // T4：前向没有单独的光照 Pass
        }

        // G-Buffer 视图只看 G-Buffer 里存的东西：天空、镜子、透明物体都不画。前向模式下没有 G-Buffer，下拉框不算数
        const bool gbufferView = mDeferred && mGBufferView > 0;
        if (!gbufferView)
        {
            DrawRenderItems(mRitemLayer[(int)RenderLayer::Sky]); // 天空：不透明之后（被挡住的像素省掉），透明之前（水要和天空混合）
            mCommandList->OMSetStencilRef(1); // 章上刻"1"
            DrawRenderItems(mRitemLayer[(int)RenderLayer::Mirrors]); // ③ 看得见的镜子像素盖章
            DrawRenderItems(mRitemLayer[(int)RenderLayer::MirrorClear]); // ③b 盖了章的地方：深度推远、刷成天空色
            // ④ 镜像：Ref 还是 1，只画在模板 = 1 的像素；Pass 常量换成第 1 份（反射后的灯）
            mCommandList->SetGraphicsRootConstantBufferView(2, passCB + passCBByteSize);
            DrawRenderItems(mRitemLayer[(int)RenderLayer::Reflected]);
            mCommandList->SetGraphicsRootConstantBufferView(2, passCB); // 换回主 Pass 常量，后面的镜子和水要用
            mCommandList->OMSetStencilRef(0); // 用完恢复
        }
        mCommandList->EndQuery(mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 5); // T5
        // 平面阴影（第 11 章的 Shadow 层）已经被阴影贴图取代，不再画；再画会和阴影贴图的影子叠成双重影子
        if (!gbufferView)
        {
            DrawRenderItems(mRitemLayer[(int)RenderLayer::Transparent]);
        }
        mCommandList->EndQuery(mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 6); // T6

        // 色调映射：HDR 纹理 → 后台缓冲。HDR 纹理先从"被写"转成"被读"，帧末就停在这个状态（约定）
        auto hdrToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            mHdrMap.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        mCommandList->ResourceBarrier(1, &hdrToSrv);
        if (mBloomEnabled && !gbufferView)
        {
            DoBloom();
        }
        mCommandList->EndQuery(mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 7); // T7
        //Bloom Pass 把视口改成了Bloom图的大小，这里换回屏幕的
        mCommandList->RSSetViewports(1, &mScreenViewport);
        mCommandList->RSSetScissorRects(1, &mScissorRect);
        D3D12_CPU_DESCRIPTOR_HANDLE backRtv = CurrentBackBufferView();
        mCommandList->OMSetRenderTargets(1, &backRtv, true, nullptr);
        mCommandList->SetGraphicsRootSignature(mTonemapRootSignature.Get());
        mCommandList->SetGraphicsRootDescriptorTable(0, HeapGpuHandle(18)); // 堆第 18 格：HDR 纹理的 SRV
        // 根参数 1：两个 32 位根常量（b0），和 Tonemap.hlsl 的 TonemapSettings 一一对应。EV 每 +1 亮一倍，所以是 2^EV
        struct
        {
            float Exposure;
            UINT Curve;
            float BloomIntensity;
        } tonemap = {exp2f(mExposureEV), (UINT)mToneCurve, mBloomEnabled ? mBloomIntensity : 0.0f};
        if (gbufferView)
        {
            tonemap = {1.0f, 0u, 0.0f}; // G-Buffer 视图：不加曝光、不压曲线、不加 Bloom，屏幕上就是存的原值
        }
        mCommandList->SetGraphicsRoot32BitConstants(1, 3, &tonemap, 0);
        mCommandList->SetPipelineState(mTonemapPSO.Get());
        mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        mCommandList->DrawInstanced(3, 1, 0, 0); // 3 个顶点 = 一个盖住全屏的大三角形，位置在 VS 里按 SV_VertexID 算
        mCommandList->EndQuery(mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 8); // T8
        if (mBlurEnabled)
        {
            DoBlur();
        }
        mCommandList->EndQuery(mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 9); // T9
        // UI 最后画：盖在最上面，不被模糊，也不做深度测试。放在 T8 后面，不算进"模糊"的耗时
        // 画在后台缓冲上：rtv 现在是 HDR 纹理，画进去就不会再上屏了
        mCommandList->OMSetRenderTargets(1, &backRtv, true, nullptr);
        ID3D12DescriptorHeap* uiHeaps[] = {mHeap.Get()}; // 字体图集的 SRV 在 mHeap 第 12～15 格；显式再绑一次，不依赖前面绑过什么
        mCommandList->SetDescriptorHeaps(_countof(uiHeaps), uiHeaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandList.Get());

        mCommandList->ResolveQueryData(
            mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
            tsBase, kTimestampCount, // 从查询堆第 tsBase 格开始，复制这一帧的 6 格
            mTimestampReadback.Get(),
            tsBase * sizeof(UINT64)); // 复制到回读缓冲的同一位置（单位是字节）

        // 后台缓冲：RENDER_TARGET → PRESENT
        auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
            CurrentBackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        mCommandList->ResourceBarrier(1, &toPresent);

        // 结束录制，提交
        ThrowIfFailed(mCommandList->Close());
        ID3D12CommandList* cmdLists[] = {mCommandList.Get()};
        mCommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);
        // 垂直同步：开 = 等下一次屏幕刷新再换（1）；关 = 不等（0），而且要带上 ALLOW_TEARING——
        // 只写 0 的话，窗口模式下 DWM 合成照样按刷新率取帧，双缓冲时帧率还是被卡在刷新率（实测 144 Hz 屏卡在 142～144）
        const UINT syncInterval = mVsync ? 1 : 0;
        const UINT presentFlags = (!mVsync && mTearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;
        ThrowIfFailed(mSwapChain->Present(syncInterval, presentFlags));

        // 不再每帧 Flush：给这份帧资源记下 Fence 值。GPU 执行到这条 Signal 时，Fence 才会变成这个值，
        // 3 帧后轮回到这份帧资源时，WaitForCurrentFrameResource 就靠它判断 GPU 用完没有
        mCurrentFrame->Fence = ++mCurrentFence;
        ThrowIfFailed(mCommandQueue->Signal(mFence.Get(), mCurrentFence));
    }

    // 鼠标：左键拖动旋转，右键拖动缩放。
    // 正负号故意和书上相反："抓着物体转"——物体跟着鼠标走，往上/往右拖是拉近
    void OnMouseDown(WPARAM btnState, int x, int y) override
    {
        if (ImGui::GetIO().WantCaptureMouse) return;
        mLastMousePos.x = x;
        mLastMousePos.y = y;
        SetCapture(mhMainWnd); // 按着键拖出窗口以后，鼠标消息仍然发给这个窗口
    }

    void OnMouseUp(WPARAM btnState, int x, int y) override
    {
        if (ImGui::GetIO().WantCaptureMouse) return;
        ReleaseCapture();
    }

    void OnMouseMove(WPARAM btnState, int x, int y) override
    {
        if (ImGui::GetIO().WantCaptureMouse) return;
        if ((btnState & MK_LBUTTON) != 0) // 左键：旋转，每像素 0.25°
        {
            float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
            float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));
            mCamera.Pitch(dy);
            mCamera.RotateY(dx);
            mCameraPreset = -1; // 离开了预设机位
        }
        // 必须放在最后：下一次 MouseMove 要用这次的位置算差值
        mLastMousePos.x = x;
        mLastMousePos.y = y;
    }

    // 滚轮调移速：往前滚一格 ×1.25，往后滚一格 ÷1.25。
    // 按倍数而不是按加减调：慢的时候每格变化小、调得细，快的时候每格变化大、调得快
    void OnMouseWheel(int delta) override
    {
        if (ImGui::GetIO().WantCaptureMouse) return;
        float speed = mCamera.GetMoveSpeed() * powf(1.25f, delta / static_cast<float>(WHEEL_DELTA));
        speed = std::clamp(speed, 0.5f, 200.0f);
        mCamera.SetMoveSpeed(speed);
        // 基类每秒刷新一次标题（CalculateFrameStats），会把这段文字带上
        mMainWndCaption = std::format(L"LearnDX12    移速 {:.1f}", speed);
    }

private:
    // ======================== 每帧 ========================

    // 阴影 Pass：站在灯的位置只画深度，画进阴影图。进来、出去时阴影图都是 PIXEL_SHADER_RESOURCE
    void DrawShadowMap(D3D12_GPU_VIRTUAL_ADDRESS shadowPassCB)
    {
        // 视口、裁剪矩形换成阴影图的大小
        mCommandList->RSSetViewports(1, &mShadowViewPort);
        mCommandList->RSSetScissorRects(1, &mShadowScissorRect);
        // 这一趟阴影图当深度缓冲写：PIXEL_SHADER_RESOURCE → DEPTH_WRITE
        D3D12_RESOURCE_BARRIER b1[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_DEPTH_WRITE),
        };
        mCommandList->ResourceBarrier(1, b1);
        // 只有 DSV（堆里只有 1 格，就是开头那格），没有颜色目标
        D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv = mShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
        mCommandList->OMSetRenderTargets(0, nullptr, false, &shadowDsv);
        mCommandList->ClearDepthStencilView(shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr); // 只清深度；最后两个 = 整张都清
        mCommandList->SetGraphicsRootConstantBufferView(2, shadowPassCB); // 第 2 份 Pass 常量：ViewProj 是灯的
        DrawRenderItems(mRitemLayer[(int)RenderLayer::Opaque], DrawPass::Shadow);
        if (mShowTestSpheres)
        {
            DrawRenderItems(mRitemLayer[(int)RenderLayer::TestSpheres], DrawPass::Shadow);
        }
        // 转回 PIXEL_SHADER_RESOURCE：主 Pass 要把它当纹理读
        D3D12_RESOURCE_BARRIER b2[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        mCommandList->ResourceBarrier(1, b2);
    }

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
                // 1) 把 mat 的 4 个字段抄进 mc（Pad 只是凑满 16 字节，填 0）
                mc.BaseColor = mat->BaseColor;
                mc.Metallic = mat->Metallic;
                mc.Roughness = mat->Roughness;
                mc.Reflectance = mat->Reflectance;
                mc.Pad = 0.0f;
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
        const XMMATRIX viewProj =
            mCamera.GetView() * mCamera.GetProj();

        XMStoreFloat4x4(
            &pc.ViewProj,
            XMMatrixTranspose(viewProj));

        const XMMATRIX invViewProj =
            XMMatrixInverse(nullptr, viewProj);

        XMStoreFloat4x4(
            &pc.InvViewProj,
            XMMatrixTranspose(invViewProj));
        XMStoreFloat4x4(&pc.ShadowTransform, XMMatrixTranspose(XMLoadFloat4x4(&mShadowTransform)));
        pc.EyePosW = mCamera.GetPosition3f();
        // 灯和环境光都是线性值：以前在 sRGB 编码过的数值上凭眼睛调的，换算时每个分量取 2.2 次方（原值写在注释里）
        pc.AmbientLight = {0.071f, 0.082f, 0.133f, 1.0f}; // 偏蓝：天空散射过来的光，阴影里带点蓝（原 0.3, 0.32, 0.4）
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
        // 夕阳：红多蓝少，偏暖（线性值，原 1.35, 1.12, 0.85）；再乘面板上的"太阳强度"
        pc.Lights[0].Strength = {1.935f * mSunIntensity, 1.283f * mSunIntensity, 0.700f * mSunIntensity};
        pc.Lights[0].FalloffStart = 5;
        // 太阳在场景后方、压得很低，朝镜头斜着照过来，水面的镜面反射点才会进入画面。
        // 归一化再传：手写的 mSunDir 四舍五入后长度是 1.000127，BlinnPhong 里点积会超过 1 → pow(负数) = NaN → 背对太阳看时屏幕上一个黑圆
        XMStoreFloat3(&pc.Lights[0].Direction, XMVector3Normalize(XMLoadFloat3(&mSunDir)));
        pc.Lights[0].FalloffEnd = 100;
        pc.Lights[0].Position = {0, 50, 0};
        pc.Lights[0].SpotPower = 0.2f;

        // Lights[1]：点光源，悬在水面上方，暖白色。Strength 是发光强度，按 1/d² 衰减，FalloffEnd 处平滑地降到 0
        // ×16：4 个单位处 16 / 4² = 1，和以前线性衰减时差不多亮
        pc.Lights[1].Strength = {1.0f * 16, 0.793f * 16, 0.325f * 16}; // 颜色：原 1.0, 0.9, 0.6
        pc.Lights[1].FalloffStart = 3.0f; // PBR 的衰减不用它了
        pc.Lights[1].FalloffEnd = 14.0f; // 影响半径
        pc.Lights[1].Position = {6.0f, 2.0f, -34.0f};

        // Lights[2]：聚光灯，挂在左边山坡上方 8 个单位，竖直朝下照，冷白色
        // ×100：照到山坡大约 10 个单位，100 / 10² = 1
        pc.Lights[2].Strength = {0.456f * 100, 0.612f * 100, 1.0f * 100}; // 颜色：原 0.7, 0.8, 1.0
        pc.Lights[2].FalloffStart = 8.0f; // PBR 的衰减不用它了
        pc.Lights[2].FalloffEnd = 16.0f; // 影响半径
        pc.Lights[2].Position = {-2.8f, 11.9f, -40.0f};
        pc.Lights[2].Direction = {0.0f, -1.0f, 0.0f};
        pc.Lights[2].SpotPower = 32.0f;
        if (mManyLights)
        {
            FillExtraLights(pc);
        }
        mCurrentFrame->PassCB->CopyData(0, pc);

        PassConstants reflectedPass = pc; // 其余都一样，只把灯反射一下
        const XMMATRIX R = XMMatrixReflect(XMLoadFloat4(&mMirrorPlane));
        const int lightCount = 3 + (mManyLights ? (int)kExtraLights : 0); // 小灯也要反射，镜子里才照得到
        for (int i = 0; i < lightCount; ++i)
        {
            // 方向：只转不平移（w = 0）；位置：要平移（w = 1）
            XMStoreFloat3(&reflectedPass.Lights[i].Direction,
                          XMVector3TransformNormal(XMLoadFloat3(&pc.Lights[i].Direction), R));
            XMStoreFloat3(&reflectedPass.Lights[i].Position,
                          XMVector3TransformCoord(XMLoadFloat3(&pc.Lights[i].Position), R));
        }
        XMStoreFloat4x4(&reflectedPass.ShadowTransform, XMMatrixTranspose(R * XMLoadFloat4x4(&mShadowTransform)));
        mCurrentFrame->PassCB->CopyData(1, reflectedPass);

        // 阴影 Pass：只把 ViewProj 换成灯的。
        // EyePosW 不改，保留相机位置：树的 GS 靠它决定朝向，阴影图里的树必须和屏幕上的树朝同一个方向
        PassConstants shadowPass = pc;
        XMStoreFloat4x4(&shadowPass.ViewProj, XMMatrixTranspose(XMLoadFloat4x4(&mLightViewProj)));
        mCurrentFrame->PassCB->CopyData(2, shadowPass);
    }

    // 第③步的 64 盏小灯：8×8 排在地砖、木箱、测试球一带，离地 0.7，影响半径 4，色相按编号转一圈
    void FillExtraLights(PassConstants& pc) const
    {
        for (UINT i = 0; i < kExtraLights; i++)
        {
            const float x = -8.0f + 24.0f * (float)(i % 8) / 7.0f;
            const float z = -42.0f + 26.0f * (float)(i / 8) / 7.0f;
            Light& L = pc.Lights[3 + i];
            L.Position = {x, (std::max)(GetHillsHeight(x, z), 0.05f) + 0.7f, z}; // 水面以下的地方按水面高度算
            L.FalloffStart = 0.0f;
            L.FalloffEnd = 4.0f; // 影响半径：光体积的球就按它放大
            L.Direction = {0.0f, -1.0f, 0.0f};
            L.SpotPower = 0.0f;
            const XMVECTOR rgb = XMColorHSVToRGB(XMVectorSet((float)i / kExtraLights, 0.7f, 1.0f, 1.0f));
            XMStoreFloat3(&L.Strength, rgb * 3.0f); // 发光强度：1 个单位外亮度 3
        }
    }

    // 灯是固定的，其实算一次就够；放在每帧算，以后让太阳动起来也不用改
    void UpdateShadowTransform()
    {
        //场景包围球：中心在原点，半径120（陆地160x160，山高+-43，最远的角离原点约117）
        const XMVECTOR center = XMVectorZero();
        const float radius = 120.f;

        //灯的观察矩阵：灯放在球外--从球心逆着光的方向退2个半径
        const XMVECTOR lightDir = XMLoadFloat3(&mSunDir);
        const XMVECTOR lightPos = center - lightDir * 2 * radius;
        const XMMATRIX V = XMMatrixLookAtLH(lightPos, center, XMVectorSet(0.f, 1.f, 0.f, 0.f)); //世界坐标换到太阳视角

        //正交投影：盒子刚好框柱球。先把球心变到灯的观察空间，结果是c
        XMFLOAT3 c;
        XMStoreFloat3(&c, XMVector3TransformCoord(center, V));
        const XMMATRIX P = XMMatrixOrthographicOffCenterLH(c.x - radius, c.x + radius, c.y - radius, c.y + radius,
                                                           c.z - radius, c.z + radius); //压进盒子，得到 NDC
        // T：NDC → 纹理坐标（u = 0.5x + 0.5，v = -0.5y + 0.5，深度不变）。行向量，平移在第 4 行
        const XMMATRIX T(
            .5f, 0, 0, 0,
            0, -.5f, 0, 0,
            0, 0, 1, 0,
            .5f, .5f, 0, 1.f); //NDC 换成 (u, v, 深度)

        XMStoreFloat4x4(&mLightViewProj, V * P);
        XMStoreFloat4x4(&mShadowTransform, V * P * T);
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
            // 高度随机取正负：总是往上戳的话，波动方程只把水推来推去、不会把多出来的水带走，
            // 平均水位越涨越高，跑二十几秒就把贴着 y = 0 的地板淹了。正负各半，平均不加水也不减水
            float m = magnitude(rng);
            if (rng() & 1) m = -m;
            mWaves->Disturb(row(rng), col(rng), m);
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
    void DrawRenderItems(const std::vector<RenderItem*>& items, DrawPass pass = DrawPass::Main)
    {
        const UINT objCBByteSize = CalcConstantBufferByteSize(sizeof(ObjectConstants)); // 256 字节对齐
        const D3D12_GPU_VIRTUAL_ADDRESS objCBStart = mCurrentFrame->ObjectCB->Resource()->GetGPUVirtualAddress();

        const UINT MatCBByteSize = CalcConstantBufferByteSize(sizeof(MaterialConstants));
        const D3D12_GPU_VIRTUAL_ADDRESS MatCBStart = mCurrentFrame->MaterialCB->Resource()->GetGPUVirtualAddress();

        for (auto& ri : items)
        {
            // PSO 是一整包状态，只能整个换。木箱用木箱的，陆地和波浪用另一套；阴影 Pass 用 ShadowPSO，G-Buffer Pass 用 GbufferPSO
            ID3D12PipelineState* const* slot = pass == DrawPass::Shadow
                                                   ? ri->ShadowPSO
                                                   : pass == DrawPass::GBuffer
                                                   ? ri->GbufferPSO
                                                   : ri->PSO;
            ID3D12PipelineState* pso = slot != nullptr ? *slot : nullptr; // 取槽里现在放的那个 PSO
            if (pso == nullptr)
            {
                continue; // 这个 Pass 没给它挂 PSO：没挂 ShadowPSO 的不投影子，没挂 GbufferPSO 的不走延迟
            }
            mCommandList->SetPipelineState(pso);
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

            //算出 tileNormal贴图的GPU 地址
            if (ri->Mat->NormalSrvHeapIndex >= 0)
            {
                CD3DX12_GPU_DESCRIPTOR_HANDLE texNormal(mHeap->GetGPUDescriptorHandleForHeapStart(),
                                                        ri->Mat->NormalSrvHeapIndex,
                                                        mCSUDescriptorSize);
                mCommandList->SetGraphicsRootDescriptorTable(6, texNormal);
            }

            mCommandList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
        }
    }

    // ======================== 初始化（按 Initialize 里的调用顺序） ========================

    void LoadTextureHelper(std::string Name, std::wstring FileName, bool sRGB = true)
    {
        auto Tex = std::make_unique<DX_Utils::Texture>();
        Tex->Name = Name;
        Tex->Filename = FileName;
        ThrowIfFailed(CreateDDSTextureFromFile12(mDevice.Get(), mCommandList.Get(), Tex->Filename.c_str(),
            Tex->Resource, Tex->UploadHeap,0,nullptr,sRGB));
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
        LoadTextureHelper("skyCubeTex", L"Textures\\grasscube1024.dds");
        LoadTextureHelper("tileNormalTex", L"Textures\\tile_nmap.dds", false);
        LoadTextureHelper("grassTex", L"Textures\\grass.dds");
        LoadTextureHelper("stoneTex", L"Textures\\stone.dds");
    }

    // 构建描述符堆
    void BuildDescriptorHeaps()
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {
            .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            .NumDescriptors = 28,
            /* 0～4 纹理，
             * 5～8 模糊用的两张纹理（各一个 SRV、一个 UAV），
             * 9 阴影图的 SRV，10天空图   
             * 11是地板法线贴图 
             * 12~15 ImGui 
             * 16 ~ 17 grass stone   
             * 18 HDR场景纹理
             * 19～24 是 Bloom 的 6 个 SRV
             * 25~26 Gbuffer0~1
             * 27 原来的深度缓冲
             */
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

        auto& skyTex = mTextures.at("skyCubeTex")->Resource;
        D3D12_SHADER_RESOURCE_VIEW_DESC skyDesc = {
            .Format = skyTex->GetDesc().Format,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE, // 资源是 6 张图的数组，SRV 把它看成立方体，用方向去查
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .TextureCube = {
                .MostDetailedMip = 0, .MipLevels = skyTex->GetDesc().MipLevels, .ResourceMinLODClamp = 0.0f
            },
        };
        CD3DX12_CPU_DESCRIPTOR_HANDLE skyHandle(mHeap->GetCPUDescriptorHandleForHeapStart(), 10, mCSUDescriptorSize);
        mDevice->CreateShaderResourceView(skyTex.Get(), &skyDesc, skyHandle);

        auto& tileNormalTex = mTextures.at("tileNormalTex")->Resource;
        D3D12_SHADER_RESOURCE_VIEW_DESC tileNormalTexDesc = {
            .Format = tileNormalTex->GetDesc().Format,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = {
                .MostDetailedMip = 0,
                .MipLevels = tileNormalTex->GetDesc().MipLevels,
                .PlaneSlice = 0,
                .ResourceMinLODClamp = 0.0f,
            },
        };
        CD3DX12_CPU_DESCRIPTOR_HANDLE tileNormalHandle(mHeap->GetCPUDescriptorHandleForHeapStart(), 11,
                                                       mCSUDescriptorSize);
        mDevice->CreateShaderResourceView(tileNormalTex.Get(), &tileNormalTexDesc,
                                          tileNormalHandle);

        // 陆地的两层纹理挨着放：16 草地、17 岩石。根参数 7 的表从 16 开始、管 2 格
        const char* landLayers[] = {"grassTex", "stoneTex"};
        for (int k = 0; k < 2; k++)
        {
            auto& landLayerTex = mTextures.at(landLayers[k])->Resource;
            D3D12_SHADER_RESOURCE_VIEW_DESC landLayerTexDesc = {
                .Format = landLayerTex->GetDesc().Format,
                .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
                .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                .Texture2D = {
                    .MostDetailedMip = 0,
                    .MipLevels = landLayerTex->GetDesc().MipLevels,
                    .PlaneSlice = 0,
                    .ResourceMinLODClamp = 0.0f,
                },
            };
            mDevice->CreateShaderResourceView(landLayerTex.Get(), &landLayerTexDesc, HeapCpuHandle(16 + k));
        }
    }

    void BuildShapeGeometry()
    {
        // 盒子：边长 2、中心在原点，24 个顶点、36 个索引。
        // 地板交给陆地了（BuildLandGeometry），这里只管木箱
        GeometryGenerator geoGen;
        GeometryGenerator::MeshData box = geoGen.CreateBox(2.0f, 2.0f, 2.0f);

        // GeometryGenerator 的顶点有位置、法线、uv，逐个字段拷过来；我们的 Vertex 还多一个切线，木箱用不到，留 0
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
        vertices.push_back(Vertex{
            XMFLOAT3(-11.0f, 0.0f, 13.0f), XMFLOAT3(0.0f, 1.0f, 0.0f),
            XMFLOAT2(0.0f, 0.0f), XMFLOAT3(1, 0, 0)
        });
        vertices.push_back(Vertex{
            XMFLOAT3(11.0f, 0.0f, 13.0f), XMFLOAT3(0.0f, 1.0f, 0.0f),
            XMFLOAT2(4.0f, 0.0f), XMFLOAT3(1, 0, 0)
        });
        vertices.push_back(Vertex{
            XMFLOAT3(11.0f, 0.0f, -13.0f), XMFLOAT3(0.0f, 1.0f, 0.0f),
            XMFLOAT2(4.0f, 5.0f), XMFLOAT3(1, 0, 0)
        });
        vertices.push_back(Vertex{
            XMFLOAT3(-11.0f, 0.0f, -13.0f), XMFLOAT3(0.0f, 1.0f, 0.0f),
            XMFLOAT2(0.0f, 5.0f), XMFLOAT3(1, 0, 0)
        });
        for (int i : {0, 1, 2, 0, 2, 3})
        {
            indices.push_back((std::uint16_t)i);
        }

        GeometryGenerator::MeshData skyBox = geoGen.CreateSphere(.5f, 20, 20);
        const UINT skyBoxVertexStart = (UINT)vertices.size();
        const UINT skyBoxIndexStart = (UINT)indices.size();
        // GeometryGenerator::Vertex 和我们的 Vertex 是两个类型，不能直接 insert，只能逐个字段拷；天空球用不到切线，留 0
        for (const auto& v : skyBox.Vertices)
        {
            vertices.push_back(Vertex{v.Position, v.Normal, v.TexC});
        }
        indices.insert(indices.end(), skyBox.GetIndices16().begin(), skyBox.GetIndices16().end());

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

        DX_Utils::SubmeshGeometry submesh_sphere;
        submesh_sphere.IndexCount = (UINT)skyBox.Indices32.size();
        submesh_sphere.StartIndexLocation = skyBoxIndexStart;
        submesh_sphere.BaseVertexLocation = (int)skyBoxVertexStart;
        mShapeGeo->DrawArgs["sphere"] = submesh_sphere;
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
        std::unique_ptr<Material> LMaterial_sky = std::make_unique<Material>();

        LMaterial_crate->Name = "crate";
        LMaterial_crate->MatCBIndex = 0;
        LMaterial_crate->BaseColor = {1, 1, 1, 1};
        LMaterial_crate->Metallic = 0.0f;
        LMaterial_crate->Roughness = 0.6f; // 木头半粗糙。原来的 0.2 在 BlinnPhong 里是光泽度 204，几乎是镜面，侧着看一大块过曝
        LMaterial_crate->Reflectance = 0.5f;

        LMaterial_grass->Name = "grass";
        LMaterial_grass->MatCBIndex = 1;
        LMaterial_grass->BaseColor = {1, 1, 1, 1};
        LMaterial_grass->Metallic = 0.0f;
        LMaterial_grass->Roughness = 0.9f; // 草地、石头都很粗糙
        LMaterial_grass->Reflectance = 0.5f;

        LMaterial_water->Name = "water";
        LMaterial_water->MatCBIndex = 2;
        LMaterial_water->BaseColor = {0.0f, 0.029f, 0.325f, .5f}; // 线性值，原 (0, 0.2, 0.6) 取 2.2 次方；透明度不是颜色，不换算
        LMaterial_water->Metallic = 0.0f;
        LMaterial_water->Roughness = 0.1f; // 不能是 0：α = 0 时 GGX 算出 0/0 = NaN
        LMaterial_water->Reflectance = 0.35f; // 水的 f0 ≈ 0.02：0.16·r² = 0.02 → r ≈ 0.35

        LMaterial_fence->Name = "fence";
        LMaterial_fence->MatCBIndex = 3;
        LMaterial_fence->BaseColor = {1.0f, 1.0f, 1.0f, 1.0f};
        LMaterial_fence->Metallic = 1.0f; // 铁丝网当金属：没有漫反射，贴图颜色就是反光的颜色
        LMaterial_fence->Roughness = 0.4f;
        LMaterial_fence->Reflectance = 0.5f; // 金属用不到（f0 直接取基础色）
        LMaterial_fence->DiffuseSrvHeapIndex = 1; //材质里的一个编号，意思是"我这个材质的贴图，放在 SRV 堆的第几格"。

        LMaterial_mirror->Name = "mirror";
        LMaterial_mirror->MatCBIndex = 4;
        LMaterial_mirror->BaseColor = {1.0f, 1.0f, 1.0f, .3f};
        LMaterial_mirror->Metallic = 0.0f;
        LMaterial_mirror->Roughness = 0.1f; // 冰面很光滑
        LMaterial_mirror->Reflectance = 0.35f; // 冰的 f0 和水差不多
        LMaterial_mirror->DiffuseSrvHeapIndex = 2;

        LMaterial_floor->Name = "floor";
        LMaterial_floor->MatCBIndex = 5;
        LMaterial_floor->BaseColor = {.793f, .793f, .793f, 1.f}; // 线性值，原 0.9 取 2.2 次方
        LMaterial_floor->Metallic = 0.0f;
        LMaterial_floor->Roughness = 0.5f; // 瓷砖；法线图的 alpha 还会把砖缝的粗糙度推到 1
        LMaterial_floor->Reflectance = 0.5f;
        LMaterial_floor->DiffuseSrvHeapIndex = 3;
        LMaterial_floor->NormalSrvHeapIndex = 11;

        LMaterial_shadow->Name = "shadow";
        LMaterial_shadow->MatCBIndex = 6;
        LMaterial_shadow->BaseColor = {.0f, .0f, .0f, .5f};
        LMaterial_shadow->Metallic = 0.0f; // 平面阴影已经不画了，下面两项随便填
        LMaterial_shadow->Roughness = 1.0f;
        LMaterial_shadow->Reflectance = 0.5f;
        LMaterial_shadow->DiffuseSrvHeapIndex = 0;

        LMaterial_fenceShadow->Name = "fenceShadow";
        LMaterial_fenceShadow->MatCBIndex = 7;
        LMaterial_fenceShadow->BaseColor = {.0f, .0f, .0f, .5f};
        LMaterial_fenceShadow->Metallic = 0.0f;
        LMaterial_fenceShadow->Roughness = 1.0f;
        LMaterial_fenceShadow->Reflectance = 0.5f;
        LMaterial_fenceShadow->DiffuseSrvHeapIndex = 1;

        LMaterial_tree->Name = "tree";
        LMaterial_tree->MatCBIndex = 8;
        LMaterial_tree->BaseColor = {1.0f, 1.0f, 1.0f, 1.0f};
        LMaterial_tree->Metallic = 0.0f;
        LMaterial_tree->Roughness = 0.9f; // 树叶很粗糙
        LMaterial_tree->Reflectance = 0.5f;
        LMaterial_tree->DiffuseSrvHeapIndex = 4;

        LMaterial_sky->Name = "sky";
        LMaterial_sky->MatCBIndex = 9;


        mMaterials[LMaterial_crate->Name] = std::move(LMaterial_crate);
        mMaterials[LMaterial_grass->Name] = std::move(LMaterial_grass);
        mMaterials[LMaterial_water->Name] = std::move(LMaterial_water);
        mMaterials[LMaterial_fence->Name] = std::move(LMaterial_fence);
        mMaterials[LMaterial_mirror->Name] = std::move(LMaterial_mirror);
        mMaterials[LMaterial_floor->Name] = std::move(LMaterial_floor);
        mMaterials[LMaterial_shadow->Name] = std::move(LMaterial_shadow);
        mMaterials[LMaterial_fenceShadow->Name] = std::move(LMaterial_fenceShadow);
        mMaterials[LMaterial_tree->Name] = std::move(LMaterial_tree);
        mMaterials[LMaterial_sky->Name] = std::move(LMaterial_sky);

        // PBR 测试球：前排红色塑料（电介质），后排金（金属，线性值）；从左到右粗糙度 0、0.25、0.5、0.75、1
        for (int row = 0; row < 2; row++)
        {
            for (int i = 0; i < 5; i++)
            {
                auto m = std::make_unique<Material>();
                m->Name = std::format("sphere{}{}", row, i);
                m->MatCBIndex = 10 + row * 5 + i;
                m->BaseColor = row == 0 ? XMFLOAT4{0.70f, 0.05f, 0.05f, 1.0f} : XMFLOAT4{1.0f, 0.766f, 0.336f, 1.0f};
                m->Metallic = row == 0 ? 0.0f : 1.0f;
                m->Roughness = 0.25f * i;
                m->Reflectance = 0.5f;
                mMaterials[m->Name] = std::move(m);
            }
        }
    }

    //构建mAllItems
    void BuildRenderItems()
    {
        UINT objCBIndex = 0;

        // 陆地：顶点里已经把高度算进去了（BuildLandGeometry），世界矩阵用单位阵。
        // 顶点格式是位置 + 顶点色，所以用 mColorPSO
        auto land = std::make_unique<RenderItem>();
        XMStoreFloat4x4(&land->World, XMMatrixIdentity());
        land->PSO = mColorPSO.GetAddressOf();
        land->ShadowPSO = mShadowLandPSO.GetAddressOf();
        land->GbufferPSO = mGBufferLandPSO.GetAddressOf();
        land->ObjCBIndex = objCBIndex++;
        land->Geo = mLandGeo.get();
        land->IndexCount = mLandGeo->DrawArgs["grid"].IndexCount;
        land->StartIndexLocation = mLandGeo->DrawArgs["grid"].StartIndexLocation;
        land->BaseVertexLocation = mLandGeo->DrawArgs["grid"].BaseVertexLocation;
        land->Mat = mMaterials.at("grass").get();
        mRitemLayer[(int)RenderLayer::Opaque].push_back(land.get());
        mAllItems.push_back(std::move(land));

        // 第 6、7 章那片 10×10 的木箱阵已经删掉（一半泡在水里，像一排踏脚石）；
        // 下面的铁丝网、木箱还要用立方体这段几何
        const DX_Utils::SubmeshGeometry& box = mShapeGeo->DrawArgs["box"];

        auto Lfence1 = std::make_unique<RenderItem>();
        Lfence1->PSO = mAlphaTestedPSO.GetAddressOf();
        Lfence1->ShadowPSO = mShadowCratePSO.GetAddressOf();
        Lfence1->GbufferPSO = mGBufferFencePSO.GetAddressOf();
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
            crate->PSO = mCratePSO.GetAddressOf();
            crate->ShadowPSO = mShadowCratePSO.GetAddressOf();
            crate->GbufferPSO = mGBufferCratePSO.GetAddressOf();
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
        mMarkMirror->PSO = mMarkMirrorsPSO.GetAddressOf();
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
        mMirrorClear->PSO = mMirrorClearPSO.GetAddressOf();
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
        mMirror->PSO = mMirrorPSO.GetAddressOf();
        mMirror->ObjCBIndex = objCBIndex++;
        mMirror->Geo = mShapeGeo.get();
        mMirror->IndexCount = mirrorArgs.IndexCount;
        mMirror->StartIndexLocation = mirrorArgs.StartIndexLocation;
        mMirror->BaseVertexLocation = mirrorArgs.BaseVertexLocation;
        mMirror->Mat = mMaterials.at("mirror").get();
        mRitemLayer[(int)RenderLayer::Transparent].push_back(mMirror.get());
        mAllItems.push_back(std::move(mMirror));

        auto LRefectFence1 = std::make_unique<RenderItem>();
        LRefectFence1->PSO = mReflectedFencePSO.GetAddressOf();
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
        LReflectCrate1->PSO = mReflectedCratePSO.GetAddressOf();
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
        LReflectCrate2->PSO = mReflectedCratePSO.GetAddressOf();
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
        water->PSO = mBlendPSO.GetAddressOf();
        water->ShadowPSO = mShadowLandPSO.GetAddressOf();
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
        floor->PSO = mFloorPSO.GetAddressOf();
        floor->ShadowPSO = mShadowCratePSO.GetAddressOf();
        floor->GbufferPSO = mGBufferFloorPSO.GetAddressOf();
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
        FenceShadow->PSO = mFenceShadowPSO.GetAddressOf();
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
        boxShadow1->PSO = mShadowPSO.GetAddressOf();
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
        boxShadow2->PSO = mShadowPSO.GetAddressOf();
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
        tree->PSO = mTreePSO.GetAddressOf();
        tree->ShadowPSO = mShadowTreePSO.GetAddressOf();
        tree->GbufferPSO = mGBufferTreePSO.GetAddressOf();
        tree->ObjCBIndex = objCBIndex++;
        tree->Geo = LGeo;
        tree->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        tree->IndexCount = LGeo->DrawArgs.at("points").IndexCount;
        tree->StartIndexLocation = LGeo->DrawArgs.at("points").StartIndexLocation;
        tree->BaseVertexLocation = LGeo->DrawArgs.at("points").BaseVertexLocation;
        tree->Mat = mMaterials.at("tree").get();
        mRitemLayer[(int)RenderLayer::Opaque].push_back(tree.get());
        mAllItems.push_back(std::move(tree));

        auto sky = std::make_unique<RenderItem>();
        // .xyww 以后深度恒为 1，球多大其实无所谓（包住相机就行）；天空图转的 41.4° 在着色器的 SkyDir 里
        XMStoreFloat4x4(&sky->World, XMMatrixScaling(5000.0f, 5000.0f, 5000.0f));
        auto LSkyGeo = mShapeGeo->DrawArgs.at("sphere");
        sky->PSO = mSkyPSO.GetAddressOf();
        sky->ObjCBIndex = objCBIndex++;
        sky->Geo = mShapeGeo.get();
        sky->IndexCount = LSkyGeo.IndexCount;
        sky->StartIndexLocation = LSkyGeo.StartIndexLocation;
        sky->BaseVertexLocation = LSkyGeo.BaseVertexLocation;
        sky->Mat = mMaterials.at("sky").get();
        mRitemLayer[(int)RenderLayer::Sky].push_back(sky.get());
        mAllItems.push_back(std::move(sky));

        // PBR 测试球：半径 0.8（天空球那个半径 0.5 的球放大 1.6 倍），浮在右边的水面上（左边是山）。
        // 两排隔 8：太阳很低，前排的影子拖得长，隔近了会落到后排上。机位 7 朝 -z 看，屏幕右边是 -x，所以 x 从大往小排
        const auto& sphereArgs = mShapeGeo->DrawArgs.at("sphere");
        for (int row = 0; row < 2; row++)
        {
            for (int i = 0; i < 5; i++)
            {
                auto ball = std::make_unique<RenderItem>();
                XMStoreFloat4x4(&ball->World, XMMatrixScaling(1.6f, 1.6f, 1.6f) *
                                XMMatrixTranslation(24.0f - 2.5f * i, 1.2f, row == 0 ? -30.0f : -38.0f));
                ball->PSO = mSpherePSO.GetAddressOf();
                ball->ShadowPSO = mShadowCratePSO.GetAddressOf();
                ball->GbufferPSO = mGBufferSpherePSO.GetAddressOf();
                ball->ObjCBIndex = objCBIndex++;
                ball->Geo = mShapeGeo.get();
                ball->IndexCount = sphereArgs.IndexCount;
                ball->StartIndexLocation = sphereArgs.StartIndexLocation;
                ball->BaseVertexLocation = sphereArgs.BaseVertexLocation;
                ball->Mat = mMaterials.at(std::format("sphere{}{}", row, i)).get();
                mRitemLayer[(int)RenderLayer::TestSpheres].push_back(ball.get());
                mAllItems.push_back(std::move(ball));
            }
        }
    }

    // 3 份帧资源，每份：3 个 Pass 常量（0 主 Pass、1 镜像、2 阴影 Pass）+ 每个渲染项 1 个物体常量
    void BuildFrameResources()
    {
        for (int i = 0; i < gNumFrameResources; i++)
        {
            mFrameResources.push_back(
                std::make_unique<FrameResource>(
                    mDevice.Get(),
                    3,
                    (UINT)mAllItems.size(),
                    (UINT)mWaves->VertexCount(),
                    (UINT)mMaterials.size()));
        }
    }

    /*
     * 根签名 = 着色器的"参数列表"。三种编号别混：
     *   根参数 N  —— 这张列表里的第 N 项（从 0 数），C++ 里 SetGraphicsRootXxx(N, …) 用它
     *   b0、t6 …  —— 着色器里的寄存器，HLSL 里 register(b0)、register(t6) 用它
     *   堆第 N 格 —— 描述符实际放在大堆 mHeap 的哪一格，描述符表用 HeapGpuHandle(N) 指过去
     *
     *   参数 0：根 CBV → b0，物体常量（DrawRenderItems 里每个物体设一次）
     *   参数 1：描述符表 → t0，漫反射纹理（材质的 DiffuseSrvHeapIndex 那一格，每个物体设一次）
     *   参数 2：根 CBV → b1，Pass 常量（主 Pass、镜像、阴影 Pass 各一份，换 Pass 时换）
     *   参数 3：根 CBV → b2，材质常量（每个物体设一次）
     *   参数 4：描述符表 → t1，阴影图（堆第 9 格）
     *   参数 5：描述符表 → t2，天空图（堆第 10 格）
     *   参数 6：描述符表 → t3，地板的法线贴图（材质的 NormalSrvHeapIndex，堆第 11 格）
     *   参数 7：描述符表 → t4、t5，陆地的草地、岩石（堆第 16、17 格）
     *   参数 8：描述符表 → t6、t7、t8，两张 G-Buffer + 深度缓冲（堆第 25、26、27 格，延迟渲染的光照 Pass 读）
     *   描述符表（参数 1、4～8）只给像素着色器看；根 CBV（参数 0、2、3）所有阶段都能看
     *
     *   静态采样器 s0：三线性过滤 + WRAP（CD3DX12_STATIC_SAMPLER_DESC 默认的寻址模式）
     *   静态采样器 s1：比较采样器，阴影图 PCF 用（BORDER 边框白色 = 深度 1，阴影图外面算亮）
     */
    void BuildRootSignature()
    {
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // 1 个 SRV，从 t0 开始    漫反射纹理

        CD3DX12_DESCRIPTOR_RANGE shadowRange;
        shadowRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1); // 1 个 SRV，从 t1 开始    阴影图

        CD3DX12_DESCRIPTOR_RANGE skyRange;
        skyRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2); //1 个 SRV，从 t2 开始   天空图

        CD3DX12_DESCRIPTOR_RANGE tileNormalRange;
        tileNormalRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3); //1 个 SRV，从t3开始 tileNormal贴图

        CD3DX12_DESCRIPTOR_RANGE landRange;
        landRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 4); // t4 草地、t5 岩石

        CD3DX12_DESCRIPTOR_RANGE gbufferRange;
        gbufferRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 6); //t6~7 Gbuffer0~1，t8 深度

        CD3DX12_ROOT_PARAMETER slotRootParameter[9];
        slotRootParameter[0].InitAsConstantBufferView(0);
        slotRootParameter[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        slotRootParameter[2].InitAsConstantBufferView(1);
        slotRootParameter[3].InitAsConstantBufferView(2);
        slotRootParameter[4].InitAsDescriptorTable(1, &shadowRange, D3D12_SHADER_VISIBILITY_PIXEL);
        slotRootParameter[5].InitAsDescriptorTable(1, &skyRange, D3D12_SHADER_VISIBILITY_PIXEL);
        slotRootParameter[6].InitAsDescriptorTable(1, &tileNormalRange, D3D12_SHADER_VISIBILITY_PIXEL);
        slotRootParameter[7].InitAsDescriptorTable(1, &landRange, D3D12_SHADER_VISIBILITY_PIXEL);
        slotRootParameter[8].InitAsDescriptorTable(1, &gbufferRange, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC samplers[2] = {
            CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR),
            CD3DX12_STATIC_SAMPLER_DESC(
                1, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, // 先比较，再把 0/1 双线性混合
                D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                D3D12_TEXTURE_ADDRESS_MODE_BORDER, 0.0f, 16,
                D3D12_COMPARISON_FUNC_LESS_EQUAL) // d(p) <= s(p) 算亮；边框色用默认的 OPAQUE_WHITE（深度 1），阴影图外面算亮
        };

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
            _countof(slotRootParameter), slotRootParameter,
            2, samplers,
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
        // 第③步的小灯：数量也用宏传（排在原来 3 盏后面），勾选"64 盏小灯"时 ReloadShaders 重编
        const std::string extraLights = std::to_string(mManyLights ? kExtraLights : 0);
        // 灯的数量用宏传给着色器，顺序必须和 UpdateMainPassCB 里填灯的顺序一致：先平行光，再点光源，最后聚光灯
        const D3D_SHADER_MACRO lightDefines[] = {
            {"NUM_DIR_LIGHTS", "1"},
            {"NUM_POINT_LIGHTS", "1"},
            {"NUM_SPOT_LIGHTS", "1"},
            {"NUM_EXTRA_LIGHTS", extraLights.c_str()},
            {"FOG", "1"},
            {nullptr, nullptr}
        };
        // 水：多开一个 ENV_REFLECT，只有水做环境反射
        const D3D_SHADER_MACRO waterDefines[] = {
            {"NUM_DIR_LIGHTS", "1"},
            {"NUM_POINT_LIGHTS", "1"},
            {"NUM_SPOT_LIGHTS", "1"},
            {"NUM_EXTRA_LIGHTS", extraLights.c_str()},
            {"FOG", "1"},
            {"ENV_REFLECT", "1"},
            {nullptr, nullptr}
        };
        //地板
        const D3D_SHADER_MACRO floorDefines[] = {
            {"NUM_DIR_LIGHTS", "1"},
            {"NUM_POINT_LIGHTS", "1"},
            {"NUM_SPOT_LIGHTS", "1"},
            {"NUM_EXTRA_LIGHTS", extraLights.c_str()},
            {"FOG", "1"},
            {"NORMAL_MAP", "1"},
            {nullptr, nullptr}
        };

        const D3D_SHADER_MACRO landDefines[] = {
            {"NUM_DIR_LIGHTS", "1"},
            {"NUM_POINT_LIGHTS", "1"},
            {"NUM_SPOT_LIGHTS", "1"},
            {"NUM_EXTRA_LIGHTS", extraLights.c_str()},
            {"FOG", "1"},
            {"LAND_TEX", "1"},
            {nullptr, nullptr}
        };

        // 测试球：不采样贴图，颜色全来自材质
        const D3D_SHADER_MACRO sphereDefines[] = {
            {"NUM_DIR_LIGHTS", "1"},
            {"NUM_POINT_LIGHTS", "1"},
            {"NUM_SPOT_LIGHTS", "1"},
            {"NUM_EXTRA_LIGHTS", extraLights.c_str()},
            {"FOG", "1"},
            {"NO_DIFFUSE_MAP", "1"},
            {nullptr, nullptr}
        };

        const D3D_SHADER_MACRO alphaTestDefines[] = {
            {"NUM_DIR_LIGHTS", "1"},
            {"NUM_POINT_LIGHTS", "1"},
            {"NUM_SPOT_LIGHTS", "1"},
            {"NUM_EXTRA_LIGHTS", extraLights.c_str()},
            {"FOG", "1"},
            {"ALPHA_TEST", "1"},
            {nullptr, nullptr}
        };

        // DXC 编译，Shader Model 6.6（Initialize 里检查过显卡支持）
        mCrateVSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", lightDefines, "VS", "vs_6_6");
        mCratePSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", lightDefines, "PS", "ps_6_6");
        mSpherePSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", sphereDefines, "PS", "ps_6_6");
        mAlphaTestedPSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", alphaTestDefines, "PS", "ps_6_6");
        mMirrorVSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", lightDefines, "VS_MirrorClear", "vs_6_6");
        mMirrorPSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", lightDefines, "PS_MirrorClear", "ps_6_6");
        // 语义名, 语义索引, 格式, 输入槽, 在顶点里的字节偏移, 按顶点/按实例, 实例步进
        mCrateInputLayout = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        // 陆地和水面用另一对着色器：除了变换，直接把顶点色输出
        mColorVSByteCode = CompileShader(L"HLSL\\Color.hlsl", lightDefines, "VS", "vs_6_6");
        mColorPSByteCode = CompileShader(L"HLSL\\Color.hlsl", landDefines, "PS", "ps_6_6");
        // 陆地和水面：位置（12 字节）+ 顶点色（16 字节），和 struct ColorVertex 一致。
        // 输入布局必须和顶点结构体、以及对应的 HLSL VertexIn 三者对齐
        mColorInputLayout = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        mTreeVSByteCode = CompileShader(L"HLSL\\TreeSprite.hlsl", alphaTestDefines, "VS", "vs_6_6");
        mTreePSByteCode = CompileShader(L"HLSL\\TreeSprite.hlsl", alphaTestDefines, "PS", "ps_6_6");
        mTreeGSByteCode = CompileShader(L"HLSL\\TreeSprite.hlsl", alphaTestDefines, "GS", "gs_6_6");

        mTreeInputLayout = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"SIZE", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        mHorzBlurCSByteCode = CompileShader(L"HLSL\\Blur.hlsl", nullptr, "HorzBlurCS", "cs_6_6");
        mVertBlurCSByteCode = CompileShader(L"HLSL\\Blur.hlsl", nullptr, "VertBlurCS", "cs_6_6");

        mShadowCratePSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", alphaTestDefines, "PS_Shadow", "ps_6_6");
        mShadowTreePSByteCode = CompileShader(L"HLSL\\TreeSprite.hlsl", alphaTestDefines, "PS_Shadow", "ps_6_6");

        mSkyVSByteCode = CompileShader(L"HLSL\\Sky.hlsl", lightDefines, "VS", "vs_6_6");
        mSkyPSByteCode = CompileShader(L"HLSL\\Sky.hlsl", lightDefines, "PS", "ps_6_6");

        mWaterPSByteCode = CompileShader(L"HLSL\\Color.hlsl", waterDefines, "PS", "ps_6_6");

        mFloorPSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", floorDefines, "PS", "ps_6_6");

        mTonemapVSByteCode = CompileShader(L"HLSL\\Tonemap.hlsl", nullptr, "VS", "vs_6_6");
        mTonemapPSByteCode = CompileShader(L"HLSL\\Tonemap.hlsl", nullptr, "PS", "ps_6_6");

        mBloomVSByteCode = CompileShader(L"HLSL\\Bloom.hlsl", nullptr, "VS", "vs_6_6");
        mBloomPrefilterPSByteCode = CompileShader(L"HLSL\\Bloom.hlsl", nullptr, "PS_Prefilter", "ps_6_6");
        mBloomDownPSByteCode = CompileShader(L"HLSL\\Bloom.hlsl", nullptr, "PS_Downsample", "ps_6_6");
        mBloomUpPSByteCode = CompileShader(L"HLSL\\Bloom.hlsl", nullptr, "PS_Upsample", "ps_6_6");

        // 延迟渲染：G-Buffer Pass 的 PS（只写材质和法线，不算光）。
        // 宏和对应的前向 PS 一样：地板要 NORMAL_MAP，陆地要 LAND_TEX，测试球要 NO_DIFFUSE_MAP，
        // 铁丝网和树要 ALPHA_TEST（不 clip 的话，洞也会写进 G-Buffer 和深度，挡住后面的东西）
        mGBufferCratePSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", lightDefines, "PS_GBuffer", "ps_6_6");
        mGBufferSpherePSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", sphereDefines, "PS_GBuffer", "ps_6_6");
        mGBufferFloorPSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", floorDefines, "PS_GBuffer", "ps_6_6");
        mGBufferFencePSByteCode = CompileShader(L"HLSL\\Triangle.hlsl", alphaTestDefines, "PS_GBuffer", "ps_6_6");
        mGBufferLandPSByteCode = CompileShader(L"HLSL\\Color.hlsl", landDefines, "PS_GBuffer", "ps_6_6");
        mGBufferTreePSByteCode = CompileShader(L"HLSL\\TreeSprite.hlsl", alphaTestDefines, "PS_GBuffer", "ps_6_6");

        // 全屏三角形的 VS：光照 Pass 和 G-Buffer 调试视图共用。调试视图每种一个 PS（宏 GBUFFER_VIEW = 1～6）
        mDeferredVSByteCode = CompileShader(L"HLSL\\Deferred.hlsl", lightDefines, "VS", "vs_6_6");
        for (int v = 0; v < kGBufferViews; v++)
        {
            const std::string view = std::to_string(v + 1);
            const D3D_SHADER_MACRO viewDefines[] = {
                {"NUM_DIR_LIGHTS", "1"},
                {"NUM_POINT_LIGHTS", "1"},
                {"NUM_SPOT_LIGHTS", "1"},
                {"GBUFFER_VIEW", view.c_str()},
                {nullptr, nullptr}
            };
            mGBufferViewPSByteCode[v] = CompileShader(L"HLSL\\Deferred.hlsl", viewDefines, "PS_GBufferView", "ps_6_6");
        }
        mDeferredPSByteCode = CompileShader(
            L"HLSL\\Deferred.hlsl",
            lightDefines,
            "PS",
            "ps_6_6");

        // 第③步光体积模式：全屏 Pass 只算原来的 3 盏（没有 NUM_EXTRA_LIGHTS = 0），64 盏小灯交给光体积。
        // 光体积的 VS / PS 也用这组宏：它们只要知道"小灯从第几盏开始"（1 + 1 + 1 = 3）
        const D3D_SHADER_MACRO baseLightDefines[] = {
            {"NUM_DIR_LIGHTS", "1"},
            {"NUM_POINT_LIGHTS", "1"},
            {"NUM_SPOT_LIGHTS", "1"},
            {"FOG", "1"},
            {nullptr, nullptr}
        };

        mDeferredBasePSByteCode = CompileShader(L"HLSL\\Deferred.hlsl", baseLightDefines, "PS", "ps_6_6");
        mLightVolumeVSByteCode = CompileShader(L"HLSL\\Deferred.hlsl", lightDefines, "VS_LightVolume", "vs_6_6");
        mLightVolumePSByteCode = CompileShader(L"HLSL\\Deferred.hlsl", lightDefines, "PS_LightVolume", "ps_6_6");
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
        psoDesc.RTVFormats[0] = kHdrFormat; //我画进去的那张纹理是什么格式
        psoDesc.DSVFormat = mDepthStencilFormat;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.SampleDesc.Quality = 0;

        // 木箱：位置 + 贴图坐标，PS 采样纹理
        psoDesc.InputLayout = {mCrateInputLayout.data(), (UINT)mCrateInputLayout.size()};
        psoDesc.VS = {mCrateVSByteCode->GetBufferPointer(), mCrateVSByteCode->GetBufferSize()};
        psoDesc.PS = {mCratePSByteCode->GetBufferPointer(), mCratePSByteCode->GetBufferSize()};
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&psoDesc,IID_PPV_ARGS(&mCratePSO)));

        // 测试球：和木箱一样，只是 PS 不采样贴图
        auto sphereDesc = psoDesc;
        sphereDesc.PS = {mSpherePSByteCode->GetBufferPointer(), mSpherePSByteCode->GetBufferSize()};
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&sphereDesc, IID_PPV_ARGS(&mSpherePSO)));

        auto floorDesc = psoDesc;
        floorDesc.PS = {mFloorPSByteCode->GetBufferPointer(), mFloorPSByteCode->GetBufferSize()};
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&floorDesc,IID_PPV_ARGS(&mFloorPSO)));

        auto shadowDesc = psoDesc;
        shadowDesc.RasterizerState.DepthBias = 10000; //固定偏移(单位：24位深度最小的一格)
        shadowDesc.RasterizerState.DepthBiasClamp = 0.0f; //0 = 不限上限
        shadowDesc.RasterizerState.SlopeScaledDepthBias = 1.0f; //斜率偏移，表面越斜着对灯，加的越多
        shadowDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // 铁丝网正反两面都要挡光
        shadowDesc.NumRenderTargets = 0; // 阴影 Pass 没有颜色目标：阴影图绑在 DSV 上，不是 RTV
        shadowDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        shadowDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT; // 和阴影图 DSV 的格式一致（R24_X8 是 SRV 读的时候用的）

        auto ShadowCratePSODesc = shadowDesc;
        ShadowCratePSODesc.PS = {mShadowCratePSByteCode->GetBufferPointer(), mShadowCratePSByteCode->GetBufferSize()};
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&ShadowCratePSODesc,IID_PPV_ARGS(&mShadowCratePSO)));
        auto ShadowLandPSODesc = shadowDesc;
        ShadowLandPSODesc.InputLayout = {mColorInputLayout.data(), (UINT)mColorInputLayout.size()};
        ShadowLandPSODesc.VS = {mColorVSByteCode->GetBufferPointer(), mColorVSByteCode->GetBufferSize()};
        ShadowLandPSODesc.PS = {nullptr, 0};
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&ShadowLandPSODesc,IID_PPV_ARGS(&mShadowLandPSO)));
        auto ShadowTreePSODesc = shadowDesc;
        ShadowTreePSODesc.InputLayout = {mTreeInputLayout.data(), (UINT)mTreeInputLayout.size()};
        ShadowTreePSODesc.VS = {mTreeVSByteCode->GetBufferPointer(), mTreeVSByteCode->GetBufferSize()};
        ShadowTreePSODesc.GS = {mTreeGSByteCode->GetBufferPointer(), mTreeGSByteCode->GetBufferSize()};
        ShadowTreePSODesc.PS = {mShadowTreePSByteCode->GetBufferPointer(), mShadowTreePSByteCode->GetBufferSize()};
        ShadowTreePSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&ShadowTreePSODesc,IID_PPV_ARGS(&mShadowTreePSO)));

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

        auto LSkyPSODesc = psoDesc;
        LSkyPSODesc.VS = {mSkyVSByteCode->GetBufferPointer(), mSkyVSByteCode->GetBufferSize()};
        LSkyPSODesc.PS = {mSkyPSByteCode->GetBufferPointer(), mSkyPSByteCode->GetBufferSize()};
        LSkyPSODesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // 相机在球里面，看到的是背面
        LSkyPSODesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; // 天空深度恒为 1，和清屏值相等，LESS 不让它过
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&LSkyPSODesc, IID_PPV_ARGS(&mSkyPSO)));

        auto gbufferDesc = psoDesc;

        // 两张颜色目标，照常进行深度测试和深度写入
        gbufferDesc.NumRenderTargets = 2;
        gbufferDesc.RTVFormats[0] = kGBuffer0Format;
        gbufferDesc.RTVFormats[1] = kGBuffer1Format;
        /*
        * SV_Target0 ↔ RTVFormats[0] ↔ GBuffer0
        * SV_Target1 ↔ RTVFormats[1] ↔ GBuffer1
        */

        // 下面 6 个和各自的前向 PSO 一一对应：前向那个改了哪些字段，这里就改哪些，只是 PS 换成 PS_GBuffer
        // 木箱、测试球、地砖：只换 PS
        auto gbCrateDesc = gbufferDesc;
        gbCrateDesc.PS = {mGBufferCratePSByteCode->GetBufferPointer(), mGBufferCratePSByteCode->GetBufferSize()};
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&gbCrateDesc, IID_PPV_ARGS(&mGBufferCratePSO)));

        auto gbSphereDesc = gbufferDesc;
        gbSphereDesc.PS = {mGBufferSpherePSByteCode->GetBufferPointer(), mGBufferSpherePSByteCode->GetBufferSize()};
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&gbSphereDesc, IID_PPV_ARGS(&mGBufferSpherePSO)));

        auto gbFloorDesc = gbufferDesc;
        gbFloorDesc.PS = {mGBufferFloorPSByteCode->GetBufferPointer(), mGBufferFloorPSByteCode->GetBufferSize()};
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&gbFloorDesc, IID_PPV_ARGS(&mGBufferFloorPSO)));

        // 铁丝网：和前向的 fenceDesc 一样两面都画
        auto gbFenceDesc = gbufferDesc;
        gbFenceDesc.PS = {mGBufferFencePSByteCode->GetBufferPointer(), mGBufferFencePSByteCode->GetBufferSize()};
        gbFenceDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&gbFenceDesc, IID_PPV_ARGS(&mGBufferFencePSO)));

        // 陆地：顶点格式不一样，输入布局和 VS 换成陆地的
        auto gbLandDesc = gbufferDesc;
        gbLandDesc.InputLayout = {mColorInputLayout.data(), (UINT)mColorInputLayout.size()};
        gbLandDesc.VS = {mColorVSByteCode->GetBufferPointer(), mColorVSByteCode->GetBufferSize()};
        gbLandDesc.PS = {mGBufferLandPSByteCode->GetBufferPointer(), mGBufferLandPSByteCode->GetBufferSize()};
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&gbLandDesc, IID_PPV_ARGS(&mGBufferLandPSO)));

        // 树：和前向的 LTreePSODesc 一样，输入布局、VS、GS 换成树的，图元是点，两面都画
        auto gbTreeDesc = gbufferDesc;
        gbTreeDesc.InputLayout = {mTreeInputLayout.data(), (UINT)mTreeInputLayout.size()};
        gbTreeDesc.VS = {mTreeVSByteCode->GetBufferPointer(), mTreeVSByteCode->GetBufferSize()};
        gbTreeDesc.GS = {mTreeGSByteCode->GetBufferPointer(), mTreeGSByteCode->GetBufferSize()};
        gbTreeDesc.PS = {mGBufferTreePSByteCode->GetBufferPointer(), mGBufferTreePSByteCode->GetBufferSize()};
        gbTreeDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        gbTreeDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&gbTreeDesc, IID_PPV_ARGS(&mGBufferTreePSO)));


        // 陆地和水面：位置 + 顶点色，PS 直接返回颜色
        psoDesc.InputLayout = {mColorInputLayout.data(), (UINT)mColorInputLayout.size()};
        psoDesc.VS = {mColorVSByteCode->GetBufferPointer(), mColorVSByteCode->GetBufferSize()};
        psoDesc.PS = {mColorPSByteCode->GetBufferPointer(), mColorPSByteCode->GetBufferSize()};
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&psoDesc,IID_PPV_ARGS(&mColorPSO)));

        psoDesc.BlendState.RenderTarget[0] = alpha;
        psoDesc.PS = {mWaterPSByteCode->GetBufferPointer(), mWaterPSByteCode->GetBufferSize()};
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

        // 色调映射：全屏三角形。没有顶点缓冲（InputLayout 不填），不测深度，写后台缓冲
        D3D12_GRAPHICS_PIPELINE_STATE_DESC tonemapDesc = {};
        tonemapDesc.pRootSignature = mTonemapRootSignature.Get();
        tonemapDesc.VS = {mTonemapVSByteCode->GetBufferPointer(), mTonemapVSByteCode->GetBufferSize()};
        tonemapDesc.PS = {mTonemapPSByteCode->GetBufferPointer(), mTonemapPSByteCode->GetBufferSize()};
        tonemapDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        tonemapDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // 只有一个三角形，不用管绕序
        tonemapDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        tonemapDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        tonemapDesc.DepthStencilState.DepthEnable = false; // 只是把一张图搬到屏幕上，没有前后遮挡
        tonemapDesc.SampleMask = UINT_MAX;
        tonemapDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        tonemapDesc.NumRenderTargets = 1;
        tonemapDesc.RTVFormats[0] = mBackBufferFormat; // 这个 Pass 写的是后台缓冲，不是 HDR 纹理
        tonemapDesc.DSVFormat = DXGI_FORMAT_UNKNOWN; // 不绑深度缓冲
        tonemapDesc.SampleDesc.Count = 1;
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&tonemapDesc, IID_PPV_ARGS(&mTonemapPSO)));

        // Bloom 三个 PSO：和色调映射一样是全屏三角形，换根签名、着色器、输出格式；放大那个开加法混合
        auto bloomDesc = tonemapDesc;
        bloomDesc.pRootSignature = mBloomRootSignature.Get();
        bloomDesc.VS = {mBloomVSByteCode->GetBufferPointer(), mBloomVSByteCode->GetBufferSize()};
        bloomDesc.RTVFormats[0] = kBloomFormat; // 空7
        bloomDesc.PS = {mBloomPrefilterPSByteCode->GetBufferPointer(), mBloomPrefilterPSByteCode->GetBufferSize()};
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&bloomDesc, IID_PPV_ARGS(&mBloomPrefilterPSO)));
        bloomDesc.PS = {mBloomDownPSByteCode->GetBufferPointer(), mBloomDownPSByteCode->GetBufferSize()};
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&bloomDesc, IID_PPV_ARGS(&mBloomDownPSO)));
        bloomDesc.PS = {mBloomUpPSByteCode->GetBufferPointer(), mBloomUpPSByteCode->GetBufferSize()};
        D3D12_RENDER_TARGET_BLEND_DESC add = {};
        add.BlendEnable = TRUE;
        add.SrcBlend = D3D12_BLEND_ONE; // 空8：结果 = 新算出来的 × ? + 原来的 × ?
        add.DestBlend = D3D12_BLEND_ONE;
        add.BlendOp = D3D12_BLEND_OP_ADD;
        add.SrcBlendAlpha = D3D12_BLEND_ONE;
        add.DestBlendAlpha = D3D12_BLEND_ZERO;
        add.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        add.LogicOp = D3D12_LOGIC_OP_NOOP;
        add.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        bloomDesc.BlendState.RenderTarget[0] = add;
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&bloomDesc, IID_PPV_ARGS(&mBloomUpPSO)));

        // G-Buffer 调试视图：和色调映射一样是全屏三角形（没有顶点缓冲、不测深度），
        // 换成主根签名（要用根参数 8 读 G-Buffer 和深度），写 HDR 纹理
        for (int v = 0; v < kGBufferViews; v++)
        {
            auto viewDesc = tonemapDesc;
            viewDesc.pRootSignature = mRootSignature.Get();
            viewDesc.VS = {mDeferredVSByteCode->GetBufferPointer(), mDeferredVSByteCode->GetBufferSize()};
            viewDesc.PS = {mGBufferViewPSByteCode[v]->GetBufferPointer(), mGBufferViewPSByteCode[v]->GetBufferSize()};
            viewDesc.RTVFormats[0] = kHdrFormat;
            ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&viewDesc, IID_PPV_ARGS(&mGBufferViewPSO[v])));
        }
        // 同样是全屏三角形：没有顶点输入，不做深度测试
        auto lightDesc = tonemapDesc;

        lightDesc.pRootSignature = mRootSignature.Get();

        lightDesc.VS = {
            mDeferredVSByteCode->GetBufferPointer(),
            mDeferredVSByteCode->GetBufferSize()
        };

        lightDesc.PS = {
            mDeferredPSByteCode->GetBufferPointer(),
            mDeferredPSByteCode->GetBufferSize()
        };

        lightDesc.RTVFormats[0] = kHdrFormat;

        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(
            &lightDesc,
            IID_PPV_ARGS(&mDeferredLightPSO)));

        // 光体积模式的全屏 Pass：和 mDeferredLightPSO 只差一个 PS
        lightDesc.PS = {mDeferredBasePSByteCode->GetBufferPointer(), mDeferredBasePSByteCode->GetBufferSize()};
        // CreateGraphicsPipelineState → mDeferredLightBasePSO
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&lightDesc, IID_PPV_ARGS(&mDeferredLightBasePSO)));
        // 光体积：从 lightDesc 复制，"不测深度、DSV = UNKNOWN、写 HDR、主根签名"都继承下来
        auto volumeDesc = lightDesc;
        volumeDesc.InputLayout = {mCrateInputLayout.data(), (UINT)mCrateInputLayout.size()}; // 这次画的是真网格（天空球用的是哪个布局？）
        // VS = mLightVolumeVSByteCode，PS = mLightVolumePSByteCode
        volumeDesc.VS = {mLightVolumeVSByteCode->GetBufferPointer(), mLightVolumeVSByteCode->GetBufferSize()};
        volumeDesc.PS = {mLightVolumePSByteCode->GetBufferPointer(), mLightVolumePSByteCode->GetBufferSize()};
        volumeDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        volumeDesc.BlendState.RenderTarget[0] = add;
        // CreateGraphicsPipelineState → mLightVolumePSO
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&volumeDesc, IID_PPV_ARGS(&mLightVolumePSO)));
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

    /*
     * HDR 场景纹理：场景画进它（RTV），色调映射 Pass 再读它（SRV，堆第 18 格）。
     * 约定：帧与帧之间停在 PIXEL_SHADER_RESOURCE。窗口变大小要重建
     */
    void BuildHdrResources()
    {
        D3D12_RESOURCE_DESC desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            .Alignment = 0,
            .Width = mClientWidth, // 和后台缓冲一样大
            .Height = mClientHeight,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = kHdrFormat,
            .SampleDesc = {.Count = 1, .Quality = 0},
            .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
            .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, // 要当渲染目标，必须带这个标志
        };
        D3D12_CLEAR_VALUE optClear = {.Format = kHdrFormat, .Color = {0.0f, 0.0f, 0.0f, 1.0f}}; // 每帧清成黑色
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        mHdrMap.Reset(); //窗口变大小时先释放旧的（D3DApp::OnResize 开头 Flush 过）
        ThrowIfFailed(mDevice->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &optClear,
            IID_PPV_ARGS(&mHdrMap)));
        // RTV：自己的 1 格小堆。堆只建一次，视图每次重建（资源换了，旧视图指着的是已经释放的东西）
        if (!mHdrRtvHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {
                .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                .NumDescriptors = 1,
                .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
                .NodeMask = 0,
            };
            ThrowIfFailed(mDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mHdrRtvHeap)));
        }
        // 两个视图放在 if 外面：每次换了新纹理都要重建，否则还指着上面刚释放的旧纹理
        mDevice->CreateRenderTargetView(mHdrMap.Get(), nullptr, mHdrRtvHeap->GetCPUDescriptorHandleForHeapStart());

        // SRV：大堆第 18 格
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {
            .Format = kHdrFormat,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = {.MostDetailedMip = 0, .MipLevels = 1},
        };
        mDevice->CreateShaderResourceView(mHdrMap.Get(), &srvDesc, HeapCpuHandle(18));
    }

    void BuildTonemapRootSignature()
    {
        CD3DX12_DESCRIPTOR_RANGE hdrRange;
        hdrRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
        CD3DX12_ROOT_PARAMETER param[2];
        param[0].InitAsDescriptorTable(1, &hdrRange, D3D12_SHADER_VISIBILITY_PIXEL);
        param[1].InitAsConstants(3, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL); // Exposure  Curve  BloomIntensity
        // 读 Bloom 的 mip 0 要双线性放大到全屏，和 Bloom 根签名用同一种采样器（s0）
        CD3DX12_STATIC_SAMPLER_DESC linearClamp(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                                D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        CD3DX12_ROOT_SIGNATURE_DESC
            rootSigDesc(_countof(param), param, 1, &linearClamp, D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> error;
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
        if (error != nullptr)
        {
            OutputDebugStringA((char*)error->GetBufferPointer());
        }
        ThrowIfFailed(hr);

        ThrowIfFailed(mDevice->CreateRootSignature(
            0,
            serialized->GetBufferPointer(),
            serialized->GetBufferSize(),
            IID_PPV_ARGS(&mTonemapRootSignature)));
    }

    //Bloom
    // Bloom 纹理：半分辨率、kBloomMips 级 mip。每级一个 RTV（自己的小堆）、一个 SRV（大堆 19～24 格）。
    // 约定：帧与帧之间所有 mip 都停在 PIXEL_SHADER_RESOURCE
    void BuildBloomResources()
    {
        D3D12_RESOURCE_DESC desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            .Alignment = 0,
            .Width = (std::max)(1u, mClientWidth / 2),
            .Height = (std::max)(1u, mClientHeight / 2),
            .DepthOrArraySize = 1,
            .MipLevels = kBloomMips,
            .Format = kBloomFormat,
            .SampleDesc = {.Count = 1, .Quality = 0},
            .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
            .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        };
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        mBloomMap.Reset();
        ThrowIfFailed(
            mDevice->CreateCommittedResource(&heapProps,D3D12_HEAP_FLAG_NONE,&desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&mBloomMap)));
        if (!mBloomRtvHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {
                .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                .NumDescriptors = kBloomMips,
                .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
                .NodeMask = 0,
            };
            ThrowIfFailed(mDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mBloomRtvHeap)));
        }
        for (UINT i = 0; i < kBloomMips; i++)
        {
            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {
                .Format = kBloomFormat,
                .ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D,
                .Texture2D = {
                    .MipSlice = i,
                    .PlaneSlice = 0
                }
            };
            mDevice->CreateRenderTargetView(mBloomMap.Get(), &rtvDesc, BloomRtv(i));
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {
                .Format = kBloomFormat,
                .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
                .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                .Texture2D = {.MostDetailedMip = i, .MipLevels = 1}, // 空6
            };
            mDevice->CreateShaderResourceView(mBloomMap.Get(), &srvDesc, HeapCpuHandle(19 + i));
        }
    }

    // Bloom 小堆里第 mip 格的 RTV
    CD3DX12_CPU_DESCRIPTOR_HANDLE BloomRtv(UINT mip) const
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(mBloomRtvHeap->GetCPUDescriptorHandleForHeapStart(), mip,
                                             mRTVDescriptorSize);
    }

    // Bloom 的根签名：一张表（t0 = 这一趟读的那一级）+ 6 个根常量（b0）+ 线性夹边采样器（s0）
    void BuildBloomRootSignature()
    {
        CD3DX12_DESCRIPTOR_RANGE srcRange;
        srcRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        CD3DX12_ROOT_PARAMETER param[2];
        param[0].InitAsDescriptorTable(1, &srcRange, D3D12_SHADER_VISIBILITY_PIXEL);
        param[1].InitAsConstants(6, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        CD3DX12_STATIC_SAMPLER_DESC linearClamp(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                                D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        CD3DX12_ROOT_SIGNATURE_DESC
            rootSigDesc(_countof(param), param, 1, &linearClamp, D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> error;
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
        if (error != nullptr)
        {
            OutputDebugStringA((char*)error->GetBufferPointer());
        }
        ThrowIfFailed(hr);

        ThrowIfFailed(mDevice->CreateRootSignature(
            0,
            serialized->GetBufferPointer(),
            serialized->GetBufferSize(),
            IID_PPV_ARGS(&mBloomRootSignature)));
    }

    // Bloom：进来、出去时 HDR 纹理和 Bloom 的所有 mip 都是 PIXEL_SHADER_RESOURCE
    void DoBloom()
    {
        mCommandList->SetGraphicsRootSignature(mBloomRootSignature.Get());
        mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const UINT w0 = (std::max)(1u, mClientWidth / 2), h0 = (std::max)(1u, mClientHeight / 2);
        auto mipW = [&](UINT i) { return (std::max)(1u, w0 >> i); };
        auto mipH = [&](UINT i) { return (std::max)(1u, h0 >> i); };
        // 只转第 i 级：Transition 的最后一个参数是子资源编号
        auto toRT = [&](UINT i)
        {
            auto b = CD3DX12_RESOURCE_BARRIER::Transition(mBloomMap.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                          D3D12_RESOURCE_STATE_RENDER_TARGET, i); // 空9
            mCommandList->ResourceBarrier(1, &b);
        };
        auto toSRV = [&](UINT i)
        {
            auto b = CD3DX12_RESOURCE_BARRIER::Transition(mBloomMap.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, i);
            mCommandList->ResourceBarrier(1, &b);
        };
        // 画一趟：写第 dst 级，读堆里第 srcSlot 格；srcW × srcH 是读的那一级的大小
        auto pass = [&](UINT dst, UINT srcSlot, float srcW, float srcH, ID3D12PipelineState* pso)
        {
            D3D12_VIEWPORT vp = {
                0.0f, 0.0f, static_cast<float>(mipW(dst)), static_cast<float>(mipH(dst)), 0.0f, 1.0f
            }; // 空10：视口要和写的那一级一样大
            D3D12_RECT sc = {0, 0, (LONG)mipW(dst), (LONG)mipH(dst)};
            mCommandList->RSSetViewports(1, &vp);
            mCommandList->RSSetScissorRects(1, &sc);
            auto rtv = BloomRtv(dst);
            mCommandList->OMSetRenderTargets(1, &rtv, true, nullptr);
            mCommandList->SetGraphicsRootDescriptorTable(0, HeapGpuHandle(srcSlot));
            struct
            {
                float TexelX, TexelY, Threshold, Knee, Radius;
                UINT Karis;
            } c = {
                1.0f / srcW, 1.0f / srcH, mBloomThreshold, 0.5f, 1.0f, 1u
            };
            mCommandList->SetGraphicsRoot32BitConstants(1, 6, &c, 0);
            mCommandList->SetPipelineState(pso);
            mCommandList->DrawInstanced(3, 1, 0, 0);
        };
        // ① 缩小：HDR → mip 0（带亮部提取），mip i-1 → mip i
        for (UINT i = 0; i < kBloomMips; i++)
        {
            toRT(i);
            if (i == 0) pass(0, 18, (float)mClientWidth, (float)mClientHeight, mBloomPrefilterPSO.Get()); // 空11
            else pass(i, 19 + (i - 1), (float)mipW(i - 1), (float)mipH(i - 1), mBloomDownPSO.Get()); // 空12
            toSRV(i);
        }
        // ② 放大叠加：从倒数第二级往上，每级读比它小一级的
        for (int i = (int)kBloomMips - 2; i >= 0; i--)
        {
            toRT((UINT)i);
            pass((UINT)i, 19 + (i + 1), (float)mipW(i + 1), (float)mipH(i + 1), mBloomUpPSO.Get()); // 空13
            toSRV((UINT)i);
        }
    }

    //延迟渲染
    void BuildGBufferResources()
    {
        // 堆建一次；窗口尺寸变化时，资源和视图都要重建
        if (!mGBufferRtvHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {
                .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                .NumDescriptors = 2,
                .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
                .NodeMask = 0,
            };

            ThrowIfFailed(mDevice->CreateDescriptorHeap(
                &heapDesc, IID_PPV_ARGS(&mGBufferRtvHeap)));
        }

        const DXGI_FORMAT formats[2] = {
            kGBuffer0Format, kGBuffer1Format
        };
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        for (int i = 0; i < 2; i++)
        {
            D3D12_RESOURCE_DESC desc = {
                .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                .Alignment = 0,
                .Width = mClientWidth,
                .Height = mClientHeight,
                .DepthOrArraySize = 1,
                .MipLevels = 1,
                .Format = formats[i],
                .SampleDesc = {
                    .Count = 1,
                    .Quality = 0
                },
                .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
                .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
            };
            D3D12_CLEAR_VALUE optClear = {
                .Format = formats[i],
                .Color = {0, 0, 0, 0}
            };
            mGBuffer[i].Reset();

            ThrowIfFailed(mDevice->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                &optClear,
                IID_PPV_ARGS(&mGBuffer[i])));

            mDevice->CreateRenderTargetView(mGBuffer[i].Get(), nullptr, GBufferRtv(i));

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {
                .Format = formats[i],
                .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
                .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                .Texture2D = {
                    .MostDetailedMip = 0,
                    .MipLevels = 1,
                }
            };
            mDevice->CreateShaderResourceView(mGBuffer[i].Get(), &srvDesc, HeapCpuHandle(25 + i));
        }

        // 深度缓冲的 SRV（堆第 27 格）：只读前 24 位深度，后 8 位模板不要（和阴影图一样）。
        // 深度缓冲是 D3DApp::OnResize 重建的，旧 SRV 指着已经释放的资源，所以每次都要跟着重建
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {
            .Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = {
                .MostDetailedMip = 0,
                .MipLevels = 1,
            }
        };
        mDevice->CreateShaderResourceView(mDepthStencilBuffer.Get(), &depthSrvDesc, HeapCpuHandle(27));
    }

    CD3DX12_CPU_DESCRIPTOR_HANDLE GBufferRtv(int i) const
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(mGBufferRtvHeap->GetCPUDescriptorHandleForHeapStart(), i,
                                             mRTVDescriptorSize);
    }

    void DrawDeferred(
        D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv,
        UINT tsBase)
    {
        D3D12_RESOURCE_BARRIER toRender[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(mGBuffer[0].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(mGBuffer[1].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        mCommandList->ResourceBarrier(_countof(toRender), toRender);
        // 清除两张颜色目标，清除值与创建资源时的 optClear 一致
        const float clear[4] = {0, 0, 0, 0};
        for (int i = 0; i < 2; i++)
        {
            mCommandList->ClearRenderTargetView(
                GBufferRtv(i), clear, 0, nullptr);
        }
        D3D12_CPU_DESCRIPTOR_HANDLE gbufferRtv = GBufferRtv(0);
        mCommandList->OMSetRenderTargets(2, &gbufferRtv, true, &dsv);
        DrawRenderItems(
            mRitemLayer[(int)RenderLayer::Opaque],
            DrawPass::GBuffer);
        if (mShowTestSpheres)
        {
            DrawRenderItems(
                mRitemLayer[(int)RenderLayer::TestSpheres],
                DrawPass::GBuffer);
        }

        mCommandList->EndQuery(
            mTimestampHeap.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            tsBase + 2); // T2：G-Buffer 写完

        // ② 写完后切换成读取：两张颜色目标 + 一张深度
        D3D12_RESOURCE_BARRIER toRead[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(mGBuffer[0].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mGBuffer[1].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mDepthStencilBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        };
        mCommandList->ResourceBarrier(_countof(toRead), toRead);

        // SSAO 的两趟（遮蔽 + 模糊）画在这里：G-Buffer 和深度刚变成可读

        mCommandList->EndQuery(mTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, tsBase + 3); // T3：SSAO 做完
        // ③ 光照 Pass（选了 G-Buffer 视图时换成调试视图）：全屏三角形读 G-Buffer 和深度，输出到 HDR 纹理
        mCommandList->OMSetRenderTargets(
            1, &hdrRtv, true, nullptr);
        mCommandList->SetGraphicsRootDescriptorTable(8, HeapGpuHandle(25));

        // 光体积模式：开了小灯、选了光体积、没在看调试视图
        const bool volumes = mManyLights && mLightVolumes && mGBufferView == 0;

        ID3D12PipelineState* pso = nullptr;
        if (mGBufferView > 0)
            pso = mGBufferViewPSO[mGBufferView - 1].Get(); // 调试视图
        else if (volumes)
            pso = mDeferredLightBasePSO.Get(); // 光体积模式：全屏只算原来 3 盏
        else
            pso = mDeferredLightPSO.Get(); // 平时：全屏把所有灯都算了

        mCommandList->SetPipelineState(pso);

        mCommandList->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        mCommandList->DrawInstanced(3, 1, 0, 0);

        if (volumes)
        {
            const DX_Utils::SubmeshGeometry& sphere = mShapeGeo->DrawArgs.at("sphere");
            D3D12_VERTEX_BUFFER_VIEW vbv = mShapeGeo->VertexBufferView();
            D3D12_INDEX_BUFFER_VIEW ibv = mShapeGeo->IndexBufferView();
            mCommandList->IASetVertexBuffers(0, 1, &vbv);
            mCommandList->IASetIndexBuffer(&ibv);
            mCommandList->SetPipelineState(mLightVolumePSO.Get());
            mCommandList->DrawIndexedInstanced(sphere.IndexCount, kExtraLights, sphere.StartIndexLocation,
                                               sphere.BaseVertexLocation, 0);
        }

        // ④ 后续前向绘制还要测深度、写模板：把深度状态恢复
        /* 深度：PIXEL_SHADER_RESOURCE → DEPTH_WRITE */
        auto depthBack = CD3DX12_RESOURCE_BARRIER::Transition(
            mDepthStencilBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        mCommandList->ResourceBarrier(1, &depthBack);

        mCommandList->OMSetRenderTargets(
            1, &hdrRtv, true, &dsv);

        mCommandList->EndQuery(
            mTimestampHeap.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            tsBase + 4); // T4：光照 / 调试视图完成
    }

    // ======================== 成员变量 ========================

    // 太阳光前进的方向：和天空图里画的太阳对上（仰角 18.6°、方位角 -17.4°），夕阳的影子拉得长
    // 指向太阳 = (cos18.6°·sin(-17.4°), sin18.6°, cos18.6°·cos(-17.4°))，光的方向取反
    XMFLOAT3 mSunDir = {0.2835f, -0.319f, -0.9045f};

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
    bool mVsync = false; // 面板"垂直同步"：默认关
    bool mBlurEnabled = false; // B 键开关，默认关
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
    ComPtr<ID3D12DescriptorHeap> mHeap; // 着色器可见的 CBV/SRV/UAV 堆
    ComPtr<ID3D12RootSignature> mRootSignature;
    // 热重载：当前用的是哪个版本的 HLSL、等待确认的新版本、上次查目录的时间
    std::filesystem::file_time_type mShaderTime = NewestShaderTime();
    std::filesystem::file_time_type mPendingShaderTime{};
    float mLastShaderPoll = 0.0f;

    ComPtr<ID3D12PipelineState> mCratePSO;
    ComPtr<ID3D12PipelineState> mSpherePSO; // PBR 测试球
    ComPtr<ID3DBlob> mSpherePSByteCode;
    ComPtr<ID3D12PipelineState> mAlphaTestedPSO;

    ComPtr<ID3DBlob> mCrateVSByteCode;
    ComPtr<ID3DBlob> mCratePSByteCode;
    ComPtr<ID3DBlob> mAlphaTestedPSByteCode;
    ComPtr<ID3DBlob> mMirrorVSByteCode;
    ComPtr<ID3DBlob> mMirrorPSByteCode;
    ComPtr<ID3DBlob> mWaterPSByteCode;
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
    double mPassMsSum[kTimestampCount - 1] = {}; // 5 段各自累加的毫秒数（阴影图、不透明、镜子、透明、模糊）
    int mPassSamples = 0; // 已经攒了几帧

    //阴影功能相关成员变量
    ComPtr<ID3D12Resource> mShadowMap; //阴影贴图
    ComPtr<ID3D12DescriptorHeap> mShadowDsvHeap; //只放阴影图的 DSV，1格
    D3D12_VIEWPORT mShadowViewPort = {0.f, 0.f, (float)kShadowMapSize, (float)kShadowMapSize, 0.0f, 1.0f};
    D3D12_RECT mShadowScissorRect = {0, 0, (LONG)kShadowMapSize, (LONG)kShadowMapSize};
    XMFLOAT4X4 mLightViewProj = {}; //阴影Pass用：世界->灯的裁剪空间
    XMFLOAT4X4 mShadowTransform = {}; //主Pass用：世界->阴影图的(u,v,深度)
    ComPtr<ID3DBlob> mShadowCratePSByteCode;
    ComPtr<ID3DBlob> mShadowTreePSByteCode;
    ComPtr<ID3D12PipelineState> mShadowCratePSO; //木箱、地板、铁丝网
    ComPtr<ID3D12PipelineState> mShadowLandPSO; //陆地
    ComPtr<ID3D12PipelineState> mShadowTreePSO; //树

    //Sky
    ComPtr<ID3D12PipelineState> mSkyPSO;
    ComPtr<ID3DBlob> mSkyVSByteCode;
    ComPtr<ID3DBlob> mSkyPSByteCode;

    //floor
    ComPtr<ID3DBlob> mFloorPSByteCode;
    ComPtr<ID3D12PipelineState> mFloorPSO;

    //ImGui
    static constexpr int kImGuiFirstSlot = 12, kImGuiSlotCount = 4;
    std::vector<int> mImGuiFreeSlots;
    float mSunAzimuth = 0.0f, mSunElevation = 0.0f; // 单位：度
    int mCameraPreset = -1; // 当前在第几个预设机位，-1 = 自由视角
    float mExposureEV = 1.0f; // 曝光，单位 EV：+1 = 亮一倍
    int mToneCurve = 2; // 0 截断 / 1 Reinhard / 2 ACES / 3 AgX
    float mSunIntensity = 1.0f; // 太阳强度倍数
    bool mShowTestSpheres = false; // 面板勾选才画 PBR 测试球

    // HDR 成员：HDR 场景纹理 + 它的 RTV 小堆，色调映射的根签名、着色器、PSO
    ComPtr<ID3D12Resource> mHdrMap;
    ComPtr<ID3D12DescriptorHeap> mHdrRtvHeap;
    ComPtr<ID3D12RootSignature> mTonemapRootSignature;
    ComPtr<ID3DBlob> mTonemapVSByteCode;
    ComPtr<ID3DBlob> mTonemapPSByteCode;
    ComPtr<ID3D12PipelineState> mTonemapPSO;

    // Bloom
    static constexpr UINT kBloomMips = 6; // 2560×1440 下是 1280×720 … 40×22
    static constexpr DXGI_FORMAT kBloomFormat = DXGI_FORMAT_R11G11B10_FLOAT; // 不要 alpha，每像素 4 字节
    ComPtr<ID3D12Resource> mBloomMap;
    ComPtr<ID3D12DescriptorHeap> mBloomRtvHeap;
    ComPtr<ID3D12RootSignature> mBloomRootSignature;
    ComPtr<ID3DBlob> mBloomVSByteCode, mBloomPrefilterPSByteCode, mBloomDownPSByteCode, mBloomUpPSByteCode;
    ComPtr<ID3D12PipelineState> mBloomPrefilterPSO, mBloomDownPSO, mBloomUpPSO;
    bool mBloomEnabled = true;
    float mBloomIntensity = 0.1f;
    float mBloomThreshold = 1.0f;

    //延迟渲染
    ComPtr<ID3D12Resource> mGBuffer[2];
    ComPtr<ID3D12DescriptorHeap> mGBufferRtvHeap;
    // G-Buffer Pass 的 6 个 PS：木箱、测试球、地砖、铁丝网、陆地、树（和各自的前向 PS 同一组宏）
    ComPtr<ID3DBlob> mGBufferCratePSByteCode, mGBufferSpherePSByteCode, mGBufferFloorPSByteCode;
    ComPtr<ID3DBlob> mGBufferFencePSByteCode, mGBufferLandPSByteCode, mGBufferTreePSByteCode;
    ComPtr<ID3D12PipelineState> mGBufferCratePSO, mGBufferSpherePSO, mGBufferFloorPSO;
    ComPtr<ID3D12PipelineState> mGBufferFencePSO, mGBufferLandPSO, mGBufferTreePSO;
    bool mDeferred = false; // 面板"延迟渲染"：勾上 = G-Buffer Pass + 光照 Pass，不勾 = 原来的前向
    // 光照 Pass：全屏三角形，读 G-Buffer 和深度、反推世界坐标再算光，写 HDR 纹理（VS 和调试视图共用 mDeferredVSByteCode）
    ComPtr<ID3DBlob> mDeferredPSByteCode;
    ComPtr<ID3D12PipelineState> mDeferredLightPSO;
    // G-Buffer 调试视图：基础色、法线、粗糙度、金属度、深度、世界坐标（面板上的下拉框选，0 = 关 = 正常算光）
    static constexpr int kGBufferViews = 6;
    int mGBufferView = 0;
    ComPtr<ID3DBlob> mDeferredVSByteCode;
    ComPtr<ID3DBlob> mGBufferViewPSByteCode[kGBufferViews];
    ComPtr<ID3D12PipelineState> mGBufferViewPSO[kGBufferViews];
    // 第③步：64 盏小灯（面板勾选，Lights[3..66]）。延迟 + 小灯时可以选光体积：每盏灯画一个球，只算球盖住的像素
    bool mManyLights = false;
    bool mLightVolumes = true;
    ComPtr<ID3DBlob> mDeferredBasePSByteCode; // 光体积模式的全屏 Pass：只算原来的 3 盏（baseLightDefines 编的 PS）
    ComPtr<ID3D12PipelineState> mDeferredLightBasePSO;
    ComPtr<ID3DBlob> mLightVolumeVSByteCode, mLightVolumePSByteCode;
    ComPtr<ID3D12PipelineState> mLightVolumePSO;

    // SSAO（只在延迟渲染下有：法线和深度直接读 G-Buffer）。面板开关
    bool mSsao = true;
    int mSsaoBlurCount = 2; // 双边模糊做几次（横 + 竖算一次）
    // 遮蔽图两张，半分辨率 kAoFormat：[0] 遮蔽 Pass 写、最终结果也在它里面（堆第 28 格），[1] 模糊时来回倒用（堆第 29 格）
    ComPtr<ID3D12Resource> mSsaoMap[2];
    ComPtr<ID3D12DescriptorHeap> mSsaoRtvHeap;
    ComPtr<ID3DBlob> mSsaoPSByteCode;
    ComPtr<ID3D12PipelineState> mSsaoPSO;
    ComPtr<ID3DBlob> mSsaoBlurPSByteCode[2]; // [0] 横，[1] 竖
    ComPtr<ID3D12PipelineState> mSsaoBlurPSO[2];
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
