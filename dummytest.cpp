// Dummy test case with fake driver errors 

#include <iostream>
#include <vector>
#include "Dodo.hpp"

struct DriverModule {
    const char* name;
    size_t size;
    uintptr_t load_address;
    bool signature_valid;
};

bool is_memory_region_available(uintptr_t addr, size_t size) {
    (void)addr; (void)size;
    return true; // Simulate success
}

Dodo::Status validate_and_load_driver(const DriverModule* driver) noexcept {
    // 1. Critical Pointer Check
    DODO_TRY(DODO_CHECK_NOT_NULL(driver, Dodo::Code::NullPointer));

    // 2. Metadata Validation (Recoverable)
    // If the name is missing, we don't crash; we just reject the load.
    DODO_TRY(DODO_CHECK_NOT_NULL(driver->name, Dodo::Code::PreconditionFailed));

    // 3. Security Boundary (Recoverable)
    // Driver size must be within kernel limits (e.g., 4MB)
    DODO_TRY(DODO_CHECK_RANGE(driver->size, size_t(1), size_t(4 * 1024 * 1024), Dodo::Code::OutOfRange));

    // 4. Memory Alignment (Recoverable)
    // Modules must be 4KB page-aligned for MMU mapping
    DODO_TRY(DODO_CHECK_ALIGNED((void*)driver->load_address, 4096, Dodo::Code::Misaligned));

    // 5. Fatal Logic Check (Invariant)
    // If the memory region isn't available but we reached this stage,
    // the OS memory map is corrupt. Halt immediately.
    DODO_INVARIANT(is_memory_region_available(driver->load_address, driver->size), Dodo::Code::InvariantBroken);

    return Dodo::Status::ok_status();
}

void my_panic_handler(const Dodo::Failure& f) noexcept {
    std::printf("\n[FATAL] System Halted at %s:%u\n", f.file, f.line);
    std::printf("Failed Condition: %s\n", f.expr);
    std::abort();
}

Dodo::Status my_fallback_handler(const Dodo::Failure& f) noexcept {
    std::printf("[LOG] Load Rejected: %s (Code: %u)\n", f.expr, (uint32_t)f.code);
    return Dodo::Status::fail(f.code);
}

int main() {
    Dodo::set_panic_handler(my_panic_handler); // set panic button (dodo) with panic button of user
    Dodo::set_fallback_handler(my_fallback_handler);

    // Scenario: Misaligned Load Address
    DriverModule malformed_driver = { "NetworkCard", 1024, 0x1235, true };
    std::cout << "Attempting to load driver..." << std::endl;

    Dodo::Status s = validate_and_load_driver(&malformed_driver);
    if (!s.ok()) {
        std::cout << "Outcome: Driver rejected. Kernel remains stable." << std::endl;
    }

    return 0;
}
