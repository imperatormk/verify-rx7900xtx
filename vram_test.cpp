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
#include <cstdarg>
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
    std::string fail;
};

struct Report {
    FILE* fp = nullptr;
    std::string path;

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
        char name[64];
        std::snprintf(name,
                      sizeof name,
                      "vram-test-%04d%02d%02d-%02d%02d%02d.log",
                      local.tm_year + 1900,
                      local.tm_mon + 1,
                      local.tm_mday,
                      local.tm_hour,
                      local.tm_min,
                      local.tm_sec);
        path = name;
        fp = std::fopen(path.c_str(), "w");
        if (fp == nullptr) {
            return false;
        }
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
        line("program: verify-rx7900xtx");
        line("time: %s", when);
        char host[256] = {};
        if (gethostname(host, sizeof host - 1) == 0) {
            line("host: %s", host);
        }
        utsname un{};
        if (uname(&un) == 0) {
            line("kernel: %s %s", un.sysname, un.release);
        }
        line("hip: %d.%d.%d", HIP_VERSION_MAJOR, HIP_VERSION_MINOR, HIP_VERSION_PATCH);
        return true;
    }

    void line(const char* fmt, ...) {
        if (fp == nullptr) {
            return;
        }
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(fp, fmt, ap);
        va_end(ap);
        std::fputc('\n', fp);
        std::fflush(fp);
    }

    void write_round(const RoundRecord& rec) {
        if (fp == nullptr) {
            return;
        }
        std::fprintf(fp, "round %d", rec.round);
        if (rec.have_seed) {
            std::fprintf(fp, " seed=0x%016llx", static_cast<unsigned long long>(rec.seed));
        }
        if (rec.have_alloc) {
            std::fprintf(fp,
                         " allocated_bytes=%llu free_after_bytes=%llu",
                         static_cast<unsigned long long>(rec.allocated),
                         static_cast<unsigned long long>(rec.free_after));
        }
        if (rec.host_pass) {
            std::fprintf(fp,
                         " host=PASS h2d=%.1fs %.2fGiB/s d2h=%.1fs %.2fGiB/s",
                         rec.h2d_s,
                         rec.h2d_rate,
                         rec.d2h_s,
                         rec.d2h_rate);
        }
        if (rec.vram_pass) {
            std::fprintf(fp,
                         " vram=PASS clear=%.3fs d2d=%.3fs %.2fGiB/s check=%.1fs %.2fGiB/s",
                         rec.clear_s,
                         rec.d2d_s,
                         rec.d2d_rate,
                         rec.check_s,
                         rec.check_rate);
        }
        if (!rec.fail.empty()) {
            std::fprintf(fp, " FAIL %s", rec.fail.c_str());
        }
        std::fputc('\n', fp);
        std::fflush(fp);
    }
};

struct RoundRecorder {
    Report* report = nullptr;
    RoundRecord rec;
    bool written = false;

    ~RoundRecorder() {
        if (!written && report != nullptr) {
            report->write_round(rec);
        }
    }
};

bool run_round(int round, int rounds, int device, size_t chunk, const std::string& sysfs_path, Report* report) {
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

    std::vector<Block> blocks;
    Staging staging;
    RoundCleanup cleanup{device, &blocks, &staging, true};

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
            char buf[192];
            std::snprintf(buf,
                          sizeof buf,
                          "host mismatch offset=%llu expected=0x%016llx actual=0x%016llx",
                          static_cast<unsigned long long>(mismatch.offset),
                          static_cast<unsigned long long>(mismatch.expected),
                          static_cast<unsigned long long>(mismatch.actual));
            rec.fail = buf;
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
            char buf[256];
            std::snprintf(buf,
                          sizeof buf,
                          "VRAM copy mismatch offset=%llu source_offset=%llu expected=0x%016llx actual=0x%016llx",
                          static_cast<unsigned long long>(copy_mismatch.offset),
                          static_cast<unsigned long long>(copy_mismatch.offset - half),
                          static_cast<unsigned long long>(copy_mismatch.expected),
                          static_cast<unsigned long long>(copy_mismatch.actual));
            rec.fail = buf;
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

    int exit_code = 2;
    int failed = 0;
    bool ran = false;
    std::string setup_fail;
    struct Finish {
        Report* report = nullptr;
        int* exit_code = nullptr;
        int rounds = 0;
        int* failed = nullptr;
        bool* ran = nullptr;
        std::string* setup_fail = nullptr;

        ~Finish() {
            if (report == nullptr || report->fp == nullptr) {
                return;
            }
            if (!*ran) {
                report->line("result: setup failed");
                if (setup_fail != nullptr && !setup_fail->empty()) {
                    report->line("reason: %s", setup_fail->c_str());
                }
            } else if (*failed == 0) {
                report->line("result: %d/%d passed", rounds, rounds);
            } else {
                report->line("result: %d/%d passed, %d failed", rounds - *failed, rounds, *failed);
            }
            report->line("exit_code: %d", *exit_code);
            std::printf("log: %s\n", report->path.c_str());
        }
    } finish{&report, &exit_code, rounds, &failed, &ran, &setup_fail};

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
    std::printf("host memory: total %s / available %s\n",
                format_bytes(mem_total).c_str(),
                format_bytes(mem_avail).c_str());
    if (mem_total > 0 && mem_total < prop.totalGlobalMem) {
        std::printf("host memory is smaller than VRAM, so the test copies in chunks.\n");
    }

    report.line("device: %s", prop.name);
    report.line("arch: %s", prop.gcnArchName);
    report.line("pci: %s", pci.c_str());
    report.line("vram_total_bytes: %llu", static_cast<unsigned long long>(prop.totalGlobalMem));
    report.line("vram_free_bytes: %llu", static_cast<unsigned long long>(free_bytes));
    report.line("host_memory_bytes: %llu", static_cast<unsigned long long>(mem_total));
    report.line("host_available_bytes: %llu", static_cast<unsigned long long>(mem_avail));
    report.line("rounds: %d", rounds);

    size_t chunk = 0;
    if (!choose_chunk(chunk_mib << 20, &chunk, &setup_fail)) {
        return exit_code;
    }
    report.line("chunk_bytes: %llu", static_cast<unsigned long long>(chunk));

    std::printf("rounds: %d  chunk: %s  max leftover: %s\n",
                rounds,
                format_bytes(chunk).c_str(),
                format_bytes(kMaxLeftover).c_str());
    std::fflush(stdout);

    ran = true;
    for (int round = 1; round <= rounds; ++round) {
        if (!run_round(round, rounds, device, chunk, sysfs_path, &report)) {
            ++failed;
        }
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
