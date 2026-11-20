#include "app/pin.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

extern "C" {
#include "monocypher.h"
}

namespace zk {

namespace {

constexpr size_t kSaltBytes = 16;
constexpr size_t kHashBytes = 32;
constexpr int kMaxMemoryKiB = 64 * 1024;  // a stored value above this is not ours
constexpr int kMaxIterations = 100;
constexpr int kFreeMisses = 5;
constexpr int64_t kFirstWaitMs = 30'000;
constexpr int64_t kMaxWaitMs = 30 * 60 * 1000;

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64encode(const uint8_t* d, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = uint32_t(d[i]) << 16 | (i + 1 < n ? uint32_t(d[i + 1]) << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        out += kB64[v >> 18 & 63];
        out += kB64[v >> 12 & 63];
        out += i + 1 < n ? kB64[v >> 6 & 63] : '=';
        out += i + 2 < n ? kB64[v & 63] : '=';
    }
    return out;
}

bool b64decode(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    if (s.empty() || s.size() % 4 != 0) return false;
    for (size_t i = 0; i < s.size(); i += 4) {
        uint32_t v = 0;
        int pad = 0;
        for (size_t j = 0; j < 4; j++) {
            char c = s[i + j];
            if (c == '=' && i + 4 == s.size() && j >= 2) { pad++; v <<= 6; continue; }
            if (pad) return false;
            const char* p = c ? std::strchr(kB64, c) : nullptr;
            if (!p) return false;
            v = v << 6 | uint32_t(p - kB64);
        }
        out.push_back(uint8_t(v >> 16));
        if (pad < 2) out.push_back(uint8_t(v >> 8));
        if (pad < 1) out.push_back(uint8_t(v));
    }
    return true;
}

bool randomBytes(uint8_t* buf, size_t n) {
    std::FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f) return false;
    size_t got = std::fread(buf, 1, n, f);
    std::fclose(f);
    return got == n;
}

// Argon2i of `pin` under the salt; the work area is allocated here and wiped after.
bool argon(const std::string& pin, const uint8_t* salt, size_t saltSize, int iterations, int memoryKiB, uint8_t out[kHashBytes]) {
    std::vector<uint8_t> work(size_t(memoryKiB) * 1024);
    crypto_argon2_config cfg{CRYPTO_ARGON2_I, uint32_t(memoryKiB), uint32_t(iterations), 1};
    crypto_argon2_inputs in{reinterpret_cast<const uint8_t*>(pin.data()), salt, uint32_t(pin.size()), uint32_t(saltSize)};
    crypto_argon2(out, kHashBytes, work.data(), cfg, in, crypto_argon2_no_extras);
    crypto_wipe(work.data(), work.size());
    return true;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

// Constant time in the length of the longer input, not in where they differ.
bool constantTimeEqual(const std::string& a, const std::string& b) {
    size_t n = std::max(a.size(), b.size());
    unsigned diff = unsigned(a.size() != b.size());
    for (size_t i = 0; i < n; i++) {
        unsigned char x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        unsigned char y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= unsigned(x ^ y);
    }
    return diff == 0;
}

}  // namespace

bool validPinFormat(const std::string& pin) {
    if (pin.size() < 4 || pin.size() > 6) return false;
    return std::all_of(pin.begin(), pin.end(), [](char c) { return c >= '0' && c <= '9'; });
}

PinHash hashPin(const std::string& pin) {
    PinHash h;
    uint8_t salt[kSaltBytes], out[kHashBytes];
    if (!randomBytes(salt, sizeof salt)) return {};  // empty: checkPin never matches it
    argon(pin, salt, sizeof salt, h.iterations, h.memoryKiB, out);
    h.salt = b64encode(salt, sizeof salt);
    h.hash = b64encode(out, sizeof out);
    crypto_wipe(out, sizeof out);
    return h;
}

bool checkPin(const PinHash& stored, const std::string& pin) {
    std::vector<uint8_t> salt, want;
    if (!b64decode(stored.salt, salt) || salt.size() < 8) return false;
    if (!b64decode(stored.hash, want) || want.size() != kHashBytes) return false;
    if (stored.iterations < 1 || stored.iterations > kMaxIterations) return false;
    if (stored.memoryKiB < 8 || stored.memoryKiB > kMaxMemoryKiB) return false;
    uint8_t got[kHashBytes];
    argon(pin, salt.data(), salt.size(), stored.iterations, stored.memoryKiB, got);
    bool ok = crypto_verify32(got, want.data()) == 0;
    crypto_wipe(got, sizeof got);
    return ok;
}

int64_t pinWaitMs(const PinBackoff& b, int64_t nowMs) { return std::max<int64_t>(0, b.waitUntilMs - nowMs); }

void recordMiss(PinBackoff& b, int64_t nowMs) {
    if (b.misses < 1000) b.misses++;
    if (b.misses <= kFreeMisses) return;
    int shift = std::min(b.misses - kFreeMisses - 1, 20);
    int64_t wait = std::min(kFirstWaitMs << shift, kMaxWaitMs);
    b.waitUntilMs = nowMs + wait;
}

void recordSuccess(PinBackoff& b) { b = PinBackoff{}; }

bool keyMatches(const Config& stored, const std::string& typedKey) {
    if (stored.apiKey.empty()) return false;
    return constantTimeEqual(stored.apiKey, trim(typedKey));
}

std::optional<std::string> takeResetKey(const std::string& dropFile) {
    std::error_code ec;
    std::string content;
    {
        std::ifstream in(dropFile);
        if (!in) return std::nullopt;
        std::ostringstream ss;
        ss << in.rdbuf();
        content = ss.str();
    }
    std::filesystem::remove(dropFile, ec);
    std::string key = trim(content);
    if (key.empty()) return std::nullopt;
    return key;
}

}  // namespace zk
