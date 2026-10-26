#pragma once

#include <cstdint>

namespace runtime
{

/**
 * 模拟时钟(Simulation Clock): 把"渲染帧步长"与"仿真步长"解耦。
 *
 * 编辑器 UI 按显示帧率跑(每渲染帧调用 advance 一次), 而游戏世界按自己的节奏推进:
 *  - 固定步长(FixedTimestep > 0): 内部累加器把真实时间切成等长仿真步, 一次渲染帧
 *    可推进零到多步(追赶), 剩余时间留到下一帧 —— 逻辑/物理确定性由此而来;
 *  - 可变步长(FixedTimestep <= 0): 每渲染帧推进一次, 步长 = 真实步长 * TimeScale;
 *  - 暂停/单步: pause 后不再推进; requestStep 在暂停下推进恰好一步;
 *  - 慢放/加速: TimeScale 缩放仿真推进速率;
 *  - 脱离渲染模拟(SimulateWithoutRender): 由宿主读取后跳过渲染, 允许在两次渲染之间
 *    连续追赶多步(批量物理结算 / 时间缩放重放 / 无头仿真)。
 */
class SimulationClock
{
public:
    void pause() noexcept
    {
        Paused = true;
        StepRequested = false;
    }
    void resume() noexcept
    {
        Paused = false;
        StepRequested = false;
    }
    void togglePause() noexcept
    {
        Paused ? resume() : pause();
    }

    /** @brief 请求推进恰好一步(暂停状态下有效; 运行状态下无副作用). */
    void requestStep() noexcept { StepRequested = true; }

    void setTimeScale(float Scale) noexcept { TimeScale = Scale > 0.0f ? Scale : 0.0f; }
    /** @brief 固定仿真步长(秒); 传入 <= 0 表示可变步长(跟随渲染帧). */
    void setFixedTimestep(float StepSeconds) noexcept
    {
        FixedTimestep = StepSeconds > 0.0f ? StepSeconds : 0.0f;
    }
    void setSimulateWithoutRender(bool Value) noexcept { SimulateWithoutRender = Value; }
    void setMaxStepsPerFrame(uint32_t MaxSteps) noexcept { MaxStepsPerFrame = MaxSteps ? MaxSteps : 1; }

    [[nodiscard]] bool isPaused() const noexcept { return Paused; }
    [[nodiscard]] float getTimeScale() const noexcept { return TimeScale; }
    [[nodiscard]] float getFixedTimestep() const noexcept { return FixedTimestep; }
    [[nodiscard]] bool isSimulateWithoutRender() const noexcept { return SimulateWithoutRender; }
    [[nodiscard]] uint32_t getMaxStepsPerFrame() const noexcept { return MaxStepsPerFrame; }

    /**
     * @brief 用本帧真实步长推进仿真。
     * @param RealDeltaTime 渲染帧真实步长(秒, 未缩放)。
     * @param StepFn 每个仿真步执行的回调, 参数为该步的仿真步长(秒, 已含 TimeScale)。
     * @return 本帧实际执行的仿真步数(暂停且无单步请求时为 0)。
     */
    template <class Fn>
    uint32_t advance(float RealDeltaTime, Fn&& StepFn)
    {
        constexpr float NominalDt = 1.0f / 60.0f;
        uint32_t Steps = 0;

        if (Paused)
        {
            if (StepRequested)
            {
                StepRequested = false;
                const float Dt = FixedTimestep > 0.0f ? FixedTimestep : NominalDt;
                StepFn(Dt);
                Steps = 1;
            }
            return Steps;
        }

        StepRequested = false;

        if (FixedTimestep > 0.0f)
        {
            Accumulator += RealDeltaTime * TimeScale;
            while (Accumulator >= FixedTimestep && Steps < MaxStepsPerFrame)
            {
                Accumulator -= FixedTimestep;
                StepFn(FixedTimestep);
                ++Steps;
            }
            // 避免"死亡螺旋": 单帧落后过多时丢弃剩余时间, 而非无限追赶.
            if (Accumulator >= FixedTimestep)
                Accumulator = 0.0f;
        }
        else
        {
            StepFn(RealDeltaTime * TimeScale);
            Steps = 1;
        }

        return Steps;
    }

private:
    bool Paused { false };
    bool StepRequested { false };
    float TimeScale { 1.0f };
    float FixedTimestep { 0.0f }; // <= 0 => 可变步长
    float Accumulator { 0.0f };
    bool SimulateWithoutRender { false };
    uint32_t MaxStepsPerFrame { 8 };
};

} // namespace runtime
