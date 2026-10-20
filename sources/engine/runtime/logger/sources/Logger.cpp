#include "logger/Logger.hpp"

#include <cstdio>
#include <stdexcept>

namespace runtime
{
const runtime::Logger::Config runtime::Logger::DefaultConfig{};

Message Message::make(
    std::uint64_t Id,
    const LogOptions& Options,
    const std::source_location& Location,
    std::string Text)
{
    Message result;
    result.Id = Id;
    result.LevelCode = Options.Level.Code;
    result.LevelPriority = Options.Level.Priority;
    result.LevelName = Options.Level.Name;
    result.CategoryName = Options.Category.Name;
    result.CategoryColor = Options.Category.Color;
    result.Destination = Options.Destination;
    result.IsVerbose = Options.IsVerbose;
    result.FileName = Location.file_name();
    result.FunctionName = Location.function_name();
    result.Line = Location.line();
    result.Column = Location.column();
    result.MessageText = std::move(Text);
    result.Timestamp = std::chrono::system_clock::now();
    result.ThreadId = std::this_thread::get_id();
    return result;
}

std::string Message::serialize(const Message& Record)
{
    constexpr std::uint32_t FormatVersion = 2;

    return std::format(
        "{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
        FormatVersion,
        Record.Id,
        Record.LevelCode,
        Record.LevelPriority,
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Record.Timestamp.time_since_epoch()).count(),
        Record.Line,
        Record.Column,
        Record.Destination.Value,
        Record.IsVerbose,
        escapeField(Record.LevelName),
        escapeField(Record.CategoryName),
        escapeField(Record.CategoryColor),
        escapeField(Record.FileName),
        escapeField(Record.FunctionName),
        escapeField(Record.MessageText));
}

Message Message::deserialize(const std::string& Str)
{
    const auto fields = splitEscapedFields(Str);
    if (fields.empty())
    {
        throw std::invalid_argument("Message::deserialize: empty record");
    }

    const auto version = std::stoul(fields[0]);
    if ((version == 1 && fields.size() != 13) ||
        (version == 2 && fields.size() != 15) ||
        (version != 1 && version != 2))
    {
        throw std::invalid_argument("Message::deserialize: invalid record");
    }

    Message output;
    output.Id = std::stoull(fields[1]);
    output.LevelCode = std::stoul(fields[2]);
    output.LevelPriority = std::stoul(fields[3]);
    const auto ns_since_epoch = static_cast<std::int64_t>(std::stoll(fields[4]));
    output.Timestamp = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds(ns_since_epoch)));
    output.Line = static_cast<std::uint_least32_t>(std::stoul(fields[5]));
    output.Column = static_cast<std::uint_least32_t>(std::stoul(fields[6]));

    const std::size_t text_offset = version == 2 ? 9 : 7;
    if (version == 2)
    {
        output.Destination = LogDestination(
            static_cast<LogDestination::underlying>(std::stoul(fields[7])));
        output.IsVerbose = fields[8] == "1";
    }

    output.LevelName = unescapeField(fields[text_offset]);
    output.CategoryName = unescapeField(fields[text_offset + 1]);
    output.CategoryColor = unescapeField(fields[text_offset + 2]);
    output.FileName = unescapeField(fields[text_offset + 3]);
    output.FunctionName = unescapeField(fields[text_offset + 4]);
    output.MessageText = unescapeField(fields[text_offset + 5]);
    return output;
}

std::string Message::escapeField(std::string_view InputChars)
{
    std::string output;
    output.reserve(InputChars.size());
    for (const char character : InputChars)
    {
        switch (character)
        {
        case '\\': output += "\\\\"; break;
        case '|': output += "\\|"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default: output.push_back(character); break;
        }
    }
    return output;
}

std::string Message::unescapeField(std::string_view InputChars)
{
    std::string output;
    output.reserve(InputChars.size());
    bool escaped = false;

    for (const char character : InputChars)
    {
        if (!escaped)
        {
            if (character == '\\')
            {
                escaped = true;
            }
            else
            {
                output.push_back(character);
            }
            continue;
        }

        switch (character)
        {
        case 'n': output.push_back('\n'); break;
        case 'r': output.push_back('\r'); break;
        case 't': output.push_back('\t'); break;
        default: output.push_back(character); break;
        }
        escaped = false;
    }

    if (escaped)
    {
        output.push_back('\\');
    }
    return output;
}

