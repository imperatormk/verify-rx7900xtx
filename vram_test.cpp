// VRAM test for the RX 7900 XTX.
//
// Works when host RAM is smaller than VRAM. The host only holds a staging
// chunk. Each 8-byte word is a function of the seed and the word index, so the
// expected bytes can be rebuilt on readback and compared directly.
//
// After the host copy check, the first half of the allocation is copied onto
// the second half with hipMemcpyDeviceToDevice and checked the same way.

#include <hip/hip_runtime_api.h>
#include <hip/hip_version.h>

#include <sys/random.h>
#include <unistd.h>

#include <dirent.h>

#include <sys/utsname.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr size_t kAlign = 2ull << 20;             // 2 MiB
constexpr size_t kPreferBlock = 1ull << 30;       // 1 GiB
constexpr size_t kMinBlock = 2ull << 20;          // 2 MiB
constexpr size_t kTargetLeftover = 64ull << 20;   // keep allocating down to this
constexpr size_t kMaxLeftover = 256ull << 20;     // fail if more than this remains free
constexpr size_t kMaxChunk = 1ull << 30;          // 1 GiB
constexpr int kDefaultRounds = 10;
constexpr size_t kDefaultChunkMiB = 256;

struct Block {
    void* ptr = nullptr;
    size_t bytes = 0;
    uint64_t base = 0;
};

struct Mismatch {
    bool found = false;
    uint64_t offset = 0;
    uint64_t expected = 0;
    uint64_t actual = 0;
};

struct CheckHit {
    bool hit = false;
    size_t index = 0;
    uint64_t expected = 0;
    uint64_t actual = 0;
};

struct SysfsVram {
    bool ok = false;
    uint64_t used = 0;
    uint64_t total = 0;
    // CPU-visible part of VRAM (mem_info_vis_vram_total): the PCI BAR window. It is smaller than
    // total when Resizable BAR is off. The copy test below does not need it, so it cannot fail on it.
    bool have_visible = false;
    uint64_t visible = 0;
};

// SplitMix64 finalizer. The word depends only on the seed and the word index.
uint64_t pattern_word(uint64_t seed, uint64_t word_index) {
    uint64_t z = seed + word_index;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

std::string format_bytes(uint64_t n) {
    char buf[64];
    const double gib = 1024.0 * 1024.0 * 1024.0;
    const double mib = 1024.0 * 1024.0;
    if (n >= static_cast<uint64_t>(gib)) {
        std::snprintf(buf, sizeof buf, "%.2f GiB", static_cast<double>(n) / gib);
    } else {
        std::snprintf(buf, sizeof buf, "%.2f MiB", static_cast<double>(n) / mib);
    }
    return buf;
}

std::string lower_compact(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == ' ' || c == '-' || c == '_') {
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

bool iequals(const std::string& a, const std::string& b) {
    return lower_compact(a) == lower_compact(b);
}

bool is_card_node(const std::string& name) {
    if (name.size() < 5 || name.compare(0, 4, "card") != 0) {
        return false;
    }
    for (size_t i = 4; i < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
            return false;
        }
    }
    return true;
}

bool parse_u64(const char* s, uint64_t* out) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return false;
    }
    *out = static_cast<uint64_t>(v);
    return true;
}

bool read_u64_file(const std::string& path, uint64_t* out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    uint64_t v = 0;
    in >> v;
    if (!in) {
        return false;
    }
    *out = v;
    return true;
}

bool read_meminfo_kb(const char* key, uint64_t* kb) {
    std::ifstream in("/proc/meminfo");
    if (!in) {
        return false;
    }
    std::string k;
    uint64_t v = 0;
    while (in >> k >> v) {
        if (k == key) {
            *kb = v;
            return true;
        }
        in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return false;
}

void hip_clear_error() {
    (void)hipGetLastError();
}

bool hip_ok(hipError_t err, const char* what, std::string* detail = nullptr) {
    if (err == hipSuccess) {
        return true;
    }
    const char* msg = hipGetErrorString(err);
    std::fprintf(stderr, "FAIL: %s: %s\n", what, msg);
    if (detail != nullptr) {
        *detail = std::string(what) + ": " + msg;
    }
    hip_clear_error();
    return false;
}

bool random_seed(uint64_t* out) {
    uint64_t seed = 0;
    ssize_t n = getrandom(&seed, sizeof seed, 0);
    if (n == static_cast<ssize_t>(sizeof seed)) {
        *out = seed;
        return true;
    }
    std::ifstream in("/dev/urandom", std::ios::binary);
    in.read(reinterpret_cast<char*>(&seed), sizeof seed);
    if (!in) {
        std::fprintf(stderr, "FAIL: could not read a random seed\n");
        return false;
    }
    *out = seed;
    return true;
}

unsigned thread_count(size_t words) {
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) {
        n = 1;
    }
    if (n > 12) {
        n = 12;
    }
    if (words < 64 * 1024 || n > words) {
        return 1;
    }
    return n;
}

