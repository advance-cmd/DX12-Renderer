#pragma once
// ThrowIfFailed：检查 D3D / DXGI 调用返回的 HRESULT，失败就抛出 DxException。
// 异常里带着出错的那句代码、文件名、行号和错误描述，WinMain 里 catch 住弹窗显示。
// 仿照书中 d3dUtil.h 的写法：宏和异常类放在头文件里，辅助函数的实现放在 DxUtil.cpp。

#include <windows.h>
#include <comdef.h>     // _com_error：把 HRESULT 翻译成可读的错误描述
#include <cstdio>
#include <d3d12.h>
#include <string>
#include <unordered_map>
#include <DirectXCollision.h>  // DirectX::BoundingBox
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

// 创建默认堆（显存）里的缓冲区，并借上传堆把 initData 复制进去（实现在 DxUtil.cpp）
// 注意：uploadBuffer 要一直活到命令列表执行完（Flush 之后）才能释放
ComPtr<ID3D12Resource> CreateDefaultBuffer(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const void* initData,
    UINT64 byteSize,
    ComPtr<ID3D12Resource>& uploadBuffer);

//编译着色器辅助函数
ComPtr<ID3DBlob> CompileShader(
    const std::wstring& filename,
    const D3D_SHADER_MACRO* defines,
    const std::string& entrypoint,
    const std::string& target);

//将离线编译的Shader加载到应用程序
ComPtr<ID3DBlob> LoadBinary(const std::wstring& filename);

UINT CalcConstantBufferByteSize(UINT BufferSize);

namespace DX_Utils
{
    // 大缓冲区里的一段：一个物体用哪几个索引、顶点编号从哪里开始算（书 6.10）
    struct SubmeshGeometry
    {
        UINT IndexCount = 0;
        UINT StartIndexLocation = 0;
        INT BaseVertexLocation = 0;   // 每个索引读顶点之前先加上它

        // 包围盒，后面的章节才用
        DirectX::BoundingBox Bounds;
    };

    // 一对顶点缓冲 + 索引缓冲，外加画里面各个物体要用的参数；多个物体可以挤在同一对缓冲区里
    struct MeshGeometry
    {
        std::string Name;

        // 内存里的副本，以后拾取、碰撞检测时 CPU 要用。顶点和索引的格式由用户定，所以用不带类型的 Blob
        ComPtr<ID3DBlob> VertexBufferCPU = nullptr;
        ComPtr<ID3DBlob> IndexBufferCPU = nullptr;

        ComPtr<ID3D12Resource> VertexBufferGPU = nullptr;
        ComPtr<ID3D12Resource> IndexBufferGPU = nullptr;

        // 上传缓冲区要活到复制命令执行完（Flush 之后），再调用 DisposeUploaders 释放
        ComPtr<ID3D12Resource> VertexBufferUploader = nullptr;
        ComPtr<ID3D12Resource> IndexBufferUploader = nullptr;

        // 算两个视图要用的参数
        UINT VertexByteStride = 0;
        UINT VertexBufferByteSize = 0;
        DXGI_FORMAT IndexFormat = DXGI_FORMAT_R16_UINT;
        UINT IndexBufferByteSize = 0;

        // 名字 → 这个物体在缓冲区里是哪一段，比如 DrawArgs["box"]
        std::unordered_map<std::string, SubmeshGeometry> DrawArgs;

        // 视图每次调用时现算。返回的是临时对象，要先存成变量才能取地址（C2102）
        D3D12_VERTEX_BUFFER_VIEW VertexBufferView() const
        {
            D3D12_VERTEX_BUFFER_VIEW vbv;
            vbv.BufferLocation = VertexBufferGPU->GetGPUVirtualAddress();
            vbv.StrideInBytes = VertexByteStride;
            vbv.SizeInBytes = VertexBufferByteSize;
            return vbv;
        }

        D3D12_INDEX_BUFFER_VIEW IndexBufferView() const
        {
            D3D12_INDEX_BUFFER_VIEW ibv;
            ibv.BufferLocation = IndexBufferGPU->GetGPUVirtualAddress();
            ibv.Format = IndexFormat;
            ibv.SizeInBytes = IndexBufferByteSize;
            return ibv;
        }

        // 数据复制到默认堆以后，上传缓冲区就没用了
        void DisposeUploaders()
        {
            VertexBufferUploader = nullptr;
            IndexBufferUploader = nullptr;
        }
    };

    struct Texture
    {
        std::string Name;   //纹理的名字，查 map 时用
        std::wstring Filename;  //dds 文件路径。注意要用宽字符串，因为 CreateDDSTextureFromFile12 要的是 wchar_t*
        ComPtr<ID3D12Resource> Resource;    //纹理本体，放在默认堆里
        ComPtr<ID3D12Resource> UploadHeap;  //上传缓冲区，Flush 之后就可以释放
    };
}
