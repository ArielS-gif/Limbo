#include "engine.h"
#include "log.h"
#include "mission.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace player {

using engine::At;

namespace {
// What the server would have done for a player entering a zone, all compiled out here: nothing spawns the
// quickbars, K2_GiveItemToPlayer is a stub, ServerExecuteInventoryItem's implementation is 'ret 10h' (nothing ever
// equips), and picked-up items never reach the inventory. The working pieces (item instances, QuickBars
// ServerAddItemInternal, the OnRep and local-update functions, AFortPawn::EquipWeaponDefinition) are used instead.
constexpr int PcQuickBars = 0x9B8, PcWorldInventory = 0xA24, PcBackpackSize = 0xB90;
constexpr int PcBuildFreeBits = 0x8D0;     // bit 1 bBuildFree
constexpr int PcInfiniteAmmoBits = 0xB88;  // bit 0 bInfiniteAmmo
// Stamina: the zone pawn's MaxStamina is held by an effect aggregator at base 0 plus a +5 bonus (the server's hero stat
// setup that would give the real base is compiled out), so writes to the attribute snap back and sprint stays empty.
// FortCheatManager.ToggleInfiniteStamina's flag (bit 7 of the dword at +0x70) is set instead, with the other cheats.
constexpr int PcCheatManager = 0x2F0, CheatManagerFlags = 0x70, CheatInfiniteStamina = 0x80;
// The same aggregators hold the movement speeds: plain writes last only until the next effect touches the attribute
// (a hero perk's sprint bonus recomputed SprintSpeed from base 0: the player stood still while sprinting). Base values
// go in through FActiveGameplayEffectsContainer::ApplyModToAttribute(const FGameplayAttribute&, op, magnitude, ModData),
// thiscall, ret 10h, with Override; FGameplayAttribute is the attribute's UProperty*.
constexpr uint32_t kApplyModToAttribute = 0x01F47EC0;
constexpr int AbilitySystemActiveEffects = 0x4F4;  // UAbilitySystemComponent.ActiveGameplayEffects
constexpr uint32_t kModOverride = 3;               // EGameplayModOp::Override
constexpr int PcClientLoaded = 0x510, PcServerLoaded = 0x511, PcPlayerState = 0x254, PcPlayer = 0x288;
// AFortInventory.Inventory, an FFortItemList at 0x244: ReplicatedEntries (+0x80) and ItemInstances (+0xC8).
constexpr int InventoryEntries = 0x244 + 0x80, InventoryInstances = 0x244 + 0xC8;
// The inventory's pending changes (TArray<UFortWorldItem*>): HandleInventoryLocalUpdate passes them to the controller,
// which updates the inventory screen. Items changed here behind its back are queued so the open screen sees them.
constexpr int InventoryPendingChanged = 0x370, InventoryPendingRemoved = 0x37C;
// The replicated entries are a fast array (FFortItemList, a FFastArraySerializer): a client gets an entry only when its
// ReplicationKey changed (a new one also needs a ReplicationID), and nothing until the array's own key changed. The
// host's own player never needed it; a joined player's inventory does. IDCounter and ArrayReplicationKey follow the
// serializer's ItemMap; each FFortItemEntry starts with ReplicationID and ReplicationKey.
constexpr int ListIdCounter = 0x244 + 0x3C, ListArrayKey = 0x244 + 0x40, EntryReplicationId = 0x0, EntryReplicationKey = 0x4;
// AFortQuickBars.PrimaryQuickBar (FQuickBar: two focus ints, then TArray<FQuickBarSlot>, each a TArray<FGuid>).
constexpr int QuickBarsPrimary = 0x240, QuickBarSlots = 0x8, QuickBarSlotSize = 0xC;
constexpr int ItemOwnerInventory = 0x48, ItemEntry = 0x4C;  // UFortWorldItem
// FFortItemEntry (0x7C bytes). The members that own heap memory are emptied in the inventory's copy, so the item's
// entry and the copy never free the same buffer.
constexpr int EntrySize = 0x7C, EntryCount = 0xC, EntryDefinition = 0x10, EntryLoadedAmmo = 0x1C, EntryGuid = 0x38;
constexpr int kEntryHeapMembers[] = { 0x20 /*AlterationDefinitions*/, 0x2C /*ItemSource*/, 0x4C /*GiftingInfo name*/,
                                      0x64 /*StateValues*/ };
constexpr int PawnHealthSet = 0x754, PawnMovementSet = 0x75C, PawnAbilitySystem = 0x770;
// A gadget fires through its item: ClientExecuteInventoryItem has the gadget's definition activate the ability spec
// whose handle the item's entry holds (FFortItemEntry.GameplayAbilitySpecHandle), and the ability reads the definition
// back as the spec's source object (GetCurrentSourceItemDefinition). The server code that grants gadget abilities that
// way is compiled out; LinkGadget ties the ability granted through the effect to the items instead.
constexpr int AbilitySystemSpecs = 0x220;  // ActivatableAbilities.Items: FGameplayAbilitySpec, 0x50 each
constexpr int SpecSize = 0x50, SpecHandle = 0xC, SpecAbility = 0x10, SpecSource = 0x1C;
constexpr int EntryAbilityHandle = 0x78, ObjectClass = 0xC;
constexpr int HealthSetHealth = 0x20, HealthSetMaxHealth = 0x24, HealthSetCurrentShield = 0x28, HealthSetShield = 0x2C;
constexpr int AttrVitality = 0x90, AttrOffense = 0x94, AttrProwess = 0x98, AttrDefense = 0x9C;  // UFortPlayerAttrSet
constexpr int MovementWalk = 0x20, MovementRun = 0x24, MovementSprint = 0x28, MovementBackward = 0x2C;
constexpr int AttrStamina = 0x24, AttrRegenRate = 0x28, AttrRegenDelay = 0x2C, AttrMaxStamina = 0x30;
constexpr int AbilitySystemAbilityCount = 0x1A0 + 0x80 + 4;  // ActivatableAbilities.Items.Num
constexpr int EffectGrantedAbilities = 0x35C;                // UGameplayEffect: FGameplayAbilitySpecDef, class at +0
constexpr int ClassDefaultObject = 0xA8;
constexpr uint32_t kEquipWeaponDefinition = 0x0080A8D0;  // AFortPawn: (UFortWeaponItemDefinition*, FGuid), thiscall, ret 14h
constexpr int WeaponDefaultMetadata = 0x634, BuildingDefinitionMetadata = 0x6B8;
constexpr int DecoToolItemDefinition = 0x5F8;  // AFortDecoTool.ItemDefinition
// ABuildingSMActor, for `editcheck`: bIsPlayerBuildable (bit 3), EditModePatternData, EditModeSupport, CurBuildingAnimType,
// EditingPlayer, AttachmentPlacementBlockingActor, and a flag word whose bit 6 also blocks editing.
constexpr int BuildingFlags = 0x348, BuildingPlayerBuildable = 0x8, BuildingEditPattern = 0x6C0, BuildingEditSupport = 0x6EC;
constexpr int BuildingAnimType = 0x6FA, BuildingEditingPlayer = 0x7A8, BuildingPlacementBlocker = 0x808, BuildingBits = 0x89C;
constexpr int VtCanBeEdited = 0x47C;
constexpr uint32_t kGameData = 0x006F1640;  // UFortGameData* (), cdecl
constexpr int GameDataEditTool = 0x994;     // TSoftObjectPtr<UFortEditToolItemDefinition>, its path at +0xC
constexpr float kEditCheckRange = 1000.0f;
// OnAboutToEnterBackpack fires on the pickup's effect (B_Pickups_C, an AFortPickupEffect), not on the AFortPickup.
constexpr int EffectItemDefinition = 0x248, ActorOwner = 0x5C;
constexpr int PickupEntry = 0x250;  // AFortPickup.PrimaryPickupItemEntry
constexpr int PawnStasisMode = 0x870;  // AFortPlayerPawn: 0 None, 1 NoMovement, 2 NoMovementOrTurning
// Teams: AFortGameMode vtable getters (rebased already when read from the vtable).
constexpr int VtGetTeamInfo = 0x464, VtSetPlayerTeam = 0x474, VtCreateTeam = 0x478;  // (team) / (team, pc) / (team)
constexpr int VtTeamInfoAddToTeam = 0x2CC;     // AFortTeamInfo (pc), ret 4
constexpr int PlayerStatePlayerTeam = 0x3D4;   // AFortTeamInfo*
constexpr int PlayerStatePlayerId = 0x260, PlayerStateWorldPlayerId = 0x2FC;  // APlayerState.PlayerID, AFortPlayerState's
// The hero look lives on the player state; the pawn builds its body from CharacterParts. On the authority nothing picks
// an outfit (only the frontend's hero exhibit does), so the zone pawn stays invisible.
constexpr int PlayerStateHeroType = 0x30C, PlayerStateGender = 0x38C;  // gender byte, then body type
constexpr int PlayerStateParts = 0x390;  // CharacterParts[6]: Head, Body, Hat, Backpack, Charm, Face
constexpr int PlayerStateBodyPart = 0x394, PlayerStateCharmPart = 0x3A0;
constexpr int PlayerStatePartSwatches = 0x3BC;   // one per part
constexpr int PlayerStateCustomization = 0x3B0;  // UCustomPlayerComponent*
constexpr int PawnPlayerState = 0x254;
constexpr uint32_t kShowHero = 0x0088BA70;         // AFortPlayerState (pawn, heroType): references, outfit, body; ret 8
constexpr uint32_t kSetReferences = 0x006C1A20;    // UCustomPlayerComponent (gender*, bodyType*, parts*, swatches*, swatches*), ret 14h
constexpr uint32_t kPickBestOutfit = 0x006C6A70;   // UCustomPlayerComponent (heroType) -> bool, ret 4
constexpr uint32_t kRefreshBody = 0x00881B10;      // AFortPlayerPawn: master mesh, parts, swatches, reinit event
constexpr wchar_t kDefaultHero[] = L"/Game/Heroes/Class_Commando.Class_Commando";  // UFortGameData.DefaultHero
constexpr wchar_t kCommandoBody[] =
    L"/Game/Characters/CharacterParts/Male/Medium/Bodies/MPR_M_Med_Soldier_Skinned.MPR_M_Med_Soldier_Skinned";
constexpr wchar_t kCommandoCharm[] = L"/Game/Characters/CharacterParts/Charms/M_Med_Soldier_04_Holster.M_Med_Soldier_04_Holster";
// PlayerPawn_Generic_C's weapon-effect overlays (adrenaline and shield, per part). Its 'Setup FX Mesh Duplicates' slaves
// them to the body part, itself a slave of Character.Mesh, and slaves have no bones: GetBoneMatrix warns every frame.
constexpr int kEffectMeshes[] = { 0xCE8, 0xCEC, 0xCF0, 0xCFC, 0xD00, 0xD04 };
constexpr int SkinnedMeshSkeletalMesh = 0x4B4, SkinnedMeshMasterPose = 0x4B8;  // USkeletalMesh*, TWeakObjectPtr
constexpr uint32_t kWeakGet = 0x00DA4200;  // FWeakObjectPtr::Get(bool bEvenIfPendingKill), thiscall
constexpr int WorldOwningGameInstance = 0xB0, GameInstanceUiManager = 0x4C, UiManagerCurrentState = 0x320;

struct KitItem {
    const wchar_t* path;
    int count;
    int quickbar;  // 0 primary, 1 secondary (building), -1 inventory only
    int slot;
    bool material;  // left out when the profile's materials came from the backend
    bool starter;   // left out when the player brings a backpack from the last zone
};
const KitItem kKit[] = {
    { L"/Game/Weapons/WeaponItemData/Melee/Melee_Impact_Pickaxe_T01.Melee_Impact_Pickaxe_T01", 1, 0, 0 },
    { L"/Game/Weapons/WeaponItemData/Assault/Assault_Auto_T02.Assault_Auto_T02", 1, 0, 1, false, true },
    { L"/Game/Items/Weapons/BuildingTools/BuildingItemData_Wall.BuildingItemData_Wall", 1, 1, 0 },
    { L"/Game/Items/Weapons/BuildingTools/BuildingItemData_Floor.BuildingItemData_Floor", 1, 1, 1 },
    { L"/Game/Items/Weapons/BuildingTools/BuildingItemData_Stair_W.BuildingItemData_Stair_W", 1, 1, 2 },
    { L"/Game/Items/Weapons/BuildingTools/BuildingItemData_RoofS.BuildingItemData_RoofS", 1, 1, 3 },
    { L"/Game/Items/Weapons/BuildingTools/EditTool.EditTool", 1, -1, 0 },
    { L"/Game/Weapons/AmmoItemData/AmmoDataBulletsMedium.AmmoDataBulletsMedium", 500, -1, 0, false, true },
    { L"/Game/Loot/ResourcePickups/WoodItemData.WoodItemData", 500, -1, 0, true },
    { L"/Game/Loot/ResourcePickups/StoneItemData.StoneItemData", 500, -1, 0, true },
    { L"/Game/Loot/ResourcePickups/MetalItemData.MetalItemData", 500, -1, 0, true },
};
constexpr wchar_t kBluGlo[] = L"/Game/Missions/Secondary/BluGlo/BluGloWorldItemData.BluGloWorldItemData";
constexpr int kBluGloCount = 100;  // cheats: the Gate's BluGlo deposits without harvesting first

// Per controller: every player in the zone is set up (co-op: the host's and the joined players').
std::set<uintptr_t> g_setUp;            // the controllers whose kit went in (not again for a respawned pawn)
constexpr int kPrimarySlots = 4;  // the harvest tool, then three weapons; more go to the backpack

void Call(uintptr_t object, const char* path, void* params = nullptr) {
    uintptr_t function = engine::FindFunction(path);
    if (object && function) engine::ProcessEvent(object, function, params);
    else LOG("player: can't call %s on %s", path, engine::Describe(object).c_str());
}

// This copy's own player (its UPlayer is a ULocalPlayer); a co-op player who joined has their connection there.
bool IsLocal(uintptr_t pc) {
    return pc && engine::IsA(At<uint32_t>(pc, PcPlayer), "LocalPlayer");
}

void Refresh(uintptr_t pc) {
    uintptr_t inventory = At<uint32_t>(pc, PcWorldInventory), quickBars = At<uint32_t>(pc, PcQuickBars);
    if (!inventory || !quickBars) return;
    if (!IsLocal(pc)) return;  // the HUD updates below are the local player's; a joined player's inventory replicates
    Call(inventory, "/Script/FortniteGame.FortInventory.HandleInventoryLocalUpdate");
    Call(pc, "/Script/FortniteGame.FortPlayerController.HandleWorldInventoryLocalUpdate");
    Call(pc, "/Script/FortniteGame.FortPlayerController.OnRep_QuickBar");
    Call(quickBars, "/Script/FortniteGame.FortQuickBars.OnRep_PrimaryQuickBar");
    Call(quickBars, "/Script/FortniteGame.FortQuickBars.OnRep_SecondaryQuickBar");
}

void QueueChange(uintptr_t inventory, uintptr_t item, bool removed) {
    engine::Append(At<engine::TArray>(inventory, removed ? InventoryPendingRemoved : InventoryPendingChanged), &item, 4);
}

void MarkArrayDirty(uintptr_t inventory) {
    int32_t& key = At<int32_t>(inventory, ListArrayKey);
    if (++key == -1) ++key;
}

void MarkEntryDirty(uintptr_t inventory, uintptr_t entry) {
    int32_t& id = At<int32_t>(entry, EntryReplicationId);
    if (id == -1) {
        int32_t& counter = At<int32_t>(inventory, ListIdCounter);
        if (++counter == -1) ++counter;
        id = counter;
    }
    At<int32_t>(entry, EntryReplicationKey)++;
    MarkArrayDirty(inventory);
}

bool SameGuid(uintptr_t entry, const void* guid) {
    return memcmp(reinterpret_cast<const void*>(entry + EntryGuid), guid, 16) == 0;
}

// The replicated copy of an item's entry follows the item's count.
void SyncEntry(uintptr_t inventory, uintptr_t item) {
    engine::TArray& entries = At<engine::TArray>(inventory, InventoryEntries);
    for (int32_t i = 0; entries.data && i < entries.num; i++) {
        uintptr_t entry = entries.data + static_cast<uintptr_t>(i) * EntrySize;
        if (SameGuid(entry, reinterpret_cast<const void*>(item + ItemEntry + EntryGuid))) {
            At<int32_t>(entry, EntryCount) = At<int32_t>(item, ItemEntry + EntryCount);
            MarkEntryDirty(inventory, entry);
        }
    }
    QueueChange(inventory, item, false);
}

void RemoveAt(engine::TArray& array, int32_t index, int size) {
    uint8_t* data = reinterpret_cast<uint8_t*>(array.data);
    memmove(data + index * size, data + (index + 1) * size, static_cast<size_t>(array.num - index - 1) * size);
    array.num--;
}

// An item whose count reached zero leaves the inventory: its quickbar slot, its instance, its replicated entry (the
// copies made here own no heap memory), and the screen is told.
void RemoveInstance(uintptr_t pc, uintptr_t inventory, int32_t index) {
    engine::TArray& instances = At<engine::TArray>(inventory, InventoryInstances);
    uintptr_t item = At<uint32_t>(instances.data, index * 4);
    if (uintptr_t quickBars = At<uint32_t>(pc, PcQuickBars)) {
        struct {
            uint8_t guid[16];
            bool findReplacement, force;
            uint8_t pad[2];
        } p{};
        memcpy(p.guid, reinterpret_cast<const void*>(item + ItemEntry + EntryGuid), 16);
        p.force = true;
        Call(quickBars, "/Script/FortniteGame.FortQuickBars.ServerRemoveItemInternal", &p);
    }
    engine::TArray& entries = At<engine::TArray>(inventory, InventoryEntries);
    for (int32_t i = 0; entries.data && i < entries.num; i++) {
        if (SameGuid(entries.data + static_cast<uintptr_t>(i) * EntrySize, reinterpret_cast<const void*>(item + ItemEntry + EntryGuid))) {
            RemoveAt(entries, i, EntrySize);
            MarkArrayDirty(inventory);
            break;
        }
    }
    RemoveAt(instances, index, 4);
    QueueChange(inventory, item, true);
}

int PropertyOffset(const char* cls, const char* name);  // below

// Adds `count` of `definition`: onto an existing stack when it has no quickbar slot and stacks at all
// (FortItemDefinition.MaxStackSize above 1: ammo, materials, BluGlo; not weapons), else as a new item.
uintptr_t AddItem(uintptr_t pc, uintptr_t definition, int count, int quickbar, int slot) {
    static int maxStackSize = PropertyOffset("/Script/FortniteGame.FortItemDefinition", "MaxStackSize");
    uintptr_t inventory = At<uint32_t>(pc, PcWorldInventory), quickBars = At<uint32_t>(pc, PcQuickBars);
    if (!inventory || !engine::IsA(definition, "FortItemDefinition")) {
        LOG("player: not an item definition: %s", engine::Describe(definition).c_str());
        return 0;
    }
    engine::TArray& instances = At<engine::TArray>(inventory, InventoryInstances);
    bool stacks = !engine::IsA(definition, "FortWeaponItemDefinition") && (maxStackSize < 0 || At<int32_t>(definition, maxStackSize) > 1);
    if (quickbar < 0 && stacks) {
        for (int32_t i = 0; instances.data && i < instances.num; i++) {
            uintptr_t item = At<uint32_t>(instances.data, i * 4);
            if (item && At<uint32_t>(item, ItemEntry + EntryDefinition) == definition) {
                At<int32_t>(item, ItemEntry + EntryCount) += count;
                SyncEntry(inventory, item);
                return item;
            }
        }
    }
    uintptr_t item = 0;
    Call(definition, "/Script/FortniteGame.FortItemDefinition.CreateTemporaryItemInstanceBP", &item);
    if (!item) return 0;
    At<int32_t>(item, ItemEntry + EntryCount) = count;
    At<uint32_t>(item, ItemOwnerInventory) = static_cast<uint32_t>(inventory);
    uint8_t copy[EntrySize];
    memcpy(copy, reinterpret_cast<const void*>(item + ItemEntry), EntrySize);
    for (int member : kEntryHeapMembers) memset(copy + member, 0, 12);
    // The item must be in the inventory before the quickbar is told: ServerAddItemInternal looks it up there.
    engine::Append(instances, &item, 4);
    engine::TArray& entries = At<engine::TArray>(inventory, InventoryEntries);
    engine::Append(entries, copy, EntrySize);
    MarkEntryDirty(inventory, entries.data + static_cast<uintptr_t>(entries.num - 1) * EntrySize);
    QueueChange(inventory, item, false);
    if (quickbar >= 0 && quickBars) {
        struct {
            uint8_t guid[16];
            uint8_t quickbar, pad[3];
            int32_t slot;
        } p{};
        memcpy(p.guid, reinterpret_cast<const void*>(item + ItemEntry + EntryGuid), 16);
        p.quickbar = static_cast<uint8_t>(quickbar);
        p.slot = slot;
        Call(quickBars, "/Script/FortniteGame.FortQuickBars.ServerAddItemInternal", &p);
    }
    return item;
}

uintptr_t Equip(uintptr_t pawn, uintptr_t definition, const void* guid) {
    uint32_t g[4];
    memcpy(g, guid, 16);
    auto equip = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t, uintptr_t, uint32_t, uint32_t, uint32_t, uint32_t)>(
        engine::Va(kEquipWeaponDefinition));
    uintptr_t weapon = equip(pawn, definition, g[0], g[1], g[2], g[3]);
    if (weapon && engine::IsA(weapon, "FortWeap_BuildingTool")) {
        At<uint32_t>(weapon, WeaponDefaultMetadata) = At<uint32_t>(definition, BuildingDefinitionMetadata);
        Call(weapon, "/Script/FortniteGame.FortWeap_BuildingTool.OnRep_DefaultMetadata");
    }
    // The trap tool places what its ItemDefinition says, which the server set: without it the placement helper has no
    // attachment type ("Cannot find spot to place on!") and ServerSpawnDeco returns at once. The OnRep hands it over.
    if (weapon && engine::IsA(weapon, "FortDecoTool") && engine::IsA(definition, "FortDecoItemDefinition")) {
        At<uint32_t>(weapon, DecoToolItemDefinition) = static_cast<uint32_t>(definition);
        Call(weapon, "/Script/FortniteGame.FortDecoTool.OnRep_ItemDefinition");
    }
    return weapon;
}

