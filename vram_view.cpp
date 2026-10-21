// Terminal viewer for vram-test JSON logs.
// English only, matching the rest of this repository.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

struct Json {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json* find(const std::string& key) const {
        if (type != OBJ) {
            return nullptr;
        }
        for (const auto& kv : obj) {
            if (kv.first == key) {
                return &kv.second;
            }
        }
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(const std::string& in) : in_(in) {}

    Json parse() {
        Json root = parse_value();
        skip();
        if (i_ != in_.size()) {
            fail("trailing data");
        }
        return root;
    }

private:
    const std::string& in_;
    size_t i_ = 0;

    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error("invalid JSON: " + message);
    }

    void skip() {
        while (i_ < in_.size() && std::isspace(static_cast<unsigned char>(in_[i_]))) {
            ++i_;
        }
    }

    char peek() {
        skip();
        if (i_ >= in_.size()) {
            fail("unexpected end");
        }
        return in_[i_];
    }

    char getc() {
        char c = peek();
        ++i_;
        return c;
    }

    Json parse_value() {
        const char c = peek();
        if (c == '{') {
            return parse_object();
        }
        if (c == '[') {
            return parse_array();
        }
        if (c == '"') {
            Json j;
            j.type = Json::STR;
            j.str = parse_string();
            return j;
        }
        if (c == 't' || c == 'f') {
            return parse_bool();
        }
        if (c == 'n') {
            return parse_null();
        }
        return parse_number();
    }

    Json parse_object() {
        getc();
        Json j;
        j.type = Json::OBJ;
        if (peek() == '}') {
            getc();
            return j;
        }
        while (true) {
            if (peek() != '"') {
                fail("object key is not a string");
            }
            std::string key = parse_string();
            if (getc() != ':') {
                fail("expected ':'");
            }
            j.obj.emplace_back(std::move(key), parse_value());
            const char c = getc();
            if (c == '}') {
                return j;
            }
            if (c != ',') {
                fail("expected ',' or '}'");
            }
        }
    }

    Json parse_array() {
        getc();
        Json j;
        j.type = Json::ARR;
        if (peek() == ']') {
            getc();
            return j;
        }
        while (true) {
            j.arr.push_back(parse_value());
            const char c = getc();
            if (c == ']') {
                return j;
            }
            if (c != ',') {
                fail("expected ',' or ']'");
            }
        }
    }

    std::string parse_string() {
        if (getc() != '"') {
            fail("expected string");
        }
        std::string out;
        while (i_ < in_.size()) {
            const unsigned char c = static_cast<unsigned char>(in_[i_++]);
            if (c == '"') {
                return out;
            }
            if (c == '\\') {
                if (i_ >= in_.size()) {
                    fail("bad escape");
                }
                const char e = in_[i_++];
                switch (e) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(e);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u':
                    out += parse_unicode();
                    break;
                default:
                    fail("bad escape");
                }
            } else if (c < 0x20) {
                fail("raw control character in string");
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
        fail("unterminated string");
    }

    std::string parse_unicode() {
        if (i_ + 4 > in_.size()) {
            fail("bad unicode escape");
        }
        int code = 0;
        for (int n = 0; n < 4; ++n) {
            const char h = in_[i_++];
            code <<= 4;
            if (h >= '0' && h <= '9') {
                code += h - '0';
            } else if (h >= 'a' && h <= 'f') {
                code += h - 'a' + 10;
            } else if (h >= 'A' && h <= 'F') {
                code += h - 'A' + 10;
            } else {
                fail("bad unicode escape");
            }
        }
        std::string out;
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        return out;
    }

    Json parse_bool() {
        Json j;
        j.type = Json::BOOL;
        if (in_.compare(i_, 4, "true") == 0) {
            i_ += 4;
            j.b = true;
            return j;
        }
        if (in_.compare(i_, 5, "false") == 0) {
            i_ += 5;
            j.b = false;
            return j;
        }
        fail("expected boolean");
    }

    Json parse_null() {
        if (in_.compare(i_, 4, "null") != 0) {
            fail("expected null");
        }
        i_ += 4;
        Json j;
        j.type = Json::NUL;
        return j;
    }

    Json parse_number() {
        const size_t start = i_;
        if (i_ < in_.size() && (in_[i_] == '-' || in_[i_] == '+')) {
            ++i_;
        }
        bool any = false;
        while (i_ < in_.size() && std::isdigit(static_cast<unsigned char>(in_[i_]))) {
            any = true;
            ++i_;
        }
        if (i_ < in_.size() && in_[i_] == '.') {
            ++i_;
            while (i_ < in_.size() && std::isdigit(static_cast<unsigned char>(in_[i_]))) {
                any = true;
                ++i_;
            }
        }
        if (i_ < in_.size() && (in_[i_] == 'e' || in_[i_] == 'E')) {
            ++i_;
            if (i_ < in_.size() && (in_[i_] == '-' || in_[i_] == '+')) {
                ++i_;
            }
            while (i_ < in_.size() && std::isdigit(static_cast<unsigned char>(in_[i_]))) {
                ++i_;
            }
        }
        if (!any) {
            fail("expected number");
        }
        Json j;
        j.type = Json::NUM;
        try {
            j.num = std::stod(in_.substr(start, i_ - start));
        } catch (const std::exception&) {
            fail("number out of range");
        }
        return j;
    }
};

int utf8_cols(const std::string& text) {
    int n = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

std::string fit(const std::string& text, int width) {
    if (width <= 0) {
        return "";
    }
    if (utf8_cols(text) <= width) {
        return text;
    }
    if (width == 1) {
        return "…";
    }
    std::string out;
    int cols = 0;
    for (size_t i = 0; i < text.size();) {
        const size_t start = i;
        ++i;
        while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
            ++i;
        }
        if (cols + 1 > width - 1) {
            break;
        }
        out.append(text, start, i - start);
        ++cols;
    }
    out += "…";
    return out;
}

std::string trim_zeros(const std::string& text) {
    const size_t dot = text.find('.');
    if (dot == std::string::npos) {
        return text;
    }
    size_t end = text.size();
    while (end > dot + 1 && text[end - 1] == '0') {
        --end;
    }
    if (end > 0 && text[end - 1] == '.') {
        --end;
    }
    return text.substr(0, end);
}

std::string format_bytes(uint64_t n) {
    char buf[64];
    const char* unit = "B";
    double value = static_cast<double>(n);
    if (n >= 1024ull * 1024ull * 1024ull) {
        value /= 1024.0 * 1024.0 * 1024.0;
        unit = "GiB";
    } else if (n >= 1024ull * 1024ull) {
        value /= 1024.0 * 1024.0;
        unit = "MiB";
    } else if (n >= 1024ull) {
        value /= 1024.0;
        unit = "KiB";
    } else {
        std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(n));
        return buf;
    }
    std::snprintf(buf, sizeof buf, "%.2f", value);
    return trim_zeros(buf) + " " + unit;
}

std::string pad_left(std::string text, int width) {
    const int n = utf8_cols(text);
    if (n < width) {
        text.insert(0, static_cast<size_t>(width - n), ' ');
    }
    return text;
}

std::string pad_right(std::string text, int width) {
    const int n = utf8_cols(text);
    if (n < width) {
        text.append(static_cast<size_t>(width - n), ' ');
    }
    return text;
}

std::string place_center(const std::string& text, int width) {
    const int n = utf8_cols(text);
    if (n >= width) {
        return text;
    }
    const int left = (width - n) / 2;
    return std::string(static_cast<size_t>(left), ' ') + text + std::string(static_cast<size_t>(width - n - left), ' ');
}

std::string format_rate(double gib_s) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%7.2f", gib_s);
    return buf;
}

