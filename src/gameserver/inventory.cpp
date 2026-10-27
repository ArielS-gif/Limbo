#include "engine.h"
#include "log.h"
#include "mission.h"
#include <windows.h>
#include <algorithm>
#include <string>

namespace inventory {

using engine::At;

namespace {
// In-game crafting. The inventory screen calls FortPlayerController.ServerCraftSchematic with the schematic's profile
// item id; its implementation (00871AF0) finds the schematic item in the player's profile, asks it to craft (the
// UFortSchematicItem vtable 02734FFC, slot 130, given the controller) and reports an ItemCrafted mission event with the
// amount it returns. That slot is the shared 'xor eax,eax; ret 4' here, so nothing is ever made. It is replaced with
// the recipe itself: the requirements are checked against the world inventory, the consumed ones taken out, and the
// result added like a pickup.
constexpr uint32_t kCraftEntry = 0x02735204, kCraftStub = 0x015AE2C0;
// Dropping from the inventory screen: FortPlayerController.ServerSpawnInventoryDrop(ItemGuid, Count) ends in the Zone
// controller's vtable (027CEF2C) slot 486, a 'ret 14h'. Replaced with: take the items out and toss them as a pickup.
constexpr uint32_t kDropEntry = 0x027CF6C4, kDropStub = 0x01F6E470;
constexpr int VtGetItemDefinition = 0x174;  // UFortItem
constexpr int SchematicRequirements = 0x43C, SchematicResult = 0x448, SchematicQuantity = 0x44C;  // UFortSchematicItemDefinition
constexpr int RequirementSize = 0xC, RequirementCount = 0x4, RequirementConsumed = 0x8;  // FSchematicRequirement

int __fastcall Craft(uintptr_t schematic, void*, uintptr_t pc) {
    if (!game::g_on.inventory) return 0;
    auto getDefinition = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t)>(At<uint32_t>(At<uint32_t>(schematic, 0), VtGetItemDefinition));
    uintptr_t definition = getDefinition(schematic);
    uintptr_t result = engine::IsA(definition, "FortSchematicItemDefinition") ? At<uint32_t>(definition, SchematicResult) : 0;
    if (!engine::IsA(pc, "FortPlayerController") || !engine::IsA(result, "FortItemDefinition")) {
        LOG("inventory: can't craft %s (result %s)", engine::Describe(definition).c_str(), engine::Describe(result).c_str());
        return 0;
    }
    int quantity = (std::max)(1, At<int32_t>(definition, SchematicQuantity));
    engine::TArray& requirements = At<engine::TArray>(definition, SchematicRequirements);
    std::string missing, used;
    for (int32_t i = 0; requirements.data && i < requirements.num; i++) {
        uintptr_t requirement = requirements.data + static_cast<uintptr_t>(i) * RequirementSize;
        uintptr_t item = At<uint32_t>(requirement, 0);
        int32_t count = At<int32_t>(requirement, RequirementCount);
        int have = player::CountItems(pc, item);
        if (have < count) missing += " " + std::to_string(count - have) + " " + engine::Name(item);
    }
    if (!missing.empty() && !game::g_on.cheats) {
        LOG("inventory: can't craft %s, missing%s", engine::Name(result).c_str(), missing.c_str());
        return 0;
    }
    player::RefreshOnce once;  // the HUD once at the end, not after each material and the result
    for (int32_t i = 0; requirements.data && i < requirements.num; i++) {
        uintptr_t requirement = requirements.data + static_cast<uintptr_t>(i) * RequirementSize;
        if (!At<uint8_t>(requirement, RequirementConsumed)) continue;
        uintptr_t item = At<uint32_t>(requirement, 0);
        int taken = player::RemoveItems(pc, item, At<int32_t>(requirement, RequirementCount));
        if (taken) used += " " + std::to_string(taken) + " " + engine::Name(item);
    }
    player::Receive(pc, result, quantity);
    LOG("inventory: crafted %d x %s from %s; used%s%s", quantity, engine::Name(result).c_str(), engine::Name(definition).c_str(),
        used.empty() ? " nothing" : used.c_str(), missing.empty() ? "" : (" (missing" + missing + ", free with cheats)").c_str());
    return quantity;
}

void __fastcall SpawnInventoryDrop(uintptr_t pc, void*, uint32_t g0, uint32_t g1, uint32_t g2, uint32_t g3, int32_t count) {
    if (!game::g_on.inventory) return;
    const uint32_t guid[4] = { g0, g1, g2, g3 };
    uintptr_t definition = 0;
    int dropped = player::RemoveItem(pc, guid, count, &definition);
    if (!dropped) {
        LOG("inventory: nothing to drop for that item");
        return;
    }
    bool spawned = player::SpawnPickup(pc, definition, dropped);
    player::AfterDrop(pc, definition, guid);
    LOG("inventory: dropped %d x %s%s", dropped, engine::Name(definition).c_str(), spawned ? "" : " (no pickup could be spawned)");
}
}  // namespace

bool Install() {
    bool craft = engine::PatchPointer(kCraftEntry, kCraftStub, reinterpret_cast<void*>(&Craft));
    bool drop = engine::PatchPointer(kDropEntry, kDropStub, reinterpret_cast<void*>(&SpawnInventoryDrop));
    LOG("inventory: crafting %s, dropping %s", craft ? "restored" : "NOT restored", drop ? "restored" : "NOT restored");
    return craft && drop;
}

}  // namespace inventory
