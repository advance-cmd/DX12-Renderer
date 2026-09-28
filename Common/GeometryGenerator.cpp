#include "GeometryGenerator.h"

#include <algorithm>    // std::min
#include <cmath>        // sinf / cosf / atan2f / acosf

using namespace DirectX;

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

GeometryGenerator::MeshData GeometryGenerator::CreateCylinder(float bottomRadius, float topRadius, float height,
                                                              uint32_t sliceCount, uint32_t stackCount)
{
    MeshData meshData;

    // ---- 侧面顶点：stackCount 层 → stackCount + 1 圈（环），从下往上 ----
    float stackHeight = height / stackCount;                          // 每层多高
    float radiusStep = (topRadius - bottomRadius) / stackCount;       // 每往上一圈，半径变多少
    float dTheta = 2.0f * XM_PI / sliceCount;                         // 相邻两刀之间的角度
    uint32_t ringCount = stackCount + 1;

    for (uint32_t i = 0; i < ringCount; i++)
    {
        float y = -0.5f * height + i * stackHeight;
        float r = bottomRadius + i * radiusStep;

        // 每圈 sliceCount + 1 个顶点：最后一个和第一个位置相同，但 u 一个是 1、一个是 0。
        // 如果共用一个顶点，接缝处那一条三角形的 u 会从 0.95 插值回 0，整张纹理被挤进这一条里
        for (uint32_t j = 0; j <= sliceCount; j++)
        {
            float c = cosf(j * dTheta);
            float s = sinf(j * dTheta);

            Vertex v;
            v.Position = XMFLOAT3(r * c, y, r * s);

            // 法线：圆柱（上下一样粗）时就是水平朝外的 (c, 0, s)。
            // 圆台的侧面是斜的，下粗上细时法线要往上翘；翘多少由"半径差 : 高度"决定。
            // （书上是用切线 × 副切线算的，化简后就是这个式子）
            XMVECTOR n = XMVectorSet(height * c, bottomRadius - topRadius, height * s, 0.0f);
            XMStoreFloat3(&v.Normal, XMVector3Normalize(n));

            // 纹理绕侧面一整圈：u 沿圆周 0 → 1，v 从下（1）到上（0）
            v.TexC = XMFLOAT2((float)j / sliceCount, 1.0f - (float)i / stackCount);

            meshData.Vertices.push_back(v);
        }
    }

    // ---- 侧面索引：和网格一样"每格两个三角形"，只是一行（一圈）有 sliceCount + 1 个顶点 ----
    // 第 i 圈第 j 个顶点的下标 = i * ringVertexCount + j；从外面看都是顺时针
    uint32_t ringVertexCount = sliceCount + 1;
    for (uint32_t i = 0; i < stackCount; i++)
    {
        for (uint32_t j = 0; j < sliceCount; j++)
        {
            uint32_t bottomLeft = i * ringVertexCount + j;
            uint32_t bottomRight = i * ringVertexCount + j + 1;
            uint32_t topLeft = (i + 1) * ringVertexCount + j;
            uint32_t topRight = (i + 1) * ringVertexCount + j + 1;

            meshData.Indices32.insert(meshData.Indices32.end(), {bottomLeft, topLeft, topRight});
            meshData.Indices32.insert(meshData.Indices32.end(), {bottomLeft, topRight, bottomRight});
        }
    }

    // ---- 顶盖、底盖 ----
    BuildCylinderCap(topRadius, 0.5f * height, true, sliceCount, meshData);
    BuildCylinderCap(bottomRadius, -0.5f * height, false, sliceCount, meshData);

    return meshData;
}

void GeometryGenerator::BuildCylinderCap(float radius, float y, bool isTop, uint32_t sliceCount, MeshData& meshData)
{
    // 盖子的顶点不能和侧面共用：位置虽然相同，但法线（竖直 vs 水平）和 uv 都不一样。
    // 所以另建一圈，从当前顶点数开始编号
    uint32_t baseIndex = (uint32_t)meshData.Vertices.size();
    float dTheta = 2.0f * XM_PI / sliceCount;
    XMFLOAT3 normal(0.0f, isTop ? 1.0f : -1.0f, 0.0f);

    // uv 按"高度"缩放（书上的做法）：纹理在盖子上的密度和侧面竖直方向一致
    float height = 2.0f * std::fabs(y);
    for (uint32_t i = 0; i <= sliceCount; i++)
    {
        float x = radius * cosf(i * dTheta);
        float z = radius * sinf(i * dTheta);
        meshData.Vertices.push_back({{x, y, z}, normal, {x / height + 0.5f, z / height + 0.5f}});
    }

    // 圆心
    meshData.Vertices.push_back({{0.0f, y, 0.0f}, normal, {0.5f, 0.5f}});
    uint32_t centerIndex = (uint32_t)meshData.Vertices.size() - 1;

    // 扇形：圆心 + 圈上相邻两个顶点。
    // 顶盖从上往下看、底盖从下往上看都要是顺时针，两者看的方向相反，所以后两个顶点的顺序也相反
    for (uint32_t i = 0; i < sliceCount; i++)
    {
        if (isTop)
            meshData.Indices32.insert(meshData.Indices32.end(), {centerIndex, baseIndex + i + 1, baseIndex + i});
        else
            meshData.Indices32.insert(meshData.Indices32.end(), {centerIndex, baseIndex + i, baseIndex + i + 1});
    }
}

