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

GeometryGenerator::MeshData GeometryGenerator::CreateBox(float width, float height, float depth)
{
    MeshData meshData;
    float w2 = 0.5f * width;    // 半宽、半高、半深：顶点坐标都是 ±w2 / ±h2 / ±d2
    float h2 = 0.5f * height;
    float d2 = 0.5f * depth;

    // 一个角被 3 个面共用，但 3 个面在这个角上的 uv、法线都不同，所以每个面各用 4 个顶点，共 24 个。
    // 每个面的 4 个顶点从盒子外面看都是"左下、左上、右上、右下"；uv 的 v 轴朝下，左上角是 (0, 0)
    // 格式：{ 位置, 法线, uv }
    meshData.Vertices = {
        // 前表面（z = -d2，法线朝 -z）
        {{-w2, -h2, -d2}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},
        {{-w2, +h2, -d2}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}},
        {{+w2, +h2, -d2}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
        {{+w2, -h2, -d2}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
        // 后表面（z = +d2，法线朝 +z）
        {{-w2, -h2, +d2}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{+w2, -h2, +d2}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        {{+w2, +h2, +d2}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{-w2, +h2, +d2}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        // 上表面（y = +h2，法线朝 +y）
        {{-w2, +h2, -d2}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
        {{-w2, +h2, +d2}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{+w2, +h2, +d2}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
        {{+w2, +h2, -d2}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
        // 下表面（y = -h2，法线朝 -y）
        {{-w2, -h2, -d2}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
        {{+w2, -h2, -d2}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},
        {{+w2, -h2, +d2}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}},
        {{-w2, -h2, +d2}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}},
        // 左表面（x = -w2，法线朝 -x）
        {{-w2, -h2, +d2}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},
        {{-w2, +h2, +d2}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{-w2, +h2, -d2}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{-w2, -h2, -d2}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        // 右表面（x = +w2，法线朝 +x）
        {{+w2, -h2, -d2}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},
        {{+w2, +h2, -d2}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{+w2, +h2, +d2}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{+w2, -h2, +d2}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
    };

    // 6 个面的索引规律一样：第 f 个面用第 4f ~ 4f+3 号顶点，
    // 拆成 (左下, 左上, 右上) 和 (左下, 右上, 右下) 两个三角形，从外面看都是顺时针
    for (uint32_t f = 0; f < 6; f++)
    {
        uint32_t base = 4 * f;
        meshData.Indices32.insert(meshData.Indices32.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return meshData;
}
