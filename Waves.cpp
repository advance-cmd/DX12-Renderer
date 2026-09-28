#include "Waves.h"
#include <algorithm>   // std::swap

using namespace DirectX;

Waves::Waves(int m, int n, float dx, float dt, float speed, float damping)
{
    mNumRows = m;
    mNumCols = n;
    mVertexCount = m * n;
    mTriangleCount = (m - 1) * (n - 1) * 2;

    mTimeStep = dt;
    mSpatialStep = dx;

    // 把波动方程整理成"新高度 = k1*旧 + k2*现在 + k3*邻居之和"的形式以后，
    // 这三个系数只跟 dt、dx、speed、damping 有关，构造函数里算一次就够了
    float d = damping * dt + 2.0f;
    float e = (speed * speed) * (dt * dt) / (dx * dx);
    mK1 = (damping * dt - 2.0f) / d;
    mK2 = (4.0f - 8.0f * e) / d;
    mK3 = (2.0f * e) / d;

    mPrevSolution.resize(m * n);
    mCurrSolution.resize(m * n);
    mNormals.resize(m * n);

    // 初始状态：一张平的网格，高度全是 0，只有 x、z 按行列铺开
    float halfWidth = (n - 1) * dx * 0.5f;
    float halfDepth = (m - 1) * dx * 0.5f;
    for (int i = 0; i < m; ++i)
    {
        float z = halfDepth - i * dx;   // 行号 i 对应 z，越往下 z 越小
        for (int j = 0; j < n; ++j)
        {
            float x = -halfWidth + j * dx;   // 列号 j 对应 x
            mPrevSolution[i * n + j] = XMFLOAT3(x, 0.0f, z);
            mCurrSolution[i * n + j] = XMFLOAT3(x, 0.0f, z);
            mNormals[i * n + j] = XMFLOAT3(0.0f, 1.0f, 0.0f);
        }
    }
}

void Waves::Update(float dt)
{
    // 帧率有快有慢，这里先把时间攒起来，攒够一个 mTimeStep 才推进一步，
    // 这样模拟步长固定，结果才稳定（不然帧一卡，波浪就炸了）
    static float t = 0.0f;
    t += dt;
    if (t < mTimeStep)
    {
        return;
    }
    t = 0.0f;

    // 只更新内部点，最外圈永远是 0
    for (int i = 1; i < mNumRows - 1; ++i)
    {
        for (int j = 1; j < mNumCols - 1; ++j)
        {
            // 新高度直接写回 mPrevSolution：这一格旧的"上一帧"值反正再也用不到了，可以直接覆盖
            mPrevSolution[i * mNumCols + j].y =
                mK1 * mPrevSolution[i * mNumCols + j].y +
                mK2 * mCurrSolution[i * mNumCols + j].y +
                mK3 * (mCurrSolution[(i + 1) * mNumCols + j].y +
                    mCurrSolution[(i - 1) * mNumCols + j].y +
                    mCurrSolution[i * mNumCols + j + 1].y +
                    mCurrSolution[i * mNumCols + j - 1].y);
        }
    }

    // 新的解写在了 mPrevSolution 里，交换一下让它变成"这一帧"，旧的那份留着当下一次的"上一帧"
    std::swap(mPrevSolution, mCurrSolution);

    // 法线：用左右、上下的高度差做中心差分，n = (-dh/dx, 1, -dh/dz)，再归一化
    for (int i = 1; i < mNumRows - 1; ++i)
    {
        for (int j = 1; j < mNumCols - 1; ++j)
        {
            float l = mCurrSolution[i * mNumCols + j - 1].y;
            float r = mCurrSolution[i * mNumCols + j + 1].y;
            float u = mCurrSolution[(i - 1) * mNumCols + j].y;
            float b = mCurrSolution[(i + 1) * mNumCols + j].y;

            // dh/dx 正比于 (r - l)，dh/dz 正比于 (b - u)，负号来自 n = (-dh/dx, 1, -dh/dz)
            XMVECTOR n = XMVectorSet(l - r, 2.0f * mSpatialStep, u - b, 0.0f);
            XMStoreFloat3(&mNormals[i * mNumCols + j], XMVector3Normalize(n));
        }
    }
}

void Waves::Disturb(int i, int j, float magnitude)
{
    // 边界点必须一直是 0，所以石子只能扔在内部
    if (i < 2 || i > mNumRows - 3 || j < 2 || j > mNumCols - 3)
    {
        return;
    }

    // 中心抬最高，四个邻居抬一半，看起来像一颗石子砸下去
    float halfMag = 0.5f * magnitude;
    mCurrSolution[i * mNumCols + j].y += magnitude;
    mCurrSolution[i * mNumCols + j + 1].y += halfMag;
    mCurrSolution[i * mNumCols + j - 1].y += halfMag;
    mCurrSolution[(i + 1) * mNumCols + j].y += halfMag;
    mCurrSolution[(i - 1) * mNumCols + j].y += halfMag;
}
