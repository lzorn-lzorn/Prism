#pragma once

#include "RHI.hpp"

#include <cstdint>
#include <future>
#include <memory>

namespace rhi
{

/** @brief Configures bounded background pipeline compilation concurrency. */
struct PipelineManagerDescriptor
{
    /** @brief Fixed background worker count; zero is normalized to at least one worker. */
    uint32_t WorkerCount { 1 };
};

/** @brief Snapshot of semantic cache and in-flight compilation counters. */
struct PipelineManagerStatistics
{
    uint64_t CacheHits { 0 };
    uint64_t CacheMisses { 0 };
    uint64_t PendingCompilations { 0 };
    uint64_t CachedPipelines { 0 };
};

/**
 * @brief 引擎级语义 Pipeline 缓存与有界异步编译管理器. 
 *
 * Manager 在创建 Native Pipeline 前从完整, 规范化 Descriptor 构造碰撞安全 Key. 
 * 同一 Key 的并发请求共享一个 shared_future；驱动编译期间不持有缓存锁. 
 * RDevice 由 Manager 共享持有，因此所有后台任务结束前设备不会提前销毁. 
 */
class PipelineManager final
{
public:
    /** @brief Creates a semantic pipeline cache for one device. @param Device Device retained for every worker task. @param Desc Worker-pool configuration. */
    explicit PipelineManager(
        std::shared_ptr<RDevice> Device,
        const PipelineManagerDescriptor& Desc = {});
    /** @brief Waits for workers and releases all cached pipelines. */
    ~PipelineManager();

    PipelineManager(const PipelineManager&) = delete;
    PipelineManager& operator=(const PipelineManager&) = delete;

    /** @brief Returns or synchronously compiles a graphics pipeline. @param Desc Complete immutable graphics descriptor. @return Shared cached pipeline, or nullptr when backend compilation fails. */
    [[nodiscard]] std::shared_ptr<RPipeline> getOrCreateGraphics(
        const GraphicsPipelineDescriptor& Desc);
    /** @brief Returns or synchronously compiles a compute pipeline. @param Desc Complete immutable compute descriptor. @return Shared cached pipeline, or nullptr when backend compilation fails. */
    [[nodiscard]] std::shared_ptr<RPipeline> getOrCreateCompute(
        const ComputePipelineDescriptor& Desc);
    /** @brief Returns or synchronously compiles a ray-tracing pipeline. @param Desc Complete immutable ray-tracing descriptor. @return Shared cached pipeline, or nullptr when unsupported or compilation fails. */
    [[nodiscard]] std::shared_ptr<RPipeline> getOrCreateRayTracing(
        const RayTracingPipelineDescriptor& Desc);
    /** @brief Requests deduplicated asynchronous graphics compilation. @param Desc Owning descriptor copied into the worker task. @return Shared future observed by all callers of the same semantic key. */
    [[nodiscard]] std::shared_future<std::shared_ptr<RPipeline>> getOrCreateGraphicsAsync(
        GraphicsPipelineDescriptor Desc);
    /** @brief Requests deduplicated asynchronous compute compilation. @param Desc Owning descriptor copied into the worker task. @return Shared future observed by all callers of the same semantic key. */
    [[nodiscard]] std::shared_future<std::shared_ptr<RPipeline>> getOrCreateComputeAsync(
        ComputePipelineDescriptor Desc);
    /** @brief Requests deduplicated asynchronous ray-tracing compilation. @param Desc Owning descriptor copied into the worker task. @return Shared future observed by all callers of the same semantic key. */
    [[nodiscard]] std::shared_future<std::shared_ptr<RPipeline>> getOrCreateRayTracingAsync(
        RayTracingPipelineDescriptor Desc);

    /** @brief Clears completed cache entries; in-flight work completes but cannot repopulate the cleared generation. */
    void clear();
    /** @brief Returns a lock-safe counter snapshot. @return Current hit, miss, pending, and cached counts. */
    [[nodiscard]] PipelineManagerStatistics getStatistics() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> Implementation;
};

} // namespace rhi