std::string format_seconds(double seconds) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%7.3f s", seconds);
    return buf;
}

const char* kEighths[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

double nice_scale(double peak, const double* steps, int count) {
    const double need = peak <= 0 ? steps[0] : peak * 1.08;
    for (int i = 0; i < count; ++i) {
        if (steps[i] >= need) {
            return steps[i];
        }
    }
    double scale = steps[count - 1];
    while (scale < need) {
        scale *= 2;
    }
    return scale;
}

std::string scale_label(double scale) {
    char buf[32];
    if (scale >= 100) {
        std::snprintf(buf, sizeof buf, "%.0f", scale);
    } else if (scale >= 10) {
        std::snprintf(buf, sizeof buf, "%.0f", scale);
    } else {
        std::snprintf(buf, sizeof buf, "%.1f", scale);
    }
    return buf;
}

struct HostCopy {
    bool present = false;
    double h2d_s = 0;
    double h2d = 0;
    double d2h_s = 0;
    double d2h = 0;
};

struct VramCopy {
    bool present = false;
    double clear_s = 0;
    double d2d_s = 0;
    double d2d = 0;
    double check_s = 0;
    double check = 0;
};

struct Mismatch {
    bool present = false;
    uint64_t offset = 0;
    bool have_source = false;
    uint64_t source = 0;
    std::string expected;
    std::string actual;
};

struct TempSensor {
    std::string name;
    bool have_before = false;
    bool have_after = false;
    int64_t before_mc = 0;
    int64_t after_mc = 0;
    bool have_critical = false;
    int64_t critical_mc = 0;
    bool have_emergency = false;
    int64_t emergency_mc = 0;
};

struct Round {
    int round = 0;
    std::string seed;
    bool have_alloc = false;
    uint64_t allocated = 0;
    uint64_t free_after = 0;
    HostCopy host;
    VramCopy vram;
    std::string error;
    Mismatch mismatch;
    std::vector<TempSensor> temperature;
    std::string result;
};

struct Report {
    std::string path;
    std::string program;
    std::string time;
    std::string host;
    std::string kernel;
    std::string hip;
    std::string device;
    std::string arch;
    std::string pci;
    bool have_vram_total = false;
    uint64_t vram_total = 0;
    bool have_vram_free = false;
    uint64_t vram_free = 0;
    bool have_host_memory = false;
    uint64_t host_memory = 0;
    bool have_host_available = false;
    uint64_t host_available = 0;
    bool have_rounds = false;
    int rounds = 0;
    bool have_chunk = false;
    uint64_t chunk = 0;
    std::vector<Round> tests;
    int passed = 0;
    int failed = 0;
    std::string result;
    std::string reason;
    bool have_exit = false;
    int exit_code = 0;
};

std::string jstr(const Json* j) {
    if (j == nullptr) {
        return "";
    }
    if (j->type == Json::STR) {
        return j->str;
    }
    return "";
}

double jnum(const Json* j, double fallback = 0) {
    if (j != nullptr && j->type == Json::NUM) {
        return j->num;
    }
    return fallback;
}

uint64_t ju64(const Json* j) {
    const double n = jnum(j, -1);
    if (n < 0) {
        return 0;
    }
    return static_cast<uint64_t>(n);
}

HostCopy read_host(const Json* j) {
    HostCopy h;
    if (j == nullptr || j->type != Json::OBJ) {
        return h;
    }
    h.present = true;
    h.h2d_s = jnum(j->find("h2d_seconds"));
    h.h2d = jnum(j->find("h2d_gib_s"));
    h.d2h_s = jnum(j->find("d2h_seconds"));
    h.d2h = jnum(j->find("d2h_gib_s"));
    return h;
}

VramCopy read_vram(const Json* j) {
    VramCopy v;
    if (j == nullptr || j->type != Json::OBJ) {
        return v;
    }
    v.present = true;
    v.clear_s = jnum(j->find("clear_seconds"));
    v.d2d_s = jnum(j->find("d2d_seconds"));
    v.d2d = jnum(j->find("d2d_gib_s"));
    v.check_s = jnum(j->find("check_seconds"));
    v.check = jnum(j->find("check_gib_s"));
    return v;
}

int64_t jmilli(const Json* j) {
    return static_cast<int64_t>(jnum(j));
}

std::vector<TempSensor> read_temperature(const Json* j) {
    std::vector<TempSensor> out;
    if (j == nullptr || j->type != Json::ARR) {
        return out;
    }
    for (const Json& item : j->arr) {
        if (item.type != Json::OBJ) {
            continue;
        }
        TempSensor sensor;
        sensor.name = jstr(item.find("name"));
        if (item.find("before_millicelsius") != nullptr) {
            sensor.have_before = true;
            sensor.before_mc = jmilli(item.find("before_millicelsius"));
        }
        if (item.find("after_millicelsius") != nullptr) {
            sensor.have_after = true;
            sensor.after_mc = jmilli(item.find("after_millicelsius"));
        }
        if (item.find("critical_millicelsius") != nullptr) {
            sensor.have_critical = true;
            sensor.critical_mc = jmilli(item.find("critical_millicelsius"));
        }
        if (item.find("emergency_millicelsius") != nullptr) {
            sensor.have_emergency = true;
            sensor.emergency_mc = jmilli(item.find("emergency_millicelsius"));
        }
        if (!sensor.name.empty() && (sensor.have_before || sensor.have_after)) {
            out.push_back(std::move(sensor));
        }
    }
    return out;
}

Mismatch read_mismatch(const Json* j) {
    Mismatch m;
    if (j == nullptr || j->type != Json::OBJ) {
        return m;
    }
    m.present = true;
    m.offset = ju64(j->find("offset"));
    if (j->find("source_offset") != nullptr) {
        m.have_source = true;
        m.source = ju64(j->find("source_offset"));
    }
    m.expected = jstr(j->find("expected"));
    m.actual = jstr(j->find("actual"));
    return m;
}

Round read_round(const Json& j) {
    Round r;
    r.round = static_cast<int>(jnum(j.find("round")));
    r.seed = jstr(j.find("seed"));
    if (j.find("allocated_bytes") != nullptr) {
        r.have_alloc = true;
        r.allocated = ju64(j.find("allocated_bytes"));
        r.free_after = ju64(j.find("free_after_bytes"));
    }
    r.host = read_host(j.find("host"));
    r.vram = read_vram(j.find("vram"));
    r.error = jstr(j.find("error"));
    r.mismatch = read_mismatch(j.find("mismatch"));
    r.temperature = read_temperature(j.find("temperature"));
    r.result = jstr(j.find("result"));
    return r;
}

Report read_report(const Json& root, const std::string& path) {
    if (root.type != Json::OBJ) {
        throw std::runtime_error("JSON root is not an object");
    }
    Report r;
    r.path = path;
    r.program = jstr(root.find("program"));
    r.time = jstr(root.find("time"));
    r.host = jstr(root.find("host"));
    r.kernel = jstr(root.find("kernel"));
    r.hip = jstr(root.find("hip"));
    r.device = jstr(root.find("device"));
    r.arch = jstr(root.find("arch"));
    r.pci = jstr(root.find("pci"));
    if (root.find("vram_total_bytes") != nullptr) {
        r.have_vram_total = true;
        r.vram_total = ju64(root.find("vram_total_bytes"));
    }
    if (root.find("vram_free_bytes") != nullptr) {
        r.have_vram_free = true;
        r.vram_free = ju64(root.find("vram_free_bytes"));
    }
    if (root.find("host_memory_bytes") != nullptr) {
        r.have_host_memory = true;
        r.host_memory = ju64(root.find("host_memory_bytes"));
    }
    if (root.find("host_available_bytes") != nullptr) {
        r.have_host_available = true;
        r.host_available = ju64(root.find("host_available_bytes"));
    }
    if (root.find("rounds") != nullptr) {
        r.have_rounds = true;
        r.rounds = static_cast<int>(jnum(root.find("rounds")));
    }
    if (root.find("chunk_bytes") != nullptr) {
        r.have_chunk = true;
        r.chunk = ju64(root.find("chunk_bytes"));
    }
    if (const Json* tests = root.find("tests")) {
        if (tests->type == Json::ARR) {
            for (const Json& item : tests->arr) {
                if (item.type == Json::OBJ) {
                    r.tests.push_back(read_round(item));
                }
            }
        }
    }
    r.passed = static_cast<int>(jnum(root.find("passed")));
    r.failed = static_cast<int>(jnum(root.find("failed")));
    r.result = jstr(root.find("result"));
    r.reason = jstr(root.find("reason"));
    if (root.find("exit_code") != nullptr) {
        r.have_exit = true;
        r.exit_code = static_cast<int>(jnum(root.find("exit_code")));
    }
    return r;
}

struct Style {
    bool color = false;
    int width = 80;

    const char* code(const char* esc) const { return color ? esc : ""; }

    std::string paint(const std::string& text, const char* esc) const {
        if (!color || esc == nullptr || esc[0] == '\0') {
            return text;
        }
        return std::string(esc) + text + "\033[0m";
    }
};

constexpr const char* kReset = "\033[0m";
constexpr const char* kBold = "\033[1m";
constexpr const char* kDim = "\033[2m";
constexpr const char* kRed = "\033[31m";
constexpr const char* kGreen = "\033[32m";
constexpr const char* kYellow = "\033[33m";
constexpr const char* kMagenta = "\033[35m";
constexpr const char* kCyan = "\033[36m";
constexpr const char* kDimRed = "\033[2;31m";

std::string repeat_cols(const std::string& unit, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) {
        out += unit;
    }
    return out;
}

