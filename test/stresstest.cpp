#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <string>
#include <iostream>
#include <vector>
#include <iomanip>
#include <x86intrin.h>

#if defined(__linux__) || defined(__APPLE__)
    #include <unistd.h>
    #include <sys/wait.h>
    #define DODO_HAS_FORK 1
#else
    #define DODO_HAS_FORK 0
#endif

#include "Dodo.hpp"

#define ITERATIONS 1'000'000

// --- Mock Hardware / State ---
struct MockMarketData {
    double price;
    uint32_t volume;
    const char* exchange_id;
};

struct MockDMA {
    void*  buffer;
    size_t size;
    size_t alignment;
};

// --- Statistics Tracking ---
struct BenchResult {
    const char* label;
    uint64_t total_cycles;
    double   avg;
    uint64_t ok_count;
    uint64_t fail_count;
    uint16_t sink;
};

// --- Minimal Test Harness ---
static int g_test_failures = 0;

#define TEST_ASSERT(expr) \
    do { \
        if (!(expr)) { \
            ++g_test_failures; \
            std::cerr << "[TEST FAIL] " << __FILE__ << ":" << __LINE__ << "  " << #expr << std::endl; \
        } \
    } while (0)

#define TEST_EQ(a,b) \
    do { \
        const auto _a = (a); \
        const auto _b = (b); \
        if (!(_a == _b)) { \
            ++g_test_failures; \
            std::cerr << "[TEST FAIL] " << __FILE__ << ":" << __LINE__ \
                      << "  " << #a << " == " << #b \
                      << "  (got " << static_cast<uint64_t>(_a) \
                      << ", expected " << static_cast<uint64_t>(_b) << ")" \
                      << std::endl; \
        } \
    } while (0)

// --- Handler Recording (no placeholders) ---
struct FailureSnapshot {
    Dodo::Code code{Dodo::Code::Ok};
    Dodo::Severity sev{Dodo::Severity::Recoverable};
    const char* expr{nullptr};
    const char* file{nullptr};
    uint32_t line{0};
    const char* func{nullptr};
};

static std::atomic<uint64_t> g_recoverable_hits{0};
static std::atomic<bool> g_panic_triggered{false};
static FailureSnapshot g_last_failure{};

// Snapshotting fallback handler (single-thread use)
Dodo::Status recording_fallback_handler(const Dodo::Failure& f) noexcept {
    g_recoverable_hits.fetch_add(1, std::memory_order_relaxed);
    g_last_failure = FailureSnapshot{f.code, f.sev, f.expr, f.file, f.line, f.func};
    return Dodo::Status::fail(f.code);
}

// Count-only fallback handler (threaded stress)
Dodo::Status counting_fallback_handler(const Dodo::Failure& f) noexcept {
    (void)f;
    g_recoverable_hits.fetch_add(1, std::memory_order_relaxed);
    return Dodo::Status::fail(f.code);
}

// Panic handler: exit with code (used for death tests via fork)
[[noreturn]] void stress_panic_handler(const Dodo::Failure& f) noexcept {
    g_panic_triggered.store(true, std::memory_order_relaxed);
    g_last_failure = FailureSnapshot{f.code, f.sev, f.expr, f.file, f.line, f.func};
    std::exit(static_cast<int>(f.code));
}

// ---Scenarios ---
Dodo::Status scenario_nested_logic(const MockMarketData& md) noexcept {
    DODO_TRY(DODO_REQUIRE(md.price > 0.0, Dodo::Code::PreconditionFailed));
    DODO_TRY(DODO_REQUIRE(md.volume > 0, Dodo::Code::PreconditionFailed));
    return Dodo::Status::ok_status();
}

Dodo::Status scenario_dma_check(const MockDMA& dma) noexcept {
    return DODO_CHECK_ALIGNED(dma.buffer, dma.alignment, Dodo::Code::Misaligned);
}

Dodo::Status scenario_safety_limits(const int* sensor_val) noexcept {
    DODO_TRY(DODO_CHECK_NOT_NULL(sensor_val, Dodo::Code::NullPointer));
    return DODO_CHECK_RANGE(*sensor_val, 0, 1024, Dodo::Code::OutOfRange);
}

static std::atomic<uint64_t> g_local_action_hits{0};
Dodo::Status local_recovery_action() noexcept {
    g_local_action_hits.fetch_add(1, std::memory_order_relaxed);
    return Dodo::Status::ok_status();
}

Dodo::Status scenario_local_fallback(bool fail) noexcept {
    Dodo::Status s = fail ? Dodo::Status::fail(Dodo::Code::Timeout) : Dodo::Status::ok_status();
    return Dodo::fallback_or(s, local_recovery_action);
}

void scenario_fatal_logic(bool corruption) noexcept {
    DODO_INVARIANT(!corruption, Dodo::Code::InvariantBroken);
}

