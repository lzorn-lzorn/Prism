#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "infra/containers/Buffer.hpp"
#include "infra/wrappers/Flag.hpp"
#include "infra/common/AtomicShared.hpp"

namespace runtime
{
enum class LogDestination_t : std::uint8_t
{
    None = 0,
    Screen = 1 << 0,
    EditorConsole = 1 << 1,
    ProgramConsole = 1 << 2,
    File = 1 << 3,
    Console = (1 << 1) | (1 << 2),
    Default = (1 << 1) | (1 << 2) | (1 << 3),
    All = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3),
};

using LogDestination = infra::Flags<LogDestination_t>;

[[nodiscard]]
constexpr bool hasDestination(LogDestination Value, LogDestination Test) noexcept
{
    return static_cast<bool>(Value & Test);
}

struct LogLevel
{
    std::uint32_t Code{0};
    std::uint32_t Priority{0};
    std::string_view Name{"Unknown"};

    [[nodiscard]]
    friend constexpr bool operator==(const LogLevel&, const LogLevel&) noexcept = default;
};

namespace levels
{
inline constexpr LogLevel Trace{10, 10, "Trace"};
inline constexpr LogLevel Debug{20, 20, "Debug"};
inline constexpr LogLevel Info{30, 30, "Info"};
inline constexpr LogLevel Warning{40, 40, "Warning"};
inline constexpr LogLevel Error{50, 50, "Error"};
inline constexpr LogLevel Fatal{60, 60, "Fatal"};
}

struct LogCategory
{
    std::string_view Name{"Default"};
    std::string_view Color{"#FFFFFF"};

    [[nodiscard]]
    friend constexpr bool operator==(const LogCategory&, const LogCategory&) noexcept = default;
};

inline constexpr LogCategory DefaultCategory{
    .Name = "Default",
    .Color = "#FFFFFF",
};

#define DECLARE_LOG_CATEGORY(NAME, COLOR) \
    inline constexpr ::runtime::LogCategory NAME{ \
        .Name = #NAME, \
        .Color = COLOR, \
    }

struct LogOptions
{
    LogLevel Level{levels::Info};
    LogCategory Category{DefaultCategory};
    LogDestination Destination{LogDestination_t::Default};
    // false: Time + Level + Text
    // true:  Time + Level + Category + Thread + Text + Source
    bool IsVerbose{false};
};

struct Message
{
    [[nodiscard]]
    static Message make(
        std::uint64_t Id,
        const LogOptions& Options,
        const std::source_location& Location,
        std::string Text);

    Message() = default;
    Message(const Message&) = default;
    Message& operator=(const Message&) = default;
    Message(Message&&) = default;
    Message& operator=(Message&&) = default;

    [[nodiscard]] static std::string serialize(const Message& Record);
    [[nodiscard]] static Message deserialize(const std::string& Str);

    std::uint_least32_t Line{0};
    std::uint_least32_t Column{0};
    std::uint32_t LevelCode{0};
    std::uint32_t LevelPriority{0};
    LogDestination Destination{LogDestination_t::Default};
    bool IsVerbose{false};
    std::uint64_t Id{0};
    std::string FileName;
    std::string FunctionName;
    std::string MessageText;
    std::string CategoryName;
    std::string CategoryColor;
    std::string LevelName;
    std::thread::id ThreadId{};
    std::chrono::system_clock::time_point Timestamp{};

private:
    static std::string escapeField(std::string_view InputChars);
    static std::string unescapeField(std::string_view InputChars);
    static std::vector<std::string> splitEscapedFields(const std::string& Line);
};

class LogTextFormatter final
{
public:
    [[nodiscard]] static std::string format(const Message& InMessage);

private:
    [[nodiscard]] static std::string_view shortFileName(std::string_view Path) noexcept;
};

class Logger
{
public:
    enum class ERunMode : std::uint8_t
    {
        BackgroundThread,
        ManualFramePump,
    };

    static constexpr std::size_t RingBufferCapacity = 8192;
    static constexpr std::size_t DefaultFrameBudgetMessages = 1024;
    static constexpr std::size_t DefaultBackgroundBatchMessages = 256;

    struct Config
    {
        ERunMode Mode{ERunMode::BackgroundThread};
        bool EnableFileOutput{true};
        std::optional<std::filesystem::path> FilePath{};
        std::size_t FlushThresholdBytes{64 * 1024};
        std::size_t BackgroundBatchMessages{DefaultBackgroundBatchMessages};
        std::chrono::milliseconds BackgroundWakeInterval{10};
        LogLevel MinimumLevel{levels::Trace};
    };

    using sink_id_t = std::uint64_t;
    using sink_callback_t = std::function<void(const Message&, std::string_view)>;
    using screen_sink_t = sink_callback_t;
    using editor_console_sink_t = sink_callback_t;

    static const Config DefaultConfig;

    [[nodiscard]] static Logger& self();

    [[nodiscard]] bool start(Config InConfig = DefaultConfig);
    [[nodiscard]] bool startManually(Config InConfig = DefaultConfig);
    void stop() noexcept;

