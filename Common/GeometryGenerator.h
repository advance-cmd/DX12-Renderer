#pragma once
#include <DirectXCollision.h>
#include <vector>

class GeometryGenerator
{
public:
    struct Vertex
    {
        DirectX::XMFLOAT3 Position;
        DirectX::XMFLOAT3 Normal;
        DirectX::XMFLOAT2 TexC;
    };

    struct MeshData
    {
        std::vector<Vertex> Vertices;
        std::vector<uint32_t> Indices32;    //索引
        std::vector<uint16_t>& GetIndices16();  // 第一次调用时把 Indices32 转成 16 位并缓存
    private:
        std::vector<uint16_t> mIndices16;
    };
    
    // width 沿 x，depth 沿 z，m 行 × n 列个顶点，平面在 y = 0 上，中心在原点
    MeshData CreateGrid(float width, float depth, std::uint32_t m, std::uint32_t n);
};