static void run_unit_tests() {
    // Handlers for deterministic recording.
    Dodo::set_fallback_handler(recording_fallback_handler);
    Dodo::set_panic_handler(stress_panic_handler);

    g_recoverable_hits.store(0, std::memory_order_relaxed);
    g_panic_triggered.store(false, std::memory_order_relaxed);
    g_last_failure = FailureSnapshot{};

    
    { // 1) require / ensure (success + failure)
        Dodo::Status ok = DODO_REQUIRE(1 == 1, Dodo::Code::PreconditionFailed);
        TEST_ASSERT(ok.ok());

        Dodo::Status bad = Dodo::Status::ok_status();

#ifndef DODO_FAST_MODE
        const uint32_t expected_line = __LINE__ + 1;
        bad = DODO_REQUIRE(1 == 2, Dodo::Code::PreconditionFailed);
#else
        bad = DODO_REQUIRE(1 == 2, Dodo::Code::PreconditionFailed);
#endif

        TEST_ASSERT(!bad.ok());
        TEST_EQ(bad.code, Dodo::Code::PreconditionFailed);

        TEST_EQ(g_last_failure.code, Dodo::Code::PreconditionFailed);
        TEST_EQ(g_last_failure.sev, Dodo::Severity::Recoverable);

#ifdef DODO_FAST_MODE
        TEST_ASSERT(g_last_failure.expr == nullptr);
        TEST_ASSERT(g_last_failure.file == nullptr);
        TEST_EQ(g_last_failure.line, 0u);
        TEST_ASSERT(g_last_failure.func == nullptr);
#else
        TEST_ASSERT(g_last_failure.expr != nullptr);
        TEST_ASSERT(std::strstr(g_last_failure.expr, "1 == 2") != nullptr);
        TEST_ASSERT(g_last_failure.file != nullptr);
        TEST_ASSERT(std::strstr(g_last_failure.file, __FILE__) != nullptr);
        TEST_EQ(g_last_failure.line, expected_line);
        TEST_ASSERT(g_last_failure.func != nullptr);
        TEST_ASSERT(std::strcmp(g_last_failure.func, "run_unit_tests") == 0);
#endif
    }


    
    { // 2) null check
        int x = 7;
        TEST_ASSERT(DODO_CHECK_NOT_NULL(&x, Dodo::Code::NullPointer).ok());

        Dodo::Status bad = DODO_CHECK_NOT_NULL((const int*)nullptr, Dodo::Code::NullPointer);
        TEST_ASSERT(!bad.ok());
        TEST_EQ(bad.code, Dodo::Code::NullPointer);
    }

    
    { // 3) range check
        TEST_ASSERT(DODO_CHECK_RANGE(10, 0, 20, Dodo::Code::OutOfRange).ok());

        Dodo::Status bad_lo = DODO_CHECK_RANGE(-1, 0, 20, Dodo::Code::OutOfRange);
        TEST_ASSERT(!bad_lo.ok());
        TEST_EQ(bad_lo.code, Dodo::Code::OutOfRange);

        Dodo::Status bad_hi = DODO_CHECK_RANGE(21, 0, 20, Dodo::Code::OutOfRange);
        TEST_ASSERT(!bad_hi.ok());
        TEST_EQ(bad_hi.code, Dodo::Code::OutOfRange);
    }

    
    { // 4) aligned check (success + failure)
        alignas(64) uint8_t buf[256];
        MockDMA dma_ok{buf, sizeof(buf), 64};
        TEST_ASSERT(scenario_dma_check(dma_ok).ok());

        MockDMA dma_bad{buf + 1, sizeof(buf) - 1, 64};
        Dodo::Status s = scenario_dma_check(dma_bad);
        TEST_ASSERT(!s.ok());
        TEST_EQ(s.code, Dodo::Code::Misaligned);
    }

    
    { // 5) DODO_TRY early return behavior
        auto f = []() noexcept -> Dodo::Status {
            DODO_TRY(DODO_REQUIRE(false, Dodo::Code::PreconditionFailed));
            // Must not execute:
            DODO_TRY(DODO_REQUIRE(false, Dodo::Code::InternalFault));
            return Dodo::Status::ok_status();
        };

        Dodo::Status s = f();
        TEST_ASSERT(!s.ok());
        TEST_EQ(s.code, Dodo::Code::PreconditionFailed);
    }

    
    { // 6) fallback_or: action called only on failure
        g_local_action_hits.store(0, std::memory_order_relaxed);

        TEST_ASSERT(scenario_local_fallback(false).ok());
        TEST_EQ(g_local_action_hits.load(std::memory_order_relaxed), 0ull);

        TEST_ASSERT(scenario_local_fallback(true).ok());
        TEST_EQ(g_local_action_hits.load(std::memory_order_relaxed), 1ull);
    }

    
    { // 7) Threaded stress (init-only handler set, then parallel calls)
        g_recoverable_hits.store(0, std::memory_order_relaxed);
        Dodo::set_fallback_handler(counting_fallback_handler);

        constexpr int kThreads = 4;
        constexpr int kItersPerThread = 200'000;

        std::vector<std::thread> th;
        th.reserve(kThreads);

        for (int t = 0; t < kThreads; ++t) {
            th.emplace_back([]{
                for (int i = 0; i < kItersPerThread; ++i) {
                    (void)DODO_REQUIRE(false, Dodo::Code::PreconditionFailed);
                }
            });
        }
        for (auto& x : th) x.join();

        const uint64_t expected = uint64_t(kThreads) * uint64_t(kItersPerThread);
        TEST_EQ(g_recoverable_hits.load(std::memory_order_relaxed), expected);

        // Restore recording handler for remaining tests/bench.
        Dodo::set_fallback_handler(recording_fallback_handler);
    }

    
#if DODO_HAS_FORK
    { // 8) Fatal invariant (death test) without killing the whole test runner
        pid_t pid = ::fork();
        if (pid == 0) {
            // Child: should terminate via panic handler exit(code).
            scenario_fatal_logic(true);
            std::exit(111); // should never reach
        }

        int status = 0;
        ::waitpid(pid, &status, 0);

        TEST_ASSERT(WIFEXITED(status));
        if (WIFEXITED(status)) {
            TEST_EQ(WEXITSTATUS(status), static_cast<int>(Dodo::Code::InvariantBroken));
        }
    }
#else
    // Non-POSIX: cannot death-test without external framework.
    // Still validates other correctness properties above.
#endif
}

