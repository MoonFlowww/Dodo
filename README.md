# Dodo 🌌
> _Dodo — so your code (and you) can sleep better._

Dodo is a single-header `C++20` failure-handling framework for systems with strict **latency**, **determinism**, and **reliability** constraints.
It provides explicit, branch-predicted checks on the hot path while moving diagnostic construction and recovery logic to cold paths.
The design avoids `exceptions`, `dynamic allocation`, `RTTI`, `locks`, and `I/O`, making it suitable for low-latency trading systems, embedded software, and safety-critical runtimes.

---

## Core Use Cases
* **High-Frequency Trading (HFT):** Guard order-entry gates with near-zero overhead.
* **Embedded Systems:** Real-time safe drivers with explicit failure handling.
* **Mission Critical Software:** Separate recoverable data errors from fatal logic invariants.

---

## Quick Start

### Include
Dodo is header-only:

```cpp
#include "Dodo.hpp"
```

### Define a function that returns `Dodo::Status`

```cpp
#include "Dodo.hpp"

Dodo::Status parse_qty(int qty) noexcept {
    // Input validation: recoverable.
    DODO_REQUIRE(qty > 0, Dodo::Code::OutOfRange);

    // ...

    // Internal assumptions: fatal.
    DODO_INVARIANT((qty % 2) == 0, Dodo::Code::InvariantBroken);

    return Dodo::Status::ok_status();
}

Dodo::Status foo() noexcept {
    DODO_TRY(parse_qty(4));
    return Dodo::Status::ok_status();
}
```

### Handle failures at a boundary
Dodo is designed so that “leaf” functions return `Status`, and boundaries (network ingress, order-entry gate, ISR boundary, task boundary, etc.) decide what to do.

```cpp
Dodo::Status s = foo();
if (!s) {
    // Map to your error domain, metrics, drop order, etc.
}
```

---

## Mental Model

### What happens on a check
Each check splits into:

* **Hot path (success):** an inlined boolean + branch prediction hint.
* **Cold path (failure):** build a `Dodo::Failure` object and dispatch to either:
  * `fallback_handler` (recoverable checks), or
  * `panic_handler` (fatal invariants).

The frontend macros exist to capture context (expression / file / line / function) without paying for it when the condition passes.

### Recoverable vs Fatal
Use these as contracts:

* **Recoverable**: input/data is allowed to be wrong; caller can decide.
  * Examples: range checks on messages, null pointers from external APIs, timeouts.
* **Fatal**: logic/contract violation; continuing could corrupt state.
  * Examples: broken invariants, impossible states, required alignment not met.

---

## Core Types

### `Dodo::Code`
A compact error code (`uint16_t`) meant to be cheap to propagate.

Current codes:

| Code | Intended meaning |
| --- | --- |
| `Ok` | Success |
| `PreconditionFailed` | Caller violated a precondition |
| `PostconditionFailed` | Callee violated a postcondition |
| `InvariantBroken` | Internal logic invariant violated |
| `NullPointer` | Null pointer observed |
| `OutOfRange` | Value out of allowed range |
| `Misaligned` | Pointer not aligned as required |
| `Overflow` | Arithmetic/size overflow detected |
| `Timeout` | Operation exceeded allowed time |
| `ExternalFault` | External dependency failed |
| `InternalFault` | Unexpected internal failure |

Practical guidance:
* Pick a code that is stable and meaningful at API boundaries.
* Avoid per-call-site unique codes unless you also provide a mapping table in your project.

### `Dodo::Severity`
* `Recoverable`: handled via fallback handler and returned as `Status`.
* `Fatal`: handled via panic handler (does not return).

### `Dodo::Failure`
A minimal context object, constructed only on the failure (cold) path.

Fields:

| Field | Meaning |
| --- | --- |
| `code` | The `Dodo::Code` associated with the failure |
| `sev` | `Recoverable` or `Fatal` |
| `expr` | Stringized check expression (may be `nullptr` in fast mode) |
| `file` | `__FILE__` (may be `nullptr` in fast mode) |
| `line` | `__LINE__` (0 in fast mode) |
| `func` | `__func__` (may be `nullptr` in fast mode) |