    // ManualFramePump 模式下由引擎事件循环每帧调用. 
    [[nodiscard]] bool pumpFrame(
        std::size_t MaxMessages = DefaultFrameBudgetMessages,
        std::optional<std::chrono::nanoseconds> TimeBudget = std::nullopt);

    // 消费当前队列并刷新日志文件. 
    void flush();

    void setScreenSink(screen_sink_t Sink);
    void clearScreenSink();
    void setEditorConsoleSink(editor_console_sink_t Sink);
    void clearEditorConsoleSink();

    sink_id_t registerSink(sink_callback_t Sink,
        LogDestination InDestination = LogDestination_t::All);
    [[nodiscard]] bool unregisterSink(sink_id_t Id);
    void clearSinks();

    void setMinimumLevel(LogLevel Level) noexcept;

    template <class... ArgTypes>
    static void logAt(
        const std::source_location& Location,
        std::format_string<ArgTypes...> Format,
        ArgTypes&&... Args) noexcept
    {
        self().logImpl(
            Location,
            LogOptions{},
            Format,
            std::forward<ArgTypes>(Args)...);
    }

    template <class... ArgTypes>
    static void logAt(
        const std::source_location& Location,
        const LogOptions& Options,
        std::format_string<ArgTypes...> Format,
        ArgTypes&&... Args) noexcept
    {
        self().logImpl(
            Location,
            Options,
            Format,
            std::forward<ArgTypes>(Args)...);
    }

    template <class... ArgTypes>
    static void logAt(
        const std::source_location& Location,
        LogLevel Level,
        LogCategory Category,
        LogDestination Destination,
        bool IsVerbose,
        std::format_string<ArgTypes...> Format,
        ArgTypes&&... Args) noexcept
    {
        logAt(
            Location,
            LogOptions{
                .Level = Level,
                .Category = Category,
                .Destination = Destination,
                .IsVerbose = IsVerbose,
            },
            Format,
            std::forward<ArgTypes>(Args)...);
    }

