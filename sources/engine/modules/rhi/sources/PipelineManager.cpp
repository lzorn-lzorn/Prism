#include "PipelineManager.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rhi
{
namespace
{
using PipelineKey = std::vector<std::byte>;

template <typename ValueType>
void append(PipelineKey& OutKey, const ValueType& Value)
{
    static_assert(std::is_trivially_copyable_v<ValueType>);
    const auto* begin = reinterpret_cast<const std::byte*>(&Value);
    OutKey.insert(OutKey.end(), begin, begin + sizeof(Value));
}

void appendBytes(PipelineKey& OutKey, std::span<const std::byte> Bytes)
{
    const uint64_t size = Bytes.size();
    append(OutKey, size);
    OutKey.insert(OutKey.end(), Bytes.begin(), Bytes.end());
}

void appendStage(PipelineKey& OutKey, const PipelineShaderStage& Stage)
{
    const uint64_t shader_hash = Stage.Shader ? Stage.Shader->getContentHash() : 0;
    append(OutKey, shader_hash);
    const auto shader_stage = Stage.Shader ? Stage.Shader->getStage() : EShaderStage_t::Vertex;
    append(OutKey, shader_stage);
    const std::string_view entry_point = Stage.Shader ? Stage.Shader->getEntryPoint() : std::string_view {};
    appendBytes(OutKey, std::as_bytes(std::span(entry_point)));
    auto constants = Stage.SpecializationConstants;
    std::ranges::sort(constants, {}, &SpecializationConstant::Id);
    const uint64_t count = constants.size();
    append(OutKey, count);
    for (const auto& constant : constants)
    {
        append(OutKey, constant.Id);
        appendBytes(OutKey, constant.Data);
    }
}

void appendStencil(PipelineKey& OutKey, const StencilFaceState& State)
{
    append(OutKey, State.FailOp); append(OutKey, State.PassOp);
    append(OutKey, State.DepthFailOp); append(OutKey, State.CompareOp);
    append(OutKey, State.CompareMask); append(OutKey, State.WriteMask);
    append(OutKey, State.Reference);
}

PipelineKey makeGraphicsKey(const GraphicsPipelineDescriptor& Desc)
{
    if (!Desc.Layout)
        throw std::invalid_argument("Graphics pipeline requires a layout.");
    PipelineKey key;
    appendBytes(key, Desc.Layout->getCompatibilityKey());
    for (const auto* stage : { &Desc.Vertex, &Desc.Pixel, &Desc.Geometry, &Desc.Hull,
        &Desc.Domain, &Desc.Task, &Desc.Mesh })
        appendStage(key, *stage);

    const uint64_t buffer_count = Desc.VertexInput.Buffers.size();
    const uint64_t attribute_count = Desc.VertexInput.Attributes.size();
    append(key, buffer_count);
    for (const auto& value : Desc.VertexInput.Buffers)
    {
        append(key, value.Binding); append(key, value.Stride); append(key, value.InputRate);
    }
    append(key, attribute_count);
    for (const auto& value : Desc.VertexInput.Attributes)
    {
        append(key, value.Location); append(key, value.Binding);
        append(key, value.Format); append(key, value.Offset);
    }
    append(key, Desc.InputAssembly.Topology); append(key, Desc.InputAssembly.PrimitiveRestartEnable);
    append(key, Desc.InputAssembly.PatchControlPoints);
    append(key, Desc.Rasterizer.FillMode); append(key, Desc.Rasterizer.CullMode);
    append(key, Desc.Rasterizer.FrontFace); append(key, Desc.Rasterizer.DepthClampEnable);
    append(key, Desc.Rasterizer.RasterizerDiscardEnable); append(key, Desc.Rasterizer.DepthBiasEnable);
    append(key, Desc.Rasterizer.DepthBiasConstantFactor); append(key, Desc.Rasterizer.DepthBiasClamp);
    append(key, Desc.Rasterizer.DepthBiasSlopeFactor); append(key, Desc.Rasterizer.LineWidth);
    append(key, Desc.Multisample.SampleShadingEnable); append(key, Desc.Multisample.MinSampleShading);
    append(key, Desc.Multisample.SampleMask); append(key, Desc.Multisample.AlphaToCoverageEnable);
    append(key, Desc.Multisample.AlphaToOneEnable);
    append(key, Desc.DepthStencil.DepthTestEnable); append(key, Desc.DepthStencil.DepthWriteEnable);
    append(key, Desc.DepthStencil.DepthCompareOp); append(key, Desc.DepthStencil.DepthBoundsTestEnable);
    append(key, Desc.DepthStencil.MinDepthBounds); append(key, Desc.DepthStencil.MaxDepthBounds);
    append(key, Desc.DepthStencil.StencilTestEnable);
    appendStencil(key, Desc.DepthStencil.Front); appendStencil(key, Desc.DepthStencil.Back);
    for (const auto& attachment : Desc.Blend.Attachments)
    {
        append(key, attachment.BlendEnable);
        append(key, attachment.Color.SrcFactor); append(key, attachment.Color.DstFactor);
        append(key, attachment.Color.Operation); append(key, attachment.Alpha.SrcFactor);
        append(key, attachment.Alpha.DstFactor); append(key, attachment.Alpha.Operation);
        append(key, attachment.WriteMask.Value);
    }
    append(key, Desc.Rendering.ColorAttachmentCount);
    for (const auto format : Desc.Rendering.ColorFormats) append(key, format);
    append(key, Desc.Rendering.DepthFormat); append(key, Desc.Rendering.StencilFormat);
    append(key, Desc.Rendering.SampleCount); append(key, Desc.Rendering.ViewMask);
    append(key, Desc.DynamicStates.Value); append(key, Desc.Compile.Flags.Value);
    return key;
}

PipelineKey makeComputeKey(const ComputePipelineDescriptor& Desc)
{
    if (!Desc.Layout)
        throw std::invalid_argument("Compute pipeline requires a layout.");
    PipelineKey key;
    appendBytes(key, Desc.Layout->getCompatibilityKey());
    appendStage(key, Desc.Compute);
    append(key, Desc.Compile.Flags.Value);
    return key;
}

PipelineKey makeRayTracingKey(const RayTracingPipelineDescriptor& Desc)
{
    if (!Desc.Layout) throw std::invalid_argument("Ray-tracing pipeline requires a layout.");
    PipelineKey key;
    appendBytes(key, Desc.Layout->getCompatibilityKey());
    const uint64_t stage_count = Desc.Stages.size();
    append(key, stage_count);
    for (const auto& stage : Desc.Stages) appendStage(key, stage);
    const uint64_t group_count = Desc.Groups.size();
    append(key, group_count);
    for (const auto& group : Desc.Groups)
    {
        append(key, group.Type); append(key, group.GeneralShader);
        append(key, group.ClosestHitShader); append(key, group.AnyHitShader);
        append(key, group.IntersectionShader);
    }
    append(key, Desc.MaxRecursionDepth);
    append(key, Desc.Compile.Flags.Value);
    return key;
}

struct PipelineKeyHash
{
    size_t operator()(const PipelineKey& Key) const noexcept
    {
        size_t result = sizeof(size_t) == 8 ? 1469598103934665603ull : 2166136261u;
        for (const auto byte : Key)
        {
            result ^= static_cast<size_t>(std::to_integer<uint8_t>(byte));
            result *= sizeof(size_t) == 8 ? 1099511628211ull : 16777619u;
        }
        return result;
    }
};
} // namespace

class PipelineManager::Impl
{
public:
    Impl(std::shared_ptr<RDevice> InDevice, uint32_t WorkerCount)
        : Device(std::move(InDevice))
    {
        if (!Device)
            throw std::invalid_argument("PipelineManager requires a device.");
        const uint32_t worker_count = std::max(1u, WorkerCount);
        Workers.reserve(worker_count);
        for (uint32_t index = 0; index < worker_count; ++index)
            Workers.emplace_back([this](std::stop_token token) { workerLoop(token); });
    }

    ~Impl()
    {
        for (auto& worker : Workers) worker.request_stop();
        QueueCondition.notify_all();
        Workers.clear();
    }

    template <typename Descriptor, typename CreateFunction>
    std::shared_future<std::shared_ptr<RPipeline>> request(
        PipelineKey Key,
        Descriptor Desc,
        CreateFunction Create)
    {
        std::shared_ptr<std::promise<std::shared_ptr<RPipeline>>> promise;
        std::shared_future<std::shared_ptr<RPipeline>> future;
        uint64_t request_generation = 0;
        {
            std::scoped_lock lock(CacheMutex);
            if (const auto found = Cache.find(Key); found != Cache.end())
            {
                ++CacheHits;
                return found->second;
            }
            ++CacheMisses;
            promise = std::make_shared<std::promise<std::shared_ptr<RPipeline>>>();
            future = promise->get_future().share();
            Cache.emplace(Key, future);
            request_generation = Generation;
            ++PendingCompilations;
        }

        enqueue([this, key = std::move(Key), desc = std::move(Desc), promise,
            create = std::move(Create), request_generation]() mutable
        {
            try
            {
                auto pipeline = create(*Device, desc);
                promise->set_value(std::move(pipeline));
            }
            catch (...)
            {
                promise->set_exception(std::current_exception());
                std::scoped_lock lock(CacheMutex);
                if (Generation == request_generation) Cache.erase(key);
            }
            --PendingCompilations;
        });
        return future;
    }

    void clear()
    {
        std::scoped_lock lock(CacheMutex);
        ++Generation;
        Cache.clear();
    }

    PipelineManagerStatistics statistics() const noexcept
    {
        std::scoped_lock lock(CacheMutex);
        return { CacheHits.load(), CacheMisses.load(), PendingCompilations.load(), Cache.size() };
    }

private:
    void enqueue(std::function<void()> Work)
    {
        {
            std::scoped_lock lock(QueueMutex);
            Queue.emplace_back(std::move(Work));
        }
        QueueCondition.notify_one();
    }

    void workerLoop(std::stop_token Token)
    {
        while (true)
        {
            std::function<void()> work;
            {
                std::unique_lock lock(QueueMutex);
                QueueCondition.wait(lock, Token, [this] { return !Queue.empty(); });
                if (Queue.empty())
                {
                    if (Token.stop_requested()) return;
                    continue;
                }
                work = std::move(Queue.front());
                Queue.pop_front();
            }
            work();
        }
    }

    std::shared_ptr<RDevice> Device;
    mutable std::mutex CacheMutex;
    std::unordered_map<PipelineKey, std::shared_future<std::shared_ptr<RPipeline>>, PipelineKeyHash> Cache;
    uint64_t Generation { 0 };
    std::atomic_uint64_t CacheHits { 0 };
    std::atomic_uint64_t CacheMisses { 0 };
    std::atomic_uint64_t PendingCompilations { 0 };
    std::mutex QueueMutex;
    std::condition_variable_any QueueCondition;
    std::deque<std::function<void()>> Queue;
    std::vector<std::jthread> Workers;
};

PipelineManager::PipelineManager(
    std::shared_ptr<RDevice> Device,
    const PipelineManagerDescriptor& Desc)
    : Implementation(std::make_unique<Impl>(std::move(Device), Desc.WorkerCount))
{
}

PipelineManager::~PipelineManager() = default;

std::shared_ptr<RPipeline> PipelineManager::getOrCreateGraphics(
    const GraphicsPipelineDescriptor& Desc)
{
    return getOrCreateGraphicsAsync(Desc).get();
}

std::shared_ptr<RPipeline> PipelineManager::getOrCreateCompute(
    const ComputePipelineDescriptor& Desc)
{
    return getOrCreateComputeAsync(Desc).get();
}

std::shared_ptr<RPipeline> PipelineManager::getOrCreateRayTracing(
    const RayTracingPipelineDescriptor& Desc)
{
    return getOrCreateRayTracingAsync(Desc).get();
}

std::shared_future<std::shared_ptr<RPipeline>> PipelineManager::getOrCreateGraphicsAsync(
    GraphicsPipelineDescriptor Desc)
{
    return Implementation->request(
        makeGraphicsKey(Desc), std::move(Desc),
        [](RDevice& device, const GraphicsPipelineDescriptor& descriptor)
        {
            return device.createGraphicsPipeline(descriptor);
        });
}

std::shared_future<std::shared_ptr<RPipeline>> PipelineManager::getOrCreateComputeAsync(
    ComputePipelineDescriptor Desc)
{
    return Implementation->request(
        makeComputeKey(Desc), std::move(Desc),
        [](RDevice& device, const ComputePipelineDescriptor& descriptor)
        {
            return device.createComputePipeline(descriptor);
        });
}

std::shared_future<std::shared_ptr<RPipeline>> PipelineManager::getOrCreateRayTracingAsync(
    RayTracingPipelineDescriptor Desc)
{
    return Implementation->request(
        makeRayTracingKey(Desc), std::move(Desc),
        [](RDevice& device, const RayTracingPipelineDescriptor& descriptor)
        {
            return device.createRayTracingPipeline(descriptor);
        });
}

void PipelineManager::clear() { Implementation->clear(); }
PipelineManagerStatistics PipelineManager::getStatistics() const noexcept
{
    return Implementation->statistics();
}

} // namespace rhi
