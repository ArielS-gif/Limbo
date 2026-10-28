#include "backend.h"
#include "engine.h"
#include "log.h"
#include "memory.h"
#include "mission.h"
#include "trace.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace net {

using engine::At;

namespace {
// ---- Hosting ----
// UWorld::Listen is compiled out of this client (00C79560 is 'xor al,al; ret 4': a ?listen URL only logs "LoadMap:
// failed to Listen"). Its steps are taken here: the engine creates the game net driver (CreateNamedNetDriver, as it
// does for a pending game), the world gets it (UWorld.NetDriver), SetWorld registers its ticks and fills its network
// object list, and the IpNetDriver listens (InitListen) with the world as the notify that handles logins and joins.
constexpr uint32_t kGEngine = 0x03267DD8;
constexpr uint32_t kCreateNamedNetDriver = 0x019802E0;  // UEngine (UWorld*, FName name, FName definition) -> bool
constexpr uint32_t kFindNamedNetDriver = 0x01984710;    // cdecl (TArray<FNamedNetDriver>*, FName) -> UNetDriver*
// The engine's DestroyNamedNetDriver for a world context (cdecl (FWorldContext&, FName)): the first driver of that name
// in its ActiveNetDrivers (FNamedNetDriver: the driver, its definition); UNetDriver.NetDriverName +0x8C.
constexpr uint32_t kDestroyNamedNetDriver = 0x019816F0;
constexpr int DriverName = 0x8C, NamedDriverSize = 8;
constexpr uint32_t kSetWorld = 0x017B4D70;              // UNetDriver (UWorld*)
constexpr int EngineWorldList = 0x730, ContextWorld = 0x1D0, ContextNetDrivers = 0x194;
constexpr int WorldNetDriver = 0x24, WorldNotify = 0x1C, WorldTimeSeconds = 0x4E0;  // WorldNotify: the FNetworkNotify base
constexpr int32_t kNameGameNetDriver = 282;
constexpr int VtInitListen = 0xF8;  // (FNetworkNotify*, FURL&, bool bReuseAddressAndPort, FString& Error) -> bool
constexpr int UrlSize = 0x80, UrlPort = 0x18;  // FURL: Protocol and Host FStrings, then Port; empty strings and options are zeros
constexpr int kDefaultPort = 7777;

// ---- Replication ----
// The server's replication loop (UNetDriver::ServerReplicateActors) is compiled out too: TickFlush (017B6E70) still
// asks IsServer() and then ticks the connections without having sent a single actor. TickFlush is wrapped here and
// does the loop's work the way the replay recorder still does it (UDemoNetDriver::TickDemoRecord, 016A5C00, and
// DemoReplicateActor, 0168BEF0): each actor of the driver's network object list that is due gets CallPreReplication,
// then for every client connection it is relevant to, a channel (CreateChannel + SetChannelActor) and ReplicateActor.
constexpr uint32_t kTickFlushEntry = 0x02BE158C, kTickFlush = 0x017B6E70;  // IpNetDriver vtable +0x11C
constexpr int DriverServerConnection = 0x5C, DriverClientConnections = 0x60, DriverWorld = 0x6C, DriverNetworkObjects = 0x274;
// UNetDriver.ReplicationFrame: an actor's properties are compared against what each connection has only once per
// value of this counter (the replay recorder bumps it with its frame number, +0x28C); the loop bumps it every frame,
// or nothing but an actor's first state would ever be sent.
constexpr int DriverReplicationFrame = 0x1DC;
// FNetworkObjectList: ActiveNetworkObjects, a TSet<TSharedPtr<FNetworkObjectInfo>> (elements of 0x10, the allocation
// bits at +0xC: inline words, the heap pointer at +0x1C, the bit count at +0x20).
constexpr int SetElementSize = 0x10, SetBitsInline = 0xC, SetBitsHeap = 0x1C, SetBitCount = 0x20;
constexpr int InfoActor = 0x0, InfoNextUpdateTime = 0x4, InfoLastReplicateTime = 0xC;  // FNetworkObjectInfo (times are doubles)
// UNetConnection: PlayerController (UPlayer), State (USOCK_Open 3), ActorChannels (a TMap keyed by TWeakObjectPtr<AActor>).
constexpr int ConnPlayerController = 0x20, ConnState = 0xCC, ConnActorChannels = 0x28F44;
// UPlayer.CurrentNetSpeed: the bytes per second a connection may send. The client asks for its configured 20000, which
// a zone's thousands of actors take minutes to fill; a hosted game allows this much.
constexpr int ConnCurrentNetSpeed = 0x24;
constexpr int32_t kNetSpeed = 200000;
constexpr int kSocketOpen = 3;
constexpr int VtConnIsNetReady = 0x11C, VtConnLevelInitialized = 0x144;  // IsNetReady(bool), ClientHasInitializedLevelFor(UObject*)
constexpr uint32_t kCreateChannel = 0x017A08D0;  // UNetConnection (EChannelType, bool bOpenedLocally, int32 index) -> UChannel*
constexpr uint32_t kWeakAssign = 0x00D9F4C0;     // FWeakObjectPtr = UObject*
constexpr uint32_t kChannelFindId = 0x01220450;  // ActorChannels' TSet::FindId (FSetElementId* out, const key*)
constexpr int ChannelPairSize = 0x14, ChannelPairValue = 0x8;
constexpr int kChannelTypeActor = 2, kReliableBuffer = 256;
constexpr uint32_t kSetChannelActor = 0x01681D70, kReplicateActor = 0x0167ED10;  // UActorChannel
constexpr int ChannelBits = 0x20, ChannelNumOutRec = 0x3C, ChannelActor = 0x4C;  // bits: OpenAcked 1, Closing 2
constexpr int VtChannelClose = 0xF4;
// Destroyed actors: UNetDriver::NotifyActorDestroyed (IpNetDriver vtable +0x134, 017AD250) kept only its property
// tracker cleanup; closing the actor's channels, the destroy messages for placed actors a client never got, and the
// network object list's entry were server code. Without them a player keeps seeing what the host destroyed (hit
// marks, picked-up loot, searched containers, harvested trees). Wrapped: an open channel is closed (the client destroys
// its copy when it closes). A destroyed placed actor (a tree, a bush, a container the zone took away as it set up) is
// also kept, as the engine's DestroyedStartupOrDormantActors: every player without a channel for it gets the engine's
// destroy message (UActorChannel::SetChannelActorForDestroy, stripped too) once its level is loaded on their side,
// players who join later too: a new channel whose one reliable bunch opens and closes it and names the actor, nothing
// else; the client finds its copy by name and destroys it as the channel closes. (Sending the dying actor through
// ReplicateActor instead writes its sub-objects, which the client can't resolve: it drops the connection.) The entry
// leaves the list (TSet::Remove(FSetElementId), 00CA7930, as the replay recorder removes dead ones).
constexpr uint32_t kNotifyDestroyedEntry = 0x02BE15A4, kNotifyDestroyed = 0x017AD250;
constexpr uint32_t kObjectListRemove = 0x00CA7930;
// FOutBunch (0xB4 here): the constructor (UChannel*, bool bClose) and the destructor's frees, as UChannel::Close
// (0166A7A0) builds its close bunch; bDormant +0x9E, bReliable +0x9F. UChannel::SendBunch is channel vtable +0x114
// (FPacketIdRange* out, FOutBunch*, bool bMerge). UNetConnection.PackageMap +0x40; UPackageMapClient::WriteObject is
// its vtable +0xEC (FArchive&, UObject* Outer, FNetworkGUID, FString Name, which it frees): the NetGUID, and the outer
// and name once (as an export) until the client has acknowledged it. The actor's NetGUID is assigned as it dies, by
// FNetGUIDCache::GetOrAssignNetGUID (017A7CC0: (FNetworkGUID* out, const UObject*)) on the driver's cache (+0x70);
// a placed actor's is static (odd).
constexpr uint32_t kOutBunchInit = 0x01667060, kBitWriterDestroy = 0x00CFD6B0, kFree = 0x00C9CAD0;
constexpr int BunchSize = 0xB4, BunchDormant = 0x9E, BunchReliable = 0x9F, BunchFreeA = 0x54, BunchFreeB = 0xA8;
constexpr int VtChannelSendBunch = 0x114, ConnPackageMap = 0x40, VtWriteObject = 0xEC;
constexpr uint32_t kGetOrAssignNetGUID = 0x017A7CC0;
constexpr int DriverGuidCache = 0x70, ObjectOuter = 0x18;
constexpr int kDestroysPerFrame = 64;
constexpr uint32_t kCallPreReplication = 0x014F0390, kIsNetStartupActor = 0x01501870;  // AActor
constexpr int VtIsNetRelevantFor = 0x1D8;  // AActor (RealViewer, ViewTarget, const FVector& SrcLocation) -> bool
constexpr int ActorBits = 0x58, ActorTearOff = 0x40, ActorRemoteRole = 0x5B, ActorNetUpdateFrequency = 0xE4;
constexpr int ActorNetDormancy = 0xC9;  // not a UPROPERTY here; AActor::FlushNetDormancy (014F7AE0) turns 4 into 2 there
constexpr uint8_t kRoleAutonomousProxy = 2, kDormInitial = 4;
// A client's moves are acknowledged (or corrected) once a frame by the pawn's movement component, as the loop's
// connection pass did through APlayerController::SendClientAdjustment: UCharacterMovementComponent::SendClientAdjustment
// (016065A0), entered through its INetworkPredictionInterface base at +0x100.
constexpr uint32_t kSendClientAdjustment = 0x016065A0;
constexpr int PawnCharacterMovement = 0x288, MovementPredictionInterface = 0x100;
// GUObjectArray: each FUObjectItem is 0xC (the object, then its flags; bit 29 PendingKill).
constexpr uint32_t kObjObjects = 0x031C3AAC, kObjNum = 0x031C3AB4;
constexpr int ObjectInternalIndex = 0x8;
constexpr uint32_t kPendingKill = 1u << 29;
// A joined player's pawn: FortGameModeZone, which would restart a late joiner, is in the FortniteServer module this
// client doesn't have; AFortGameMode::RestartPlayer (00708A90, vtable +0x3DC) is called for it instead.
constexpr int VtGameModeRestartPlayer = 0x3DC;
// A joined player's loading screen waits for its player state's bHasFinishedLoading (AFortPlayerState +0x2F0, bit 2),
// which the server sets once the client has loaded: compiled out here, so it is set when the player has a pawn.
// The controller's own flags go with it (bHasClientFinishedLoading +0x510 is the server's copy; +0x511 replicates).
constexpr int PcPlayerState = 0x254, PsLoadedBits = 0x2F0, PcClientLoaded = 0x510, PcServerLoaded = 0x511;
constexpr uint8_t kPsHasFinishedLoading = 0x04;
constexpr ULONGLONG kRestartDelayMs = 2000, kRestartRetryMs = 5000;
// The zone's tiles: each foundation replicates the level it picked (LevelToStream) and builds its streaming level on
// the client, but the "load and show" a client gets for them (AGameMode::ReplicateStreamingStatus, FortGameMode vtable
// +0x378: ClientUpdateLevelStreamingStatus for every streaming level) is sent once at join, before those streaming
// levels exist there. It is sent again 5, 10, 20 and 40 s later, then every minute (tiles bring their own foundations).
// Every 5 s for 3 minutes (44 levels a time over a VPN) stalled the host for up to 16 s in one of those sends.
constexpr int VtGameModeReplicateStreamingStatus = 0x378;
constexpr ULONGLONG kStatusFirstMs = 5000, kStatusLateMs = 60000;
// And a joined client doesn't get through its loads by itself (the levels sit with a load request pending); the
// server's world manager blocks on them, so the client does too: GameplayStatics.FlushLevelStreaming every 2 s.
constexpr ULONGLONG kClientFlushMs = 2000;

// Joining: AFortGameSession::ValidatePlayer (007B5770: (const FUniqueNetIdRepl&, bool bFromPreLogin, FText& Error) ->
// bool) wants a reservation made through the matchmaking beacon ("Validation Failure: No reservation in game..
// kicking"); here players join by address, so a hosted game lets them in. Its two calls are pointed at this check.
constexpr uint32_t kValidatePlayer = 0x007B5770;
constexpr uint32_t kValidatePlayerCalls[] = { 0x007ABB10, 0x007ABCD5 };

// A joined player has no MCP profile group on the host (querying another account's profile is a dedicated-server
// request, compiled out: "FATAL: Should not be possible to issue dedicated server commands without WITH_SERVER_CODE"),
// so the controller code reached from RestartPlayer (00876E20) casts a null profile and calls 00833C30 on it. That
// call is skipped without one.
constexpr uint32_t kProfileApply = 0x00833C30, kProfileApplyCall = 0x00876FBC;

// The game is announced to the backend while it is hosted (ot65/session), so the Play tab of another copy finds it.
constexpr ULONGLONG kAnnounceMs = 15000;
constexpr size_t kMaxJoined = 3;  // four players in all, as in the game's own matchmaking

uintptr_t g_driver = 0, g_driverWorld = 0;  // the game net driver this DLL made listen, and its world
// The zone's game net driver from its first tick, before it listens: the world doesn't use it yet (it would be a
// listen server while it generates, which the zone's setup doesn't survive), but the engine tells it about every
// actor destroyed, so the placed ones the zone clears as it sets up are on the record joining players get.
uintptr_t g_recordDriver = 0, g_recordContext = 0;
int g_port = 0;
struct Joiner {
    uintptr_t pc;
    ULONGLONG seen, nextRestart, nextStatus;
    uintptr_t setupPawn;           // the pawn the player setup ran for
    ULONGLONG statusGap = kStatusFirstMs;  // until the next streaming status: doubles, up to kStatusLateMs
};
constexpr ULONGLONG kHeroWaitMs = 8000;  // a joined player's loadout from the backend, before spawning without it
std::vector<Joiner> g_joiners;  // joined controllers waiting for (or given) a pawn
bool g_skipInitialDormant = true;  // `netdormancy off` sends placed actors that start dormant too (tests)
std::vector<uintptr_t> g_due;
std::vector<uint8_t> g_deferred;
ULONGLONG g_nextStatus = 0, g_nextAnnounce = 0;
bool g_open = false;  // announced to other players (once the mission is loaded)
std::string g_announced;  // the host's account the game was announced under
int g_sent = 0, g_opened = 0;

bool Live(uintptr_t object) {
    if (!object) return false;
    int32_t index = At<int32_t>(object, ObjectInternalIndex);
    if (index < 0 || index >= At<int32_t>(engine::Va(kObjNum), 0)) return false;
    uintptr_t item = At<uint32_t>(engine::Va(kObjObjects), 0) + static_cast<uintptr_t>(index) * 0xC;
    return At<uint32_t>(item, 0) == object && !(At<uint32_t>(item, 4) & kPendingKill);
}

uintptr_t FindChannel(uintptr_t connection, uintptr_t actor) {
    uint32_t key[2] = {};
    reinterpret_cast<void*(__thiscall*)(void*, uintptr_t)>(engine::Va(kWeakAssign))(key, actor);
    int32_t id = -1;
    reinterpret_cast<void*(__thiscall*)(uintptr_t, int32_t*, const void*)>(engine::Va(kChannelFindId))(
        connection + ConnActorChannels, &id, key);
    if (id < 0) return 0;
    uintptr_t pairs = At<uint32_t>(connection, ConnActorChannels);
    return pairs ? At<uint32_t>(pairs + static_cast<uintptr_t>(id) * ChannelPairSize, ChannelPairValue) : 0;
}

bool IsNetReady(uintptr_t connection) {
    return reinterpret_cast<int32_t(__thiscall*)(uintptr_t, bool)>(At<uint32_t>(At<uint32_t>(connection, 0), VtConnIsNetReady))(
               connection, false) != 0;
}

bool LevelReady(uintptr_t connection, uintptr_t actor) {
    return reinterpret_cast<bool(__thiscall*)(uintptr_t, uintptr_t)>(At<uint32_t>(At<uint32_t>(connection, 0), VtConnLevelInitialized))(
        connection, actor);
}

bool Relevant(uintptr_t actor, uintptr_t viewer, uintptr_t viewTarget, const float* location) {
    return reinterpret_cast<bool(__thiscall*)(uintptr_t, uintptr_t, uintptr_t, const float*)>(
        At<uint32_t>(At<uint32_t>(actor, 0), VtIsNetRelevantFor))(actor, viewer, viewTarget, location);
}

bool Due(uintptr_t actor) {
    if (!Live(actor)) return false;
    if (At<uint8_t>(actor, ActorRemoteRole) == 0 && !(At<uint8_t>(actor, ActorBits) & ActorTearOff)) return false;
    // Placed actors that start dormant (most of a zone's buildings) are not sent until something wakes them.
    if (g_skipInitialDormant && At<uint8_t>(actor, ActorNetDormancy) == kDormInitial &&
        reinterpret_cast<bool(__thiscall*)(uintptr_t)>(engine::Va(kIsNetStartupActor))(actor))
        return false;
    return true;
}

void CollectDue(uintptr_t list, double now) {
    g_due.clear();
    uintptr_t elements = At<uint32_t>(list, 0);
    int32_t bits = At<int32_t>(list, SetBitCount);
    uintptr_t heap = At<uint32_t>(list, SetBitsHeap);
    const uint32_t* words = reinterpret_cast<const uint32_t*>(heap ? heap : list + SetBitsInline);
    for (int32_t i = 0; elements && i < bits; i++) {
        if (!(words[i >> 5] & (1u << (i & 31)))) continue;
        uintptr_t info = At<uint32_t>(elements + static_cast<uintptr_t>(i) * SetElementSize, 0);
        if (!info || At<double>(info, InfoNextUpdateTime) > now) continue;
        if (Due(At<uint32_t>(info, InfoActor))) g_due.push_back(info);
    }
}

// The actor's entry in the network object list (the set at list+0), removed.
void RemoveFromList(uintptr_t list, uintptr_t actor) {
    uintptr_t elements = At<uint32_t>(list, 0);
    int32_t bits = At<int32_t>(list, SetBitCount);
    uintptr_t heap = At<uint32_t>(list, SetBitsHeap);
    const uint32_t* words = reinterpret_cast<const uint32_t*>(heap ? heap : list + SetBitsInline);
    for (int32_t i = 0; elements && i < bits; i++) {
        if (!(words[i >> 5] & (1u << (i & 31)))) continue;
        uintptr_t info = At<uint32_t>(elements + static_cast<uintptr_t>(i) * SetElementSize, 0);
        if (!info || At<uint32_t>(info, InfoActor) != actor) continue;
        reinterpret_cast<void(__thiscall*)(uintptr_t, int32_t)>(engine::Va(kObjectListRemove))(list, i);
        return;
    }
}

void SendAdjustment(uintptr_t pawn) {
    if (!pawn || At<uint8_t>(pawn, ActorRemoteRole) != kRoleAutonomousProxy) return;
    uintptr_t movement = At<uint32_t>(pawn, PawnCharacterMovement);
    if (!movement || !engine::IsA(movement, "CharacterMovementComponent")) return;
    reinterpret_cast<void(__thiscall*)(uintptr_t)>(engine::Va(kSendClientAdjustment))(movement + MovementPredictionInterface);
}

// One actor to one connection; false if the connection (or the channel) is too full to take it this frame.
bool ReplicateTo(uintptr_t connection, uintptr_t actor) {
    uintptr_t channel = FindChannel(connection, actor);
    if (!channel) {
        if (!IsNetReady(connection)) return false;
        channel = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t, int32_t, bool, int32_t)>(engine::Va(kCreateChannel))(
            connection, kChannelTypeActor, true, -1);
        if (!channel) return false;
        reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t)>(engine::Va(kSetChannelActor))(channel, actor);
        g_opened++;
    }
    if (At<uint8_t>(channel, ChannelBits) & 2) return true;  // closing
    if (At<int32_t>(channel, ChannelNumOutRec) >= kReliableBuffer - 1 || !IsNetReady(connection)) return false;
    if (reinterpret_cast<bool(__thiscall*)(uintptr_t)>(engine::Va(kReplicateActor))(channel)) g_sent++;
    return true;
}