void rule(const Style& style, const std::string& left, const std::string& right) {
    const int dashes = style.width - utf8_cols(left) - utf8_cols(right);
    std::cout << style.paint(left, kDim) << style.paint(repeat_cols("─", std::max(1, dashes)), kDim)
              << style.paint(right, kDim) << "\n";
}

void box_line(const Style& style, const std::string& plain, const std::string& shown) {
    const int inner = style.width - 4;
    const int pad = std::max(0, inner - utf8_cols(plain));
    std::cout << style.paint("│", kDim) << " " << shown << std::string(static_cast<size_t>(pad), ' ') << " "
              << style.paint("│", kDim) << "\n";
}

std::string status_word(const Report& report) {
    if (report.result == "passed") {
        return "PASSED";
    }
    if (report.result == "failed") {
        return "FAILED";
    }
    if (report.result == "setup_failed") {
        return "SETUP FAILED";
    }
    if (report.result == "in_progress") {
        return "IN PROGRESS";
    }
    if (report.result.empty()) {
        return "UNKNOWN";
    }
    std::string word = report.result;
    for (char& c : word) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return word;
}

const char* status_color(const Report& report) {
    if (report.result == "passed") {
        return kGreen;
    }
    if (report.result == "in_progress") {
        return kYellow;
    }
    return kRed;
}

std::string meter(const Style& style, double value, double scale, int width, const char* fill_color) {
    if (width < 1) {
        width = 1;
    }
    double frac = 0;
    if (scale > 0) {
        frac = value / scale;
    }
    if (frac < 0) {
        frac = 0;
    }
    if (frac > 1) {
        frac = 1;
    }
    const double cells = frac * width;
    const int full = static_cast<int>(cells);
    const double rem = cells - full;
    std::string fill;
    fill += repeat_cols("█", full);
    std::string rest;
    if (full < width) {
        const int eighth = static_cast<int>(std::floor(rem * 8.0 + 0.5));
        if (eighth >= 8) {
            fill += "█";
        } else if (eighth <= 0) {
            rest += "░";
        } else {
            fill += kEighths[eighth];
        }
        const int used = full + 1;
        if (used < width) {
            rest += repeat_cols("░", width - used);
        }
    }
    return style.paint("▕", kDim) + style.paint(fill, fill_color) + style.paint(rest, kDim) + style.paint("▏", kDim);
}

std::string vcell(double value, double scale, int row, int height, const char* solid) {
    double fill = 0;
    if (scale > 0) {
        fill = value / scale * height;
    }
    if (fill < 0) {
        fill = 0;
    }
    if (fill > height) {
        fill = height;
    }
    const int full = static_cast<int>(std::floor(fill));
    const double frac = fill - full;
    if (row < full) {
        return solid;
    }
    if (row == full && frac > 0) {
        int eighth = static_cast<int>(std::floor(frac * 8.0 + 0.5));
        if (eighth < 1) {
            eighth = 1;
        }
        if (eighth > 8) {
            eighth = 8;
        }
        if (eighth == 8) {
            return solid;
        }
        return kEighths[eighth];
    }
    return " ";
}

struct Sample {
    bool present = false;
    int round = 0;
    double value = 0;
};

struct Stats {
    int n = 0;
    double min = 0;
    double max = 0;
    double sum = 0;
    int peak_round = 0;

    void add(int round, double value) {
        if (n == 0 || value < min) {
            min = value;
        }
        if (n == 0 || value > max) {
            max = value;
            peak_round = round;
        }
        sum += value;
        ++n;
    }

    double avg() const { return n == 0 ? 0 : sum / n; }
};

void collect(const std::vector<Round>& tests, bool (*present)(const Round&), double (*value)(const Round&), std::vector<Sample>* out, Stats* stats) {
    out->clear();
    *stats = Stats{};
    for (const Round& round : tests) {
        Sample s;
        s.round = round.round;
        s.present = present(round);
        if (s.present) {
            s.value = value(round);
            stats->add(round.round, s.value);
        }
        out->push_back(s);
    }
}

bool host_present(const Round& r) { return r.host.present; }
double h2d_of(const Round& r) { return r.host.h2d; }
double d2h_of(const Round& r) { return r.host.d2h; }
bool vram_present(const Round& r) { return r.vram.present; }
double d2d_of(const Round& r) { return r.vram.d2d; }
double check_of(const Round& r) { return r.vram.check; }

