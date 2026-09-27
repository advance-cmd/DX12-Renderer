#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <memory>
#include <wrl/client.h>
#include "Common/UploadBuffer.h"
using namespace DirectX;
using namespace Microsoft::WRL;

inline constexpr int gNumFrameResources = 3;

struct ObjectConstants { XMFLOAT4X4 World; };      // 每个物体一份
struct PassConstants   { XMFLOAT4X4 ViewProj; };   // 每个 Pass 一份，所有物体共用

struct FrameResource
{
    FrameResource(const FrameResource&) = delete;
    FrameResource& operator=(const FrameResource&) = delete;
    FrameResource(ID3D12Device* device,UINT passCount, UINT objectCount);

    ComPtr<ID3D12CommandAllocator> CmdListAlloc; //这一帧专用的命令分配器
    std::unique_ptr<UploadBuffer<ObjectConstants>> ObjectCB; //这一帧专用的常量缓冲
    UINT64 Fence = 0;
    std::unique_ptr<UploadBuffer<PassConstants>> PassCB;
};