void SendDestroys(uintptr_t connection);

void ReplicateActors(uintptr_t driver) {
    engine::TArray& connections = At<engine::TArray>(driver, DriverClientConnections);
    uintptr_t world = At<uint32_t>(driver, DriverWorld), list = At<uint32_t>(driver, DriverNetworkObjects);
    if (connections.num <= 0 || !world || !list) return;
    At<int32_t>(driver, DriverReplicationFrame)++;
    double now = At<float>(world, WorldTimeSeconds);
    CollectDue(list, now);
    // Longest-waiting first (never sent: 0), so a full connection still gets every actor in turn, as the engine's
    // priority sort did; otherwise the actors early in the list would take the whole budget every frame.
    std::sort(g_due.begin(), g_due.end(), [](uintptr_t a, uintptr_t b) {
        return At<double>(a, InfoLastReplicateTime) < At<double>(b, InfoLastReplicateTime);
    });
    for (uintptr_t info : g_due)
        reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t)>(engine::Va(kCallPreReplication))(At<uint32_t>(info, InfoActor), driver);
    g_deferred.assign(g_due.size(), 0);
    for (int32_t c = 0; c < connections.num; c++) {
        uintptr_t connection = At<uint32_t>(connections.data, c * 4);
        if (!connection || At<int32_t>(connection, ConnState) != kSocketOpen) continue;
        if (At<int32_t>(connection, ConnCurrentNetSpeed) < kNetSpeed) At<int32_t>(connection, ConnCurrentNetSpeed) = kNetSpeed;
        uintptr_t pc = At<uint32_t>(connection, ConnPlayerController);
        if (!pc) continue;  // still logging in
        uintptr_t pawn = engine::PawnOf(pc);
        uintptr_t viewTarget = pawn && Live(pawn) ? pawn : pc;
        float location[3] = {};
        engine::Location(viewTarget, location);
        SendAdjustment(pawn && Live(pawn) ? pawn : 0);
        // The player's own controller and pawn first, then everything else that is relevant to it.
        for (int pass = 0; pass < 2; pass++) {
            for (size_t i = 0; i < g_due.size(); i++) {
                uintptr_t actor = At<uint32_t>(g_due[i], InfoActor);
                bool own = actor == pc || actor == pawn;
                if (own != (pass == 0)) continue;
                if (!own && (!LevelReady(connection, actor) || !Relevant(actor, pc, viewTarget, location))) continue;
                if (!ReplicateTo(connection, actor)) g_deferred[i] = 1;
            }
        }
        SendDestroys(connection);  // then what the host destroyed that this player still has
    }
    for (size_t i = 0; i < g_due.size(); i++) {
        if (g_deferred[i]) continue;  // tried again next frame
        uintptr_t info = g_due[i], actor = At<uint32_t>(info, InfoActor);
        float frequency = At<float>(actor, ActorNetUpdateFrequency);
        At<double>(info, InfoNextUpdateTime) = now + 1.0 / (frequency > 0.01f ? frequency : 0.01f);
        At<double>(info, InfoLastReplicateTime) = now;
    }
}

