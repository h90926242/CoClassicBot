#include "offsets.h"
#include "base.h"
#include "log.h"
#include <detours.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

extern HMODULE g_hModule;

namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;

enum class Kind { Function, Global };

struct EntryDef {
    const char* name;
    Kind kind;
    uintptr_t defaultRva;     // best known RVA
    const char* defaultBuild; // build id defaultRva was verified on
    bool required;
};

// Build tag for the RVAs that used to be hardcoded (upstream commit 254ace7,
// 2026-03-29). These are only trusted if the user explicitly clears "build".
constexpr const char* kLegacyBuild = "legacy-2026-03-29";
// ImConquer.exe version 1079 (PE stamp 0x6AB0822B), as computed by ComputeBuildId().
// RVAs tagged with it were found and verified live with Cheat Engine on 2026-09-27.
constexpr const char* kBuild1079 = "1214010-3488e6a61f50d9f5";
constexpr const char* kJsonName = "coclassic_offsets.json";

// Order must match enum GameAddr.
constexpr EntryDef kDefs[] = {
    { "HeroPtr",                   Kind::Global,   0x6BCEF0, kBuild1079,   true  },
    { "RoleMgr",                   Kind::Global,   0x6C4F08, kBuild1079,   true  },
    { "EntityInfo",                Kind::Global,   0x4DF590, kLegacyBuild, false },
    { "EntitySet",                 Kind::Global,   0x4DF5F0, kLegacyBuild, false },
    { "GameMap",                   Kind::Global,   0x6B9B40, kBuild1079,   false },
    { "FillRectPtr",               Kind::Global,   0x408D10, kLegacyBuild, false },
    { "UIWindowMgr",               Kind::Global,   0x6B8E48, kBuild1079,   false },
    { "CEntity_RenderVisual",      Kind::Function, 0x1AFD20, kLegacyBuild, false },
    { "CNetClient_GetInstance",    Kind::Function, 0x0B9490, kLegacyBuild, false },
    { "CNetClient_SendMsg",        Kind::Function, 0x18EEA0, kLegacyBuild, false },
    { "CHero_GetMaxHp",            Kind::Function, 0x179980, kLegacyBuild, false },
    { "CHero_GetMaxMana",          Kind::Function, 0x179B90, kLegacyBuild, false },
    { "CStatTable_GetValue",       Kind::Function, 0x1F1490, kLegacyBuild, false },
    { "CHero_GetStat",             Kind::Function, 0x1B6BC0, kBuild1079,   false },
    { "StatBlock_Decrypt",         Kind::Function, 0x1EA9E0, kBuild1079,   false },
    { "CHero_GetCurrentMana",      Kind::Function, 0x1A58F0, kLegacyBuild, false },
    { "CHero_FatalStrike",         Kind::Function, 0x2297B0, kLegacyBuild, false },
    { "CHero_Walk",                Kind::Function, 0x23A940, kBuild1079,   false },
    { "CHero_Jump",                Kind::Function, 0x23AC50, kBuild1079,   false },
    { "CHero_StartMining",         Kind::Function, 0x18BB10, kLegacyBuild, false },
    { "MsgUpdate_Process",         Kind::Function, 0x1E0870, kLegacyBuild, false },
    { "TradeWindow_HandleMessage", Kind::Function, 0x10B1F0, kLegacyBuild, false },
    { "CGameUI_ShowMsg",           Kind::Function, 0x004370, kBuild1079,   false },
    { "GetTimestamp",              Kind::Function, 0x0A7A10, kBuild1079,   false },
};
static_assert(std::size(kDefs) == static_cast<size_t>(GameAddr::Count), "kDefs out of sync with GameAddr");

uintptr_t g_addr[static_cast<size_t>(GameAddr::Count)] = {};

// ── Module memory ────────────────────────────────────────────────────

struct Region { const uint8_t* begin; const uint8_t* end; };