GeometryGenerator::MeshData GeometryGenerator::CreateSphere(float radius, uint32_t sliceCount, uint32_t stackCount)
{
    MeshData meshData;

    // 球面坐标：phi 从北极（0）到南极（π），theta 绕 y 轴一圈（0 → 2π）
    //   x = r·sinφ·cosθ,  y = r·cosφ,  z = r·sinφ·sinθ
    // 球心在原点，所以法线就是"位置归一化"；uv：u = θ / 2π，v = φ / π
    float phiStep = XM_PI / stackCount;
    float thetaStep = 2.0f * XM_PI / sliceCount;

    // 北极：只有 1 个顶点（uv 只能取一个值，所以极点附近的纹理会被挤成一个点）
    meshData.Vertices.push_back({{0.0f, +radius, 0.0f}, {0.0f, +1.0f, 0.0f}, {0.0f, 0.0f}});

    // 中间 stackCount - 1 圈（两极不算圈），每圈 sliceCount + 1 个顶点（接缝处多一个，理由同圆台）
    for (uint32_t i = 1; i <= stackCount - 1; i++)
    {
        float phi = i * phiStep;
        for (uint32_t j = 0; j <= sliceCount; j++)
        {
            float theta = j * thetaStep;

            Vertex v;
            v.Position = XMFLOAT3(radius * sinf(phi) * cosf(theta), radius * cosf(phi), radius * sinf(phi) * sinf(theta));
            XMStoreFloat3(&v.Normal, XMVector3Normalize(XMLoadFloat3(&v.Position)));
            v.TexC = XMFLOAT2(theta / XM_2PI, phi / XM_PI);
            meshData.Vertices.push_back(v);
        }
    }

    // 南极
    meshData.Vertices.push_back({{0.0f, -radius, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}});

    // ---- 索引 ----
    uint32_t ringVertexCount = sliceCount + 1;

    // 最上面一层：北极（0 号）和第一圈（1 号开始）连成扇形
    for (uint32_t j = 1; j <= sliceCount; j++)
        meshData.Indices32.insert(meshData.Indices32.end(), {0u, j + 1, j});

    // 中间各层：相邻两圈之间"每格两个三角形"。圈的编号从 1 开始（0 号是北极）
    uint32_t baseIndex = 1;
    for (uint32_t i = 0; i < stackCount - 2; i++)
    {
        for (uint32_t j = 0; j < sliceCount; j++)
        {
            uint32_t upperLeft = baseIndex + i * ringVertexCount + j;         // 上面那圈（离北极近）
            uint32_t upperRight = baseIndex + i * ringVertexCount + j + 1;
            uint32_t lowerLeft = baseIndex + (i + 1) * ringVertexCount + j;
            uint32_t lowerRight = baseIndex + (i + 1) * ringVertexCount + j + 1;

            meshData.Indices32.insert(meshData.Indices32.end(), {upperLeft, upperRight, lowerLeft});
            meshData.Indices32.insert(meshData.Indices32.end(), {lowerLeft, upperRight, lowerRight});
        }
    }

    // 最下面一层：最后一圈和南极（最后一个顶点）连成扇形
    uint32_t southPoleIndex = (uint32_t)meshData.Vertices.size() - 1;
    baseIndex = southPoleIndex - ringVertexCount;   // 最后一圈的第一个顶点
    for (uint32_t j = 0; j < sliceCount; j++)
        meshData.Indices32.insert(meshData.Indices32.end(), {southPoleIndex, baseIndex + j, baseIndex + j + 1});

    return meshData;
}