int g_destroyedClosed = 0, g_destroyedSent = 0;

struct DestroyedPlaced {
    uint32_t guid;
    uintptr_t level;  // its outer
    std::string name;
};
std::vector<DestroyedPlaced> g_destroyedPlaced;
// Which of them each connection has had (by index); a new package map is a new connection.
struct ConnectionDestroys {
    uintptr_t packageMap = 0;
    std::vector<uint8_t> sent;
};
std::map<uintptr_t, ConnectionDestroys> g_connectionDestroys;

ConnectionDestroys& DestroysOf(uintptr_t connection) {
    ConnectionDestroys& state = g_connectionDestroys[connection];
    uintptr_t packageMap = At<uint32_t>(connection, ConnPackageMap);
    if (state.packageMap != packageMap) state = ConnectionDestroys{packageMap, {}};
    state.sent.resize(g_destroyedPlaced.size(), 0);
    return state;
}

// The destroy message for a placed actor this connection never had a channel for.
bool SendDestroy(uintptr_t connection, const DestroyedPlaced& destroyed) {
    uintptr_t packageMap = At<uint32_t>(connection, ConnPackageMap);
    if (!packageMap) return false;
    uintptr_t channel = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t, int32_t, bool, int32_t)>(engine::Va(kCreateChannel))(
        connection, kChannelTypeActor, true, -1);
    if (!channel) return false;
    alignas(16) uint8_t bunch[0x100] = {};
    static_assert(sizeof(bunch) >= BunchSize, "FOutBunch");
    reinterpret_cast<void(__thiscall*)(void*, uintptr_t, bool)>(engine::Va(kOutBunchInit))(bunch, channel, true);
    bunch[BunchReliable] = 1;
    bunch[BunchDormant] = 0;
    struct FStringArg { wchar_t* data; int32_t num, max; } name;  // by value, freed by WriteObject
    name.num = name.max = static_cast<int32_t>(destroyed.name.size()) + 1;
    name.data = static_cast<wchar_t*>(engine::Realloc(nullptr, name.num * sizeof(wchar_t)));
    for (int32_t i = 0; i < name.num; i++) name.data[i] = i + 1 < name.num ? static_cast<wchar_t>(destroyed.name[i]) : 0;
    reinterpret_cast<bool(__thiscall*)(uintptr_t, void*, uintptr_t, uint32_t, FStringArg)>(
        At<uint32_t>(At<uint32_t>(packageMap, 0), VtWriteObject))(packageMap, bunch, destroyed.level, destroyed.guid, name);
    uint32_t range[2] = {};
    reinterpret_cast<void*(__thiscall*)(uintptr_t, void*, void*, bool)>(At<uint32_t>(At<uint32_t>(channel, 0), VtChannelSendBunch))(
        channel, range, bunch, false);
    for (int at : {BunchFreeB, BunchFreeA})
        if (uint32_t data = *reinterpret_cast<uint32_t*>(bunch + at)) reinterpret_cast<void(__cdecl*)(uint32_t)>(engine::Va(kFree))(data);
    reinterpret_cast<void(__thiscall*)(void*)>(engine::Va(kBitWriterDestroy))(bunch);
    return true;
}

