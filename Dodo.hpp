#ifndef DODO_FAILURE_HANDLING_HPP
#define DODO_FAILURE_HANDLING_HPP
// ============================================================================
// DODO FRAMEWORK
// Context: NASA (Safety), Embedded (Determinism), HFT (Latency)
// Constraints: No alloc, no except, no locks, no RTTI, no I/O
// ============================================================================

#include <cstdint>
#include <cstddef>

// ----------------------------------------------------------------------------
// Compiler Intrinsics & Optimization Macros
// ----------------------------------------------------------------------------
#if defined(__GNUC__) || defined(__clang__)
#define DODO_LIKELY(x)      __builtin_expect(!!(x), 1)
#define DODO_UNLIKELY(x)    __builtin_expect(!!(x), 0)
#define DODO_COLD           __attribute__((cold))
#define DODO_NOINLINE       __attribute__((noinline))
#define DODO_TRAP()         __builtin_trap()
#elif defined(_MSC_VER)
#define DODO_LIKELY(x)      (x)
#define DODO_UNLIKELY(x)    (x)
#define DODO_COLD           // MSVC handles cold code via profile-guided opts mostly
#define DODO_NOINLINE       __declspec(noinline)
#define DODO_TRAP()         __debugbreak()
#else
#define DODO_LIKELY(x)      (x)
#define DODO_UNLIKELY(x)    (x)
#define DODO_COLD
#define DODO_NOINLINE
#define DODO_TRAP()         (*(volatile int*)0 = 0)
#endif

namespace Dodo {
    // --------------------------------------------------------------------------
    // Core Types (POD, register-passable)
    // --------------------------------------------------------------------------

    enum class Code : uint16_t {
        Ok = 0,
        PreconditionFailed,
        PostconditionFailed,
        InvariantBroken,
        NullPointer,
        OutOfRange,
        Misaligned,
        Overflow,
        Timeout,
        ExternalFault,
        InternalFault
    };

    enum class Severity : uint8_t { Recoverable, Fatal };

    // Minimal context. Constructed only on the cold path.
    struct Failure {
        Code code;
        Severity sev;
        const char *expr; // nullptr if DODO_FAST_MODE
        const char *file; // nullptr if DODO_FAST_MODE
        uint32_t line; // 0 if DODO_FAST_MODE
        const char *func; // nullptr if DODO_FAST_MODE
    };

    // Status: nodiscard forces the caller to handle the error.
    // Fits in a single register (2 bytes + padding).
    struct [[nodiscard]] Status {
        Code code;

        constexpr bool ok() const noexcept { return code == Code::Ok; }

        // Helper for boolean contexts
        explicit constexpr operator bool() const noexcept { return ok(); }

        static constexpr Status ok_status() noexcept { return Status{Code::Ok}; }
        static constexpr Status fail(Code c) noexcept { return Status{c}; }
    };

    // --------------------------------------------------------------------------
    // Policy Hooks (Function Pointers)
    // --------------------------------------------------------------------------

    using PanicFn = void(*)(const Failure &) noexcept;
    using FallbackFn = Status(*)(const Failure &) noexcept;

    // Default implementations
    [[noreturn]] inline void default_panic(const Failure &) noexcept {
        DODO_TRAP();
        // Hint to compiler that this path is dead
        while (true) {
        }
    }

    inline Status default_fallback(const Failure &f) noexcept {
        return Status{f.code};
    }

    // Global hooks (relaxed atomicity assumed: set during single-threaded init)
    namespace internal {
        static PanicFn g_panic_handler = default_panic;
        static FallbackFn g_fallback_handler = default_fallback;
    }

    inline void set_panic_handler(PanicFn fn) noexcept {
        internal::g_panic_handler = fn ? fn : default_panic;
    }

    inline void set_fallback_handler(FallbackFn fn) noexcept {
        internal::g_fallback_handler = fn ? fn : default_fallback;
    }

    // --------------------------------------------------------------------------
    // Cold Path Endpoints (Optimization: Move failure logic out of I-Cache)
    // --------------------------------------------------------------------------

    // 1) fail_fast: Fatal endpoint. Never returns.
    [[noreturn]] DODO_COLD DODO_NOINLINE
    inline void fail_fast(const Failure &f) noexcept {
        internal::g_panic_handler(f);
        // Should not reach here, but ensure noreturn semantics
        DODO_TRAP();
        while (true) {
        }
    }

    // 2) fail_recoverable: Recoverable endpoint.
    DODO_COLD DODO_NOINLINE
    inline Status fail_recoverable(const Failure &f) noexcept {
        return internal::g_fallback_handler(f);
    }

    // --------------------------------------------------------------------------
    // Hot Path Logic (Inline, Branch Predicted)
    // --------------------------------------------------------------------------

    // 3) require: Precondition (Recoverable)
    // Usage: status = Dodo::require(x > 0, Code::OutOfRange, ctx);
    inline Status require(bool cond, Code code, const Failure &f) noexcept {
        (void) code;
        if (DODO_LIKELY(cond)) {
            return Status::ok_status();
        }
        return fail_recoverable(f);
    }