std::vector<std::string> Message::splitEscapedFields(const std::string& Line)
{
    std::vector<std::string> output;
    std::string current;
    current.reserve(Line.size());
    bool escaped = false;

    for (const char character : Line)
    {
        if (!escaped && character == '|')
        {
            output.push_back(std::move(current));
            current.clear();
            continue;
        }

        current.push_back(character);
        if (!escaped && character == '\\')
        {
            escaped = true;
        }
        else
        {
            escaped = false;
        }
    }

    output.push_back(std::move(current));
    return output;
}

std::string LogTextFormatter::format(const Message& InMessage)
{
    const auto timestamp = std::chrono::floor<std::chrono::milliseconds>(InMessage.Timestamp);
    if (!InMessage.IsVerbose)
    {
        return std::format(
            "[{:%H:%M:%S}][{}] {}",
            timestamp,
            InMessage.LevelName,
            InMessage.MessageText);
    }

    return std::format(
        "[{:%H:%M:%S}][{}][{}][Thread {}] {} [{}:{} {}]",
        timestamp,
        InMessage.LevelName,
        InMessage.CategoryName,
        InMessage.ThreadId,
        InMessage.MessageText,
        shortFileName(InMessage.FileName),
        InMessage.Line,
        InMessage.FunctionName);
}

std::string_view LogTextFormatter::shortFileName(std::string_view Path) noexcept
{
    const auto position = Path.find_last_of("/\\");
    return position == std::string_view::npos ? Path : Path.substr(position + 1);
}

Logger::Logger()
    : MessageQueue(RingBufferCapacity)
{
    SinksSnapshot.store(std::make_shared<const sink_list_t>(), std::memory_order_release);
    SerializedBuffer.reserve(64 * 1024);
}

Logger::~Logger()
{
    stop();
}

Logger& Logger::self()
{
    static Logger instance;
    return instance;
}

bool Logger::start(Config InConfig)
{
    InConfig.Mode = ERunMode::BackgroundThread;
    return startImpl(std::move(InConfig));
}

bool Logger::startManually(Config InConfig)
{
    InConfig.Mode = ERunMode::ManualFramePump;
    return startImpl(std::move(InConfig));
}

bool Logger::startImpl(Config InConfig)
{
    std::lock_guard lifecycle_lock(LifecycleMutex);
    stopLocked();

    if (InConfig.FlushThresholdBytes == 0)
    {
        InConfig.FlushThresholdBytes = 64 * 1024;
    }
    if (InConfig.BackgroundBatchMessages == 0)
    {
        InConfig.BackgroundBatchMessages = 1;
    }

    CurrentConfig = std::move(InConfig);
    CurrentMode.store(CurrentConfig.Mode, std::memory_order_release);
    MinimumPriority.store(CurrentConfig.MinimumLevel.Priority, std::memory_order_release);
    SerializedBuffer.clear();
    SerializedBuffer.reserve(CurrentConfig.FlushThresholdBytes);
    openLogFile();

    IsRunning.store(true, std::memory_order_release);
    if (CurrentConfig.Mode == ERunMode::BackgroundThread)
    {
        try
        {
            WorkerThread = std::jthread([this](std::stop_token StopToken) {
                backgroundLoop(StopToken);
            });
        }
        catch (...)
        {
            IsRunning.store(false, std::memory_order_release);
            closeLogFile();
            return false;
        }
    }
    return true;
}

void Logger::stop() noexcept
{
    std::lock_guard lifecycle_lock(LifecycleMutex);
    stopLocked();
}

void Logger::stopLocked() noexcept
{
    IsRunning.store(false, std::memory_order_release);
    QueueCondition.notify_all();
    waitForActiveProducers();

    if (WorkerThread.joinable())
    {
        WorkerThread.request_stop();
        QueueCondition.notify_all();
        WorkerThread.join();
    }

    std::lock_guard consumer_lock(ConsumerMutex);
    while (consumeOne())
    {
    }
    flushSerializedBuffer();
    closeLogFile();
}

