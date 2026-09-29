#include "Camera.h"
#include <cassert>
#include <cmath>

using namespace DirectX;

Camera::Camera()
{
    SetLens(0.25f * XM_PI, 1.0f, 1.0f, 1000.0f);
}

XMVECTOR Camera::GetPosition() const { return XMLoadFloat3(&mPosition); }
XMFLOAT3 Camera::GetPosition3f() const { return mPosition; }

void Camera::SetPosition(float x, float y, float z)
{
    mPosition = XMFLOAT3(x, y, z);
    mViewDirty = true;
}

void Camera::SetPosition(const XMFLOAT3& v)
{
    mPosition = v;
    mViewDirty = true;
}

XMVECTOR Camera::GetRight() const { return XMLoadFloat3(&mRight); }
XMFLOAT3 Camera::GetRight3f() const { return mRight; }
XMVECTOR Camera::GetUp() const { return XMLoadFloat3(&mUp); }
XMFLOAT3 Camera::GetUp3f() const { return mUp; }
XMVECTOR Camera::GetLook() const { return XMLoadFloat3(&mLook); }
XMFLOAT3 Camera::GetLook3f() const { return mLook; }

float Camera::GetNearZ() const { return mNearZ; }
float Camera::GetFarZ() const { return mFarZ; }
float Camera::GetAspect() const { return mAspect; }
float Camera::GetFovY() const { return mFovY; }

// 水平视场角：tan(水平半角) = 近平面半宽 / 近平面距离
float Camera::GetFovX() const
{
    float halfWidth = 0.5f * GetNearWindowWidth();
    return 2.0f * atanf(halfWidth / mNearZ);
}

float Camera::GetNearWindowWidth() const { return mAspect * mNearWindowHeight; }
float Camera::GetNearWindowHeight() const { return mNearWindowHeight; }
float Camera::GetFarWindowWidth() const { return mAspect * mFarWindowHeight; }
float Camera::GetFarWindowHeight() const { return mFarWindowHeight; }

void Camera::SetLens(float fovY, float aspect, float zn, float zf)
{
    mFovY = fovY;
    mAspect = aspect;
    mNearZ = zn;
    mFarZ = zf;

    // 平面高度 = 2 × 距离 × tan(半个视场角)
    mNearWindowHeight = 2.0f * mNearZ * tanf(0.5f * mFovY);
    mFarWindowHeight = 2.0f * mFarZ * tanf(0.5f * mFovY);

    XMStoreFloat4x4(&mProj, XMMatrixPerspectiveFovLH(mFovY, mAspect, mNearZ, mFarZ));
}

void Camera::LookAt(FXMVECTOR pos, FXMVECTOR target, FXMVECTOR worldUp)
{
    XMVECTOR L = XMVector3Normalize(XMVectorSubtract(target, pos));
    XMVECTOR R = XMVector3Normalize(XMVector3Cross(worldUp, L));
    XMVECTOR U = XMVector3Cross(L, R);   // L、R 都是单位向量且互相垂直，叉积已经是单位向量

    XMStoreFloat3(&mPosition, pos);
    XMStoreFloat3(&mLook, L);
    XMStoreFloat3(&mRight, R);
    XMStoreFloat3(&mUp, U);
    mViewDirty = true;
}

void Camera::LookAt(const XMFLOAT3& pos, const XMFLOAT3& target, const XMFLOAT3& up)
{
    LookAt(XMLoadFloat3(&pos), XMLoadFloat3(&target), XMLoadFloat3(&up));
}

XMMATRIX Camera::GetView() const
{
    assert(!mViewDirty);   // 忘了先调 UpdateViewMatrix
    return XMLoadFloat4x4(&mView);
}

XMMATRIX Camera::GetProj() const { return XMLoadFloat4x4(&mProj); }

XMFLOAT4X4 Camera::GetView4x4f() const
{
    assert(!mViewDirty);
    return mView;
}

XMFLOAT4X4 Camera::GetProj4x4f() const { return mProj; }