uintptr_t Inventory(uintptr_t pc) {
    uintptr_t inventory = At<uint32_t>(pc, PcWorldInventory);
    if (!inventory) {
        inventory = engine::SpawnActor(engine::FindClass("/Script/FortniteGame.FortInventory"), pc);
        At<uint32_t>(pc, PcWorldInventory) = static_cast<uint32_t>(inventory);
        LOG("player: spawned the world inventory %s", engine::Describe(inventory).c_str());
    }
    if (inventory) {
        uintptr_t owner = pc;
        Call(inventory, "/Script/Engine.Actor.SetOwner", &owner);
    }
    return inventory;
}

uintptr_t QuickBars(uintptr_t pc) {
    uintptr_t quickBars = At<uint32_t>(pc, PcQuickBars);
    if (!quickBars) {
        // Owned by the controller: ServerAddItemInternal checks that its owner is a FortPlayerController.
        quickBars = engine::SpawnActor(engine::FindClass("/Script/FortniteGame.FortQuickBars"), pc);
        At<uint32_t>(pc, PcQuickBars) = static_cast<uint32_t>(quickBars);
        LOG("player: spawned the quickbars %s", engine::Describe(quickBars).c_str());
    }
    return quickBars;
}

void SetIfUnset(uintptr_t object, int offset, float value) {
    if (At<float>(object, offset) <= 0.0f) At<float>(object, offset) = value;
}

