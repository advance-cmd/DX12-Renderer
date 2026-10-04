#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <memory>
#include "Common/UploadBuffer.h"   // 也带来了 ComPtr 和 ThrowIfFailed（来自 DxUtil.h）
using namespace DirectX;

// 帧资源的份数：CPU 最多领先 GPU 这么多帧。RenderItem::NumFramesDirty 的初始值也用它
inline constexpr int gNumFrameResources = 3;
inline constexpr int MaxLights = 100;

struct Light
{
    XMFLOAT3 Strength;
    float FalloffStart;

    XMFLOAT3 Direction;
    float FalloffEnd;

    XMFLOAT3 Position;
    float SpotPower;
};
static_assert(sizeof(Light) == 48); //布局检查

// 下面两个结构体必须和 HLSL 里的 ObjectConstants / PassConstants 一一对应（矩阵写入前要转置）
struct ObjectConstants
{
    XMFLOAT4X4 World;
    XMFLOAT4X4 WorldInvTranspose;   //这里的逆转置矩阵是为了Normal准备的，为了避免破坏方向性
};

struct PassConstants
{
    XMFLOAT4X4 ViewProj;
    XMFLOAT4X4 ShadowTransform;

    XMFLOAT3 EyePosW;
    float padding;

    XMFLOAT4 AmbientLight;
    XMFLOAT4 FogColor;
    float FogStart;
    float FogRange;
    XMFLOAT2 FogPad;
    Light Lights[MaxLights];
};

struct Material
{
    std::string Name;
    int MatCBIndex;
    int NumFramesDirty = gNumFrameResources;

    XMFLOAT4 DiffuseAlbedo;
    XMFLOAT3 FresnelR0;
    float Roughness;
    int DiffuseSrvHeapIndex = 0;
    int NormalSrvHeapIndex = -1;
};

struct MaterialConstants
{
    XMFLOAT4 DiffuseAlbedo;
    XMFLOAT3 FresnelR0;
    float Roughness;
};

struct ColorVertex
{
    XMFLOAT3 Pos;
    XMFLOAT3 Normal;
    XMFLOAT4 Color;
};

// 一帧要用到的、CPU 会改写的东西各备一份。
// 3 份轮流用：CPU 改第 k 份时，GPU 可能还在读另外两份，互不干扰
struct FrameResource
{
    FrameResource(ID3D12Device* device, UINT passCount, UINT objectCount, UINT waveVertCount,UINT materialCount);
    FrameResource(const FrameResource&) = delete;
    FrameResource& operator=(const FrameResource&) = delete;

    // GPU 执行完这一帧的命令之前不能 Reset 分配器，所以每帧要有自己的
    ComPtr<ID3D12CommandAllocator> CmdListAlloc;

    // GPU 读完之前不能改常量，所以每帧也要有自己的一份
    std::unique_ptr<UploadBuffer<PassConstants>> PassCB;
    // 每个渲染项占一个元素（按 ObjCBIndex）
    std::unique_ptr<UploadBuffer<ObjectConstants>> ObjectCB;
    // 水面的动态顶点缓冲：每帧 CPU 都要重写，而 GPU 可能还在读上一帧的，所以每份帧资源各有一份
    std::unique_ptr<UploadBuffer<ColorVertex>> WavesVB;
    //材质缓冲
    std::unique_ptr<UploadBuffer<MaterialConstants>> MaterialCB;

    // 提交这一帧命令后记下的 Fence 值。0 表示还没提交过；
    // 轮回到这一份时，GPU 的 Fence 还没到这个值，就说明它还在用，要等
    UINT64 Fence = 0;
};
