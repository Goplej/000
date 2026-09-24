// =============================================================================
//  lca/mem.cpp  --  hardware probe, rlimit enforcement, budget accounting
// =============================================================================
#include "lca/mem.h"
#include "lca/buf.h"

#include <cerrno>
#include <fstream>
#include <sstream>
#include <algorithm>

#if defined(__linux__)
#  include <sys/resource.h>
#  include <sys/sysinfo.h>
#  include <unistd.h>
#endif
#if defined(__APPLE__)
#  include <sys/resource.h>
#  include <sys/sysctl.h>
#  include <unistd.h>
#endif

namespace lca {
namespace {

// -----------------------------------------------------------------------------
// Small file helpers (kept local: mem.cpp must not depend on the file engine)
// -----------------------------------------------------------------------------
std::string read_text_file(const std::string& path, size_t max_bytes = 1u << 20) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::string out;
    out.resize(max_bytes);
    f.read(&out[0], std::streamsize(max_bytes));
    out.resize(size_t(f.gcount()));
    return out;
}

std::string first_line_of(const std::string& path) {
    std::string s = read_text_file(path, 4096);
    size_t nl = s.find('\n');
    if (nl != std::string::npos) s.resize(nl);
    return trim(s);
}

// Extracts a numeric field from /proc/meminfo-like text: "MemTotal:  16330812 kB".
bool meminfo_field(const std::string& text, const char* key, uint64_t* out_bytes) {
    size_t pos = text.find(key);
    if (pos == std::string::npos) return false;
    pos += std::strlen(key);
    while (pos < text.size() && (text[pos] == ':' || text[pos] == ' ' || text[pos] == '\t')) ++pos;
    uint64_t value = 0;
    bool any = false;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        value = value * 10 + uint64_t(text[pos] - '0');
        any = true;
        ++pos;
    }
    if (!any) return false;
    // Values in meminfo are in kB.
    while (pos < text.size() && text[pos] == ' ') ++pos;
    if (text.compare(pos, 2, "kB") == 0) *out_bytes = value * 1024ull;
    else                                  *out_bytes = value;
    return true;
}

// -----------------------------------------------------------------------------
// GPU probe: nvidia-smi first (RTX class), then amdgpu sysfs.
// -----------------------------------------------------------------------------
void probe_gpu(HardwareInfo* hw) {
#if defined(__linux__)
    // --- NVIDIA via nvidia-smi -------------------------------------------
    {
        const char* cmd = "nvidia-smi --query-gpu=name,memory.total,memory.free,driver_version "
                          "--format=csv,noheader,nounits 2>/dev/null";
        FILE* p = ::popen(cmd, "r");
        if (p) {
            char line[512];
            if (std::fgets(line, sizeof(line), p)) {
                std::string s = trim(line);
                auto parts = split(s, ',');
                if (parts.size() >= 3) {
                    hw->gpu_present = true;
                    hw->gpu_name = trim(parts[0]);
                    hw->vram_total_bytes = uint64_t(std::strtoull(trim(parts[1]).c_str(), nullptr, 10)) * 1024ull * 1024ull;
                    hw->vram_free_bytes = uint64_t(std::strtoull(trim(parts[2]).c_str(), nullptr, 10)) * 1024ull * 1024ull;
                    if (parts.size() >= 4) hw->gpu_driver = trim(parts[3]);
                }
            }
            ::pclose(p);
            if (hw->gpu_present) return;
        }
    }
    // --- AMD via sysfs ----------------------------------------------------
    for (int card = 0; card < 4; ++card) {
        std::string base = "/sys/class/drm/card" + std::to_string(card) + "/device/";
        std::string total = read_text_file(base + "mem_info_vram_total", 64);
        if (total.empty()) continue;
        uint64_t total_v = std::strtoull(trim(total).c_str(), nullptr, 10);
        if (total_v == 0) continue;
        hw->gpu_present = true;
        std::string free_v = read_text_file(base + "mem_info_vram_used", 64);
        hw->vram_total_bytes = total_v;
        hw->vram_free_bytes = free_v.empty() ? 0 : total_v - std::strtoull(trim(free_v).c_str(), nullptr, 10);
        std::string vendor = first_line_of(base + "uevent");
        hw->gpu_name = vendor.empty() ? ("DRM card " + std::to_string(card)) : vendor;
        return;
    }
#else
    (void)hw;
#endif
}