// An attribute's base value, through the pawn's ability system (see kApplyModToAttribute).
bool SetBase(uintptr_t pawn, const char* setClass, const char* attribute, float value) {
    uintptr_t abilitySystem = At<uint32_t>(pawn, PawnAbilitySystem);
    uintptr_t property = engine::FindPropertyOn(engine::FindClass(setClass), attribute);
    if (!abilitySystem || !property) {
        LOG("player: can't set the base of %s.%s", setClass, attribute);
        return false;
    }
    uintptr_t gameplayAttribute = property;
    reinterpret_cast<void(__thiscall*)(uintptr_t, const uintptr_t*, uint32_t, float, const void*)>(engine::Va(kApplyModToAttribute))(
        abilitySystem + AbilitySystemActiveEffects, &gameplayAttribute, kModOverride, value, nullptr);
    return true;
}

// Health, speeds and stamina come from the server's pawn setup; without it they can be zero (Alpharium's values).
void Stats(uintptr_t pc, uintptr_t pawn) {
    uintptr_t health = At<uint32_t>(pawn, PawnHealthSet), movement = At<uint32_t>(pawn, PawnMovementSet);
    if (health) {
        LOG("player: health %.0f / %.0f", At<float>(health, HealthSetHealth), At<float>(health, HealthSetMaxHealth));
        if (At<float>(health, HealthSetMaxHealth) <= 0.0f || At<float>(health, HealthSetHealth) <= 0.0f) {
            SetIfUnset(health, HealthSetMaxHealth, 100.0f);
            float full = At<float>(health, HealthSetMaxHealth);
            Call(pawn, "/Script/FortniteGame.FortPawn.SetHealth", &full);
        }
    }
    if (movement) {
        LOG("player: speeds walk %.0f, run %.0f, sprint %.0f", At<float>(movement, MovementWalk), At<float>(movement, MovementRun),
            At<float>(movement, MovementSprint));
        if (At<float>(movement, MovementWalk) <= 0.0f || At<float>(movement, MovementRun) <= 0.0f ||
            At<float>(movement, MovementSprint) <= 0.0f) {
            const char* set = "/Script/FortniteGame.FortMovementSet";
            SetBase(pawn, set, "WalkSpeed", 200.0f);
            SetBase(pawn, set, "RunSpeed", 410.0f);
            SetBase(pawn, set, "SprintSpeed", 550.0f);
            SetBase(pawn, set, "BackwardSpeedMultiplier", 0.65f);
            Call(movement, "/Script/FortniteGame.FortMovementSet.OnRep_SpeedMultiplier");
            LOG("player: speed bases set; now walk %.0f, run %.0f, sprint %.0f", At<float>(movement, MovementWalk),
                At<float>(movement, MovementRun), At<float>(movement, MovementSprint));
        }
    }
    uintptr_t stamina = engine::FindSubobject(pawn, "FortPlayerAttrSet");
    if (!stamina) stamina = engine::FindSubobject(At<uint32_t>(pc, PcPlayerState), "FortPlayerAttrSet");
    if (stamina) {
        LOG("player: stamina %.0f / %.0f", At<float>(stamina, AttrStamina), At<float>(stamina, AttrMaxStamina));
        if (At<float>(stamina, AttrMaxStamina) < 50.0f) {  // 0, or a perk's +5 on a base of 0
            const char* set = "/Script/FortniteGame.FortPlayerAttrSet";
            SetBase(pawn, set, "MaxStamina", 100.0f);
            SetBase(pawn, set, "Stamina", 100.0f);
            SetBase(pawn, set, "StaminaRegenRate", 0.65f);
            SetBase(pawn, set, "StaminaRegenDelay", 0.5f);
            LOG("player: stamina bases set; now %.0f / %.0f", At<float>(stamina, AttrStamina), At<float>(stamina, AttrMaxStamina));
            Call(stamina, "/Script/FortniteGame.FortPlayerAttrSet.OnRep_Stamina");
            Call(stamina, "/Script/FortniteGame.FortPlayerAttrSet.OnRep_MaxStamina");
            Call(stamina, "/Script/FortniteGame.FortPlayerAttrSet.OnRep_StaminaRegenDelay");
            Call(stamina, "/Script/FortniteGame.FortPlayerAttrSet.OnRep_StaminaRegenRate");
        }
    }
}

