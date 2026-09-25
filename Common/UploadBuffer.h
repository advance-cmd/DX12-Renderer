#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include "DxUtil.h"

template<typename T>
class UploadBuffer
{
public:
    UploadBuffer(const UploadBuffer&) = delete;
    UploadBuffer& operator=(const UploadBuffer&) = delete;
    //初始化
    UploadBuffer(ID3D12Device* device,UINT elementCount,bool isConstantBuffer) :mIsConstantBuffer(isConstantBuffer)
    {
        mElementByteSize = sizeof(T);
        
        if (isConstantBuffer)
        {
            mElementByteSize = CalcConstantBufferByteSize(sizeof(T));
        }
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(mElementByteSize * elementCount);

        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mUploadBuffer)));

        ThrowIfFailed(mUploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedData)));
    }
    ~UploadBuffer()
    {
        if (mUploadBuffer != nullptr)
        {
            mUploadBuffer->Unmap(0, nullptr);
        }
        mMappedData = nullptr;
    }
    
    ID3D12Resource* Resource() const
    {
        return mUploadBuffer.Get();
    }
    
    //每帧更新
    void CopyData(int elementIndex,const T& data)
    {
        memcpy(&mMappedData[elementIndex * mElementByteSize], &data, sizeof(T));
    }
private:
    ComPtr<ID3D12Resource> mUploadBuffer;
    BYTE* mMappedData = nullptr;
    
    UINT mElementByteSize = 0;
    bool mIsConstantBuffer = false;
};
