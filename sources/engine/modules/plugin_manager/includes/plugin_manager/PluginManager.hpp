#pragma once

namespace modules
{

class PluginManager
{
public:
	static PluginManager& self() 
	{
		static PluginManager instance;
		return instance;
	}

	~PluginManager() = default;
	PluginManager(const PluginManager&) = delete;
	PluginManager& operator=(const PluginManager&) = delete;
	PluginManager(PluginManager&&) = delete;
	PluginManager& operator=(PluginManager&&) = delete;

private:
	PluginManager() = default;
};

}