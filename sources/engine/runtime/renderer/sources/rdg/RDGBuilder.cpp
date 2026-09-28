#include <rdg/RDGBuilder.hpp>

#include <stdexcept>
#include <utility>

namespace runtime::renderer
{

using detail::EResourceKind;
using detail::RDGAccess;
using detail::RDGAttachmentDecl;
using detail::RDGPass;
using detail::RDGResource;

// ============================================================================
//  RDGPassBuilder
// ============================================================================

RDGResourceId RDGPassBuilder::createTexture(RDGTextureDesc Desc, std::string_view Name)
{
	return Builder.declareTexture(std::move(Desc), Name);
}

RDGResourceId RDGPassBuilder::createBuffer(RDGBufferDesc Desc, std::string_view Name)
{
	return Builder.declareBuffer(std::move(Desc), Name);
}

void RDGPassBuilder::read(RDGResourceId Resource, rhi::EResourceState State)
{
	Pass.Reads.push_back({ Resource, State });
}

void RDGPassBuilder::write(RDGResourceId Resource, rhi::EResourceState State)
{
	Pass.Writes.push_back({ Resource, State });
}

void RDGPassBuilder::renderTarget(RDGResourceId Resource, rhi::ELoadOp LoadOp,
	rhi::EStoreOp StoreOp, rhi::ClearColorValue Clear)
{
	Pass.ColorAttachments.push_back({ Resource, LoadOp, StoreOp, Clear, {} });
}

void RDGPassBuilder::depthStencil(RDGResourceId Resource, rhi::ELoadOp LoadOp,
	rhi::EStoreOp StoreOp, rhi::ClearDepthStencilValue Clear)
{
	Pass.DepthAttachment = { Resource, LoadOp, StoreOp, {}, Clear };
}

// ============================================================================
//  RDGExecuteContext
// ============================================================================

rhi::RCommandList& RDGExecuteContext::getCommandList() const noexcept
{
	return CommandList;
}

const std::shared_ptr<rhi::RImage>& RDGExecuteContext::getImage(RDGResourceId Resource) const
{
	static const std::shared_ptr<rhi::RImage> EmptyImage;
	const RDGResource& Res = Builder.getResource(Resource);
	return Res.Kind == EResourceKind::Texture ? Res.Image : EmptyImage;
}

const std::shared_ptr<rhi::RImageView>& RDGExecuteContext::getView(RDGResourceId Resource) const
{
	static const std::shared_ptr<rhi::RImageView> EmptyView;
	const RDGResource& Res = Builder.getResource(Resource);
	return Res.Kind == EResourceKind::Texture ? Res.ImageView : EmptyView;
}

const std::shared_ptr<rhi::RBuffer>& RDGExecuteContext::getBuffer(RDGResourceId Resource) const
{
	static const std::shared_ptr<rhi::RBuffer> EmptyBuffer;
	const RDGResource& Res = Builder.getResource(Resource);
	return Res.Kind == EResourceKind::Buffer ? Res.Buffer : EmptyBuffer;
}

// ============================================================================
//  RDGBuilder
// ============================================================================

RDGBuilder::~RDGBuilder() = default;

RDGResourceId RDGBuilder::importImage(const std::shared_ptr<rhi::RImage>& Image,
	const std::shared_ptr<rhi::RImageView>& View, std::string_view Name,
	rhi::EResourceState InitialState)
{
	if (!Image)
		throw std::invalid_argument("RDGBuilder::importImage: Image 不能为空.");

	RDGResource Resource;
	Resource.Kind = EResourceKind::Texture;
	Resource.TextureDesc.Format = Image->getDescriptor().Format;
	Resource.TextureDesc.Width = Image->getDescriptor().Width;
	Resource.TextureDesc.Height = Image->getDescriptor().Height;
	Resource.TextureDesc.Depth = Image->getDescriptor().Depth;
	Resource.TextureDesc.MipLevels = Image->getDescriptor().MipLevels;
	Resource.TextureDesc.ArrayLayers = Image->getDescriptor().ArrayLayers;
	Resource.TextureDesc.Usage = Image->getDescriptor().Usage;
	Resource.TextureDesc.Samples = Image->getDescriptor().SampleCount;
	Resource.Name = std::string(Name);
	Resource.Imported = true;
	Resource.LastState = InitialState;
	Resource.Image = Image;
	Resource.ImageView = View ? View : Device.createImageView({ .Image = Image });

	Resources.push_back(std::move(Resource));
	return static_cast<RDGResourceId>(Resources.size() - 1);
}

RDGResourceId RDGBuilder::importBuffer(const std::shared_ptr<rhi::RBuffer>& Buffer,
	std::string_view Name, rhi::EResourceState InitialState)
{
	if (!Buffer)
		throw std::invalid_argument("RDGBuilder::importBuffer: Buffer 不能为空.");

	RDGResource Resource;
	Resource.Kind = EResourceKind::Buffer;
	Resource.BufferDesc.Size = Buffer->getDescriptor().Size;
	Resource.BufferDesc.Usage = Buffer->getDescriptor().Usage;
	Resource.Name = std::string(Name);
	Resource.Imported = true;
	Resource.LastState = InitialState;
	Resource.Buffer = Buffer;

	Resources.push_back(std::move(Resource));
	return static_cast<RDGResourceId>(Resources.size() - 1);
}

void RDGBuilder::addGraphicsPass(std::string_view Name,
	std::function<void(RDGPassBuilder&)> Setup,
	std::function<void(RDGExecuteContext&)> Execute)
{
	RDGPass Pass;
	Pass.Name = std::string(Name);
	Pass.Type = ERDGPassType::Graphics;
	Pass.Setup = std::move(Setup);
	Pass.Execute = std::move(Execute);

	RDGPassBuilder Builder(Pass, *this);
	if (Pass.Setup)
		Pass.Setup(Builder);

	Passes.push_back(std::move(Pass));
}

void RDGBuilder::addComputePass(std::string_view Name,
	std::function<void(RDGPassBuilder&)> Setup,
	std::function<void(RDGExecuteContext&)> Execute)
{
	RDGPass Pass;
	Pass.Name = std::string(Name);
	Pass.Type = ERDGPassType::Compute;
	Pass.Setup = std::move(Setup);
	Pass.Execute = std::move(Execute);

	RDGPassBuilder Builder(Pass, *this);
	if (Pass.Setup)
		Pass.Setup(Builder);

	Passes.push_back(std::move(Pass));
}

void RDGBuilder::addCopyPass(std::string_view Name,
	std::function<void(RDGPassBuilder&)> Setup,
	std::function<void(RDGExecuteContext&)> Execute)
{
	RDGPass Pass;
	Pass.Name = std::string(Name);
	Pass.Type = ERDGPassType::Copy;
	Pass.Setup = std::move(Setup);
	Pass.Execute = std::move(Execute);

	RDGPassBuilder Builder(Pass, *this);
	if (Pass.Setup)
		Pass.Setup(Builder);

	Passes.push_back(std::move(Pass));
}

void RDGBuilder::compile()
{
	// v1: Pass 按声明顺序执行. 逐 Pass 追踪资源状态并生成 barrier.
	for (uint32_t PassIndex = 0; PassIndex < Passes.size(); ++PassIndex)
	{
		RDGPass& Pass = Passes[PassIndex];

		// 附件先作为写访问处理(状态: RenderTarget / DepthWrite).
		for (const RDGAttachmentDecl& Attachment : Pass.ColorAttachments)
		{
			RDGResource& Resource = getResource(Attachment.Resource);
			planAccess(Pass, Resource, rhi::EResourceState::RenderTarget, true);
		}
		if (Pass.DepthAttachment)
		{
			RDGResource& Resource = getResource(Pass.DepthAttachment->Resource);
			planAccess(Pass, Resource, rhi::EResourceState::DepthWrite, true);
		}

		// 普通写访问.
		for (const RDGAccess& Access : Pass.Writes)
		{
			RDGResource& Resource = getResource(Access.Resource);
			planAccess(Pass, Resource, Access.State, true);
		}

		// 读访问.
		for (const RDGAccess& Access : Pass.Reads)
		{
			RDGResource& Resource = getResource(Access.Resource);
			planAccess(Pass, Resource, Access.State, false);
		}
	}
}

void RDGBuilder::execute(rhi::RCommandList& CommandList)
{
	for (const RDGPass& Pass : Passes)
	{
		// 1. 先下发本 Pass 的 barrier.
		if (!Pass.GlobalBarriersBefore.empty() || !Pass.BufferBarriersBefore.empty() ||
			!Pass.ImageBarriersBefore.empty())
		{
			CommandList.barriers(Pass.GlobalBarriersBefore, Pass.BufferBarriersBefore,
				Pass.ImageBarriersBefore);
		}

		RDGExecuteContext Context(CommandList, *this);

		// 2. Graphics Pass 需要自动包裹 rendering scope.
		if (Pass.Type == ERDGPassType::Graphics)
		{
			rhi::RenderingInfo Info;
			std::vector<rhi::ColorAttachment> ColorAttachments;
			ColorAttachments.reserve(Pass.ColorAttachments.size());

			for (const RDGAttachmentDecl& Attachment : Pass.ColorAttachments)
			{
				const RDGResource& Resource = getResource(Attachment.Resource);
				if (!Resource.ImageView)
					throw std::logic_error("RDGBuilder::execute: 颜色附件视图为空.");
				ColorAttachments.push_back({
					.View = Resource.ImageView,
					.LoadOp = Attachment.LoadOp,
					.StoreOp = Attachment.StoreOp,
					.ClearValue = Attachment.ClearColor,
				});
			}
			if (!ColorAttachments.empty())
			{
				Info.ColorAttachments = ColorAttachments;
				// 渲染区域取自第一个颜色附件的尺寸.
				const RDGResource& First = getResource(Pass.ColorAttachments.front().Resource);
				Info.Area = { 0, 0, First.TextureDesc.Width, First.TextureDesc.Height };
			}

			rhi::DepthStencilAttachment DepthAttachment;
			if (Pass.DepthAttachment)
			{
				const RDGResource& Resource = getResource(Pass.DepthAttachment->Resource);
				if (!Resource.ImageView)
					throw std::logic_error("RDGBuilder::execute: 深度附件视图为空.");
				DepthAttachment.View = Resource.ImageView;
				DepthAttachment.Depth.LoadOp = Pass.DepthAttachment->LoadOp;
				DepthAttachment.Depth.StoreOp = Pass.DepthAttachment->StoreOp;
				DepthAttachment.Stencil.LoadOp = rhi::ELoadOp::DontCare;
				DepthAttachment.Stencil.StoreOp = rhi::EStoreOp::DontCare;
				DepthAttachment.ClearValue = Pass.DepthAttachment->ClearDepth;
				Info.DepthStencil = &DepthAttachment;

				// 无颜色附件时, 渲染区域取自深度附件.
				if (Info.Area.Width == 0)
					Info.Area = { 0, 0, Resource.TextureDesc.Width,
						Resource.TextureDesc.Height };
			}

			if (Info.Area.Width == 0)
				throw std::logic_error("RDGBuilder::execute: Graphics Pass 缺少附件.");

			CommandList.beginRendering(Info);
			if (Pass.Execute)
				Pass.Execute(Context);
			CommandList.endRendering();
		}
		else
		{
			// Compute / Copy: 无渲染作用域.
			if (Pass.Execute)
				Pass.Execute(Context);
		}
	}
}

void RDGBuilder::clear()
{
	Passes.clear();
	Resources.clear();
	// TransientCache 保留: 下一帧同名资源复用物理 RHI 句柄.
}

// ============================================================================
//  内部实现
// ============================================================================

RDGResourceId RDGBuilder::declareTexture(RDGTextureDesc Desc, std::string_view Name)
{
	RDGResource Resource;
	Resource.Kind = EResourceKind::Texture;
	Resource.TextureDesc = std::move(Desc);
	Resource.Name = std::string(Name);

	// 跨帧复用: 同名且描述一致的 transient 资源直接复用上次分配的物理句柄.
	auto It = TransientCache.find(Resource.Name);
	if (It != TransientCache.end() && It->second.Kind == EResourceKind::Texture &&
		It->second.TextureDesc == Resource.TextureDesc)
	{
		Resource.Image = It->second.Image;
		Resource.ImageView = It->second.ImageView;
	}

	Resources.push_back(std::move(Resource));
	return static_cast<RDGResourceId>(Resources.size() - 1);
}

RDGResourceId RDGBuilder::declareBuffer(RDGBufferDesc Desc, std::string_view Name)
{
	RDGResource Resource;
	Resource.Kind = EResourceKind::Buffer;
	Resource.BufferDesc = std::move(Desc);
	Resource.Name = std::string(Name);

	auto It = TransientCache.find(Resource.Name);
	if (It != TransientCache.end() && It->second.Kind == EResourceKind::Buffer &&
		It->second.BufferDesc == Resource.BufferDesc)
	{
		Resource.Buffer = It->second.Buffer;
	}

	Resources.push_back(std::move(Resource));
	return static_cast<RDGResourceId>(Resources.size() - 1);
}

RDGResource& RDGBuilder::getResource(RDGResourceId Id)
{
	if (Id >= Resources.size())
		throw std::out_of_range("RDGBuilder: 无效资源句柄.");
	return Resources[Id];
}

const RDGResource& RDGBuilder::getResource(RDGResourceId Id) const
{
	if (Id >= Resources.size())
		throw std::out_of_range("RDGBuilder: 无效资源句柄.");
	return Resources[Id];
}

void RDGBuilder::ensureAllocated(RDGResource& Resource)
{
	if (Resource.Imported)
		return;

	if (Resource.Kind == EResourceKind::Texture)
	{
		if (Resource.Image)
			return;
		const RDGTextureDesc& D = Resource.TextureDesc;
		rhi::RImage::Descriptor_t ImageDesc {
			.Format = D.Format,
			.Dimension = rhi::EImageDimension::Texture2D,
			.Width = D.Width,
			.Height = D.Height,
			.Depth = D.Depth,
			.MipLevels = D.MipLevels,
			.ArrayLayers = D.ArrayLayers,
			.SharingMode = rhi::ESharingMode::Exclusive,
			.MemoryProperty = rhi::EMemoryProperty_t::DeviceLocal,
			.Usage = D.Usage,
			.SampleCount = D.Samples,
		};
		Resource.Image = Device.createImage(ImageDesc);
		Resource.ImageView = Device.createImageView({ .Image = Resource.Image });
	}
	else
	{
		if (Resource.Buffer)
			return;
		const RDGBufferDesc& D = Resource.BufferDesc;
		Resource.Buffer = Device.createBuffer(rhi::RBuffer::Descriptor_t {
			.Size = D.Size,
			.Usage = D.Usage,
			.MemoryUsage = rhi::EMemoryUsage::GPUOnly,
			.DebugName = Resource.Name,
		});
	}

	// 保存物理句柄供下一帧复用.
	TransientCache[Resource.Name] = Resource;
}

void RDGBuilder::planAccess(RDGPass& Pass, RDGResource& Resource, rhi::EResourceState State,
	bool IsWrite)
{
	// 惰性分配 transient 资源(首次被访问时).
	ensureAllocated(Resource);

	if (Resource.LastState != State)
	{
		// 状态变化 → 布局转换 barrier.
		if (Resource.Kind == EResourceKind::Texture)
		{
			rhi::ImageBarrier Barrier {
				.Image = Resource.Image,
				.Before = Resource.LastState,
				.After = State,
				.Range = {
					.Aspect = rhi::EImageAspect::Auto,
					.BaseMipLevel = 0,
					.MipLevelCount = Resource.TextureDesc.MipLevels,
					.BaseArrayLayer = 0,
					.ArrayLayerCount = Resource.TextureDesc.ArrayLayers,
				},
			};
			Pass.ImageBarriersBefore.push_back(Barrier);
		}
		else
		{
			rhi::BufferBarrier Barrier {
				.Buffer = Resource.Buffer,
				.Before = Resource.LastState,
				.After = State,
			};
			Pass.BufferBarriersBefore.push_back(Barrier);
		}
		Resource.LastState = State;
		Resource.LastAccessWasWrite = IsWrite;
	}
	else if (Resource.LastAccessWasWrite || IsWrite)
	{
		// 同状态下的 RAW/WAR/WAW → 仅内存可见性 GlobalBarrier(不改变布局).
		if (Resource.LastState != rhi::EResourceState::Undefined)
		{
			Pass.GlobalBarriersBefore.push_back({
				.Before = State,
				.After = State,
			});
		}
		Resource.LastAccessWasWrite = IsWrite;
	}
}

} // namespace runtime::renderer
