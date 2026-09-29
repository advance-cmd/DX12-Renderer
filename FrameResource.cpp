#include "FrameResource.h"

FrameResource::FrameResource(ID3D12Device* device, UINT passCount, UINT objectCount, UINT waveVertCount,UINT materialCount)
{
    // 类型要和命令列表一致（DIRECT）
    ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(CmdListAlloc.GetAddressOf())));

    // 最后一个参数 true：常量缓冲，每个元素按 256 字节对齐
    PassCB = std::make_unique<UploadBuffer<PassConstants>>(device, passCount, true);
    ObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(device, objectCount, true);
    // false：这是顶点缓冲，顶点之间要紧凑排列，不能按 256 字节对齐
    WavesVB = std::make_unique<UploadBuffer<ColorVertex>>(device, waveVertCount, false);
    MaterialCB =
    std::make_unique<UploadBuffer<MaterialConstants>>(
        device,
        materialCount,
        true); // true：按常量缓冲处理
}