void print_chart(const Style& style,
                 const std::string& title,
                 double scale,
                 const std::vector<Sample>& left,
                 const char* left_name,
                 const char* left_color,
                 const std::vector<Sample>* right,
                 const char* right_name,
                 const char* right_color) {
    const int height = 8;
    const bool pair = right != nullptr;
    const int bar_w = pair ? 2 : 1;
    int label_w = 1;
    for (const Sample& sample : left) {
        label_w = std::max(label_w, utf8_cols(std::to_string(sample.round)));
    }
    const int group = std::max(bar_w, label_w);
    const int gap = pair ? 2 : 1;
    std::string title_plain = "  " + title;
    std::string scale_plain = "scale " + scale_label(scale) + " GiB/s";
    const int room = style.width - utf8_cols(title_plain) - 1;
    if (utf8_cols(scale_plain) <= room) {
        const int spaces = style.width - utf8_cols(title_plain) - utf8_cols(scale_plain);
        std::cout << style.paint(title_plain, kBold) << std::string(static_cast<size_t>(std::max(1, spaces)), ' ')
                  << style.paint(scale_plain, kDim) << "\n";
    } else {
        std::cout << style.paint(title_plain, kBold) << "\n";
    }

    const int axis = 7;
    auto y_label = [&](int row) {
        std::string text;
        bool tick = false;
        if (row == height - 1) {
            text = scale_label(scale);
            tick = true;
        } else if (row == height / 2 - 1) {
            text = scale_label(scale / 2);
            tick = true;
        }
        text = pad_left(text, axis - 2);
        return text + (tick ? " ┤" : " │");
    };

    for (int row = height - 1; row >= 0; --row) {
        std::string axis_plain = y_label(row);
        std::string body_shown;
        for (size_t i = 0; i < left.size(); ++i) {
            const std::string a = left[i].present ? vcell(left[i].value, scale, row, height, "█") : " ";
            std::string cell = style.paint(a, left_color);
            std::string plain = a;
            if (pair) {
                const Sample& s = (*right)[i];
                const std::string b = s.present ? vcell(s.value, scale, row, height, "▓") : " ";
                plain += b;
                cell += style.paint(b, right_color);
            }
            const int extra = group - utf8_cols(plain);
            if (extra > 0) {
                const int lead = extra / 2;
                cell = std::string(static_cast<size_t>(lead), ' ') + cell + std::string(static_cast<size_t>(extra - lead), ' ');
            }
            body_shown += cell;
            if (i + 1 != left.size()) {
                body_shown += std::string(static_cast<size_t>(gap), ' ');
            }
        }
        std::cout << style.paint(axis_plain, kDim) << body_shown << "\n";
    }

    std::string labels = pad_left("", axis - 2) + " ┴";
    std::string ticks;
    for (size_t i = 0; i < left.size(); ++i) {
        ticks += place_center(std::to_string(left[i].round), group);
        if (i + 1 != left.size()) {
            ticks += std::string(static_cast<size_t>(gap), ' ');
        }
    }
    const std::string baseline = repeat_cols("─", std::max(1, utf8_cols(ticks)));
    std::cout << style.paint(labels, kDim) << style.paint(baseline, kDim) << "\n";
    std::cout << std::string(static_cast<size_t>(axis), ' ') << style.paint(ticks, kDim) << "\n";

    std::string legend = std::string(static_cast<size_t>(axis), ' ') + style.paint("█", left_color) + " " + left_name;
    if (pair) {
        legend += std::string("    ") + style.paint("▓", right_color) + " " + right_name;
    }
    std::cout << legend << "\n";
}

std::string join_meta(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& part : parts) {
        if (part.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += " · ";
        }
        out += part;
    }
    return out;
}

void print_rate_row(const Style& style, const char* name, double rate, double seconds, double scale, const char* color) {
    const std::string prefix = std::string("  ") + name + "  " + format_rate(rate) + " GiB/s  " + format_seconds(seconds) + "  ";
    const int bar = std::max(8, style.width - utf8_cols(prefix) - 2);
    std::cout << "  " << name << "  " << style.paint(format_rate(rate), color) << " GiB/s  " << style.paint(format_seconds(seconds), kDim)
              << "  " << meter(style, rate, scale, bar, color) << "\n";
}

std::string format_celsius(int64_t millicelsius) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f°C", static_cast<double>(millicelsius) / 1000.0);
    return buf;
}

std::string temp_margin(int64_t value, int64_t limit, const char* what) {
    char buf[80];
    if (value >= limit) {
        std::snprintf(buf,
                      sizeof buf,
                      "OVER %s by %s",
                      what,
                      format_celsius(value - limit).c_str());
    } else {
        std::snprintf(buf,
                      sizeof buf,
                      "%s under %s",
                      format_celsius(limit - value).c_str(),
                      what);
    }
    return buf;
}

int meter_index(int64_t mark, int64_t scale, int width) {
    if (scale <= 0 || width <= 0) {
        return -1;
    }
    int index = static_cast<int>((static_cast<double>(mark) / static_cast<double>(scale)) * width);
    if (index < 0) {
        index = 0;
    }
    if (index >= width) {
        index = width - 1;
    }
    return index;
}

// Scale is 0°C through emergency. ┃ is critical. The right cap is emergency
// until the reading passes it, when the scale grows and │ marks emergency.
void print_temp_sensor(const Style& style, const TempSensor& sensor, bool* emergency_inside) {
    const bool have_shown = sensor.have_after || sensor.have_before;
    if (!have_shown) {
        return;
    }
    const int64_t shown = sensor.have_after ? sensor.after_mc : sensor.before_mc;
    std::string reading;
    if (sensor.have_before && sensor.have_after && sensor.before_mc != sensor.after_mc) {
        char buf[64];
        std::snprintf(buf,
                      sizeof buf,
                      "%.1f → %s",
                      static_cast<double>(sensor.before_mc) / 1000.0,
                      format_celsius(sensor.after_mc).c_str());
        reading = buf;
    } else {
        reading = format_celsius(shown);
    }

    std::string margin_plain;
    std::string margin_shown;
    auto add_margin = [&](const std::string& text, bool over) {
        if (!margin_plain.empty()) {
            margin_plain += "   ";
            margin_shown += "   ";
        }
        margin_plain += text;
        margin_shown += style.paint(text, over ? kRed : kGreen);
    };
    const bool over_emergency = sensor.have_emergency && shown >= sensor.emergency_mc;
    const bool over_critical = sensor.have_critical && shown >= sensor.critical_mc;
    if (over_emergency) {
        add_margin(temp_margin(shown, sensor.emergency_mc, "emergency"), true);
    } else {
        if (sensor.have_critical) {
            add_margin(temp_margin(shown, sensor.critical_mc, "critical"), over_critical);
        }
        if (sensor.have_emergency) {
            add_margin(temp_margin(shown, sensor.emergency_mc, "emergency"), false);
        }
    }
    const std::string name = pad_right(sensor.name, 10);
    std::cout << "  " << name << "  " << style.paint(reading, over_critical ? kRed : kBold)
              << (margin_shown.empty() ? "" : "   " + margin_shown) << "\n";

    if (!sensor.have_critical && !sensor.have_emergency) {
        return;
    }
    int64_t scale = shown > 0 ? shown : 1;
    bool emergency_at_end = false;
    if (sensor.have_emergency) {
        if (shown <= sensor.emergency_mc) {
            scale = sensor.emergency_mc > 0 ? sensor.emergency_mc : 1;
            emergency_at_end = true;
        } else {
            scale = shown;
            if (emergency_inside != nullptr) {
                *emergency_inside = true;
            }
        }
    } else if (sensor.have_critical && shown <= sensor.critical_mc) {
        scale = sensor.critical_mc > 0 ? sensor.critical_mc : 1;
    }
    const int inner = std::max(16, style.width - 4);
    const int crit_i = sensor.have_critical && sensor.critical_mc < scale ? meter_index(sensor.critical_mc, scale, inner) : -1;
    int emerg_i = -1;
    if (sensor.have_emergency && !emergency_at_end && sensor.emergency_mc < scale) {
        emerg_i = meter_index(sensor.emergency_mc, scale, inner);
        if (emerg_i > inner - 2) {
            emerg_i = inner - 2;
        }
        if (emerg_i == crit_i && emerg_i + 1 < inner) {
            ++emerg_i;
        }
    }

    std::string bar = style.paint("▕", kDim);
    for (int i = 0; i < inner; ++i) {
        const double center = (static_cast<double>(i) + 0.5) / static_cast<double>(inner) * static_cast<double>(scale);
        if (i == crit_i) {
            bar += style.paint("┃", kYellow);
            continue;
        }
        if (i == emerg_i) {
            bar += style.paint("│", kYellow);
            continue;
        }
        const bool past_mark = emerg_i >= 0 && i > emerg_i;
        if (center < static_cast<double>(shown) || past_mark) {
            const bool past_emergency = past_mark || (sensor.have_emergency && center >= static_cast<double>(sensor.emergency_mc));
            const bool past_critical = sensor.have_critical && center >= static_cast<double>(sensor.critical_mc);
            if (past_emergency) {
                bar += style.paint("▓", kRed);
            } else if (past_critical) {
                bar += style.paint(over_emergency ? "█" : "▓", kRed);
            } else {
                bar += style.paint("█", over_critical ? kRed : kGreen);
            }
        } else {
            bar += style.paint("░", kDim);
        }
    }
    const char* cap_color = emergency_at_end ? kYellow : kDim;
    bar += style.paint("▏", cap_color);
    std::cout << "  " << bar << "\n";
}