void Camera::Strafe(float d)
{
    // mPosition += d * mRight
    XMVECTOR s = XMVectorReplicate(d*MoveSpeed);
    XMStoreFloat3(&mPosition, XMVectorMultiplyAdd(s, XMLoadFloat3(&mRight), XMLoadFloat3(&mPosition)));
    mViewDirty = true;
}

void Camera::Walk(float d)
{
    // mPosition += d * mLook
    XMVECTOR s = XMVectorReplicate(d*MoveSpeed);
    XMStoreFloat3(&mPosition, XMVectorMultiplyAdd(s, XMLoadFloat3(&mLook), XMLoadFloat3(&mPosition)));
    mViewDirty = true;
}

void Camera::Ascend(float d)
{
    // 世界的"上"永远是 (0, 1, 0)，所以只改 y，不用管摄像机的三根轴
    mPosition.y += d * MoveSpeed;
    mViewDirty = true;
}

void Camera::Pitch(float angle)
{
    // 绕右向量转上向量和前向量。右向量是转轴，绕自己转不会变，所以不用转
    XMMATRIX R = XMMatrixRotationAxis(XMLoadFloat3(&mRight), angle);
    XMStoreFloat3(&mUp, XMVector3TransformNormal(XMLoadFloat3(&mUp), R));
    XMStoreFloat3(&mLook, XMVector3TransformNormal(XMLoadFloat3(&mLook), R));
    mViewDirty = true;
}

void Camera::RotateY(float angle)
{
    // 绕世界 y 轴转三根轴。世界 y 轴不是摄像机的任何一根轴，所以三根都要转
    XMMATRIX R = XMMatrixRotationY(angle);
    XMStoreFloat3(&mRight, XMVector3TransformNormal(XMLoadFloat3(&mRight), R));
    XMStoreFloat3(&mUp, XMVector3TransformNormal(XMLoadFloat3(&mUp), R));
    XMStoreFloat3(&mLook, XMVector3TransformNormal(XMLoadFloat3(&mLook), R));
    mViewDirty = true;
}

void Camera::UpdateViewMatrix()
{
    if (!mViewDirty)
        return;

    XMVECTOR R = XMLoadFloat3(&mRight);
    XMVECTOR L = XMLoadFloat3(&mLook);
    XMVECTOR P = XMLoadFloat3(&mPosition);

    // 一直转来转去，浮点误差会让三根轴慢慢不垂直、不是单位长度，每次都拉回来
    L = XMVector3Normalize(L);
    XMVECTOR U = XMVector3Normalize(XMVector3Cross(L, R));
    R = XMVector3Cross(U, L);   // U、L 已经是互相垂直的单位向量，不用再归一化

    XMStoreFloat3(&mRight, R);
    XMStoreFloat3(&mUp, U);
    XMStoreFloat3(&mLook, L);

    // 按公式 15.1 填：前三列是三根轴竖着放，最后一行是 -Q·u、-Q·v、-Q·w
    float x = -XMVectorGetX(XMVector3Dot(P, R));
    float y = -XMVectorGetX(XMVector3Dot(P, U));
    float z = -XMVectorGetX(XMVector3Dot(P, L));

    mView(0, 0) = mRight.x; mView(0, 1) = mUp.x; mView(0, 2) = mLook.x; mView(0, 3) = 0.0f;
    mView(1, 0) = mRight.y; mView(1, 1) = mUp.y; mView(1, 2) = mLook.y; mView(1, 3) = 0.0f;
    mView(2, 0) = mRight.z; mView(2, 1) = mUp.z; mView(2, 2) = mLook.z; mView(2, 3) = 0.0f;
    mView(3, 0) = x;        mView(3, 1) = y;     mView(3, 2) = z;       mView(3, 3) = 1.0f;

    mViewDirty = false;
}

void Camera::SetMoveSpeed(float speed)
{
    MoveSpeed = speed;
}

float Camera::GetMoveSpeed() const { return MoveSpeed; }
