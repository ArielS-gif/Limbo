#include "backend.h"
#include "engine.h"
#include "log.h"
#include "mission.h"
#include <windows.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

namespace loadout {

using engine::At;

namespace {
// The server would have applied the player's hero from their profile; in this client only the outfit part survives
// (ApplyHero). The hero chosen in the lobby comes from the backend's loadout route instead: its type and gender, the
// ability points in the primary and the four secondary traits, the harvest tier and the gadgets picked in the lobby.
//
// UFortHeroType: PrimaryTrait, SecondaryTraits[4] (UFortTrait*), AbilityKits (FHeroAbilityKit, 0x20 each: the kit,
// then its RequiredGPTags). The harvest kits Kit_HarvestWeapon_T01..T08 require Homebase.HarvestWeapon.Tier0..7 and
// hold the pickaxe. UFortTrait: AbilityKitSlots[5], a TArray<UFortAbilityKit*> each, unlocked one per point.
constexpr int HeroPrimaryTrait = 0x770, HeroSecondaryTraits = 0x774, HeroAbilityKits = 0x788, HeroKitSize = 0x20;
constexpr int HeroKitTags = 0x4;  // FGameplayTagContainer: TArray<FGameplayTag> (an FName each, 8 bytes) first
constexpr int TraitKitSlots = 0x418, TraitSlotSize = 0xC, kTraitSlots = 5;
// The hero's DefaultSpecialization (UFortHeroSpecialization) grants kits by hero level, among them the gadgets (Frag
// Grenade, Shockwave...): SpecializationSlots, FFortSpecializationSlot (0x30 each: GrantedAbilityKit, RemovedAbilityKit,
// AttributeRequirements, RequiredTags, MinimumHeroLevel).
constexpr int HeroSpecialization = 0x784, SpecializationSlots = 0x484, SpecSlotSize = 0x30;
constexpr int SpecSlotRemoved = 0x4, SpecSlotTags = 0x14, SpecSlotMinLevel = 0x2C;
// UFortAbilityKit, a UFortAbilitySet (GameplayAbilities, PassiveGameplayEffects) with Gadgets and Items (FAbilityKitItem,
// 0xC each: definition, quantity, replenishment).
constexpr int KitGadgets = 0x148, KitItems = 0x154, KitItemSize = 0xC;
constexpr int GadgetAbility = 0x5BC;  // UFortGadgetItemDefinition.GameplayAbility
constexpr int PcPlayerState = 0x254, PlayerStateHeroType = 0x30C, PlayerStateGender = 0x38C, PlayerStateAbilitySystem = 0x608;
// AFortPawn's IAbilitySystemInterface (its slot 2 returns the pawn's AbilitySystemComponent at +0x770).
constexpr int PawnAbilityInterface = 0x518, PawnAbilitySystem = 0x770;
// UFortAbilitySet::GiveToAbilitySystem(FFortAbilitySetHandle* out, IAbilitySystemInterface*, UObject* source, 0),
// thiscall, ret 10h: grants the set's abilities and applies its passive effects (the AI pawns' own BeginPlay uses it).
constexpr uint32_t kGiveAbilitySet = 0x00575C40;
// Primary quickbar slots (DefaultInput.ini: HotbarKey5/6 are Z and X, HotbarKey8/9 the keys 5 and 6): harvest tool and
// three weapons, the hero's ability gadgets on Z and X, and the homebase gadgets picked in the lobby (Select Your
// Gadgets, the profile's mode loadout; the same for every hero) under the HUD's Gadgets label, keys 5 and 6.
constexpr int kGadgetSlot = 4, kGadgetSlots = 2;
constexpr int kLobbyGadgetSlot = 7;
constexpr size_t kLobbyGadgetSlots = 2;
constexpr uint64_t kAutoSaveMs = 60000;  // the backpack is saved this often too, so closing the game loses little
constexpr int kMaxPoints = 5;
// The hero's stats. The server applied them with the hero's abilities (UFortGlobals::InitializePlayerGameplayAbilities;
// its zone call is compiled out, the lobby still runs it for the hero card): UFortHero::ApplySharedGE puts the hero's
// Fortitude, Offense, Tech and Resistance at its level (its specialization's curves) into the player's attribute set
// through the hero type's CombinedStatSourceGE (GE_SharedStats, set-by-caller magnitudes); the CombinedStatGEs map
// those onto max health, shield, weapon and trap damage; then the player's homebase record adds its bonuses (found by
// the local player in a static map, so the lobby's record is still there in a zone). The same calls are made here.
constexpr uint32_t kApplySharedGE = 0x00749DA0;         // UFortHero (UAbilitySystemComponent*), thiscall, ret 4
constexpr uint32_t kHomebaseRecord = 0x0079F8B0;        // (ULocalPlayer*) -> the player's homebase record, cdecl
constexpr uint32_t kApplyHomebaseEffects = 0x007B3330;  // record (IAbilitySystemInterface*, 0), thiscall, ret 8
constexpr int PcPlayer = 0x288;                         // APlayerController.Player
constexpr int HeroCombinedStatGEs = 0x794, StatEffectSize = 0x8;  // FGameplayEffectApplicationInfo: class, level
// The base attributes come from the hero's pawn class: its PawnStatHandle row (FFortPlayerPawnStats: health, shield,
// speeds, stamina, regeneration). The server wrote them into the player state's attribute sets as base values; the lobby
// still does it for the hero card's proxy actor. In a zone they stayed at the sets' defaults (health -1, speeds 0), and
// the mapped effects multiplied that base.
constexpr uint32_t kGetPawnStats = 0x0083C250;       // AFortPawn: const FFortPawnStats* (), thiscall
constexpr uint32_t kInitAttributeSets = 0x008ADDB0;  // FFortPlayerAttributeSets (const FFortPawnStats*), thiscall, ret 4
constexpr int PlayerStateAttributeSets = 0x5F0;      // health, damage, movement, construction, player, homebase sets
constexpr int PawnHealthSet = 0x754, HealthSetMaxHealth = 0x24;
constexpr int StatsMaxHealth = 0x0, StatsWalk = 0x4, StatsRun = 0x8, StatsSprint = 0xC, StatsMaxStamina = 0x34;
constexpr int StatsMaxShield = 0x50;
constexpr uint32_t kFallbackPawnStats = 0x03188BE0;  // what GetPawnStats returns without a row
constexpr float kMinRowSpeed = 50.0f;
// The hero type's own pawn class (PlayerPawn_Ninja_C...). The zone's game mode spawns its DefaultPawnClass,
// PlayerPawn_Generic_C, whatever the hero; the hero pawns add the class's gameplay tags (a gadget's RequiredEquipTags).
// FortGameMode keeps AGameMode's GetDefaultPawnClassForController_Implementation (vtable +0x2D4), wrapped here.
constexpr int HeroOverridePawnClass = 0x7D0, SoftPath = 0xC, ObjectClass = 0xC, ClassDefaultObject = 0xA8;
constexpr uint32_t kDefaultPawnClassEntry = 0x02704DD4, kDefaultPawnClass = 0x016B5CF0;  // AGameMode (AController*), ret 4
// The mapped effects read the hero's stats from their source, the effect context's instigator. The ability system's
// own context names the player state, through which no ability system is found here, so every magnitude fell back to 0
// (max health 0, weapon damage 0). The context is made the same way and the pawn named instigator instead.
constexpr uint32_t kMakeEffectContext = 0x01F696E0;  // UAbilitySystemComponent (FGameplayEffectContextHandle* out), ret 4
constexpr int EffectContextHandleSize = 0xC, HandleContext = 0x4, VtAddInstigator = 0x8;  // (instigator, causer)
// Without the lobby's hero item: the specialization's curves at the loadout's level, straight into the attributes.
// bool FCurveTableRowHandle::Eval(float x, float* y, const FString& context), thiscall, ret 0Ch.
constexpr uint32_t kEvalCurveRow = 0x0166EFE0;
constexpr int SpecStatCurves[] = { 0x494, 0x4A0, 0x4AC, 0x4B8 };  // Vitality, Offense, Prowess, Defence
constexpr const char* kStatAttributes[] = { "Vitality", "Offense", "Prowess", "Defense" };

struct Loadout {
    std::string hero;
    int gender = 0;
    int level = 1;
    int points[5] = {};  // primary, alpha, beta, gamma, delta
    int harvest = -1;
    std::vector<std::string> gadgets;
    std::vector<std::pair<std::string, int>> items;  // world item asset path, count: the profile's materials
    struct Carried {
        std::string path;
        int count, slot;
    };
    std::vector<Carried> backpack;  // theater0: what the player carried out of the last zone
    struct Structure {
        int outpost;
        std::string asset;
        int level;
    };
    std::vector<Structure> structures;  // the outposts' structures and their levels
};

// The materials given at the start (definition, path, count), for what was used or gathered by the end.
struct Material {
    uintptr_t definition;
    std::string path;
    int count;
};
// Every player in the zone has their own loadout (co-op: the host and the players who joined it), fetched by account;
// what was given and granted is kept per controller and per ability system.
std::map<uintptr_t, std::vector<Material>> g_materials;  // by controller, game thread only
std::map<uintptr_t, uint64_t> g_nextAutoSave;            // by controller

// The ability system lives on the player state, so a respawned pawn has the hero's kits and stats already.
// The ability systems the heroes' stats and kits went into, with their player's controller (forgotten when they leave).
std::map<uintptr_t, uintptr_t> g_statsAbilitySystems;
std::map<uintptr_t, uintptr_t> g_kitsAbilitySystems;
// A controller's pawn class, resolved from its lobby hero before it spawns (0: the zone's default); the spawn reads it.
std::map<uintptr_t, uintptr_t> g_pawnClasses;  // game thread

enum State { kIdle, kPending, kDone, kFailed };
struct Entry {
    State state = kIdle;
    Loadout loadout;
    std::map<std::string, std::string> schematics;  // the profile's schematic item ids -> their names (kept out of Loadout,
                                                     // which is copied around: about a thousand of them)
};
std::mutex g_lock;                      // the requests' worker threads write these
std::map<std::string, Entry> g_entries;  // by account id
int g_generation = 0;                   // a request from an earlier world is ignored

bool IsLocal(uintptr_t pc);

Loadout Parse(const std::string& text, std::map<std::string, std::string>& schematics) {
    Loadout lobby;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        std::istringstream words(line);
        std::string key;
        words >> key;
        if (key == "hero") words >> lobby.hero;
        else if (key == "gender") words >> lobby.gender;
        else if (key == "level") words >> lobby.level;
        else if (key == "points") for (int& points : lobby.points) words >> points;
        else if (key == "harvest") words >> lobby.harvest;
        else if (key == "gadget") {
            std::string name;
            words >> name;
            if (!name.empty()) lobby.gadgets.push_back(name);
        } else if (key == "item") {
            std::string path;
            int count = 0;
            words >> path >> count;
            if (!path.empty() && count > 0) lobby.items.emplace_back(path, count);
        } else if (key == "structure") {
            Loadout::Structure structure{ 0, "", 0 };
            words >> structure.outpost >> structure.asset >> structure.level;
            if (structure.outpost > 0 && !structure.asset.empty() && structure.level > 0) lobby.structures.push_back(structure);
        } else if (key == "schematic") {
            std::string id, name;
            words >> id >> name;
            if (!id.empty() && !name.empty()) schematics[id] = name;
        } else if (key == "backpack") {
            Loadout::Carried carried{ "", 0, -1 };
            words >> carried.path >> carried.count >> carried.slot;
            if (!carried.path.empty() && carried.count > 0) lobby.backpack.push_back(carried);
        }
    }
    return lobby;
}

bool Get(uintptr_t pc, Loadout& out) {
    std::string account = pc ? backend::AccountId(pc) : "";
    std::lock_guard<std::mutex> guard(g_lock);
    auto entry = g_entries.find(account);
    if (account.empty() || entry == g_entries.end() || entry->second.state != kDone) return false;
    out = entry->second.loadout;
    return true;
}

std::string TagNames(uintptr_t container) {
    engine::TArray& tags = At<engine::TArray>(container, 0);
    std::string names;
    for (int32_t i = 0; tags.data && i < tags.num && i < 8; i++)
        names += (names.empty() ? "" : ",") + engine::NameOf(At<int32_t>(tags.data, i * 8));
    return names;
}

// The harvest kit for the loadout's tier, or 0. Tier n (tag Homebase.HarvestWeapon.Tier<n>) is Kit_HarvestWeapon_T0<n+1>;
// matched by name, as the tags' FNames don't read back reliably here.
uintptr_t HarvestKit(uintptr_t heroType, int tier) {
    engine::TArray& kits = At<engine::TArray>(heroType, HeroAbilityKits);
    char wanted[32];
    snprintf(wanted, sizeof(wanted), "Kit_HarvestWeapon_T%02d", tier + 1);
    for (int32_t i = 0; kits.data && i < kits.num; i++) {
        uintptr_t kit = At<uint32_t>(kits.data + static_cast<uintptr_t>(i) * HeroKitSize, 0);
        if (kit && engine::Name(kit) == wanted) return kit;
    }
    return 0;
}

bool IsHarvestKit(uintptr_t kit) {
    return kit && engine::Name(kit).rfind("Kit_HarvestWeapon_", 0) == 0;
}

uintptr_t HeroType(uintptr_t pc) {
    uintptr_t ps = pc ? At<uint32_t>(pc, PcPlayerState) : 0;
    uintptr_t heroType = ps ? At<uint32_t>(ps, PlayerStateHeroType) : 0;
    return engine::IsA(heroType, "FortHeroType") ? heroType : 0;
}

// The pawn's ability-system interface, checked: its GetAbilitySystemComponent must return the pawn's component.
uintptr_t AbilityInterface(uintptr_t pawn) {
    uintptr_t iface = pawn + PawnAbilityInterface, vtable = At<uint32_t>(iface, 0);
    if (vtable < engine::Va(0x02600000) || vtable >= engine::Va(0x03400000)) return 0;
    auto get = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t)>(At<uint32_t>(vtable, 8));
    uintptr_t abilitySystem = At<uint32_t>(pawn, PawnAbilitySystem);
    return abilitySystem && get(iface) == abilitySystem ? iface : 0;
}

