#include "GameTimer.h"

#include "DxUtil.h"

GameTimer::GameTimer() :
    mSecondPerCount(0.0), mDeltaTime(-1.0), mBaseTime(0), mPausedTime(0), mStopTime(0),
    mPrevTime(0), mCurrTime(0), mStopped(false)
{
    __int64 countsPerSec;
    QueryPerformanceFrequency((LARGE_INTEGER*)&countsPerSec);
    mSecondPerCount = 1.0 / (double)countsPerSec;
}

float GameTimer::TotalTime() const
{
    // 暂停中：时间冻结在 Stop 那一刻，本次暂停还没累加进 mPausedTime
    if (mStopped)
    {
        return (float)(((mStopTime - mPausedTime) - mBaseTime) * mSecondPerCount);
    }
    // 运行中：截止到最近一次 Tick，扣掉之前所有暂停的时长
    return (float)(((mCurrTime - mPausedTime) - mBaseTime) * mSecondPerCount);
}

float GameTimer::DeltaTime() const
{
    return (float)mDeltaTime;
}

void GameTimer::Reset()
{
    __int64 currTime;
    QueryPerformanceCounter((LARGE_INTEGER*)&currTime); // 读计数器 = "现在"

    mBaseTime = currTime; // 零点
    mPrevTime = currTime; // 上一帧也从这里算起
    mStopTime = 0;
    mStopped = false;
    mPausedTime = 0; // 书里漏了这句，建议补上
}

void GameTimer::Start()
{
    if (mStopped)
    {
        __int64 currTime;
        QueryPerformanceCounter((LARGE_INTEGER*)&currTime);
        mPausedTime +=currTime-mStopTime;
        mPrevTime = currTime;
        mStopTime = 0;
        mStopped = false;
    }
}

void GameTimer::Stop()
{
    if (!mStopped)
    {
        QueryPerformanceCounter((LARGE_INTEGER*)&mStopTime);
        mStopped = true;
    }
}

void GameTimer::Tick()
{
    if (mStopped)
    {
        mDeltaTime = 0.0;
        return;
    }
    QueryPerformanceCounter((LARGE_INTEGER*)&mCurrTime);
    mDeltaTime = (mCurrTime - mPrevTime) * mSecondPerCount;
    mPrevTime = mCurrTime;
    mDeltaTime = max(mDeltaTime, 0.0);
}
