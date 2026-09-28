#pragma once

#include <atomic>
#include <thread>
#include <vector>
#include <memory>

#include <renderer/RenderThread.hpp>
#include <RTCommon.hpp>

namespace runtime
{

class Engine final
{
public:
	static Engine& self() 
	{
		static Engine instance;
		return instance;
	}
	~Engine() = default;
	Engine(const Engine&) = delete;
	Engine& operator=(const Engine&) = delete;
	Engine(Engine&&) = delete;
	Engine& operator=(Engine&&) = delete;
	/**
	 * @brief 负责初始化基础设置和与平台相关的设置
	 * 	- 日志
	 *  - 线程池
	 *	- UI: 获取底层窗口, 交换链, 翻译底层事件循环
	 * 	- RHI:
	 */
	void preinitialize();

	/**
	 * @brief 正式初始化引擎各个子系统
	 *  - Network:
	 *  - Video, Audio:
	 *  - Physics
	 *  - Viewport/Editor:
	 */
	void initialize();
	
	/**
	 * @brief 本质上就是引擎执行的第一帧
	 */
	void postinitialize();
	void run();
	void render();
	void quit();
	bool shouldQuit() const { return ShouldQuit; }
	void destroy();
private:
	Engine() = default;
	void tick(float DeltaTime);

private:
	std::atomic<bool> ShouldQuit { false };
	bool IsFirstFrame { true };

	std::unique_ptr<runtime::RenderThread> RenderThread;
	
};
}