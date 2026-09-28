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

    // 中心在原点，width 沿 x，height 沿 y，depth 沿 z。24 个顶点（每面 4 个），36 个索引
    // 书上还有第 4 个参数 numSubdivisions（把每个面再细分），配合 Subdivide 用，暂不需要
    MeshData CreateBox(float width, float height, float depth);

    // 圆台（上下半径不同；相等就是圆柱，顶半径为 0 就是圆锥），中心在原点，轴沿 y。
    // sliceCount：绕一圈切几刀；stackCount：从下到上分几层。带顶盖和底盖
    MeshData CreateCylinder(float bottomRadius, float topRadius, float height,
                            std::uint32_t sliceCount, std::uint32_t stackCount);

    // UV 球：像地球仪的经纬线，slice 是经线方向切几刀，stack 是纬线方向分几层。
    // 两极各只有 1 个顶点，所以极点附近三角形又细又密
    MeshData CreateSphere(float radius, std::uint32_t sliceCount, std::uint32_t stackCount);

    // 几何球：正二十面体每细分一次，每个三角形一分为四，再把顶点推到球面上。三角形大小均匀。
    // 细分次数最多 6 次。每细分一次三角形 ×4，顶点也差不多 ×4：
    // 5 次约 3 万个顶点，6 次约 12 万个，超过 65535，就不能用 16 位索引了
    MeshData CreateGeosphere(float radius, std::uint32_t numSubdivisions);

private:
    // 圆台的顶盖 / 底盖：一圈顶点 + 圆心，扇形三角形
    void BuildCylinderCap(float radius, float y, bool isTop, std::uint32_t sliceCount, MeshData& meshData);
    // 把每个三角形一分为四（取三条边的中点）
    void Subdivide(MeshData& meshData);
    // 两个顶点的中点：位置、uv 取平均，法线取平均后再归一化
    Vertex MidPoint(const Vertex& v0, const Vertex& v1);
};
