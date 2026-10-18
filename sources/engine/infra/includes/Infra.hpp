#pragma once

// ---------- Common: 平台抽象 / 调试 / 异常 / 全局句柄与基础类型 ----------
#include "infra/common/AtomicShared.hpp"
#include "infra/common/Common.hpp"
#include "infra/common/Debug.hpp"
#include "infra/common/Assert.hpp"
#include "infra/common/Error.hpp"
#include "infra/common/Global.hpp"
#include "infra/common/MarcoArgsNum.hpp"
#include "infra/common/Platform.hpp"
#include "infra/common/SMFControl.hpp"
#include "infra/common/STLInterface.hpp"

// ---------- Containers: 容器 ----------
#include "infra/containers/Buffer.hpp"
#include "infra/containers/MPMCQueue.hpp"
#include "infra/containers/String.hpp"

// ---------- Functions: 句柄管理 ----------
#include "infra/functions/HandleManager.hpp"
#include "infra/functions/Tag.hpp"

// ---------- Math: 数学库 ----------
#include "infra/math/Color.hpp"
#include "infra/math/Geometry.hpp"
#include "infra/math/MathCommon.hpp"
#include "infra/math/Matrix.hpp"
#include "infra/math/Quaternion.hpp"
#include "infra/math/Vector.hpp"

// ---------- Wrappers: 委托 / 标志位 / 观察者 ----------
#include "infra/wrappers/Delegate.hpp"
#include "infra/wrappers/Flag.hpp"
#include "infra/wrappers/Observer.hpp"

#include "infra/reflect/Reflect.hpp"
#include "infra/serde/Serde.hpp"

#include "infra/memory/Memory.hpp"
