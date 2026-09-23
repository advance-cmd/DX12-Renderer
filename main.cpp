#include "Common/D3DApp.h"

#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <d3dcompiler.h>    // D3DCompileFromFile，第 6 章编译 HLSL 时才用
#include <DirectXColors.h>  // Colors::LightSteelBlue 之类的预定义颜色

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
}

extern "C" {
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

using namespace DirectX;
using namespace DirectX::PackedVector;

// 第 4 章的 Demo：继承框架，初始化全由 D3DApp 完成，这里只写每帧要做的事
class InitDirect3DApp : public D3DApp
{
public:
    InitDirect3DApp(HINSTANCE hInstance) : D3DApp(hInstance)
    {
    }

private:
    void Update(const GameTimer& gt) override
    {
    }

    void Draw(const GameTimer& gt) override
    {
        // 4.5 节在这里写：重置命令分配器/命令列表 → 设置视口和裁剪矩形 → 清屏 → 提交 → Present
    }
};

//在 Windows 应用程序中WinMain函数就相当于大多数语言中的main()函数
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR pCmdLine, int nShowCmd)
{
    // ThrowIfFailed 失败时会抛 DxException，在这里统一接住，弹窗显示是哪句代码、第几行出的错
    try
    {
        InitDirect3DApp theApp(hInstance);
        if (!theApp.Initialize())
        {
            return 0;
        }

        //初始化完毕后就可以开启消息循环了
        return theApp.Run();
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK | MB_ICONERROR);
        return 0;
    }
}
