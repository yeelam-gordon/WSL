/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SlowOperationWatcherUnitTests.cpp

Abstract:

    Unit tests for SlowOperationWatcher. These exercise the emission logic through an
    injected recording sink, so no ETW listener or running distro is required. The
    watcher emits AT MOST ONE event:

      - Fast path (finishes under SlowThreshold): nothing is emitted.
      - Slow completion (finishes at >= SlowThreshold): exactly one event with the real
        elapsed time.
      - Reset() behaves like the destructor and never double-emits.

--*/

#include "precomp.h"
#include "Common.h"

#include <mutex>
#include <thread>
#include <vector>

namespace SlowOperationWatcherUnitTests {

namespace {

    using namespace std::chrono_literals;

    struct RecordedEvent
    {
        std::string Name;
        std::chrono::milliseconds Threshold;
        std::chrono::milliseconds Elapsed;
        unsigned Line;
    };

    // The sink is a plain function pointer (no captures), so recorded events land in this
    // file-scope recorder. TAEF runs the methods of a class serially, and each test resets
    // the recorder up front, so there is no cross-test interference.
    struct Recorder
    {
        std::mutex Lock;
        std::vector<RecordedEvent> Events;

        void Clear()
        {
            std::scoped_lock lock{Lock};
            Events.clear();
        }

        std::vector<RecordedEvent> Snapshot()
        {
            std::scoped_lock lock{Lock};
            return Events;
        }
    };

    Recorder g_recorder;

    void RecordSink(const SlowOperationWatcher::Event& e) noexcept
    try
    {
        std::scoped_lock lock{g_recorder.Lock};
        g_recorder.Events.push_back(RecordedEvent{e.Name, e.Threshold, e.Elapsed, e.Location.line()});
    }
    CATCH_LOG()

} // namespace

class SlowOperationWatcherUnitTests
{
    WSL_TEST_CLASS(SlowOperationWatcherUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        g_recorder.Clear();
        return true;
    }

    // An operation that finishes comfortably under the threshold emits nothing.
    TEST_METHOD(FastPathEmitsNothing)
    {
        {
            SlowOperationWatcher watcher{"FastPath", 30s, &RecordSink};
        }

        VERIFY_ARE_EQUAL(g_recorder.Snapshot().size(), static_cast<size_t>(0));
    }

    // The default threshold fast path stays silent, asserted through the injected sink.
    // Also compile-checks the single-argument call-site form (all timing params defaulted),
    // which is how every real call site constructs the watcher.
    TEST_METHOD(DefaultConstructionFastPathEmitsNothing)
    {
        {
            // Injected sink with the DEFAULT threshold (10 s): a scope that exits immediately
            // is well under threshold, so nothing must be recorded. This is the assertion the
            // production-sink single-arg form below cannot make (it writes to ETW, not g_recorder).
            SlowOperationWatcher watcher{"Defaults", std::chrono::seconds{10}, &RecordSink};
        }
        VERIFY_ARE_EQUAL(g_recorder.Snapshot().size(), static_cast<size_t>(0));

        {
            // Compile/behavior smoke test of the single-argument overload used by call sites
            // (defaults the threshold and the production telemetry sink). Fast path, so
            // it emits nothing and touches no ETW; nothing new should reach the recorder.
            SlowOperationWatcher watcher{"Defaults"};
        }
        VERIFY_ARE_EQUAL(g_recorder.Snapshot().size(), static_cast<size_t>(0));
    }

    // Reset() before the threshold disarms the watcher permanently: it stays silent at
    // Reset(), AND -- critically -- the destructor must not later re-evaluate a larger
    // elapsed (including work done after Reset()) and emit. Reset() is an authoritative
    // "operation ended here" marker, so post-Reset work must never be attributed to it.
    TEST_METHOD(ResetBeforeThresholdEmitsNothing)
    {
        // A large threshold relative to scheduling jitter: Reset() fires ~immediately after
        // construction, so the elapsed-at-Reset can only cross 300ms if the thread stalls for
        // >300ms before the very next statement, which is not realistic even on a busy CI box.
        constexpr auto threshold = 300ms;
        {
            SlowOperationWatcher watcher{"ResetFast", threshold, &RecordSink};

            // The watched operation finished quickly (under threshold) -> Reset() here.
            watcher.Reset();
            VERIFY_ARE_EQUAL(g_recorder.Snapshot().size(), static_cast<size_t>(0));

            // Simulate slow, unrelated work AFTER Reset() that pushes total elapsed past the
            // threshold. The destructor at scope exit must still emit nothing (Reset() already
            // claimed the single report), which is the property under test.
            std::this_thread::sleep_for(400ms);
        }

        VERIFY_ARE_EQUAL(g_recorder.Snapshot().size(), static_cast<size_t>(0));
    }

    // A slow operation emits exactly ONE record with the real elapsed time.
    TEST_METHOD(SlowCompletionEmitsOneRecord)
    {
        constexpr auto threshold = 50ms;
        {
            SlowOperationWatcher watcher{"SlowPhase", threshold, &RecordSink};

            // Stay alive well past the threshold.
            std::this_thread::sleep_for(200ms);
        }

        const auto events = g_recorder.Snapshot();
        VERIFY_ARE_EQUAL(events.size(), static_cast<size_t>(1));

        const auto& record = events[0];
        VERIFY_ARE_EQUAL(record.Name, std::string{"SlowPhase"});
        VERIFY_ARE_EQUAL(record.Threshold, threshold);

        // The record carries the real elapsed time, not just the threshold.
        VERIFY_IS_TRUE(record.Elapsed >= threshold);
        VERIFY_IS_TRUE(record.Elapsed >= 150ms);

        // source_location survived being copied into the Event (by value, no dangling ref).
        VERIFY_IS_TRUE(record.Line != 0);
    }

    // Reset() after the threshold emits the one record, and the destructor
    // that follows must not emit a second one (at-most-once via m_reported.exchange).
    TEST_METHOD(ResetAfterThresholdEmitsOnce)
    {
        constexpr auto threshold = 50ms;
        {
            SlowOperationWatcher watcher{"ResetSlow", threshold, &RecordSink};

            std::this_thread::sleep_for(150ms);
            watcher.Reset();

            const auto afterReset = g_recorder.Snapshot();
            VERIFY_ARE_EQUAL(afterReset.size(), static_cast<size_t>(1));
            VERIFY_IS_TRUE(afterReset[0].Elapsed >= threshold);
        }

        // Destruction after Reset() must not produce a second record.
        VERIFY_ARE_EQUAL(g_recorder.Snapshot().size(), static_cast<size_t>(1));
    }
};

} // namespace SlowOperationWatcherUnitTests