// Abilities are granted by an effect that grants an ability: Alpharium's way, borrowing GE_GrantReflectMeleeDmg's
// GrantedAbilities[0] for each ability in turn. Returns how many were granted.
int GrantAbilityClasses(uintptr_t pawn, const uintptr_t* abilities, size_t count) {
    uintptr_t abilitySystem = At<uint32_t>(pawn, PawnAbilitySystem);
    if (!abilitySystem) return 0;
    uintptr_t effect = engine::Load(
        L"/Game/Abilities/Player/Constructor/Traits/CreativeEngineering/GE_GrantReflectMeleeDmg.GE_GrantReflectMeleeDmg_C");
    uintptr_t defaults = effect ? At<uint32_t>(effect, ClassDefaultObject) : 0;
    if (!defaults || At<int32_t>(defaults, EffectGrantedAbilities + 4) < 1) {
        LOG("player: no ability-granting effect (%s)", engine::Describe(effect).c_str());
        return 0;
    }
    uintptr_t granted = At<uint32_t>(defaults, EffectGrantedAbilities);
    uint32_t original = At<uint32_t>(granted, 0);
    int done = 0;
    for (size_t i = 0; i < count; i++) {
        if (!abilities[i]) continue;
        At<uint32_t>(granted, 0) = static_cast<uint32_t>(abilities[i]);
        struct {
            uintptr_t effectClass;
            float level;
            uint8_t context[0xC];
            int32_t handle;
        } p{ effect, 1.0f };
        Call(abilitySystem, "/Script/GameplayAbilities.AbilitySystemComponent.BP_ApplyGameplayEffectToSelf", &p);
        done++;
    }
    At<uint32_t>(granted, 0) = original;
    return done;
}

// Jump, sprint, interact and consumables. Only for a pawn that has none yet, unless forced.
void Abilities(uintptr_t pawn, bool force) {
    uintptr_t abilitySystem = At<uint32_t>(pawn, PawnAbilitySystem);
    if (!abilitySystem) return;
    int32_t have = At<int32_t>(abilitySystem, AbilitySystemAbilityCount);
    if (have > 0 && !force) {
        LOG("player: the pawn has %d abilities already", have);
        return;
    }
    uintptr_t abilities[] = {
        engine::FindClass("/Script/FortniteGame.FortGameplayAbility_Jump"),
        engine::FindClass("/Script/FortniteGame.FortGameplayAbility_Sprint"),
        engine::Load(L"/Game/Abilities/Player/Generic/Traits/DefaultPlayer/GA_DefaultPlayer_InteractSearch.GA_DefaultPlayer_InteractSearch_C"),
        engine::Load(L"/Game/Abilities/Player/Generic/Traits/DefaultPlayer/GA_DefaultPlayer_InteractUse.GA_DefaultPlayer_InteractUse_C"),
        engine::Load(L"/Game/Abilities/Player/Generic/Traits/DefaultPlayer/GA_DefaultPlayer_Consumable.GA_DefaultPlayer_Consumable_C"),
    };
    int count = GrantAbilityClasses(pawn, abilities, sizeof(abilities) / sizeof(abilities[0]));
    LOG("player: granted %d abilities (the pawn had %d)", count, have);
}

void EquipFirstWeapon(uintptr_t pc, uintptr_t pawn) {
    uintptr_t inventory = At<uint32_t>(pc, PcWorldInventory);
    engine::TArray& instances = At<engine::TArray>(inventory, InventoryInstances);
    for (int32_t i = 0; instances.data && i < instances.num; i++) {
        uintptr_t item = At<uint32_t>(instances.data, i * 4);
        uintptr_t definition = item ? At<uint32_t>(item, ItemEntry + EntryDefinition) : 0;
        if (definition && engine::Name(definition).find("Pickaxe") != std::string::npos) {
            LOG("player: equipped %s", engine::Describe(Equip(pawn, definition, reinterpret_cast<const void*>(item + ItemEntry + EntryGuid))).c_str());
            return;
        }
    }
}

// The offset of a UPROPERTY (UProperty::Offset_Internal, +0x3C), looked up once by name.
int PropertyOffset(const char* cls, const char* name) {
    uintptr_t property = engine::FindPropertyOn(engine::FindClass(cls), name);
    return property ? At<int32_t>(property, 0x3C) : -1;
}
}  // namespace

// After a drop: if the pawn still holds the dropped item's weapon (that item, by the GUID the weapon was equipped
// with, now gone; without the GUID, a weapon of that kind with none left), it switches to the pickaxe, as the server's
// drop did (else the dropped weapon stays in the hands while its slot is empty).
void AfterDrop(uintptr_t pc, uintptr_t definition, const void* guid) {
    static int currentWeapon = PropertyOffset("/Script/FortniteGame.FortPawn", "CurrentWeapon");
    static int weaponData = PropertyOffset("/Script/FortniteGame.FortWeapon", "WeaponData");
    static int weaponGuid = PropertyOffset("/Script/FortniteGame.FortWeapon", "ItemEntryGuid");
    uintptr_t pawn = engine::PawnOf(pc), inventory = At<uint32_t>(pc, PcWorldInventory);
    if (!pawn || !inventory || currentWeapon < 0 || weaponData < 0) return;
    uintptr_t weapon = At<uint32_t>(pawn, currentWeapon);
    if (!weapon || At<uint32_t>(weapon, weaponData) != definition) return;
    bool gone;
    if (weaponGuid >= 0) {
        if (memcmp(reinterpret_cast<const void*>(weapon + weaponGuid), guid, 16) != 0) return;  // another of that kind
        gone = true;
        engine::TArray& instances = At<engine::TArray>(inventory, InventoryInstances);
        for (int32_t i = 0; instances.data && i < instances.num && gone; i++) {
            uintptr_t item = At<uint32_t>(instances.data, i * 4);
            if (item && SameGuid(item + ItemEntry, guid)) gone = false;  // only some of it was dropped
        }
    } else {
        gone = CountItems(pc, definition) == 0;
    }
    if (gone) EquipFirstWeapon(pc, pawn);
}