The framework never allocates; if you want richer diagnostics, store them externally (e.g., ring buffer, per-thread scratch, flight recorder) inside your handlers.

### `Dodo::Status`
A small POD return type (currently wraps `Dodo::Code`).

Key properties:
* `[[nodiscard]]`: forces callers to handle the return value.
* `ok()` / `operator bool()`: convenient success checks.
* `ok_status()` / `fail(Code)`: constructors.

Typical convention:
* `return Status::ok_status();` on success.
* return the result of a check / `DODO_TRY` on failure.

---

## Policy Hooks (Customizing Behavior)

Dodo uses global function pointers as policy hooks. They are intended to be configured during single-threaded startup and then treated as read-only.

### Panic handler
```cpp
using PanicFn = void(*)(const Dodo::Failure&) noexcept;
void Dodo::set_panic_handler(PanicFn fn) noexcept;
```

Default behavior:
* If you never set a handler, Dodo uses a trap-based default panic handler.
* Passing `nullptr` to `set_panic_handler` resets back to the default.

When invoked:
* Called by `fail_fast()`.
* Must not return (the framework traps after calling it to guarantee `[[noreturn]]`).

Typical usage:
* hard abort / trap (default),
* send a structured crash record to a shared memory region,
* trigger a watchdog reset.

### Fallback handler
```cpp
using FallbackFn = Dodo::Status(*)(const Dodo::Failure&) noexcept;
void Dodo::set_fallback_handler(FallbackFn fn) noexcept;
```

Default behavior:
* If you never set a handler, Dodo returns `Status{f.code}` (it propagates the `Failure` code).
* Passing `nullptr` to `set_fallback_handler` resets back to the default.

When invoked:
* Called by `fail_recoverable()`.
* Its return value becomes the `Status` returned to the caller.

Typical usage:
* map `Failure.code` to your `Status` domain,
* record metrics (counter per code),
* push failure context to a lock-free ring buffer.

Constraints:
* Keep handlers `noexcept` and allocation-free.
* Avoid locks and syscalls if used on hot boundaries.

---

## API Reference

### Frontend Macros (recommended entry points)
These macros capture context and route to the underlying functions.

| Macro | Returns | Intended use | On failure |
| --- | --- | --- | --- |
| `DODO_REQUIRE(cond, code)` | `Status` | Precondition validation | calls fallback handler |
| `DODO_ENSURE(cond, code)` | `Status` | Postcondition validation | calls fallback handler |
| `DODO_INVARIANT(cond, code)` | `void` | Logic/invariant validation | calls panic handler, traps |
| `DODO_CHECK_NOT_NULL(ptr, code)` | `Status` | Null pointer validation | calls fallback handler |
| `DODO_CHECK_RANGE(v, lo, hi, code)` | `Status` | Inclusive range check | calls fallback handler |
| `DODO_CHECK_ALIGNED(ptr, alignment, code)` | `Status` | Alignment check | calls fallback handler |

Parameter notes:
* `cond`: boolean expression. Evaluated exactly once.
* `code`: a `Dodo::Code` value. It is also embedded into the `Failure` object; keep it semantically consistent.
* `alignment`: must be a power of 2 and non-zero (see “Checks” section).


### Underlying functions (call directly only when you need custom context)
The frontend macros are thin wrappers around the `Dodo::` functions below. In most code you should prefer the macros because they:
* capture call-site context (`expr`, `file`, `line`, `func`), and
* ensure the `Failure` object is constructed only on the cold (failure) path.

If you call the functions directly, you are responsible for providing a `Dodo::Failure` object.

#### Failure endpoints

```cpp
[[noreturn]] void Dodo::fail_fast(const Dodo::Failure& f) noexcept;
Dodo::Status Dodo::fail_recoverable(const Dodo::Failure& f) noexcept;
```

* `fail_fast`: dispatches to the global panic handler and then traps. Never returns.
* `fail_recoverable`: dispatches to the global fallback handler and returns its `Status`.

#### Hot-path checks

