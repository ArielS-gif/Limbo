#include "ue.h"
#include "log.h"
#include "memory.h"
#include <windows.h>
#include <cstring>
#include <vector>

namespace ue {

Globals g;

// Writable sections of the exe, where both globals live.
static std::vector<mem::Section> DataSections() {
    std::vector<mem::Section> out;
    for (auto& s : mem::ModuleSections(reinterpret_cast<HMODULE>(g.exeBase))) {
        if (s.characteristics & IMAGE_SCN_MEM_WRITE) out.push_back(s);
    }
    return out;
}

// Reads a NUL-terminated ANSI or UTF-16 string (UTF-16 is narrowed; FNames are ASCII in practice).
static std::string ReadName(uintptr_t addr, bool wide) {
    std::string out;
    for (int i = 0; i < 1024; i++) {
        if (wide) {
            uint16_t c;
            if (!mem::Read(addr + i * 2, c) || c == 0) break;
            out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        } else {
            char c;
            if (!mem::Read(addr + i, c) || c == 0) break;
            out.push_back(c);
        }
    }
    return out;
}

static bool EntryIs(uintptr_t entry, int32_t index, const char* expected) {
    int32_t stored;
    if (!mem::Read(entry + kNameEntryIndex, stored)) return false;
    if (stored != (index << 1) && stored != index) return false;  // 4.12 stores index << 1 | bIsWide
    return ReadName(entry + kNameEntryString, false) == expected;
}

bool FindNames() {
    for (auto& s : DataSections()) {
        for (uintptr_t addr = s.base; addr + 4 <= s.base + s.size; addr += 4) {
            uintptr_t names = mem::ReadOr<uint32_t>(addr);
            if (!mem::Readable(names, kNameChunkTable * 4 + 8)) continue;
            uintptr_t chunk0 = mem::ReadOr<uint32_t>(names);
            if (!mem::Readable(chunk0, 8)) continue;
            if (!EntryIs(mem::ReadOr<uint32_t>(chunk0), 0, "None")) continue;
            if (!EntryIs(mem::ReadOr<uint32_t>(chunk0 + 4), 1, "ByteProperty")) continue;

            int32_t count = mem::ReadOr<int32_t>(names + kNameChunkTable * 4);
            if (count < 1000 || count > 2 * 1024 * 1024) {
                LOG("names candidate at exe+0x%X has odd count %d, skipping", addr - g.exeBase, count);
                continue;
            }
            g.namesPtrAddr = addr;
            g.names = names;
            g.numNames = count;
            LOG("GNames: pointer at exe+0x%X -> TNameEntryArray 0x%08X, %d names", addr - g.exeBase, names, count);
            return true;
        }
    }
    LOG("GNames not found");
    return false;
}

// True if the first items of an FUObjectItem array (with the given stride) hold objects whose InternalIndex matches.
static int CountIndexMatches(uintptr_t items, int stride, int sample) {
    int matches = 0;
    for (int i = 0; i < sample; i++) {
        uintptr_t object = mem::ReadOr<uint32_t>(items + i * stride);
        if (!object) continue;
        if (!mem::Readable(object, 0x1C)) return 0;
        if (mem::ReadOr<int32_t>(object + obj::Index) != i) return 0;
        matches++;
    }
    return matches;
}

bool FindObjects() {
    const int strides[] = { 16, 12, 8, 20, 24 };
    for (auto& s : DataSections()) {
        for (uintptr_t addr = s.base; addr + 12 <= s.base + s.size; addr += 4) {
            int32_t max = mem::ReadOr<int32_t>(addr + 4);
            int32_t num = mem::ReadOr<int32_t>(addr + 8);
            if (max < 65536 || max > 16 * 1024 * 1024 || num < 1000 || num > max) continue;
            uintptr_t items = mem::ReadOr<uint32_t>(addr);
            if (!mem::Readable(items, 64 * 24)) continue;
            for (int stride : strides) {
                if (CountIndexMatches(items, stride, 64) < 48) continue;
                g.objectArrayAddr = addr;
                g.objects = items;
                g.objectItemSize = stride;
                g.maxObjects = max;
                g.numObjects = num;
                LOG("GObjects: array at exe+0x%X -> items 0x%08X, stride %d, %d/%d objects",
                    addr - g.exeBase, items, stride, num, max);
                return true;
            }
        }
    }
    LOG("GObjects not found");
    return false;
}

void Refresh() {
    g.numNames = mem::ReadOr<int32_t>(g.names + kNameChunkTable * 4, g.numNames);
    g.numObjects = mem::ReadOr<int32_t>(g.objectArrayAddr + 8, g.numObjects);
}

std::string NameOf(int32_t index) {
    if (index >= g.numNames) Refresh();
    if (index < 0 || index >= g.numNames) return "";
    uintptr_t chunk = mem::ReadOr<uint32_t>(g.names + (index / kNameChunkSize) * 4);
    if (!chunk) return "";
    uintptr_t entry = mem::ReadOr<uint32_t>(chunk + (index % kNameChunkSize) * 4);
    if (!entry) return "";
    int32_t stored = mem::ReadOr<int32_t>(entry + kNameEntryIndex);
    return ReadName(entry + kNameEntryString, (stored & 1) != 0);
}

uintptr_t ObjectAt(int32_t index) {
    if (index >= g.numObjects) Refresh();
    if (index < 0 || index >= g.numObjects) return 0;
    return mem::ReadOr<uint32_t>(g.objects + index * g.objectItemSize);
}

std::string ObjectName(uintptr_t object) {
    if (!mem::Readable(object, 0x1C)) return "<bad>";
    std::string name = NameOf(mem::ReadOr<int32_t>(object + obj::Name));
    int32_t number = mem::ReadOr<int32_t>(object + obj::NameNumber);
    if (number > 0) name += "_" + std::to_string(number - 1);
    return name;
}

std::string PathName(uintptr_t object) {
    std::vector<std::string> parts;
    for (int depth = 0; object && depth < 32; depth++) {
        parts.push_back(ObjectName(object));
        object = mem::ReadOr<uint32_t>(object + obj::Outer);
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!out.empty()) out += '.';
        out += *it;
    }
    return out;
}

std::string ClassName(uintptr_t object) {
    return ObjectName(mem::ReadOr<uint32_t>(object + obj::Class));
}

std::string FullName(uintptr_t object) {
    return ClassName(object) + " " + PathName(object);
}

bool IsA(uintptr_t object, const char* className) {
    uintptr_t cls = mem::ReadOr<uint32_t>(object + obj::Class);
    for (int depth = 0; cls && depth < 32; depth++) {
        if (ObjectName(cls) == className) return true;
        cls = mem::ReadOr<uint32_t>(cls + strct::Super);
    }
    return false;
}

}  // namespace ue
