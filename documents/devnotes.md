
# 命名规范
函数命名, 变量命名, 文件命名要有描述性, 少用缩写. 对于特殊前缀应该提前约定

## 代码命名
### 类型, TypeTrait, Concept 
类型, TypeTrait, Concept 表示类型语义使用大驼峰.
除非与标准库兼容的 `using value_type = xxx;` 使用与标准库兼容的名称
```Cpp
template <typename Ty>
struct TypeA {
    using pointer = Ty*;
    using const_pointer = const Ty*;
    using value_type = Ty;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    using Tarits = Ty::Tarits;
};
```
使用特定的字母开头用于表示该类的特殊作用:
- I- : 接口类
- E- : 枚举

### 函数形参, 局部变量, 成员变量
函数形参, 局部变量, 成员变量表示值语义使用下划线命名.
lambda表达式视为函数, 不适用于此; 全局变量不适用于此

### 全局常量, 全局变量和枚举使用大驼峰
```Cpp
constexpr int32_t MaxSupportedNumber = 100; 
```
对于全局变量使用 G- 开头
```cpp
enum class State {
    Start,
    End,
    Hold
};
```
并且应该使用 `enum class` 而不是 `enum`

### 函数, 成员函数, lambda
函数, 成员函数, lambda 使用小驼峰且动词, 介词, 动词短语开头, 表示动作.

api 完整注释格式:
```
/**
 * @brief API 行为
 * @tparam 类型约束的输入
 *      \tparam1 类型参数1 含义;
 * @param 参数含义和有效输入范围
 *      \param1 参数1 含义; 有效输入范围
 *      \param2 参数2 含义; 有效输入范围
 *      ...
 * @return 返回值含义和有效返回范围
 * @note 注意事项
 */
```
非完整格式(并非每一个 api 都需要严格注释可以有以下简化)
```cpp
// @brief API 行为
```
或者
```cpp
// @note API 行为
```

### 命名空间
命名空间 (namespace) 应该使用 **一个小写单词** 表示. 虽然你不需要吝啬命名空间的使用, 但
也应该确实其含义以免多级命名空间过长的情况, 大致控制在三级(不用非要用满三级): 

**主名称空间::系统名称空间::内部模块名称空间**

## 文件格式
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
才能成功引入 GenericApplication 下的 `Event.hpp`. 拥有更好的可读性, 同时对于模块内部, 
例如 GenericApplication内部, 也推荐使用 `""` 而不是 `<>` 来指定路径, 两者功能没有太大
区别, 主要是用于区分内部外部模块.

同时应该在 include/your_moulde/ 下提供一个或若干头文件(往往一个即可), 作为外部统一获取
所用功能的文件

> [include]
> 对于 `#include ""` 和 `#include <>` 的区别, 实际上两者的功能并无太大区别:
> `#include ""` 会先以当前目录为起点开始找, 然后在从 CMake 中 `target_include_directories`
> 中指定的路径开始寻找, 最后从系统目录找.
> `#include <>` 会跳过当前目前直接从 CMake 中指定的 `target_include_directories` 中
> 寻找, 如果强行使用, 例如 `#include <Common.hpp>` 实际上 Common.hpp 在上一级目录, 
> 构建器也能找到, 但是抛出警告.

## 目录
目录统一使用全小写的下划线来命名, 因为有的文件系统是不区别大小写的, 统一使用小写来防止不必
要的问题