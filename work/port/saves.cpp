/* MotorStorm Apocalypse port: the saves and their backups -- see saves.h. */
#include "saves.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace saves {
namespace {

/* ---- small things --------------------------------------------------------- */

std::time_t to_time_t(fs::file_time_type t)
{
    using namespace std::chrono;
    const auto sys = time_point_cast<system_clock::duration>(t - fs::file_time_type::clock::now() + system_clock::now());
    return system_clock::to_time_t(sys);
}

fs::path clean(const std::string& p)
{
    std::string t = p;
    while (t.size() > 1 && (t.back() == '/' || t.back() == '\\')) t.pop_back();
    return fs::path(t);
}

bool is_dir(const fs::path& p)
{
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    return !ec && fs::is_directory(st);
}

bool is_file(const fs::path& p)
{
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    return !ec && fs::is_regular_file(st);
}

std::tm local_tm(std::time_t t)
{
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

std::string stamp(std::time_t t)
{
    const std::tm tm = local_tm(t);
    char b[32];
    snprintf(b, sizeof b, "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}

bool read_file(const fs::path& p, uint64_t cap, std::vector<uint8_t>& out)
{
    out.clear();
    if (!is_file(p)) return false;
    std::error_code ec;
    const uint64_t n = fs::file_size(p, ec);
    if (ec || n > cap) return false;
    FILE* f = fopen(p.string().c_str(), "rb");
    if (!f) return false;
    out.resize((size_t)n);
    const size_t got = n ? fread(out.data(), 1, (size_t)n, f) : 0;
    fclose(f);
    return got == n;
}

bool write_file(const fs::path& p, const uint8_t* d, size_t n)
{
    FILE* f = fopen(p.string().c_str(), "wb");
    if (!f) return false;
    const size_t put = n ? fwrite(d, 1, n, f) : 0;
    return (fclose(f) == 0) && put == n;
}

/* One regular file under a root. */
struct Entry {
    std::string rel;        /* with '/' */
    uint64_t size = 0;
    fs::file_time_type mtime{};
};

/* Every regular file under `root`, sorted by name. Symbolic links, special
 * files and names starting with a dot are left out. False when the folder
 * could not be read through. */
bool walk(const fs::path& root, const std::string& prefix, int depth, std::vector<Entry>& out)
{
    if (depth > 8) return false;
    std::error_code ec;
    fs::directory_iterator it(root, ec), end;
    if (ec) return false;
    for (; it != end; it.increment(ec)) {
        if (ec) return false;
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code e2;
        const fs::file_status st = it->symlink_status(e2);
        if (e2) return false;
        if (fs::is_directory(st)) {
            if (!walk(it->path(), prefix + name + "/", depth + 1, out)) return false;
        } else if (fs::is_regular_file(st)) {
            Entry e;
            e.rel = prefix + name;
            e.size = fs::file_size(it->path(), e2);
            if (e2) return false;
            e.mtime = fs::last_write_time(it->path(), e2);
            out.push_back(e);
        }
    }
    if (depth == 0)
        std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.rel < b.rel; });
    return true;
}

/* The folders directly under `root` (the saves of a saves folder or of a backup). */
std::vector<std::string> subdirs(const fs::path& root)
{
    std::vector<std::string> v;
    std::error_code ec;
    fs::directory_iterator it(root, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (is_dir(it->path())) v.push_back(name);
    }
    std::sort(v.begin(), v.end());
    return v;
}

/* A copy of what walk() sees, with the files' times. `dst` must not exist. */
bool copy_tree(const fs::path& src, const fs::path& dst)
{
    std::vector<Entry> files;
    if (!walk(src, "", 0, files)) return false;
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) return false;
    /* Empty save folders too: they are still saves in the list. */
    for (const std::string& d : subdirs(src)) {
        fs::create_directories(dst / d, ec);
        if (ec) return false;
    }
    for (const Entry& e : files) {
        const fs::path to = dst / fs::path(e.rel);
        fs::create_directories(to.parent_path(), ec);
        if (ec) return false;
        if (!fs::copy_file(src / fs::path(e.rel), to, fs::copy_options::none, ec) || ec) return false;
        fs::last_write_time(to, e.mtime, ec);
        ec.clear();     /* a time that would not set is not a failed copy */
    }
    return true;
}

/* The same files with the same bytes. */
bool same_tree(const fs::path& a, const fs::path& b)
{
    std::vector<Entry> fa, fb;
    if (!walk(a, "", 0, fa) || !walk(b, "", 0, fb)) return false;
    if (fa.size() != fb.size() || subdirs(a) != subdirs(b)) return false;
    for (size_t i = 0; i < fa.size(); i++)
        if (fa[i].rel != fb[i].rel || fa[i].size != fb[i].size) return false;
    std::vector<uint8_t> da, db;
    for (const Entry& e : fa) {
        if (!read_file(a / fs::path(e.rel), UINT64_MAX, da) || !read_file(b / fs::path(e.rel), UINT64_MAX, db)) return false;
        if (da != db) return false;
    }
    return true;
}

void remove_tree(const fs::path& p)
{
    std::error_code ec;
    fs::remove_all(p, ec);
}

/* A name the lists gave out: one path component, not hidden. */
bool plain_name(const std::string& n)
{
    return !n.empty() && n[0] != '.' && n.find_first_of("/\\:") == std::string::npos;
}

Result fail(const std::string& m)
{
    Result r;
    r.message = m;
    return r;
}

/* ---- PARAM.SFO ------------------------------------------------------------
 * The reader of ps3recomp's libs/system/ps3_psf.h again (that header is the
 * savedata module's own): "\0PSF", version, then little-endian key table
 * start, data table start and entry count; 16-byte entries (key offset u16,
 * format u16, length u32, max length u32, data offset u32). Format 0x0204 is
 * NUL-terminated UTF-8. */
uint32_t le16(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8; }
uint32_t le32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

bool is_psf(const std::vector<uint8_t>& b)
{
    return b.size() >= 0x14 && b[0] == 0 && b[1] == 'P' && b[2] == 'S' && b[3] == 'F';
}

void parse_sfo(const std::vector<uint8_t>& b, SaveInfo& s)
{
    if (!is_psf(b)) return;
    const uint64_t n = b.size();
    const uint64_t ktab = le32(&b[8]), dtab = le32(&b[12]), cnt = le32(&b[16]);
    if (cnt > 256 || ktab > n || dtab > n || 0x14 + 16 * cnt > n) return;
    for (uint64_t i = 0; i < cnt; i++) {
        const uint8_t* e = &b[0x14 + 16 * i];
        const uint64_t ko = le16(e), fmt = le16(e + 2), len = le32(e + 4), dof = le32(e + 12);
        if (ktab + ko >= n || dtab + dof + len > n || fmt != 0x0204) continue;
        const uint8_t* k = &b[ktab + ko];
        const void* kend = memchr(k, 0, (size_t)(n - (ktab + ko)));
        if (!kend) continue;
        const std::string key((const char*)k, (const char*)kend);
        std::string* dst = key == "TITLE" ? &s.title : key == "SUB_TITLE" ? &s.subtitle : key == "DETAIL" ? &s.detail : nullptr;
        if (!dst) continue;
        const uint8_t* d = &b[dtab + dof];
        size_t l = 0;
        while (l < len && l < 1024 && d[l]) l++;
        dst->assign((const char*)d, l);
        /* The PS3 breaks lines in these; one line here. */
        for (char& ch : *dst) if ((unsigned char)ch < 0x20) ch = ' ';
    }
}

/* ---- backup names --------------------------------------------------------- */

bool digits(const std::string& s, size_t at, size_t n)
{
    if (s.size() < at + n) return false;
    for (size_t i = at; i < at + n; i++) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

/* <YYYYmmdd-HHMMSS>[-<reason>][-<n>], with or without "saves-" before it. */
bool parse_name(const std::string& name, std::time_t& t, std::string& reason)
{
    std::string s = name;
    if (s.compare(0, 6, "saves-") == 0) s = s.substr(6);
    if (!digits(s, 0, 8) || s.size() < 15 || s[8] != '-' || !digits(s, 9, 6)) return false;
    if (s.size() > 15 && s[15] != '-') return false;
    std::tm tm{};
    tm.tm_year = atoi(s.substr(0, 4).c_str()) - 1900;
    tm.tm_mon = atoi(s.substr(4, 2).c_str()) - 1;
    tm.tm_mday = atoi(s.substr(6, 2).c_str());
    tm.tm_hour = atoi(s.substr(9, 2).c_str());
    tm.tm_min = atoi(s.substr(11, 2).c_str());
    tm.tm_sec = atoi(s.substr(13, 2).c_str());
    tm.tm_isdst = -1;
    t = mktime(&tm);
    if (t == (std::time_t)-1) return false;
    reason = "manual";
    if (s.size() > 16) {
        size_t e = 16;
        while (e < s.size() && s[e] >= 'a' && s[e] <= 'z') e++;
        const std::string r = s.substr(16, e - 16);
        if (r == "play" || r == "restore" || r == "import" || r == "manual") reason = r;
    }
    return true;
}

bool automatic(const std::string& reason) { return reason == "play" || reason == "restore"; }

/* A free name in `dir`: base, base-2, base-3... (two backups in one second). */
std::string free_name(const fs::path& dir, const std::string& base, const std::string& ext)
{
    std::error_code ec;
    for (int i = 1; i < 1000; i++) {
        const std::string n = i == 1 ? base + ext : base + "-" + std::to_string(i) + ext;
        if (!fs::exists(fs::symlink_status(dir / n, ec))) return n;
    }
    return base + "-x" + ext;
}

/* What an interrupted backup or import left behind. */
void sweep_tmp(const fs::path& dir)
{
    std::error_code ec;
    std::vector<fs::path> gone;
    fs::directory_iterator it(dir, ec), end;
    for (; !ec && it != end; it.increment(ec))
        if (it->path().filename().string().compare(0, 5, ".tmp-") == 0) gone.push_back(it->path());
    for (const fs::path& p : gone) remove_tree(p);
}

/* ---- zip ------------------------------------------------------------------ */

uint32_t crc32(const uint8_t* d, size_t n)
{
    static uint32_t table[256];
    static bool made = false;
    if (!made) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        made = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put16(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, x & 0xFFFF); put16(v, x >> 16); }

/* Inflate (RFC 1951) into exactly out.size() bytes. Every read and write is
 * bounds-checked; anything unexpected is a failure, never a guess. After
 * Mark Adler's puff.c in method (canonical codes decoded a bit at a time),
 * written for this file. */
struct Inflate {
    const uint8_t* in; size_t in_n, in_at = 0;
    uint8_t* out; size_t out_n, out_at = 0;
    uint32_t bitbuf = 0; int bitcnt = 0;
    bool bad = false;

    uint32_t bits(int need)
    {
        uint32_t v = bitbuf;
        while (bitcnt < need) {
            if (in_at >= in_n) { bad = true; return 0; }
            v |= (uint32_t)in[in_at++] << bitcnt;
            bitcnt += 8;
        }
        bitbuf = need < 32 ? v >> need : 0;
        bitcnt -= need;
        return need ? v & ((1u << need) - 1) : 0;
    }
};

struct Huffman {
    uint16_t count[16];
    uint16_t symbol[288];
};

/* <0: over-subscribed; 0: complete; >0: incomplete. */
int huff_build(Huffman& h, const uint16_t* length, int n)
{
    for (int l = 0; l <= 15; l++) h.count[l] = 0;
    for (int s = 0; s < n; s++) h.count[length[s]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l <= 15; l++) {
        left <<= 1;
        left -= h.count[l];
        if (left < 0) return left;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = (uint16_t)(offs[l] + h.count[l]);
    for (int s = 0; s < n; s++)
        if (length[s]) h.symbol[offs[length[s]]++] = (uint16_t)s;
    return left;
}

int huff_decode(Inflate& s, const Huffman& h)
{
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= 15; l++) {
        code |= (int)s.bits(1);
        if (s.bad) return -1;
        const int count = h.count[l];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

bool inflate_codes(Inflate& s, const Huffman& lencode, const Huffman& distcode)
{
    static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const uint16_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const uint16_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
                                       9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    for (;;) {
        int sym = huff_decode(s, lencode);
        if (sym < 0 || s.bad) return false;
        if (sym < 256) {
            if (s.out_at >= s.out_n) return false;
            s.out[s.out_at++] = (uint8_t)sym;
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            const size_t len = lbase[sym] + s.bits(lext[sym]);
            const int ds = huff_decode(s, distcode);
            if (ds < 0 || ds >= 30 || s.bad) return false;
            const size_t dist = dbase[ds] + s.bits(dext[ds]);
            if (s.bad || dist > s.out_at || len > s.out_n - s.out_at) return false;
            for (size_t i = 0; i < len; i++, s.out_at++) s.out[s.out_at] = s.out[s.out_at - dist];
        }
    }
}

bool inflate(const uint8_t* in, size_t in_n, uint8_t* out, size_t out_n)
{
    Inflate s{};
    s.in = in; s.in_n = in_n; s.out = out; s.out_n = out_n;
    for (;;) {
        const uint32_t last = s.bits(1), type = s.bits(2);
        if (s.bad) return false;
        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.in_n - s.in_at < 4) return false;
            const uint32_t len = le16(s.in + s.in_at), nlen = le16(s.in + s.in_at + 2);
            s.in_at += 4;
            if ((len ^ 0xFFFF) != nlen || len > s.in_n - s.in_at || len > s.out_n - s.out_at) return false;
            if (len) memcpy(s.out + s.out_at, s.in + s.in_at, len);
            s.in_at += len; s.out_at += len;
        } else if (type == 1) {
            Huffman lc, dc;
            uint16_t l[288];
            int i = 0;
            for (; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            huff_build(lc, l, 288);
            for (i = 0; i < 30; i++) l[i] = 5;
            huff_build(dc, l, 30);
            if (!inflate_codes(s, lc, dc)) return false;
        } else if (type == 2) {
            static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            const int nlen = (int)s.bits(5) + 257, ndist = (int)s.bits(5) + 1, ncode = (int)s.bits(4) + 4;
            if (s.bad || nlen > 286 || ndist > 30) return false;
            uint16_t l[320];
            for (int i = 0; i < 19; i++) l[i] = 0;
            for (int i = 0; i < ncode; i++) l[order[i]] = (uint16_t)s.bits(3);
            if (s.bad) return false;
            Huffman cl;
            if (huff_build(cl, l, 19) != 0) return false;
            int i = 0;
            while (i < nlen + ndist) {
                const int sym = huff_decode(s, cl);
                if (sym < 0 || s.bad) return false;
                if (sym < 16) { l[i++] = (uint16_t)sym; continue; }
                uint16_t v = 0;
                int rep;
                if (sym == 16) {
                    if (i == 0) return false;
                    v = l[i - 1];
                    rep = 3 + (int)s.bits(2);
                } else if (sym == 17) {
                    rep = 3 + (int)s.bits(3);
                } else {
                    rep = 11 + (int)s.bits(7);
                }
                if (s.bad || i + rep > nlen + ndist) return false;
                while (rep--) l[i++] = v;
            }
            if (l[256] == 0) return false;
            Huffman lc, dc;
            int e = huff_build(lc, l, nlen);
            if (e < 0 || (e > 0 && nlen - lc.count[0] != 1)) return false;
            e = huff_build(dc, l + nlen, ndist);
            if (e < 0 || (e > 0 && ndist - dc.count[0] != 1)) return false;
            if (!inflate_codes(s, lc, dc)) return false;
        } else {
            return false;
        }
        if (last) break;
    }
    return s.out_at == s.out_n;
}

/* One path component of an archive: what a save folder or a save's file may
 * be called, on Linux and on Windows. */
bool safe_component(const std::string& c)
{
    if (c.empty() || c.size() > 64 || c[0] == '.' || c.back() == '.' || c.back() == ' ') return false;
    for (char ch : c) {
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                        ch == '.' || ch == '_' || ch == '-' || ch == ' ';
        if (!ok) return false;
    }
    std::string stem = c.substr(0, c.find('.'));
    for (char& ch : stem) if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    static const char* const dev[] = { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6",
                                       "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6",
                                       "LPT7", "LPT8", "LPT9" };
    for (const char* d : dev) if (stem == d) return false;
    return true;
}

std::string lower(std::string s)
{
    for (char& ch : s) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
    return s;
}

/* An entry of the central directory, checked. */
struct ZipEntry {
    std::vector<std::string> parts;     /* the name's components */
    uint32_t method = 0, crc = 0, csize = 0, usize = 0;
    uint64_t data = 0;                  /* offset of its bytes in the archive */
};

/* Reads the central directory of `z` into `out` (files only), checking
 * everything that can be checked without unpacking. The error is a line for
 * the player. */
bool zip_index(const std::vector<uint8_t>& z, std::vector<ZipEntry>& out, std::string& err)
{
    const uint64_t n = z.size();
    if (n < 22) { err = "It is not a .zip file."; return false; }
    /* The end record: the last one whose comment ends exactly at the file's end. */
    uint64_t eocd = UINT64_MAX;
    for (uint64_t back = 22; back <= n && back <= 22 + 65535; back++) {
        const uint64_t at = n - back;
        if (le32(&z[at]) == 0x06054b50 && le16(&z[at + 20]) == back - 22) { eocd = at; break; }
    }
    if (eocd == UINT64_MAX) { err = "It is not a .zip file."; return false; }
    const uint32_t disk = le16(&z[eocd + 4]), cd_disk = le16(&z[eocd + 6]);
    const uint32_t here = le16(&z[eocd + 8]), total = le16(&z[eocd + 10]);
    const uint64_t cd_size = le32(&z[eocd + 12]), cd_at = le32(&z[eocd + 16]);
    if (disk || cd_disk || here != total) { err = "A .zip in several parts is not supported."; return false; }
    if (total == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_at == 0xFFFFFFFFu) { err = "The .zip is too large (zip64)."; return false; }
    if (total > k_max_entries) { err = "The .zip has too many files."; return false; }
    if (cd_at > eocd || cd_size > eocd - cd_at) { err = "The .zip is damaged."; return false; }
    uint64_t at = cd_at, sum = 0;
    std::set<std::string> seen;
    for (uint32_t i = 0; i < total; i++) {
        if (eocd - at < 46 || le32(&z[at]) != 0x02014b50) { err = "The .zip is damaged."; return false; }
        const uint32_t made = le16(&z[at + 4]), flags = le16(&z[at + 8]), method = le16(&z[at + 10]);
        const uint32_t crc = le32(&z[at + 16]), csize = le32(&z[at + 20]), usize = le32(&z[at + 24]);
        const uint32_t nlen = le16(&z[at + 28]), xlen = le16(&z[at + 30]), clen = le16(&z[at + 32]);
        const uint32_t edisk = le16(&z[at + 34]), eattr = le32(&z[at + 38]);
        const uint64_t lho = le32(&z[at + 42]);
        if ((uint64_t)46 + nlen + xlen + clen > eocd - at) { err = "The .zip is damaged."; return false; }
        const std::string name((const char*)&z[at + 46], nlen);
        at += 46 + nlen + xlen + clen;
        if (flags & 0x0041) { err = "The .zip is encrypted."; return false; }
        if (edisk) { err = "A .zip in several parts is not supported."; return false; }
        if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu || lho == 0xFFFFFFFFu) { err = "The .zip is too large (zip64)."; return false; }
        /* Unix file type, when a Unix made it: regular files and folders only. */
        if ((made >> 8) == 3) {
            const uint32_t type = (eattr >> 16) & 0170000;
            if (type == 0120000) { err = "The .zip holds a symbolic link (" + name + ")."; return false; }
            if (type != 0 && type != 0100000 && type != 0040000) { err = "The .zip holds a special file (" + name + ")."; return false; }
        }
        if (name.empty() || name.find('\0') != std::string::npos) { err = "The .zip has a file without a name."; return false; }
        if (name[0] == '/' || name.find('\\') != std::string::npos || name.find(':') != std::string::npos) {
            err = "The .zip has a path that is not relative (" + name + ")."; return false;
        }
        const bool folder = name.back() == '/';
        ZipEntry e;
        size_t p = 0;
        const std::string body = folder ? name.substr(0, name.size() - 1) : name;
        while (p <= body.size()) {
            size_t q = body.find('/', p);
            if (q == std::string::npos) q = body.size();
            const std::string part = body.substr(p, q - p);
            if (part == ".." ) { err = "The .zip has a path that leaves its folder (" + name + ")."; return false; }
            if (!safe_component(part)) { err = "The .zip has a name that is not allowed (" + name + ")."; return false; }
            e.parts.push_back(part);
            p = q + 1;
        }
        if (e.parts.size() > 3) { err = "The .zip is not a saves archive: folders inside a save (" + name + ")."; return false; }
        if (folder) {
            if (usize) { err = "The .zip is damaged."; return false; }
            continue;
        }
        if (method != 0 && method != 8) { err = "The .zip uses a compression that is not supported."; return false; }
        if (method == 0 && csize != usize) { err = "The .zip is damaged."; return false; }
        sum += usize;
        if (sum > k_max_total) { err = "The .zip unpacks to more than " + format_size(k_max_total) + "."; return false; }
        if (!seen.insert(lower(body)).second) { err = "The .zip has the same file twice (" + name + ")."; return false; }
        /* The local header: the same name, the bytes before the directory. */
        if (lho > cd_at || cd_at - lho < 30 || le32(&z[lho]) != 0x04034b50) { err = "The .zip is damaged."; return false; }
        const uint64_t lnlen = le16(&z[lho + 26]), lxlen = le16(&z[lho + 28]);
        e.data = lho + 30 + lnlen + lxlen;
        if (e.data > cd_at || csize > cd_at - e.data) { err = "The .zip is damaged."; return false; }
        if (lnlen != nlen || memcmp(&z[lho + 30], name.data(), nlen) != 0) { err = "The .zip is damaged (names differ)."; return false; }
        e.method = method; e.crc = crc; e.csize = csize; e.usize = usize;
        out.push_back(std::move(e));
    }
    return true;
}

} /* namespace */

/* ---- the public side ------------------------------------------------------ */

Config default_config()
{
    Config c;
    const char* s = getenv("PS3_SAVEDATA_ROOT");
    c.save_root = (s && *s) ? s : "gamedata/dev_hdd0/home/00000001/savedata";
    const char* u = getenv("PS3_USER_DIR");
    const std::string user = (u && *u) ? u : "user";
    c.backup_root = user + "/backups";
    c.export_root = user + "/exports";
    return c;
}

void ensure_folder(const std::string& path)
{
    std::error_code ec;
    fs::create_directories(clean(path), ec);
}

std::string format_time(std::time_t t)
{
    static const char* const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    const std::tm tm = local_tm(t);
    char b[48];
    snprintf(b, sizeof b, "%d %s %04d %02d:%02d", tm.tm_mday, mon[tm.tm_mon >= 0 && tm.tm_mon < 12 ? tm.tm_mon : 0],
             tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
    return b;
}

std::string format_size(uint64_t n)
{
    char b[48];
    if (n < 1024) snprintf(b, sizeof b, "%u bytes", (unsigned)n);
    else if (n < 1024 * 1024) snprintf(b, sizeof b, "%u KB", (unsigned)((n + 512) / 1024));
    else snprintf(b, sizeof b, "%.1f MB", (double)n / (1024.0 * 1024.0));
    return b;
}

const char* reason_label(const std::string& r)
{
    if (r == "play") return "before Play";
    if (r == "restore") return "before restore";
    if (r == "import") return "imported";
    return "manual";
}

std::vector<SaveInfo> list_saves(const std::string& root)
{
    std::vector<SaveInfo> v;
    const fs::path base = clean(root);
    for (const std::string& d : subdirs(base)) {
        SaveInfo s;
        s.dir = d;
        std::vector<Entry> files;
        walk(base / d, "", 0, files);
        fs::file_time_type newest{};
        for (const Entry& e : files) {
            s.size += e.size;
            if (s.files == 0 || e.mtime > newest) newest = e.mtime;
            s.files++;
        }
        std::error_code ec;
        if (!s.files) newest = fs::last_write_time(base / d, ec);
        s.mtime = to_time_t(newest);
        std::vector<uint8_t> sfo;
        if (read_file(base / d / "PARAM.SFO", 1 << 20, sfo)) parse_sfo(sfo, s);
        if (s.title.empty()) s.title = d;
        if (is_file(base / d / "ICON0.PNG")) s.icon = (base / d / "ICON0.PNG").string();
        v.push_back(std::move(s));
    }
    return v;
}

std::vector<BackupInfo> list_backups(const Config& c)
{
    struct Row { BackupInfo b; fs::file_time_type made; };
    std::vector<Row> rows;
    const fs::path base = clean(c.backup_root);
    for (const std::string& d : subdirs(base)) {
        Row r;
        r.b.name = d;
        std::error_code ec;
        r.made = fs::last_write_time(base / d, ec);
        if (!parse_name(d, r.b.time, r.b.reason)) {
            r.b.time = ec ? 0 : to_time_t(r.made);
            r.b.reason = "manual";
        }
        r.b.automatic = automatic(r.b.reason);
        r.b.saves = (int)subdirs(base / d).size();
        std::vector<Entry> files;
        walk(base / d, "", 0, files);
        for (const Entry& e : files) r.b.size += e.size;
        rows.push_back(std::move(r));
    }
    /* By the time in the name; two of one second by when they were written. */
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.b.time != b.b.time) return a.b.time > b.b.time;
        if (a.made != b.made) return a.made > b.made;
        return a.b.name > b.b.name;
    });
    std::vector<BackupInfo> v;
    for (Row& r : rows) v.push_back(std::move(r.b));
    return v;
}