void Logger::waitForActiveProducers() noexcept
{
    auto active = ActiveProducers.load(std::memory_order_acquire);
    while (active != 0)
    {
        ActiveProducers.wait(active, std::memory_order_acquire);
        active = ActiveProducers.load(std::memory_order_acquire);
    }
}

void Logger::backgroundLoop(std::stop_token StopToken) noexcept
{
    while (!StopToken.stop_requested())
    {
        if (drainBatch(CurrentConfig.BackgroundBatchMessages, std::nullopt))
        {
            continue;
        }

        std::unique_lock wait_lock(QueueWaitMutex);
        QueueCondition.wait_for(
            wait_lock,
            CurrentConfig.BackgroundWakeInterval,
            [this, &StopToken] {
                return StopToken.stop_requested() ||
                    !IsRunning.load(std::memory_order_acquire) ||
                    !MessageQueue.isEmpty();
            });
    }
}

bool Logger::pumpFrame(
    std::size_t MaxMessages,
    std::optional<std::chrono::nanoseconds> TimeBudget)
{
    if (!IsRunning.load(std::memory_order_acquire) ||
        CurrentMode.load(std::memory_order_acquire) != ERunMode::ManualFramePump)
    {
        return false;
    }
    return drainBatch(MaxMessages, TimeBudget);
}

bool Logger::drainBatch(
    std::size_t MaxMessages,
    std::optional<std::chrono::nanoseconds> TimeBudget)
{
    if (MaxMessages == 0)
    {
        return false;
    }

    std::unique_lock consumer_lock(ConsumerMutex, std::try_to_lock);
    if (!consumer_lock.owns_lock())
    {
        return false;
    }

    const auto begin = std::chrono::steady_clock::now();
    std::size_t consumed = 0;
    while (consumed < MaxMessages && consumeOne())
    {
        ++consumed;
        if (TimeBudget.has_value() &&
            std::chrono::steady_clock::now() - begin >= *TimeBudget)
        {
            break;
        }
    }

    flushSerializedBuffer();
    return consumed != 0;
}

bool Logger::consumeOne()
{
    auto message = MessageQueue.tryPop();
    if (!message.has_value())
    {
        return false;
    }
    dispatchMessage(*message);
    return true;
}

void Logger::dispatchMessage(const Message& InMessage) noexcept
{
    try
    {
        const std::string formatted = LogTextFormatter::format(InMessage);
        const LogDestination destinations = InMessage.Destination;

        if (hasDestination(destinations, LogDestination_t::Screen))
        {
            dispatchScreen(InMessage, formatted);
        }
        if (hasDestination(destinations, LogDestination_t::EditorConsole))
        {
            dispatchEditorConsole(InMessage, formatted);
        }
        if (hasDestination(destinations, LogDestination_t::ProgramConsole))
        {
            writeProgramConsole(InMessage, formatted);
        }
        if (hasDestination(destinations, LogDestination_t::File))
        {
            writeFile(InMessage, formatted);
        }

        dispatchCustomSinks(InMessage, formatted);
    }
    catch (...)
    {
        FormattingErrors.fetch_add(1, std::memory_order_relaxed);
    }
}

void Logger::dispatchScreen(const Message& InMessage, std::string_view Formatted) noexcept
{
    const auto sink = ScreenSinkSnapshot.load(std::memory_order_acquire);
    if (!sink || !*sink)
    {
        return;
    }

    try
    {
        (*sink)(InMessage, Formatted);
    }
    catch (...)
    {
        SinkErrors.fetch_add(1, std::memory_order_relaxed);
    }
}

void Logger::dispatchEditorConsole(const Message& InMessage, std::string_view Formatted) noexcept
{
    const auto sink = EditorConsoleSinkSnapshot.load(std::memory_order_acquire);
    if (!sink || !*sink)
    {
        return;
    }

    try
    {
        (*sink)(InMessage, Formatted);
    }
    catch (...)
    {
        SinkErrors.fetch_add(1, std::memory_order_relaxed);
    }
}

