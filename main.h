#pragma once
#include <d3d12.h>
#include <Windows.h>
#include <wrl/client.h>
#include <dxgi1_4.h>

#include "Common/DxUtil.h"   // ThrowIfFailed

using namespace Microsoft::WRL;

//用于指认所创建主窗口的句柄
inline HWND ghMainWnd = 0;

inline ComPtr<IDXGIFactory4> gDxgiFactory;
inline ComPtr<ID3D12Device> gDevice;
inline ComPtr<ID3D12Fence1> gFence;
inline UINT gRTVDescriptorSize;
inline UINT gDSVDescriptorSize;
inline UINT gCSUDescriptorSize;
inline DXGI_FORMAT gBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
inline UINT g4xMsaaQuality = 0;

inline ComPtr<ID3D12CommandQueue> gCommandQueue;
inline ComPtr<ID3D12CommandAllocator> gCommandAllocator;
inline ComPtr<ID3D12GraphicsCommandList> gCommandList;

inline ComPtr<IDXGISwapChain3> gSwapChain;
inline constexpr UINT gSwapChainBufferCount = 2;   // 双缓冲
inline UINT gClientWidth = 0;
inline UINT gClientHeight = 0;

//封装初始化Windows应用程序所需的代码
bool InitWindowsApp(HINSTANCE instanceHandle, int show);

//封装消息循环代码
int Run();

// 窗口过程会处理窗口所接收到的消息
LRESULT CALLBACK
WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

//在 Windows 应用程序中WinMain函数就相当于大多数语言中的main()函数
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR pCmdLine, int nShowCmd);