size_t ModuleSize()
{
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_qwModuleBase);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_qwModuleBase + dos->e_lfanew);
    return nt->OptionalHeader.SizeOfImage;
}

// Executable, committed pages of the game module. Uses VirtualQuery rather
// than section headers because the Themida-packed image has unreliable ones.
std::vector<Region> CodeRegions()
{
    std::vector<Region> out;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(g_qwModuleBase);
    const uint8_t* end = p + ModuleSize();
    MEMORY_BASIC_INFORMATION mbi;
    while (p < end && VirtualQuery(p, &mbi, sizeof(mbi))) {
        const uint8_t* rb = static_cast<const uint8_t*>(mbi.BaseAddress);
        const uint8_t* re = (std::min)(rb + mbi.RegionSize, end);
        const DWORD prot = mbi.Protect & 0xFF;
        const bool exec = prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE
                       || prot == PAGE_EXECUTE_WRITECOPY;
        if (mbi.State == MEM_COMMIT && exec && !(mbi.Protect & PAGE_GUARD))
            out.push_back({ (std::max)(rb, p), re });
        p = rb + mbi.RegionSize;
    }
    return out;
}

bool InRegions(const uint8_t* a, size_t n, const std::vector<Region>& regions)
{
    for (const auto& r : regions)
        if (a >= r.begin && a + n <= r.end)
            return true;
    return false;
}

bool IsReadable(uintptr_t a, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)))
        return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    return a + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

size_t InstrLen(const uint8_t* p)
{
    // Detours' disassembler doubles as a length decoder when pDst is null.
    PVOID next = DetourCopyInstruction(nullptr, nullptr, const_cast<uint8_t*>(p), nullptr, nullptr);
    return next ? static_cast<size_t>(static_cast<const uint8_t*>(next) - p) : 0;
}

// Sanity check for an RVA that comes from the JSON (cached or typed in by hand).
bool Validate(Kind kind, uintptr_t a, const std::vector<Region>& code)
{
    const uintptr_t base = static_cast<uintptr_t>(g_qwModuleBase);
    if (a <= base || a >= base + ModuleSize())
        return false;

    // Themida may leave data pages executable, so don't require globals to be outside code.
    if (kind == Kind::Global)
        return IsReadable(a, 8);

    const auto* p = reinterpret_cast<const uint8_t*>(a);
    if (!InRegions(p, 16, code))
        return false;
    // MSVC pads between functions with int3/ret/nop and aligns to 16.
    const uint8_t prev = p[-1];
    const bool boundary = prev == 0xCC || prev == 0xC3 || prev == 0x90 || (a & 0xF) == 0;
    return boundary && InstrLen(p) != 0;
}

// ── Signatures ───────────────────────────────────────────────────────

using Pattern = std::vector<int16_t>;   // -1 = wildcard byte

struct Sig {
    Pattern pat;
    size_t dispOff = 0;    // Global: offset of the rip-relative disp32 in pat
    size_t instrEnd = 0;   // Global: offset of the end of the referencing instruction
};

std::string PatternToString(const Pattern& pat)
{
    std::string s;
    char buf[4];
    for (int16_t b : pat) {
        if (!s.empty()) s += ' ';
        if (b < 0) { s += "??"; continue; }
        snprintf(buf, sizeof(buf), "%02X", b);
        s += buf;
    }
    return s;
}

Pattern PatternFromString(const std::string& s)
{
    Pattern pat;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == ' ') { ++i; continue; }
        if (s[i] == '?') {
            pat.push_back(-1);
            while (i < s.size() && s[i] == '?') ++i;
            continue;
        }
        pat.push_back(static_cast<int16_t>(strtoul(s.substr(i, 2).c_str(), nullptr, 16)));
        i += 2;
    }
    return pat;
}

bool MatchAt(const uint8_t* p, const Pattern& pat)
{
    for (size_t i = 0; i < pat.size(); ++i)
        if (pat[i] >= 0 && p[i] != static_cast<uint8_t>(pat[i]))
            return false;
    return true;
}