// `items`: this copy's own player's inventory as this copy sees it (a joined player's is the host's, replicated), the
// held weapon's clip and the cheat flags.
void ListItems() {
    static int currentWeapon = PropertyOffset("/Script/FortniteGame.FortPawn", "CurrentWeapon");
    static int ammoCount = PropertyOffset("/Script/FortniteGame.FortWeapon", "AmmoCount");
    uintptr_t pc = engine::LocalPlayerController(), pawn = engine::PawnOf(pc);
    if (!pc || !At<uint32_t>(pc, PcWorldInventory)) {
        LOG("player: no inventory here");
        return;
    }
    std::vector<HeldItem> held = Items(pc);
    for (const HeldItem& item : held) LOG("  %s x%d%s", engine::Name(item.definition).c_str(), item.count,
                                          item.slot >= 0 ? (", slot " + std::to_string(item.slot + 1)).c_str() : "");
    uintptr_t weapon = pawn && currentWeapon >= 0 ? At<uint32_t>(pawn, currentWeapon) : 0;
    LOG("player: %zu item(s); holding %s%s; infinite ammo %d, free building %d", held.size(), engine::Name(weapon).c_str(),
        weapon && ammoCount >= 0 ? (" (clip " + std::to_string(At<int32_t>(weapon, ammoCount)) + ")").c_str() : "",
        At<uint8_t>(pc, PcInfiniteAmmoBits) & 1, (At<uint8_t>(pc, PcBuildFreeBits) >> 1) & 1);
}

// A joined player's own copy: the cheats the host gives every player (infinite ammo, free building, infinite stamina)
// are flags on the host's copy of their controller, but the client predicts its own shots, builds and sprint: its clip
// ran dry and the gun stopped (the ammo shown went to 0). The same flags on this copy's own controller, every tick
// while joined (a respawn or a new zone brings a new one).
void ClientCheats(uintptr_t pc) {
    if (!game::g_on.cheats || !engine::IsLive(pc) || !engine::IsA(pc, "FortPlayerController")) return;
    bool was = At<uint8_t>(pc, PcInfiniteAmmoBits) & 1;
    At<uint8_t>(pc, PcBuildFreeBits) |= 2;
    At<uint8_t>(pc, PcInfiniteAmmoBits) |= 1;
    if (uintptr_t cheats = At<uint32_t>(pc, PcCheatManager)) At<uint32_t>(cheats, CheatManagerFlags) |= CheatInfiniteStamina;
    if (!was) LOG("player: infinite ammo, free building and infinite stamina on this joined player's own controller too");
}

void Reset() {
    g_setUp.clear();
}

bool Setup(uintptr_t pc, uintptr_t pawn) {
    if (!pc || !pawn || !engine::IsA(pc, "FortPlayerController")) {
        LOG("player: no player to set up (%s, %s)", engine::Describe(pc).c_str(), engine::Describe(pawn).c_str());
        return false;
    }
    LOG("player: setting up %s with %s", engine::Describe(pc).c_str(), engine::Describe(pawn).c_str());
    uintptr_t inventory = Inventory(pc), quickBars = QuickBars(pc);
    if (!inventory || !quickBars) {
        LOG("player: no inventory or quickbars; stopping");
        return false;
    }
    At<int32_t>(pc, PcBackpackSize) = 999;
    bool firstSetup = !g_setUp.count(pc);
    loadout::Prepare(pc);
    if (firstSetup) {  // the kit once per controller, not again for a respawned pawn
        g_setUp.insert(pc);
        bool tutorial = game::Tutorial();  // starts from nothing: the harvest tool, building pieces, the edit tool
        bool materials = !tutorial && loadout::GiveMaterials(pc);
        bool backpack = !tutorial && loadout::GiveBackpack(pc);
        for (const KitItem& kit : kKit) {
            if ((kit.material && materials) || (kit.starter && backpack)) continue;
            if (tutorial && (kit.material || kit.starter)) continue;
            uintptr_t definition = engine::Load(kit.path);
            // The first primary slot is the pickaxe: the lobby hero's harvest weapon when the backend told us.
            if (kit.quickbar == 0 && kit.slot == 0) {
                if (uintptr_t pickaxe = loadout::Pickaxe(pc)) definition = pickaxe;
            }
            if (definition) AddItem(pc, definition, kit.count, kit.quickbar, kit.slot);
        }
        if (game::g_on.cheats && !tutorial) {
            if (uintptr_t bluGlo = engine::Load(kBluGlo)) AddItem(pc, bluGlo, kBluGloCount, -1, 0);
            At<uint8_t>(pc, PcBuildFreeBits) |= 2;
            At<uint8_t>(pc, PcInfiniteAmmoBits) |= 1;
            if (uintptr_t cheats = At<uint32_t>(pc, PcCheatManager)) At<uint32_t>(cheats, CheatManagerFlags) |= CheatInfiniteStamina;
        }
        engine::TArray& instances = At<engine::TArray>(inventory, InventoryInstances);
        LOG("player: the inventory has %d items%s", instances.num,
            tutorial ? " (the tutorial: from nothing)"
                     : game::g_on.cheats ? " (with 100 BluGlo, free building, infinite ammo and stamina)" : "");
    }
    // bHasInitializedWorldInventory stays false on purpose: the world manager would start saving profiles to MCP.
    Refresh(pc);
    EnsureTeam(pc);
    ApplyHero(pc, pawn);
    EquipFirstWeapon(pc, pawn);
    Abilities(pawn, true);  // the 2 abilities a zone pawn starts with are none of these
    // Base stats before the hero's passive effects: an effect's aggregator keeps the value it finds as the base, so
    // stamina 0 plus a perk's +5 would stay at 5 (no sprint).
    Stats(pc, pawn);
    loadout::Grant(pc, pawn, firstSetup);
    loadout::ApplyStats(pc, pawn);
    // The Zone controller's OnRep marks the player state loaded (the loading screen waits for it) and switches to
    // game-only input.
    if (At<uint8_t>(pc, PcClientLoaded) && At<uint8_t>(pc, PcServerLoaded))
        Call(pc, "/Script/FortniteGame.FortPlayerController.OnRep_bHasServerFinishedLoading");
    else
        LOG("player: loading flags client %d, server %d", At<uint8_t>(pc, PcClientLoaded), At<uint8_t>(pc, PcServerLoaded));
    return true;
}

void OnPickup(uintptr_t effect) {
    uintptr_t pc = engine::LocalPlayerController();
    if (!game::g_on.player || !pc || !At<uint32_t>(pc, PcWorldInventory) || !engine::IsA(effect, "FortPickupEffect")) return;
    uintptr_t definition = At<uint32_t>(effect, EffectItemDefinition);
    uintptr_t pickup = At<uint32_t>(effect, ActorOwner);
    if (!engine::IsA(definition, "FortItemDefinition")) {
        LOG("player: pickup effect %s without an item definition", engine::Describe(effect).c_str());
        return;
    }
    int count = engine::IsA(pickup, "FortPickup") ? At<int32_t>(pickup, PickupEntry + EntryCount) : 0;
    if (count <= 0) count = engine::IsA(definition, "FortResourceItemDefinition") ? 10 : 1;
    Receive(pc, definition, count);
    LOG("player: picked up %d x %s", count, engine::Name(definition).c_str());
}

// A received weapon goes to the first empty weapon slot of the primary quickbar (slot 0 is the harvest tool; one freed
// by a drop is used again), else to the backpack.
uintptr_t Receive(uintptr_t pc, uintptr_t definition, int count) {
    bool weapon = engine::IsA(definition, "FortWeaponItemDefinition");
    int slot = -1;
    if (weapon) {
        bool used[kPrimarySlots] = {};
        for (const HeldItem& held : Items(pc))
            if (held.slot >= 0 && held.slot < kPrimarySlots) used[held.slot] = true;
        for (int s = 1; s < kPrimarySlots && slot < 0; s++)
            if (!used[s]) slot = s;
    }
    uintptr_t item = AddItem(pc, definition, count, slot >= 0 ? 0 : -1, slot);
    Refresh(pc);
    return item;
}