Result backup(const Config& c, const char* reason_in)
{
    std::string reason = reason_in ? reason_in : "manual";
    if (reason != "play" && reason != "restore" && reason != "import") reason = "manual";
    const fs::path src = clean(c.save_root), base = clean(c.backup_root);
    Result r;
    if (subdirs(src).empty()) {
        r.ok = r.skipped = true;
        r.message = "No saves to back up yet.";
        return r;
    }
    if (reason == "play") {
        const std::vector<BackupInfo> have = list_backups(c);
        if (!have.empty() && same_tree(src, base / have[0].name)) {
            r.ok = r.skipped = true;
            r.message = "The saves have not changed since the last backup.";
            r.path = have[0].name;
            return r;
        }
    }
    std::error_code ec;
    fs::create_directories(base, ec);
    if (ec) return fail("Backing up the saves failed: could not make " + base.string() + ".");
    sweep_tmp(base);
    const std::string name = free_name(base, stamp(time(nullptr)) + "-" + reason, "");
    const fs::path tmp = base / (".tmp-" + name);
    if (!copy_tree(src, tmp)) {
        remove_tree(tmp);
        return fail("Backing up the saves failed: could not copy them to " + base.string() + ".");
    }
    fs::rename(tmp, base / name, ec);
    if (ec) {
        remove_tree(tmp);
        return fail("Backing up the saves failed: could not make " + (base / name).string() + ".");
    }
    r.ok = true;
    r.path = name;
    r.message = "Backed up the saves (" + name + ").";
    return r;
}