struct Granted {
    int kits = 0, abilities = 0, effects = 0, items = 0, gadgets = 0;
    std::string names;
};

// A gadget, into the primary quickbar's `slot` (-1: the inventory only).
void GiveGadget(uintptr_t pc, uintptr_t pawn, uintptr_t gadget, bool items, int slot, Granted& granted) {
    if (!engine::IsA(gadget, "FortGadgetItemDefinition")) return;
    uintptr_t ability = At<uint32_t>(gadget, GadgetAbility);
    if (ability) player::GrantAbility(pawn, ability);
    if (items) player::GiveItem(pc, gadget, 1, slot >= 0 ? 0 : -1, slot >= 0 ? slot : 0);
    // The quickbar fires a gadget through its item, which must hold the granted ability's handle (a respawned pawn's
    // new abilities too).
    if (ability && !player::LinkGadget(pc, pawn, gadget, ability))
        LOG("loadout: %s's ability %s is not on the pawn", engine::Name(gadget).c_str(), engine::Name(ability).c_str());
    granted.gadgets++;
    granted.names += " +" + engine::Name(gadget);
}

void GrantKit(uintptr_t pc, uintptr_t pawn, uintptr_t iface, uintptr_t kit, bool items, bool kitItems, Granted& granted) {
    if (!engine::IsA(kit, "FortAbilitySet")) return;
    struct {
        uint8_t abilitySystem[8];  // TWeakObjectPtr
        engine::TArray abilities, effects;
    } handle{};
    reinterpret_cast<void*(__thiscall*)(uintptr_t, void*, uintptr_t, uintptr_t, uint32_t)>(engine::Va(kGiveAbilitySet))(
        kit, &handle, iface, pawn, 0);
    granted.kits++;
    granted.abilities += handle.abilities.num;
    granted.effects += handle.effects.num;
    granted.names += " " + engine::Name(kit);
    if (handle.abilities.data) engine::Free(reinterpret_cast<void*>(handle.abilities.data));
    if (handle.effects.data) engine::Free(reinterpret_cast<void*>(handle.effects.data));
    if (!engine::IsA(kit, "FortAbilityKit")) return;
    engine::TArray& kitItemList = At<engine::TArray>(kit, KitItems);
    for (int32_t i = 0; items && kitItems && kitItemList.data && i < kitItemList.num; i++) {
        uintptr_t entry = kitItemList.data + static_cast<uintptr_t>(i) * KitItemSize;
        uintptr_t definition = At<uint32_t>(entry, 0);
        int32_t quantity = At<int32_t>(entry, 4);
        if (definition && player::GiveItem(pc, definition, quantity > 0 ? quantity : 1, -1, 0)) granted.items++;
    }
    engine::TArray& gadgets = At<engine::TArray>(kit, KitGadgets);
    for (int32_t i = 0; gadgets.data && i < gadgets.num; i++)
        GiveGadget(pc, pawn, At<uint32_t>(gadgets.data, i * 4), items, granted.gadgets < kGadgetSlots ? kGadgetSlot + granted.gadgets : -1,
                   granted);
}