    template <class... ArgTypes>
    void logImpl(
        const std::source_location& Location,
        const LogOptions& Options,
        std::format_string<ArgTypes...> Format,
        ArgTypes&&... Args) noexcept
    {
        if (Options.Destination == LogDestination_t::None ||
            Options.Level.Priority < MinimumPriority.load(std::memory_order_relaxed) ||
            !IsRunning.load(std::memory_order_acquire))
        {
            return;
        }

        ProducerGuard guard(*this);
        if (!IsRunning.load(std::memory_order_acquire))
        {
            return;
        }

        try
        {
            Message message = Message::make(
                NextMessageId.fetch_add(1, std::memory_order_relaxed),
                Options,
                Location,
                std::format(Format, std::forward<ArgTypes>(Args)...));

            if (!MessageQueue.tryPush(std::move(message)))
            {
                DroppedMessages.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            QueueCondition.notify_one();
        }
        catch (...)
        {
            FormattingErrors.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] ERunMode getRunMode() const noexcept;
    [[nodiscard]] std::filesystem::path getLogFilePath() const;
    [[nodiscard]] std::uint64_t getDroppedMessageCount() const noexcept;
    [[nodiscard]] std::uint64_t getFormattingErrorCount() const noexcept;
    [[nodiscard]] std::uint64_t getSinkErrorCount() const noexcept;
    [[nodiscard]] std::uint64_t getFileErrorCount() const noexcept;
    [[nodiscard]] bool isRunning() const noexcept;

    // debug::print/println 的无队列, 线程安全输出后端. 
    static void writeDebugText(std::string_view Text, bool AppendNewLine) noexcept;

private:
    struct SinkEntry
    {
        sink_id_t Id{0};
        LogDestination Destination{LogDestination_t::All};
        sink_callback_t Callback;
    };

    using sink_list_t = std::vector<SinkEntry>;

    class ProducerGuard final
    {
    public:
        explicit ProducerGuard(Logger& InOwner) noexcept
            : Owner(InOwner)
        {
            Owner.ActiveProducers.fetch_add(1, std::memory_order_acq_rel);
        }

        ~ProducerGuard() noexcept
        {
            if (Owner.ActiveProducers.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                Owner.ActiveProducers.notify_all();
            }
        }

        ProducerGuard(const ProducerGuard&) = delete;
        ProducerGuard& operator=(const ProducerGuard&) = delete;

    private:
        Logger& Owner;
    };

    Logger();
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    [[nodiscard]] bool startImpl(Config InConfig);
    void stopLocked() noexcept;
    void waitForActiveProducers() noexcept;
    void backgroundLoop(std::stop_token StopToken) noexcept;
    [[nodiscard]] bool drainBatch(
        std::size_t MaxMessages,
        std::optional<std::chrono::nanoseconds> TimeBudget);
    bool consumeOne();

    void dispatchMessage(const Message& InMessage) noexcept;
    void dispatchScreen(const Message& InMessage, std::string_view Formatted) noexcept;
    void dispatchEditorConsole(const Message& InMessage, std::string_view Formatted) noexcept;
    void dispatchCustomSinks(const Message& InMessage, std::string_view Formatted) noexcept;
    void writeProgramConsole(const Message& InMessage, std::string_view Formatted) noexcept;

    void openLogFile() noexcept;
    void writeFile(const Message& InMessage, std::string_view Formatted) noexcept;
    void flushSerializedBuffer() noexcept;
    void closeLogFile() noexcept;

    infra::RingBuffer<Message, std::allocator<Message>, infra::ERingBufferPolicy::MPSC> MessageQueue;

    infra::AtomicShared<const sink_list_t> SinksSnapshot;
    infra::AtomicShared<const screen_sink_t> ScreenSinkSnapshot;
    infra::AtomicShared<const editor_console_sink_t> EditorConsoleSinkSnapshot;
// #ifdef __cpp_lib_atomic_shared_ptr
// 	std::atomic<std::shared_ptr<const sink_list_t>> SinksSnapshot;
// 	std::atomic<std::shared_ptr<const screen_sink_t>> ScreenSinkSnapshot;
// 	std::atomic<std::shared_ptr<const editor_console_sink_t>> EditorConsoleSinkSnapshot;
// 
// #else
// 	std::shared_ptr<const sink_list_t> SinksSnapshot;
// 	std::shared_ptr<const screen_sink_t> ScreenSinkSnapshot;
// 	std::shared_ptr<const editor_console_sink_t> EditorConsoleSinkSnapshot;
// #endif
    std::jthread WorkerThread;
    mutable std::mutex LifecycleMutex;
    std::mutex ProgramConsoleMutex;
    std::mutex BuiltinSinkUpdateMutex;
    std::mutex ConsumerMutex;
    std::mutex SinkUpdateMutex;
    std::mutex QueueWaitMutex;
    std::condition_variable QueueCondition;

    std::ofstream LogFile;
    std::filesystem::path LogFilePath;
    std::string SerializedBuffer;
    Config CurrentConfig;

    std::atomic<ERunMode> CurrentMode{ERunMode::BackgroundThread};
    std::atomic<bool> IsRunning{false};
    std::atomic<std::uint32_t> MinimumPriority{levels::Trace.Priority};
    std::atomic<std::uint64_t> NextMessageId{1};
    std::atomic<std::uint64_t> NextSinkId{1};
    std::atomic<std::uint64_t> ActiveProducers{0};
    std::atomic<std::uint64_t> DroppedMessages{0};
    std::atomic<std::uint64_t> FormattingErrors{0};
    std::atomic<std::uint64_t> SinkErrors{0};
    std::atomic<std::uint64_t> FileErrors{0};
};

#define LOG(...) \
    ::runtime::Logger::logAt(std::source_location::current(), __VA_ARGS__)

#define LOG_TRACE(CATEGORY, FORMAT, ...) \
    LOG(::runtime::LogOptions{.Level = ::runtime::levels::Trace, .Category = (CATEGORY)}, FORMAT __VA_OPT__(, ) __VA_ARGS__)

#define LOG_DEBUG(CATEGORY, FORMAT, ...) \
    LOG(::runtime::LogOptions{.Level = ::runtime::levels::Debug, .Category = (CATEGORY)}, FORMAT __VA_OPT__(, ) __VA_ARGS__)

#define LOG_INFO(CATEGORY, FORMAT, ...) \
    LOG(::runtime::LogOptions{.Level = ::runtime::levels::Info, .Category = (CATEGORY)}, FORMAT __VA_OPT__(, ) __VA_ARGS__)

#define LOG_WARNING(CATEGORY, FORMAT, ...) \
    LOG(::runtime::LogOptions{.Level = ::runtime::levels::Warning, .Category = (CATEGORY)}, FORMAT __VA_OPT__(, ) __VA_ARGS__)

#define LOG_ERROR(CATEGORY, FORMAT, ...) \
    LOG(::runtime::LogOptions{.Level = ::runtime::levels::Error, .Category = (CATEGORY)}, FORMAT __VA_OPT__(, ) __VA_ARGS__)

#define LOG_FATAL(CATEGORY, FORMAT, ...) \
    LOG(::runtime::LogOptions{.Level = ::runtime::levels::Fatal, .Category = (CATEGORY)}, FORMAT __VA_OPT__(, ) __VA_ARGS__)

} // namespace runtime

namespace debug
{
template <class... ArgTypes>
void print(std::format_string<ArgTypes...> Format, ArgTypes&&... Args) noexcept
{
    try
    {
        runtime::Logger::writeDebugText(
            std::format(Format, std::forward<ArgTypes>(Args)...),
            false);
    }
    catch (...)
    {
    }
}

template <class... ArgTypes>
void println(std::format_string<ArgTypes...> Format, ArgTypes&&... Args) noexcept
{
    try
    {
        runtime::Logger::writeDebugText(
            std::format(Format, std::forward<ArgTypes>(Args)...),
            true);
    }
    catch (...)
    {
    }
}

inline void println() noexcept
{
    runtime::Logger::writeDebugText({}, true);
}
} // namespace debug
