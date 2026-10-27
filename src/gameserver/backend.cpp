#include "backend.h"
#include "engine.h"
#include "log.h"
#include "memory.h"
#include <windows.h>
#include <winhttp.h>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace backend {

using engine::At;

namespace {
// The backend: 127.0.0.1:3551, or the "host:port" in backend.txt next to this DLL (a co-op player on another PC
// points it at the host's backend, as their patch pak points the game there).
constexpr wchar_t kDefaultHost[] = L"127.0.0.1";
constexpr INTERNET_PORT kDefaultPort = 3551;

struct Address {
    std::wstring host = kDefaultHost;
    INTERNET_PORT port = kDefaultPort;
};

const Address& BackendAddress() {
    static Address address = [] {
        Address a;
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&BackendAddress), &self);
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(self, path, MAX_PATH);
        std::wstring file = path;
        file = file.substr(0, file.find_last_of(L'\\') + 1) + L"backend.txt";
        FILE* f = _wfopen(file.c_str(), L"r");
        if (!f) return a;
        char line[256] = {};
        if (fgets(line, sizeof(line), f)) {
            std::string text = line;
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
            size_t colon = text.rfind(':');
            std::string host = colon == std::string::npos ? text : text.substr(0, colon);
            if (!host.empty()) a.host.assign(host.begin(), host.end());
            if (colon != std::string::npos) a.port = static_cast<INTERNET_PORT>(atoi(text.c_str() + colon + 1));
        }
        fclose(f);
        LOG("backend: %ls:%u (backend.txt)", a.host.c_str(), a.port);
        return a;
    }();
    return address;
}
constexpr int PcPlayerState = 0x254;
constexpr int PlayerStateUniqueId = 0x280;     // FUniqueNetIdRepl: TSharedPtr<const FUniqueNetId>, object first
constexpr int NetIdString = 0xC;               // FUniqueNetIdString: vtable, TSharedFromThis (8), then the FString
constexpr int GameModeZoneInstanceId = 0x2F4;  // AFortGameMode, FString

// An FString (data, num with the terminator, max) as ASCII; anything else becomes '?'.
std::string ReadFString(uintptr_t at) {
    uintptr_t data = mem::ReadOr<uint32_t>(at);
    int32_t num = mem::ReadOr<int32_t>(at + 4);
    if (!data || num <= 1 || num > 8192) return "";
    std::vector<wchar_t> text(num);
    if (!mem::Copy(text.data(), data, num * sizeof(wchar_t))) return "";
    std::string out;
    for (int32_t i = 0; i < num - 1 && text[i]; i++) out.push_back(text[i] < 0x80 ? static_cast<char>(text[i]) : '?');
    return out;
}

int Request(const wchar_t* verb, const std::wstring& path, const std::string& body, std::string& response) {
    int status = -1;
    HINTERNET session = WinHttpOpen(L"OTGameServer", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    const Address& backend = BackendAddress();
    HINTERNET connection = session ? WinHttpConnect(session, backend.host.c_str(), backend.port, 0) : nullptr;
    HINTERNET request = connection ? WinHttpOpenRequest(connection, verb, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    if (request) WinHttpSetTimeouts(request, 3000, 3000, 5000, 10000);
    const wchar_t* headers = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : L"Content-Type: application/json\r\n";
    DWORD headerLength = body.empty() ? 0 : static_cast<DWORD>(-1L);
    void* data = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    if (request && WinHttpSendRequest(request, headers, headerLength, data, static_cast<DWORD>(body.size()),
                                      static_cast<DWORD>(body.size()), 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code,
                            &size, WINHTTP_NO_HEADER_INDEX);
        status = static_cast<int>(code);
        for (DWORD available = 0; WinHttpQueryDataAvailable(request, &available) && available > 0;) {
            std::vector<char> chunk(available);
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
            response.append(chunk.data(), read);
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return status;
}
}  // namespace

void Post(const std::wstring& path, const std::string& json, std::function<void(int, const std::string&)> done) {
    std::thread([path, json, done] {
        std::string response;
        int status = Request(L"POST", path, json, response);
        if (done) done(status, response);
    }).detach();
}

void Get(const std::wstring& path, std::function<void(int, const std::string&)> done) {
    std::thread([path, done] {
        std::string response;
        int status = Request(L"GET", path, "", response);
        if (done) done(status, response);
    }).detach();
}

std::string AccountId(uintptr_t pc) {
    uintptr_t ps = pc ? mem::ReadOr<uint32_t>(pc + PcPlayerState) : 0;
    uintptr_t netId = ps ? mem::ReadOr<uint32_t>(ps + PlayerStateUniqueId) : 0;
    std::string id = netId ? ReadFString(netId + NetIdString) : "";
    if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos) return "";
    return id;
}

std::string ZoneInstanceId() {
    uintptr_t gameMode = engine::GameMode();
    return gameMode ? ReadFString(gameMode + GameModeZoneInstanceId) : "";
}

std::string JsonField(const std::string& json, const char* name) {
    std::string key = std::string("\"") + name + "\"";
    size_t at = json.find(key);
    if (at == std::string::npos) return "";
    at = json.find(':', at + key.size());
    if (at == std::string::npos) return "";
    size_t open = json.find('"', at + 1);
    size_t close = open == std::string::npos ? std::string::npos : json.find('"', open + 1);
    return close == std::string::npos ? "" : json.substr(open + 1, close - open - 1);
}

}  // namespace backend
