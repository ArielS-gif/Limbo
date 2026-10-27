#pragma once
#include <string>

// Step 2: watch what the game does on its own when it loads a zone.
//
// Hooks UObject::ProcessEvent and logs, to gameserver.log, every call where the function or the object's class
// belongs to the mission machinery (game mode, game state, world/mission manager, AI director, zone classes).
// Every function's first call also goes to seen.log. Commands are read from command.txt in `dir` (one per line,
// run on the game thread, file deleted after reading):
//   travel <url>   PlayerController::SwitchLevel(url), e.g. travel /Game/Maps/Zones/Zone_Temperate_Forest
//   pc <Function> [0|1]  calls a (Fort)PlayerController function on the local player, optionally with a bool
//   cheat <Function> [text]  calls a (Fort)CheatManager function (spawning a cheat manager first if needed)
//   console <command>  runs a console command as the local player, e.g. console flushlog
//   missiongen <class path>  writes a mission generator into the zone's world record (where world/validate's
//                  answer goes), e.g. missiongen /Game/World/MissionGens/MissionGen_1Gate.MissionGen_1Gate_C
//   missiongen-next <class path>  the same for the next zone loaded, written as soon as its world record exists
//   natives        writes re\natives.txt and re\classes.txt (native function and vtable addresses, for tools\re.js)
//   load <path>    StaticLoadObject, e.g. load /Game/Blueprints/Adventures/Gate/GateWithBluGlo.GateWithBluGlo
//   call <object> <function path> [args]  ProcessEvent with typed args o:<object> i:<int> f:<float> b:<0|1> s:<text>
//   set <object> <offset> <typed value> / get <object> <offset> [dwords]   write / read a field
//   actors <class name part>  live instances with their location
//   watch <word>   also logs calls whose function path or object class contains the word (e.g. watch Reload)
// Objects: gm, gs, pc, wm, mm, world, 0x<address>, or a path (e.g. /Script/FortniteGame.Default__FortGameMode).
// Mission commands (game.h):
//   play           travels from the lobby into Close the Gate in Stonewood
//   fix [<name> on|off]  lists or switches the pieces: zone, player, atlas, objectives, director, autostart, ui,
//                  cheats, endzone, results
//   mission [path]  FortMissionLibrary.LoadMission in a zone (default /Game/Blueprints/Adventures/Gate/GateWithBluGlo...)
//   objectives     logs the missions and their objectives' status;  win  sets every live mission to Succeeded
//   endzone [win|lose]  shows the results now;  leave (or lobby)  goes back to the lobby; a click on the results
//                  screen leaves too
//   director       spawns the AI director now;  nav  sizes the navigation bounds and rebuilds the navmesh
//   husks          logs the husks the DLL gives goals to: where they are, their goal and how far it is
//   spawnhusk <type> [count]  spawns husks 6 m in front of the player (husk, fire, pitcher, smasher, ...; no type
//                  lists them)
//   setup, abilities, ui, hero, team  re-runs the player setup / grants the abilities / forces the in-game UI /
//                  re-dresses the pawn as the hero / puts the player on the team;  give <item path> [count]  adds an
//                  item, e.g. give /Game/Weapons/WeaponItemData/Shotguns/... 1
//   state          logs the world, game mode, game state, player flags and the mission-related actors that exist
//   summary        writes summary.txt: call counts of every function seen so far
//   dump           step 1's dump (names, objects, classes) into dump\
// When the world changes, `state` runs by itself 3 s and 15 s later. Changes in what
// GameMode.ReadyToStartMatch answers are logged as they happen.
// The same commands can be typed in the game's console as `cheatscript <command>` (game.h).
namespace trace {
bool Start(const std::wstring& dir);
void Execute(const std::string& line);  // runs one command line as if read from command.txt
}
