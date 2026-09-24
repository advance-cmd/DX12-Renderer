#pragma once
// ThrowIfFailed：检查 D3D / DXGI 调用返回的 HRESULT，失败就抛出 DxException。
// 异常里带着出错的那句代码、文件名、行号和错误描述，WinMain 里 catch 住弹窗显示。
// 仿照书中 d3dUtil.h 的写法，做成纯头文件，不需要额外的 .cpp。

#include <windows.h>
#include <comdef.h>     // _com_error：把 HRESULT 翻译成可读的错误描述
#include <cstdio>
#include <d3d12.h>
#include <string>
#include <d3dx12/d3dx12_barriers.h>
#include <d3dx12/d3dx12_core.h>
#include <d3dx12/d3dx12_resource_helpers.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class DxException
{
public:
    DxException() = default;

    DxException(HRESULT hr, const std::wstring& functionName, const std::wstring& filename, int lineNumber)
        : ErrorCode(hr), FunctionName(functionName), Filename(filename), LineNumber(lineNumber)
    {
    }

    std::wstring ToString() const
    {
        // 错误码按 0x887A0005 这种十六进制格式给出，方便直接拿去搜
        wchar_t code[16];
        swprintf_s(code, L"0x%08X", static_cast<unsigned>(ErrorCode));
        _com_error err(ErrorCode);
        return FunctionName + L"\n\n位置：" + Filename + L" 第 " + std::to_wstring(LineNumber) + L" 行"
            + L"\n错误码：" + code + L"\n描述：" + err.ErrorMessage();
    }

    HRESULT ErrorCode = S_OK;
    std::wstring FunctionName;
    std::wstring Filename;
    int LineNumber = -1;
};

// do { } while (0) 包一层，让宏在 if/else 里当一条普通语句用也不会出错
#ifndef ThrowIfFailed
#define ThrowIfFailed(x)                                                           \
    do                                                                             \
    {                                                                              \
        HRESULT hrCheck_ = (x);                                                    \
        if (FAILED(hrCheck_)) { throw DxException(hrCheck_, L#x, __FILEW__, __LINE__); } \
    } while (0)
#endif

ComPtr<ID3D12Resource> CreateDefaultBuffer(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const void* initData,
    UINT64 byteSize,
    ComPtr<ID3D12Resource>& uploadBuffer)
{
    ComPtr<ID3D12Resource> defaultBuffer;

    ThrowIfFailed(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), // ① 放在哪种堆
        D3D12_HEAP_FLAG_NONE, // ② 堆的额外选项
        &CD3DX12_RESOURCE_DESC::Buffer(byteSize), // ③ 资源长什么样
        D3D12_RESOURCE_STATE_COMMON, // ④ 初始状态
        nullptr, // ⑤ 优化清除值
        IID_PPV_ARGS(defaultBuffer.GetAddressOf()))); // ⑥ 结果写到哪

    //创建一个处于中介位置的上传堆
    ThrowIfFailed(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(byteSize),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(uploadBuffer.GetAddressOf())));

    //描述我们希望复制到默认缓冲区中的数据
    D3D12_SUBRESOURCE_DATA subResourceData = {
        .pData = initData,  // 数据在 CPU 内存的哪里
        .RowPitch = static_cast<LONG_PTR>(byteSize),    // 一行有多少字节
        .SlicePitch = static_cast<LONG_PTR>(byteSize)   // 一层有多少字节
    };
    
    cmdList->ResourceBarrier(
        1,
        &CD3DX12_RESOURCE_BARRIER::Transition(
            defaultBuffer.Get(),
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_DEST));
    
    UpdateSubresources<1>(
        cmdList,
        defaultBuffer.Get(),
        uploadBuffer.Get(),
        0,
        0,
        1,
        &subResourceData);
    
    cmdList->ResourceBarrier(
        1,
        &CD3DX12_RESOURCE_BARRIER::Transition(
            defaultBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_GENERIC_READ));
    
    return defaultBuffer;
}
