#include "DxUtil.h"

#include <d3dcompiler.h>
#include <fstream>
#include <dxcapi.h>
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
    //编译器对象建一次就够，用static
    static ComPtr<IDxcUtils> utils;
    static ComPtr<IDxcCompiler3> compiler;
    static ComPtr<IDxcIncludeHandler> includeHandler;
    
    if (compiler == nullptr)
    {
        ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils,IID_PPV_ARGS(&utils)));
        ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler,IID_PPV_ARGS(&compiler)));
        ThrowIfFailed(utils->CreateDefaultIncludeHandler(&includeHandler)); //负责处理#inlcude "LightingUtil.hlsl"
    }
    
    // 读源文件。我们的 .hlsl 里有中文注释，按 UTF-8 读
    ComPtr<IDxcBlobEncoding> source;
    ThrowIfFailed(utils->LoadFile(filename.c_str(),nullptr,&source));
    DxcBuffer buffer = {
        .Ptr = source->GetBufferPointer(),
        .Size = source->GetBufferSize(),
        .Encoding = DXC_CP_UTF8
    };
    
    // 参数就是命令行，比如 dxc HLSL\Sky.hlsl -E PS -T ps_6_6 -D FOG=1
    std::wstring wEntry = ToWide(entrypoint.c_str());
    std::wstring wTarget = ToWide(target.c_str());
    std::vector<std::wstring> defineStrs;
    for (const D3D_SHADER_MACRO* d = defines; d!=nullptr && d->Name!=nullptr; d++)
    {
        defineStrs.push_back(ToWide(d->Name) + L"=" +ToWide(d->Definition));
    }
    std::vector<LPCWSTR> args = {filename.c_str(), L"-E", wEntry.c_str(), L"-T", wTarget.c_str()};
    for (const std::wstring& d : defineStrs)
    {
        args.push_back(L"-D");
        args.push_back(d.c_str());
    }
#if defined(DEBUG) || defined(_DEBUG)
    args.push_back(L"-Zi");           // 带调试信息（RenderDoc / PIX 里能看源码）
    args.push_back(L"-Qembed_debug"); // 调试信息直接塞进字节码，不另存 pdb
    args.push_back(L"-Od");           // 不优化
#endif
    ComPtr<IDxcResult> result;
    ThrowIfFailed(compiler->Compile(&buffer,args.data(),(UINT32)args.size(),includeHandler.Get(),IID_PPV_ARGS(&result)));
    
    // 报错和警告输出到调试窗口
    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (errors != nullptr && errors->GetStringLength() > 0)
    {
        OutputDebugStringA(errors->GetStringPointer());
    }
    HRESULT status = S_OK;
    ThrowIfFailed(result->GetStatus(&status));
    if (FAILED(status))
    {
        // DXC 的报错是 UTF-8，转成宽字符塞进异常：弹窗里直接看到第几行错了什么
        const char* msg = (errors != nullptr) ? errors->GetStringPointer() : "";
        const int len = MultiByteToWideChar(CP_UTF8, 0, msg, -1, nullptr, 0);
        std::wstring wMsg(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, msg, -1, wMsg.data(), len);
        throw DxException(status, filename + L"（" + wEntry + L"）编译失败：\n" + wMsg, __FILEW__, __LINE__);
    }

    // IDxcBlob 和 ID3DBlob 的 IID 是同一个，所以能直接取成 ID3DBlob，外面的代码一行不用改
    ComPtr<ID3DBlob> code;
    ThrowIfFailed(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&code), nullptr));
    return code;
}
