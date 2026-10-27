#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Internal interfaces of the mission server; game.h describes the pieces. Everything that calls into the engine
// runs on the game thread, without the tracer's lock.
namespace game {
struct Switches {
    bool zone = true, player = true, atlas = true, objectives = true, director = true, autostart = true;
    bool ui = true, cheats = true;
    bool endzone = true, results = true;  // the end of the zone; its results screen (off: countdown and return only)
    bool inventory = true;                // crafting from schematics
    bool host = true;                     // co-op: the zone listens for other players once its mission is loaded
    bool roamers = true;                  // husk groups out on the map (director::UpdateRoamers)
};
extern Switches g_on;
// The tutorial (the onboarding fort, "help Ramirez"): a single-player zone that starts from nothing. The player gets
// the harvest tool, building pieces and the edit tool only (no kit, backpack, profile materials or cheats), nothing is
// saved from it (the real backpack stays as it was), no co-op, no roaming husks or chest guardians (it scripts its own).
bool Tutorial();
}  // namespace game

// Co-op: this client hosts its zone for other copies of the game (net.cpp), or joins one as a plain client.
namespace net {
bool Install();          // the IpNetDriver TickFlush wrapper (startup)
bool AllowChatLogin();   // the game's chat login without TLS: first thing at load, before the game logs in
void PatchCallSites();   // AFortGameSession::ValidatePlayer's calls (game thread)
void Reset();            // the world changed
void Prepare();          // the zone's first tick: its game net driver, not used yet, records what the zone destroys
bool Host(int port);     // makes the current world listen (0: the default port, 7777)
void Open();             // tells other players about the hosted game (the backend's list), from now on
bool Hosting();          // this DLL made the current world listen
bool IsClient();         // the current world is connected to a host: the server pieces stay off
void Join(const std::string& address);  // "ip[:port]": the game's own open command
void JoinAny();          // the first game another player hosts, from the backend (as the Play tab does)
void Status();           // logs the connections
void SetSkipInitialDormant(bool skip);  // `netdormancy on|off`
void Inspect(const std::string& part);
void InspectDestroyed(const std::string& part);  // `netdestroyed <name part>`: the destroyed placed actors on record
void ListLevels();       // `levels`: the streaming levels and their state
void Bring();            // `bring`: the joined players' pawns beside the host's
void Walk(int seconds);  // `walk [seconds]`: the local pawn walks forward (movement tests)
void Build();            // `build`: a wood wall in front of the local pawn, as the build key places it (tests)
void OnWorldTick();      // from world-tick events: the walk
std::vector<uintptr_t> JoinedControllers();  // when hosting: the joined players' controllers (live ones)
std::vector<uintptr_t> PlayerPawns();  // the local player's pawn and, when hosting, every joined player's (live ones)  // `netactor <name part>`: why matching actors are (not) sent to each player
void Advance(uint64_t now);   // the joined players' pawns (game thread, zone ticks)
void AdvanceClient(uint64_t now);  // joined to a host: the zone's tiles get loaded (game thread, every tick)
}

namespace zone {
bool Install();         // FortGameMode's PreInitializeComponents and ChoosePlayerStart vtable entries
uintptr_t SpawnSpot();  // the fallback spawn spot chosen in this world, or 0
}

namespace atlas {
bool Install();      // hooks the SpawnAtPlacementActorsAsArray stub
uintptr_t Goal();    // the mission's first prop spawned in this world (Close the Gate: the Atlas), or 0
}

namespace endzone {
bool Install();             // the mission manager's zone-complete slot and FortGameMode's gameplay-action slot
void Reset();               // the world changed
void OnWorldTick();         // from world-tick events only, never from UI callbacks
void OnHudZoneCompleted();  // NewHUD_C.HandleZoneCompleted ran: the results layer is up
void Finish(bool won);      // ends the zone now (testing)
void ClientCheck();          // a co-op player who joined: the host's results screen (game thread, ticks)
void ClientReturned(uintptr_t zoneWorld);  // a co-op player who joined is back in the lobby: its UI is checked
void RequestLeave();        // the results screen's leave, or straight back to the lobby
}

namespace objectives {
void Reset();         // the world changed: objectives activate afresh
bool Install();       // objective and mission vtable entries (startup)
bool PatchDeposit();  // the call in K2_RemoveItemFromPlayer (game thread)
void List();          // logs the missions and their objectives (under the lock)
void ForceSuccess();  // sets every live mission to Succeeded
}

namespace director {
bool Install();         // the GetAIDirector UFunction (startup)
bool PatchCallSites();  // the compiled-out getters' call sites (game thread)
void Reset();           // the world changed
void Check();           // drops pointers to destroyed actors
void Spawn();
bool Spawned();
bool Ready();             // the director got WorldReady: its encounter timer runs
void StartAmbient();      // the husks outside the objective: roaming patrols and the chests' guardians (once ready)
void UpdateGoals();       // gives each husk its goal: the player when close, else the Atlas (throttled; every tick)
void UpdateRoamers(bool active);  // husk groups out on the map while the zone runs (active: the mission is loaded)
void ListHusks();         // logs the husks, their goals and distances
void SpawnHusk(const std::string& arg);  // "<type> [count]" in front of the player; no type lists the types
void UseOldEncounters();  // FortGlobals.bEnableNewEncounters = false
bool ExpandNavigation();  // sizes the zone's navigation bounds; the navmesh then builds in the background
int NavigationPending();  // bounds updates, dirty areas and tiles still to build; -1 without a navigation system
}

