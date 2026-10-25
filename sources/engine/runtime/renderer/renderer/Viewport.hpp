#pragma once

#include <renderer/RenderPass.hpp>
#include <RHI.hpp>

#include <string>

/**
 * ============================================================================
 *  runtime::renderer - Viewport / RenderTarget(视口与渲染目标)
 * ============================================================================
 *
 *  [要解决的问题]
 *  编辑器把一个 3D 视口面板交给游戏, 游戏把场景渲染进该视口; 而"渲染进视口"
 *  的本质是: 游戏把场景渲染到一张离屏渲染目标(RT)上, 由 UI 的 3DView 控件采样
 *  这张 RT 合成到窗口. 因此视口 = 相机 + 渲染目标描述 + 矩形, 而不是"直接往控件上画".
 *
 *  [与引擎/游戏/编辑器的关系]
 *  这些是引擎提供的抽象:
 *    - 编辑器: 创建一个 Viewport(指定相机 + 目标 + 矩形), 交给引擎;
 *    - 游戏: 用引擎的 setRenderViewport 把自己的场景渲染进该 Viewport;
 *    - 引擎: 负责 RT 的创建/复用/生命周期, 以及 UI 采样合成.
 */

namespace runtime::renderer
{

/**
 * @brief 离屏渲染目标描述(RTT 池的键).
 *
 * 池按 (Format, Width, Height, Samples, Usage) 复用显存, 与 RDG 的瞬态资源池互补:
 * RDG 管"帧内瞬态", 这里管"跨帧持久、需被 UI 采样"的渲染目标.
 */
struct RenderTargetDesc
{
    rhi::EFormat Format { rhi::EFormat::BGRA8_UNorm };
    uint32_t Width { 0 };
    uint32_t Height { 0 };
    rhi::ESampleCount Samples { rhi::ESampleCount::Count1 };
    /** @brief 既是渲染目标(Target)又作为 UI 采样源(Sampled). */
    rhi::EImageUsage Usage { rhi::EImageUsage_t::Target | rhi::EImageUsage_t::Sampled };
};

/**
 * @brief 视口: 一个可被渲染的矩形区域 + 其相机.
 *
 * @note 相机矩阵是"该视口的观察视角", 由编辑器(或 standalone 主视口)指定;
 *       游戏场景通过它渲染到 Target 指定的离屏 RT, 再由 UI 3DView 控件采样合成.
 */
struct Viewport
{
    std::string Name;
    RenderView Camera;
    RenderTargetDesc Target;
    uint32_t X { 0 };
    uint32_t Y { 0 };
    uint32_t Width { 0 };
    uint32_t Height { 0 };
};

} // namespace runtime::renderer