int prune(const Config& c, int keep)
{
    if (keep < 1) keep = 1;
    int kept = 0, gone = 0;
    for (const BackupInfo& b : list_backups(c)) {       /* newest first */
        if (!b.automatic) continue;
        if (kept < keep) { kept++; continue; }
        if (!plain_name(b.name)) continue;
        remove_tree(clean(c.backup_root) / b.name);
        gone++;
    }
    return gone;
}

Result backup_for_play(const Config& c, int keep)
{
    Result r = backup(c, "play");
    if (r.ok) prune(c, keep);
    return r;
}

Result restore(const Config& c, const std::string& name, bool game_running)
{
    if (game_running)
        return fail("Restore from the start screen: the game has the saves open.");
    const fs::path src = clean(c.backup_root) / name, dst = clean(c.save_root);
    if (!plain_name(name) || !is_dir(src)) return fail("That backup is not there any more.");
    if (subdirs(src).empty()) return fail("That backup has no saves in it.");
    const Result before = backup(c, "restore");
    if (!before.ok) return fail("The saves were not restored: they could not be backed up first.");
    const fs::path staged = fs::path(dst.string() + ".restore-new"), old = fs::path(dst.string() + ".restore-old");
    remove_tree(staged);
    remove_tree(old);
    std::error_code ec;
    if (dst.has_parent_path()) fs::create_directories(dst.parent_path(), ec);
    if (!copy_tree(src, staged)) {
        remove_tree(staged);
        return fail("The saves were not restored: the backup could not be copied. Nothing was changed.");
    }
    const bool had = fs::exists(fs::symlink_status(dst, ec));
    if (had) {
        fs::rename(dst, old, ec);
        if (ec) {
            remove_tree(staged);
            return fail("The saves were not restored: the saves folder could not be moved aside. Nothing was changed.");
        }
    }
    fs::rename(staged, dst, ec);
    if (ec) {
        std::error_code e2;
        if (had) fs::rename(old, dst, e2);
        remove_tree(staged);
        return fail("The saves were not restored: the copy could not be put in place. Nothing was changed.");
    }
    remove_tree(old);
    Result r;
    r.ok = true;
    r.path = before.skipped ? std::string() : before.path;
    std::time_t t = 0;
    std::string reason;
    r.message = "Restored the saves" + (parse_name(name, t, reason) ? " of " + format_time(t) : " from " + name) +
                (r.path.empty() ? "." : ". The saves as they were are in the list, as \"before restore\".");
    return r;
}