// `equip <name part>`: the local player's first item whose definition's name has the part, through
// ServerExecuteInventoryItem as the quickbar keys send it (a building piece puts the player in build mode).
// `drop <name part>`: all of it, through ServerSpawnInventoryDrop as the inventory screen sends it.
bool ExecuteByName(uintptr_t pc, const std::string& part, bool drop) {
    uintptr_t inventory = pc ? At<uint32_t>(pc, PcWorldInventory) : 0;
    if (!inventory) return false;
    engine::TArray& instances = At<engine::TArray>(inventory, InventoryInstances);
    for (int32_t i = 0; instances.data && i < instances.num; i++) {
        uintptr_t item = At<uint32_t>(instances.data, i * 4);
        uintptr_t definition = item ? At<uint32_t>(item, ItemEntry + EntryDefinition) : 0;
        if (!definition || engine::Name(definition).find(part) == std::string::npos) continue;
        struct { uint8_t guid[16]; int32_t count; } params = {};
        memcpy(params.guid, reinterpret_cast<const void*>(item + ItemEntry + EntryGuid), 16);
        params.count = At<int32_t>(item, ItemEntry + EntryCount);
        Call(pc, drop ? "/Script/FortniteGame.FortPlayerController.ServerSpawnInventoryDrop"
                      : "/Script/FortniteGame.FortPlayerController.ServerExecuteInventoryItem", &params);
        // From here the event hook doesn't see the call (it ignores the DLL's own), so the host equips it itself; a
        // joined client's call goes to the host, which does.
        if (!drop && !net::IsClient()) OnExecuteItem(pc, params.guid);
        LOG("player: %s %s", drop ? "dropped" : "executed", engine::Name(definition).c_str());
        return true;
    }
    LOG("player: no item named like %s (%d items)", part.c_str(), instances.num);
    return false;
}

std::vector<HeldItem> Items(uintptr_t pc) {
    std::vector<HeldItem> held;
    uintptr_t inventory = At<uint32_t>(pc, PcWorldInventory), quickBars = At<uint32_t>(pc, PcQuickBars);
    if (!inventory) return held;
    engine::TArray& instances = At<engine::TArray>(inventory, InventoryInstances);
    for (int32_t i = 0; instances.data && i < instances.num; i++) {
        uintptr_t item = At<uint32_t>(instances.data, i * 4);
        if (!item) continue;
        HeldItem h{ At<uint32_t>(item, ItemEntry + EntryDefinition), At<int32_t>(item, ItemEntry + EntryCount), -1 };
        // The primary quickbar's slots hold item GUIDs (FQuickBarSlot: TArray<FGuid>).
        engine::TArray& slots = At<engine::TArray>(quickBars, QuickBarsPrimary + QuickBarSlots);
        for (int32_t s = 0; quickBars && slots.data && s < slots.num && s < 16 && h.slot < 0; s++) {
            engine::TArray& guids = At<engine::TArray>(slots.data + static_cast<uintptr_t>(s) * QuickBarSlotSize, 0);
            for (int32_t g = 0; guids.data && g < guids.num; g++)
                if (memcmp(reinterpret_cast<const void*>(guids.data + static_cast<uintptr_t>(g) * 16),
                           reinterpret_cast<const void*>(item + ItemEntry + EntryGuid), 16) == 0)
                    h.slot = s;
        }
        held.push_back(h);
    }
    return held;
}

int CountItems(uintptr_t pc, uintptr_t definition) {
    uintptr_t inventory = At<uint32_t>(pc, PcWorldInventory);
    if (!inventory) return 0;
    engine::TArray& instances = At<engine::TArray>(inventory, InventoryInstances);
    int count = 0;
    for (int32_t i = 0; instances.data && i < instances.num; i++) {
        uintptr_t item = At<uint32_t>(instances.data, i * 4);
        if (item && At<uint32_t>(item, ItemEntry + EntryDefinition) == definition) count += At<int32_t>(item, ItemEntry + EntryCount);
    }
    return count;
}

// The pawn spawns in stasis (no movement, "WAITING TO START..."), and the server code that lets players go when the
// zone starts is compiled out. HandleInStasis is the OnRep: it gives the controller its move (and look) input back.
bool ReleaseStasis(uintptr_t pawn) {
    if (!At<uint8_t>(pawn, PawnStasisMode)) return false;
    At<uint8_t>(pawn, PawnStasisMode) = 0;
    Call(pawn, "/Script/FortniteGame.FortPlayerPawn.HandleInStasis");
    LOG("player: released %s from stasis", engine::Describe(pawn).c_str());
    return true;
}

void OnExecuteItem(uintptr_t pc, const void* guid) {
    uintptr_t inventory = At<uint32_t>(pc, PcWorldInventory), pawn = engine::PawnOf(pc);
    if (!game::g_on.player || !inventory || !pawn) return;
    engine::TArray& instances = At<engine::TArray>(inventory, InventoryInstances);
    for (int32_t i = 0; instances.data && i < instances.num; i++) {
        uintptr_t item = At<uint32_t>(instances.data, i * 4);
        if (item && memcmp(reinterpret_cast<const void*>(item + ItemEntry + EntryGuid), guid, 16) == 0) {
            uintptr_t definition = At<uint32_t>(item, ItemEntry + EntryDefinition);
            if (engine::IsA(definition, "FortWeaponItemDefinition")) Equip(pawn, definition, guid);
            return;
        }
    }
}

void OnHudEnterState() {
    uintptr_t pc = engine::LocalPlayerController();
    if (game::g_on.player && pc && g_setUp.count(pc)) Refresh(pc);
}

void Give(const std::wstring& path, int count, bool everyone) {
    std::vector<uintptr_t> players = { engine::LocalPlayerController() };
    if (everyone) for (uintptr_t joined : net::JoinedControllers()) players.push_back(joined);
    uintptr_t definition = engine::Load(path);
    for (uintptr_t pc : players) {
        if (!pc || !definition || !At<uint32_t>(pc, PcWorldInventory)) {
            LOG("player: can't give %ls to %s (set the player up first)", path.c_str(), engine::Name(pc).c_str());
            continue;
        }
        Receive(pc, definition, count);
        LOG("player: gave %d x %s to %s", count, engine::Name(definition).c_str(), engine::Name(pc).c_str());
    }
}

void GrantAbilities() {
    uintptr_t pawn = engine::PawnOf(engine::LocalPlayerController());
    if (pawn) Abilities(pawn, true);
}

bool ForceInGameUI() {
    uintptr_t pc = engine::LocalPlayerController();
    uintptr_t action = engine::FindObject("/Script/FortniteUI.Default__FortAsyncAction_SetUIState", "FortAsyncAction_SetUIState");
    if (!pc || UiState() < 0 || !action) {
        LOG("player: can't force the in-game UI (no player, UI manager or SetUIState)");
        return false;
    }
    struct {
        uintptr_t context;
        uint8_t state, pad[3];
        uintptr_t result;
    } p{ pc, 6 /*EFortUIState::InGame*/ };
    Call(action, "/Script/FortniteUI.FortAsyncAction_SetUIState.SetUIState", &p);
    if (p.result) Call(p.result, "/Script/Engine.BlueprintAsyncActionBase.Activate");
    // A plain FortPlayerController never switches to game-only input by itself.
    uintptr_t widgets = engine::FindObject("/Script/UMG.Default__WidgetBlueprintLibrary", "WidgetBlueprintLibrary");
    uintptr_t target = pc;
    Call(widgets, "/Script/UMG.WidgetBlueprintLibrary.SetInputMode_GameOnly", &target);
    LOG("player: forced the in-game UI (state now %d)", UiState());
    return true;
}