// Benchmark
static volatile uint16_t g_sink = 0;
template<typename Func>
BenchResult run_bench(const char* label, Func f) {
    uint64_t ok = 0, fail = 0;

    _mm_lfence();
    uint64_t start = __rdtsc();
    _mm_lfence();

    for (int i = 0; i < ITERATIONS; ++i) {
        const Dodo::Status s = f();

        ok   += static_cast<uint64_t>(s.ok());
        fail += static_cast<uint64_t>(!s.ok());

        g_sink ^= static_cast<uint16_t>(s.code);
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }

    _mm_lfence();
    uint64_t end = __rdtsc();
    _mm_lfence();

    const uint64_t total = end - start;
    return { label, total, static_cast<double>(total) / ITERATIONS, ok, fail, g_sink };
}

int main() {
    // Run correctness + recording tests first.
    run_unit_tests();

    if (g_test_failures != 0) {
        std::cerr << "\nUnit tests failed: " << g_test_failures << std::endl;
        return 1;
    }

    // Handlers for benchmark run.
    g_recoverable_hits.store(0, std::memory_order_relaxed);
    g_panic_triggered.store(false, std::memory_order_relaxed);
    Dodo::set_fallback_handler(recording_fallback_handler);
    Dodo::set_panic_handler(stress_panic_handler);

    std::vector<BenchResult> results;

    // EXECUTION

    // Scenario 1: HFT Nested Hot Path (Success)
    MockMarketData md_good = {150.25, 1000, "NYSE"};
    results.push_back(run_bench("HFT Hot Path (Success)", [&]() -> Dodo::Status {
        return scenario_nested_logic(md_good);
    }));

    // Scenario 2: Embedded DMA Alignment (Success)
    alignas(64) uint64_t align_buffer[64];
    MockDMA dma_good = { (void*)align_buffer, 512, 64 };
    results.push_back(run_bench("Embedded DMA (Aligned)", [&]() -> Dodo::Status {
        return scenario_dma_check(dma_good);
    }));

    // Scenario 3: Safety Range Check (Success)
    int sensor = 500;
    results.push_back(run_bench("Safety Range (In-Bounds)", [&]() -> Dodo::Status {
        return scenario_safety_limits(&sensor);
    }));

    // Scenario 4: Local Fallback (Success path)
    results.push_back(run_bench("Local Fallback (No-op)", [&]() -> Dodo::Status {
        return scenario_local_fallback(false);
    }));

    // Scenario 5: The "Cost of Failure" (Cold Path)
    results.push_back(run_bench("COLD PATH (Failure)", [&]() -> Dodo::Status {
        return scenario_safety_limits(nullptr); // Triggers NullPointer
    }));

    // REPORTING
    std::cout << std::left
              << std::setw(30) << "Scenario"
              << std::setw(15) << "Avg Cycles"
              << std::setw(12) << "OK"
              << std::setw(12) << "FAIL"
              << "Sink"
              << std::endl;

    std::cout << std::string(75, '-') << std::endl;

    for (const auto& res : results) {
        std::cout << std::left << std::setw(30) << res.label
                  << std::setw(15) << std::fixed << std::setprecision(2) << res.avg
                  << std::setw(12) << res.ok_count
                  << std::setw(12) << res.fail_count
                  << res.sink
                  << std::endl;
    }

    std::cout << "\nTotal Recoverable Errors Handled: "
              << g_recoverable_hits.load(std::memory_order_relaxed) << std::endl;

    
#if DODO_HAS_FORK // fatal invariant demo without killing the test
    std::cout << "\nDeath-test: Fatal Invariant (child process)..." << std::endl;
    pid_t pid = ::fork();
    if (pid == 0) {
        scenario_fatal_logic(true);
        std::exit(111);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        std::cout << "Child exited with code: " << WEXITSTATUS(status) << std::endl;
    } else {
        std::cout << "Child did not exit normally." << std::endl;
    }
#else
    std::cout << "\nSkipping fatal invariant demo (no fork support)." << std::endl;
#endif

    return 0;
}