uint64_t clamp_u64(uint64_t v, uint64_t lo, uint64_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

}  // namespace

// -----------------------------------------------------------------------------
// HardwareInfo
// -----------------------------------------------------------------------------
std::string HardwareInfo::describe() const {
    std::string s;
    s += "cpu            : " + (cpu_model.empty() ? std::string("unknown") : cpu_model) + "\n";
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%d cores / %d threads\n", cpu_cores, cpu_threads);
    s += "topology       : "; s += buf;
    s += "os             : " + os_name + " (" + kernel + ")\n";
    s += "ram            : " + human_bytes(ram_total_bytes) + " total, " +
         human_bytes(ram_available_bytes) + " available\n";
    s += "swap           : " + human_bytes(swap_total_bytes) + "\n";
    if (gpu_present) {
        s += "gpu            : " + gpu_name + "\n";
        s += "vram           : " + human_bytes(vram_total_bytes) + " total, " +
             human_bytes(vram_free_bytes) + " free\n";
        if (!gpu_driver.empty()) s += "gpu driver     : " + gpu_driver + "\n";
    } else {
        s += "gpu            : none detected\n";
    }
    return s;
}

Json HardwareInfo::to_json() const {
    Json j = Json::object();
    j["cpu_model"]   = cpu_model;
    j["cpu_cores"]   = int64_t(cpu_cores);
    j["cpu_threads"] = int64_t(cpu_threads);
    j["ram_total"]   = int64_t(ram_total_bytes);
    j["ram_free"]    = int64_t(ram_available_bytes);
    j["swap_total"]  = int64_t(swap_total_bytes);
    j["os"]          = os_name;
    j["kernel"]      = kernel;
    j["gpu"]         = gpu_name;
    j["gpu_present"] = gpu_present;
    j["vram_total"]  = int64_t(vram_total_bytes);
    j["vram_free"]   = int64_t(vram_free_bytes);
    j["gpu_driver"]  = gpu_driver;
    return j;
}

