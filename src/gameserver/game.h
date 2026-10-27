#pragma once
#include <cstdint>
#include <string>

// Step 4, the mission server: makes a zone loaded with ?zoneid=... play its mission in this client, whose server
// code is mostly compiled out. The pieces (each can be switched off at runtime with `fix <name> off`):
//   zone        the zone's game mode spawns the Zone game state, player controller and player state, which the HUD
//               (and so the loading screen), the AI director and the mission UI need
//   player      the player's quickbars, starting items, abilities and stats; equipping; picking things up
//   atlas       FortMission.SpawnAtPlacementActorsAsArray: places mission props (the Atlas) on placement actors
//   objectives  FortObjectiveBase Activate/StartPlaying/StopObjective, and the BluGlo deposit
//   director    spawns the AI director and goal manager and points the compiled-out getters at them
//   autostart   loads the zone's mission (GateWithBluGlo for now) once the zone and the player are ready
//   ui          forces the in-game UI if the HUD has not come up 10 s after the player setup
//   cheats      free building, infinite ammo and 100 BluGlo, for testing
namespace game {

void Install();  // at startup, before any zone loads

// ProcessEvent calls the mission code reacts to are recognised once per UFunction, by path (0 = none).
int EventFor(const std::string& functionPath);
// Both run on the game thread from the ProcessEvent hook, without the tracer's lock, before the original call.
void OnEvent(int event, uintptr_t object, void* params);
void Tick();

// Commands from command.txt (run under the tracer's lock); true if `cmd` was one of the mission commands.
bool Command(const std::string& cmd, const std::string& arg);

}  // namespace game