// Each frame, for each player: the destroyed placed actors they haven't had, whose level they have loaded.
void SendDestroys(uintptr_t connection) {
    ConnectionDestroys& state = DestroysOf(connection);
    int budget = kDestroysPerFrame;
    for (size_t i = 0; i < g_destroyedPlaced.size() && budget > 0; i++) {
        if (state.sent[i]) continue;
        const DestroyedPlaced& destroyed = g_destroyedPlaced[i];
        if (!Live(destroyed.level)) {
            state.sent[i] = 1;  // the level is gone (and the actor with it, there too)
            continue;
        }
        if (!LevelReady(connection, destroyed.level)) continue;
        if (!IsNetReady(connection)) return;
        if (!SendDestroy(connection, destroyed)) return;
        state.sent[i] = 1;
        g_destroyedSent++;
        budget--;
    }
}

void ForgetActor(uintptr_t driver, uintptr_t actor) {
    if (uintptr_t list = At<uint32_t>(driver, DriverNetworkObjects)) RemoveFromList(list, actor);
    if (At<uint8_t>(actor, ActorRemoteRole) == 0) return;  // never replicated
    size_t placed = SIZE_MAX;
    uintptr_t cache = At<uint32_t>(driver, DriverGuidCache), level = At<uint32_t>(actor, ObjectOuter);
    if (cache && level && reinterpret_cast<bool(__thiscall*)(uintptr_t)>(engine::Va(kIsNetStartupActor))(actor)) {
        uint32_t guid = 0;
        reinterpret_cast<void*(__thiscall*)(uintptr_t, uint32_t*, uintptr_t)>(engine::Va(kGetOrAssignNetGUID))(cache, &guid, actor);
        if ((guid & 1) && guid != 1) {
            placed = g_destroyedPlaced.size();
            g_destroyedPlaced.push_back({guid, level, engine::Name(actor)});
        }
    }
    engine::TArray& connections = At<engine::TArray>(driver, DriverClientConnections);
    for (int32_t c = 0; c < connections.num; c++) {
        uintptr_t connection = At<uint32_t>(connections.data, c * 4);
        if (!connection || At<int32_t>(connection, ConnState) != kSocketOpen || !At<uint32_t>(connection, ConnPlayerController)) continue;
        uintptr_t channel = FindChannel(connection, actor);
        if (!channel) continue;  // a placed one is sent its destroy message by SendDestroys
        g_destroyedClosed++;
        if (!(At<uint8_t>(channel, ChannelBits) & 2))
            reinterpret_cast<void(__thiscall*)(uintptr_t)>(At<uint32_t>(At<uint32_t>(channel, 0), VtChannelClose))(channel);
        if (placed != SIZE_MAX) DestroysOf(connection).sent[placed] = 1;  // closing it destroys it there
    }
}

void __fastcall NotifyActorDestroyed(uintptr_t driver, void*, uintptr_t actor, bool seamless) {
    if (driver && (driver == g_driver || driver == g_recordDriver) && actor && !seamless && !At<uint32_t>(driver, DriverServerConnection) &&
        Live(driver))
        ForgetActor(driver, actor);
    reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t, bool)>(engine::Va(kNotifyDestroyed))(driver, actor, seamless);
}

void __fastcall TickFlush(uintptr_t driver, void*, float delta) {
    if (driver == g_driver && !At<uint32_t>(driver, DriverServerConnection) && Live(driver)) ReplicateActors(driver);
    reinterpret_cast<void(__thiscall*)(uintptr_t, float)>(engine::Va(kTickFlush))(driver, delta);
}

