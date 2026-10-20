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
    const auto* Begin = reinterpret_cast<const std::byte*>(&Value);
    OutKey.insert(OutKey.end(), Begin, Begin + sizeof(Value));
}

void appendBytes(PipelineKey& OutKey, std::span<const std::byte> Bytes)
{
    const uint64_t Size = Bytes.size();
    append(OutKey, Size);
    OutKey.insert(OutKey.end(), Bytes.begin(), Bytes.end());
}

void appendStage(PipelineKey& OutKey, const PipelineShaderStage& Stage)
{
    const uint64_t ShaderHash = Stage.Shader ? Stage.Shader->getContentHash() : 0;
    append(OutKey, ShaderHash);
    const auto ShaderStage = Stage.Shader ? Stage.Shader->getStage() : EShaderStage_t::Vertex;
    append(OutKey, ShaderStage);
    const std::string_view EntryPoint = Stage.Shader ? Stage.Shader->getEntryPoint() : std::string_view {};
    appendBytes(OutKey, std::as_bytes(std::span(EntryPoint)));
    auto Constants = Stage.SpecializationConstants;
    std::ranges::sort(Constants, {}, &SpecializationConstant::Id);
    const uint64_t Count = Constants.size();
    append(OutKey, Count);
    for (const auto& Constant : Constants)
    {
        append(OutKey, Constant.Id);
        appendBytes(OutKey, Constant.Data);
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
    PipelineKey Key;
    appendBytes(Key, Desc.Layout->getCompatibilityKey());
    for (const auto* Stage : { &Desc.Vertex, &Desc.Pixel, &Desc.Geometry, &Desc.Hull,
        &Desc.Domain, &Desc.Task, &Desc.Mesh })
        appendStage(Key, *Stage);

    const uint64_t BufferCount = Desc.VertexInput.Buffers.size();
    const uint64_t AttributeCount = Desc.VertexInput.Attributes.size();
    append(Key, BufferCount);
    for (const auto& Value : Desc.VertexInput.Buffers)
    {
        append(Key, Value.Binding); append(Key, Value.Stride); append(Key, Value.InputRate);
    }
    append(Key, AttributeCount);
    for (const auto& Value : Desc.VertexInput.Attributes)
    {
        append(Key, Value.Location); append(Key, Value.Binding);
        append(Key, Value.Format); append(Key, Value.Offset);
    }
    append(Key, Desc.InputAssembly.Topology); append(Key, Desc.InputAssembly.PrimitiveRestartEnable);
    append(Key, Desc.InputAssembly.PatchControlPoints);
    append(Key, Desc.Rasterizer.FillMode); append(Key, Desc.Rasterizer.CullMode);
    append(Key, Desc.Rasterizer.FrontFace); append(Key, Desc.Rasterizer.DepthClampEnable);
    append(Key, Desc.Rasterizer.RasterizerDiscardEnable); append(Key, Desc.Rasterizer.DepthBiasEnable);
    append(Key, Desc.Rasterizer.DepthBiasConstantFactor); append(Key, Desc.Rasterizer.DepthBiasClamp);
    append(Key, Desc.Rasterizer.DepthBiasSlopeFactor); append(Key, Desc.Rasterizer.LineWidth);
    append(Key, Desc.Multisample.SampleShadingEnable); append(Key, Desc.Multisample.MinSampleShading);
    append(Key, Desc.Multisample.SampleMask); append(Key, Desc.Multisample.AlphaToCoverageEnable);
    append(Key, Desc.Multisample.AlphaToOneEnable);
    append(Key, Desc.DepthStencil.DepthTestEnable); append(Key, Desc.DepthStencil.DepthWriteEnable);
    append(Key, Desc.DepthStencil.DepthCompareOp); append(Key, Desc.DepthStencil.DepthBoundsTestEnable);
    append(Key, Desc.DepthStencil.MinDepthBounds); append(Key, Desc.DepthStencil.MaxDepthBounds);
    append(Key, Desc.DepthStencil.StencilTestEnable);
    appendStencil(Key, Desc.DepthStencil.Front); appendStencil(Key, Desc.DepthStencil.Back);
    for (const auto& Attachment : Desc.Blend.Attachments)
    {
        append(Key, Attachment.BlendEnable);
        append(Key, Attachment.Color.SrcFactor); append(Key, Attachment.Color.DstFactor);
        append(Key, Attachment.Color.Operation); append(Key, Attachment.Alpha.SrcFactor);
        append(Key, Attachment.Alpha.DstFactor); append(Key, Attachment.Alpha.Operation);
        append(Key, Attachment.WriteMask.Value);
    }
    append(Key, Desc.Rendering.ColorAttachmentCount);
    for (const auto Format : Desc.Rendering.ColorFormats) append(Key, Format);
    append(Key, Desc.Rendering.DepthFormat); append(Key, Desc.Rendering.StencilFormat);
    append(Key, Desc.Rendering.SampleCount); append(Key, Desc.Rendering.ViewMask);
    append(Key, Desc.DynamicStates.Value); append(Key, Desc.Compile.Flags.Value);
    return Key;
}

PipelineKey makeComputeKey(const ComputePipelineDescriptor& Desc)
{
    if (!Desc.Layout)
        throw std::invalid_argument("Compute pipeline requires a layout.");
    PipelineKey Key;
    appendBytes(Key, Desc.Layout->getCompatibilityKey());
    appendStage(Key, Desc.Compute);
    append(Key, Desc.Compile.Flags.Value);
    return Key;
}

PipelineKey makeRayTracingKey(const RayTracingPipelineDescriptor& Desc)
{
    if (!Desc.Layout) throw std::invalid_argument("Ray-tracing pipeline requires a layout.");
    PipelineKey Key;
    appendBytes(Key, Desc.Layout->getCompatibilityKey());
    const uint64_t StageCount = Desc.Stages.size();
    append(Key, StageCount);
    for (const auto& Stage : Desc.Stages) appendStage(Key, Stage);
    const uint64_t GroupCount = Desc.Groups.size();
    append(Key, GroupCount);
    for (const auto& Group : Desc.Groups)
    {
        append(Key, Group.Type); append(Key, Group.GeneralShader);
        append(Key, Group.ClosestHitShader); append(Key, Group.AnyHitShader);
        append(Key, Group.IntersectionShader);
    }
    append(Key, Desc.MaxRecursionDepth);
    append(Key, Desc.Compile.Flags.Value);
    return Key;
}

struct PipelineKeyHash
{
    size_t operator()(const PipelineKey& Key) const noexcept
    {
        size_t Result = sizeof(size_t) == 8 ? 1469598103934665603ull : 2166136261u;
        for (const auto Byte : Key)
        {
            Result ^= static_cast<size_t>(std::to_integer<uint8_t>(Byte));
            Result *= sizeof(size_t) == 8 ? 1099511628211ull : 16777619u;
        }
        return Result;
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
        const uint32_t NumWorkers = std::max(1u, WorkerCount);
        Workers.reserve(NumWorkers);
        for (uint32_t Index = 0; Index < NumWorkers; ++Index)
            Workers.emplace_back([this](std::stop_token Token) { workerLoop(Token); });
    }

    ~Impl()
    {
        for (auto& Worker : Workers) Worker.request_stop();
        QueueCondition.notify_all();
        Workers.clear();
    }

    template <typename Descriptor, typename CreateFunction>
    std::shared_future<std::shared_ptr<RPipeline>> request(
        PipelineKey Key,
        Descriptor Desc,
        CreateFunction Create)
    {
        std::shared_ptr<std::promise<std::shared_ptr<RPipeline>>> Promise;
        std::shared_future<std::shared_ptr<RPipeline>> Future;
        uint64_t RequestGeneration = 0;
        {
            std::scoped_lock Lock(CacheMutex);
            if (const auto Found = Cache.find(Key); Found != Cache.end())
            {
                ++CacheHits;
                return Found->second;
            }
            ++CacheMisses;
            Promise = std::make_shared<std::promise<std::shared_ptr<RPipeline>>>();
            Future = Promise->get_future().share();
            Cache.emplace(Key, Future);
            RequestGeneration = Generation;
            ++PendingCompilations;
        }

        enqueue([this, Key = std::move(Key), Desc = std::move(Desc), Promise,
            Create = std::move(Create), RequestGeneration]() mutable
        {
            try
            {
                auto Pipeline = Create(*Device, Desc);
                Promise->set_value(std::move(Pipeline));
            }
            catch (...)
            {
                Promise->set_exception(std::current_exception());
                std::scoped_lock Lock(CacheMutex);
                if (Generation == RequestGeneration) Cache.erase(Key);
            }
            --PendingCompilations;
        });
        return Future;
    }

    void clear()
    {
        std::scoped_lock Lock(CacheMutex);
        ++Generation;
        Cache.clear();
    }

    PipelineManagerStatistics statistics() const noexcept
    {
        std::scoped_lock Lock(CacheMutex);
        return { CacheHits.load(), CacheMisses.load(), PendingCompilations.load(), Cache.size() };
    }

private:
    void enqueue(std::function<void()> Work)
    {
        {
            std::scoped_lock Lock(QueueMutex);
            Queue.emplace_back(std::move(Work));
        }
        QueueCondition.notify_one();
    }

    void workerLoop(std::stop_token Token)
    {
        while (true)
        {
            std::function<void()> Work;
            {
                std::unique_lock Lock(QueueMutex);
                QueueCondition.wait(Lock, Token, [this] { return !Queue.empty(); });
                if (Queue.empty())
                {
                    if (Token.stop_requested()) return;
                    continue;
                }
                Work = std::move(Queue.front());
                Queue.pop_front();
            }
            Work();
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
        [](RDevice& Device, const GraphicsPipelineDescriptor& Descriptor)
        {
            return Device.createGraphicsPipeline(Descriptor);
        });
}

std::shared_future<std::shared_ptr<RPipeline>> PipelineManager::getOrCreateComputeAsync(
    ComputePipelineDescriptor Desc)
{
    return Implementation->request(
        makeComputeKey(Desc), std::move(Desc),
        [](RDevice& Device, const ComputePipelineDescriptor& Descriptor)
        {
            return Device.createComputePipeline(Descriptor);
        });
}

std::shared_future<std::shared_ptr<RPipeline>> PipelineManager::getOrCreateRayTracingAsync(
    RayTracingPipelineDescriptor Desc)
{
    return Implementation->request(
        makeRayTracingKey(Desc), std::move(Desc),
        [](RDevice& Device, const RayTracingPipelineDescriptor& Descriptor)
        {
            return Device.createRayTracingPipeline(Descriptor);
        });
}

void PipelineManager::clear() { Implementation->clear(); }
PipelineManagerStatistics PipelineManager::getStatistics() const noexcept
{
    return Implementation->statistics();
}

} // namespace rhi