void print_temperatures(const Style& style, const std::vector<TempSensor>& sensors) {
    if (sensors.empty()) {
        return;
    }
    bool emergency_inside = false;
    bool any_critical = false;
    bool any_emergency = false;
    for (const TempSensor& sensor : sensors) {
        if (sensor.have_critical) {
            any_critical = true;
        }
        if (sensor.have_emergency) {
            any_emergency = true;
        }
        print_temp_sensor(style, sensor, &emergency_inside);
    }
    if (!any_critical && !any_emergency) {
        return;
    }
    std::cout << "  ";
    if (any_critical) {
        std::cout << style.paint("┃", kYellow) << style.paint(" critical", kDim);
    }
    if (any_emergency) {
        if (any_critical) {
            std::cout << "    ";
        }
        std::cout << style.paint(emergency_inside ? "│" : "▏", kYellow) << style.paint(" emergency", kDim);
    }
    std::cout << "\n";
}

struct HistorySeries {
    std::string name;
    std::vector<double> samples;
    std::vector<int> rounds;
    const char* color = kCyan;
    bool have_critical = false;
    double critical = 0;
};

std::string format_axis_value(double value) {
    char buf[16];
    if (std::fabs(value) >= 100.0) {
        std::snprintf(buf, sizeof buf, "%.0f", value);
    } else {
        std::snprintf(buf, sizeof buf, "%.1f", value);
    }
    return buf;
}

// One line per series across the rounds. Critical values, when present, are dashed.
void draw_history(const Style& style, const char* title, const char* subtitle, const std::vector<HistorySeries>& series, int rows, double min_span) {
    if (series.empty() || rows < 2) {
        return;
    }
    double y_min = series[0].samples[0];
    double y_max = y_min;
    bool any_critical = false;
    for (const HistorySeries& item : series) {
        for (double sample : item.samples) {
            y_min = std::min(y_min, sample);
            y_max = std::max(y_max, sample);
        }
        if (item.have_critical) {
            any_critical = true;
            y_max = std::max(y_max, item.critical);
        }
    }
    if (y_max - y_min < min_span) {
        const double mid = (y_max + y_min) / 2.0;
        y_min = mid - min_span / 2.0;
        y_max = mid + min_span / 2.0;
    }
    const double pad = std::max((y_max - y_min) * 0.08, min_span * 0.05);
    y_min -= pad;
    y_max += pad;
    if (y_max <= y_min) {
        y_max = y_min + 1;
    }

    int axis = 6;
    axis = std::max(axis, utf8_cols(format_axis_value(y_min)) + 2);
    axis = std::max(axis, utf8_cols(format_axis_value(y_max)) + 2);
    const int label_w = 16;
    int cols = style.width - axis - 1 - label_w;
    if (cols < 20) {
        cols = std::max(16, style.width - axis);
    }

    bool rounds_unique = true;
    int round_lo = 0;
    int round_hi = 0;
    bool have_round = false;
    for (const HistorySeries& item : series) {
        for (size_t i = 0; i < item.rounds.size(); ++i) {
            if (i > 0 && item.rounds[i] == item.rounds[i - 1]) {
                rounds_unique = false;
            }
            if (!have_round || item.rounds[i] < round_lo) {
                round_lo = item.rounds[i];
            }
            if (!have_round || item.rounds[i] > round_hi) {
                round_hi = item.rounds[i];
            }
            have_round = true;
        }
    }

    struct Cell {
        int series = -1;
        std::string glyph;
    };
    std::vector<std::vector<Cell>> grid(static_cast<size_t>(rows), std::vector<Cell>(static_cast<size_t>(cols)));
    auto y_of = [&](double value) {
        double t = (value - y_min) / (y_max - y_min);
        if (t < 0) {
            t = 0;
        }
        if (t > 1) {
            t = 1;
        }
        int y = static_cast<int>(std::lround((1.0 - t) * (rows - 1)));
        if (y < 0) {
            y = 0;
        }
        if (y >= rows) {
            y = rows - 1;
        }
        return y;
    };
    auto x_of = [&](int index, int count, int round) {
        if (cols <= 1) {
            return 0;
        }
        double t = 0;
        if (rounds_unique && round_hi > round_lo) {
            t = static_cast<double>(round - round_lo) / static_cast<double>(round_hi - round_lo);
        } else if (count > 1) {
            t = static_cast<double>(index) / static_cast<double>(count - 1);
        }
        int x = static_cast<int>(std::lround(t * (cols - 1)));
        if (x < 0) {
            x = 0;
        }
        if (x >= cols) {
            x = cols - 1;
        }
        return x;
    };
    auto place = [&](int row, int col, int series_index, const std::string& glyph) {
        if (row < 0 || col < 0 || row >= rows || col >= cols) {
            return;
        }
        Cell& cell = grid[static_cast<size_t>(row)][static_cast<size_t>(col)];
        if (cell.series < 0) {
            cell.series = series_index;
            cell.glyph = glyph;
        } else if (cell.series != series_index) {
            cell.series = -2;
            cell.glyph = "●";
        }
    };

    std::vector<int> end_row(series.size(), -1);
    for (size_t s = 0; s < series.size(); ++s) {
        const HistorySeries& item = series[s];
        const int count = static_cast<int>(item.samples.size());
        int prev_c = x_of(0, count, item.rounds.empty() ? 0 : item.rounds[0]);
        int prev_r = y_of(item.samples[0]);
        place(prev_r, prev_c, static_cast<int>(s), "─");
        for (int i = 1; i < count; ++i) {
            const int next_c = x_of(i, count, item.rounds[static_cast<size_t>(i)]);
            const int next_r = y_of(item.samples[static_cast<size_t>(i)]);
            const int c0 = prev_c;
            const int r0 = prev_r;
            const int span = std::max(1, next_c - c0);
            for (int step = 1; step <= span; ++step) {
                const int col = c0 + (next_c - c0) * step / span;
                const int row = r0 + (next_r - r0) * step / span;
                if (col == prev_c && row == prev_r) {
                    continue;
                }
                std::string glyph = "─";
                if (row < prev_r) {
                    place(prev_r, col, static_cast<int>(s), "╯");
                    for (int mid = prev_r - 1; mid > row; --mid) {
                        place(mid, col, static_cast<int>(s), "│");
                    }
                    glyph = "╰";
                } else if (row > prev_r) {
                    place(prev_r, col, static_cast<int>(s), "╮");
                    for (int mid = prev_r + 1; mid < row; ++mid) {
                        place(mid, col, static_cast<int>(s), "│");
                    }
                    glyph = "╭";
                }
                place(row, col, static_cast<int>(s), glyph);
                prev_r = row;
                prev_c = col;
            }
        }
        end_row[s] = prev_r;
    }

    struct CritMark {
        double value = 0;
        std::string name;
    };
    std::vector<std::vector<CritMark>> crits(static_cast<size_t>(rows));
    for (const HistorySeries& item : series) {
        if (!item.have_critical) {
            continue;
        }
        CritMark mark;
        mark.value = item.critical;
        mark.name = item.name;
        crits[static_cast<size_t>(y_of(item.critical))].push_back(mark);
    }

    std::cout << style.paint(std::string("  ") + title, kBold) << style.paint(std::string("   ") + subtitle, kDim) << "\n";
    for (int row = 0; row < rows; ++row) {
        const bool has_crit = !crits[static_cast<size_t>(row)].empty();
        const bool label_row = row == 0 || row == rows - 1 || has_crit;
        std::string axis_text;
        if (label_row) {
            double shown = y_min + (1.0 - static_cast<double>(row) / (rows - 1)) * (y_max - y_min);
            if (has_crit) {
                shown = crits[static_cast<size_t>(row)][0].value;
            } else if (row == 0) {
                shown = y_max;
            } else if (row == rows - 1) {
                shown = y_min;
            }
            axis_text = format_axis_value(shown);
        }
        axis_text = pad_left(axis_text, axis - 2);
        std::cout << style.paint(axis_text + (label_row ? " ┤" : " │"), kDim);
        for (int col = 0; col < cols; ++col) {
            const Cell& cell = grid[static_cast<size_t>(row)][static_cast<size_t>(col)];
            if (cell.series >= 0) {
                std::cout << style.paint(cell.glyph, series[static_cast<size_t>(cell.series)].color);
            } else if (cell.series == -2) {
                std::cout << style.paint(cell.glyph, kBold);
            } else if (has_crit) {
                std::cout << style.paint("┈", kDimRed);
            } else {
                std::cout << " ";
            }
        }
        if (has_crit) {
            std::string note;
            for (const CritMark& mark : crits[static_cast<size_t>(row)]) {
                if (!note.empty()) {
                    note += ", ";
                }
                note += mark.name;
            }
            std::cout << " " << style.paint(fit(note, label_w), kDimRed);
        } else {
            std::string shown;
            int used = axis + cols + 1;
            for (size_t s = 0; s < series.size(); ++s) {
                if (end_row[s] != row) {
                    continue;
                }
                const int gap = shown.empty() ? 0 : 1;
                const int need = gap + utf8_cols(series[s].name);
                if (used + need > style.width) {
                    break;
                }
                if (gap != 0) {
                    shown += " ";
                }
                shown += style.paint(series[s].name, series[s].color);
                used += need;
            }
            if (!shown.empty()) {
                std::cout << " " << shown;
            }
        }
        std::cout << "\n";
    }
    std::cout << style.paint(std::string(static_cast<size_t>(axis - 2), ' ') + " └" + repeat_cols("─", cols), kDim) << "\n";

    std::string ticks(static_cast<size_t>(cols), ' ');
    const HistorySeries& ticks_from = series[0];
    int last_round = -1;
    const int tick_count = static_cast<int>(ticks_from.samples.size());
    for (size_t i = 0; i < ticks_from.rounds.size(); ++i) {
        if (ticks_from.rounds[i] == last_round) {
            continue;
        }
        last_round = ticks_from.rounds[i];
        int col = x_of(static_cast<int>(i), tick_count, last_round);
        const std::string num = std::to_string(last_round);
        if (col < 0 || col >= cols) {
            continue;
        }
        if (col + static_cast<int>(num.size()) > cols) {
            col = cols - static_cast<int>(num.size());
        }
        bool free = true;
        for (size_t k = 0; k < num.size(); ++k) {
            if (ticks[static_cast<size_t>(col + static_cast<int>(k))] != ' ') {
                free = false;
            }
        }
        if (!free) {
            continue;
        }
        for (size_t k = 0; k < num.size(); ++k) {
            ticks[static_cast<size_t>(col + static_cast<int>(k))] = num[k];
        }
    }
    std::cout << std::string(static_cast<size_t>(axis), ' ') << style.paint(ticks, kDim) << "\n";

    std::cout << std::string(static_cast<size_t>(axis), ' ');
    for (const HistorySeries& item : series) {
        std::cout << style.paint("── ", item.color) << item.name << "   ";
    }
    if (any_critical) {
        std::cout << style.paint("┈┈ ", kDimRed) << "critical";
    }
    std::cout << "\n\n";
}

