#pragma once
#include <cstdint>
#include <functional>
#include <string>

// The Fortify-Alpha backend's local-only routes for this DLL (src/routes/ot65.js there). Requests run on a worker
// thread; the callback runs there too, so it must not call into the engine.
namespace backend {
void Post(const std::wstring& path, const std::string& json, std::function<void(int status, const std::string& body)> done);
void Get(const std::wstring& path, std::function<void(int status, const std::string& body)> done);
std::string AccountId(uintptr_t pc);      // the player's account id from its player state's unique net id, or ""
std::string ZoneInstanceId();             // the game mode's zone instance id (the ?zoneid JSON), or ""
std::string JsonField(const std::string& json, const char* name);  // a string field of a flat JSON object, or ""
}
