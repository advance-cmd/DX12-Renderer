#include "DxUtil.h"

#include <d3dcompiler.h>
#include <fstream>
#include <vector>

ComPtr<ID3D12Resource> CreateDefaultBuffer(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const void* initData,
    UINT64 byteSize,
    ComPtr<ID3D12Resource>& uploadBuffer)
{
    ComPtr<ID3D12Resource> defaultBuffer;

    // /permissive- 下不能对临时对象取地址（C2102），所以先存成具名变量
    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);

    ThrowIfFailed(device->CreateCommittedResource(
        &defaultHeap, // ① 放在哪种堆
        D3D12_HEAP_FLAG_NONE, // ② 堆的额外选项
        &bufferDesc, // ③ 资源长什么样
        D3D12_RESOURCE_STATE_COMMON, // ④ 初始状态
        nullptr, // ⑤ 优化清除值
        IID_PPV_ARGS(defaultBuffer.GetAddressOf()))); // ⑥ 结果写到哪

    //创建一个处于中介位置的上传堆
    ThrowIfFailed(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(uploadBuffer.GetAddressOf())));

    //描述我们希望复制到默认缓冲区中的数据
    D3D12_SUBRESOURCE_DATA subResourceData = {
        .pData = initData,  // 数据在 CPU 内存的哪里
        .RowPitch = static_cast<LONG_PTR>(byteSize),    // 一行有多少字节
        .SlicePitch = static_cast<LONG_PTR>(byteSize)   // 一层有多少字节
    };

    auto toCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
        defaultBuffer.Get(),
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->ResourceBarrier(1, &toCopyDest);

    UpdateSubresources<1>(
        cmdList,
        defaultBuffer.Get(),
        uploadBuffer.Get(),
        0,
        0,
        1,
        &subResourceData);

    auto toGenericRead = CD3DX12_RESOURCE_BARRIER::Transition(
        defaultBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    cmdList->ResourceBarrier(1, &toGenericRead);

    return defaultBuffer;
}

ComPtr<ID3DBlob> LoadBinary(const std::wstring& filename)
{
    std::ifstream fin(filename, std::ios::binary);
    
    fin.seekg (0, std::ios_base::end);
    std::ifstream::pos_type fileSize = fin.tellg();
    fin.seekg (0, std::ios_base::beg);
    
    ComPtr<ID3DBlob> blob;
    ThrowIfFailed(D3DCreateBlob(fileSize, blob.GetAddressOf()));
    
    fin.read((char*)blob->GetBufferPointer(), fileSize);
    return blob;
}

UINT CalcConstantBufferByteSize(UINT BufferSize)
{
    return 255 + BufferSize & ~255;
}

ComPtr<ID3DBlob> CompileShader(const std::wstring& filename, const D3D_SHADER_MACRO* defines,
                               const std::string& entrypoint, const std::string& target)
{
    //若处于调试模式，则使用调试标志
    UINT compileFlags = 0;
#if defined (DEBUG) || defined (_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    HRESULT hr = S_OK;

    ComPtr<ID3DBlob> byteCode; // 接编译好的字节码（ppCode）
    ComPtr<ID3DBlob> errors; // 接错误 / 警告信息（ppErrorMsgs）

    hr = D3DCompileFromFile(
        filename.c_str(), defines,D3D_COMPILE_STANDARD_FILE_INCLUDE, entrypoint.c_str(), target.c_str(), compileFlags,
        0, &byteCode, &errors);
    
    //将错误信息输出到调试窗口
    if (errors != nullptr)
    {
        OutputDebugStringA((char*)errors->GetBufferPointer());
    }
    
    ThrowIfFailed(hr);
    
    return byteCode;
}