// Login put the player on Spectator if no team existed yet (the zone hook creates HumanCampaign first); mission props
// refuse interaction from another team. Success is the player state's team pointer.
void EnsureTeam(uintptr_t pc) {
    uintptr_t ps = pc ? At<uint32_t>(pc, PcPlayerState) : 0;
    uintptr_t gm = engine::GameMode();
    // What the player places (walls, traps) takes the team of the player state whose WorldPlayerId it is given
    // (ABuildingActor vtable +0x360, 00637510). The server's world id is never assigned here, and with -1 a trap keeps
    // Team Spectator and its damage takes the player for an enemy. The session's PlayerID stands in for it.
    if (engine::IsA(ps, "FortPlayerState") && At<int32_t>(ps, PlayerStateWorldPlayerId) == -1) {
        At<int32_t>(ps, PlayerStateWorldPlayerId) = At<int32_t>(ps, PlayerStatePlayerId);
        LOG("player: world player id %d", At<int32_t>(ps, PlayerStateWorldPlayerId));
    }
    if (!engine::IsA(ps, "FortPlayerState") || !engine::IsA(gm, "FortGameMode") || At<uint32_t>(ps, PlayerStatePlayerTeam)) return;
    uintptr_t vtable = At<uint32_t>(gm, 0);
    auto getTeamInfo = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t, uint32_t)>(At<uint32_t>(vtable, VtGetTeamInfo));
    auto createTeam = reinterpret_cast<void(__thiscall*)(uintptr_t, uint32_t)>(At<uint32_t>(vtable, VtCreateTeam));
    auto setPlayerTeam = reinterpret_cast<bool(__thiscall*)(uintptr_t, uint32_t, uintptr_t)>(At<uint32_t>(vtable, VtSetPlayerTeam));
    if (!getTeamInfo(gm, 0)) createTeam(gm, 0);
    setPlayerTeam(gm, 0, pc);
    uintptr_t info = getTeamInfo(gm, 0);
    if (!At<uint32_t>(ps, PlayerStatePlayerTeam) && info)
        reinterpret_cast<bool(__thiscall*)(uintptr_t, uintptr_t)>(At<uint32_t>(At<uint32_t>(info, 0), VtTeamInfoAddToTeam))(info, pc);
    if (!engine::IsA(ps, "FortPlayerStateZone")) LOG("player: the player state is not a Zone one; the pawn's team stays Spectator");
    LOG("player: put the player on the team %s", engine::Describe(At<uint32_t>(ps, PlayerStatePlayerTeam)).c_str());
}

void ApplyHero(uintptr_t pc, uintptr_t pawn) {
    uintptr_t ps = pc ? At<uint32_t>(pc, PcPlayerState) : 0;
    if (!engine::IsA(ps, "FortPlayerState") || !engine::IsA(pawn, "FortPlayerPawn") || At<uint32_t>(pawn, PawnPlayerState) != ps)
        return;
    // RestartPlayer already resolved the hero (PS HeroId, the MCP defaultHeroId, else GameData.DefaultHero); the outfit
    // code reads the hero type without a null check, so anything that is not a FortHeroType falls back to the Commando.
    uintptr_t heroType = At<uint32_t>(ps, PlayerStateHeroType);
    if (!engine::IsA(heroType, "FortHeroType")) {
        heroType = engine::Load(kDefaultHero);
        if (!engine::IsA(heroType, "FortHeroType")) {
            LOG("player: no hero type to dress the pawn with");
            return;
        }
        At<uint32_t>(ps, PlayerStateHeroType) = static_cast<uint32_t>(heroType);
    }
    uint8_t& gender = At<uint8_t>(ps, PlayerStateGender);
    if (gender != 1 && gender != 2) gender = 1;
    // The frontend's own hero exhibit: SetReferences, PickBestOutfitForHero, then the pawn's body refresh. It uses the
    // customization component unchecked, so only with one.
    uintptr_t customization = At<uint32_t>(ps, PlayerStateCustomization);
    if (customization && engine::IsLive(customization)) {
        reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t, uintptr_t)>(engine::Va(kShowHero))(ps, pawn, heroType);
        if (!At<uint32_t>(ps, PlayerStateBodyPart)) {  // the exhibit is gated on a local controller: the same by hand
            reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)>(engine::Va(kSetReferences))(
                customization, ps + PlayerStateGender, ps + PlayerStateGender + 1, ps + PlayerStateParts, ps + 0x3A8,
                ps + PlayerStatePartSwatches);
            reinterpret_cast<bool(__thiscall*)(uintptr_t, uintptr_t)>(engine::Va(kPickBestOutfit))(customization, heroType);
            reinterpret_cast<void(__thiscall*)(uintptr_t)>(engine::Va(kRefreshBody))(pawn);  // whatever PickBest returned
        }
    }
    if (!At<uint32_t>(ps, PlayerStateBodyPart)) {
        // No outfit came out: the Commando's own parts, written directly (the refresh tolerates no component).
        for (int i = 0; i < 6; i++) {
            At<uint32_t>(ps, PlayerStateParts + i * 4) = 0;
            At<uint32_t>(ps, PlayerStatePartSwatches + i * 4) = 0;
        }
        At<uint32_t>(ps, PlayerStateBodyPart) = static_cast<uint32_t>(engine::Load(kCommandoBody));
        At<uint32_t>(ps, PlayerStateCharmPart) = static_cast<uint32_t>(engine::Load(kCommandoCharm));
        gender = 1;
        At<uint8_t>(ps, PlayerStateGender + 1) = 2;  // body type Medium
        reinterpret_cast<void(__thiscall*)(uintptr_t)>(engine::Va(kRefreshBody))(pawn);
    }
    LOG("player: hero %s, gender %d, body %s", engine::Describe(heroType).c_str(), gender,
        engine::Describe(At<uint32_t>(ps, PlayerStateBodyPart)).c_str());
    FixEffectMeshes(pawn);
}

// Unbind must run while the overlay's master is still the body part: it indexes the master's clothing actors with the
// overlay's own index, unchecked.
void FixEffectMeshes(uintptr_t pawn) {
    if (!engine::IsLive(pawn) || !engine::IsA(pawn, "PlayerPawn_Generic_C")) return;  // the hero pawns are its children
    auto weakGet = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t, bool)>(engine::Va(kWeakGet));
    for (int offset : kEffectMeshes) {
        uintptr_t mesh = At<uint32_t>(pawn, offset);
        if (!mesh || !At<uint32_t>(mesh, SkinnedMeshSkeletalMesh)) continue;
        uintptr_t master = weakGet(mesh + SkinnedMeshMasterPose, false);
        uintptr_t root = master ? weakGet(master + SkinnedMeshMasterPose, false) : 0;
        if (!root) continue;  // already mastered by a component with bones
        uint8_t restore[4] = { 1 };
        Call(mesh, "/Script/Engine.SkeletalMeshComponent.UnbindClothFromMasterPoseComponent", restore);
        uintptr_t newMaster = root;
        Call(mesh, "/Script/Engine.SkinnedMeshComponent.SetMasterPoseComponent", &newMaster);
    }
}

int UiState() {
    uintptr_t world = engine::World();
    uintptr_t gameInstance = world ? At<uint32_t>(world, WorldOwningGameInstance) : 0;
    uintptr_t manager = gameInstance ? At<uint32_t>(gameInstance, GameInstanceUiManager) : 0;
    return manager ? At<uint8_t>(manager, UiManagerCurrentState) : -1;
}

uintptr_t GiveItem(uintptr_t pc, uintptr_t definition, int count, int quickbar, int slot, bool refresh) {
    uintptr_t item = AddItem(pc, definition, count, quickbar, slot);
    if (item && refresh) Refresh(pc);
    return item;
}

bool GrantAbility(uintptr_t pawn, uintptr_t abilityClass) {
    return GrantAbilityClasses(pawn, &abilityClass, 1) == 1;
}

bool SetAttribute(uintptr_t pawn, const char* setClass, const char* attribute, float value) {
    return SetBase(pawn, setClass, attribute, value);
}

std::string StatLine(uintptr_t pc, uintptr_t pawn) {
    uintptr_t health = At<uint32_t>(pawn, PawnHealthSet);
    uintptr_t attributes = engine::FindSubobject(pawn, "FortPlayerAttrSet");
    if (!attributes) attributes = engine::FindSubobject(At<uint32_t>(pc, PcPlayerState), "FortPlayerAttrSet");
    char line[200];
    snprintf(line, sizeof(line), "health %.0f/%.0f, shield %.0f/%.0f, fortitude %.0f, offense %.0f, tech %.0f, resistance %.0f",
             health ? At<float>(health, HealthSetHealth) : -1.0f, health ? At<float>(health, HealthSetMaxHealth) : -1.0f,
             health ? At<float>(health, HealthSetCurrentShield) : -1.0f, health ? At<float>(health, HealthSetShield) : -1.0f,
             attributes ? At<float>(attributes, AttrVitality) : -1.0f, attributes ? At<float>(attributes, AttrOffense) : -1.0f,
             attributes ? At<float>(attributes, AttrProwess) : -1.0f, attributes ? At<float>(attributes, AttrDefense) : -1.0f);
    return line;
}