GeometryGenerator::MeshData GeometryGenerator::CreateGeosphere(float radius, uint32_t numSubdivisions)
{
    MeshData meshData;
    numSubdivisions = std::min<uint32_t>(numSubdivisions, 6u);

    // 正二十面体：12 个顶点、20 个面，顶点都在单位球上（X、Z 是算好的常数）
    const float X = 0.525731f;
    const float Z = 0.850651f;
    const XMFLOAT3 pos[12] = {
        {-X, 0.0f, Z}, {X, 0.0f, Z}, {-X, 0.0f, -Z}, {X, 0.0f, -Z},
        {0.0f, Z, X}, {0.0f, Z, -X}, {0.0f, -Z, X}, {0.0f, -Z, -X},
        {Z, X, 0.0f}, {-Z, X, 0.0f}, {Z, -X, 0.0f}, {-Z, -X, 0.0f},
    };
    const uint32_t k[60] = {
        1, 4, 0,   4, 9, 0,   4, 5, 9,   8, 5, 4,   1, 8, 4,
        1, 10, 8,  10, 3, 8,  8, 3, 5,   3, 2, 5,   3, 7, 2,
        3, 10, 7,  10, 6, 7,  6, 11, 7,  6, 0, 11,  6, 1, 0,
        10, 1, 6,  11, 0, 9,  2, 11, 9,  5, 2, 9,   11, 2, 7,
    };

    meshData.Vertices.resize(12);
    for (uint32_t i = 0; i < 12; i++)
        meshData.Vertices[i].Position = pos[i];
    meshData.Indices32.assign(std::begin(k), std::end(k));

    // 细分：边的中点在球的"里面"，所以细分完的形状还不是球
    for (uint32_t i = 0; i < numSubdivisions; i++)
        Subdivide(meshData);

    // 把每个顶点沿着"从球心出发的方向"推到半径为 radius 的球面上
    for (Vertex& v : meshData.Vertices)
    {
        XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&v.Position));
        XMStoreFloat3(&v.Position, radius * n);
        XMStoreFloat3(&v.Normal, n);

        // uv 由球面坐标反推，和 CreateSphere 的定义一致
        float theta = atan2f(v.Position.z, v.Position.x);   // [-π, π]
        if (theta < 0.0f)
            theta += XM_2PI;                                 // 变成 [0, 2π)
        float phi = acosf(std::clamp(v.Position.y / radius, -1.0f, 1.0f));
        v.TexC = XMFLOAT2(theta / XM_2PI, phi / XM_PI);
    }
    // 注意：几何球没有像 UV 球那样在接缝处复制顶点，所以跨过接缝（u 从 0.9x 跳回 0.0x）的那一圈三角形，
    // 纹理会被挤压、倒过来。几何球一般不贴这种经纬度纹理，主要用在不需要 uv 的地方（比如光照测试）

    return meshData;
}

void GeometryGenerator::Subdivide(MeshData& meshData)
{
    MeshData inputCopy = meshData;
    meshData.Vertices.clear();
    meshData.Indices32.clear();

    /*
             v1
             *
            / \
           /   \
        m0*-----*m1
         / \   / \
        /   \ /   \
       *-----*-----*
       v0    m2     v2
    */
    // 每个三角形变成 4 个：3 个角上的小三角形 + 中间 1 个倒过来的。绕序和原三角形一致。
    // 为了简单，每个小三角形组各自存 6 个顶点，相邻三角形共享的边中点会重复存
    uint32_t numTris = (uint32_t)inputCopy.Indices32.size() / 3;
    for (uint32_t i = 0; i < numTris; i++)
    {
        const Vertex& v0 = inputCopy.Vertices[inputCopy.Indices32[i * 3 + 0]];
        const Vertex& v1 = inputCopy.Vertices[inputCopy.Indices32[i * 3 + 1]];
        const Vertex& v2 = inputCopy.Vertices[inputCopy.Indices32[i * 3 + 2]];

        uint32_t b = i * 6;
        meshData.Vertices.push_back(v0);                // b + 0
        meshData.Vertices.push_back(v1);                // b + 1
        meshData.Vertices.push_back(v2);                // b + 2
        meshData.Vertices.push_back(MidPoint(v0, v1));  // b + 3 = m0
        meshData.Vertices.push_back(MidPoint(v1, v2));  // b + 4 = m1
        meshData.Vertices.push_back(MidPoint(v0, v2));  // b + 5 = m2

        meshData.Indices32.insert(meshData.Indices32.end(), {
            b + 0, b + 3, b + 5,    // 左下角：v0, m0, m2
            b + 3, b + 4, b + 5,    // 中间：  m0, m1, m2
            b + 5, b + 4, b + 2,    // 右下角：m2, m1, v2
            b + 3, b + 1, b + 4,    // 顶上：  m0, v1, m1
        });
    }
}

GeometryGenerator::Vertex GeometryGenerator::MidPoint(const Vertex& v0, const Vertex& v1)
{
    XMVECTOR p0 = XMLoadFloat3(&v0.Position), p1 = XMLoadFloat3(&v1.Position);
    XMVECTOR n0 = XMLoadFloat3(&v0.Normal), n1 = XMLoadFloat3(&v1.Normal);
    XMVECTOR t0 = XMLoadFloat2(&v0.TexC), t1 = XMLoadFloat2(&v1.TexC);

    Vertex m;
    XMStoreFloat3(&m.Position, 0.5f * (p0 + p1));
    XMStoreFloat3(&m.Normal, XMVector3Normalize(0.5f * (n0 + n1)));   // 两个单位向量的平均不是单位长度
    XMStoreFloat2(&m.TexC, 0.5f * (t0 + t1));
    return m;
}
