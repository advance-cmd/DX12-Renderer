#pragma once
#include <vector>
#include <DirectXMath.h>

// 波浪的 CPU 端模拟：只算高度和法线，不碰任何 D3D 资源（书上的 Waves 类）。
// 思路：把水面看成一张 m 行 n 列的网格，用离散化的波动方程逐帧推进：
//   h(i,j) 的新高度 = k1*它的上一帧 + k2*它的这一帧 + k3*(上下左右四个邻居的"这一帧")
// 最外圈的高度固定为 0（零边界条件），波打到边上就流走了，不会反射回来。
class Waves
{
public:
    // m 行 n 列顶点；dx 相邻两点的距离；dt 模拟步长；speed 波速；damping 阻尼（越大衰减越快）
    Waves(int m, int n, float dx, float dt, float speed, float damping);

    Waves(const Waves&) = delete;
    Waves& operator=(const Waves&) = delete;

    int RowCount() const { return mNumRows; }
    int ColumnCount() const { return mNumCols; }
    int VertexCount() const { return mVertexCount; }
    int TriangleCount() const { return mTriangleCount; }
    float Width() const { return mNumCols * mSpatialStep; }
    float Depth() const { return mNumRows * mSpatialStep; }

    // 第 i 个顶点当前的位置（x、z 从一开始就不变，只有 y 在动）
    const DirectX::XMFLOAT3& Position(int i) const { return mCurrSolution[i]; }

    // 第 i 个顶点的法线，用左右、上下邻居的高度差算出来（水面越陡，法线越歪）
    const DirectX::XMFLOAT3& Normal(int i) const { return mNormals[i]; }

    // 推进模拟。dt 是这一帧的真实耗时，内部会攒够一个步长才走一步
    void Update(float dt);

    // 在 (i, j) 处"扔一颗石子"：把这一点和上下左右四个邻居的高度一起抬高
    void Disturb(int i, int j, float magnitude);

private:
    int mNumRows = 0;
    int mNumCols = 0;
    int mVertexCount = 0;
    int mTriangleCount = 0;

    // 从构造参数预先算好的三个系数，每个点都一样，不用每帧重算
    float mK1 = 0.0f;
    float mK2 = 0.0f;
    float mK3 = 0.0f;

    float mTimeStep = 0.0f;    // 每多少秒推进一次模拟
    float mSpatialStep = 0.0f; // 相邻两点的距离（dx）

    // 算下一帧要用到"上一帧"和"这一帧"两份高度，所以必须都留着
    std::vector<DirectX::XMFLOAT3> mPrevSolution;
    std::vector<DirectX::XMFLOAT3> mCurrSolution;
    std::vector<DirectX::XMFLOAT3> mNormals;
};
