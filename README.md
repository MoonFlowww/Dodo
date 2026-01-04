# Dodo 🌌

**NASA-Grade Safety. HFT Latency. Embedded Determinism.**

Dodo is a high-performance, single-header C++20 framework designed for environments where **failure is not an option** and **latency is measured in CPU cycles**. It provides a zero-cost abstraction for defensive programming by separating "Hot Path" execution logic from "Cold Path" failure handling.

---

## Core Use Cases
* **High-Frequency Trading (HFT):** Guarding order-entry gates with sub-nanosecond overhead.
* **Aerospace & Automotive:** Enforcing "Design by Contract" with zero dynamic allocation or exceptions.
* **Embedded Systems:** Real-time safe drivers with deterministic error propagation.
* **Mission Critical Software:** Distinguishing between recoverable data errors and fatal logic invariants.

---

## 🛠 Documentation & API

### Safety Checks (Frontend Macros)

| Macro | Severity | Description |
| --- | --- | --- |
| `DODO_REQUIRE(cond, code)` | Recoverable | Pre-condition check; returns `Status` on failure. |
| `DODO_ENSURE(cond, code)` | Recoverable | Post-condition check; returns `Status` on failure. |
| `DODO_INVARIANT(cond, code)` | **Fatal** | Logic check; triggers `fail_fast` (panic). |
| `DODO_CHECK_NOT_NULL(ptr, code)` | Recoverable | Optimized null pointer validation. |
| `DODO_CHECK_RANGE(v, lo, hi, code)` | Recoverable | Bound check using unsigned comparison tricks. |
| `DODO_CHECK_ALIGNED(p, align, code)` | Recoverable | Power-of-2 memory alignment check. |

### Control Flow
* **`DODO_TRY(stmt)`**: Standardizes early returns. If the statement fails, it immediately propagates the error.
* **`fallback_or(status, action)`**: Provides a local override to recover from a failure at the call site.

---

## Underlying Optimization (Proof of Trust)
Dodo achieves its performance through several key architectural decisions:

1. **Branch Prediction Hints:** Every check uses `DODO_LIKELY` (`__builtin_expect`), ensuring the CPU keeps the success path in the pipeline.

2. **Cold Path Isolation:** All failure logic—including the construction of the `Failure` context—is marked with `__attribute__((cold))` and `noinline`. This pushes error-handling code out of the Instruction Cache (I-Cache).

3. **Register-Passable Status:** The `Dodo::Status` struct is a POD (Plain Old Data) type that fits in a single CPU register (2 bytes + padding), making error propagation nearly free.

4. **Zero-Allocation:** Dodo never calls `new` or `malloc`. It relies on pre-registered static function pointers for panics and fallbacks.



---

## Benchmark Results
Reproduced via the included `run_stresstest.sh` on an x86_64 native build:

| Scenario | Mode | Avg Cycles | Status |
| --- | --- | --- | --- |
| **Hot Path (Success)** | Release (-O3) | **~6.8 - 8.7** | Highly Optimized |
| **Memory Alignment** | Release (-O3) | **~7.8 - 9.5** | Single-Instruction  |
| **Cold Path (Failure)** | Release (-O3) | **~14.5 - 23.4** | Exception-Free Recovery |
| **Debug Mode** | Debug (-O0) | **~27.3 - 49.1** | Traceability Enabled |
> Note: Latency varies by compiler LTO and `DODO_FAST_MODE` settings.

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


### Optimization Knob: `DODO_FAST_MODE`
In extreme production environments, define `DODO_FAST_MODE` to strip all string literals (`__FILE__`, `__func__`) from the binary, significantly reducing `.rodata` size and eliminating diagnostic overhead.