```cpp
Dodo::Status Dodo::require(bool cond, Dodo::Code code, const Dodo::Failure& f) noexcept;
Dodo::Status Dodo::ensure(bool cond, Dodo::Code code, const Dodo::Failure& f) noexcept;
void Dodo::invariant(bool cond, Dodo::Code code, const Dodo::Failure& f) noexcept;

template<class T>
Dodo::Status Dodo::check_not_null(const T* p, Dodo::Code code, const Dodo::Failure& f) noexcept;

template<class T>
Dodo::Status Dodo::check_range(T v, T lo, T hi, Dodo::Code code, const Dodo::Failure& f) noexcept;

Dodo::Status Dodo::check_aligned(const void* p, size_t align, Dodo::Code code, const Dodo::Failure& f) noexcept;
```

Semantics (all `noexcept`):
* On success: returns `Status::ok_status()` (or does nothing for `invariant`).
* On failure:
  * `require` / `ensure` / `check_*` call `fail_recoverable(f)`.
  * `invariant` calls `fail_fast(f)`.

Notes:
* The `code` parameter is currently unused internally (kept for readability and to allow future instrumentation). The macros pass the same value both as `code` and as `f.code`; if you call these functions directly, keep those consistent.

#### Propagation primitive

```cpp
Dodo::Status Dodo::propagate(Dodo::Status s) noexcept;
```

Currently this is an identity function, used by `DODO_TRY` as a single chokepoint for “return the error”.
Projects sometimes patch this to add lightweight instrumentation (counters, trace hooks) without changing call sites.

#### Context-capture helpers (mostly internal)
These are exposed as macros in the header to avoid overhead:

* `DODO_MAKE_FAIL(severity, code, cond_str)` builds a `Failure` with call-site metadata.
* `DODO_EXPR_STR(x)` stringizes `x` (or becomes `nullptr` in `DODO_FAST_MODE`).
* `DODO_CTX(code, severity)` builds a `Failure` without a condition string (useful for manual construction).

Prefer using the frontend macros unless you have a specific reason to construct failures manually.

### Control flow helpers

#### `DODO_TRY(stmt)`
Use inside a function returning `Dodo::Status`.

Semantics:
* Evaluates `stmt` once.
* If `stmt` returns a failing `Status`, it returns early from the current function.

This standardizes “early return” style without exceptions:

```cpp
Dodo::Status f() noexcept {
    DODO_TRY(g());
    DODO_TRY(h());
    return Dodo::Status::ok_status();
}
```

#### `Dodo::fallback_or(status, action)`
```cpp
using FallbackAction = Dodo::Status(*)(void) noexcept;
Dodo::Status Dodo::fallback_or(Dodo::Status s, FallbackAction action) noexcept;
```

Semantics:
* If `s.ok()`, returns `s`.
* Otherwise executes `action()` and returns its result.

Use this when a local call site wants to override the global fallback strategy:

```cpp
Dodo::Status s = DODO_CHECK_RANGE(x, 0, 10, Dodo::Code::OutOfRange);
return Dodo::fallback_or(s, []() noexcept {
    // Convert to a different code, clamp, or choose a safe default.
    return Dodo::Status::fail(Dodo::Code::ExternalFault);
});
```

Guidance:
* Use sparingly; global fallback is usually the right policy for consistency.
* Prefer it at well-defined boundaries (parsers, adapters, protocol layers).

---

## Checks: Detailed Semantics and Pitfalls

### `DODO_REQUIRE` / `DODO_ENSURE`
* Both are recoverable and return a `Status`.
* They differ only in intent (pre vs post condition). The distinction is valuable in code review and when mapping codes.

When to use:
* `REQUIRE`: validate caller inputs, external data, config.
* `ENSURE`: validate outputs of an operation that can legitimately fail (e.g. an external adapter).

When **not** to use:
* Do not use for internal invariants. Prefer `DODO_INVARIANT`.

### `DODO_INVARIANT`
* Fatal by design.
* Use when failure implies a programmer error or state corruption risk.

