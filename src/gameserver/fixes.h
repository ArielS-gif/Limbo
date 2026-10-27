#pragma once

// Patches for code paths the client was never meant to run as a mission server.
namespace fixes {

// World requests: AFortWorldManager fetches world/info and world/validate through the UFortMcpWorlds singleton,
// but first sets its user to an empty one, so the requests would need the 'DedicatedServer' service permission,
// which only a real dedicated server login gets ("Unable to find service permissions for server."). This keeps
// the logged-in player the frontend attached instead, so the world manager's requests run as that player.
bool Install();

}  // namespace fixes
