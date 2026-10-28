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
// controller's vtable (027CEF2C; the outposts' controller 027D1A94 the same) slot 486, a 'ret 14h'. Replaced with: take
// the items out and toss them as a pickup.
constexpr uint32_t kZoneControllerVtable = 0x027CEF2C, kOutpostControllerVtable = 0x027D1A94;
constexpr uint32_t kDropSlot = 486, kDropStub = 0x01F6E470;
// A joined player's craft: the implementation looks the item id up in that player's profile (0084FE60, thiscall with the
// FString by value), but the host has only its own player's profile, so nothing was found and nothing made. The lookup
// is wrapped: a joined player's id is named by their loadout from the backend (their profile's schematics), and the
// host's own schematic item of that name stands in for it (every account has every schematic); without one, the recipe
// is made here directly.
constexpr uint32_t kFindProfileItemCall = 0x00871B3D, kFindProfileItem = 0x0084FE60;
constexpr int VtGetItemDefinition = 0x174;  // UFortItem
constexpr int SchematicRequirements = 0x43C, SchematicResult = 0x448, SchematicQuantity = 0x44C;  // UFortSchematicItemDefinition
constexpr int RequirementSize = 0xC, RequirementCount = 0x4, RequirementConsumed = 0x8;  // FSchematicRequirement

uintptr_t DefinitionOf(uintptr_t item) {
    return reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t)>(At<uint32_t>(At<uint32_t>(item, 0), VtGetItemDefinition))(item);
}

int CraftFrom(uintptr_t definition, uintptr_t pc);

int __fastcall Craft(uintptr_t schematic, void*, uintptr_t pc) {
    return CraftFrom(DefinitionOf(schematic), pc);
}

struct FStringArg {
    const wchar_t* data;
    int32_t num, max;
};

uintptr_t __fastcall FindProfileItem(uintptr_t pc, void*, FStringArg id) {
    std::string itemId;
    for (int32_t i = 0; id.data && i < id.num && id.data[i]; i++) itemId += static_cast<char>(id.data[i]);
    // The original takes the string (and frees it).
    uintptr_t item = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t, FStringArg)>(engine::Va(kFindProfileItem))(pc, id);
    if (item || !game::g_on.inventory || pc == engine::LocalPlayerController()) return item;
    std::string name = loadout::SchematicName(pc, itemId);
    if (name.empty()) {
        LOG("inventory: %s's item %s is not a schematic of their profile", engine::Name(pc).c_str(), itemId.c_str());
        return 0;
    }
    auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
        return text;
    };
    for (uintptr_t schematic : engine::FindAllA("FortSchematicItem"))
        if (lower(engine::Name(DefinitionOf(schematic))) == name) return schematic;
    for (uintptr_t definition : engine::FindAllA("FortSchematicItemDefinition")) {
        if (lower(engine::Name(definition)) != name) continue;
        CraftFrom(definition, pc);
        return 0;
    }
    LOG("inventory: no schematic %s here", name.c_str());
    return 0;
}

int CraftFrom(uintptr_t definition, uintptr_t pc) {
    if (!game::g_on.inventory) return 0;
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

// Disassembling: the inventory screen's Disassemble (FortInventoryContext.DisassembleItem, 02016170) and the server
// request FortPlayerController.ServerDisassembleInventoryItem(ItemGuid) (the controller's slot 461, 00872570) both call
// 0084B210: it checks that the item can be disassembled, plays the feedback and, on a client, sends the request; on the
// server it stops there (the rest is server code). Both calls are wrapped: on the server the item goes and something
// comes back. The item's DisassemblyLootTierKey names loot that only the server's tables have (this client carries
// LootTierData_Client alone; PickLootDrops finds nothing), so it is half of what crafting the item took, rounded up.
constexpr uint32_t kDisassemble = 0x0084B210;  // AFortPlayerController (FGuid by value) -> bool, thiscall, ret 10h
constexpr uint32_t kDisassembleCalls[] = { 0x0087257F, 0x020161BC };
constexpr int ActorRole = 0xC8, kRoleAuthority = 3;

bool __fastcall Disassemble(uintptr_t pc, void*, uint32_t g0, uint32_t g1, uint32_t g2, uint32_t g3) {
    bool ok = reinterpret_cast<bool(__thiscall*)(uintptr_t, uint32_t, uint32_t, uint32_t, uint32_t)>(engine::Va(kDisassemble))(
        pc, g0, g1, g2, g3);
    if (!ok || !game::g_on.inventory || At<uint8_t>(pc, ActorRole) != kRoleAuthority) return ok;
    const uint32_t guid[4] = { g0, g1, g2, g3 };
    uintptr_t definition = 0;
    if (!player::RemoveItem(pc, guid, 1, &definition)) {
        LOG("inventory: nothing to disassemble for that item");
        return ok;
    }
    uintptr_t recipe = 0;
    for (uintptr_t schematic : engine::FindAllA("FortSchematicItemDefinition"))
        if (At<uint32_t>(schematic, SchematicResult) == definition) recipe = schematic;
    std::string got;
    player::RefreshOnce once;
    engine::TArray* requirements = recipe ? &At<engine::TArray>(recipe, SchematicRequirements) : nullptr;
    for (int32_t i = 0; requirements && requirements->data && i < requirements->num; i++) {
        uintptr_t requirement = requirements->data + static_cast<uintptr_t>(i) * RequirementSize;
        uintptr_t item = At<uint32_t>(requirement, 0);
        int count = (At<int32_t>(requirement, RequirementCount) + 1) / 2;
        if (!At<uint8_t>(requirement, RequirementConsumed) || !engine::IsA(item, "FortItemDefinition") || count <= 0) continue;
        player::Receive(pc, item, count);
        got += " " + std::to_string(count) + " " + engine::Name(item);
    }
    LOG("inventory: %s disassembled %s:%s", engine::Name(pc).c_str(), engine::Name(definition).c_str(),
        got.empty() ? " nothing came back (no recipe for it)" : got.c_str());
    return ok;
}
}  // namespace