// A gadget picked in the lobby by name (G_Teleporter): /Game/Abilities/Player/Generic/Gadgets/<folder>/<name>, the
// folder being the name without "G_" but for a few.
uintptr_t LobbyGadget(const std::string& name) {
    static const struct {
        const char* name;
        const char* folder;
    } kFolders[] = { { "G_Seeker", "SeekerBomb" }, { "G_Cart", "Track" }, { "G_Track", "Track" }, { "G_DebugAttributeGain", "Debug" } };
    std::string folder = name.rfind("G_", 0) == 0 ? name.substr(2) : name;
    for (const auto& entry : kFolders)
        if (name == entry.name) folder = entry.folder;
    std::string path = "/Game/Abilities/Player/Generic/Gadgets/" + folder + "/" + name + "." + name;
    uintptr_t gadget = engine::Load(std::wstring(path.begin(), path.end()));
    return engine::IsA(gadget, "FortGadgetItemDefinition") ? gadget : 0;
}

// A kit of the hero's, and whether its items are given (the harvest kit's pickaxe comes with the zone's own kit).
struct HeroKit {
    uintptr_t kit;
    bool items;
};

// The specialization's kits up to the hero's level, without those a later slot removes (no slot here needs tags).
void SpecializationKits(uintptr_t specialization, int level, std::vector<HeroKit>& kits) {
    if (!engine::IsA(specialization, "FortHeroSpecialization")) return;
    engine::TArray& slots = At<engine::TArray>(specialization, SpecializationSlots);
    std::vector<uintptr_t> removed;
    for (int32_t i = 0; slots.data && i < slots.num; i++) {
        uintptr_t slot = slots.data + static_cast<uintptr_t>(i) * SpecSlotSize;
        if (At<int32_t>(slot, SpecSlotMinLevel) <= level && At<uint32_t>(slot, SpecSlotRemoved))
            removed.push_back(At<uint32_t>(slot, SpecSlotRemoved));
    }
    for (int32_t i = 0; slots.data && i < slots.num; i++) {
        uintptr_t slot = slots.data + static_cast<uintptr_t>(i) * SpecSlotSize;
        uintptr_t kit = At<uint32_t>(slot, 0);
        if (!kit || At<int32_t>(slot, SpecSlotMinLevel) > level || !TagNames(slot + SpecSlotTags).empty()) continue;
        if (std::find(removed.begin(), removed.end(), kit) != removed.end()) continue;
        kits.push_back({ kit, true });
    }
}

