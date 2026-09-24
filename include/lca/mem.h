// =============================================================================
//  lca/mem.h  --  hardware detection and hard memory budgets
// -----------------------------------------------------------------------------
//  The agent must never be able to exceed the machine's 8 GB RAM / 4 GB VRAM
//  envelope.  Two independent mechanisms enforce that:
//
//    1. MemoryGuard::initialise() installs an RLIMIT_AS (and RLIMIT_DATA)
//       rlimit on the process, so the kernel itself refuses allocations beyond
//       the budget.  This is a hard wall, not a best-effort counter.
//    2. MemoryGuard::reserve()/release() track the large, agent-controlled
//       buffers (file snapshots, HTTP bodies, model context, diffs) so the
//       agent can degrade gracefully before the kernel wall is reached.
//
//  Windows/non-Linux builds degrade to the accounting path only; everything is
//  guarded by __linux__ / POSIX feature tests.
// =============================================================================
#ifndef LCA_MEM_H
#define LCA_MEM_H

#include "lca/common.h"
#include "lca/buf.h"

#include <mutex>

namespace lca {

// -----------------------------------------------------------------------------
// Hardware description
// -----------------------------------------------------------------------------
struct HardwareInfo {
    std::string cpu_model;
    int         cpu_cores{0};
    int         cpu_threads{0};
    uint64_t    ram_total_bytes{0};
    uint64_t    ram_available_bytes{0};
    uint64_t    swap_total_bytes{0};
    std::string os_name;
    std::string kernel;

    // GPU (optional).  The reference target is an RTX 2050 with 4 GB.
    bool        gpu_present{false};
    std::string gpu_name;
    uint64_t    vram_total_bytes{0};
    uint64_t    vram_free_bytes{0};
    std::string gpu_driver;

    std::string describe() const;
    Json        to_json() const;
};

// Probes /proc, sysfs, sysconf and (when present) nvidia-smi.
HardwareInfo detect_hardware();

// -----------------------------------------------------------------------------
// Budgets
// -----------------------------------------------------------------------------
// Both caps are clamped so that they can never reach the hardware limits the
// project is required to stay under: RAM < 8 GiB, VRAM < 4 GiB.
struct MemoryLimits {
    uint64_t ram_cap_bytes{6ull * 1024 * 1024 * 1024};    // hard RAM ceiling
    uint64_t vram_cap_bytes{3ull * 1024 * 1024 * 1024};   // hard VRAM ceiling
    double   safety_fraction{0.80};   // fraction of detected RAM to use at most
    bool     install_rlimits{true};   // kernel-enforced RLIMIT_AS / RLIMIT_DATA
    bool     reserve_transient_headroom{true};
};

// -----------------------------------------------------------------------------
// MemoryGuard -- process-wide budget accounting + kernel enforcement
// -----------------------------------------------------------------------------
class MemoryGuard {
public:
    static MemoryGuard& instance();

    // Applies the limits: installs rlimits and records the budgets.  Safe to
    // call more than once; later calls only tighten the envelope.
    Error initialise(const MemoryLimits& limits);
    Error initialise_from_hardware();          // uses detect_hardware() + defaults

    bool   initialised() const;
    const MemoryLimits& limits() const { return limits_; }
    const HardwareInfo& hardware() const { return hw_; }

    // ---- accounting -------------------------------------------------------
    // Returns false when the reservation would push usage past the budget.  The
    // caller is expected to free something (or fail the operation) instead of
    // proceeding, which keeps the agent inside the envelope by construction.
    bool reserve(uint64_t bytes, const char* what);
    void release(uint64_t bytes);

    bool reserve_vram(uint64_t bytes, const char* what);
    void release_vram(uint64_t bytes);

    uint64_t used_bytes() const;
    uint64_t peak_bytes() const;
    uint64_t budget_bytes() const;
    uint64_t vram_used_bytes() const;
    uint64_t vram_budget_bytes() const;
    uint64_t remaining_bytes() const;

    // Largest single allocation that can still succeed right now.
    uint64_t largest_reservation() const;

    // ---- reporting --------------------------------------------------------
    Json        report_json() const;
    std::string report_text() const;
    std::string one_line_summary() const;

    // Reads the kernel's view of our own peak/current RSS (Linux).
    uint64_t process_rss_bytes() const;
    uint64_t process_peak_rss_bytes() const;
    uint64_t system_available_bytes() const;

    // Re-applies the rlimits; used by the CLI when --max-ram is passed.
    Error enforce_rlimits(uint64_t address_space_bytes);

private:
    MemoryGuard() = default;
    MemoryGuard(const MemoryGuard&) = delete;
    MemoryGuard& operator=(const MemoryGuard&) = delete;

    Error recompute_defaults();

    mutable std::mutex   mu_;
    MemoryLimits         limits_{};
    HardwareInfo         hw_{};
    bool                 initialised_{false};
    bool                 rlimits_installed_{false};
    uint64_t             used_{0};
    uint64_t             peak_{0};
    uint64_t             vram_used_{0};
    uint64_t             rejected_reservations_{0};
    uint64_t             rlimit_bytes_{0};
};

// -----------------------------------------------------------------------------
// RAII helper: hold a reservation for the lifetime of a scope.
// -----------------------------------------------------------------------------
class MemoryReservation {
public:
    MemoryReservation(uint64_t bytes, const char* what)
        : bytes_(bytes), held_(MemoryGuard::instance().reserve(bytes, what)) {}
    ~MemoryReservation() { if (held_) MemoryGuard::instance().release(bytes_); }

    MemoryReservation(const MemoryReservation&) = delete;
    MemoryReservation& operator=(const MemoryReservation&) = delete;
    MemoryReservation(MemoryReservation&& other) noexcept
        : bytes_(other.bytes_), held_(other.held_) { other.held_ = false; }

    bool ok() const { return held_; }
    explicit operator bool() const { return held_; }

private:
    uint64_t bytes_;
    bool     held_;
};

// Convenience: the effective in-memory working budget for bulk operations.
// Chooses a conservative slice (default 25%) of the RAM budget so a single
// file dump can never consume the whole envelope.
uint64_t bulk_operation_budget(double fraction = 0.25);

}  // namespace lca

#endif  // LCA_MEM_H