void Logger::dispatchCustomSinks(const Message& InMessage, std::string_view Formatted) noexcept
{
    const auto sinks = SinksSnapshot.load(std::memory_order_acquire);
    if (!sinks)
    {
        return;
    }

    for (const auto& sink : *sinks)
    {
        if (!sink.Callback || !hasDestination(InMessage.Destination, sink.Destination))
        {
            continue;
        }

        try
        {
            sink.Callback(InMessage, Formatted);
        }
        catch (...)
        {
            SinkErrors.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void Logger::writeProgramConsole(const Message& InMessage, std::string_view Formatted) noexcept
{
    std::lock_guard console_lock(ProgramConsoleMutex);
    std::FILE* stream = InMessage.LevelPriority >= levels::Error.Priority ? stderr : stdout;
    std::fwrite(Formatted.data(), sizeof(char), Formatted.size(), stream);
    std::fwrite("\n", sizeof(char), 1, stream);
    if (InMessage.LevelPriority >= levels::Error.Priority)
    {
        std::fflush(stream);
    }
}

void Logger::writeDebugText(std::string_view Text, bool AppendNewLine) noexcept
{
    Logger& logger = self();
    std::lock_guard console_lock(logger.ProgramConsoleMutex);
    std::fwrite(Text.data(), sizeof(char), Text.size(), stdout);
    if (AppendNewLine)
    {
        std::fwrite("\n", sizeof(char), 1, stdout);
    }
    std::fflush(stdout);
}

Logger::sink_id_t Logger::registerSink(sink_callback_t Sink, LogDestination InDestination)
{
    if (!Sink || InDestination == LogDestination_t::None)
    {
        return 0;
    }

    const sink_id_t id = NextSinkId.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard update_lock(SinkUpdateMutex);
    const auto current = SinksSnapshot.load(std::memory_order_acquire);
    sink_list_t next = current ? *current : sink_list_t{};
    next.push_back(SinkEntry{
        .Id = id,
        .Destination = InDestination,
        .Callback = std::move(Sink),
    });
    SinksSnapshot.store(
        std::make_shared<const sink_list_t>(std::move(next)),
        std::memory_order_release);
    return id;
}

bool Logger::unregisterSink(sink_id_t Id)
{
    if (Id == 0)
    {
        return false;
    }

    std::lock_guard update_lock(SinkUpdateMutex);
    const auto current = SinksSnapshot.load(std::memory_order_acquire);
    if (!current)
    {
        return false;
    }

    sink_list_t next;
    next.reserve(current->size());
    bool removed = false;
    for (const auto& sink : *current)
    {
        if (sink.Id == Id)
        {
            removed = true;
        }
        else
        {
            next.push_back(sink);
        }
    }

    if (removed)
    {
        SinksSnapshot.store(
            std::make_shared<const sink_list_t>(std::move(next)),
            std::memory_order_release);
    }
    return removed;
}

void Logger::clearSinks()
{
    std::lock_guard update_lock(SinkUpdateMutex);
    SinksSnapshot.store(std::make_shared<const sink_list_t>(), std::memory_order_release);
}

void Logger::setScreenSink(screen_sink_t Sink)
{
    std::lock_guard update_lock(BuiltinSinkUpdateMutex);
    ScreenSinkSnapshot.store(
        Sink ? std::make_shared<const screen_sink_t>(std::move(Sink)) : nullptr,
        std::memory_order_release);
}

void Logger::clearScreenSink()
{
    std::lock_guard update_lock(BuiltinSinkUpdateMutex);
    ScreenSinkSnapshot.store(nullptr, std::memory_order_release);
}

void Logger::setEditorConsoleSink(editor_console_sink_t Sink)
{
    std::lock_guard update_lock(BuiltinSinkUpdateMutex);
    EditorConsoleSinkSnapshot.store(
        Sink ? std::make_shared<const editor_console_sink_t>(std::move(Sink)) : nullptr,
        std::memory_order_release);
}

void Logger::clearEditorConsoleSink()
{
    std::lock_guard update_lock(BuiltinSinkUpdateMutex);
    EditorConsoleSinkSnapshot.store(nullptr, std::memory_order_release);
}

void Logger::setMinimumLevel(LogLevel Level) noexcept
{
    MinimumPriority.store(Level.Priority, std::memory_order_release);
}

void Logger::flush()
{
    std::lock_guard lifecycle_lock(LifecycleMutex);
    std::lock_guard consumer_lock(ConsumerMutex);
    while (consumeOne())
    {
    }
    flushSerializedBuffer();
    if (LogFile.is_open())
    {
        LogFile.flush();
        if (!LogFile)
        {
            FileErrors.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void Logger::openLogFile() noexcept
{
    LogFilePath.clear();
    if (!CurrentConfig.EnableFileOutput)
    {
        return;
    }

    try
    {
        if (CurrentConfig.FilePath.has_value())
        {
            LogFilePath = *CurrentConfig.FilePath;
        }
        else
        {
            const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            LogFilePath = std::filesystem::current_path() /
                "logs" /
                std::format("runtime_{}.log", timestamp);
        }

        const auto parent = LogFilePath.parent_path();
        if (!parent.empty())
        {
            std::error_code error;
            std::filesystem::create_directories(parent, error);
            if (error)
            {
                FileErrors.fetch_add(1, std::memory_order_relaxed);
                LogFilePath.clear();
                return;
            }
        }

        LogFile.open(LogFilePath, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!LogFile.is_open())
        {
            FileErrors.fetch_add(1, std::memory_order_relaxed);
            LogFilePath.clear();
        }
    }
    catch (...)
    {
        FileErrors.fetch_add(1, std::memory_order_relaxed);
        LogFilePath.clear();
    }
}

void Logger::writeFile(const Message& InMessage, std::string_view Formatted) noexcept
{
    if (!LogFile.is_open())
    {
        return;
    }

    try
    {
        SerializedBuffer.append(Formatted);
        SerializedBuffer.push_back('\n');
        if (SerializedBuffer.size() >= CurrentConfig.FlushThresholdBytes ||
            InMessage.LevelPriority >= levels::Error.Priority)
        {
            flushSerializedBuffer();
        }
    }
    catch (...)
    {
        FileErrors.fetch_add(1, std::memory_order_relaxed);
    }
}

void Logger::flushSerializedBuffer() noexcept
{
    if (!LogFile.is_open() || SerializedBuffer.empty())
    {
        return;
    }

    LogFile.write(SerializedBuffer.data(), static_cast<std::streamsize>(SerializedBuffer.size()));
    if (!LogFile)
    {
        FileErrors.fetch_add(1, std::memory_order_relaxed);
        LogFile.clear();
    }
    SerializedBuffer.clear();
}

void Logger::closeLogFile() noexcept
{
    if (!LogFile.is_open())
    {
        return;
    }
    LogFile.flush();
    if (!LogFile)
    {
        FileErrors.fetch_add(1, std::memory_order_relaxed);
    }
    LogFile.close();
}

Logger::ERunMode Logger::getRunMode() const noexcept
{
    return CurrentMode.load(std::memory_order_acquire);
}

std::filesystem::path Logger::getLogFilePath() const
{
    std::lock_guard lifecycle_lock(LifecycleMutex);
    return LogFilePath;
}

std::uint64_t Logger::getDroppedMessageCount() const noexcept
{
    return DroppedMessages.load(std::memory_order_relaxed);
}

std::uint64_t Logger::getFormattingErrorCount() const noexcept
{
    return FormattingErrors.load(std::memory_order_relaxed);
}

std::uint64_t Logger::getSinkErrorCount() const noexcept
{
    return SinkErrors.load(std::memory_order_relaxed);
}

std::uint64_t Logger::getFileErrorCount() const noexcept
{
    return FileErrors.load(std::memory_order_relaxed);
}

bool Logger::isRunning() const noexcept
{
    return IsRunning.load(std::memory_order_acquire);
}
} // namespace runtime