bool Install() {
    bool craft = engine::PatchPointer(kCraftEntry, kCraftStub, reinterpret_cast<void*>(&Craft));
    bool joined = engine::PatchCall(kFindProfileItemCall, kFindProfileItem, reinterpret_cast<void*>(&FindProfileItem));
    // Both controller kinds that play zones: the Zone one and the outposts' (its own vtable, the same slot).
    int drop = 0, disassemble = 0;
    for (uint32_t vtable : { kZoneControllerVtable, kOutpostControllerVtable })
        drop += engine::PatchPointer(vtable + kDropSlot * 4, kDropStub, reinterpret_cast<void*>(&SpawnInventoryDrop));
    for (uint32_t site : kDisassembleCalls) disassemble += engine::PatchCall(site, kDisassemble, reinterpret_cast<void*>(&Disassemble));
    LOG("inventory: crafting %s%s, dropping %s, disassembling %s", craft ? "restored" : "NOT restored",
        joined ? " (for joined players too)" : "", drop == 2 ? "restored" : "NOT restored", disassemble == 2 ? "restored" : "NOT restored");
    return craft && drop == 2;
}

// `craft [schematic name part]`: the inventory screen's Craft button (FortInventoryContext.CraftSchematic) on the first
// schematic of this player's profile whose definition has the part, with CanCraftSchematic's answer logged first.
void CraftTest(const std::string& part) {
    uintptr_t pc = engine::LocalPlayerController();
    uintptr_t library = engine::FindObject("/Script/FortniteGame.Default__BlueprintContextLibrary", "BlueprintContextLibrary");
    struct {
        uintptr_t object, cls, result;
    } context{ pc, engine::FindClass("/Script/FortniteUI.FortInventoryContext"), 0 };
    engine::ProcessEvent(library, engine::FindFunction("/Script/FortniteGame.BlueprintContextLibrary.GetContext"), &context);
    uintptr_t schematic = 0;
    for (uintptr_t item : engine::FindAllA("FortSchematicItem")) {
        if (part.empty() || engine::Name(DefinitionOf(item)).find(part) != std::string::npos) {
            schematic = item;
            break;
        }
    }
    if (!engine::IsA(context.result, "FortInventoryContext") || !schematic) {
        LOG("inventory: craft needs the inventory context (%s) and a schematic like \"%s\" (%s)", engine::Describe(context.result).c_str(),
            part.c_str(), engine::Describe(schematic).c_str());
        return;
    }
    struct {
        uintptr_t item;
        uint8_t failCause;
        bool result;
        uint8_t pad[2];
    } can{ schematic }, craft{ schematic };
    engine::ProcessEvent(context.result, engine::FindFunction("/Script/FortniteUI.FortInventoryContext.CanCraftSchematic"), &can);
    engine::ProcessEvent(context.result, engine::FindFunction("/Script/FortniteUI.FortInventoryContext.CraftSchematic"), &craft);
    LOG("inventory: craft %s: can craft %d (fail cause %d), craft %d (fail cause %d)", engine::Describe(schematic).c_str(), can.result,
        can.failCause, craft.result, craft.failCause);
}

// `disassemble <item name part>`: the inventory screen's Disassemble (FortInventoryContext.DisassembleItem) on this
// player's first world item whose definition has the part.
void DisassembleTest(const std::string& part) {
    constexpr int PcWorldInventory = 0xA24, InventoryInstances = 0x244 + 0xC8, ItemEntry = 0x4C, EntryDefinition = 0x10;
    uintptr_t pc = engine::LocalPlayerController();
    uintptr_t inventory = pc ? At<uint32_t>(pc, PcWorldInventory) : 0;
    uintptr_t item = 0;
    engine::TArray* instances = inventory ? &At<engine::TArray>(inventory, InventoryInstances) : nullptr;
    for (int32_t i = 0; instances && instances->data && i < instances->num && !item; i++) {
        uintptr_t candidate = At<uint32_t>(instances->data, i * 4);
        if (candidate && engine::Name(At<uint32_t>(candidate, ItemEntry + EntryDefinition)).find(part) != std::string::npos) item = candidate;
    }
    uintptr_t library = engine::FindObject("/Script/FortniteGame.Default__BlueprintContextLibrary", "BlueprintContextLibrary");
    struct {
        uintptr_t object, cls, result;
    } context{ pc, engine::FindClass("/Script/FortniteUI.FortInventoryContext"), 0 };
    engine::ProcessEvent(library, engine::FindFunction("/Script/FortniteGame.BlueprintContextLibrary.GetContext"), &context);
    if (!item || !engine::IsA(context.result, "FortInventoryContext")) {
        LOG("inventory: disassemble needs an item like \"%s\" (%s) and the inventory context", part.c_str(), engine::Describe(item).c_str());
        return;
    }
    struct {
        uintptr_t item;
        bool result;
        uint8_t pad[3];
    } p{ item };
    engine::ProcessEvent(context.result, engine::FindFunction("/Script/FortniteUI.FortInventoryContext.DisassembleItem"), &p);
    LOG("inventory: disassemble %s: %d", engine::Describe(item).c_str(), p.result);
}

}  // namespace inventory
