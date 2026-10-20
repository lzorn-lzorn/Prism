#pragma once 

class ModuleLoader
{
public:
    static ModuleLoader& self() 
    {
        static ModuleLoader instance;
        return instance;
    }

    ~ModuleLoader() = default;
    ModuleLoader(const ModuleLoader&) = delete;
    ModuleLoader& operator=(const ModuleLoader&) = delete;
    ModuleLoader(ModuleLoader&&) = delete;
    ModuleLoader& operator=(ModuleLoader&&) = delete;
private:
    ModuleLoader() = default;

};