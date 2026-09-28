
# 命名规范
函数命名, 变量命名, 文件命名要有描述性, 少用缩写. 对于特殊前缀应该提前约定
## 变量命名
局部变量包括局部类型: 下划线
函数形参: 大驼峰, 必要时可以使用 In- Out- 前缀和成员变量进行区分
成员变量: 大驼峰
类型名: 大驼峰

对于全局常量或者枚举值, 应该使用大驼峰的方式命名, 例如 
```Cpp
constexpr int32_t MaxSupportedNumber = 100; 

enum class State {
    Start,
    End,
    Hold
};
```

大驼峰的含义是一个名词, 表示一个具体实体. 而变量, 类型均是一个实体. 对于局部变量使用下划
线是因为要将局部变量和函数形参做区分. 同样地, 在类内使用的 `using value_type = xxx` 也
是为了和由外部指定的 `template<typename ValueType>` 进行区分, 顺便和标准库保持一致.
如果在全局或者整个命名空间均定义了类型例如 `using WindowId_t = uint64_t;`, 使用 `_t`
后缀来防止和变量名重名

```Cpp
template <typename Ty>
struct TypeA {
    using pointer = Ty*;
    using const_pointer = const Ty*;
    using value_type = Ty;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
};
```

类名在大驼峰的基础上, 通过首字母来表征该类的语义, 但是对于大部分实体类型或者对象不需要加
任何前缀

|前缀|含义|
| I- | 接口类 |
| T- | 模板类, 该模版并非语法上的模版, 而是通过模版实现通用功能的类 |
| G- | 全局对象 |
| E- | 枚举 |

但是对于 concept 约束使用下划线, 这是为了对齐C++的格式, 因为一个 concept 常见的一个用法
```Cpp
template <your_concept Ty>
```
相当于对齐原本的 `typename`

使用 namespace 来进行名称的区分往往是更具有可读性的.

## 函数命名
函数普遍以小驼峰命名, 开头词为动词; 对于名词开头的函数使用大驼峰, 且返回值只能表征执行过
程成功与否而不能直接返回结果

小驼峰实际上表示的是一个行为或者过程

## 命名空间
命名空间 (namespace) 应该使用 **一个小写单词** 表示. 虽然你不需要吝啬命名空间的使用, 但
也应该确实其含义以免多级命名空间过长的情况, 大致控制在三级(不用非要用满三级): 

**主名称空间::系统名称空间::内部模块名称空间**

## 头文件
头文件统一使用 .hpp 因为 .h 的后缀有时静态分析器会按照C来解析

在使用 `add_internal_library` 时, 可以参考 `GenericApplication/CMakeLists.txt`
```cmake
add_internal_library(GenericApplication
    TYPE                STATIC
    SOURCES             ${SOURCES_LIST}
    INCLUDES            "${CMAKE_CURRENT_SOURCE_DIR}/includes"
    PRIVATE_INCLUDES    "${CMAKE_CURRENT_SOURCE_DIR}/sources"
    PRIVATE_DEPENDENCY  SDL3::SDL3 Core::Core
    ALIAS               Module::GenericApplication
)
```
同时在 include/ 下创建一个同名的文件夹内部放置你的具体的头文件. 如此做的原因时当外部模块
引用本某模块的头文件时, 需要指定模块名. 例如 Application 模块
```cpp
#include <generic_application/event/Event.hpp>
```
才能成功引入 GenericApplication 下的 `Event.hpp`. 拥有更好的可读性, 同时对于模块内部, 例如 GenericApplication
内部, 也推荐使用 `""` 而不是 `<>` 来指定路径, 两者功能没有太大区别, 主要是用于区分内部
外部模块

> [include]
> 对于 `#include ""` 和 `#include <>` 的区别, 实际上两者的功能并无太大区别:
> `#include ""` 会先以当前目录为起点开始找, 然后在从 CMake 中 `target_include_directories`
> 中指定的路径开始寻找, 最后从系统目录找.
> `#include <>` 会跳过当前目前直接从 CMake 中指定的 `target_include_directories` 中
> 寻找, 如果强行使用, 例如 `#include <Common.hpp>` 实际上 Common.hpp 在上一级目录, 
> 构建器也能找到, 但是抛出警告.