std::vector<const uint8_t*> FindAll(const Pattern& pat, const std::vector<Region>& code, size_t maxHits)
{
    std::vector<const uint8_t*> hits;
    if (pat.empty() || pat[0] < 0)
        return hits;
    const auto first = static_cast<uint8_t>(pat[0]);
    for (const auto& r : code) {
        if (static_cast<size_t>(r.end - r.begin) < pat.size())
            continue;
        const uint8_t* last = r.end - pat.size();
        for (const uint8_t* p = r.begin; p <= last; ++p) {
            p = static_cast<const uint8_t*>(memchr(p, first, last - p + 1));
            if (!p)
                break;
            if (MatchAt(p, pat)) {
                hits.push_back(p);
                if (hits.size() >= maxHits)
                    return hits;
            }
        }
    }
    return hits;
}

// Appends one instruction, wildcarding the bytes that move when the game is
// relinked: rip-relative displacements and relative branch/call targets.
void AppendInstr(Pattern& pat, const uint8_t* p, size_t len)
{
    const size_t start = pat.size();
    for (size_t i = 0; i < len; ++i)
        pat.push_back(p[i]);
    auto wild = [&](size_t from, size_t n) {
        for (size_t k = from; k < from + n && k < len; ++k)
            pat[start + k] = -1;
    };

    if (len == 5 && (p[0] == 0xE8 || p[0] == 0xE9)) {             // call/jmp rel32
        wild(1, 4);
        return;
    }
    if (len == 6 && p[0] == 0x0F && (p[1] & 0xF0) == 0x80) {      // jcc rel32
        wild(2, 4);
        return;
    }
    if (len == 2 && (p[0] == 0xEB || (p[0] & 0xF0) == 0x70)) {    // jmp/jcc rel8
        wild(1, 1);
        return;
    }
    // ModRM with mod=00 rm=101 means [rip + disp32]. Opcode is at least one
    // byte in, so start at 1; a false positive only over-wildcards.
    for (size_t j = 1; j + 5 <= len; ++j)
        if ((p[j] & 0xC7) == 0x05)
            wild(j + 1, 4);
}

// Extends `pat` (which already describes the bytes at `target`) instruction by
// instruction starting at `next` until it matches `target` and nothing else.
bool ExtendUntilUnique(Pattern& pat, const uint8_t* target, const uint8_t* next,
                       const std::vector<Region>& code)
{
    std::vector<const uint8_t*> cands;
    bool haveCands = false;
    for (int n = 0; n < 48 && pat.size() <= 96; ++n) {
        if (pat.size() >= 6) {
            if (!haveCands) {
                cands = FindAll(pat, code, SIZE_MAX);
                haveCands = true;
            } else {
                std::erase_if(cands, [&](const uint8_t* c) { return !MatchAt(c, pat); });
            }
            if (cands.size() == 1 && cands[0] == target)
                return true;
            if (cands.empty())
                return false;
        }
        if (!InRegions(next, 16, code))
            return false;
        const size_t len = InstrLen(next);
        if (!len)
            return false;
        AppendInstr(pat, next, len);
        next += len;
    }
    return false;
}

std::optional<Sig> MakeFunctionSig(uintptr_t fn, const std::vector<Region>& code)
{
    Sig sig;
    const auto* p = reinterpret_cast<const uint8_t*>(fn);
    if (!ExtendUntilUnique(sig.pat, p, p, code))
        return std::nullopt;
    return sig;
}

