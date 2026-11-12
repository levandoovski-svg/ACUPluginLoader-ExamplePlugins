#pragma once

#include <Windows.h>
#include <cstdint>
#include <vector>

// Byte-signature scanner for the live ACU.exe.
//
// The exe is packed on disk (its code section only exists after the game
// unpacks it), so signatures can only be matched in the running process. This
// is also why the patterns below are derived from live memory rather than from
// the file.
//
// The 1.5.0 and 1.5.1 builds keep the same instructions and only move them, so
// a signature keeps working across versions while a literal address does not.
namespace AcuScan
{
    inline uintptr_t ModuleBase()
    {
        HMODULE h = GetModuleHandleW(L"ACU.exe");
        if (!h)
            h = GetModuleHandleW(nullptr);
        return (uintptr_t)h;
    }

    // Pattern syntax: "48 8B ?? 24" ('?' or '??' matches any byte).
    // Returns the address of the single match, or 0 if absent/ambiguous.
    inline uintptr_t Find(const char* pattern)
    {
        auto hexDigit = [](char c) -> int
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };

        std::vector<uint8_t> bytes;
        std::vector<bool> wildcard;
        for (const char* s = pattern; *s; )
        {
            while (*s == ' ') ++s;
            if (!*s) break;
            if (*s == '?')
            {
                while (*s == '?') ++s;
                bytes.push_back(0);
                wildcard.push_back(true);
                continue;
            }
            int hi = hexDigit(s[0]);
            int lo = s[1] ? hexDigit(s[1]) : -1;
            if (hi < 0 || lo < 0) return 0;
            bytes.push_back((uint8_t)((hi << 4) | lo));
            wildcard.push_back(false);
            s += 2;
        }
        if (bytes.empty()) return 0;

        uintptr_t base = ModuleBase();
        if (!base) return 0;
        auto* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        auto* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

        uintptr_t found = 0;
        auto* sections = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        {
            const auto& sec = sections[i];
            if (!(sec.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            size_t size = sec.Misc.VirtualSize ? sec.Misc.VirtualSize : sec.SizeOfRawData;
            const uint8_t* p = (const uint8_t*)(base + sec.VirtualAddress);
            if (size < bytes.size()) continue;

            for (size_t k = 0; k + bytes.size() <= size; ++k)
            {
                size_t j = 0;
                for (; j < bytes.size(); ++j)
                    if (!wildcard[j] && p[k + j] != bytes[j]) break;
                if (j != bytes.size()) continue;
                if (found) return 0;   // ambiguous: refuse rather than guess
                found = (uintptr_t)(p + k);
            }
        }
        return found;
    }
}