void TraitKits(uintptr_t trait, int points, std::vector<HeroKit>& kits) {
    if (!engine::IsA(trait, "FortTrait")) return;
    for (int slot = 0; slot < kTraitSlots && slot < points; slot++) {
        engine::TArray& slotKits = At<engine::TArray>(trait, TraitKitSlots + slot * TraitSlotSize);
        for (int32_t i = 0; slotKits.data && i < slotKits.num; i++) kits.push_back({ At<uint32_t>(slotKits.data, i * 4), true });
    }
}

// The hero's kits for the loadout: inherent ones (those without requirements, and the harvest kit of the loadout's
// tier), the specialization's up to the hero's level and the traits' by points.
std::vector<HeroKit> Kits(uintptr_t heroType, const Loadout& lobby) {
    std::vector<HeroKit> kits;
    uintptr_t harvest = lobby.harvest >= 0 ? HarvestKit(heroType, lobby.harvest) : 0;
    engine::TArray& inherent = At<engine::TArray>(heroType, HeroAbilityKits);
    for (int32_t i = 0; inherent.data && i < inherent.num; i++) {
        uintptr_t entry = inherent.data + static_cast<uintptr_t>(i) * HeroKitSize;
        uintptr_t kit = At<uint32_t>(entry, 0);
        if (kit == harvest) kits.push_back({ kit, false });
        else if (!IsHarvestKit(kit) && TagNames(entry + HeroKitTags).empty()) kits.push_back({ kit, true });
    }
    SpecializationKits(At<uint32_t>(heroType, HeroSpecialization), lobby.level, kits);
    TraitKits(At<uint32_t>(heroType, HeroPrimaryTrait), (std::min)(lobby.points[0], kMaxPoints), kits);
    for (int i = 0; i < 4; i++)
        TraitKits(At<uint32_t>(heroType, HeroSecondaryTraits + i * 4), (std::min)(lobby.points[i + 1], kMaxPoints), kits);
    return kits;
}

// What the hero's kits give (the gadgets, the grenades they throw, the pickaxe): given anew in every zone, so never
// carried out of one.
std::vector<uintptr_t> HeroItems(uintptr_t pc) {
    std::vector<uintptr_t> items;
    Loadout lobby;
    uintptr_t heroType = HeroType(pc);
    if (!Get(pc, lobby) || !heroType) return items;
    for (const HeroKit& kit : Kits(heroType, lobby)) {
        if (!engine::IsA(kit.kit, "FortAbilityKit")) continue;
        engine::TArray& kitItems = At<engine::TArray>(kit.kit, KitItems);
        for (int32_t i = 0; kitItems.data && i < kitItems.num; i++)
            items.push_back(At<uint32_t>(kitItems.data + static_cast<uintptr_t>(i) * KitItemSize, 0));
        engine::TArray& gadgets = At<engine::TArray>(kit.kit, KitGadgets);
        for (int32_t i = 0; gadgets.data && i < gadgets.num; i++) items.push_back(At<uint32_t>(gadgets.data, i * 4));
    }
    return items;
}

bool FromHero(const std::vector<uintptr_t>& heroItems, uintptr_t definition) {
    return engine::IsA(definition, "FortGadgetItemDefinition") ||
           std::find(heroItems.begin(), heroItems.end(), definition) != heroItems.end();
}

std::wstring ReadWide(uintptr_t at) {
    const wchar_t* data = reinterpret_cast<const wchar_t*>(At<uint32_t>(at, 0));
    int32_t num = At<int32_t>(at, 4);
    return data && num > 1 ? std::wstring(data, num - 1) : std::wstring();
}

// The hero type's own pawn class, loaded (game thread), or 0.
uintptr_t HeroPawnClass(uintptr_t heroType) {
    std::wstring path = heroType ? ReadWide(heroType + HeroOverridePawnClass + SoftPath) : L"";
    uintptr_t cls = path.empty() ? 0 : engine::Load(path);
    return cls && engine::IsChildOf(cls, "FortPlayerPawn") ? cls : 0;
}

uintptr_t __fastcall DefaultPawnClass(uintptr_t gameMode, void*, uintptr_t controller) {
    uintptr_t cls = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t, uintptr_t)>(engine::Va(kDefaultPawnClass))(gameMode, controller);
    auto resolved = g_pawnClasses.find(controller);
    uintptr_t pawnClass = resolved != g_pawnClasses.end() ? resolved->second : 0;
    if (pawnClass && engine::IsA(controller, "FortPlayerController")) {
        LOG("loadout: %s spawns as %s instead of %s", engine::Name(controller).c_str(), engine::Name(pawnClass).c_str(),
            engine::Name(cls).c_str());
        return pawnClass;
    }
    return cls;
}

// The controller belongs to this copy of the game (its UPlayer is a ULocalPlayer, not a joined player's connection).
bool IsLocal(uintptr_t pc) {
    return pc && engine::IsA(At<uint32_t>(pc, PcPlayer), "LocalPlayer");
}

