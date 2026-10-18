这个委托库提供了四类委托，适合不同场景：

- `Delegate`：可复制, 拥有 callable，适合策略, 回调存储. 
- `UniqueDelegate`：仅移动, 拥有 callable，适合捕获 `unique_ptr`, `promise` 等 move-only 对象的任务系统. 
- `DelegateRef`：非拥有, 引用外部 callable，适合生命周期由外部保证的场合（注意当前实现中构造函数是 `private`，若直接使用需要调整访问控制）. 
- `MulticastDelegate`：多播委托，支持延迟增删, 异常隔离, 生命周期绑定, 结果收集. 

下面给出几个相对复杂的案例，覆盖多播广播, 异常处理, 生命周期管理, 结果聚合和 move-only 任务. 

---

## 1. 事件总线：广播期间的延迟增删

`MulticastDelegate` 的广播语义是：

- 广播开始时，当前订阅列表的快照被固定；
- 广播期间 **新添加** 的回调不会在本轮执行；
- 广播期间 **被移除** 的尚未执行的回调，本轮不会执行；
- 回调对象真正的析构发生在广播完全结束后，因此回调可以在执行过程中安全地移除自己. 

### 示例代码

```cpp
#include <iostream>
#include <string>

struct Event {
    int id;
    std::string message;
};

class EventBus {
public:
    using Delegate = core::MulticastDelegate<void(const Event&)>;

    core::DelegateHandle subscribe(Delegate::callback_type cb) {
        return events_.add(std::move(cb));
    }

    // 便捷版：直接接受任意 callable
    template <typename F>
    core::DelegateHandle subscribe(F&& cb) {
        return events_.addCallable(std::forward<F>(cb));
    }

    bool unsubscribe(core::DelegateHandle h) {
        return events_.remove(h);
    }

    void publish(const Event& e) {
        events_.broadcast(e);
    }

    void publishGuarded(auto&& errorHandler, const Event& e) {
        events_.broadcastGuarded(
            std::forward<decltype(errorHandler)>(errorHandler),
            e
        );
    }

private:
    Delegate events_;
};

int main() {
    EventBus bus;
    core::DelegateHandle h1, h2, h3;

    // 第一个回调：如果收到 id == 1 的事件，移除尚未执行的 h2
    h1 = bus.subscribe([&](const Event& e) {
        std::cout << "[h1] receive: " << e.id << "\n";
        if (e.id == 1) {
            std::cout << "[h1] remove h2\n";
            bus.unsubscribe(h2);
        }
    });

    // 第二个回调
    h2 = bus.subscribe([&](const Event& e) {
        std::cout << "[h2] receive: " << e.id << "\n";
    });

    // 第三个回调：收到 id == 2 时，添加一个新回调
    h3 = bus.subscribe([&](const Event& e) {
        std::cout << "[h3] receive: " << e.id << "\n";
        if (e.id == 2) {
            std::cout << "[h3] add late listener\n";
            bus.subscribe([&](const Event& e) {
                std::cout << "[late] receive: " << e.id << "\n";
            });
        }
    });

    std::cout << "=== broadcast id=1 ===\n";
    bus.publish({1, "hello"});
    // 输出：
    // [h1] receive: 1
    // [h1] remove h2
    // [h3] receive: 1
    // h2 被移除，本轮不会执行

    std::cout << "=== broadcast id=2 ===\n";
    bus.publish({2, "world"});
    // h2 已失效不会执行
    // [h1] receive: 2
    // [h3] receive: 2
    // [h3] add late listener
    // late 不会在本轮执行

    std::cout << "=== broadcast id=3 ===\n";
    bus.publish({3, "again"});
    // 现在 late listener 才会执行
    // [h1] receive: 3
    // [h3] receive: 3
    // [late] receive: 3
}
```

**要点**：  
`h2` 在 `h1` 中被移除，因为 `h2` 尚未执行，所以本轮不会触发；新添加的 `late` 在下一轮才执行. 

---

## 2. 异常隔离：`broadcastGuarded`

对于非 `noexcept` 的多播委托，`broadcast` 会在第一个异常处中断. 若希望每个回调的异常都被捕获并继续广播，可以使用 `broadcastGuarded`. 它的错误处理器签名必须为：

```cpp
void(DelegateHandle, std::exception_ptr)
```

### 示例代码