void parallel_fill(uint64_t* data, size_t words, uint64_t seed, uint64_t base_word) {
    const unsigned n = thread_count(words);
    if (n == 1) {
        for (size_t i = 0; i < words; ++i) {
            data[i] = pattern_word(seed, base_word + i);
        }
        return;
    }
    std::vector<std::thread> threads;
    threads.reserve(n);
    const size_t span = (words + n - 1) / n;
    for (unsigned t = 0; t < n; ++t) {
        const size_t begin = static_cast<size_t>(t) * span;
        if (begin >= words) {
            break;
        }
        const size_t end = std::min(words, begin + span);
        threads.emplace_back([data, begin, end, seed, base_word]() {
            for (size_t i = begin; i < end; ++i) {
                data[i] = pattern_word(seed, base_word + i);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
}

std::optional<CheckHit> parallel_check(uint64_t* data, size_t words, uint64_t seed, uint64_t base_word) {
    const unsigned n = thread_count(words);
    std::vector<CheckHit> hits(n);
    if (n == 1) {
        for (size_t i = 0; i < words; ++i) {
            const uint64_t expected = pattern_word(seed, base_word + i);
            const uint64_t actual = data[i];
            if (expected != actual) {
                CheckHit hit;
                hit.hit = true;
                hit.index = i;
                hit.expected = expected;
                hit.actual = actual;
                return hit;
            }
        }
        return std::nullopt;
    }
    std::vector<std::thread> threads;
    threads.reserve(n);
    const size_t span = (words + n - 1) / n;
    for (unsigned t = 0; t < n; ++t) {
        const size_t begin = static_cast<size_t>(t) * span;
        if (begin >= words) {
            break;
        }
        const size_t end = std::min(words, begin + span);
        threads.emplace_back([data, begin, end, seed, base_word, hit = &hits[t]]() {
            for (size_t i = begin; i < end; ++i) {
                const uint64_t expected = pattern_word(seed, base_word + i);
                const uint64_t actual = data[i];
                if (expected != actual) {
                    hit->hit = true;
                    hit->index = i;
                    hit->expected = expected;
                    hit->actual = actual;
                    return;
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    std::optional<CheckHit> best;
    for (const CheckHit& hit : hits) {
        if (!hit.hit) {
            continue;
        }
        if (!best || hit.index < best->index) {
            best = hit;
        }
    }
    return best;
}

bool is_rx7900xtx(const hipDeviceProp_t& prop) {
    return lower_compact(prop.name).find("7900xtx") != std::string::npos;
}

int find_device() {
    int count = 0;
    if (!hip_ok(hipGetDeviceCount(&count), "hipGetDeviceCount")) {
        return -1;
    }
    if (count < 1) {
        std::fprintf(stderr, "FAIL: no HIP device\n");
        return -1;
    }
    int found = -1;
    for (int i = 0; i < count; ++i) {
        hipDeviceProp_t prop{};
        if (hipGetDeviceProperties(&prop, i) != hipSuccess) {
            hip_clear_error();
            continue;
        }
        std::printf("found: gpu %d  %s  (%s)  VRAM %s\n",
                    i,
                    prop.name,
                    prop.gcnArchName,
                    format_bytes(prop.totalGlobalMem).c_str());
        if (found < 0 && is_rx7900xtx(prop)) {
            found = i;
        }
    }
    if (found < 0) {
        std::fprintf(stderr, "FAIL: RX 7900 XTX not found\n");
    }
    return found;
}

std::string pci_bus_id(int device) {
    char bus[64] = {};
    if (hipDeviceGetPCIBusId(bus, static_cast<int>(sizeof bus), device) != hipSuccess) {
        hip_clear_error();
        return {};
    }
    return bus;
}

std::string sysfs_device_path(const std::string& pci) {
    if (pci.empty()) {
        return {};
    }
    DIR* dir = opendir("/sys/class/drm");
    if (dir == nullptr) {
        return {};
    }
    std::string found;
    while (dirent* de = readdir(dir)) {
        if (!is_card_node(de->d_name)) {
            continue;
        }
        const std::string uevent_path = std::string("/sys/class/drm/") + de->d_name + "/device/uevent";
        std::ifstream in(uevent_path);
        std::string line;
        while (std::getline(in, line)) {
            const std::string key = "PCI_SLOT_NAME=";
            if (line.compare(0, key.size(), key) == 0 && iequals(line.substr(key.size()), pci)) {
                found = std::string("/sys/class/drm/") + de->d_name + "/device";
                break;
            }
        }
        if (!found.empty()) {
            break;
        }
    }
    closedir(dir);
    return found;
}

struct TempReading {
    std::string name;
    int64_t millicelsius = 0;
    bool have_critical = false;
    int64_t critical_millicelsius = 0;
    bool have_emergency = false;
    int64_t emergency_millicelsius = 0;
};

bool read_text_line(const std::string& path, std::string* out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    if (!std::getline(in, line)) {
        return false;
    }
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
        line.pop_back();
    }
    if (line.empty()) {
        return false;
    }
    *out = line;
    return true;
}

// hwmonN is not stable across boots. Find the amdgpu node under this device.
std::string find_amdgpu_hwmon(const std::string& device_path) {
    if (device_path.empty()) {
        return {};
    }
    const std::string dir_path = device_path + "/hwmon";
    DIR* dir = opendir(dir_path.c_str());
    if (dir == nullptr) {
        return {};
    }
    std::string found;
    while (dirent* de = readdir(dir)) {
        const std::string name = de->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        const std::string node = dir_path + "/" + name;
        std::string kind;
        if (read_text_line(node + "/name", &kind) && kind == "amdgpu") {
            found = node;
            break;
        }
    }
    closedir(dir);
    return found;
}

std::vector<TempReading> read_temperatures(const std::string& hwmon_path) {
    std::vector<TempReading> out;
    if (hwmon_path.empty()) {
        return out;
    }
    for (int index = 1; index <= 32; ++index) {
        const std::string stem = hwmon_path + "/temp" + std::to_string(index);
        uint64_t milli = 0;
        if (!read_u64_file(stem + "_input", &milli)) {
            break;
        }
        TempReading reading;
        if (!read_text_line(stem + "_label", &reading.name)) {
            reading.name = "temp" + std::to_string(index);
        }
        reading.millicelsius = static_cast<int64_t>(milli);
        uint64_t limit = 0;
        if (read_u64_file(stem + "_crit", &limit)) {
            reading.have_critical = true;
            reading.critical_millicelsius = static_cast<int64_t>(limit);
        }
        if (read_u64_file(stem + "_emergency", &limit)) {
            reading.have_emergency = true;
            reading.emergency_millicelsius = static_cast<int64_t>(limit);
        }
        out.push_back(std::move(reading));
    }
    return out;
}

std::string format_temp_c(int64_t millicelsius) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f C", static_cast<double>(millicelsius) / 1000.0);
    return buf;
}

std::string format_temp_list(const std::vector<TempReading>& readings) {
    std::string out;
    for (const TempReading& reading : readings) {
        if (!out.empty()) {
            out += "   ";
        }
        out += reading.name;
        out += " ";
        out += format_temp_c(reading.millicelsius);
    }
    return out;
}

bool read_sysfs_vram(const std::string& device_path, SysfsVram* out) {
    out->ok = false;
    if (device_path.empty()) {
        return false;
    }
    uint64_t used = 0;
    uint64_t total = 0;
    if (!read_u64_file(device_path + "/mem_info_vram_used", &used) ||
        !read_u64_file(device_path + "/mem_info_vram_total", &total)) {
        return false;
    }
    out->ok = true;
    out->used = used;
    out->total = total;
    uint64_t visible = 0;
    if (read_u64_file(device_path + "/mem_info_vis_vram_total", &visible)) {
        out->have_visible = true;
        out->visible = visible;
    }
    return true;
}

// Size of PCI BAR 0, the GPU's VRAM aperture, from the first line of <device>/resource
// ("start end flags" in hex). 256 MiB means Resizable BAR is not active.
bool read_bar0_bytes(const std::string& device_path, uint64_t* out) {
    if (device_path.empty()) {
        return false;
    }
    std::ifstream in(device_path + "/resource");
    std::string start_text;
    std::string end_text;
    if (!(in >> start_text >> end_text)) {
        return false;
    }
    const uint64_t start = std::strtoull(start_text.c_str(), nullptr, 16);
    const uint64_t end = std::strtoull(end_text.c_str(), nullptr, 16);
    if (end <= start) {
        return false;
    }
    *out = end - start + 1;
    return true;
}

void* try_hip_malloc(size_t bytes) {
    void* ptr = nullptr;
    hipError_t err = hipMalloc(&ptr, bytes);
    if (err != hipSuccess) {
        hip_clear_error();
        return nullptr;
    }
    return ptr;
}

// Try to allocate the request, shrinking by 64 MiB after each failure.
bool alloc_with_shrink(size_t want, void** ptr, size_t* got) {
    if (want < kMinBlock) {
        return false;
    }
    want &= ~(kAlign - 1);
    while (want >= kMinBlock) {
        void* p = try_hip_malloc(want);
        if (p != nullptr) {
            *ptr = p;
            *got = want;
            return true;
        }
        if (want <= (64ull << 20)) {
            break;
        }
        want = (want - (64ull << 20)) & ~(kAlign - 1);
    }
    return false;
}

bool allocate_vram(std::vector<Block>* blocks, size_t* allocated, size_t* free_after, size_t* total) {
    uint64_t base = 0;
    size_t prev_free = std::numeric_limits<size_t>::max();
    int stalls = 0;
    for (int guard = 0; guard < 100000; ++guard) {
        size_t free_b = 0;
        size_t total_b = 0;
        if (!hip_ok(hipMemGetInfo(&free_b, &total_b), "hipMemGetInfo")) {
            return false;
        }
        *total = total_b;
        *free_after = free_b;
        *allocated = static_cast<size_t>(base);
        if (free_b <= kTargetLeftover) {
            return true;
        }
        if (free_b >= prev_free) {
            if (++stalls >= 2) {
                return free_b <= kMaxLeftover;
            }
        } else {
            stalls = 0;
        }
        prev_free = free_b;

        size_t aligned = free_b & ~(kAlign - 1);
        if (aligned < kMinBlock) {
            return free_b <= kMaxLeftover;
        }
        size_t want = std::min(aligned, kPreferBlock);
        void* ptr = nullptr;
        size_t got = 0;
        if (!alloc_with_shrink(want, &ptr, &got)) {
            return free_b <= kMaxLeftover;
        }
        blocks->push_back(Block{ptr, got, base});
        base += got;
    }
    return false;
}

void free_blocks(std::vector<Block>* blocks) {
    for (Block& block : *blocks) {
        if (block.ptr != nullptr) {
            (void)hipFree(block.ptr);
            block.ptr = nullptr;
        }
    }
    blocks->clear();
    hip_clear_error();
}

void print_progress(const char* phase, uint64_t done, uint64_t total) {
    static const char* active = nullptr;
    static uint64_t last = 0;
    if (active != phase) {
        active = phase;
        last = 0;
    }
    const bool tty = ::isatty(STDOUT_FILENO) == 1;
    const uint64_t step = 4ull << 30;
    const bool boundary = (done / step) != (last / step);
    if (tty) {
        std::printf("\r  %s  %s / %s", phase, format_bytes(done).c_str(), format_bytes(total).c_str());
        if (done >= total) {
            std::printf("\n");
        }
        std::fflush(stdout);
    } else if (done == 0 || done >= total || boundary) {
        std::printf("  %s  %s / %s\n", phase, format_bytes(done).c_str(), format_bytes(total).c_str());
    }
    last = done;
}

bool move_chunk(bool to_device, void* device_at, uint8_t* host, size_t nbytes) {
    hipError_t err = to_device
                         ? hipMemcpy(device_at, host, nbytes, hipMemcpyHostToDevice)
                         : hipMemcpy(host, device_at, nbytes, hipMemcpyDeviceToHost);
    if (err != hipSuccess) {
        std::fprintf(stderr,
                     "\nFAIL: hipMemcpy (%s, %s): %s\n",
                     to_device ? "H2D" : "D2H",
                     format_bytes(nbytes).c_str(),
                     hipGetErrorString(err));
        hip_clear_error();
        return false;
    }
    return true;
}

bool transfer_and_check(const std::vector<Block>& blocks,
                        uint64_t seed,
                        uint8_t* host,
                        size_t chunk,
                        bool to_device,
                        Mismatch* mismatch) {
    uint64_t total = 0;
    for (const Block& block : blocks) {
        total += block.bytes;
    }
    uint64_t done = 0;
    const char* phase = to_device ? "host -> VRAM" : "VRAM -> host";
    print_progress(phase, 0, total);
    for (const Block& block : blocks) {
        auto* words = reinterpret_cast<uint64_t*>(host);
        for (size_t off = 0; off < block.bytes; off += chunk) {
            const size_t nbytes = std::min(chunk, block.bytes - off);
            const size_t nwords = nbytes / sizeof(uint64_t);
            const uint64_t base_word = (block.base + off) / sizeof(uint64_t);
            auto* device_at = static_cast<uint8_t*>(block.ptr) + off;
            if (to_device) {
                parallel_fill(words, nwords, seed, base_word);
                if (!move_chunk(true, device_at, host, nbytes)) {
                    return false;
                }
            } else {
                std::memset(host, 0, nbytes);
                if (!move_chunk(false, device_at, host, nbytes)) {
                    return false;
                }
                if (auto hit = parallel_check(words, nwords, seed, base_word)) {
                    mismatch->found = true;
                    mismatch->offset = block.base + static_cast<uint64_t>(off) +
                                       static_cast<uint64_t>(hit->index) * sizeof(uint64_t);
                    mismatch->expected = hit->expected;
                    mismatch->actual = hit->actual;
                    std::printf("\n");
                    return false;
                }
            }
            done += nbytes;
            print_progress(phase, done, total);
        }
    }
    return true;
}

bool locate_span(const std::vector<Block>& blocks, uint64_t logical, uint8_t** ptr, size_t* room) {
    for (const Block& block : blocks) {
        const uint64_t end = block.base + block.bytes;
        if (logical >= block.base && logical < end) {
            const size_t off = static_cast<size_t>(logical - block.base);
            *ptr = static_cast<uint8_t*>(block.ptr) + off;
            *room = block.bytes - off;
            return true;
        }
    }
    return false;
}

bool zero_device_range(const std::vector<Block>& blocks, uint64_t begin, uint64_t end) {
    const uint64_t total = end - begin;
    uint64_t done = 0;
    print_progress("clear VRAM dst", 0, total);
    while (done < total) {
        uint8_t* ptr = nullptr;
        size_t room = 0;
        if (!locate_span(blocks, begin + done, &ptr, &room)) {
            std::fprintf(stderr, "\nFAIL: clear range is outside the allocation\n");
            return false;
        }
        size_t nbytes = room;
        if (static_cast<uint64_t>(nbytes) > total - done) {
            nbytes = static_cast<size_t>(total - done);
        }
        if (nbytes == 0) {
            std::fprintf(stderr, "\nFAIL: clear range made no progress\n");
            return false;
        }
        hipError_t err = hipMemset(ptr, 0, nbytes);
        if (err != hipSuccess) {
            std::fprintf(stderr, "\nFAIL: hipMemset (%s): %s\n", format_bytes(nbytes).c_str(), hipGetErrorString(err));
            hip_clear_error();
            return false;
        }
        done += nbytes;
        print_progress("clear VRAM dst", done, total);
    }
    return true;
}

bool copy_device_range(const std::vector<Block>& blocks, uint64_t src_begin, uint64_t dst_begin, uint64_t bytes) {
    uint64_t done = 0;
    print_progress("VRAM -> VRAM", 0, bytes);
    while (done < bytes) {
        uint8_t* src = nullptr;
        uint8_t* dst = nullptr;
        size_t src_room = 0;
        size_t dst_room = 0;
        if (!locate_span(blocks, src_begin + done, &src, &src_room) ||
            !locate_span(blocks, dst_begin + done, &dst, &dst_room)) {
            std::fprintf(stderr, "\nFAIL: VRAM copy range is outside the allocation\n");
            return false;
        }
        size_t nbytes = std::min(src_room, dst_room);
        if (static_cast<uint64_t>(nbytes) > bytes - done) {
            nbytes = static_cast<size_t>(bytes - done);
        }
        if (nbytes == 0) {
            std::fprintf(stderr, "\nFAIL: VRAM copy made no progress\n");
            return false;
        }
        hipError_t err = hipMemcpy(dst, src, nbytes, hipMemcpyDeviceToDevice);
        if (err != hipSuccess) {
            std::fprintf(stderr,
                         "\nFAIL: hipMemcpy (D2D, %s): %s\n",
                         format_bytes(nbytes).c_str(),
                         hipGetErrorString(err));
            hip_clear_error();
            return false;
        }
        done += nbytes;
        print_progress("VRAM -> VRAM", done, bytes);
    }
    return true;
}

bool check_device_range(const std::vector<Block>& blocks,
                        uint64_t seed,
                        uint8_t* host,
                        size_t chunk,
                        uint64_t begin,
                        uint64_t end,
                        uint64_t source_base,
                        Mismatch* mismatch) {
    const uint64_t total = end - begin;
    uint64_t done = 0;
    print_progress("VRAM copy check", 0, total);
    auto* words = reinterpret_cast<uint64_t*>(host);
    while (done < total) {
        uint8_t* device_at = nullptr;
        size_t room = 0;
        if (!locate_span(blocks, begin + done, &device_at, &room)) {
            std::fprintf(stderr, "\nFAIL: VRAM check range is outside the allocation\n");
            return false;
        }
        size_t nbytes = std::min(room, chunk);
        if (static_cast<uint64_t>(nbytes) > total - done) {
            nbytes = static_cast<size_t>(total - done);
        }
        nbytes &= ~(sizeof(uint64_t) - 1);
        if (nbytes == 0) {
            std::fprintf(stderr, "\nFAIL: VRAM check range made no progress\n");
            return false;
        }
        std::memset(host, 0, nbytes);
        if (!move_chunk(false, device_at, host, nbytes)) {
            return false;
        }
        const uint64_t logical = begin + done;
        const uint64_t base_word = (logical - source_base) / sizeof(uint64_t);
        if (auto hit = parallel_check(words, nbytes / sizeof(uint64_t), seed, base_word)) {
            mismatch->found = true;
            mismatch->offset = logical + static_cast<uint64_t>(hit->index) * sizeof(uint64_t);
            mismatch->expected = hit->expected;
            mismatch->actual = hit->actual;
            std::printf("\n");
            return false;
        }
        done += nbytes;
        print_progress("VRAM copy check", done, total);
    }
    return true;
}

struct Staging {
    uint8_t* ptr = nullptr;
    bool pinned = false;

    ~Staging() { reset(); }

    bool alloc(size_t bytes) {
        void* raw = nullptr;
        hipError_t err = hipHostMalloc(&raw, bytes, hipHostMallocDefault);
        if (err == hipSuccess) {
            ptr = static_cast<uint8_t*>(raw);
            pinned = true;
            return true;
        }
        hip_clear_error();
        raw = std::aligned_alloc(64, bytes);
        if (raw == nullptr) {
            std::fprintf(stderr, "FAIL: could not allocate the staging buffer (%s)\n", format_bytes(bytes).c_str());
            return false;
        }
        ptr = static_cast<uint8_t*>(raw);
        pinned = false;
        return true;
    }

    void reset() {
        if (ptr == nullptr) {
            return;
        }
        if (pinned) {
            (void)hipHostFree(ptr);
            hip_clear_error();
        } else {
            std::free(ptr);
        }
        ptr = nullptr;
        pinned = false;
    }
};

struct RoundCleanup {
    int device = 0;
    std::vector<Block>* blocks = nullptr;
    Staging* staging = nullptr;
    bool active = true;

    ~RoundCleanup() {
        if (!active) {
            return;
        }
        if (staging != nullptr) {
            staging->reset();
        }
        if (blocks != nullptr) {
            free_blocks(blocks);
        }
        (void)hipSetDevice(device);
        (void)hipDeviceSynchronize();
        // Drop the caching allocator so it does not keep holding VRAM.
        (void)hipDeviceReset();
    }
};

bool within_leftover(uint64_t gap) {
    return gap <= kMaxLeftover;
}

struct RoundRecord {
    int round = 0;
    bool have_seed = false;
    uint64_t seed = 0;
    bool have_alloc = false;
    uint64_t allocated = 0;
    uint64_t free_after = 0;
    bool host_pass = false;
    double h2d_s = 0;
    double h2d_rate = 0;
    double d2h_s = 0;
    double d2h_rate = 0;
    bool vram_pass = false;
    double clear_s = 0;
    double d2d_s = 0;
    double d2d_rate = 0;
    double check_s = 0;
    double check_rate = 0;
    bool have_mismatch = false;
    bool have_source_offset = false;
    uint64_t mismatch_offset = 0;
    uint64_t source_offset = 0;
    uint64_t expected = 0;
    uint64_t actual = 0;
    std::string fail;
    std::vector<TempReading> temp_before;
    std::vector<TempReading> temp_after;
};

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

void json_string(FILE* fp, const std::string& text) {
    const std::string escaped = json_escape(text);
    std::fputc('"', fp);
    std::fwrite(escaped.data(), 1, escaped.size(), fp);
    std::fputc('"', fp);
}

struct Report {
    FILE* fp = nullptr;
    std::string path;
    std::string time;
    std::string host;
    std::string kernel;
    std::string hip;
    bool have_device = false;
    std::string device;
    std::string arch;
    std::string pci;
    bool have_vram_total = false;
    uint64_t vram_total_bytes = 0;
    bool have_vram_free = false;
    uint64_t vram_free_bytes = 0;
    bool have_cpu_visible = false;
    uint64_t cpu_visible_bytes = 0;
    uint64_t sysfs_vram_total_bytes = 0;
    bool have_bar0 = false;
    uint64_t bar0_bytes = 0;
    bool have_host_memory = false;
    uint64_t host_memory_bytes = 0;
    bool have_host_available = false;
    uint64_t host_available_bytes = 0;
    bool have_rounds = false;
    int rounds = 0;
    bool have_chunk = false;
    uint64_t chunk_bytes = 0;
    std::vector<RoundRecord> tests;
    bool finalized = false;

    ~Report() {
        if (fp != nullptr) {
            std::fclose(fp);
            fp = nullptr;
        }
    }

    bool open() {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        char name[80];
        std::snprintf(name,
                      sizeof name,
                      "vram-test-%04d%02d%02d-%02d%02d%02d.json",
                      local.tm_year + 1900,
                      local.tm_mon + 1,
                      local.tm_mday,
                      local.tm_hour,
                      local.tm_min,
                      local.tm_sec);
        path = name;
        char when[80];
        std::snprintf(when,
                      sizeof when,
                      "%04d-%02d-%02d %02d:%02d:%02d",
                      local.tm_year + 1900,
                      local.tm_mon + 1,
                      local.tm_mday,
                      local.tm_hour,
                      local.tm_min,
                      local.tm_sec);
        time = when;
        char host_name[256] = {};
        if (gethostname(host_name, sizeof host_name - 1) == 0) {
            host = host_name;
        }
        utsname un{};
        if (uname(&un) == 0) {
            kernel = std::string(un.sysname) + " " + un.release;
        }
        char hip_text[64];
        std::snprintf(hip_text, sizeof hip_text, "%d.%d.%d", HIP_VERSION_MAJOR, HIP_VERSION_MINOR, HIP_VERSION_PATCH);
        hip = hip_text;
        return true;
    }

    void add_round(const RoundRecord& rec) { tests.push_back(rec); }

    int count_failed() const {
        int n = 0;
        for (const RoundRecord& rec : tests) {
            if (!(rec.host_pass && rec.vram_pass && rec.fail.empty())) {
                ++n;
            }
        }
        return n;
    }

    // Rewrite the JSON through a temporary file in the same directory, then rename it
    // over the destination so readers never see a partial document.
    bool write_snapshot(const char* result, int passed, int failed, int exit_code, bool write_exit, const std::string& setup_fail) {
        const std::string tmp_path = path + ".tmp";
        fp = std::fopen(tmp_path.c_str(), "w");
        if (fp == nullptr) {
            std::fprintf(stderr, "FAIL: could not write %s\n", tmp_path.c_str());
            return false;
        }

        std::fprintf(fp, "{\n");
        std::fprintf(fp, "  \"program\": \"verify-rx7900xtx\",\n");
        std::fprintf(fp, "  \"time\": ");
        json_string(fp, time);
        std::fprintf(fp, ",\n");
        if (!host.empty()) {
            std::fprintf(fp, "  \"host\": ");
            json_string(fp, host);
            std::fprintf(fp, ",\n");
        }
        if (!kernel.empty()) {
            std::fprintf(fp, "  \"kernel\": ");
            json_string(fp, kernel);
            std::fprintf(fp, ",\n");
        }
        std::fprintf(fp, "  \"hip\": ");
        json_string(fp, hip);
        std::fprintf(fp, ",\n");
        if (have_device) {
            std::fprintf(fp, "  \"device\": ");
            json_string(fp, device);
            std::fprintf(fp, ",\n  \"arch\": ");
            json_string(fp, arch);
            std::fprintf(fp, ",\n  \"pci\": ");
            json_string(fp, pci);
            std::fprintf(fp, ",\n");
        }
        if (have_vram_total) {
            std::fprintf(fp, "  \"vram_total_bytes\": %llu,\n", static_cast<unsigned long long>(vram_total_bytes));
        }
        if (have_vram_free) {
            std::fprintf(fp, "  \"vram_free_bytes\": %llu,\n", static_cast<unsigned long long>(vram_free_bytes));
        }
        if (have_cpu_visible) {
            std::fprintf(fp, "  \"cpu_visible_vram_bytes\": %llu,\n", static_cast<unsigned long long>(cpu_visible_bytes));
            std::fprintf(fp, "  \"large_bar\": %s,\n", cpu_visible_bytes >= sysfs_vram_total_bytes ? "true" : "false");
        }
        if (have_bar0) {
            std::fprintf(fp, "  \"pci_bar0_bytes\": %llu,\n", static_cast<unsigned long long>(bar0_bytes));
        }
        if (have_host_memory) {
            std::fprintf(fp, "  \"host_memory_bytes\": %llu,\n", static_cast<unsigned long long>(host_memory_bytes));
        }
        if (have_host_available) {
            std::fprintf(fp, "  \"host_available_bytes\": %llu,\n", static_cast<unsigned long long>(host_available_bytes));
        }
        if (have_rounds) {
            std::fprintf(fp, "  \"rounds\": %d,\n", rounds);
        }
        if (have_chunk) {
            std::fprintf(fp, "  \"chunk_bytes\": %llu,\n", static_cast<unsigned long long>(chunk_bytes));
        }

        std::fprintf(fp, "  \"tests\": [\n");
        for (size_t i = 0; i < tests.size(); ++i) {
            write_test(tests[i], i + 1 == tests.size());
        }
        std::fprintf(fp, "  ],\n");

        std::fprintf(fp, "  \"passed\": %d,\n", passed);
        std::fprintf(fp, "  \"failed\": %d,\n", failed);
        std::fprintf(fp, "  \"result\": ");
        json_string(fp, result);
        if (!setup_fail.empty()) {
            std::fprintf(fp, ",\n  \"reason\": ");
            json_string(fp, setup_fail);
        }
        if (write_exit) {
            std::fprintf(fp, ",\n  \"exit_code\": %d", exit_code);
        }
        std::fprintf(fp, "\n}\n");
        if (std::fflush(fp) != 0) {
            std::fclose(fp);
            fp = nullptr;
            std::remove(tmp_path.c_str());
            std::fprintf(stderr, "FAIL: could not flush %s\n", tmp_path.c_str());
            return false;
        }
        const int fd = fileno(fp);
        if (fd >= 0 && fsync(fd) != 0) {
            std::fclose(fp);
            fp = nullptr;
            std::remove(tmp_path.c_str());
            std::fprintf(stderr, "FAIL: could not flush %s to disk\n", tmp_path.c_str());
            return false;
        }
        std::fclose(fp);
        fp = nullptr;
        if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
            std::remove(tmp_path.c_str());
            std::fprintf(stderr, "FAIL: could not replace %s\n", path.c_str());
            return false;
        }
        return true;
    }

    void publish_progress() {
        const int failed = count_failed();
        const int passed = static_cast<int>(tests.size()) - failed;
        (void)write_snapshot("in_progress", passed, failed, 0, false, "");
    }

    void finish(int exit_code, bool ran, int failed, const std::string& setup_fail) {
        if (finalized) {
            return;
        }
        finalized = true;
        if (!ran) {
            (void)write_snapshot("setup_failed", 0, 0, exit_code, true, setup_fail);
            return;
        }
        const int passed = rounds - failed;
        if (failed == 0) {
            (void)write_snapshot("passed", passed, failed, exit_code, true, "");
        } else {
            (void)write_snapshot("failed", passed, failed, exit_code, true, "");
        }
    }

    const TempReading* find_temp(const std::vector<TempReading>& readings, const std::string& name) const {
        for (const TempReading& reading : readings) {
            if (reading.name == name) {
                return &reading;
            }
        }
        return nullptr;
    }

    void write_temperature(const RoundRecord& rec) {
        std::vector<std::string> names;
        auto add_names = [&names](const std::vector<TempReading>& readings) {
            for (const TempReading& reading : readings) {
                bool seen = false;
                for (const std::string& name : names) {
                    if (name == reading.name) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    names.push_back(reading.name);
                }
            }
        };
        add_names(rec.temp_before);
        add_names(rec.temp_after);
        std::fprintf(fp, ",\n      \"temperature\": [");
        for (size_t i = 0; i < names.size(); ++i) {
            const TempReading* before = find_temp(rec.temp_before, names[i]);
            const TempReading* after = find_temp(rec.temp_after, names[i]);
            const TempReading* limits = after != nullptr ? after : before;
            std::fprintf(fp, "\n        {\n          \"name\": ");
            json_string(fp, names[i]);
            if (before != nullptr) {
                std::fprintf(fp, ",\n          \"before_millicelsius\": %lld", static_cast<long long>(before->millicelsius));
            }
            if (after != nullptr) {
                std::fprintf(fp, ",\n          \"after_millicelsius\": %lld", static_cast<long long>(after->millicelsius));
            }
            if (limits != nullptr && limits->have_critical) {
                std::fprintf(fp,
                             ",\n          \"critical_millicelsius\": %lld",
                             static_cast<long long>(limits->critical_millicelsius));
            }
            if (limits != nullptr && limits->have_emergency) {
                std::fprintf(fp,
                             ",\n          \"emergency_millicelsius\": %lld",
                             static_cast<long long>(limits->emergency_millicelsius));
            }
            std::fprintf(fp, "\n        }%s", i + 1 == names.size() ? "" : ",");
        }
        std::fprintf(fp, "\n      ]");
    }

    void write_test(const RoundRecord& rec, bool last) {
        std::fprintf(fp, "    {\n");
        std::fprintf(fp, "      \"round\": %d", rec.round);
        if (rec.have_seed) {
            char seed[32];
            std::snprintf(seed, sizeof seed, "0x%016llx", static_cast<unsigned long long>(rec.seed));
            std::fprintf(fp, ",\n      \"seed\": ");
            json_string(fp, seed);
        }
        if (rec.have_alloc) {
            std::fprintf(fp, ",\n      \"allocated_bytes\": %llu", static_cast<unsigned long long>(rec.allocated));
            std::fprintf(fp, ",\n      \"free_after_bytes\": %llu", static_cast<unsigned long long>(rec.free_after));
        }
        if (rec.host_pass) {
            std::fprintf(fp, ",\n      \"host\": {\n");
            std::fprintf(fp, "        \"result\": \"pass\",\n");
            std::fprintf(fp, "        \"h2d_seconds\": %.6f,\n", rec.h2d_s);
            std::fprintf(fp, "        \"h2d_gib_s\": %.6f,\n", rec.h2d_rate);
            std::fprintf(fp, "        \"d2h_seconds\": %.6f,\n", rec.d2h_s);
            std::fprintf(fp, "        \"d2h_gib_s\": %.6f\n", rec.d2h_rate);
            std::fprintf(fp, "      }");
        }
        if (rec.vram_pass) {
            std::fprintf(fp, ",\n      \"vram\": {\n");
            std::fprintf(fp, "        \"result\": \"pass\",\n");
            std::fprintf(fp, "        \"clear_seconds\": %.6f,\n", rec.clear_s);
            std::fprintf(fp, "        \"d2d_seconds\": %.6f,\n", rec.d2d_s);
            std::fprintf(fp, "        \"d2d_gib_s\": %.6f,\n", rec.d2d_rate);
            std::fprintf(fp, "        \"check_seconds\": %.6f,\n", rec.check_s);
            std::fprintf(fp, "        \"check_gib_s\": %.6f\n", rec.check_rate);
            std::fprintf(fp, "      }");
        }
        if (!rec.fail.empty()) {
            std::fprintf(fp, ",\n      \"error\": ");
            json_string(fp, rec.fail);
        }
        if (!rec.temp_before.empty() || !rec.temp_after.empty()) {
            write_temperature(rec);
        }
        if (rec.have_mismatch) {
            std::fprintf(fp, ",\n      \"mismatch\": {\n");
            std::fprintf(fp, "        \"offset\": %llu", static_cast<unsigned long long>(rec.mismatch_offset));
            if (rec.have_source_offset) {
                std::fprintf(fp, ",\n        \"source_offset\": %llu", static_cast<unsigned long long>(rec.source_offset));
            }
            char expected[32];
            char actual[32];
            std::snprintf(expected, sizeof expected, "0x%016llx", static_cast<unsigned long long>(rec.expected));
            std::snprintf(actual, sizeof actual, "0x%016llx", static_cast<unsigned long long>(rec.actual));
            std::fprintf(fp, ",\n        \"expected\": ");
            json_string(fp, expected);
            std::fprintf(fp, ",\n        \"actual\": ");
            json_string(fp, actual);
            std::fprintf(fp, "\n      }");
        }
        const bool pass = rec.host_pass && rec.vram_pass && rec.fail.empty();
        std::fprintf(fp, ",\n      \"result\": \"%s\"\n", pass ? "pass" : "fail");
        std::fprintf(fp, "    }%s\n", last ? "" : ",");
    }
};

struct RoundRecorder {
    Report* report = nullptr;
    RoundRecord rec;
    bool written = false;

    ~RoundRecorder() {
        if (!written && report != nullptr) {
            report->add_round(rec);
        }
    }
};

bool run_round(int round, int rounds, int device, size_t chunk, const std::string& sysfs_path, const std::string& hwmon_path, Report* report) {
    RoundRecorder recorded;
    recorded.report = report;
    recorded.rec.round = round;
    RoundRecord& rec = recorded.rec;
    std::printf("\n--- round %d/%d ---\n", round, rounds);
    std::fflush(stdout);

    if (!hip_ok(hipSetDevice(device), "hipSetDevice", &rec.fail)) {
        return false;
    }

    uint64_t seed = 0;
    if (!random_seed(&seed)) {
        rec.fail = "could not read a random seed";
        return false;
    }
    rec.have_seed = true;
    rec.seed = seed;
    std::printf("seed: 0x%016llx\n", static_cast<unsigned long long>(seed));

    size_t free_before = 0;
    size_t total = 0;
    if (!hip_ok(hipMemGetInfo(&free_before, &total), "hipMemGetInfo", &rec.fail)) {
        return false;
    }
    SysfsVram sysfs_before{};
    read_sysfs_vram(sysfs_path, &sysfs_before);
    std::printf("before alloc  HIP free %s / total %s\n",
                format_bytes(free_before).c_str(),
                format_bytes(total).c_str());
    if (sysfs_before.ok) {
        std::printf("before alloc  sysfs used %s / total %s\n",
                    format_bytes(sysfs_before.used).c_str(),
                    format_bytes(sysfs_before.total).c_str());
    }

    rec.temp_before = read_temperatures(hwmon_path);
    if (!rec.temp_before.empty()) {
        std::printf("temperature before  %s\n", format_temp_list(rec.temp_before).c_str());
    }

    std::vector<Block> blocks;
    Staging staging;
    RoundCleanup cleanup{device, &blocks, &staging, true};
    struct TempAtEnd {
        RoundRecord* rec = nullptr;
        const std::string* hwmon = nullptr;
        ~TempAtEnd() {
            if (rec == nullptr || hwmon == nullptr) {
                return;
            }
            rec->temp_after = read_temperatures(*hwmon);
            if (!rec->temp_after.empty()) {
                std::printf("temperature after   %s\n", format_temp_list(rec->temp_after).c_str());
            }
        }
    } temp_at_end{&rec, &hwmon_path};

    std::printf("allocating VRAM...\n");
    std::fflush(stdout);
    size_t allocated = 0;
    size_t free_after = 0;
    size_t total_after = 0;
    if (!allocate_vram(&blocks, &allocated, &free_after, &total_after)) {
        std::fprintf(stderr,
                     "FAIL: round %d could not allocate the free VRAM (allocated %s, free %s)\n",
                     round,
                     format_bytes(allocated).c_str(),
                     format_bytes(free_after).c_str());
        rec.have_alloc = true;
        rec.allocated = allocated;
        rec.free_after = free_after;
        rec.fail = "could not allocate the free VRAM";
        return false;
    }
    rec.have_alloc = true;
    rec.allocated = allocated;
    rec.free_after = free_after;

    const uint64_t unallocated =
        free_before > allocated + free_after ? free_before - allocated - free_after : 0;
    std::printf("allocated %s  (%zu blocks), free after alloc %s\n",
                format_bytes(allocated).c_str(),
                blocks.size(),
                format_bytes(free_after).c_str());

    if (!within_leftover(free_after) || !within_leftover(unallocated) || allocated == 0) {
        std::fprintf(stderr,
                     "FAIL: round %d did not fill VRAM (free %s, unaccounted %s, limit %s)\n",
                     round,
                     format_bytes(free_after).c_str(),
                     format_bytes(unallocated).c_str(),
                     format_bytes(kMaxLeftover).c_str());
        rec.fail = "did not fill VRAM";
        return false;
    }

    SysfsVram sysfs_after{};
    if (!read_sysfs_vram(sysfs_path, &sysfs_after)) {
        std::fprintf(stderr, "FAIL: round %d could not read sysfs VRAM usage\n", round);
        rec.fail = "could not read sysfs VRAM usage";
        return false;
    }
    // The runtime may reuse VRAM that sysfs still counts as used.
    // Judge fullness from the used amount after allocation, not from the delta.
    const uint64_t sysfs_free =
        sysfs_after.total > sysfs_after.used ? sysfs_after.total - sysfs_after.used : 0;
    const bool covers_alloc = sysfs_after.used + kMaxLeftover >= allocated;
    std::printf("after alloc  sysfs used %s / total %s (free %s)\n",
                format_bytes(sysfs_after.used).c_str(),
                format_bytes(sysfs_after.total).c_str(),
                format_bytes(sysfs_free).c_str());
    if (!within_leftover(sysfs_free) || !covers_alloc) {
        std::fprintf(stderr,
                     "FAIL: round %d sysfs usage (%s / %s) does not show VRAM exhausted\n",
                     round,
                     format_bytes(sysfs_after.used).c_str(),
                     format_bytes(sysfs_after.total).c_str());
        rec.fail = "sysfs usage does not show VRAM exhausted";
        return false;
    }

    if (!staging.alloc(chunk)) {
        rec.fail = "could not allocate the staging buffer";
        return false;
    }
    std::printf("staging: %s (%s)\n",
                format_bytes(chunk).c_str(),
                staging.pinned ? "pinned" : "pageable");

    const auto t0 = std::chrono::steady_clock::now();
    Mismatch mismatch;
    if (!transfer_and_check(blocks, seed, staging.ptr, chunk, true, &mismatch)) {
        std::fprintf(stderr, "FAIL: round %d host-to-VRAM copy failed\n", round);
        rec.fail = "host-to-VRAM copy failed";
        return false;
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (!hip_ok(hipDeviceSynchronize(), "hipDeviceSynchronize", &rec.fail)) {
        return false;
    }
    if (!transfer_and_check(blocks, seed, staging.ptr, chunk, false, &mismatch)) {
        if (mismatch.found) {
            std::fprintf(stderr,
                         "FAIL: round %d mismatch at offset %llu expected 0x%016llx actual 0x%016llx\n",
                         round,
                         static_cast<unsigned long long>(mismatch.offset),
                         static_cast<unsigned long long>(mismatch.expected),
                         static_cast<unsigned long long>(mismatch.actual));
            rec.fail = "host mismatch";
            rec.have_mismatch = true;
            rec.mismatch_offset = mismatch.offset;
            rec.expected = mismatch.expected;
            rec.actual = mismatch.actual;
        } else {
            std::fprintf(stderr, "FAIL: round %d VRAM-to-host copy failed\n", round);
            rec.fail = "VRAM-to-host copy failed";
        }
        return false;
    }
    const auto t2 = std::chrono::steady_clock::now();

    const double h2d = std::chrono::duration<double>(t1 - t0).count();
    const double d2h = std::chrono::duration<double>(t2 - t1).count();
    const double gib = static_cast<double>(allocated) / (1024.0 * 1024.0 * 1024.0);
    rec.host_pass = true;
    rec.h2d_s = h2d;
    rec.h2d_rate = h2d > 0 ? gib / h2d : 0;
    rec.d2h_s = d2h;
    rec.d2h_rate = d2h > 0 ? gib / d2h : 0;
    std::printf("round %d host copy: PASS  (H2D %.1f s, %.2f GiB/s / D2H check %.1f s, %.2f GiB/s)\n",
                round,
                h2d,
                rec.h2d_rate,
                d2h,
                rec.d2h_rate);

    const uint64_t half = (static_cast<uint64_t>(allocated) / 2) & ~uint64_t{7};
    if (half < sizeof(uint64_t)) {
        std::fprintf(stderr, "FAIL: round %d allocated region is too small to split\n", round);
        rec.fail = "allocated region is too small to split";
        return false;
    }
    std::printf("VRAM copy: %s from offset 0 to offset %s\n",
                format_bytes(half).c_str(),
                format_bytes(half).c_str());

    const auto t3 = std::chrono::steady_clock::now();
    if (!zero_device_range(blocks, half, half + half)) {
        std::fprintf(stderr, "FAIL: round %d could not clear the VRAM destination\n", round);
        rec.fail = "could not clear the VRAM destination";
        return false;
    }
    if (!hip_ok(hipDeviceSynchronize(), "hipDeviceSynchronize", &rec.fail)) {
        return false;
    }
    const auto t4 = std::chrono::steady_clock::now();
    if (!copy_device_range(blocks, 0, half, half)) {
        std::fprintf(stderr, "FAIL: round %d VRAM-to-VRAM copy failed\n", round);
        rec.fail = "VRAM-to-VRAM copy failed";
        return false;
    }
    if (!hip_ok(hipDeviceSynchronize(), "hipDeviceSynchronize", &rec.fail)) {
        return false;
    }
    const auto t5 = std::chrono::steady_clock::now();
    Mismatch copy_mismatch;
    if (!check_device_range(blocks, seed, staging.ptr, chunk, half, half + half, half, &copy_mismatch)) {
        if (copy_mismatch.found) {
            std::fprintf(stderr,
                         "FAIL: round %d VRAM copy mismatch at offset %llu (source offset %llu) "
                         "expected 0x%016llx actual 0x%016llx\n",
                         round,
                         static_cast<unsigned long long>(copy_mismatch.offset),
                         static_cast<unsigned long long>(copy_mismatch.offset - half),
                         static_cast<unsigned long long>(copy_mismatch.expected),
                         static_cast<unsigned long long>(copy_mismatch.actual));
            rec.fail = "VRAM copy mismatch";
            rec.have_mismatch = true;
            rec.have_source_offset = true;
            rec.mismatch_offset = copy_mismatch.offset;
            rec.source_offset = copy_mismatch.offset - half;
            rec.expected = copy_mismatch.expected;
            rec.actual = copy_mismatch.actual;
        } else {
            std::fprintf(stderr, "FAIL: round %d VRAM copy readback failed\n", round);
            rec.fail = "VRAM copy readback failed";
        }
        return false;
    }
    const auto t6 = std::chrono::steady_clock::now();

    const double clear_s = std::chrono::duration<double>(t4 - t3).count();
    const double d2d = std::chrono::duration<double>(t5 - t4).count();
    const double d2d_check = std::chrono::duration<double>(t6 - t5).count();
    const double half_gib = static_cast<double>(half) / (1024.0 * 1024.0 * 1024.0);
    std::printf(
        "round %d VRAM copy: PASS  (clear %.3f s / D2D %.3f s, %.2f GiB/s / check %.1f s, %.2f GiB/s)\n",
        round,
        clear_s,
        d2d,
        d2d > 0 ? half_gib / d2d : 0,
        d2d_check,
        d2d_check > 0 ? half_gib / d2d_check : 0);
    rec.vram_pass = true;
    rec.clear_s = clear_s;
    rec.d2d_s = d2d;
    rec.d2d_rate = d2d > 0 ? half_gib / d2d : 0;
    rec.check_s = d2d_check;
    rec.check_rate = d2d_check > 0 ? half_gib / d2d_check : 0;
    return true;
}

bool choose_chunk(size_t requested, size_t* chunk_out, std::string* fail) {
    uint64_t available_kb = 0;
    if (!read_meminfo_kb("MemAvailable:", &available_kb)) {
        std::fprintf(stderr, "FAIL: could not read MemAvailable\n");
        if (fail != nullptr) {
            *fail = "could not read MemAvailable";
        }
        return false;
    }
    const uint64_t available = available_kb * 1024ull;
    uint64_t limit = available / 4;
    limit &= ~((1ull << 20) - 1);
    if (limit > kMaxChunk) {
        limit = kMaxChunk;
    }
    size_t chunk = requested;
    if (static_cast<uint64_t>(chunk) > limit) {
        std::printf("shrinking chunk from %s to %s (MemAvailable %s)\n",
                    format_bytes(chunk).c_str(),
                    format_bytes(limit).c_str(),
                    format_bytes(available).c_str());
        chunk = static_cast<size_t>(limit);
    }
    if (chunk < (1ull << 20)) {
        std::fprintf(stderr, "FAIL: not enough host memory for the staging buffer\n");
        if (fail != nullptr) {
            *fail = "not enough host memory for the staging buffer";
        }
        return false;
    }
    *chunk_out = chunk;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    int rounds = kDefaultRounds;
    size_t chunk_mib = kDefaultChunkMiB;
    if (argc >= 2) {
        uint64_t v = 0;
        if (!parse_u64(argv[1], &v) || v < 1 || v > 10000) {
            std::fprintf(stderr, "usage: %s [rounds] [chunk_mib]\n", argv[0]);
            return 2;
        }
        rounds = static_cast<int>(v);
    }
    if (argc >= 3) {
        uint64_t v = 0;
        if (!parse_u64(argv[2], &v) || v < 1 || v > 1024 * 1024) {
            std::fprintf(stderr, "usage: %s [rounds] [chunk_mib]\n", argv[0]);
            return 2;
        }
        chunk_mib = static_cast<size_t>(v);
    }
    if (argc > 3) {
        std::fprintf(stderr, "usage: %s [rounds] [chunk_mib]\n", argv[0]);
        return 2;
    }

    Report report;
    if (!report.open()) {
        std::fprintf(stderr, "FAIL: could not create the result log\n");
        return 2;
    }
    std::printf("log: %s\n", report.path.c_str());
    report.rounds = rounds;
    report.have_rounds = true;

    int exit_code = 2;
    int failed = 0;
    bool ran = false;
    std::string setup_fail;
    struct Finish {
        Report* report = nullptr;
        int* exit_code = nullptr;
        int* failed = nullptr;
        bool* ran = nullptr;
        std::string* setup_fail = nullptr;

        ~Finish() {
            if (report == nullptr) {
                return;
            }
            const std::string reason = setup_fail == nullptr ? std::string() : *setup_fail;
            report->finish(*exit_code, *ran, *failed, reason);
            std::printf("log: %s\n", report->path.c_str());
        }
    } finish{&report, &exit_code, &failed, &ran, &setup_fail};

    std::printf("=== VRAM test (RX 7900 XTX) ===\n");
    const int device = find_device();
    if (device < 0) {
        setup_fail = "RX 7900 XTX not found";
        return exit_code;
    }
    if (!hip_ok(hipSetDevice(device), "hipSetDevice", &setup_fail)) {
        return exit_code;
    }

    hipDeviceProp_t prop{};
    if (!hip_ok(hipGetDeviceProperties(&prop, device), "hipGetDeviceProperties", &setup_fail)) {
        return exit_code;
    }
    size_t free_bytes = 0;
    size_t total_bytes = 0;
    if (!hip_ok(hipMemGetInfo(&free_bytes, &total_bytes), "hipMemGetInfo", &setup_fail)) {
        return exit_code;
    }

    const std::string pci = pci_bus_id(device);
    const std::string sysfs_path = sysfs_device_path(pci);
    const std::string hwmon_path = find_amdgpu_hwmon(sysfs_path);
    SysfsVram sysfs{};
    read_sysfs_vram(sysfs_path, &sysfs);

    uint64_t mem_total_kb = 0;
    uint64_t mem_avail_kb = 0;
    read_meminfo_kb("MemTotal:", &mem_total_kb);
    read_meminfo_kb("MemAvailable:", &mem_avail_kb);
    const uint64_t mem_total = mem_total_kb * 1024ull;
    const uint64_t mem_avail = mem_avail_kb * 1024ull;

    std::printf("device: gpu %d  %s  (%s)  PCI %s\n", device, prop.name, prop.gcnArchName, pci.c_str());
    std::printf("VRAM total: %s (%llu bytes)\n",
                format_bytes(prop.totalGlobalMem).c_str(),
                static_cast<unsigned long long>(prop.totalGlobalMem));
    std::printf("VRAM free: %s / HIP total %s\n",
                format_bytes(free_bytes).c_str(),
                format_bytes(total_bytes).c_str());
    if (sysfs.ok) {
        std::printf("sysfs VRAM: used %s / total %s\n",
                    format_bytes(sysfs.used).c_str(),
                    format_bytes(sysfs.total).c_str());
    }
    uint64_t bar0 = 0;
    const bool have_bar0 = read_bar0_bytes(sysfs_path, &bar0);
    if (sysfs.ok && sysfs.have_visible) {
        std::printf("CPU-visible VRAM: %s of %s%s%s\n",
                    format_bytes(sysfs.visible).c_str(),
                    format_bytes(sysfs.total).c_str(),
                    have_bar0 ? ", PCI BAR 0 " : "",
                    have_bar0 ? format_bytes(bar0).c_str() : "");
        if (sysfs.visible < sysfs.total) {
            std::printf("WARNING: only %s of VRAM is CPU-visible: Resizable BAR is not active.\n"
                        "         Enable \"Above 4G Decoding\" and \"Re-Size BAR Support\" in the BIOS (CSM off).\n"
                        "         This copy test still passes; software that maps VRAM into the host fails.\n"
                        "         See doc/bar-check.md.\n",
                        format_bytes(sysfs.visible).c_str());
        }
    }
    const std::vector<TempReading> sensors = read_temperatures(hwmon_path);
    if (sensors.empty()) {
        std::printf("temperature: sensors not available\n");
    } else {
        std::string limits;
        for (const TempReading& sensor : sensors) {
            if (!limits.empty()) {
                limits += "; ";
            }
            limits += sensor.name;
            if (sensor.have_critical) {
                limits += " critical " + format_temp_c(sensor.critical_millicelsius);
            }
            if (sensor.have_emergency) {
                limits += " emergency " + format_temp_c(sensor.emergency_millicelsius);
            }
        }
        std::printf("temperature: %s\n", limits.c_str());
    }
    std::printf("host memory: total %s / available %s\n",
                format_bytes(mem_total).c_str(),
                format_bytes(mem_avail).c_str());
    if (mem_total > 0 && mem_total < prop.totalGlobalMem) {
        std::printf("host memory is smaller than VRAM, so the test copies in chunks.\n");
    }

    report.have_device = true;
    report.device = prop.name;
    report.arch = prop.gcnArchName;
    report.pci = pci;
    report.have_vram_total = true;
    report.vram_total_bytes = prop.totalGlobalMem;
    report.have_vram_free = true;
    report.vram_free_bytes = free_bytes;
    report.have_cpu_visible = sysfs.ok && sysfs.have_visible;
    report.cpu_visible_bytes = sysfs.visible;
    report.sysfs_vram_total_bytes = sysfs.total;
    report.have_bar0 = have_bar0;
    report.bar0_bytes = bar0;
    report.have_host_memory = true;
    report.host_memory_bytes = mem_total;
    report.have_host_available = true;
    report.host_available_bytes = mem_avail;

    size_t chunk = 0;
    if (!choose_chunk(chunk_mib << 20, &chunk, &setup_fail)) {
        return exit_code;
    }
    report.have_chunk = true;
    report.chunk_bytes = chunk;

    std::printf("rounds: %d  chunk: %s  max leftover: %s\n",
                rounds,
                format_bytes(chunk).c_str(),
                format_bytes(kMaxLeftover).c_str());
    std::fflush(stdout);

    ran = true;
    for (int round = 1; round <= rounds; ++round) {
        if (!run_round(round, rounds, device, chunk, sysfs_path, hwmon_path, &report)) {
            ++failed;
        }
        report.publish_progress();
    }

    std::printf("\nresult: %d/%d passed", rounds - failed, rounds);
    if (failed == 0) {
        std::printf("\n");
        exit_code = 0;
        return exit_code;
    }
    std::printf(", %d failed\n", failed);
    exit_code = 1;
    return exit_code;
}
