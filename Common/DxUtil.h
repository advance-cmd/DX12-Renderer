#pragma once
// ThrowIfFailed：检查 D3D / DXGI 调用返回的 HRESULT，失败就抛出 DxException。
// 异常里带着出错的那句代码、文件名、行号和错误描述，WinMain 里 catch 住弹窗显示。
// 仿照书中 d3dUtil.h 的写法，做成纯头文件，不需要额外的 .cpp。

#include <windows.h>
#include <comdef.h>     // _com_error：把 HRESULT 翻译成可读的错误描述
#include <cstdio>
#include <string>

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

    HRESULT      ErrorCode = S_OK;
    std::wstring FunctionName;
    std::wstring Filename;
    int          LineNumber = -1;
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