HardwareInfo detect_hardware() {
    HardwareInfo hw;

#if defined(__linux__)
    // --- CPU --------------------------------------------------------------
    {
        std::string cpuinfo = read_text_file("/proc/cpuinfo", 1u << 20);
        auto lines = split_lines(cpuinfo);
        int threads = 0;
        for (const std::string& line : lines) {
            if (starts_with(line, "processor")) ++threads;
            if (hw.cpu_model.empty() && starts_with(line, "model name")) {
                size_t colon = line.find(':');
                if (colon != std::string::npos) hw.cpu_model = trim(line.substr(colon + 1));
            }
            if (hw.cpu_model.empty() && starts_with(line, "Model")) {
                size_t colon = line.find(':');
                if (colon != std::string::npos) hw.cpu_model = trim(line.substr(colon + 1));
            }
        }
        hw.cpu_threads = threads;
    }
    // --- memory -----------------------------------------------------------
    {
        std::string meminfo = read_text_file("/proc/meminfo", 1u << 18);
        meminfo_field(meminfo, "MemTotal", &hw.ram_total_bytes);
        meminfo_field(meminfo, "MemAvailable", &hw.ram_available_bytes);
        if (hw.ram_available_bytes == 0) meminfo_field(meminfo, "MemFree", &hw.ram_available_bytes);
        meminfo_field(meminfo, "SwapTotal", &hw.swap_total_bytes);
    }
    {
        struct sysinfo si {};
        if (::sysinfo(&si) == 0 && hw.ram_total_bytes == 0) {
            hw.ram_total_bytes = uint64_t(si.totalram) * uint64_t(si.mem_unit);
            hw.ram_available_bytes = uint64_t(si.freeram) * uint64_t(si.mem_unit);
        }
    }
    // --- os ---------------------------------------------------------------
    hw.os_name = first_line_of("/etc/os-release");
    if (starts_with(hw.os_name, "PRETTY_NAME=")) {
        hw.os_name = hw.os_name.substr(std::strlen("PRETTY_NAME="));
        if (!hw.os_name.empty() && hw.os_name.front() == '"') hw.os_name.erase(0, 1);
        if (!hw.os_name.empty() && hw.os_name.back() == '"')  hw.os_name.pop_back();
    }
    if (hw.os_name.empty()) hw.os_name = "Linux";
    hw.kernel = "kernel " + first_line_of("/proc/sys/kernel/osrelease");
    if (hw.kernel == "kernel ") hw.kernel = "kernel unknown";
#endif

#if defined(__APPLE__)
    {
        size_t len = sizeof(uint64_t);
        uint64_t mem = 0;
        if (::sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) {
            hw.ram_total_bytes = mem;
            hw.ram_available_bytes = mem;
        }
        char model[256] = {0};
        len = sizeof(model);
        if (::sysctlbyname("machdep.cpu.brand_string", model, &len, nullptr, 0) == 0) hw.cpu_model = model;
        hw.os_name = "macOS";
        hw.kernel = "darwin";
    }
#endif

    // --- threads / cores ---------------------------------------------------
    long nproc = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc > 0) {
        if (hw.cpu_threads == 0) hw.cpu_threads = int(nproc);
        if (hw.cpu_cores == 0)   hw.cpu_cores   = int(nproc);
    }
#if defined(__linux__)
    if (hw.cpu_cores > 0) {
        // Distinguish physical cores from SMT threads when the topology is exposed.
        std::string topo = read_text_file("/sys/devices/system/cpu/cpu0/topology/thread_siblings_list", 64);
        size_t siblings = topo.empty() ? 0 : std::count(topo.begin(), topo.end(), ',') + 1;
        if (siblings > 1 && hw.cpu_threads > 0 && int(siblings) <= hw.cpu_threads)
            hw.cpu_cores = hw.cpu_threads / int(siblings);
    }
#endif
    if (hw.cpu_threads == 0) hw.cpu_threads = 1;
    if (hw.cpu_cores == 0)   hw.cpu_cores = hw.cpu_threads;
    if (hw.cpu_model.empty()) hw.cpu_model = "unknown CPU";

    probe_gpu(&hw);
    return hw;
}

// -----------------------------------------------------------------------------
// MemoryGuard
// -----------------------------------------------------------------------------
MemoryGuard& MemoryGuard::instance() {
    static MemoryGuard guard;
    return guard;
}

Error MemoryGuard::initialise(const MemoryLimits& limits) {
    uint64_t target = 0;
    bool     install = false;
    {
        // Note: enforce_rlimits() takes the same mutex, so the lock must be
        // released before it is called (std::mutex is not recursive).
        std::lock_guard<std::mutex> lock(mu_);
        limits_ = limits;
        if (!initialised_) {
            hw_ = detect_hardware();
            initialised_ = true;
        }
        Error e = recompute_defaults();
        if (!e.ok()) return e;
        target = rlimit_bytes_;
        install = limits_.install_rlimits;
    }
    if (install) return enforce_rlimits(target);
    return {};
}

Error MemoryGuard::initialise_from_hardware() {
    MemoryLimits limits;   // defaults already respect the 8 GB / 4 GB envelope
    MemoryGuard& g = instance();
    {
        std::lock_guard<std::mutex> lock(g.mu_);
        if (!g.initialised_) {
            g.hw_ = detect_hardware();
            g.initialised_ = true;
        }
    }
    return g.initialise(limits);
}

