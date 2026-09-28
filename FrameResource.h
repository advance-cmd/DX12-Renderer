#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <memory>
#include "Common/UploadBuffer.h"   // 也带来了 ComPtr 和 ThrowIfFailed（来自 DxUtil.h）

// 帧资源的份数：CPU 最多领先 GPU 这么多帧。RenderItem::NumFramesDirty 的初始值也用它
inline constexpr int gNumFrameResources = 3;

// 下面两个结构体必须和 HLSL 里的 ObjectConstants / PassConstants 一一对应（矩阵写入前要转置）
struct ObjectConstants
{
    DirectX::XMFLOAT4X4 World;      // 每个物体一份：局部 → 世界
};

struct PassConstants
{
    DirectX::XMFLOAT4X4 ViewProj;   // 每个 Pass 一份，所有物体共用：世界 → 裁剪空间
};

struct ColorVertex
{
    DirectX::XMFLOAT3 Pos;
    DirectX::XMFLOAT4 Color;
};

// 一帧要用到的、CPU 会改写的东西各备一份。
// 3 份轮流用：CPU 改第 k 份时，GPU 可能还在读另外两份，互不干扰
struct FrameResource
{
    FrameResource(ID3D12Device* device, UINT passCount, UINT objectCount,UINT waveVertCount);
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

    // 提交这一帧命令后记下的 Fence 值。0 表示还没提交过；
    // 轮回到这一份时，GPU 的 Fence 还没到这个值，就说明它还在用，要等
    UINT64 Fence = 0;
};
