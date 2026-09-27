#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <memory>
#include <wrl/client.h>
#include "Common/UploadBuffer.h"
using namespace DirectX;
using namespace Microsoft::WRL;

//HLSL常量
struct ObjectConstants
{
    XMFLOAT4X4 MVP; //需要和HLSL的Cbuffer对应
};

struct FrameResource
{
    FrameResource(const FrameResource&) = delete;
    FrameResource& operator=(const FrameResource&) = delete;
    FrameResource(ID3D12Device* device, UINT objectCount);

    ComPtr<ID3D12CommandAllocator> CmdListAlloc; //这一帧专用的命令分配器
    std::unique_ptr<UploadBuffer<ObjectConstants>> ObjectCB; //这一帧专用的常量缓冲
    UINT64 Fence = 0;
};