// The base attributes from the hero pawn class's stats row, as base values, as the server's spawn set them.
void InitBaseStats(uintptr_t pc, uintptr_t pawn, uintptr_t heroType) {
    uintptr_t pawnClass = HeroPawnClass(heroType);
    if (!pawnClass) pawnClass = At<uint32_t>(pawn, ObjectClass);
    uintptr_t defaults = pawnClass ? At<uint32_t>(pawnClass, ClassDefaultObject) : 0;
    uintptr_t ps = At<uint32_t>(pc, PcPlayerState), sets = ps ? ps + PlayerStateAttributeSets : 0;
    if (!defaults || !sets || !At<uint32_t>(sets, 0)) {
        LOG("loadout: no base stats (%s, the player state's health set %s)", engine::Describe(pawnClass).c_str(),
            sets ? engine::Describe(At<uint32_t>(sets, 0)).c_str() : "null");
        return;
    }
    uintptr_t stats = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t)>(engine::Va(kGetPawnStats))(defaults);
    if (!stats || stats == engine::Va(kFallbackPawnStats) || !(At<float>(stats, StatsWalk) >= kMinRowSpeed)) {
        // No row: its stand-in has walk/run/sprint 10. Only a health base, so the mapped effects multiply a real one.
        LOG("loadout: %s has no pawn stats row; max health base 100 only", engine::Name(pawnClass).c_str());
        player::SetAttribute(pawn, "/Script/FortniteGame.FortHealthSet", "MaxHealth", 100.0f);
        return;
    }
    reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t)>(engine::Va(kInitAttributeSets))(sets, stats);
    LOG("loadout: base stats from %s: max health %.0f, max shield %.0f, walk %.0f, run %.0f, sprint %.0f, stamina %.0f%s",
        engine::Name(pawnClass).c_str(), At<float>(stats, StatsMaxHealth), At<float>(stats, StatsMaxShield), At<float>(stats, StatsWalk),
        At<float>(stats, StatsRun), At<float>(stats, StatsSprint), At<float>(stats, StatsMaxStamina),
        At<uint32_t>(sets, 0) == At<uint32_t>(pawn, PawnHealthSet) ? "" : " (not the pawn's health set!)");
}

// An effect context naming the pawn as instigator, so effects that read their source's attributes find its ability system.
void PawnContext(uintptr_t abilitySystem, uintptr_t pawn, uint8_t* handle) {
    reinterpret_cast<void*(__thiscall*)(uintptr_t, void*)>(engine::Va(kMakeEffectContext))(abilitySystem, handle);
    uintptr_t context = *reinterpret_cast<uint32_t*>(handle + HandleContext);
    if (context)
        reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t, uintptr_t)>(At<uint32_t>(At<uint32_t>(context, 0), VtAddInstigator))(
            context, pawn, pawn);
}

// The lobby's item for the zone's hero type (the profile's items outlive the trip to the zone), or 0.
uintptr_t HeroItem(uintptr_t heroType) {
    uintptr_t function = engine::FindFunction("/Script/FortniteGame.FortHero.GetHeroTypeBP");
    std::vector<uintptr_t> heroes = engine::FindAllA("FortHero");
    uintptr_t found = 0;
    for (uintptr_t hero : heroes) {
        uintptr_t type = 0;
        if (function) engine::ProcessEvent(hero, function, &type);
        if (type == heroType && !found) found = hero;
    }
    LOG("loadout: %zu hero item(s) here; %s", heroes.size(),
        found ? ("the zone's is " + engine::Describe(found)).c_str() : "none of the zone's hero type");
    return found;
}

// The hero's four stats from its specialization's curves at the loadout's level, as the attributes' base values.
bool StatsFromCurves(uintptr_t pawn, uintptr_t heroType, int level) {
    uintptr_t specialization = At<uint32_t>(heroType, HeroSpecialization);
    if (!engine::IsA(specialization, "FortHeroSpecialization")) return false;
    struct {
        const wchar_t* data;
        int32_t num, max;
    } context{};
    std::string values;
    for (int i = 0; i < 4; i++) {
        float value = 0.0f;
        if (!reinterpret_cast<bool(__thiscall*)(uintptr_t, float, float*, const void*)>(engine::Va(kEvalCurveRow))(
                specialization + SpecStatCurves[i], static_cast<float>(level), &value, &context))
            continue;  // no such curve
        player::SetAttribute(pawn, "/Script/FortniteGame.FortPlayerAttrSet", kStatAttributes[i], value);
        values += std::string(values.empty() ? "" : ", ") + kStatAttributes[i] + " " + std::to_string(static_cast<int>(value));
    }
    LOG("loadout: %s at level %d from its curves: %s", engine::Name(specialization).c_str(), level, values.c_str());
    return !values.empty();
}
}  // namespace

bool Install() {
    bool ok = engine::PatchPointer(kDefaultPawnClassEntry, kDefaultPawnClass, reinterpret_cast<void*>(&DefaultPawnClass));
    LOG("loadout: %s", ok ? "zone players spawn as their lobby hero's pawn class" : "FortGameMode's pawn class slot not as expected");
    return ok;
}

void Resolve(uintptr_t pc) {
    Loadout lobby;
    if (!pc || g_pawnClasses.count(pc) || !Settled(pc)) return;
    g_pawnClasses[pc] = 0;
    if (!Get(pc, lobby) || lobby.hero.empty()) return;
    std::wstring name(lobby.hero.begin(), lobby.hero.end());
    uintptr_t heroType = engine::Load(L"/Game/Heroes/" + name + L"." + name);
    uintptr_t pawnClass = engine::IsA(heroType, "FortHeroType") ? HeroPawnClass(heroType) : 0;
    g_pawnClasses[pc] = pawnClass;
    LOG("loadout: %s (%s)'s pawn class is %s", lobby.hero.c_str(), engine::Name(pc).c_str(),
        pawnClass ? engine::Name(pawnClass).c_str() : "the zone's default");
}

std::string SchematicName(uintptr_t pc, const std::string& itemId) {
    std::string account = pc ? backend::AccountId(pc) : "";
    std::lock_guard<std::mutex> guard(g_lock);
    auto entry = g_entries.find(account);
    if (entry == g_entries.end()) return "";
    auto found = entry->second.schematics.find(itemId);
    return found != entry->second.schematics.end() ? found->second : "";
}

// A joined player left: what was kept by their controller goes, or a player joining later at the same address (the
// controller's memory is reused) would get their pawn class, materials or "stats already applied".
void Forget(uintptr_t pc) {
    g_pawnClasses.erase(pc);
    g_materials.erase(pc);
    g_nextAutoSave.erase(pc);
    for (auto* systems : { &g_statsAbilitySystems, &g_kitsAbilitySystems })
        for (auto it = systems->begin(); it != systems->end();) it = it->second == pc ? systems->erase(it) : std::next(it);
}

void Reset() {
    g_statsAbilitySystems.clear();
    g_kitsAbilitySystems.clear();
    g_pawnClasses.clear();
    g_materials.clear();
    g_nextAutoSave.clear();
    std::lock_guard<std::mutex> guard(g_lock);
    g_entries.clear();
    g_generation++;
}