    // 4) ensure: Postcondition (Recoverable)
    inline Status ensure(bool cond, Code code, const Failure &f) noexcept {
        (void) code;
        if (DODO_LIKELY(cond)) {
            return Status::ok_status();
        }
        return fail_recoverable(f);
    }

    // 5) invariant: Internal Consistency (Fatal)
    inline void invariant(bool cond, Code code, const Failure &f) noexcept {
        (void) code;
        if (DODO_UNLIKELY(!cond)) {
            fail_fast(f);
        }
    }

    // 6) check_not_null (Recoverable)
    // Template instantiates to a simple pointer check.
    template<class T>
    inline Status check_not_null(const T *p, Code code, const Failure &f) noexcept {
        (void) code;
        if (DODO_LIKELY(p != nullptr)) {
            return Status::ok_status();
        }
        return fail_recoverable(f);
    }

    // 7) check_range (Recoverable)
    // Optimized to unsigned comparison trick where possible by compilers
    template<class T>
    inline Status check_range(T v, T lo, T hi, Code code, const Failure &f) noexcept {
        (void) code;
        if (DODO_LIKELY(v >= lo && v <= hi)) {
            return Status::ok_status();
        }
        return fail_recoverable(f);
    }

    // 8) check_aligned (Recoverable)
    inline Status check_aligned(const void *p, size_t align, Code code, const Failure &f) noexcept {
        (void) code;
        // Note: align must be power of 2. Use invariant() to enforce this if needed,
        // but here we assume caller correctness for speed.
        const uintptr_t addr = reinterpret_cast<uintptr_t>(p);
        if (DODO_LIKELY((addr & (align - 1)) == 0)) {
            return Status::ok_status();
        }
        return fail_recoverable(f);
    }

    // 9) propagate: Standardize early return
    inline Status propagate(Status s) noexcept {
        return s;
    }

    // 10) fallback_or: Explicit local fallback
    using FallbackAction = Status(*)(void) noexcept;

    inline Status fallback_or(Status s, FallbackAction action) noexcept {
        if (DODO_LIKELY(s.ok())) {
            return s;
        }
        return action();
    }
}

// ----------------------------------------------------------------------------
// Macros (Context Capture & Convenience)
// ----------------------------------------------------------------------------

// Optimization parm: DODO_FAST_MODE
// If defined, strips string literals from binary to reduce rodata size
#ifdef DODO_FAST_MODE
#define DODO_CTX(c, s) Dodo::Failure{(c), (s), nullptr, nullptr, 0u, nullptr}
#define DODO_EXPR_STR(cond) nullptr
#define DODO_MAKE_FAIL(severity, code_enum, cond_str) \
        Dodo::Failure{(code_enum), (severity), (cond_str), nullptr, 0u, nullptr}
#else
#define DODO_CTX(c, s) Dodo::Failure{(c), (s), #c, __FILE__, static_cast<uint32_t>(__LINE__), __func__}
#define DODO_EXPR_STR(cond) #cond
#define DODO_MAKE_FAIL(severity, code_enum, cond_str) \
        Dodo::Failure{(code_enum), (severity), (cond_str), __FILE__, static_cast<uint32_t>(__LINE__), __func__}
#endif

// API Macros
// Note: We pass the Failure object construction into the inline function
// The compiler's inlining and dead-code elimination will ensure the Failure
// struct is NOT constructed on the stack unless the branch is taken

#define DODO_REQUIRE(cond, code) \
    Dodo::require((cond), (code), DODO_MAKE_FAIL(Dodo::Severity::Recoverable, (code), DODO_EXPR_STR(cond)))

#define DODO_ENSURE(cond, code) \
    Dodo::ensure((cond), (code), DODO_MAKE_FAIL(Dodo::Severity::Recoverable, (code), DODO_EXPR_STR(cond)))

#define DODO_INVARIANT(cond, code) \
    Dodo::invariant((cond), (code), DODO_MAKE_FAIL(Dodo::Severity::Fatal, (code), DODO_EXPR_STR(cond)))

#define DODO_CHECK_NOT_NULL(ptr, code) \
    Dodo::check_not_null((ptr), (code), DODO_MAKE_FAIL(Dodo::Severity::Recoverable, (code), DODO_EXPR_STR(ptr)))

#define DODO_CHECK_RANGE(v, lo, hi, code) \
    Dodo::check_range((v), (lo), (hi), (code), DODO_MAKE_FAIL(Dodo::Severity::Recoverable, (code), DODO_EXPR_STR(v)))

#define DODO_CHECK_ALIGNED(ptr, alignment, code) \
    Dodo::check_aligned((ptr), (alignment), (code), DODO_MAKE_FAIL(Dodo::Severity::Recoverable, (code), DODO_EXPR_STR(ptr)))


// Control Sugar-Flow 
#define DODO_TRY(stmt) \
    do { \
        Dodo::Status _dodo_s = (stmt); \
        if (DODO_UNLIKELY(!_dodo_s.ok())) { \
            return Dodo::propagate(_dodo_s); \
        } \
    } while(0)

#endif