// What the player brings from the lobby (the backend's loadout route): the hero and the profile's materials.
namespace loadout {
bool Install();                                // zone players spawn as their lobby hero's pawn class (startup)
void Reset();                                  // the world changed
void Resolve(uintptr_t pc);                    // a player's lobby hero's pawn class, once the backend answered (game thread)
void Fetch(uintptr_t pc);                      // asks the backend once per world (async)
bool Settled(uintptr_t pc);                    // a player's answer is in, or there won't be one
void Prepare(uintptr_t pc);                    // the player state's hero type and gender, before the pawn is dressed
uintptr_t Pickaxe(uintptr_t pc);               // the hero's harvest-kit pickaxe for the loadout's tier, or 0
bool GiveMaterials(uintptr_t pc);              // the profile's ingredients and wood/stone/metal; false if none came
bool GiveBackpack(uintptr_t pc);               // what the player carried out of the last zone; false if nothing
void Save(uintptr_t pc, bool autosave = false);  // the backpack and the materials used or gathered go back
void AutoSave(uintptr_t pc, uint64_t now);     // every minute in a zone (closing the game never saves)
void Grant(uintptr_t pc, uintptr_t pawn, bool items);  // the unlocked trait kits (and their items on the first setup)
void ApplyStats(uintptr_t pc, uintptr_t pawn);  // the hero card's stats: health, shield, damage (once per ability system)
}

namespace inventory {
bool Install();  // the schematic item's craft slot and the Zone controller's drop slot (startup)
}

// Server rules the client lacks: editing pieces still being built (with the reasons a piece can't be edited logged),
// and environment damage (falls).
namespace gameplay {
bool Install();         // CanBeEdited in the building classes' vtables (startup)
void PatchCallSites();  // the environment damage helper's callers (game thread)
}

namespace weapons {
bool Install();  // the weapons' reload/equip FX slot: the local player's sounds play where they can be heard (startup)
}

// The lobby's Play tab: missions chosen there open here (the backend's matchmaking answers no one).
namespace lobby {
bool Install();                // wraps FortMatchmakingContext.StartMatchmaking (startup)
void Advance(uint64_t now);    // every tick, in the lobby too: picks up the chosen zone and travels
std::wstring MissionInfo();    // the chosen mission's FortMissionInfo path for the zone being opened, or ""
void ForgetMission();          // loaded: later zones use the default mission again
void PlayFromTab(const std::string& part);
void Outpost(int n);                        // `outpost [1-4]`: the outpost zone (the storm shield base)
void PlayMission(const std::string& arg);   // `playmission <generator> [zone]`: that mission at once (tests)  // `lobbyplay [generator name part]`: Play on the Play tab (tests)
}

namespace player {
void Reset();
bool Setup(uintptr_t pc, uintptr_t pawn);
void ClientCheats(uintptr_t pc);
void ListItems();                 // `items`: this copy's player's inventory, held weapon's clip, cheat flags  // a joined player's copy: the host's cheats on its own controller too (every tick)
bool ReleaseStasis(uintptr_t pawn);  // true if the pawn was in stasis
void OnPickup(uintptr_t effect);
void OnExecuteItem(uintptr_t pc, const void* guid);
void OnHudEnterState();
void Give(const std::wstring& path, int count, bool everyone = false);  // `give <path> [count] [all]`
void GrantAbilities();  // grants jump, sprint, interact and consumables even if the pawn has abilities
bool ForceInGameUI();
void EnsureTeam(uintptr_t pc);                 // the player on the HumanCampaign team (mission props check it)
void ApplyHero(uintptr_t pc, uintptr_t pawn);  // dresses the pawn as the player's hero
void FixEffectMeshes(uintptr_t pawn);          // re-masters the weapon effect overlays (cheap; every tick)
int UiState();                                                // UI manager CurrentState (0 Invalid, 6 InGame), -1 none
int RemoveItems(uintptr_t pc, uintptr_t definition, int count);  // returns how many were removed
// Adds `count` of an item definition to the world inventory (quickbar 0 primary, 1 secondary, -1 inventory only).
uintptr_t GiveItem(uintptr_t pc, uintptr_t definition, int count, int quickbar, int slot, bool refresh = true);
uintptr_t Receive(uintptr_t pc, uintptr_t definition, int count);  // like a pickup: a weapon takes the next free slot
int CountItems(uintptr_t pc, uintptr_t definition);                // how many the world inventory holds
struct HeldItem {
    uintptr_t definition;
    int count;
    int slot;  // primary quickbar slot, -1 if none
};
std::vector<HeldItem> Items(uintptr_t pc);  // the world inventory's items
bool ExecuteByName(uintptr_t pc, const std::string& part, bool drop = false);  // `equip`/`drop <name part>`
// Takes up to `count` (all if 0) from the item with this GUID; returns how many, and the item's definition.
int RemoveItem(uintptr_t pc, const void* guid, int count, uintptr_t* definition);
void AfterDrop(uintptr_t pc, uintptr_t definition, const void* guid);  // the dropped weapon leaves the hands (the pickaxe comes out)
bool SpawnPickup(uintptr_t pc, uintptr_t definition, int count);  // tossed in front of the player's pawn
bool GrantAbility(uintptr_t pawn, uintptr_t abilityClass);  // one gameplay ability class, through the granting effect
// Points the gadget's items at the pawn's spec of its granted ability, so the quickbar can fire it.
bool LinkGadget(uintptr_t pc, uintptr_t pawn, uintptr_t gadget, uintptr_t abilityClass);
// An attribute's base value through the pawn's ability system (setClass a script path, e.g. FortHealthSet's).
bool SetAttribute(uintptr_t pawn, const char* setClass, const char* attribute, float value);
std::string StatLine(uintptr_t pc, uintptr_t pawn);  // health, shield and the hero stats, for the log
void FillHealth(uintptr_t pawn);                     // health and shield to their maximums
void EditCheck();  // `editcheck`: what keeps the building pieces near the player from being edited
}