void Fetch(uintptr_t pc) {
    std::string account = pc ? backend::AccountId(pc) : "";
    if (account.empty()) return;  // the player state's net id is not there yet; asked again next tick
    int generation;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        Entry& entry = g_entries[account];
        if (entry.state != kIdle) return;
        entry.state = kPending;
        generation = g_generation;
    }
    std::wstring path = L"/fortnite/api/game/v2/ot65/loadout/" + std::wstring(account.begin(), account.end());
    backend::Get(path, [generation, account](int status, const std::string& body) {
        std::map<std::string, std::string> schematics;
        Loadout lobby = status == 200 ? Parse(body, schematics) : Loadout{};
        {
            std::lock_guard<std::mutex> guard(g_lock);
            if (generation != g_generation) return;
            Entry& entry = g_entries[account];
            entry.loadout = lobby;
            entry.schematics.swap(schematics);
            entry.state = status == 200 ? kDone : kFailed;
        }
        if (status == 200)
            LOG("loadout: %s's lobby hero is %s (gender %d), points %d %d %d %d %d, harvest tier %d, %zu gadget(s), %zu "
                "material(s)", account.c_str(), lobby.hero.c_str(), lobby.gender, lobby.points[0], lobby.points[1],
                lobby.points[2], lobby.points[3], lobby.points[4], lobby.harvest, lobby.gadgets.size(), lobby.items.size());
        else
            LOG("loadout: the backend's loadout route answered %d for %s; the zone's own hero is kept", status, account.c_str());
    });
}

bool Settled(uintptr_t pc) {
    std::string account = pc ? backend::AccountId(pc) : "";
    std::lock_guard<std::mutex> guard(g_lock);
    auto entry = g_entries.find(account);
    return entry != g_entries.end() && (entry->second.state == kDone || entry->second.state == kFailed);
}

void Prepare(uintptr_t pc) {
    Loadout lobby;
    uintptr_t ps = pc ? At<uint32_t>(pc, PcPlayerState) : 0;
    if (!Get(pc, lobby) || lobby.hero.empty() || !engine::IsA(ps, "FortPlayerState")) return;
    std::wstring name(lobby.hero.begin(), lobby.hero.end());
    uintptr_t heroType = engine::Load(L"/Game/Heroes/" + name + L"." + name);
    if (!engine::IsA(heroType, "FortHeroType")) {
        LOG("loadout: no hero type %s in this build; keeping %s", lobby.hero.c_str(), engine::Name(HeroType(pc)).c_str());
        return;
    }
    uintptr_t before = At<uint32_t>(ps, PlayerStateHeroType);
    At<uint32_t>(ps, PlayerStateHeroType) = static_cast<uint32_t>(heroType);
    if (lobby.gender == 1 || lobby.gender == 2) At<uint8_t>(ps, PlayerStateGender) = static_cast<uint8_t>(lobby.gender);
    if (before != heroType) LOG("loadout: the player state's hero %s -> %s", engine::Name(before).c_str(), engine::Name(heroType).c_str());
}

uintptr_t Pickaxe(uintptr_t pc) {
    Loadout lobby;
    uintptr_t heroType = HeroType(pc);
    if (!Get(pc, lobby) || lobby.harvest < 0 || !heroType) return 0;
    uintptr_t kit = HarvestKit(heroType, lobby.harvest), pickaxe = 0;
    if (kit) {
        engine::TArray& items = At<engine::TArray>(kit, KitItems);
        if (items.data && items.num > 0) pickaxe = At<uint32_t>(items.data, 0);  // FAbilityKitItem.Item
    }
    if (!engine::IsA(pickaxe, "FortWeaponItemDefinition")) {
        LOG("loadout: %s has no harvest kit for tier %d", engine::Name(heroType).c_str(), lobby.harvest);
        return 0;
    }
    LOG("loadout: pickaxe %s (%s)", engine::Name(pickaxe).c_str(), engine::Name(kit).c_str());
    return pickaxe;
}

bool GiveMaterials(uintptr_t pc) {
    Loadout lobby;
    std::vector<Material>& materials = g_materials[pc];
    materials.clear();
    if (!Get(pc, lobby) || lobby.items.empty()) return false;
    for (const auto& item : lobby.items) {
        uintptr_t definition = engine::Load(std::wstring(item.first.begin(), item.first.end()));
        if (definition && player::GiveItem(pc, definition, item.second, -1, 0, false))
            materials.push_back({ definition, item.first, item.second });
        else
            LOG("loadout: no world item %s", item.first.c_str());
    }
    LOG("loadout: %zu of %zu materials from %s's profile", materials.size(), lobby.items.size(), engine::Name(pc).c_str());
    return !materials.empty();
}

bool GiveBackpack(uintptr_t pc) {
    Loadout lobby;
    if (!Get(pc, lobby) || lobby.backpack.empty()) return false;
    std::vector<uintptr_t> heroItems = HeroItems(pc);
    std::string names;
    int given = 0;
    for (const auto& carried : lobby.backpack) {
        uintptr_t definition = engine::Load(std::wstring(carried.path.begin(), carried.path.end()));
        if (!definition) {
            LOG("loadout: the backpack's %s is not in this build", carried.path.c_str());
            continue;
        }
        if (FromHero(heroItems, definition)) continue;  // saved before hero items were left out; the hero gives them
        bool slotted = carried.slot >= 1;  // slot 0 is the hero's harvest tool
        // Weapons are one per item: a save from before stacks were refused can hold "2 rifles" in one entry; the
        // others go to the backpack as items of their own.
        if (engine::IsA(definition, "FortWeaponItemDefinition") && carried.count > 1) {
            for (int extra = 1; extra < carried.count; extra++) player::GiveItem(pc, definition, 1, -1, 0, false);
            if (!player::GiveItem(pc, definition, 1, slotted ? 0 : -1, slotted ? carried.slot : 0, false)) continue;
        } else if (!player::GiveItem(pc, definition, carried.count, slotted ? 0 : -1, slotted ? carried.slot : 0, false)) {
            continue;
        }
        given++;
        names += " " + engine::Name(definition) + (carried.count > 1 ? " x" + std::to_string(carried.count) : "");
    }
    LOG("loadout: the backpack brought %d item(s):%s", given, names.c_str());
    return given > 0;
}