// `editcheck`: why the building pieces around the player can't be edited. The client asks the server to begin editing
// only when ABuildingSMActor::CanBeEdited (vtable +0x47C) agrees: an EditModeSupport made from the blueprint's
// EditModePatternData, bIsPlayerBuildable, no build animation playing, nobody editing it and nothing blocking it. The
// server then equips the edit tool found in the inventory by the game data's definition (UFortGameData +0x994).
void EditCheck() {
    uintptr_t pc = engine::LocalPlayerController(), pawn = engine::PawnOf(pc);
    float at[3];
    if (!pawn || !engine::Location(pawn, at)) {
        LOG("editcheck: no pawn");
        return;
    }
    uintptr_t gameData = reinterpret_cast<uintptr_t(__cdecl*)()>(engine::Va(kGameData))();
    const wchar_t* wanted = gameData ? reinterpret_cast<const wchar_t*>(At<uint32_t>(gameData, GameDataEditTool + 0xC)) : nullptr;
    std::string held;
    for (const HeldItem& item : Items(pc))
        if (engine::IsA(item.definition, "FortEditToolItemDefinition")) held += " " + engine::Describe(item.definition);
    LOG("editcheck: the game's edit tool %ls; in the inventory:%s", wanted ? wanted : L"(none)", held.empty() ? " none" : held.c_str());
    int shown = 0;
    for (uintptr_t building : engine::FindAllA("BuildingSMActor")) {
        float where[3];
        if (!engine::Location(building, where)) continue;
        float dx = where[0] - at[0], dy = where[1] - at[1], dz = where[2] - at[2];
        float distance = sqrtf(dx * dx + dy * dy + dz * dz);
        if (distance > kEditCheckRange || !(At<uint8_t>(building, BuildingFlags) & BuildingPlayerBuildable)) continue;
        auto canEdit = reinterpret_cast<bool(__thiscall*)(uintptr_t)>(At<uint32_t>(At<uint32_t>(building, 0), VtCanBeEdited));
        LOG("editcheck: %s %.0f away: can edit %d; pattern %s, support %s, build anim %d, editing %s, blocked by %s, flag %d",
            engine::Name(building).c_str(), distance, canEdit(building), engine::Name(At<uint32_t>(building, BuildingEditPattern)).c_str(),
            engine::Name(At<uint32_t>(building, BuildingEditSupport)).c_str(), At<uint8_t>(building, BuildingAnimType),
            engine::Name(At<uint32_t>(building, BuildingEditingPlayer)).c_str(),
            engine::Name(At<uint32_t>(building, BuildingPlacementBlocker)).c_str(), (At<uint32_t>(building, BuildingBits) >> 6) & 1);
        if (++shown >= 10) break;
    }
    if (!shown) LOG("editcheck: no player-buildable pieces within %.0f", kEditCheckRange);
}

void FillHealth(uintptr_t pawn) {
    uintptr_t health = At<uint32_t>(pawn, PawnHealthSet);
    if (!health) return;
    const char* set = "/Script/FortniteGame.FortHealthSet";
    SetBase(pawn, set, "Health", At<float>(health, HealthSetMaxHealth));
    SetBase(pawn, set, "CurrentShield", At<float>(health, HealthSetShield));
    Call(health, "/Script/FortniteGame.FortHealthSet.OnRep_MaxHealth");
    Call(health, "/Script/FortniteGame.FortHealthSet.OnRep_Health");
    Call(health, "/Script/FortniteGame.FortHealthSet.OnRep_Shield");
    Call(health, "/Script/FortniteGame.FortHealthSet.OnRep_CurrentShield");
}

bool LinkGadget(uintptr_t pc, uintptr_t pawn, uintptr_t gadget, uintptr_t abilityClass) {
    uintptr_t abilitySystem = At<uint32_t>(pawn, PawnAbilitySystem), inventory = At<uint32_t>(pc, PcWorldInventory);
    if (!abilitySystem || !inventory || !abilityClass) return false;
    engine::TArray& specs = At<engine::TArray>(abilitySystem, AbilitySystemSpecs);
    uintptr_t spec = 0;
    for (int32_t i = 0; specs.data && i < specs.num; i++) {
        uintptr_t candidate = specs.data + static_cast<uintptr_t>(i) * SpecSize;
        uintptr_t ability = At<uint32_t>(candidate, SpecAbility);  // the class default object
        if (ability && At<uint32_t>(ability, ObjectClass) == abilityClass) spec = candidate;  // the latest grant
    }
    if (!spec) return false;
    At<uint32_t>(spec, SpecSource) = static_cast<uint32_t>(gadget);
    int32_t handle = At<int32_t>(spec, SpecHandle);
    engine::TArray& instances = At<engine::TArray>(inventory, InventoryInstances);
    engine::TArray& entries = At<engine::TArray>(inventory, InventoryEntries);
    for (int32_t i = 0; instances.data && i < instances.num; i++) {
        uintptr_t item = At<uint32_t>(instances.data, i * 4);
        if (!item || At<uint32_t>(item, ItemEntry + EntryDefinition) != gadget) continue;
        At<int32_t>(item, ItemEntry + EntryAbilityHandle) = handle;
        for (int32_t e = 0; entries.data && e < entries.num; e++) {
            uintptr_t entry = entries.data + static_cast<uintptr_t>(e) * EntrySize;
            if (SameGuid(entry, reinterpret_cast<const void*>(item + ItemEntry + EntryGuid))) {
                At<int32_t>(entry, EntryAbilityHandle) = handle;
                MarkEntryDirty(inventory, entry);
            }
        }
    }
    return true;
}

int RemoveItems(uintptr_t pc, uintptr_t definition, int count) {
    uintptr_t inventory = At<uint32_t>(pc, PcWorldInventory);
    if (!inventory) return 0;
    engine::TArray& instances = At<engine::TArray>(inventory, InventoryInstances);
    int removed = 0;
    for (int32_t i = 0; instances.data && i < instances.num && removed < count; i++) {
        uintptr_t item = At<uint32_t>(instances.data, i * 4);
        if (!item || At<uint32_t>(item, ItemEntry + EntryDefinition) != definition) continue;
        int32_t& have = At<int32_t>(item, ItemEntry + EntryCount);
        int take = (std::min)(have, count - removed);
        have -= take;
        removed += take;
        if (have > 0) {
            SyncEntry(inventory, item);
        } else {
            RemoveInstance(pc, inventory, i);
            i--;
        }
    }
    if (removed) Refresh(pc);
    return removed;
}

int RemoveItem(uintptr_t pc, const void* guid, int count, uintptr_t* definition) {
    uintptr_t inventory = At<uint32_t>(pc, PcWorldInventory);
    if (!inventory) return 0;
    engine::TArray& instances = At<engine::TArray>(inventory, InventoryInstances);
    for (int32_t i = 0; instances.data && i < instances.num; i++) {
        uintptr_t item = At<uint32_t>(instances.data, i * 4);
        if (!item || !SameGuid(item + ItemEntry, guid)) continue;
        if (definition) *definition = At<uint32_t>(item, ItemEntry + EntryDefinition);
        int32_t& have = At<int32_t>(item, ItemEntry + EntryCount);
        int take = count > 0 ? (std::min)(have, count) : have;
        have -= take;
        if (have > 0) SyncEntry(inventory, item);
        else RemoveInstance(pc, inventory, i);
        Refresh(pc);
        return take;
    }
    return 0;
}

// A dropped item lands as a normal pickup (AFortPickup, tossed from the pawn the way the game's own drops are).
bool SpawnPickup(uintptr_t pc, uintptr_t definition, int count) {
    uintptr_t pawn = engine::PawnOf(pc);
    float at[3];
    if (!engine::IsA(definition, "FortWorldItemDefinition") || !pawn || !engine::Location(pawn, at)) return false;
    uintptr_t pickup = engine::SpawnActor(engine::FindClass("/Script/FortniteGame.FortPickup"), pc);
    if (!pickup) return false;
    engine::Teleport(pickup, at);  // spawned at the origin: the toss starts from where the actor is
    uintptr_t item = 0;
    Call(definition, "/Script/FortniteGame.FortItemDefinition.CreateTemporaryItemInstanceBP", &item);
    if (!item) return false;
    uint8_t* entry = reinterpret_cast<uint8_t*>(pickup + PickupEntry);
    memcpy(entry, reinterpret_cast<const void*>(item + ItemEntry), EntrySize);
    for (int member : kEntryHeapMembers) memset(entry + member, 0, 12);
    At<int32_t>(pickup + PickupEntry, EntryCount) = count;
    Call(pickup, "/Script/FortniteGame.FortPickup.OnRep_PrimaryPickupItemEntry");
    struct {
        float location[3];
        uintptr_t owner;
        int32_t overrideMaxStack;
    } toss{ { at[0], at[1], at[2] - 80.0f }, pawn, 0 };
    Call(pickup, "/Script/FortniteGame.FortPickup.TossPickup", &toss);
    return true;
}

}  // namespace player