// Draw each sensor from the first sample of the run to the last.
// The horizontal axis is round order. Critical limits are dashed lines.
void print_temperature_history(const Style& style, const Report& report) {
    std::vector<HistorySeries> series;
    const char* palette[] = {kCyan, kYellow, kMagenta, kGreen};
    auto find_series = [&series](const std::string& name) -> HistorySeries* {
        for (HistorySeries& item : series) {
            if (item.name == name) {
                return &item;
            }
        }
        return nullptr;
    };
    for (const Round& round : report.tests) {
        for (const TempSensor& sensor : round.temperature) {
            HistorySeries* dest = find_series(sensor.name);
            if (dest == nullptr) {
                series.push_back(HistorySeries{});
                dest = &series.back();
                dest->name = sensor.name;
                dest->color = palette[(series.size() - 1) % 4];
            }
            if (sensor.have_critical) {
                dest->have_critical = true;
                dest->critical = static_cast<double>(sensor.critical_mc) / 1000.0;
            }
            if (sensor.have_before) {
                dest->samples.push_back(static_cast<double>(sensor.before_mc) / 1000.0);
                dest->rounds.push_back(round.round);
            }
            if (sensor.have_after) {
                dest->samples.push_back(static_cast<double>(sensor.after_mc) / 1000.0);
                dest->rounds.push_back(round.round);
            }
        }
    }
    series.erase(std::remove_if(series.begin(),
                                series.end(),
                                [](const HistorySeries& item) { return item.samples.size() < 2; }),
                 series.end());
    draw_history(style, "temperature", "start to finish", series, 12, 8);
}

std::string align_right(std::string text, int width) {
    const int n = utf8_cols(text);
    if (n < width) {
        text.insert(0, static_cast<size_t>(width - n), ' ');
    }
    return text;
}

// O for pass, X for fail. Columns stay intact when the table wraps.
void print_result_table(const Style& style, const Report& report) {
    if (report.tests.empty()) {
        return;
    }
    int max_round = 1;
    for (const Round& round : report.tests) {
        max_round = std::max(max_round, std::max(1, round.round));
    }
    int digits = 1;
    for (int value = max_round; value >= 10; value /= 10) {
        ++digits;
    }
    const int cell = std::max(2, digits);
    const int gap = 1;
    const int label_w = 6;
    const int avail = std::max(cell, style.width - label_w - 1);
    const int per_row = std::max(1, avail / (cell + gap));

    auto mark_of = [](bool pass) { return pass ? "O" : "X"; };
    auto host_pass = [](const Round& round) {
        if (round.host.present) {
            return true;
        }
        return round.result == "pass";
    };
    auto vram_pass = [](const Round& round) {
        if (round.vram.present) {
            return true;
        }
        return round.result == "pass";
    };

    std::cout << style.paint("  results", kBold) << "\n";
    for (size_t begin = 0; begin < report.tests.size(); begin += static_cast<size_t>(per_row)) {
        const size_t end = std::min(report.tests.size(), begin + static_cast<size_t>(per_row));
        if (begin > 0) {
            std::cout << "\n";
        }
        auto print_row = [&](const std::string& label, const auto& value_of) {
            std::cout << style.paint(pad_right(label, label_w), kDim);
            for (size_t i = begin; i < end; ++i) {
                std::cout << " ";
                const std::string text = value_of(report.tests[i]);
                const bool pass = text == "O";
                const bool fail = text == "X";
                const char* color = pass ? kGreen : (fail ? kRed : kDim);
                std::cout << style.paint(align_right(text, cell), color);
            }
            std::cout << "\n";
        };
        print_row("", [](const Round& round) { return std::to_string(round.round); });
        print_row("host", [&](const Round& round) { return std::string(mark_of(host_pass(round))); });
        print_row("vram", [&](const Round& round) { return std::string(mark_of(vram_pass(round))); });
        print_row("round", [](const Round& round) { return std::string(round.result == "pass" ? "O" : "X"); });
    }
    std::cout << "\n";
}