// The object path of a definition ("FortWeaponRangedItemDefinition /Game/...X.X" -> "/Game/...X.X").
std::string PathOf(uintptr_t object) {
    std::string full = engine::Describe(object);
    size_t space = full.find(' ');
    return space == std::string::npos ? full : full.substr(space + 1);
}

void Save(uintptr_t pc, bool autosave) {
    if (game::Tutorial()) {  // the tutorial's inventory isn't the player's: the real backpack and materials stay as they are
        if (!autosave) LOG("loadout: the tutorial's inventory is not saved");
        return;
    }
    std::string account = backend::AccountId(pc);
    if (account.empty()) {
        if (!autosave) LOG("loadout: no account id; the backpack is not saved");
        return;
    }
    // The backpack: what the player holds, minus what every zone gives anew (building pieces, the edit tool, the hero's
    // harvest tool in slot 0, gadgets and the rest of the hero's kit items), the materials (counted below) and mission
    // items.
    std::vector<uintptr_t> heroItems = HeroItems(pc);
    std::string backpack, materials, names;
    int carried = 0;
    for (const player::HeldItem& item : player::Items(pc)) {
        if (item.count <= 0 || item.slot == 0 || FromHero(heroItems, item.definition) ||
            engine::IsA(item.definition, "FortBuildingItemDefinition") ||
            engine::IsA(item.definition, "FortEditToolItemDefinition") || engine::IsA(item.definition, "FortResourceItemDefinition") ||
            engine::IsA(item.definition, "FortIngredientItemDefinition") || engine::IsA(item.definition, "FortNeverPersistItemDefinition") ||
            engine::IsA(item.definition, "FortOutpostItemDefinition"))  // the outpost's structures are kept apart
            continue;
        backpack += std::string(carried ? "," : "") + "{\"path\":\"" + PathOf(item.definition) + "\",\"count\":" +
                    std::to_string(item.count) + ",\"slot\":" + std::to_string(item.slot) + "}";
        names += " " + engine::Name(item.definition);
        carried++;
    }
    // Materials go back as changes since the last save, which becomes the new baseline (autosaves don't count twice).
    int changed = 0;
    for (Material& material : g_materials[pc]) {
        int now = player::CountItems(pc, material.definition), delta = now - material.count;
        if (!delta) continue;
        material.count = now;
        materials += std::string(changed ? "," : "") + "{\"path\":\"" + material.path + "\",\"delta\":" + std::to_string(delta) + "}";
        changed++;
    }
    std::string json = "{\"accountId\":\"" + account + "\",\"backpack\":[" + backpack + "],\"materials\":[" + materials + "]}";
    LOG("loadout: %s the backpack (%d item(s):%s) and %d material change(s)", autosave ? "autosaving" : "saving", carried,
        names.c_str(), changed);
    backend::Post(L"/fortnite/api/game/v2/ot65/zone/leave", json, [autosave](int status, const std::string& body) {
        if (status != 200 || !autosave) LOG("loadout: the backend saved the zone's inventory: %d %s", status, body.substr(0, 300).c_str());
    });
}

void AutoSave(uintptr_t pc, uint64_t now) {
    uint64_t& next = g_nextAutoSave[pc];
    if (!next || now < next) {
        if (!next) next = now + kAutoSaveMs;
        return;
    }
    next = now + kAutoSaveMs;
    Save(pc, true);
}

void Grant(uintptr_t pc, uintptr_t pawn, bool items) {
    Loadout lobby;
    uintptr_t heroType = HeroType(pc);
    if (!Get(pc, lobby) || !heroType || !engine::IsA(pawn, "FortPawn")) return;
    uintptr_t iface = AbilityInterface(pawn);
    if (!iface) {
        LOG("loadout: %s's ability-system interface is not where expected; no hero abilities", engine::Name(pawn).c_str());
        return;
    }
    uintptr_t abilitySystem = At<uint32_t>(pawn, PawnAbilitySystem);
    if (g_kitsAbilitySystems.count(abilitySystem)) {
        LOG("loadout: the hero's kits are on this ability system already (a respawn)");
        return;
    }
    g_kitsAbilitySystems[abilitySystem] = pc;
    Granted granted;
    for (const HeroKit& kit : Kits(heroType, lobby)) GrantKit(pc, pawn, iface, kit.kit, items, kit.items, granted);
    std::vector<uintptr_t> picked;
    for (const std::string& name : lobby.gadgets) {
        uintptr_t gadget = LobbyGadget(name);
        if (!gadget) {
            LOG("loadout: no gadget %s in this build", name.c_str());
        } else if (picked.size() < kLobbyGadgetSlots && std::find(picked.begin(), picked.end(), gadget) == picked.end()) {
            GiveGadget(pc, pawn, gadget, items, kLobbyGadgetSlot + static_cast<int>(picked.size()), granted);
            picked.push_back(gadget);
        }
    }
    LOG("loadout: %s: %d kit(s), %d abilities, %d passive effect(s), %d item(s), %d gadget(s):%s", engine::Name(heroType).c_str(),
        granted.kits, granted.abilities, granted.effects, granted.items, granted.gadgets, granted.names.c_str());
}

// The outpost zone's number (Zone_Outpost_PvE_0N), 0 elsewhere.
int OutpostNumber() {
    std::string world = engine::Name(engine::World());
    return game::Outpost() && !world.empty() && world.back() >= '1' && world.back() <= '4' ? world.back() - '0' : 0;
}

void GiveOutpostStructures(uintptr_t pc) {
    Loadout lobby;
    int outpost = OutpostNumber();
    if (!outpost || !Get(pc, lobby)) return;
    std::string given;
    for (const Loadout::Structure& structure : lobby.structures) {
        if (structure.outpost != outpost) continue;
        std::string path = "/Game/TheOutpost/Items/" + structure.asset + "." + structure.asset;
        uintptr_t definition = engine::Load(std::wstring(path.begin(), path.end()));
        if (!engine::IsA(definition, "FortOutpostItemDefinition")) continue;
        player::SetStructureLevel(pc, definition, structure.level);
        given += " " + structure.asset + " " + std::to_string(structure.level);
    }
    LOG("loadout: outpost %d's structures:%s", outpost, given.empty() ? " none built" : given.c_str());
}