bool __fastcall ValidatePlayer(uintptr_t session, void*, const void* id, bool fromPreLogin, void* error) {
    if (Hosting()) {
        bool room = JoinedControllers().size() < kMaxJoined;
        if (!room) LOG("net: the game is full (%zu joined); a player is turned away", kMaxJoined);
        return room;
    }
    return reinterpret_cast<bool(__thiscall*)(uintptr_t, const void*, bool, void*)>(engine::Va(kValidatePlayer))(session, id,
                                                                                                             fromPreLogin, error);
}

void __fastcall ProfileApply(uintptr_t profile, void*, uintptr_t argument) {
    if (profile) reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t)>(engine::Va(kProfileApply))(profile, argument);
}

uintptr_t WorldContext(uintptr_t world) {
    uintptr_t gEngine = At<uint32_t>(engine::Va(kGEngine), 0);
    engine::TArray& contexts = At<engine::TArray>(gEngine, EngineWorldList);
    for (int32_t i = 0; contexts.data && i < contexts.num; i++) {
        uintptr_t context = At<uint32_t>(contexts.data, i * 4);
        if (context && At<uint32_t>(context, ContextWorld) == world) return context;
    }
    return 0;
}

uintptr_t FindDriver(uintptr_t context) {
    return reinterpret_cast<uintptr_t(__cdecl*)(uintptr_t, int32_t, int32_t)>(engine::Va(kFindNamedNetDriver))(
        context + ContextNetDrivers, kNameGameNetDriver, 0);
}

// The record driver of a zone that never listened (the engine only shuts down the drivers a world uses): taken out of
// the world context, if it is still the first game net driver there.
void DropRecordDriver() {
    uintptr_t driver = g_recordDriver, context = g_recordContext;
    g_recordDriver = g_recordContext = 0;
    if (!driver || driver == g_driver || !context) return;
    engine::TArray& drivers = At<engine::TArray>(context, ContextNetDrivers);
    for (int32_t i = 0; drivers.data && i < drivers.num; i++) {
        uintptr_t named = At<uint32_t>(drivers.data, i * NamedDriverSize);
        if (!named || At<int32_t>(named, DriverName) != kNameGameNetDriver || At<int32_t>(named, DriverName + 4) != 0) continue;
        if (named != driver) return;
        reinterpret_cast<void(__cdecl*)(uintptr_t, int32_t, int32_t)>(engine::Va(kDestroyNamedNetDriver))(context, kNameGameNetDriver, 0);
        LOG("net: the unused game net driver is gone");
        return;
    }
}
}  // namespace

void Prepare() {
    uintptr_t world = engine::World(), gEngine = At<uint32_t>(engine::Va(kGEngine), 0);
    uintptr_t context = world ? WorldContext(world) : 0;
    if (!gEngine || !context || At<uint32_t>(world, WorldNetDriver) || g_recordDriver) return;
    if (FindDriver(context)) {
        LOG("net: a game net driver is already there; the zone's setup isn't recorded");
        return;
    }
    bool created = reinterpret_cast<bool(__thiscall*)(uintptr_t, uintptr_t, int32_t, int32_t, int32_t, int32_t)>(
        engine::Va(kCreateNamedNetDriver))(gEngine, world, kNameGameNetDriver, 0, kNameGameNetDriver, 0);
    g_recordDriver = created ? FindDriver(context) : 0;
    g_recordContext = g_recordDriver ? context : 0;
    LOG("net: %s", g_recordDriver ? "the zone's setup is recorded for joining players (its destroyed placed actors)"
                                    : "no game net driver for the zone's setup");
}

// Chat and presence (XMPP): the backend's server takes the game's PLAIN login without TLS, but libjingle's
// PlainSaslHandler::ChooseBestSaslMechanism (024E86E0) offers PLAIN only on an encrypted stream or with allow_plain
// (+0x50), which the game leaves off: "XMPP log-in credentials not valid (6)". Its jne past both checks becomes a jmp.
constexpr uint32_t kSaslPlainCheck = 0x024E86E8;
constexpr uint8_t kJne = 0x75, kJmp = 0xEB;

bool AllowChatLogin() {
    bool plain = engine::PatchByte(kSaslPlainCheck, kJne, kJmp);
    LOG("net: chat login %s", plain ? "allowed without TLS (the backend's XMPP server)" : "NOT patched");
    return plain;
}

// "Remember me": the next start logs in with the saved refresh token as the game starts, while the login screen's own
// service-status question (asked as the application, before anyone is logged in) is still out. The status answer's
// handler (007DBAA0) takes any answer that arrives during a login as the login's own service check and calls
// ContinueLoggingIn(2), which finds the login still in its first step (1) and aborts it ("unexpected logout or state
// change"); the game then forgets the saved token. An answer in that first step is ignored here: once logged in, the
// login asks for the service status itself.
constexpr uint32_t kContinueLoggingIn = 0x007C46B0;      // UFortOnlineAccount (expected step), thiscall, ret 4
constexpr uint32_t kStatusContinueCall = 0x007DBB54;     // its call in the status answer's handler
constexpr int OnlineAccountLoginStep = 0x10C;
constexpr int kStepLoggingIn = 1;

void __fastcall StatusContinueLoggingIn(uintptr_t account, void*, int expected) {
    if (At<int32_t>(account, OnlineAccountLoginStep) == kStepLoggingIn) {
        LOG("net: a service status answer arrived while still logging in (a remembered login): left for the login's own check");
        return;
    }
    reinterpret_cast<void(__thiscall*)(uintptr_t, int)>(engine::Va(kContinueLoggingIn))(account, expected);
}

bool AllowRememberedLogin() {
    bool ok = engine::PatchCall(kStatusContinueCall, kContinueLoggingIn, reinterpret_cast<void*>(&StatusContinueLoggingIn));
    LOG("net: remembered logins %s", ok ? "no longer abort on an early service status answer" : "NOT patched");
    return ok;
}

bool Install() {
    bool destroyed = engine::PatchPointer(kNotifyDestroyedEntry, kNotifyDestroyed, reinterpret_cast<void*>(&NotifyActorDestroyed));
    LOG("net: IpNetDriver.NotifyActorDestroyed %s", destroyed ? "wrapped (players see what the host destroyed)" : "NOT wrapped");
    bool hooked = engine::PatchPointer(kTickFlushEntry, kTickFlush, reinterpret_cast<void*>(&TickFlush));
    LOG("net: IpNetDriver.TickFlush %s", hooked ? "wrapped (actor replication for hosted games)" : "NOT wrapped");
    return hooked;
}

void PatchCallSites() {
    int done = 0;
    for (uint32_t site : kValidatePlayerCalls) done += engine::PatchCall(site, kValidatePlayer, reinterpret_cast<void*>(&ValidatePlayer));
    LOG("net: %d of %zu player validation calls let joined players in while hosting", done, std::size(kValidatePlayerCalls));
    bool guarded = engine::PatchCall(kProfileApplyCall, kProfileApply, reinterpret_cast<void*>(&ProfileApply));
    LOG("net: the profile call in a joined controller's setup is %s", guarded ? "guarded" : "NOT guarded");
}

// The hosted game's entry in the backend (players: the host and the joined), or its removal.
void Announce(const std::string& account, bool closed) {
    if (account.empty()) return;
    std::string json = "{\"accountId\":\"" + account + "\",\"port\":" + std::to_string(g_port ? g_port : kDefaultPort) +
                       ",\"players\":" + std::to_string(1 + JoinedControllers().size()) + ",\"zone\":\"" +
                       backend::JsonField(backend::ZoneInstanceId(), "theaterMissionId") + "\"" + (closed ? ",\"closed\":true}" : "}");
    backend::Post(L"/fortnite/api/game/v2/ot65/session", json, [closed](int status, const std::string& body) {
        if (status != 200) LOG("net: the backend didn't take the %s: %d %s", closed ? "game's end" : "game's announcement", status,
                               body.substr(0, 200).c_str());
    });
}

