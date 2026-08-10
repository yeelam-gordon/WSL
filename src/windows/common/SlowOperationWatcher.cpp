/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SlowOperationWatcher.cpp

Abstract:

    See header for contract. Emission is at most one event:

      - If the operation finishes (destructor or Reset()) and the elapsed time is at least
        SlowThreshold, emit one event with the real elapsed time.
      - If the operation finishes under SlowThreshold, emit nothing.

--*/

#include "precomp.h"
#include "SlowOperationWatcher.h"

namespace {
// std::source_location::file_name() returns the path as the compiler saw it, which on
// MSVC is an absolute build-agent path. Strip to the basename so telemetry groups the
// same file across different build environments without leaking machine-specific paths.
// The substring is taken from the same null-terminated char array, so the returned view's
// data() is safe to pass to C APIs that expect a null-terminated string.
constexpr std::string_view Basename(std::string_view Path) noexcept
{
    const auto pos = Path.find_last_of("\\/");
    return pos == std::string_view::npos ? Path : Path.substr(pos + 1);
}

static_assert(Basename("/foo/bar/test.cpp") == "test.cpp");
static_assert(Basename("C:\\src\\test.cpp") == "test.cpp");
static_assert(Basename("no_separator.cpp") == "no_separator.cpp");
} // namespace

// clang-format off
SlowOperationWatcher::SlowOperationWatcher(
    _In_z_ const char* Name,
    Duration SlowThreshold,
    Sink OnSlow,
    std::source_location Location) noexcept :
    // clang-format on
    m_name(Name), m_slowThreshold(SlowThreshold), m_location(Location), m_start(std::chrono::steady_clock::now()), m_sink(OnSlow), m_finished(false)
{
    WI_ASSERT(m_sink != nullptr);
    WI_ASSERT(m_slowThreshold.count() > 0);
}

SlowOperationWatcher::~SlowOperationWatcher() noexcept
{
    Finish();
}

void SlowOperationWatcher::Reset() noexcept
{
    Finish();
}

void SlowOperationWatcher::Finish() noexcept
{
    if (!m_finished)
    {
        m_finished = true;
        const auto elapsed = Elapsed();
        if (elapsed >= m_slowThreshold)
        {
            Emit(elapsed);
        }
    }
}

SlowOperationWatcher::Duration SlowOperationWatcher::Elapsed() const noexcept
{
    return std::chrono::duration_cast<Duration>(std::chrono::steady_clock::now() - m_start);
}

void SlowOperationWatcher::Emit(Duration Elapsed) noexcept
{
    m_sink(Event{m_name, m_slowThreshold, Elapsed, m_location});
}

void SlowOperationWatcher::EmitTelemetry(const Event& Record) noexcept
try
{
    WSL_LOG_TELEMETRY(
        "SlowOperation",
        PDT_ProductAndServicePerformance,
        TraceLoggingValue(Record.Name, "name"),
        TraceLoggingInt64(Record.Threshold.count(), "thresholdMs"),
        TraceLoggingInt64(Record.Elapsed.count(), "elapsedMs"),
        TraceLoggingValue(Basename(Record.Location.file_name()).data(), "file"),
        TraceLoggingValue(Record.Location.function_name(), "function"),
        TraceLoggingUInt32(Record.Location.line(), "line"));
}
CATCH_LOG()
