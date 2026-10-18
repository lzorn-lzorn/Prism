#pragma once

#include "IUIRenderer.hpp"
#include "FontHandle.hpp"
#include "TextLayout.hpp"
#include "TextConfig.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ui
{
class TextLayoutEngine;

/**
 * @brief 独立于控件的单线程 FreeType/HarfBuzz 文本服务. 
 * 原始引用绑定的渲染器必须存活至 shutdown; 共享绑定由服务持有. 
 * 初始化, 布局和关闭必须在渲染器没有活动帧时
 * 进行; 图集上传会立即执行. 
 * 即使渲染器抛出异常, noexcept 析构函数也会尝试销毁所有图集. 
 * 若渲染器发生此类故障, 无法保证资源被回收. 
 * 布局拥有自身数据, 但其图集纹理会随服务关闭而失效. 
 */
class FontService
{
public:
    static FontService& self();
    void initialize(IUIRenderer& Renderer);
    void initialize(std::shared_ptr<IUIRenderer> Renderer);
    bool isInitialized() const noexcept;
    void shutdown() noexcept;
    FontService(const FontService&) = delete;
    FontService& operator=(const FontService&) = delete;
    FontService(FontService&&) = delete;
    FontService& operator=(FontService&&) = delete;

    // @note 加载 Unicode 字体的第零个字体面; 相同路径共享句柄, 未初始化时抛出 logic_error. 
    FontHandle loadFont(const std::string& Path);
    // @note 为主字体注册回退链; 句柄必须属于本服务, 不得包含主字体自身. 
    void setFallbackFonts(FontHandle Font, std::vector<FontHandle> Fallbacks);
    /**
     * @brief 为有效的 UTF-32 文本创建布局, 所有簇和光标偏移均为码点索引. 文本长度不得超过 int 的最大值; 不接受编码字节或代理标量. 
     * 换行符和 CRLF 会创建新行. 自动换行保留空白, 并在空格, 制表符和 Unicode 空白的簇安全边界处换行. 过长的单词允许溢出, 不拆分簇. 
     * 每个段落作为一个推断方向的文本段处理, 并非完整的双向段落实现. 
     * 灰度字形采用白色 RGBA8 覆盖率, 彩色 BGRA 字形转为直通 RGBA8; 
     * 固定尺寸字形按最近尺寸读取并缩放, 缓存在 1024 x 1024 图集页中. 
     * 度量基于字形推进距离, 不包含斜体或变音符号的墨迹外伸部分. 
     * 未初始化时抛出 logic_error; 无效输入或失效句柄会抛出 invalid_argument; 字体加载, 塑形, 
     * 光栅化和渲染器故障会抛出异常. 
     * @param Font 本服务加载的字体句柄. 
     * @param UTF32 待布局的有效 UTF-32 文本, 存储必须在调用期间有效. 
     * @param Size 逻辑单位的字体大小. 范围为 (0, 256];
     * @param PixelScale 从逻辑单位到像素的缩放系数. 必须有限且为正
     * @note Size * PixelScale 不得超过 512 像素. 光栅化尺寸按 1/64 像素取整, 最小为 1/64 像素
     * @param MaxWidth 自动换行的最大逻辑宽度, 必须有限且非负, 零表示不自动换行. 
     * @return 拥有文本, 字形, 度量和光标数据的布局; 图集纹理依赖本服务存活. 
     */
    TextLayout createTextLayout(FontHandle Font, std::u32string_view UTF32, float Size,
                                float PixelScale = 1.0f, float MaxWidth = 0.0f);

    // @note 全局默认配置/字体/回退字体; App 在 initialize 时写入, TextWidget 的钩子默认读取.
    void setDefaultTextConfig(const TextConfig& Value);
    const TextConfig& getDefaultTextConfig() const noexcept;
    void setDefaultFont(FontHandle Value) noexcept;
    FontHandle getDefaultFont() const noexcept;
    void setDefaultFallbackFonts(std::vector<FontHandle> Value);
    const std::vector<FontHandle>& getDefaultFallbackFonts() const noexcept;

    // @note 创建文字排版引擎, 用全局默认配置/字体/回退初始化; 供 TextWidget 等文本控件使用.
    std::unique_ptr<TextLayoutEngine> createTextLayoutEngine();

private:
    FontService();
    ~FontService() noexcept;
    void checkInitialized() const;
    struct Impl;
    std::shared_ptr<IUIRenderer> RendererOwner;
    std::unique_ptr<Impl> State;
    TextConfig DefaultTextConfig;
    FontHandle DefaultFont{};
    std::vector<FontHandle> DefaultFallbacks;
};
}