// The hard caps are deliberately conservative and strictly inside the physical
// envelope: RAM stays below 8 GiB, VRAM below 4 GiB, whatever the host reports.
Error MemoryGuard::recompute_defaults() {
    // --- RAM -------------------------------------------------------------
    const uint64_t kEightGiB   = 8ull * 1024 * 1024 * 1024;
    const uint64_t kVramFourGiB = 4ull * 1024 * 1024 * 1024;

    uint64_t budget = limits_.ram_cap_bytes;
    budget = clamp_u64(budget, 256ull * 1024 * 1024, kEightGiB - (64ull * 1024 * 1024));

    if (hw_.ram_total_bytes > 0) {
        uint64_t from_host = uint64_t(double(hw_.ram_total_bytes) * limits_.safety_fraction);
        // Never plan to use more than what the host reports as installed.
        if (from_host < budget) budget = from_host;
    }
    // Keep a slice of the system free so the shell, editor and compiler the
    // agent drives still have room to run.
    if (hw_.ram_available_bytes > 0) {
        uint64_t usable = hw_.ram_available_bytes + (hw_.ram_available_bytes / 4);
        if (usable < budget) budget = usable;
    }
    budget = clamp_u64(budget, 128ull * 1024 * 1024, kEightGiB - (64ull * 1024 * 1024));
    limits_.ram_cap_bytes = budget;

    // --- VRAM ------------------------------------------------------------
    uint64_t vram = limits_.vram_cap_bytes;
    vram = clamp_u64(vram, 64ull * 1024 * 1024, kVramFourGiB - (128ull * 1024 * 1024));
    if (hw_.vram_total_bytes > 0) {
        uint64_t from_host = uint64_t(double(hw_.vram_total_bytes) * limits_.safety_fraction);
        if (from_host < vram) vram = from_host;
    }
    limits_.vram_cap_bytes = clamp_u64(vram, 64ull * 1024 * 1024, kVramFourGiB - (128ull * 1024 * 1024));

    // rlimit address space: the RAM budget, but never below the amount already
    // committed (the kernel refuses to shrink an rlimit below current usage).
#if defined(__linux__)
    uint64_t rss = process_rss_bytes();
    uint64_t want = limits_.ram_cap_bytes;
    if (limits_.reserve_transient_headroom) want = want + (want / 8);   // allocator slack
    want = clamp_u64(want, rss + (64ull * 1024 * 1024), kEightGiB - (64ull * 1024 * 1024));
    rlimit_bytes_ = want;
#else
    rlimit_bytes_ = limits_.ram_cap_bytes;
#endif
    return {};
}

bool MemoryGuard::initialised() const {
    std::lock_guard<std::mutex> lock(mu_);
    return initialised_;
}

bool MemoryGuard::reserve(uint64_t bytes, const char* what) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!initialised_) return true;                 // accounting disabled
    if (limits_.ram_cap_bytes == 0) return true;
    if (bytes > limits_.ram_cap_bytes) {
        ++rejected_reservations_;
        return false;
    }
    if (used_ + bytes > limits_.ram_cap_bytes) {
        ++rejected_reservations_;
        return false;
    }
    used_ += bytes;
    if (used_ > peak_) peak_ = used_;
    (void)what;   // reserved for future per-category attribution
    return true;
}

void MemoryGuard::release(uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    used_ = bytes > used_ ? 0 : used_ - bytes;
}

bool MemoryGuard::reserve_vram(uint64_t bytes, const char* what) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!initialised_) return true;
    if (limits_.vram_cap_bytes == 0 || bytes > limits_.vram_cap_bytes) return false;
    if (vram_used_ + bytes > limits_.vram_cap_bytes) return false;
    vram_used_ += bytes;
    (void)what;
    return true;
}

void MemoryGuard::release_vram(uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    vram_used_ = bytes > vram_used_ ? 0 : vram_used_ - bytes;
}