void print_bandwidth_history(const Style& style, const Report& report) {
    HistorySeries h2d{"host → vram", {}, {}, kCyan};
    HistorySeries d2h{"vram → host", {}, {}, kGreen};
    HistorySeries check{"readback", {}, {}, kYellow};
    HistorySeries d2d{"vram → vram", {}, {}, kMagenta};
    for (const Round& round : report.tests) {
        if (round.host.present) {
            h2d.samples.push_back(round.host.h2d);
            h2d.rounds.push_back(round.round);
            d2h.samples.push_back(round.host.d2h);
            d2h.rounds.push_back(round.round);
        }
        if (round.vram.present) {
            d2d.samples.push_back(round.vram.d2d);
            d2d.rounds.push_back(round.round);
            check.samples.push_back(round.vram.check);
            check.rounds.push_back(round.round);
        }
    }
    std::vector<HistorySeries> host;
    if (h2d.samples.size() >= 2) {
        host.push_back(std::move(h2d));
    }
    if (d2h.samples.size() >= 2) {
        host.push_back(std::move(d2h));
    }
    if (check.samples.size() >= 2) {
        host.push_back(std::move(check));
    }
    std::vector<HistorySeries> device;
    if (d2d.samples.size() >= 2) {
        device.push_back(std::move(d2d));
    }
    if (!host.empty()) {
        draw_history(style, "host bandwidth", "GiB/s", host, 8, 0.6);
    }
    if (!device.empty()) {
        draw_history(style, "VRAM bandwidth", "GiB/s", device, 8, 8);
    }
}

void render(const Style& style, const Report& report) {
    const std::string word = status_word(report);
    const char* sc = status_color(report);
    const std::string name = report.path.empty() || report.path == "-" ? "stdin" : report.path;
    const std::string top_left = "┌─ " + fit(name, std::max(8, style.width - utf8_cols(word) - 12)) + " ";
    const std::string top_right_plain = " " + word + " ─┐";
    const int dashes = style.width - utf8_cols(top_left) - utf8_cols(top_right_plain);
    const std::string status_esc = std::string(kBold) + sc;
    std::cout << style.paint(top_left, kDim) << style.paint(repeat_cols("─", std::max(1, dashes)), kDim) << " "
              << style.paint(word, status_esc.c_str()) << style.paint(" ─┐", kDim) << "\n";

    const int inner = style.width - 4;
    auto line = [&](const std::string& plain, const std::string& shown) { box_line(style, fit(plain, inner), shown); };

    if (!report.device.empty()) {
        const std::string shown = style.paint(fit(report.device, inner), kBold);
        line(report.device, shown);
    } else if (!report.program.empty()) {
        line(report.program, style.paint(report.program, kBold));
    }

    const std::string id = join_meta({
        report.arch,
        report.pci,
        report.hip.empty() ? "" : "HIP " + report.hip,
    });
    if (!id.empty()) {
        line(id, fit(id, inner));
    }
    const std::string when = join_meta({report.time, report.host, report.kernel});
    if (!when.empty()) {
        line(when, style.paint(fit(when, inner), kDim));
    }

    rule(style, "├", "┤");

    uint64_t peak_alloc = 0;
    for (const Round& round : report.tests) {
        if (round.have_alloc && round.allocated > peak_alloc) {
            peak_alloc = round.allocated;
        }
    }
    if (report.have_vram_total && report.vram_total > 0) {
        const std::string text = "VRAM allocation   " + format_bytes(peak_alloc) + " of " + format_bytes(report.vram_total);
        line(text, text);
        const int bar = std::max(10, inner - 2);
        const std::string m = meter(style, static_cast<double>(peak_alloc), static_cast<double>(report.vram_total), bar, kGreen);
        const int pad = std::max(0, inner - (bar + 2));
        box_line(style, std::string(static_cast<size_t>(bar + 2 + pad), 'x'), m + std::string(static_cast<size_t>(pad), ' '));
    } else if (peak_alloc > 0) {
        const std::string text = "VRAM allocation   " + format_bytes(peak_alloc);
        line(text, text);
    }
    if (report.have_host_memory) {
        std::string text = "host memory       " + format_bytes(report.host_memory);
        if (report.have_host_available && report.host_memory >= report.host_available) {
            text += "   " + format_bytes(report.host_memory - report.host_available) + " in use";
        }
        if (report.have_host_available) {
            text += "   " + format_bytes(report.host_available) + " available";
        }
        line(text, fit(text, inner));
        if (report.have_host_available && report.host_memory > 0) {
            const int bar = std::max(10, inner - 2);
            const double used = static_cast<double>(report.host_memory - report.host_available);
            const std::string m = meter(style, used, static_cast<double>(report.host_memory), bar, kCyan);
            const int pad = std::max(0, inner - (bar + 2));
            box_line(style, std::string(static_cast<size_t>(bar + 2 + pad), 'x'), m + std::string(static_cast<size_t>(pad), ' '));
        }
    }

    std::vector<std::string> foot;
    if (report.have_chunk) {
        foot.push_back("chunk " + format_bytes(report.chunk));
    }
    if (report.have_rounds) {
        foot.push_back(std::to_string(report.tests.size()) + " of " + std::to_string(report.rounds) + " rounds");
    } else if (!report.tests.empty()) {
        foot.push_back(std::to_string(report.tests.size()) + " rounds");
    }
    foot.push_back(std::to_string(report.passed) + " passed");
    foot.push_back(std::to_string(report.failed) + " failed");
    if (report.have_exit) {
        foot.push_back("exit " + std::to_string(report.exit_code));
    }
    const std::string foot_text = join_meta(foot);
    if (!foot_text.empty()) {
        line(foot_text, style.paint(fit(foot_text, inner), kDim));
    }
    if (!report.reason.empty()) {
        const std::string reason = "reason   " + report.reason;
        line(reason, style.paint(fit(reason, inner), kRed));
    }
    rule(style, "└", "┘");
    std::cout << "\n";

    const double host_steps[] = {2, 4, 8, 16, 32, 64};
    const double vram_steps[] = {64, 128, 256, 512, 1024, 2048};
    std::vector<Sample> h2d, d2h, d2d, check;
    Stats h2d_s, d2h_s, d2d_s, check_s;
    collect(report.tests, host_present, h2d_of, &h2d, &h2d_s);
    collect(report.tests, host_present, d2h_of, &d2h, &d2h_s);
    collect(report.tests, vram_present, d2d_of, &d2d, &d2d_s);
    collect(report.tests, vram_present, check_of, &check, &check_s);

    const double host_peak = std::max(h2d_s.max, std::max(d2h_s.max, check_s.max));
    const double host_scale = nice_scale(host_peak, host_steps, 6);
    const double vram_scale = nice_scale(d2d_s.max, vram_steps, 6);

    if (h2d_s.n > 0 || d2h_s.n > 0) {
        print_chart(style, "host transfers", host_scale, h2d, "host → vram", kCyan, &d2h, "vram → host", kGreen);
        std::cout << "\n";
    }
    if (d2d_s.n > 0) {
        print_chart(style, "VRAM copy", vram_scale, d2d, "vram → vram", kMagenta, nullptr, "", "");
        std::cout << "\n";
    }
    if (h2d_s.n == 0 && d2d_s.n == 0) {
        std::cout << style.paint("  no bandwidth samples in this log\n", kDim);
    }

    for (const Round& round : report.tests) {
        const bool pass = round.result == "pass";
        const char* rc = pass ? kGreen : kRed;
        std::string head = "round " + std::to_string(round.round);
        head += pass ? "  PASS" : "  FAIL";
        if (!round.seed.empty()) {
            head += "    seed " + round.seed;
        }
        std::cout << style.paint(head, rc) << "\n";
        if (round.have_alloc) {
            std::string alloc = "  allocated " + format_bytes(round.allocated) + "   free after " + format_bytes(round.free_after);
            std::cout << style.paint(alloc, kDim) << "\n";
        }
        if (round.host.present) {
            print_rate_row(style, "host → vram ", round.host.h2d, round.host.h2d_s, host_scale, kCyan);
            print_rate_row(style, "vram → host ", round.host.d2h, round.host.d2h_s, host_scale, kGreen);
        }
        if (round.vram.present) {
            print_rate_row(style, "vram → vram ", round.vram.d2d, round.vram.d2d_s, vram_scale, kMagenta);
            print_rate_row(style, "readback    ", round.vram.check, round.vram.check_s, host_scale, kYellow);
            std::cout << style.paint("  clear " + format_seconds(round.vram.clear_s), kDim) << "\n";
        }
        if (!round.error.empty()) {
            std::cout << style.paint("  error  " + round.error, kRed) << "\n";
        }
        if (round.mismatch.present) {
            std::cout << style.paint("  mismatch at offset " + std::to_string(round.mismatch.offset), kRed) << "\n";
            if (round.mismatch.have_source) {
                std::cout << style.paint("  source offset " + std::to_string(round.mismatch.source), kRed) << "\n";
            }
            if (!round.mismatch.expected.empty() || !round.mismatch.actual.empty()) {
                std::cout << style.paint("  expected " + round.mismatch.expected + "   actual " + round.mismatch.actual, kRed) << "\n";
            }
        }
        print_temperatures(style, round.temperature);
        std::cout << "\n";
    }

    if (h2d_s.n + d2h_s.n + d2d_s.n + check_s.n > 0) {
        rule(style, "┌", "┐");
        const std::string heading = "  " + pad_right("bandwidth", 14) + "  " + pad_left("min", 7) + "  " + pad_left("avg", 7) + "  "
                                    + pad_left("max", 7);
        box_line(style, heading, style.paint(heading, kDim));
        auto summary = [&](const char* name, const Stats& stats, const char* color) {
            if (stats.n == 0) {
                return;
            }
            const std::string min_s = format_rate(stats.min);
            const std::string avg_s = format_rate(stats.avg());
            const std::string max_s = format_rate(stats.max);
            const std::string plain = "  " + pad_right(name, 14) + "  " + min_s + "  " + avg_s + "  " + max_s + "   GiB/s";
            const std::string shown = "  " + pad_right(name, 14) + "  " + style.paint(min_s, kDim) + "  " + style.paint(avg_s, color) + "  "
                                      + style.paint(max_s, color) + style.paint("   GiB/s", kDim);
            box_line(style, plain, shown);
        };
        summary("host → vram", h2d_s, kCyan);
        summary("vram → host", d2h_s, kGreen);
        summary("vram → vram", d2d_s, kMagenta);
        summary("readback", check_s, kYellow);
        if (d2d_s.n > 0) {
            const std::string peak = "  peak VRAM copy   " + format_rate(d2d_s.max) + " GiB/s   round " + std::to_string(d2d_s.peak_round);
            box_line(style, peak, style.paint(peak, kMagenta));
        }
        rule(style, "└", "┘");
    }
    std::cout << "\n";
    print_bandwidth_history(style, report);
    print_temperature_history(style, report);
    print_result_table(style, report);
}