```cpp
#include <exception>
#include <iostream>
#include <stdexcept>

struct ErrorReporter {
    void operator()(core::DelegateHandle h, std::exception_ptr ep) const {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& ex) {
            std::cerr << "Listener slot=" << h.SlotIndex
                      << " generation=" << h.Generation
                      << " failed: " << ex.what() << "\n";
        }
    }
};

int main() {
    EventBus bus;

    bus.subscribe([&](const Event& e) {
        std::cout << "[normal] id=" << e.id << "\n";
    });

    bus.subscribe([&](const Event& e) {
        std::cout << "[throwing] id=" << e.id << "\n";
        throw std::runtime_error("boom");
    });

    bus.subscribe([&](const Event& e) {
        std::cout << "[after] id=" << e.id << "\n";
    });

    Event e{1, "test"};
    bus.publishGuarded(ErrorReporter{}, e);

    // 输出：
    // [normal] id=1
    // [throwing] id=1
    // Listener slot=1 ... failed: boom
    // [after] id=1
}
```

**注意**：`broadcastGuarded` 只在非 `noexcept` 签名下可用. 若签名是 `void(const Event&) noexcept`，回调不能抛出异常，也不会有该接口. 

---

## 3. 生命周期绑定：`addShared` / `addWeak` / `addWeakOr`

多播委托支持直接绑定成员函数，同时管理订阅者生命周期：

- `addShared`：委托持有 `shared_ptr`，保证订阅者存活；
- `addWeak`：仅持有 `weak_ptr`，当订阅者已销毁时静默跳过；
- `addWeakOr`：当订阅者已销毁时调用 fallback（仅适用于非 `void` 返回类型）. 

### 示例代码

```cpp
#include <memory>
#include <iostream>
#include <vector>

class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(int id) : id_(id) {}

    void onEvent(const Event& e) {
        std::cout << "[Session " << id_ << "] receive event " << e.id << "\n";
    }

    int queryValue(const Event& e) const {
        return id_ * 100 + e.id;
    }

private:
    int id_;
};

int main() {
    using EventDelegate = core::MulticastDelegate<void(const Event&)>;
    using QueryDelegate = core::MulticastDelegate<int(const Event&)>;

    EventDelegate events;
    QueryDelegate queries;

    auto session = std::make_shared<Session>(42);
    std::weak_ptr<Session> weakSession = session;

    // 持有 shared_ptr：即使外部 session 被 reset，订阅仍然有效
    auto hShared = events.addShared<&Session::onEvent>(session);

    // 只持有 weak_ptr：session 销毁后自动跳过
    auto hWeak = events.addWeak<&Session::onEvent>(weakSession);

    // 非 void 返回，带 fallback
    auto hQuery = queries.addWeakOr<&Session::queryValue>(
        weakSession,
        [](const Event& e) { return -1; }
    );

    Event e{10, "msg"};

    // 此时 session 仍然存活，两个回调都会执行
    events.broadcast(e);
    auto results = queries.collect(e);
    // results 包含 42*100 + 10 = 4210

    std::cout << "Query result: " << results[0] << "\n";

    // 销毁 session
    session.reset();

    std::cout << "=== after session reset ===\n";
    events.broadcast(e);
    // hShared 仍然持有 session，所以 onEvent 仍会执行
    // hWeak 已失效，会被跳过

    results = queries.collect(e);
    // weak 失效，fallback 被调用，返回 -1
    std::cout << "Query result after reset: " << results[0] << "\n";
}
```

**要点**：  
- `addShared` 会延长订阅者生命周期，可能导致对象在事件系统中存活过久；  
- `addWeak` 更适合观测者模式，避免悬挂；  
- `addWeakOr` 为需要返回值的场景提供了降级处理. 

---

## 4. 返回值聚合与短路：`visitResults` / `collect`

对于非 `void` 返回类型的多播委托，不能直接 `broadcast`，而应使用：

- `visitResults(visitor, args...)`：逐个访问结果，`Visitor` 返回 `bool` 时可实现短路；
- `collect(args...)`：将所有结果收集到 `std::vector<RetType>`. 

### 示例：校验链（短路）

```cpp
#include <iostream>
#include <vector>

struct Request {
    int value;
};

int main() {
    core::MulticastDelegate<bool(const Request&)> validators;

    validators.addCallable([](const Request& r) {
        std::cout << "validator 1: positive\n";
        return r.value > 0;
    });

    validators.addCallable([](const Request& r) {
        std::cout << "validator 2: less than 100\n";
        return r.value < 100;
    });

    validators.addCallable([](const Request& r) {
        std::cout << "validator 3: even\n";
        return r.value % 2 == 0;
    });

    Request req{50};

    bool allValid = true;
    validators.visitResults([&](bool ok) {
        if (!ok) {
            allValid = false;
            return false;   // 停止后续校验器
        }
        return true;        // 继续下一个校验器
    }, req);

    std::cout << "All valid: " << std::boolalpha << allValid << "\n";
}
```

