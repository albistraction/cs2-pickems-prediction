// Utilidades gerais: strings, datas, HTML e arquivos.
#pragma once
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace util {

inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---------------------------------------------------------------- datas
inline int daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy =
        (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2u) / 5u + static_cast<unsigned>(d) - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

// "YYYY-MM-DD" -> dias desde 1970-01-01 (INT_MIN se inválida)
inline int dayNumber(const std::string& s) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return INT_MIN;
    if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1990 || y > 2100) return INT_MIN;
    return daysFromCivil(y, m, d);
}

inline std::string dateString(int z) {
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = static_cast<int>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", y, m, d);
    return buf;
}

inline int todayDay() { return static_cast<int>(std::time(nullptr) / 86400); }

inline std::string nowString() {
    static std::mutex mu;
    std::lock_guard<std::mutex> g(mu);
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::gmtime(&t));
    return buf;
}

// unix em milissegundos (string) -> dia
inline int dayFromUnixMs(const std::string& v) {
    long long ms = std::atoll(v.c_str());
    if (ms <= 0) return 0;
    if (ms < 100000000000LL) ms *= 1000;  // veio em segundos
    return static_cast<int>(ms / 1000 / 86400);
}

// ---------------------------------------------------------------- HTML
inline void appendUtf8(std::string& out, long cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

inline std::string htmlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i);
            if (semi != std::string::npos && semi - i <= 10) {
                std::string ent = s.substr(i + 1, semi - i - 1);
                std::string rep;
                bool ok = true;
                if (ent == "amp") rep = "&";
                else if (ent == "lt") rep = "<";
                else if (ent == "gt") rep = ">";
                else if (ent == "quot") rep = "\"";
                else if (ent == "apos") rep = "'";
                else if (ent == "nbsp") rep = " ";
                else if (ent.size() > 1 && ent[0] == '#') {
                    long cp = (ent[1] == 'x' || ent[1] == 'X') ? std::strtol(ent.c_str() + 2, nullptr, 16)
                                                               : std::strtol(ent.c_str() + 1, nullptr, 10);
                    if (cp > 0) appendUtf8(rep, cp); else ok = false;
                } else ok = false;
                if (ok) {
                    out += rep;
                    i = semi + 1;
                    continue;
                }
            }
        }
        out += s[i++];
    }
    return out;
}

// Remove tags, decodifica entidades e compacta espaços.
inline std::string stripTags(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool inTag = false;
    for (char c : s) {
        if (c == '<') { inTag = true; out += ' '; continue; }
        if (c == '>') { inTag = false; continue; }
        if (!inTag) out += c;
    }
    out = htmlDecode(out);
    std::string res;
    bool space = false;
    for (char c : out) {
        if (std::isspace(static_cast<unsigned char>(c))) { space = true; continue; }
        if (space && !res.empty()) res += ' ';
        space = false;
        res += c;
    }
    return res;
}

// Texto do elemento cuja tag contém a posição `pos`: do '>' seguinte até `close`.
inline std::string cellText(const std::string& s, size_t pos, const std::string& close) {
    if (pos == std::string::npos || pos >= s.size()) return "";
    size_t a = s.find('>', pos);
    if (a == std::string::npos) return "";
    size_t b = s.find(close, a + 1);
    if (b == std::string::npos) b = std::min(s.size(), a + 1 + 500);
    return stripTags(s.substr(a + 1, b - a - 1));
}

// Tag HTML completa que contém a posição `pos` (de '<' até '>').
inline std::string tagAt(const std::string& s, size_t pos) {
    if (pos == std::string::npos || pos >= s.size()) return "";
    size_t a = s.rfind('<', pos);
    size_t b = s.find('>', pos);
    if (a == std::string::npos || b == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}

// Valor de atributo attr="..." a partir de `from`.
inline std::string attrValue(const std::string& s, const std::string& attr, size_t from = 0) {
    std::string pat = attr + "=\"";
    size_t p = s.find(pat, from);
    if (p == std::string::npos) return "";
    p += pat.size();
    size_t q = s.find('"', p);
    if (q == std::string::npos) return "";
    return htmlDecode(s.substr(p, q - p));
}

inline std::string digitsAt(const std::string& s, size_t pos) {
    std::string out;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) out += s[pos++];
    return out;
}

// Primeiro número em uma string ("(1000 points)" -> 1000, "75.0%" -> 75).
inline double toNum(const std::string& s, double def = NAN) {
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        bool start = std::isdigit(static_cast<unsigned char>(c)) ||
                     ((c == '-' || c == '+' || c == '.') && i + 1 < s.size() &&
                      std::isdigit(static_cast<unsigned char>(s[i + 1])));
        if (start) return std::strtod(s.c_str() + i, nullptr);
    }
    return def;
}

inline bool isInt(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

inline size_t countOf(const std::string& s, const std::string& pat) {
    size_t n = 0, p = 0;
    while ((p = s.find(pat, p)) != std::string::npos) { ++n; p += pat.size(); }
    return n;
}

inline std::string fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

// ---------------------------------------------------------------- arquivos
inline bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

inline bool writeFile(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
        if (!f) return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

inline std::string env(const char* name, const std::string& def = "") {
    const char* v = std::getenv(name);
    return v && *v ? std::string(v) : def;
}
inline int envInt(const char* name, int def) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : def;
}

}  // namespace util