void Reset() {
    if (!g_announced.empty()) Announce(g_announced, true);  // the hosted world is gone
    g_announced.clear();
    g_nextAnnounce = 0;
    g_open = false;
    DropRecordDriver();
    g_driver = g_driverWorld = 0;
    g_port = 0;
    g_joiners.clear();
    g_destroyedPlaced.clear();
    g_connectionDestroys.clear();
}

void Open() {
    g_open = true;
}

bool Hosting() {
    return g_driver && g_driverWorld == engine::World() && Live(g_driver);
}

bool IsClient() {
    uintptr_t world = engine::World();
    uintptr_t driver = world ? At<uint32_t>(world, WorldNetDriver) : 0;
    return driver && At<uint32_t>(driver, DriverServerConnection) != 0;
}

bool Host(int port) {
    uintptr_t world = engine::World();
    if (!world) return false;
    if (Hosting()) {
        LOG("net: already hosting on port %d", g_port);
        return true;
    }
    if (At<uint32_t>(world, WorldNetDriver)) {
        LOG("net: this world already has a net driver (%s)", engine::Describe(At<uint32_t>(world, WorldNetDriver)).c_str());
        return false;
    }
    uintptr_t gEngine = At<uint32_t>(engine::Va(kGEngine), 0), context = WorldContext(world);
    if (!gEngine || !context) {
        LOG("net: no engine or world context");
        return false;
    }
    uintptr_t driver = g_recordDriver && g_recordContext == context && Live(g_recordDriver) ? g_recordDriver : 0;
    if (!driver) {
        bool created = reinterpret_cast<bool(__thiscall*)(uintptr_t, uintptr_t, int32_t, int32_t, int32_t, int32_t)>(
            engine::Va(kCreateNamedNetDriver))(gEngine, world, kNameGameNetDriver, 0, kNameGameNetDriver, 0);
        driver = created ? FindDriver(context) : 0;
    }
    if (!driver) {
        LOG("net: CreateNamedNetDriver(GameNetDriver) failed");
        return false;
    }
    At<uint32_t>(world, WorldNetDriver) = static_cast<uint32_t>(driver);
    reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t)>(engine::Va(kSetWorld))(driver, world);
    alignas(8) uint8_t url[UrlSize] = {};
    At<int32_t>(reinterpret_cast<uintptr_t>(url), UrlPort) = port > 0 ? port : kDefaultPort;
    engine::TArray error = {};
    bool listening = reinterpret_cast<bool(__thiscall*)(uintptr_t, uintptr_t, void*, bool, void*)>(
        At<uint32_t>(At<uint32_t>(driver, 0), VtInitListen))(driver, world + WorldNotify, url, false, &error);
    if (!listening) {
        LOG("net: InitListen failed: %ls", error.data ? reinterpret_cast<const wchar_t*>(error.data) : L"(no reason)");
        At<uint32_t>(world, WorldNetDriver) = 0;
        if (error.data) engine::Free(reinterpret_cast<void*>(error.data));
        return false;
    }
    g_driver = driver;
    g_driverWorld = world;
    g_port = At<int32_t>(reinterpret_cast<uintptr_t>(url), UrlPort);
    uintptr_t list = At<uint32_t>(driver, DriverNetworkObjects);
    LOG("net: hosting: %s listens on UDP port %d; %d network object(s) to replicate", engine::Describe(driver).c_str(), g_port,
        list ? At<int32_t>(list, 4) : -1);
    return true;
}

// `join` without an address: the first game another player hosts, as the Play tab finds it (ot65/sessions).
std::mutex g_lookupLock;
std::string g_lookupAddress;

void JoinAny() {
    std::string account = backend::AccountId(engine::LocalPlayerController());
    backend::Get(L"/fortnite/api/game/v2/ot65/sessions?accountId=" + std::wstring(account.begin(), account.end()),
                 [](int status, const std::string& body) {
                     std::string address = status == 200 ? backend::JsonField(body, "address") : "";
                     if (address.empty()) {
                         LOG("net: no game to join (the backend answered %d %s)", status, body.substr(0, 120).c_str());
                         return;
                     }
                     std::lock_guard<std::mutex> guard(g_lookupLock);
                     g_lookupAddress = address;
                 });
}

void Join(const std::string& address) {
    std::string target = address.find(':') == std::string::npos ? address + ":" + std::to_string(kDefaultPort) : address;
    trace::Execute("console open " + target);
}

void Status() {
    if (!Hosting()) {
        LOG("net: %s", IsClient() ? "connected to a host as a client" : "not hosting");
        return;
    }
    engine::TArray& connections = At<engine::TArray>(g_driver, DriverClientConnections);
    uintptr_t list = At<uint32_t>(g_driver, DriverNetworkObjects);
    LOG("net: hosting on port %d: %d connection(s), %d network object(s), %d channel(s) opened, %d actor update(s) sent; "
        "destroyed: %d channel(s) closed; %zu placed actor(s) destroyed, %d destroy message(s) sent",
        g_port, connections.num, list ? At<int32_t>(list, 4) : -1, g_opened, g_sent, g_destroyedClosed, g_destroyedPlaced.size(), g_destroyedSent);
    for (int32_t c = 0; c < connections.num; c++) {
        uintptr_t connection = At<uint32_t>(connections.data, c * 4);
        uintptr_t pc = connection ? At<uint32_t>(connection, ConnPlayerController) : 0;
        LOG("  %s: state %d, controller %s, pawn %s", engine::Describe(connection).c_str(),
            connection ? At<int32_t>(connection, ConnState) : -1, engine::Describe(pc).c_str(),
            engine::Describe(pc ? engine::PawnOf(pc) : 0).c_str());
    }
}

void SetSkipInitialDormant(bool skip) {
    g_skipInitialDormant = skip;
    LOG("net: placed actors that start dormant are %s", skip ? "not sent until woken" : "sent like the rest");
}

void InspectDestroyed(const std::string& part) {
    int shown = 0;
    for (size_t i = 0; i < g_destroyedPlaced.size(); i++) {
        const DestroyedPlaced& d = g_destroyedPlaced[i];
        if (d.name.find(part) == std::string::npos) continue;
        std::string sent;
        for (auto& [connection, state] : g_connectionDestroys)
            sent += " " + std::string(i < state.sent.size() && state.sent[i] ? "sent" : "not sent");
        LOG("net: destroyed %s in %s, NetGUID %u%s%s", d.name.c_str(), Live(d.level) ? engine::Describe(d.level).c_str() : "(level gone)",
            d.guid, sent.empty() ? "" : ";", sent.c_str());
        if (++shown >= 20) break;
    }
    LOG("net: %d of %zu destroyed placed actor(s) named like \"%s\" shown", shown, g_destroyedPlaced.size(), part.c_str());
}

