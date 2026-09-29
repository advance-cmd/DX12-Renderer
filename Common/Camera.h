#pragma once
#include <DirectXMath.h>

// 第一人称摄像机（书 15.2）。
// 存两样东西：1) 摄像机坐标系在世界里的位置和三根轴（右 / 上 / 前），用来建观察矩阵；
//             2) 镜头（视锥体）的参数，用来建投影矩阵。
class Camera
{
public:
    Camera();

    // 位置
    DirectX::XMVECTOR GetPosition() const;
    DirectX::XMFLOAT3 GetPosition3f() const;
    void SetPosition(float x, float y, float z);
    void SetPosition(const DirectX::XMFLOAT3& v);

    // 三根轴（世界坐标）
    DirectX::XMVECTOR GetRight() const;
    DirectX::XMFLOAT3 GetRight3f() const;
    DirectX::XMVECTOR GetUp() const;
    DirectX::XMFLOAT3 GetUp3f() const;
    DirectX::XMVECTOR GetLook() const;
    DirectX::XMFLOAT3 GetLook3f() const;

    // 视锥体参数
    float GetNearZ() const;
    float GetFarZ() const;
    float GetAspect() const;
    float GetFovY() const;
    float GetFovX() const;

    // 近 / 远平面在观察空间里的宽和高
    float GetNearWindowWidth() const;
    float GetNearWindowHeight() const;
    float GetFarWindowWidth() const;
    float GetFarWindowHeight() const;

    // 设镜头：垂直视场角（弧度）、宽高比、近平面、远平面。窗口大小变了要重新调
    void SetLens(float fovY, float aspect, float zn, float zf);

    // 站在 pos，看向 target，worldUp 一般是 (0, 1, 0)
    void LookAt(DirectX::FXMVECTOR pos, DirectX::FXMVECTOR target, DirectX::FXMVECTOR worldUp);
    void LookAt(const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& target, const DirectX::XMFLOAT3& up);

    // 取矩阵。GetView 之前必须先调 UpdateViewMatrix，否则 Debug 下会 assert
    DirectX::XMMATRIX GetView() const;
    DirectX::XMMATRIX GetProj() const;
    DirectX::XMFLOAT4X4 GetView4x4f() const;
    DirectX::XMFLOAT4X4 GetProj4x4f() const;

    // 沿右向量左右平移 / 沿前向量前后走，走的距离 = d × 移速（d 一般传 DeltaTime，移速就是每秒几个单位）
    void Strafe(float d);
    void Walk(float d);

    // 沿世界 y 轴升降（d 为负就下降），和朝向无关：低着头按 E 也是笔直往上（UE 编辑器视口 Q / E 的手感）
    void Ascend(float d);

    // 抬头低头：绕自己的右向量转；左右转头：绕世界 y 轴转（地平线保持水平）
    void Pitch(float angle);
    void RotateY(float angle);

    // 改过位置或朝向后调一次，重建观察矩阵（没改过就什么都不做）
    void UpdateViewMatrix();
    
    // 移速（单位 / 秒）：Walk、Strafe、Ascend 走的距离都会乘上它
    void SetMoveSpeed(float speed);
    float GetMoveSpeed() const;

private:
    // 摄像机坐标系（世界坐标）
    DirectX::XMFLOAT3 mPosition = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 mRight = {1.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 mUp = {0.0f, 1.0f, 0.0f};
    DirectX::XMFLOAT3 mLook = {0.0f, 0.0f, 1.0f};

    // 视锥体参数
    float mNearZ = 0.0f;
    float mFarZ = 0.0f;
    float mAspect = 0.0f;
    float mFovY = 0.0f;
    float mNearWindowHeight = 0.0f;
    float mFarWindowHeight = 0.0f;

    // 位置或朝向改过、观察矩阵还没重建（和 NumFramesDirty 同一个思路）
    bool mViewDirty = true;

    // 单位矩阵（书上用 MathHelper::Identity4x4()，我们的项目里没有 MathHelper）
    DirectX::XMFLOAT4X4 mView = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    DirectX::XMFLOAT4X4 mProj = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    
    float mMoveSpeed = 1.0f;
};
