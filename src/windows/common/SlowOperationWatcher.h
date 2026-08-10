/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SlowOperationWatcher.h

Abstract:

    RAII guard that times a scoped operation and emits **at most one** `SlowOperation`
    telemetry event. There are exactly two outcomes:

      1. The operation finishes in under `SlowThreshold` (10 s default) -- the common,
         healthy case. Nothing is emitted.
      2. The operation finishes but took at least `SlowThreshold`. On the watched operation's
         end -- the destructor, or the earlier Reset() call site -- one event is emitted carrying
         the real `elapsedMs` measured from construction to that point (the
         full scope lifetime for the destructor case; up to the Reset() call when Reset() is
         used to disarm early).

    `SlowThreshold` is the "slow enough to be worth reporting" filter applied at completion.
    Every event carries the phase name and the call site (std::source_location). The watcher
    is a passive observer: it never affects the operation it measures, and a failure in the
    telemetry path can never crash or slow the watched code.

    Usage:

        SlowOperationWatcher slow{"WaitForMiniInitConnect"};
        m_miniInitChannel = wsl::shared::SocketChannel{AcceptConnection(timeout), ...};

    If the scope needs to outlive the watched operation (for example to keep a
    pointer into an internal receive buffer alive without a nested block), call
    Reset() to disarm the watcher early:

        SlowOperationWatcher slow{"WaitForCreateInstanceResult"};
        const auto& result = channel.ReceiveMessage<...>(...);
        slow.Reset();
        // result remains valid and usable here

--*/

#pragma once

#include <windows.h>
#include <chrono>
#include <source_location>

class SlowOperationWatcher
{
public:
    // Alias for the millisecond durations used throughout the class.
    using Duration = std::chrono::milliseconds;

    // Contents of one SlowOperation telemetry record. Exposed so tests can observe
    // emissions through a custom sink without standing up an ETW listener.
    struct Event
    {
        const char* Name;   // phase identifier; must outlive the watcher
        Duration Threshold; // configured slow threshold
        Duration Elapsed;   // real time since construction
        // By value, not a reference: a diagnostic sink may copy the Event and read it after
        // the watcher is destroyed, which would dangle a reference. std::source_location is
        // cheap to copy, so keep Event self-contained.
        std::source_location Location;
    };

    // Receives the (at most one) SlowOperation record. Must be noexcept and must not block:
    // it is invoked from Reset() or the destructor. The default writes the telemetry event.
    using Sink = void (*)(const Event&) noexcept;

    // Name is taken as a char-array reference (const char (&)[N]) rather than a const char*
    // to force callers to pass an array and block the easy UAF of a temporary's pointer
    // (e.g. std::string::c_str()): the pointed-to storage must outlive the watcher.
    // The array type does not by itself guarantee static storage -- a non-static array would
    // also compile -- so in practice always pass a string literal. Keep Name a short CamelCase
    // phase identifier that the backend query can switch on (e.g. "WaitForMiniInitConnect").
    // SlowThreshold is the "slow enough to report at completion" filter. OnSlow defaults to
    // the telemetry emitter; tests inject a recording sink.
    template <size_t N>
    explicit SlowOperationWatcher(
        const char (&Name)[N],
        Duration SlowThreshold = std::chrono::seconds{10},
        Sink OnSlow = &EmitTelemetry,
        std::source_location Location = std::source_location::current()) noexcept :
        SlowOperationWatcher(static_cast<const char*>(Name), SlowThreshold, OnSlow, Location)
    {
    }

    // On destruction, emits the record if the operation was slow (>= threshold).
    ~SlowOperationWatcher() noexcept;

    // Disarm the watcher early (equivalent to the destructor, but at an explicit point). If the
    // operation took at least SlowThreshold, emits one record with the real elapsed time. The
    // fast path (under threshold) stays silent. Emits at most once across Reset() + the destructor.
    void Reset() noexcept;

    SlowOperationWatcher(const SlowOperationWatcher&) = delete;
    SlowOperationWatcher& operator=(const SlowOperationWatcher&) = delete;
    SlowOperationWatcher(SlowOperationWatcher&&) = delete;
    SlowOperationWatcher& operator=(SlowOperationWatcher&&) = delete;

private:
    // clang-format off
    explicit SlowOperationWatcher(
        _In_z_ const char* Name,
        Duration SlowThreshold,
        Sink OnSlow,
        std::source_location Location) noexcept;
    // clang-format on

    // Default sink: writes the SlowOperation telemetry event.
    static void EmitTelemetry(const Event& Record) noexcept;

    Duration Elapsed() const noexcept;
    void Emit(Duration Elapsed) noexcept;

    // Emit the record if the operation was slow and this is the first Finish() call.
    void Finish() noexcept;

    const char* const m_name;
    const Duration m_slowThreshold;
    const std::source_location m_location;
    const std::chrono::steady_clock::time_point m_start;
    const Sink m_sink;
    bool m_finished;
};