void Inspect(const std::string& part) {
    if (!Hosting()) {
        LOG("net: not hosting");
        return;
    }
    uintptr_t list = At<uint32_t>(g_driver, DriverNetworkObjects), world = At<uint32_t>(g_driver, DriverWorld);
    double now = At<float>(world, WorldTimeSeconds);
    engine::TArray& connections = At<engine::TArray>(g_driver, DriverClientConnections);
    uintptr_t elements = At<uint32_t>(list, 0);
    int32_t bits = At<int32_t>(list, SetBitCount);
    uintptr_t heap = At<uint32_t>(list, SetBitsHeap);
    const uint32_t* words = reinterpret_cast<const uint32_t*>(heap ? heap : list + SetBitsInline);
    int shown = 0;
    for (int32_t i = 0; elements && i < bits && shown < 12; i++) {
        if (!(words[i >> 5] & (1u << (i & 31)))) continue;
        uintptr_t info = At<uint32_t>(elements + static_cast<uintptr_t>(i) * SetElementSize, 0);
        uintptr_t actor = info ? At<uint32_t>(info, InfoActor) : 0;
        if (!actor || !Live(actor) || engine::Name(actor).find(part) == std::string::npos) continue;
        shown++;
        LOG("net: %s: next update in %.1f s, dormancy %d, remote role %d, due %d", engine::Name(actor).c_str(),
            At<double>(info, InfoNextUpdateTime) - now, At<uint8_t>(actor, ActorNetDormancy), At<uint8_t>(actor, ActorRemoteRole),
            Due(actor));
        for (int32_t c = 0; c < connections.num; c++) {
            uintptr_t connection = At<uint32_t>(connections.data, c * 4);
            uintptr_t pc = connection ? At<uint32_t>(connection, ConnPlayerController) : 0;
            if (!pc) continue;
            uintptr_t pawn = engine::PawnOf(pc);
            uintptr_t viewTarget = pawn && Live(pawn) ? pawn : pc;
            float location[3] = {};
            engine::Location(viewTarget, location);
            uintptr_t channel = FindChannel(connection, actor);
            LOG("  to %s: level ready %d, relevant %d, channel %s", engine::Name(pc).c_str(), LevelReady(connection, actor),
                Relevant(actor, pc, viewTarget, location), engine::Describe(channel).c_str());
        }
    }
    LOG("net: %d network object(s) named like \"%s\" shown", shown, part.c_str());
}

// `levels`: the streaming levels of this world (the zone's tiles): the world asset's path (TAssetPtr, its path FString
// at +0x30), bShouldBeLoaded/bShouldBeVisible (+0x90 bits 0x20/0x40), the state bits at +0x98, and what follows.
void ListLevels() {
    int shown = 0;
    for (uintptr_t level : engine::FindAllA("LevelStreaming")) {
        if (!Live(level)) continue;
        uintptr_t text = At<uint32_t>(level, 0x30);
        std::wstring path = text && At<int32_t>(level, 0x34) > 0 ? reinterpret_cast<const wchar_t*>(text) : L"";
        uint8_t flags = At<uint8_t>(level, 0x90);
        std::string after;
        for (int offset = 0xB0; offset < 0xE0; offset += 4) {
            uint32_t value = At<uint32_t>(level, offset);
            char text[96];
            snprintf(text, sizeof(text), " %X=%08X%s", offset, value, value > 0x10000 && mem::Readable(value, 0x10) && Live(value) ? (" (" + engine::Name(value) + ")").c_str() : "");
            after += text;
        }
        LOG("  %s %ls: load %d visible %d, bits %02X;%s", engine::Name(level).c_str(), path.c_str(), (flags & 0x20) != 0,
            (flags & 0x40) != 0, At<uint8_t>(level, 0x98), after.c_str());
        shown++;
    }
    LOG("net: %d streaming level(s)", shown);
}

// Test helpers. `bring` (host): every joined player's pawn 3 m beside the host's. `walk <seconds>`: the local pawn walks
// forward, through the same AddMovementInput as the keyboard (on a client its moves go to the host as ServerMove).
ULONGLONG g_walkUntil = 0;

void Bring() {
    uintptr_t host = engine::PawnOf(engine::LocalPlayerController());
    float at[3];
    if (!Hosting() || !host || !engine::Location(host, at)) {
        LOG("net: bring needs a hosted game and the host's pawn");
        return;
    }
    float side = 300.0f;
    for (auto& joiner : g_joiners) {
        uintptr_t pawn = Live(joiner.pc) ? engine::PawnOf(joiner.pc) : 0;
        if (!pawn || !Live(pawn)) continue;
        float spot[3] = { at[0] + side, at[1], at[2] + 50.0f };
        side += 200.0f;
        bool moved = engine::Teleport(pawn, spot);
        LOG("net: %s %s beside the host", engine::Name(pawn).c_str(), moved ? "moved" : "could not be moved");
    }
}

// `build`: a wood wall 4 m in front of the local pawn, through ServerCreateBuildingActor as the build key sends it
// (FBuildingClassData {class, previous level, upgrade level}, BuildLoc, BuildRot, bMirrored), on the 512-unit grid.
void Build() {
    uintptr_t pc = engine::LocalPlayerController(), pawn = engine::PawnOf(pc);
    float at[3], forward[3] = {};
    if (!pawn || !engine::Location(pawn, at)) {
        LOG("net: build needs the local pawn");
        return;
    }
    engine::ProcessEvent(pawn, engine::FindFunction("/Script/Engine.Actor.GetActorForwardVector"), forward);
    struct {
        uintptr_t buildingClass;
        int32_t previousLevel, upgradeLevel;
        float location[3], rotation[3];
        bool mirrored;
        uint8_t pad[3];
    } p{};
    p.buildingClass = engine::Load(L"/Game/Building/ActorBlueprints/Player/Wood/L1/PBWA_W1_Solid.PBWA_W1_Solid_C");
    bool alongX = std::fabs(forward[1]) > std::fabs(forward[0]);  // facing along Y: the wall runs along X
    float x = at[0] + forward[0] * 400.0f, y = at[1] + forward[1] * 400.0f;
    p.location[0] = std::floor(x / 512.0f) * 512.0f + (alongX ? 256.0f : 0.0f);
    p.location[1] = std::floor(y / 512.0f) * 512.0f + (alongX ? 0.0f : 256.0f);
    p.location[2] = at[2] - 90.0f;
    p.rotation[1] = alongX ? 0.0f : 90.0f;
    engine::ProcessEvent(pc, engine::FindFunction("/Script/FortniteGame.FortPlayerController.ServerCreateBuildingActor"), &p);
    LOG("net: build %s at (%.0f, %.0f, %.0f), yaw %.0f", engine::Name(p.buildingClass).c_str(), p.location[0], p.location[1],
        p.location[2], p.rotation[1]);
}

void Walk(int seconds) {
    g_walkUntil = GetTickCount64() + static_cast<ULONGLONG>(seconds > 0 ? seconds : 3) * 1000;
    LOG("net: walking forward for %d s", seconds > 0 ? seconds : 3);
}

void OnWorldTick() {
    if (!g_walkUntil) return;
    if (GetTickCount64() > g_walkUntil) {
        g_walkUntil = 0;
        return;
    }
    uintptr_t pawn = engine::PawnOf(engine::LocalPlayerController());
    if (!pawn || !Live(pawn)) return;
    struct {
        float direction[3];
        float scale;
        bool force;
        uint8_t pad[3];
    } p{};
    engine::ProcessEvent(pawn, engine::FindFunction("/Script/Engine.Actor.GetActorForwardVector"), p.direction);
    p.scale = 1.0f;
    p.force = true;
    engine::ProcessEvent(pawn, engine::FindFunction("/Script/Engine.Pawn.AddMovementInput"), &p);
    static ULONGLONG lastReport = 0;
    if (GetTickCount64() - lastReport > 1000) {  // what a client's pawn makes of the input, once a second
        lastReport = GetTickCount64();
        struct { bool value; uint8_t pad[3]; } local{}, ignored{};
        struct { float xyz[3]; } velocity{};
        engine::ProcessEvent(pawn, engine::FindFunction("/Script/Engine.Pawn.IsLocallyControlled"), &local);
        uintptr_t controller = engine::LocalPlayerController();
        engine::ProcessEvent(controller, engine::FindFunction("/Script/Engine.Controller.IsMoveInputIgnored"), &ignored);
        engine::ProcessEvent(pawn, engine::FindFunction("/Script/Engine.Actor.GetVelocity"), &velocity);
        uintptr_t movement = At<uint32_t>(pawn, PawnCharacterMovement);
        LOG("net: walk: %s locally controlled %d, move input ignored %d, role %d, movement mode %d, velocity (%.0f, %.0f, %.0f)",
            engine::Name(pawn).c_str(), local.value, ignored.value, At<uint8_t>(pawn, 0xC8), movement ? At<uint8_t>(movement, 0x120) : -1,
            velocity.xyz[0], velocity.xyz[1], velocity.xyz[2]);
    }
}

