// files.h - where things live on the MPC, the library of MIDI files, progress records, and
// the key of the project open on the MPC. Worker thread only (file I/O, allocation).
//
//   <drive>/SightReader/Library/<Folder>/*.mid    the library (tools/library.py fills it)
//   <drive>/SightReader/Library/<Folder>/index.tsv optional titles: file, title, composer, level, source
//   <drive>/SightReader/Generated/*.mid             exercises saved from GEN
//   <drive>/SightReader/progress.tsv                one line per piece
//   <drive>/SightReader/days.txt                    days practised (for the streak)
//   <plugin folder>/Content/Drills/<Folder>/*.mid   drills shipped with the plugin
//
// <drive> is the drive the plugin is installed on (the folder above "VST2 Plugins").
//
// SPDX-License-Identifier: MIT
#ifndef SR_FILES_H
#define SR_FILES_H

#include "inflate.h"
#include "music.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
  #include <dirent.h>
  #include <dlfcn.h>
  #include <sys/stat.h>
  #include <unistd.h>
  #define SR_POSIX 1
#else
  #define SR_POSIX 0
#endif

namespace sr {

inline bool readWholeFile(const std::string& path, std::vector<uint8_t>& out, size_t maxBytes = 64u << 20)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[16384]; size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) { out.insert(out.end(), buf, buf + n); if (out.size() > maxBytes) break; }
    std::fclose(f);
    return true;
}
inline bool writeWholeFile(const std::string& path, const void* data, size_t size)
{
    std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data, 1, size, f) == size;
    ok = (std::fclose(f) == 0) && ok;
    if (ok) ok = std::rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) std::remove(tmp.c_str());
    return ok;
}
inline void makeDirs(const std::string& path)
{
#if SR_POSIX
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i)
    {
        cur.push_back(path[i]);
        if ((path[i] == '/' && i > 0) || i + 1 == path.size()) mkdir(cur.c_str(), 0775);
    }
#else
    (void)path;
#endif
}
inline long fileMTime(const std::string& path)
{
#if SR_POSIX
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return (long)st.st_mtime;
#endif
    (void)path;
    return 0;
}
inline std::string lower(std::string s) { for (char& c : s) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c); return s; }

// The plugin's own folder and the drive it sits on.
struct Places
{
    std::string pluginDir, driveRoot, home;
    void locate(const std::string& pluginFolder)
    {
        pluginDir = pluginFolder;
        size_t v = pluginDir.find("/VST2 Plugins/");
        if (v == std::string::npos && pluginDir.size() >= 13 && pluginDir.compare(pluginDir.size() - 13, 13, "/VST2 Plugins") == 0) v = pluginDir.size() - 13;
        if (v != std::string::npos) driveRoot = pluginDir.substr(0, v);
        if (driveRoot.empty())
        {
#if SR_POSIX
            struct stat st;
            if (stat("/nvme", &st) == 0) driveRoot = "/nvme";
            else if (const char* h = std::getenv("HOME")) driveRoot = h;
            else driveRoot = "/tmp";
#else
            driveRoot = ".";
#endif
        }
        if (const char* over = std::getenv("SIGHTREADER_HOME")) home = over;   // tests
        else home = driveRoot + "/SightReader";
    }
};

// ---------------------------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------------------------
struct LibEntry
{
    std::string path, file, title, composer, source;
    int level = 0;
};
struct LibFolder { std::string name, dir; bool builtIn = false, playlist = false; std::vector<LibEntry> entries; };

inline std::string prettyName(const std::string& file)
{
    std::string s = file;
    size_t dot = s.rfind('.'); if (dot != std::string::npos) s = s.substr(0, dot);
    for (char& c : s) if (c == '_' || c == '-') c = ' ';
    return s;
}

inline void readIndex(const std::string& dir, std::map<std::string, LibEntry>& out)
{
    std::vector<uint8_t> raw;
    if (!readWholeFile(dir + "/index.tsv", raw, 4u << 20)) return;
    std::string text(raw.begin(), raw.end());
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t nl = text.find('\n', pos); if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos); pos = nl + 1;
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f; size_t a = 0;
        while (true) { size_t t = line.find('\t', a); f.push_back(line.substr(a, t == std::string::npos ? std::string::npos : t - a)); if (t == std::string::npos) break; a = t + 1; }
        LibEntry e; e.file = f[0];
        if (f.size() > 1) e.title = f[1];
        if (f.size() > 2) e.composer = f[2];
        if (f.size() > 3) e.level = std::atoi(f[3].c_str());
        if (f.size() > 4) e.source = f[4];
        out[e.file] = e;
    }
}