Result remove_backup(const Config& c, const std::string& name)
{
    const fs::path p = clean(c.backup_root) / name;
    if (!plain_name(name) || !is_dir(p)) return fail("That backup is not there any more.");
    std::error_code ec;
    fs::remove_all(p, ec);
    if (ec || fs::exists(fs::symlink_status(p, ec))) return fail("The backup could not be deleted.");
    Result r;
    r.ok = true;
    r.message = "Deleted the backup.";
    return r;
}

Result export_archive(const Config& c, const std::string& name)
{
    fs::path src;
    std::string tag;
    if (name.empty()) {
        src = clean(c.save_root);
        tag = stamp(time(nullptr));
    } else {
        src = clean(c.backup_root) / name;
        if (!plain_name(name) || !is_dir(src)) return fail("That backup is not there any more.");
        std::time_t t = 0;
        std::string reason;
        tag = parse_name(name, t, reason) ? stamp(t) + "-" + reason : stamp(time(nullptr));
    }
    if (subdirs(src).empty()) return fail(name.empty() ? "No saves to export yet." : "That backup has no saves in it.");
    std::vector<Entry> files;
    if (!walk(src, "", 0, files)) return fail("The export failed: the saves could not be read.");
    if (files.size() > 60000) return fail("The export failed: too many files.");

    std::vector<uint8_t> zip, dir, data;
    for (const Entry& e : files) {
        if (!read_file(src / fs::path(e.rel), 0xFFFFFFFEu, data) || zip.size() + data.size() > 0xFFFFFF00u)
            return fail("The export failed: " + e.rel + " could not be read.");
        const std::tm tm = local_tm(to_time_t(e.mtime));
        const uint32_t dtime = (uint32_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
        const uint32_t ddate = tm.tm_year >= 80 ? (uint32_t)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday) : 0x21;
        const uint32_t crc = crc32(data.data(), data.size()), at = (uint32_t)zip.size();
        put32(zip, 0x04034b50); put16(zip, 20); put16(zip, 0); put16(zip, 0);
        put16(zip, dtime); put16(zip, ddate); put32(zip, crc);
        put32(zip, (uint32_t)data.size()); put32(zip, (uint32_t)data.size());
        put16(zip, (uint32_t)e.rel.size()); put16(zip, 0);
        zip.insert(zip.end(), e.rel.begin(), e.rel.end());
        zip.insert(zip.end(), data.begin(), data.end());
        put32(dir, 0x02014b50); put16(dir, 3 << 8 | 20); put16(dir, 20); put16(dir, 0); put16(dir, 0);
        put16(dir, dtime); put16(dir, ddate); put32(dir, crc);
        put32(dir, (uint32_t)data.size()); put32(dir, (uint32_t)data.size());
        put16(dir, (uint32_t)e.rel.size()); put16(dir, 0); put16(dir, 0); put16(dir, 0); put16(dir, 0);
        put32(dir, 0100644u << 16); put32(dir, at);
        dir.insert(dir.end(), e.rel.begin(), e.rel.end());
    }
    const uint32_t cd_at = (uint32_t)zip.size();
    zip.insert(zip.end(), dir.begin(), dir.end());
    put32(zip, 0x06054b50); put16(zip, 0); put16(zip, 0);
    put16(zip, (uint32_t)files.size()); put16(zip, (uint32_t)files.size());
    put32(zip, (uint32_t)dir.size()); put32(zip, cd_at); put16(zip, 0);

    const fs::path out = clean(c.export_root);
    std::error_code ec;
    fs::create_directories(out, ec);
    if (ec) return fail("The export failed: could not make " + out.string() + ".");
    const std::string file = free_name(out, "MotorStormApocalypse-saves-" + tag, ".zip");
    const fs::path tmp = out / (".tmp-" + file);
    if (!write_file(tmp, zip.data(), zip.size())) {
        fs::remove(tmp, ec);
        return fail("The export failed: could not write to " + out.string() + ".");
    }
    fs::rename(tmp, out / file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return fail("The export failed: could not write to " + out.string() + ".");
    }
    Result r;
    r.ok = true;
    r.path = (out / file).string();
    r.message = "Exported to " + r.path;
    return r;
}