### 示例：收集所有得分

```cpp
core::MulticastDelegate<int(const Request&)> scorers;

scorers.addCallable([](const Request& r) { return r.value * 2; });
scorers.addCallable([](const Request& r) { return r.value + 10; });
scorers.addCallable([](const Request& r) { return r.value - 5; });

Request req{7};
auto scores = scorers.collect(req);

for (int s : scores) {
    std::cout << "score: " << s << "\n";
}
```

**注意**：`collect` 要求返回类型不是引用且可移动构造，并且参数必须满足多路复用条件（引用或可复制构造）. 

---

## 5. Move-only 任务队列：`UniqueDelegate`

`UniqueDelegate` 可以持有 move-only callable，例如捕获了 `unique_ptr` 的 lambda. 这非常适合任务队列, 异步回调等场景. 

### 示例代码

```cpp
#include <iostream>
#include <memory>
#include <vector>

class TaskQueue {
public:
    using Task = core::UniqueDelegate<void()>;

    void push(Task t) {
        tasks_.push_back(std::move(t));
    }

    template <typename F>
    void emplace(F&& f) {
        tasks_.emplace_back(std::forward<F>(f));
    }

    void runAll() {
        for (auto& t : tasks_) {
            t();
        }
        tasks_.clear();
    }

private:
    std::vector<Task> tasks_;
};

int main() {
    TaskQueue queue;

    queue.emplace([] {
        std::cout << "ordinary task\n";
    });

    // 捕获 unique_ptr，只能移动
    auto ptr = std::make_unique<int>(42);
    queue.emplace([p = std::move(ptr)] {
        std::cout << "move-only task, value = " << *p << "\n";
    });

    queue.runAll();
}
```

**要点**：`UniqueDelegate` 不可复制，但可以移动，因此可以安全存入 `std::vector` 并转移所有权. 它的内部实现保证了 move-only lambda 的构造和销毁. 

---

## 6. 可复制 `Delegate` 作为策略对象

`Delegate` 具备复制语义，适合作为可复制的策略, 回调配置. 小 callable 使用内联存储，大对象自动堆分配. 

### 示例代码

```cpp
#include <iostream>
#include <functional>

class Calculator {
public:
    using Strategy = core::Delegate<int(int, int)>;

    void setStrategy(Strategy s) {
        strategy_ = std::move(s);
    }

    int compute(int a, int b) const {
        return strategy_(a, b);
    }

private:
    Strategy strategy_;
};

int main() {
    Calculator calc;

    calc.setStrategy([](int a, int b) { return a + b; });

    auto saved = calc;   // 复制整个 Calculator，strategy 也被复制

    calc.setStrategy([](int a, int b) { return a * b; });

    std::cout << "calc: 3 + 4 = " << calc.compute(3, 4) << "\n";   // 12
    std::cout << "saved: 3 + 4 = " << saved.compute(3, 4) << "\n"; // 7
}
```

**要点**：复制 `Delegate` 会深拷贝绑定的 callable（如果 callable 可复制）. 这在需要保存不同配置或历史版本时非常有用. 

---

## 总结注意事项

1. **生命周期**：  
   - `DelegateRef` 不拥有目标，必须保证目标在调用期间存活；  
   - 多播委托中的 `addShared` 会延长生命周期，`addWeak` 则不会. 

2. **广播期间的增删**：  
   - 新添加的回调不会在本轮执行；  
   - 被移除但尚未执行的回调不会执行；  
   - 回调对象的真正销毁在广播结束后. 

3. **异常与 `noexcept`**：  
   - 非 `noexcept` 多播委托可以用 `broadcastGuarded` 隔离异常；  
   - `noexcept` 签名下不允许抛出，也不会生成该接口. 

4. **参数多路复用**：  
   - 多播广播要求参数能被多次传递，因此参数必须是引用或可复制构造；  
   - 右值引用或 move-only 参数无法直接用于多播. 

5. **成员函数绑定**：  
   目前 `addRaw` / `addShared` / `addWeak` 等的实现要求绑定的成员函数为非 `const`（因为内部使用 `ClassType*` 进行可调用性检查）. 如果需要绑定 `const` 成员函数，可以先用 lambda 包装：

   ```cpp
   auto sp = std::make_shared<Session>(1);
   events.addShared([sp](const Event& e) {
       sp->onEvent(e);   // 如果 onEvent 是 const 成员
   });
   ```

以上案例覆盖了该委托库最复杂, 最实用的几个场景，你可以根据实际需求组合使用. 