std::vector<uintptr_t> JoinedControllers() {
    std::vector<uintptr_t> controllers;
    if (!Hosting()) return controllers;
    for (const Joiner& joiner : g_joiners)
        if (Live(joiner.pc)) controllers.push_back(joiner.pc);
    return controllers;
}

std::vector<uintptr_t> PlayerPawns() {
    std::vector<uintptr_t> pawns;
    uintptr_t local = engine::PawnOf(engine::LocalPlayerController());
    if (local && Live(local)) pawns.push_back(local);
    if (Hosting()) {
        for (const Joiner& joiner : g_joiners) {
            uintptr_t pawn = Live(joiner.pc) ? engine::PawnOf(joiner.pc) : 0;
            if (pawn && Live(pawn) && pawn != local) pawns.push_back(pawn);
        }
    }
    return pawns;
}

void AdvanceClient(ULONGLONG now) {
    std::string join;
    {
        std::lock_guard<std::mutex> guard(g_lookupLock);
        join.swap(g_lookupAddress);
    }
    if (!join.empty()) {
        LOG("net: joining the game at %s", join.c_str());
        Join(join);
    }
    static ULONGLONG next = 0;
    static uintptr_t clientWorld = 0;  // the host's zone this copy was in, while joined
    bool client = IsClient();
    uintptr_t world = engine::World();
    if (clientWorld && !client && world != clientWorld) {  // back from the host's zone (sent home, or the host left)
        endzone::ClientReturned(clientWorld);
        clientWorld = 0;
    }
    if (client) {
        clientWorld = world;
        endzone::ClientCheck();
        player::ClientCheats(engine::LocalPlayerController());
    }
    if (now < next || !client) return;
    next = now + kClientFlushMs;
    uintptr_t pc = engine::LocalPlayerController();
    uintptr_t statics = engine::FindObject("/Script/Engine.Default__GameplayStatics", "GameplayStatics");
    uintptr_t flush = engine::FindFunction("/Script/Engine.GameplayStatics.FlushLevelStreaming");
    if (!pc || !statics || !flush) return;
    uintptr_t context = pc;
    engine::ProcessEvent(statics, flush, &context);
}

void Advance(ULONGLONG now) {
    if (!Hosting()) return;
    engine::TArray& connections = At<engine::TArray>(g_driver, DriverClientConnections);
    uintptr_t gameMode = engine::GameMode();
    // A player who left (their connection gone, their controller destroyed) is forgotten at once: once the garbage
    // collector has freed the controller, its address can hold another object, and the host's Leave called
    // ClientReturnToMainMenu on that (a crash, seconds after a friend went back to the lobby).
    std::vector<uintptr_t> connected;
    for (int32_t c = 0; c < connections.num; c++) {
        uintptr_t connection = At<uint32_t>(connections.data, c * 4);
        if (connection) connected.push_back(At<uint32_t>(connection, ConnPlayerController));
    }
    for (auto joiner = g_joiners.begin(); joiner != g_joiners.end();) {
        if (std::find(connected.begin(), connected.end(), joiner->pc) != connected.end() && Live(joiner->pc)) {
            ++joiner;
            continue;
        }
        LOG("net: a joined player left (controller 0x%08X)", static_cast<unsigned>(joiner->pc));
        loadout::Forget(joiner->pc);
        player::Forget(joiner->pc);
        joiner = g_joiners.erase(joiner);
    }
    for (int32_t c = 0; c < connections.num; c++) {
        uintptr_t connection = At<uint32_t>(connections.data, c * 4);
        uintptr_t pc = connection ? At<uint32_t>(connection, ConnPlayerController) : 0;
        if (!pc || !Live(pc)) continue;
        auto joiner = std::find_if(g_joiners.begin(), g_joiners.end(), [pc](const Joiner& j) { return j.pc == pc; });
        if (joiner == g_joiners.end()) {
            g_joiners.push_back({ pc, now, now + kRestartDelayMs, now + kStatusFirstMs, 0 });
            LOG("net: %s joined", engine::Describe(pc).c_str());
            continue;
        }
        if (gameMode && now >= joiner->nextStatus) {
            joiner->nextStatus = now + joiner->statusGap;
            joiner->statusGap = (std::min)(joiner->statusGap * 2, kStatusLateMs);
            reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t)>(
                At<uint32_t>(At<uint32_t>(gameMode, 0), VtGameModeReplicateStreamingStatus))(gameMode, pc);
        }
        // The player's lobby loadout (hero, gadgets, materials, backpack) from the backend, by their account; the hero's
        // pawn class must be known before the pawn spawns.
        loadout::Fetch(pc);
        bool heroKnown = loadout::Settled(pc) || now - joiner->seen > kHeroWaitMs;
        if (heroKnown) loadout::Resolve(pc);
        uintptr_t pawn = engine::PawnOf(pc);
        if (pawn && Live(pawn)) {
            if (joiner->setupPawn != pawn) {  // the same setup as the host's own player: kit, hero, abilities, stats
                joiner->setupPawn = pawn;
                At<uint8_t>(pc, PcClientLoaded) = 1;
                At<uint8_t>(pc, PcServerLoaded) = 1;
                if (game::g_on.player) player::Setup(pc, pawn);
            }
            player::ReleaseStasis(pawn);  // the local player's is released by the zone's own step
            uintptr_t state = At<uint32_t>(pc, PcPlayerState);
            if (state && !(At<uint8_t>(state, PsLoadedBits) & kPsHasFinishedLoading)) {
                At<uint8_t>(state, PsLoadedBits) |= kPsHasFinishedLoading;
                LOG("net: %s marked as loaded", engine::Name(pc).c_str());
            }
            if (game::g_on.player) loadout::AutoSave(pc, now);
        } else if (gameMode && heroKnown && now >= joiner->nextRestart) {
            joiner->nextRestart = now + kRestartRetryMs;
            reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t)>(At<uint32_t>(At<uint32_t>(gameMode, 0), VtGameModeRestartPlayer))(
                gameMode, pc);
            pawn = engine::PawnOf(pc);
            LOG("net: RestartPlayer(%s) -> pawn %s", engine::Name(pc).c_str(), engine::Describe(pawn).c_str());
        }
    }
    if (g_open && now >= g_nextAnnounce) {
        g_nextAnnounce = now + kAnnounceMs;
        std::string account = backend::AccountId(engine::LocalPlayerController());
        if (!account.empty()) {
            if (g_announced.empty()) LOG("net: the game is open to other players (announced to the backend)");
            g_announced = account;
            Announce(account, false);
        }
    }
    if (now >= g_nextStatus && connections.num > 0) {
        g_nextStatus = now + 30000;
        Status();
    }
}

}  // namespace net
