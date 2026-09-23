#pragma once

class GameTimer
{
public:
    GameTimer();
    
    float TotalTime() const;    //秒为单位
    float DeltaTime() const;
    
    void Reset();   //在开始消息之前调用
    void Start();   //解除计时器暂停时调用
    void Stop();    //暂停计时器时调用
    void Tick();    //每帧都要调用
    
private:
    double mSecondPerCount; 
    double mDeltaTime;
    
    __int64 mBaseTime;
    __int64 mPausedTime;
    __int64 mStopTime;
    __int64 mPrevTime;
    __int64 mCurrTime;
    
    bool mStopped;
};
