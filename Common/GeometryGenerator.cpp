#include "GeometryGenerator.h"

std::vector<uint16_t>& GeometryGenerator::MeshData::GetIndices16()
{
    if (mIndices16.empty())
    {
        mIndices16.resize(Indices32.size());
        for (uint32_t i = 0; i < mIndices16.size(); i++)
        {
            mIndices16[i] = static_cast<uint16_t>(Indices32[i]);
        }
    }
    return mIndices16;
}

GeometryGenerator::MeshData GeometryGenerator::CreateGrid(float width, float depth, std::uint32_t m, std::uint32_t n)
{
    MeshData meshData;
    float dx = width / (n - 1); //相邻两列的间距
    float dz = depth / (m - 1); //相邻两行的间距

    for (uint32_t i = 0; i < m; i++)
    {
        for (uint32_t j = 0; j < n; j++)
        {
            float x = -width / 2 + j * dx;
            float z = +depth / 2 - i * dz;
            DirectX::XMFLOAT3 Normal(0, 1, 0);
            DirectX::XMFLOAT2 UV(static_cast<float>(j) / (n - 1), static_cast<float>(i) / (m - 1));
            meshData.Vertices.push_back(Vertex(DirectX::XMFLOAT3(x, 0, z), DirectX::XMFLOAT3(Normal),
                                               DirectX::XMFLOAT2(UV.x, UV.y)));
        }
    }
    for (uint32_t i = 0; i < m - 1; i++)
    {
        uint32_t first_VertexIndex = i * n;
        uint32_t last_VertexIndex = first_VertexIndex + n - 1;
        meshData.Indices32.push_back(first_VertexIndex);
        meshData.Indices32.push_back(first_VertexIndex + 1);
        meshData.Indices32.push_back(last_VertexIndex + 1);
        for (uint32_t j = 1; j < n - 1; j++)
        {
            meshData.Indices32.push_back(first_VertexIndex + j);
            meshData.Indices32.push_back(first_VertexIndex + j + n);
            meshData.Indices32.push_back(first_VertexIndex + j + n-1);
            meshData.Indices32.push_back(first_VertexIndex + j);
            meshData.Indices32.push_back(first_VertexIndex + j + 1);
            meshData.Indices32.push_back(first_VertexIndex + j + n);
        }
        meshData.Indices32.push_back(last_VertexIndex);
        meshData.Indices32.push_back(last_VertexIndex + n);
        meshData.Indices32.push_back(last_VertexIndex + n-1);
    }
    meshData.GetIndices16();
    return meshData;
}