void SaveStructure(uintptr_t pc, uintptr_t structure, int level, const std::vector<std::pair<std::string, int>>& accountItems) {
    std::string account = backend::AccountId(pc), asset = engine::Name(structure);
    int outpost = OutpostNumber();
    if (account.empty() || !outpost) return;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto entry = g_entries.find(account);
        if (entry != g_entries.end()) {
            auto& structures = entry->second.loadout.structures;
            auto found = std::find_if(structures.begin(), structures.end(),
                                      [&](const Loadout::Structure& s) { return s.outpost == outpost && s.asset == asset; });
            if (found != structures.end()) found->level = level;
            else structures.push_back({ outpost, asset, level });
        }
    }
    std::string items;
    for (const auto& [name, quantity] : accountItems)
        items += std::string(items.empty() ? "" : ",") + "{\"name\":\"" + name + "\",\"quantity\":" + std::to_string(quantity) + "}";
    std::string json = "{\"accountId\":\"" + account + "\",\"outpost\":" + std::to_string(outpost) + ",\"structure\":\"" + asset +
                       "\",\"level\":" + std::to_string(level) + ",\"accountItems\":[" + items + "]}";
    backend::Post(L"/fortnite/api/game/v2/ot65/outpost/upgrade", json, [asset, level](int status, const std::string&) {
        LOG("loadout: %s level %d %s (%d)", asset.c_str(), level, status == 200 ? "kept" : "NOT kept", status);
    });
}

void GiveLobbyGadget(const std::string& name) {
    uintptr_t pc = engine::LocalPlayerController(), pawn = engine::PawnOf(pc);
    uintptr_t gadget = LobbyGadget(name);
    if (!pc || !pawn || !gadget) {
        LOG("loadout: can't give the gadget %s (no player, or no such gadget)", name.c_str());
        return;
    }
    Granted granted;
    GiveGadget(pc, pawn, gadget, true, kLobbyGadgetSlot, granted);
    LOG("loadout: gave the gadget %s (key 5)", engine::Name(gadget).c_str());
}

void ApplyStats(uintptr_t pc, uintptr_t pawn) {
    Loadout lobby;
    uintptr_t heroType = HeroType(pc);
    uintptr_t abilitySystem = engine::IsA(pawn, "FortPawn") ? At<uint32_t>(pawn, PawnAbilitySystem) : 0;
    uintptr_t iface = abilitySystem ? AbilityInterface(pawn) : 0;
    if (!Get(pc, lobby) || !heroType || !iface) return;
    if (g_statsAbilitySystems.count(abilitySystem)) {
        LOG("loadout: the hero's stats are on this ability system already: %s", player::StatLine(pc, pawn).c_str());
        return;
    }
    g_statsAbilitySystems[abilitySystem] = pc;
    uintptr_t ps = At<uint32_t>(pc, PcPlayerState);
    LOG("loadout: stats before the hero: %s (the pawn's ability system is %sthe player state's)", player::StatLine(pc, pawn).c_str(),
        ps && At<uint32_t>(ps, PlayerStateAbilitySystem) == abilitySystem ? "" : "not ");
    InitBaseStats(pc, pawn, heroType);
    LOG("loadout: stats with the base: %s", player::StatLine(pc, pawn).c_str());
    // The hero's four stats, then what they map onto. The hero item is this copy's own profile's: a joined player's
    // come from the hero's curves at their level.
    if (uintptr_t hero = IsLocal(pc) ? HeroItem(heroType) : 0)
        reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t)>(engine::Va(kApplySharedGE))(hero, abilitySystem);
    else
        StatsFromCurves(pawn, heroType, lobby.level);
    uintptr_t apply = engine::FindFunction("/Script/GameplayAbilities.AbilitySystemComponent.BP_ApplyGameplayEffectToSelf");
    engine::TArray& effects = At<engine::TArray>(heroType, HeroCombinedStatGEs);
    std::string names;
    std::vector<int32_t> handles;
    for (int32_t i = 0; apply && effects.data && i < effects.num; i++) {
        uintptr_t entry = effects.data + static_cast<uintptr_t>(i) * StatEffectSize;
        struct {
            uintptr_t effectClass;
            float level;
            uint8_t context[EffectContextHandleSize];
            int32_t handle;
        } p{ At<uint32_t>(entry, 0), At<float>(entry, 4) };
        if (!p.effectClass) continue;
        PawnContext(abilitySystem, pawn, p.context);  // its reference is kept (one small context per effect)
        engine::ProcessEvent(abilitySystem, apply, &p);
        handles.push_back(p.handle);
        names += " " + engine::Name(p.effectClass);
    }
    LOG("loadout: stats with the hero:%s: %s", names.c_str(), player::StatLine(pc, pawn).c_str());
    // Should a mapped effect still fail to read its source, its multiplier is 0: max health 0 leaves the player dead on
    // their feet. Then the mapped effects go again, and the base stats stay.
    uintptr_t health = At<uint32_t>(pawn, PawnHealthSet);
    if (health && !(At<float>(health, HealthSetMaxHealth) >= 1.0f)) {
        uintptr_t remove = engine::FindFunction("/Script/GameplayAbilities.AbilitySystemComponent.RemoveActiveGameplayEffect");
        for (int32_t handle : handles) {
            struct {
                int32_t handle, stacks;
                bool removed;
                uint8_t pad[3];
            } p{ handle, -1 };
            if (remove) engine::ProcessEvent(abilitySystem, remove, &p);
        }
        LOG("loadout: the mapped effects left max health at %.0f; removed them again: %s", At<float>(health, HealthSetMaxHealth),
            player::StatLine(pc, pawn).c_str());
    }
    // The homebase's bonuses, as the server added them after the hero's (this copy's own player only: the record is
    // looked up through a ULocalPlayer, and a joined player's UPlayer is their connection).
    uintptr_t localPlayer = IsLocal(pc) ? At<uint32_t>(pc, PcPlayer) : 0;
    uintptr_t record = localPlayer ? reinterpret_cast<uintptr_t(__cdecl*)(uintptr_t)>(engine::Va(kHomebaseRecord))(localPlayer) : 0;
    if (record) {
        reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t, uint32_t)>(engine::Va(kApplyHomebaseEffects))(record, iface, 0);
        LOG("loadout: stats with the homebase (%s): %s", engine::Describe(record).c_str(), player::StatLine(pc, pawn).c_str());
    } else {
        LOG("loadout: no homebase record for %s", engine::Describe(localPlayer).c_str());
    }
    player::FillHealth(pawn);
    LOG("loadout: stats now: %s", player::StatLine(pc, pawn).c_str());
}

}  // namespace loadout