uint64_t MemoryGuard::used_bytes() const       { std::lock_guard<std::mutex> l(mu_); return used_; }
uint64_t MemoryGuard::peak_bytes() const       { std::lock_guard<std::mutex> l(mu_); return peak_; }
uint64_t MemoryGuard::budget_bytes() const     { std::lock_guard<std::mutex> l(mu_); return limits_.ram_cap_bytes; }
uint64_t MemoryGuard::vram_used_bytes() const  { std::lock_guard<std::mutex> l(mu_); return vram_used_; }
uint64_t MemoryGuard::vram_budget_bytes() const{ std::lock_guard<std::mutex> l(mu_); return limits_.vram_cap_bytes; }

uint64_t MemoryGuard::remaining_bytes() const {
    std::lock_guard<std::mutex> l(mu_);
    return limits_.ram_cap_bytes > used_ ? limits_.ram_cap_bytes - used_ : 0;
}

uint64_t MemoryGuard::largest_reservation() const {
    std::lock_guard<std::mutex> l(mu_);
    // Keep a little room for bookkeeping structures of the caller.
    uint64_t free_bytes = limits_.ram_cap_bytes > used_ ? limits_.ram_cap_bytes - used_ : 0;
    return free_bytes > (1u << 20) ? free_bytes - (1u << 20) : 0;
}

Json MemoryGuard::report_json() const {
    std::lock_guard<std::mutex> l(mu_);
    Json j = Json::object();
    j["hardware"]         = hw_.to_json();
    j["ram_budget"]       = int64_t(limits_.ram_cap_bytes);
    j["ram_used"]         = int64_t(used_);
    j["ram_peak"]         = int64_t(peak_);
    j["vram_budget"]      = int64_t(limits_.vram_cap_bytes);
    j["vram_used"]        = int64_t(vram_used_);
    j["rlimit_address_space"] = int64_t(rlimit_bytes_);
    j["rejected"]         = int64_t(rejected_reservations_);
    j["within_limits"]    = (limits_.ram_cap_bytes < (8ull * 1024 * 1024 * 1024)) &&
                            (limits_.vram_cap_bytes < (4ull * 1024 * 1024 * 1024));
    return j;
}

std::string MemoryGuard::report_text() const {
    std::lock_guard<std::mutex> l(mu_);
    std::string s;
    s += "hardware\n--------\n" + hw_.describe();
    s += "\nbudgets\n-------\n";
    s += "ram budget     : " + human_bytes(limits_.ram_cap_bytes) + "\n";
    s += "ram reserved   : " + human_bytes(used_) + " (peak " + human_bytes(peak_) + ")\n";
    s += "vram budget    : " + human_bytes(limits_.vram_cap_bytes) + "\n";
    s += "vram reserved  : " + human_bytes(vram_used_) + "\n";
    s += "rlimit AS      : " + (rlimit_bytes_ ? human_bytes(rlimit_bytes_) : std::string("not applied")) + "\n";
    s += "process rss    : " + human_bytes(process_rss_bytes()) +
         " (peak " + human_bytes(process_peak_rss_bytes()) + ")\n";
    if (rejected_reservations_)
        s += "rejected alloc : " + std::to_string(rejected_reservations_) + "\n";
    s += "within <8GB/<4GB envelope: ";
    s += (limits_.ram_cap_bytes < (8ull * 1024 * 1024 * 1024) &&
          limits_.vram_cap_bytes < (4ull * 1024 * 1024 * 1024)) ? "yes" : "NO";
    s += "\n";
    return s;
}

std::string MemoryGuard::one_line_summary() const {
    std::lock_guard<std::mutex> l(mu_);
    std::string s = "ram " + human_bytes(used_) + "/" + human_bytes(limits_.ram_cap_bytes);
    s += ", vram " + human_bytes(vram_used_) + "/" + human_bytes(limits_.vram_cap_bytes);
    s += ", rss " + human_bytes(process_rss_bytes());
    return s;
}