// Globals have no bytes of their own to match, so we sign an instruction that
// references them (mov/lea/cmp reg, [rip+disp32]) plus enough context after it.
std::optional<Sig> MakeGlobalSig(uintptr_t target, const std::vector<Region>& code)
{
    int tried = 0;
    for (const auto& r : code) {
        for (const uint8_t* d = r.begin + 4; d + 4 <= r.end; ++d) {
            int32_t disp;
            memcpy(&disp, d, 4);
            if (reinterpret_cast<uintptr_t>(d + 4) + disp != target)
                continue;
            if ((d[-1] & 0xC7) != 0x05)
                continue;
            // Find where the referencing instruction starts (opcode + optional prefixes).
            const uint8_t* start = nullptr;
            for (int back = 2; back <= 4 && !start; ++back)
                if (InstrLen(d - back) == static_cast<size_t>(back + 4))
                    start = d - back;
            if (!start)
                continue;

            Sig sig;
            const size_t len = static_cast<size_t>(d + 4 - start);
            AppendInstr(sig.pat, start, len);
            sig.dispOff = static_cast<size_t>(d - start);
            sig.instrEnd = len;
            if (ExtendUntilUnique(sig.pat, start, start + len, code))
                return sig;
            if (++tried >= 32)
                return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<uintptr_t> ApplySig(const Sig& sig, Kind kind, const std::vector<Region>& code)
{
    const auto hits = FindAll(sig.pat, code, 2);
    if (hits.size() != 1)
        return std::nullopt;
    if (kind == Kind::Function)
        return reinterpret_cast<uintptr_t>(hits[0]);
    int32_t disp;
    memcpy(&disp, hits[0] + sig.dispOff, 4);
    return reinterpret_cast<uintptr_t>(hits[0]) + sig.instrEnd + disp;
}

// ── Build id / persistence ───────────────────────────────────────────

std::string ComputeBuildId()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::ifstream f(exe, std::ios::binary);
    uint64_t h = 1469598103934665603ULL;   // FNV-1a 64
    uint64_t size = 0;
    std::vector<char> buf(1 << 16);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto got = static_cast<size_t>(f.gcount());
        for (size_t i = 0; i < got; ++i) {
            h ^= static_cast<uint8_t>(buf[i]);
            h *= 1099511628211ULL;
        }
        size += got;
    }
    if (size == 0) {
        // Couldn't read the exe — fall back to the in-memory PE header.
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_qwModuleBase);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_qwModuleBase + dos->e_lfanew);
        h = (static_cast<uint64_t>(nt->FileHeader.TimeDateStamp) << 32) | nt->OptionalHeader.SizeOfImage;
    }
    char out[48];
    snprintf(out, sizeof(out), "%llx-%llx", static_cast<unsigned long long>(size),
             static_cast<unsigned long long>(h));
    return out;
}

fs::path JsonPath()
{
    wchar_t dll[MAX_PATH] = {};
    GetModuleFileNameW(g_hModule, dll, MAX_PATH);
    return fs::path(dll).parent_path() / kJsonName;
}

json LoadJson()
{
    std::ifstream f(JsonPath());
    if (!f)
        return json::object();
    json doc = json::parse(f, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        spdlog::error("[offsets] {} is not valid JSON — ignoring it", kJsonName);
        return json::object();
    }
    return doc;
}

void SaveJson(const json& doc)
{
    const fs::path path = JsonPath();
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) {
            spdlog::error("[offsets] Cannot write {}", tmp.string());
            return;
        }
        f << doc.dump(2);
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec)
        spdlog::error("[offsets] Cannot replace {}: {}", path.string(), ec.message());
}

std::string Hex(uintptr_t v)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────