std::string read_stream(std::istream& in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string read_file(const std::string& path) {
    if (path == "-") {
        return read_stream(std::cin);
    }
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("could not read " + path);
    }
    return read_stream(in);
}

std::string newest_log() {
    DIR* dir = opendir(".");
    if (dir == nullptr) {
        throw std::runtime_error("could not open the current directory");
    }
    std::string best_name;
    timespec best_time{};
    bool found = false;
    while (dirent* ent = readdir(dir)) {
        const std::string name = ent->d_name;
        const std::string prefix = "vram-test-";
        const std::string suffix = ".json";
        if (name.size() <= prefix.size() + suffix.size()) {
            continue;
        }
        if (name.compare(0, prefix.size(), prefix) != 0 || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }
        struct stat st{};
        if (stat(name.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (!found || st.st_mtim.tv_sec > best_time.tv_sec || (st.st_mtim.tv_sec == best_time.tv_sec && st.st_mtim.tv_nsec > best_time.tv_nsec)) {
            found = true;
            best_name = name;
            best_time = st.st_mtim;
        }
    }
    closedir(dir);
    if (!found) {
        throw std::runtime_error("no vram-test-*.json in the current directory");
    }
    return best_name;
}

void usage() {
    std::cout
        << "Usage: vram-view [options] [file...]\n"
        << "\n"
        << "Show a vram-test JSON log as a terminal report.\n"
        << "With no file, the newest vram-test-*.json in the current directory is used.\n"
        << "A file name of - reads stdin.\n"
        << "\n"
        << "Options:\n"
        << "  --plain       Do not use color\n"
        << "  --color       Use color even when stdout is not a terminal\n"
        << "  --width N     Set the report width in columns (minimum 64)\n"
        << "  -h, --help    Show this help\n";
}

int terminal_width() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 64) {
        return std::min(100, static_cast<int>(ws.ws_col));
    }
    return 80;
}

}  // namespace

int main(int argc, char** argv) {
    bool plain = false;
    bool force_color = false;
    int width = 0;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        }
        if (arg == "--plain") {
            plain = true;
            continue;
        }
        if (arg == "--color") {
            force_color = true;
            continue;
        }
        if (arg == "--width") {
            if (i + 1 >= argc) {
                std::cerr << "FAIL: --width needs a number\n";
                return 2;
            }
            try {
                width = std::stoi(argv[++i]);
            } catch (const std::exception&) {
                std::cerr << "FAIL: --width needs a number\n";
                return 2;
            }
            if (width < 64) {
                std::cerr << "FAIL: --width must be at least 64\n";
                return 2;
            }
            continue;
        }
        if (arg.rfind("--width=", 0) == 0) {
            try {
                width = std::stoi(arg.substr(8));
            } catch (const std::exception&) {
                std::cerr << "FAIL: --width needs a number\n";
                return 2;
            }
            if (width < 64) {
                std::cerr << "FAIL: --width must be at least 64\n";
                return 2;
            }
            continue;
        }
        if (!arg.empty() && arg[0] == '-' && arg != "-") {
            std::cerr << "FAIL: unknown option " << arg << "\n";
            return 2;
        }
        files.push_back(arg);
    }

    try {
        if (files.empty()) {
            files.push_back(newest_log());
        }
        Style style;
        style.color = force_color || (!plain && isatty(STDOUT_FILENO) && std::getenv("NO_COLOR") == nullptr);
        if (plain) {
            style.color = false;
        }
        style.width = width > 0 ? std::min(width, 120) : terminal_width();
        for (size_t i = 0; i < files.size(); ++i) {
            const std::string text = read_file(files[i]);
            Parser parser(text);
            const Report report = read_report(parser.parse(), files[i]);
            if (i > 0) {
                std::cout << "\n";
            }
            render(style, report);
        }
    } catch (const std::exception& ex) {
        std::cerr << "FAIL: " << ex.what() << "\n";
        return 2;
    }
    return 0;
}