Result import_archive(const Config& c, const std::string& zip_path)
{
    const fs::path zp = clean(zip_path);
    if (!is_file(zp)) return fail("No file at " + zip_path + ".");
    std::error_code ec;
    const uint64_t zsize = fs::file_size(zp, ec);
    if (ec) return fail("The file could not be read.");
    if (zsize > k_max_archive) return fail("The file is larger than " + format_size(k_max_archive) + ": not a saves archive.");
    std::vector<uint8_t> z;
    if (!read_file(zp, k_max_archive, z)) return fail("The file could not be read.");
    std::vector<ZipEntry> entries;
    std::string err;
    if (!zip_index(z, entries, err)) return fail("Not imported. " + err);

    /* <save>/<file>, or one folder around that. */
    bool flat = false, wrapped = true, sfo = false;
    for (const ZipEntry& e : entries) {
        if (e.parts.size() == 2 && e.parts[1] == "PARAM.SFO") flat = true;
        if (e.parts.size() != 3 || e.parts[0] != entries[0].parts[0]) wrapped = false;
    }
    if (!flat && !wrapped) return fail("Not imported. The .zip is not a saves archive: no <save>/PARAM.SFO in it.");
    const size_t skip = flat ? 0 : 1;
    for (const ZipEntry& e : entries) {
        if (e.parts.size() != skip + 2)
            return fail("Not imported. The .zip is not a saves archive: every file must be inside a save's folder.");
        if (e.parts[skip + 1] == "PARAM.SFO") sfo = true;
    }
    if (!sfo) return fail("Not imported. The .zip is not a saves archive: no <save>/PARAM.SFO in it.");

    const fs::path base = clean(c.backup_root);
    fs::create_directories(base, ec);
    if (ec) return fail("Not imported: could not make " + base.string() + ".");
    sweep_tmp(base);
    const std::string name = free_name(base, stamp(time(nullptr)) + "-import", "");
    const fs::path tmp = base / (".tmp-" + name);
    std::vector<uint8_t> data;
    for (const ZipEntry& e : entries) {
        data.assign(e.usize, 0);
        bool ok = true;
        if (e.method == 0) { if (e.usize) memcpy(data.data(), &z[e.data], e.usize); }
        else ok = inflate(z.data() + e.data, e.csize, data.data(), data.size());
        const std::string shown = e.parts[skip] + "/" + e.parts[skip + 1];
        if (!ok || crc32(data.data(), data.size()) != e.crc) {
            remove_tree(tmp);
            return fail("Not imported. The .zip is damaged (" + shown + ").");
        }
        if (e.parts[skip + 1] == "PARAM.SFO" && !is_psf(data)) {
            remove_tree(tmp);
            return fail("Not imported. " + shown + " is not a PS3 save's PARAM.SFO.");
        }
        const fs::path dir = tmp / e.parts[skip];
        fs::create_directories(dir, ec);
        if (ec || !write_file(dir / e.parts[skip + 1], data.data(), data.size())) {
            remove_tree(tmp);
            return fail("Not imported: could not write to " + base.string() + ".");
        }
    }
    fs::rename(tmp, base / name, ec);
    if (ec) {
        remove_tree(tmp);
        return fail("Not imported: could not write to " + base.string() + ".");
    }
    Result r;
    r.ok = true;
    r.path = name;
    r.message = "Imported as a backup. Restore it from the list to play with it.";
    return r;
}

std::vector<std::string> list_archives(const Config& c)
{
    struct Row { std::string path; fs::file_time_type t; };
    std::vector<Row> rows;
    std::error_code ec;
    fs::directory_iterator it(clean(c.export_root), ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.' || lower(it->path().extension().string()) != ".zip") continue;
        if (!is_file(it->path())) continue;
        std::error_code e2;
        rows.push_back({ it->path().string(), fs::last_write_time(it->path(), e2) });
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.t != b.t ? a.t > b.t : a.path > b.path; });
    std::vector<std::string> v;
    for (Row& r : rows) v.push_back(std::move(r.path));
    return v;
}

} /* namespace saves */
