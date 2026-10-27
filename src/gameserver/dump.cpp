#include "dump.h"
#include "log.h"
#include "memory.h"
#include "ue.h"
#include <cstdio>
#include <map>

namespace dump {

static FILE* OpenOut(const std::wstring& dir, const wchar_t* name) {
    return _wfopen((dir + name).c_str(), L"w");
}

void Names(const std::wstring& dir) {
    FILE* f = OpenOut(dir, L"names.txt");
    if (!f) return;
    int written = 0;
    for (int32_t i = 0; i < ue::g.numNames; i++) {
        std::string name = ue::NameOf(i);
        if (name.empty()) continue;
        fprintf(f, "%d\t%s\n", i, name.c_str());
        written++;
    }
    fclose(f);
    LOG("names.txt: %d names", written);
}

void Objects(const std::wstring& dir) {
    FILE* all = OpenOut(dir, L"objects.txt");
    FILE* classes = OpenOut(dir, L"classes.txt");
    if (!all || !classes) return;

    std::map<std::string, int> byClass;
    int written = 0;
    for (int32_t i = 0; i < ue::g.numObjects; i++) {
        uintptr_t object = ue::ObjectAt(i);
        if (!object) continue;
        std::string cls = ue::ClassName(object);
        std::string path = ue::PathName(object);
        fprintf(all, "[%d] 0x%08X %s %s\n", i, static_cast<unsigned>(object), cls.c_str(), path.c_str());
        if (cls == "Class" || cls == "ScriptStruct" || cls == "Enum" || cls == "BlueprintGeneratedClass")
            fprintf(classes, "%s %s\n", cls.c_str(), path.c_str());
        byClass[cls]++;
        written++;
    }
    fclose(all);
    fclose(classes);
    LOG("objects.txt: %d objects", written);

    // Most common object types, to sanity-check the dump at a glance.
    std::multimap<int, std::string, std::greater<int>> top;
    for (auto& kv : byClass) top.insert({ kv.second, kv.first });
    int shown = 0;
    for (auto& kv : top) {
        LOG("  %6d x %s", kv.first, kv.second.c_str());
        if (++shown == 15) break;
    }
}

// Addresses hardcoded by Blaze (its Dumper-7 SDK) and Alpharium. If they match what we found, their SDK and offsets
// were made from this exact exe and can be relied on for the next steps.
void KnownOffsets() {
    const uint32_t kGObjects = 0x2DC3AAC, kGNames = 0x2DBC5A8, kGWorld = 0x2E68BEC, kProcessEvent = 0x9A9E70;
    const int kProcessEventIdx = 0x34;
    auto rva = [](uintptr_t addr) { return static_cast<uint32_t>(addr - ue::g.exeBase); };
    auto verdict = [](bool same) { return same ? "match" : "DIFFERENT"; };

    LOG("known offsets (Blaze SDK / Alpharium):");
    LOG("  GObjects      ours exe+0x%X, theirs exe+0x%X: %s", rva(ue::g.objectArrayAddr), kGObjects,
        verdict(rva(ue::g.objectArrayAddr) == kGObjects));
    LOG("  GNames        ours exe+0x%X, theirs exe+0x%X: %s", rva(ue::g.namesPtrAddr), kGNames,
        verdict(rva(ue::g.namesPtrAddr) == kGNames));

    // ProcessEvent is UObject's vtable slot 0x34; object 0 is a package, which doesn't override it.
    uintptr_t first = ue::ObjectAt(0);
    uintptr_t vtable = mem::ReadOr<uint32_t>(first + ue::obj::VTable);
    uintptr_t processEvent = mem::ReadOr<uint32_t>(vtable + kProcessEventIdx * 4);
    LOG("  ProcessEvent  vtable[0x%X] of %s -> exe+0x%X, theirs exe+0x%X: %s", kProcessEventIdx,
        ue::FullName(first).c_str(), rva(processEvent), kProcessEvent, verdict(rva(processEvent) == kProcessEvent));

    uintptr_t world = mem::ReadOr<uint32_t>(ue::g.exeBase + kGWorld);
    LOG("  GWorld        exe+0x%X -> %s", kGWorld, world ? ue::FullName(world).c_str() : "null");
}

// Every loaded class (native or blueprint) in the families a mission needs, with its parent. Shows what the client
// really has, e.g. whether the mission rules sit in FortGameMode itself since there is no FortGameModeZone.
void MissionClasses() {
    const char* families[] = {
        "GameMode", "GameState", "PlayerController", "PlayerState", "GameSession",
        "MissionManager", "MissionGen", "WorldManager", "ZoneTheme", "AIDirector",
    };
    for (const char* family : families) {
        LOG("  *%s*:", family);
        int found = 0;
        for (int32_t i = 0; i < ue::g.numObjects; i++) {
            uintptr_t object = ue::ObjectAt(i);
            if (!object) continue;
            std::string cls = ue::ClassName(object);
            if (cls != "Class" && cls != "BlueprintGeneratedClass") continue;
            if (ue::ObjectName(object).find(family) == std::string::npos) continue;
            uintptr_t super = mem::ReadOr<uint32_t>(object + ue::strct::Super);
            LOG("    %-56s : %s", ue::PathName(object).c_str(), super ? ue::ObjectName(super).c_str() : "-");
            found++;
        }
        if (!found) LOG("    (none loaded)");
    }
}

}  // namespace dump