inline void scanFolder(LibFolder& fo)
{
    fo.entries.clear();
#if SR_POSIX
    std::map<std::string, LibEntry> idx;
    readIndex(fo.dir, idx);
    DIR* d = opendir(fo.dir.c_str());
    if (!d) return;
    while (dirent* de = readdir(d))
    {
        std::string n = de->d_name;
        if (n.empty() || n[0] == '.') continue;
        std::string l = lower(n);
        if (l.size() < 5 || (l.compare(l.size() - 4, 4, ".mid") != 0 && (l.size() < 6 || l.compare(l.size() - 5, 5, ".midi") != 0))) continue;
        LibEntry e;
        auto it = idx.find(n);
        if (it != idx.end()) e = it->second;
        e.file = n; e.path = fo.dir + "/" + n;
        if (e.title.empty()) e.title = prettyName(n);
        fo.entries.push_back(e);
    }
    closedir(d);
    std::sort(fo.entries.begin(), fo.entries.end(), [](const LibEntry& a, const LibEntry& b) { return lower(a.file) < lower(b.file); });
#endif
}

inline void scanLibrary(const Places& pl, std::vector<LibFolder>& out)
{
    out.clear();
#if SR_POSIX
    auto addTree = [&out](const std::string& root, bool builtIn, const char* prefix) {
        DIR* d = opendir(root.c_str());
        if (!d) return;
        std::vector<std::string> names;
        while (dirent* de = readdir(d)) { std::string n = de->d_name; if (!n.empty() && n[0] != '.') names.push_back(n); }
        closedir(d);
        std::sort(names.begin(), names.end());
        for (const std::string& n : names)
        {
            struct stat st;
            std::string p = root + "/" + n;
            if (stat(p.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            LibFolder f; f.name = std::string(prefix) + n; f.dir = p; f.builtIn = builtIn;
            scanFolder(f);
            if (!f.entries.empty()) out.push_back(f);
        }
    };
    addTree(pl.pluginDir + "/Content/Drills", true, "Drills: ");
    addTree(pl.home + "/Library", false, "");
    LibFolder gen; gen.name = "Generated"; gen.dir = pl.home + "/Generated"; scanFolder(gen);
    if (!gen.entries.empty()) out.push_back(gen);
#else
    (void)pl;
#endif
}

// ---------------------------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------------------------
struct PieceProgress
{
    int plays = 0, best = 0, last = 0, bestTempo = 0;
    long lastTime = 0; double minutes = 0;
    uint8_t sections[16] = {0};          // last pass accuracy per sixteenth of the piece, 0..100 (255 none)
    PieceProgress() { std::memset(sections, 255, sizeof(sections)); }
};

struct ProgressStore
{
    std::map<std::string, PieceProgress> pieces;
    std::vector<std::string> days;       // YYYY-MM-DD, sorted

    static std::string keyFor(const std::string& path, const std::string& home, const std::string& pluginDir)
    {
        if (!home.empty() && path.compare(0, home.size(), home) == 0) return path.substr(home.size());
        if (!pluginDir.empty() && path.compare(0, pluginDir.size(), pluginDir) == 0) return "builtin:" + path.substr(pluginDir.size());
        return path;
    }
    void load(const std::string& home)
    {
        pieces.clear(); days.clear();
        std::vector<uint8_t> raw;
        if (readWholeFile(home + "/progress.tsv", raw, 8u << 20))
        {
            std::string t(raw.begin(), raw.end()); size_t pos = 0;
            while (pos < t.size())
            {
                size_t nl = t.find('\n', pos); if (nl == std::string::npos) nl = t.size();
                std::string line = t.substr(pos, nl - pos); pos = nl + 1;
                if (line.empty() || line[0] == '#') continue;
                std::vector<std::string> f; size_t a = 0;
                while (true) { size_t tb = line.find('\t', a); f.push_back(line.substr(a, tb == std::string::npos ? std::string::npos : tb - a)); if (tb == std::string::npos) break; a = tb + 1; }
                if (f.size() < 7) continue;
                PieceProgress p;
                p.plays = std::atoi(f[1].c_str()); p.best = std::atoi(f[2].c_str()); p.last = std::atoi(f[3].c_str());
                p.lastTime = std::atol(f[4].c_str()); p.minutes = std::atof(f[5].c_str()); p.bestTempo = std::atoi(f[6].c_str());
                if (f.size() > 7) { const char* s = f[7].c_str(); for (int i = 0; i < 16 && *s; ++i) { p.sections[i] = (uint8_t)std::atoi(s); while (*s && *s != ',') ++s; if (*s == ',') ++s; } }
                pieces[f[0]] = p;
            }
        }
        if (readWholeFile(home + "/days.txt", raw, 1u << 20))
        {
            std::string t(raw.begin(), raw.end()); size_t pos = 0;
            while (pos < t.size()) { size_t nl = t.find('\n', pos); if (nl == std::string::npos) nl = t.size(); if (nl > pos) days.push_back(t.substr(pos, nl - pos)); pos = nl + 1; }
        }
    }
    void save(const std::string& home) const
    {
        makeDirs(home);
        std::string out = "# piece\tplays\tbest%\tlast%\tlast time\tminutes\tbest tempo%\tsection accuracy (16)\n";
        char buf[256];
        for (const auto& kv : pieces)
        {
            const PieceProgress& p = kv.second;
            std::snprintf(buf, sizeof(buf), "\t%d\t%d\t%d\t%ld\t%.1f\t%d\t", p.plays, p.best, p.last, p.lastTime, p.minutes, p.bestTempo);
            out += kv.first + buf;
            for (int i = 0; i < 16; ++i) { std::snprintf(buf, sizeof(buf), "%s%d", i ? "," : "", (int)p.sections[i]); out += buf; }
            out += "\n";
        }
        writeWholeFile(home + "/progress.tsv", out.data(), out.size());
        std::string d;
        for (const std::string& s : days) d += s + "\n";
        writeWholeFile(home + "/days.txt", d.data(), d.size());
    }
    static std::string today()
    {
        std::time_t t = std::time(nullptr); std::tm tmv;
#if SR_POSIX
        localtime_r(&t, &tmv);
#else
        tmv = *std::localtime(&t);
#endif
        char buf[16]; std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
        return buf;
    }
    void markToday()
    {
        std::string d = today();
        if (days.empty() || days.back() != d) { days.push_back(d); std::sort(days.begin(), days.end()); days.erase(std::unique(days.begin(), days.end()), days.end()); }
    }
    int streak() const
    {
        if (days.empty()) return 0;
        // count consecutive days ending today (or yesterday)
        auto toDay = [](const std::string& s) { std::tm tmv{}; tmv.tm_year = std::atoi(s.c_str()) - 1900; tmv.tm_mon = std::atoi(s.c_str() + 5) - 1; tmv.tm_mday = std::atoi(s.c_str() + 8); tmv.tm_hour = 12; return (long)(std::mktime(&tmv) / 86400); };
        long todayN = toDay(today());
        long expect = todayN;
        int n = 0;
        for (size_t i = days.size(); i-- > 0;)
        {
            long d = toDay(days[i]);
            if (d == expect) { ++n; --expect; }
            else if (n == 0 && d == todayN - 1) { n = 1; expect = d - 1; }
            else if (d < expect) break;
        }
        return n;
    }
};

// ---------------------------------------------------------------------------------------------
// The key of the project open on the MPC (its Sounds browser "key", and pad perform scale).
// ---------------------------------------------------------------------------------------------
struct MpcKey { bool valid = false; std::string projectKey, padScale; int padRoot = -1; };

inline std::string jsonStringAfter(const std::string& text, const char* key, size_t from = 0)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t k = text.find(pat, from);
    if (k == std::string::npos) return "";
    size_t c = text.find(':', k + pat.size()); if (c == std::string::npos) return "";
    size_t q = text.find('"', c); if (q == std::string::npos || q > c + 4) return "";
    size_t e = text.find('"', q + 1); if (e == std::string::npos) return "";
    return text.substr(q + 1, e - q - 1);
}

inline MpcKey readMpcKey()
{
    MpcKey mk;
    std::vector<uint8_t> raw;
    if (!readWholeFile("/data/Settings/MPC/CurrentProject", raw, 1u << 16)) return mk;
    std::string cur(raw.begin(), raw.end());
    size_t v = cur.find("lastProjectPath");
    if (v == std::string::npos) return mk;
    size_t q = cur.find("val=\"", v); if (q == std::string::npos) return mk;
    size_t e = cur.find('"', q + 5); if (e == std::string::npos) return mk;
    std::string path = cur.substr(q + 5, e - q - 5);
    // The autosave is often newer than the last explicit save.
    std::string dir = path.substr(0, path.rfind('/'));
    std::string base = path.substr(path.rfind('/') + 1);
    std::string name = base.size() > 4 ? base.substr(0, base.size() - 4) : base;
    std::string autosave = dir + "/" + name + "_[AutoSave]/" + base;
    std::string use = fileMTime(autosave) > fileMTime(path) ? autosave : path;
    if (!readWholeFile(use, raw)) return mk;
    std::vector<uint8_t> json;
    if (!gunzip(raw.data(), raw.size(), json)) return mk;
    std::string text(json.begin(), json.end());
    size_t data = text.find("\"data\"");
    mk.projectKey = jsonStringAfter(text, "key", data == std::string::npos ? 0 : data);
    size_t pps = text.find("\"padPerformSettings\"");
    if (pps != std::string::npos)
    {
        std::string scale = jsonStringAfter(text, "scaleType", pps);
        std::string root = jsonStringAfter(text, "baseNote", pps);
        mk.padScale = root + " " + scale;
        int used = 0; mk.padRoot = pcFromName(root.c_str(), &used);
    }
    mk.valid = !mk.projectKey.empty() || !mk.padScale.empty();
    return mk;
}

} // namespace sr
#endif