bool GameOffsets::Resolve()
{
    if (!g_qwModuleBase)
        return false;

    static const std::string build = ComputeBuildId();
    static bool loggedBuild = false;
    if (!loggedBuild) {
        spdlog::info("[offsets] Game build id: {}", build);
        loggedBuild = true;
    }

    const uintptr_t base = static_cast<uintptr_t>(g_qwModuleBase);
    const auto code = CodeRegions();
    json doc = LoadJson();
    if (!doc["entries"].is_object())
        doc["entries"] = json::object();
    json& entries = doc["entries"];
    bool changed = false;

    for (size_t i = 0; i < std::size(kDefs); ++i) {
        if (g_addr[i])
            continue;
        const EntryDef& def = kDefs[i];

        json& e = entries[def.name];
        if (!e.is_object()) {
            e = { { "rva", Hex(def.defaultRva) }, { "build", def.defaultBuild } };
            changed = true;
        } else if (def.defaultBuild == build && e.value("build", std::string()) != build) {
            // The DLL ships a verified RVA for exactly this game build — prefer it
            // over whatever an older JSON remembered.
            e = { { "rva", Hex(def.defaultRva) }, { "build", def.defaultBuild } };
            changed = true;
        }
        const std::string entryBuild = e.value("build", std::string());
        const uintptr_t rva = strtoull(e.value("rva", std::string("0")).c_str(), nullptr, 16);

        uintptr_t addr = 0;
        const char* how = nullptr;

        // 1. Same build (or manually entered): trust the stored RVA if it looks sane.
        if (rva && (entryBuild == build || entryBuild.empty())) {
            if (Validate(def.kind, base + rva, code)) {
                addr = base + rva;
                how = entryBuild.empty() ? "manual rva" : "cached rva";
            } else if (entryBuild.empty()) {
                spdlog::warn("[offsets] {}: manual rva {} fails sanity check — ignoring it",
                             def.name, Hex(rva));
            }
        }

        // 2. Different build: follow the patch with the stored signature.
        if (!addr && e.contains("sig")) {
            Sig sig;
            sig.pat = PatternFromString(e.value("sig", std::string()));
            sig.dispOff = e.value("dispOff", size_t{0});
            sig.instrEnd = e.value("instrEnd", size_t{0});
            if (auto found = ApplySig(sig, def.kind, code); found && Validate(def.kind, *found, code)) {
                addr = *found;
                how = "signature";
            }
        }

        if (!addr)
            continue;
        g_addr[i] = addr;
        spdlog::info("[offsets] {} = base+{} ({})", def.name, Hex(addr - base), how);

        // Re-sign on every new build so the *next* patch can be followed too.
        if (entryBuild != build || !e.contains("sig")) {
            auto sig = def.kind == Kind::Function ? MakeFunctionSig(addr, code)
                                                  : MakeGlobalSig(addr, code);
            if (sig) {
                e["sig"] = PatternToString(sig->pat);
                e["dispOff"] = sig->dispOff;
                e["instrEnd"] = sig->instrEnd;
            } else {
                e.erase("sig");
                spdlog::warn("[offsets] {}: could not build a unique signature; "
                             "it will need a manual rva after the next patch", def.name);
            }
            e["rva"] = Hex(addr - base);
            e["build"] = build;
            changed = true;
        }
    }

    if (changed)
        SaveJson(doc);

    for (size_t i = 0; i < std::size(kDefs); ++i)
        if (kDefs[i].required && !g_addr[i])
            return false;
    return true;
}

uintptr_t GameOffsets::Get(GameAddr id)
{
    const auto i = static_cast<size_t>(id);
    return i < std::size(g_addr) ? g_addr[i] : 0;
}

const char* GameOffsets::Name(GameAddr id)
{
    const auto i = static_cast<size_t>(id);
    return i < std::size(kDefs) ? kDefs[i].name : "?";
}

void GameOffsets::ReportUnresolved()
{
    int missing = 0;
    for (size_t i = 0; i < std::size(kDefs); ++i) {
        if (g_addr[i])
            continue;
        ++missing;
        spdlog::error("[offsets] UNRESOLVED: {}", kDefs[i].name);
    }
    if (missing == 0) {
        spdlog::info("[offsets] All {} game addresses resolved", std::size(kDefs));
        return;
    }
    spdlog::error("[offsets] {} address(es) could not be found for this game build; "
                  "features that use them are disabled.", missing);
    spdlog::error("[offsets] To fix: open {} next to coclassic.dll, set \"rva\" to the new "
                  "address (from Ghidra/Cheat Engine) and \"build\" to \"\" for each entry above, "
                  "then re-inject. A signature is generated automatically after that.", kJsonName);
}