uint64_t MemoryGuard::process_rss_bytes() const {
#if defined(__linux__)
    std::string statm = read_text_file("/proc/self/statm", 256);
    std::istringstream is(statm);
    uint64_t total_pages = 0, resident_pages = 0;
    if (is >> total_pages >> resident_pages) {
        long page = ::sysconf(_SC_PAGESIZE);
        if (page <= 0) page = 4096;
        (void)total_pages;
        return resident_pages * uint64_t(page);
    }
#endif
    return 0;
}

uint64_t MemoryGuard::process_peak_rss_bytes() const {
#if defined(__linux__)
    std::string status = read_text_file("/proc/self/status", 1u << 16);
    size_t pos = status.find("VmHWM:");
    if (pos != std::string::npos) {
        uint64_t kb = std::strtoull(status.c_str() + pos + 6, nullptr, 10);
        return kb * 1024ull;
    }
#endif
    return 0;
}

uint64_t MemoryGuard::system_available_bytes() const {
#if defined(__linux__)
    std::string meminfo = read_text_file("/proc/meminfo", 1u << 18);
    uint64_t avail = 0;
    if (meminfo_field(meminfo, "MemAvailable", &avail) && avail) return avail;
    uint64_t total = 0, caches = 0;
    meminfo_field(meminfo, "MemFree", &avail);
    meminfo_field(meminfo, "Cached", &caches);
    meminfo_field(meminfo, "MemTotal", &total);
    (void)total;
    return avail + caches;
#else
    std::lock_guard<std::mutex> l(mu_);
    return hw_.ram_available_bytes;
#endif
}

Error MemoryGuard::enforce_rlimits(uint64_t address_space_bytes) {
#if defined(__linux__) || defined(__APPLE__)
    if (address_space_bytes == 0) return {};
    // Never lower an already-lower limit; only tighten.
    struct rlimit current {};
    if (::getrlimit(RLIMIT_AS, &current) == 0) {
        if (current.rlim_cur != RLIM_INFINITY && uint64_t(current.rlim_cur) <= address_space_bytes)
            address_space_bytes = uint64_t(current.rlim_cur);
    }
    struct rlimit want {};
    want.rlim_cur = rlim_t(address_space_bytes);
    want.rlim_max = rlim_t(address_space_bytes);
    if (::setrlimit(RLIMIT_AS, &want) != 0) {
        return LCA_FAIL(Code::ResourceExhausted,
                        std::string("setrlimit(RLIMIT_AS) failed: ") + std::strerror(errno));
    }
#  if defined(__linux__)
    // RLIMIT_DATA bounds brk/mmap-anonymous growth on Linux.
    struct rlimit want_data {};
    want_data.rlim_cur = rlim_t(address_space_bytes);
    want_data.rlim_max = rlim_t(address_space_bytes);
    if (::setrlimit(RLIMIT_DATA, &want_data) != 0) {
        // A failure here is not fatal: RLIMIT_AS is already in force.
        LCA_LOG_WARN("mem", std::string("setrlimit(RLIMIT_DATA) failed: ") + std::strerror(errno));
    }
#  endif
    {
        std::lock_guard<std::mutex> l(mu_);
        rlimits_installed_ = true;
        rlimit_bytes_ = address_space_bytes;
    }
    return {};
#else
    (void)address_space_bytes;
    return {};
#endif
}

uint64_t bulk_operation_budget(double fraction) {
    MemoryGuard& g = MemoryGuard::instance();
    uint64_t budget = g.budget_bytes();
    uint64_t portion = uint64_t(double(budget) * (fraction <= 0.0 ? 0.25 : fraction));
    // At least 4 MiB so small fixtures always work, at most 1 GiB so the agent
    // can never eat the envelope in one read.
    return clamp_u64(portion, 4ull * 1024 * 1024, 1024ull * 1024 * 1024);
}

}  // namespace lca
