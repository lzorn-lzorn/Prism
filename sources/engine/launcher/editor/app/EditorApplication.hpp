#pragma once

#include "engine/IEditor.hpp"
#include <memory>

namespace ui { class GenericApplication; }
namespace runtime::renderer { class RendererServer; }

namespace editor
{

/**
 * @brief 编辑器宿主: 构建编辑器界面 + 演示场景, 并由 Engine 驱动其每帧渲染.
 *
 * 它实现 runtime::IEditor 契约, 通过引用访问 UI 应用(GenericApplication)与
 * 渲染服务(RendererServer), 自身不拥有它们 —— 二者分别由 UI 子系统与
 * Renderer 子系统管理。编辑器把"界面/场景/每帧渲染"封装为一个可被 Engine
 * 托管的执行实体, 使组合根(Main)保持瘦身。
 */
class EditorApplication final : public runtime::IEditor
{
public:
    EditorApplication(ui::GenericApplication& App, runtime::renderer::RendererServer& Renderer);
    ~EditorApplication() override;

    void initialize() override;
    void tick(float DeltaTime) override;
    void destroy() override;

private:
    struct Impl;
    std::unique_ptr<Impl> ImplPtr;
};

} // namespace editor
