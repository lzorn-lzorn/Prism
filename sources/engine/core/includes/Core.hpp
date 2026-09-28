#pragma once

// ---------- Common: 平台抽象 / 调试 / 异常 / 全局句柄与基础类型 ----------
#include <core/common/AtomicShared.hpp>
#include <core/common/Common.hpp>
#include <core/common/Debug.hpp>
#include <core/common/ExceptionHandler.hpp>
#include <core/common/Global.hpp>
#include <core/common/MarcoArgsNum.hpp>
#include <core/common/Platform.hpp>
#include <core/common/SMFControl.hpp>
#include <core/common/STLInterface.hpp>

// ---------- Containers: 容器 ----------
#include <core/containers/Buffer.hpp>
#include <core/containers/MPMCQueue.hpp>
#include <core/containers/String.hpp>

// ---------- Functions: 句柄管理 ----------
#include <core/functions/HandleManager.hpp>

// ---------- Math: 数学库 ----------
#include <core/math/Color.hpp>
#include <core/math/Geometry.hpp>
#include <core/math/MathCommon.hpp>
#include <core/math/Matrix.hpp>
#include <core/math/Quaternion.hpp>
#include <core/math/Vector.hpp>

// ---------- Wrappers: 委托 / 标志位 / 观察者 ----------
#include <core/wrappers/Delegate.hpp>
#include <core/wrappers/Flag.hpp>
#include <core/wrappers/Observer.hpp>

// ---------- Memory: 内存组件(占位, 待实现) ----------
// 当前 memory/ 下仅有空的 todo.hpp, 尚未提供任何公开接口.
// 待内存组件实现后, 在此补充其公开头文件, 例如:
//   #include <core/memory/Allocator.hpp>