Common uses:
* “This branch should be impossible.”
* “This pointer must be aligned because we allocated it.”
* “This enum value is validated earlier.”

### `DODO_CHECK_NOT_NULL(ptr, code)`
* Checks `ptr != nullptr`.
* Useful on externally provided pointers or API outputs.

Guidance:
* For references (`T&`), this is unnecessary.
* If a null pointer is always a programmer error in your codebase, consider making it an invariant instead.

### `DODO_CHECK_RANGE(v, lo, hi, code)`
* Inclusive range check: passes when `lo <= v && v <= hi`.

Guidance:
* Ensure `lo <= hi` at the call site (if that can vary). If it must always be true, enforce with `DODO_INVARIANT(lo <= hi, ...)`.
* For “half-open” ranges, use explicit conditions:

```cpp
DODO_REQUIRE(v >= lo && v < hi, Dodo::Code::OutOfRange);
```

### `DODO_CHECK_ALIGNED(ptr, alignment, code)`
* Checks `(uintptr_t(ptr) & (alignment - 1)) == 0`.

**Call-site requirements (important):**
* `alignment` must be **non-zero**.
* `alignment` must be a **power of 2**.

Recommended pattern:

```cpp
DODO_INVARIANT(alignment != 0 && (alignment & (alignment - 1)) == 0,
               Dodo::Code::InvariantBroken);
DODO_TRY(DODO_CHECK_ALIGNED(p, alignment, Dodo::Code::Misaligned));
```

---

## Optimization Knob: `DODO_FAST_MODE`

In extreme production environments, define `DODO_FAST_MODE` to strip string literals from the binary, reducing `.rodata` and removing diagnostic strings.

Effect:
* `Failure.expr`, `Failure.file`, `Failure.func` become `nullptr`.
* `Failure.line` becomes `0`.

This retains the error codes and control flow but drops call-site text.

---

## Underlying Optimization
Dodo achieves its performance through several key architectural decisions:

1. **Branch Prediction Hints:** Every check uses `DODO_LIKELY` / `DODO_UNLIKELY` (`__builtin_expect` on GCC/Clang), keeping the success path in the pipeline.

2. **Cold Path Isolation:** Failure endpoints are marked `cold` + `noinline`, pushing failure logic out of Cache.

3. **Register-Passable Status:** `Dodo::Status` is small and cheap to return.

4. **Zero-Allocation:** No `new`, no `malloc`. You control behavior via pre-registered function pointers.

---

## Benchmark Results
Reproduced via the included `run_stresstest.sh` on an x86_64 native build:

| Scenario | Mode | Avg Cycles | Status |
| --- | --- | --- | --- |
| **Hot Path (Success)** | Release (-O3) | **~6.8 - 8.7** | Highly Optimized |
| **Memory Alignment** | Release (-O3) | **~7.8 - 9.5** | Single-Instruction  |
| **Cold Path (Failure)** | Release (-O3) | **~14.5 - 23.4** | Exception-Free Recovery |
| **Debug Mode** | Debug  (-O0) | **~27.3 - 49.1** | Traceability Enabled |

Each method’s latency was measured in **CPU cycles** using `uint64_t __rdtsc()` and **1M** iterations to stabilize branch predictors and caches.

> Example conversion:
> * 3.5 GHz ⇒ 1 cycle ≈ 0.286 ns
> * 4.0 GHz ⇒ 1 cycle ≈ 0.250 ns
> * 4.7 GHz ⇒ 1 cycle ≈ 0.213 ns
>
> `time per cycle = 1 / frequency`

Note: Latency varies by compiler, LTO, and `DODO_FAST_MODE`.

---

## Open-Source Reproduction
To verify the benchmarks on your hardware, use the provided stress test suite:

1. Ensure you have `g++` (C++20) and a Linux-based environment (for `fork` death-tests).

2. Run the sweep:
```bash
chmod +x run_stresstest.sh
./run_stresstest.sh
```

3. Check `dodo_all_results.txt` for a comprehensive report across `O3`, `LTO`, `FAST_MODE`, and various Sanitizers (`ASan`, `UBSan`, `TSan`).
