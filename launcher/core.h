/*
 * core.h
 * C++ / Win32 + ImGui port of the xlauncher core (no VCL).
 * Licensed under GPL v3. See LICENSE.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <vector>
#include <atomic>

namespace net {

constexpr int    FAVORITES_FILE_VERSION = 1;
constexpr int    MAX_MASTER_SERVERS     = 100000;
constexpr DWORD  MASTER_QUERY_TIMEOUT   = 3000; // ms

struct PlayerInfo {
    std::string name;
    int         score = 0;
};

struct RuleInfo {
    std::string rule;
    std::string value;
};

struct ServerInfo {
    std::string  address;          // host or numeric IP, as authored
    std::string  dottedAddress;    // resolved numeric IP (if address was a hostname)
    int          port = 7777;
    unsigned short tag = 0;

    std::string  hostName;
    bool         passworded = false;
    int          players = 0;
    int          maxPlayers = 0;
    int          ping = 9999;      // 9999 = unknown
    std::string  mode;
    std::string  map;
    bool         queryInfoReceived = false;
    bool         queryPingReceived = false;
    bool         queryCompleted = false;

    std::string  serverPassword;
    std::string  rconPassword;

    std::vector<PlayerInfo> playersList;
    std::vector<RuleInfo>   rules;
};

enum class QueryOp { Ping, Info, Players, Rules };

enum class SortMode { HostName, Players, Ping, Mode, Map };
enum class SortDir  { Up, Down };

struct QueryResult {
    enum Kind {
        Ping,
        Info,
        Players,
        Rules,
        DnsResolved,
        MasterList,
        GameLaunch
    } kind = Ping;

    std::string    ip;             // packet source (dotted)
    unsigned short port = 0;       // packet source port
    unsigned short tag = 0;        // packet tag (bytes 8-9)

    // Info
    bool         passworded = false;
    int          players = 0;
    int          maxPlayers = 0;
    std::string  hostName;
    std::string  mode;
    std::string  map;

    // Ping
    int pingMs = 0;

    // Players / Rules
    std::vector<PlayerInfo> playersList;
    std::vector<RuleInfo>   rules;

    // DnsResolved
    std::string host;
    std::string address;

    // MasterList
    bool          masterOk = false;
    std::string   masterError;
    std::vector<std::string> masterList;

    // GameLaunch
    int launchError = 0;           // 0 = success, see LaunchError
};

enum LaunchError {
    LE_None = 0,
    LE_Execute,
    LE_Allocate,
    LE_WritePath,
    LE_CreateRemoteThread,
    LE_LoadLibrary,
    LE_Resume
};

struct QuerySpec {
    std::string    address;        // host or IP
    unsigned short port = 7777;
    unsigned short tag = 0;
    bool ping = false;
    bool info = true;
    bool players = false;
    bool rules = false;
};

// ---- lifecycle -----------------------------------------------------------
bool  Init();                 // WSAStartup + UDP socket + threads
void  Shutdown();
bool  SocketOK();

// ---- queries -------------------------------------------------------------
void  EnqueueQuery(const QuerySpec &spec);          // UI->net command
void  TakeResults(std::vector<QueryResult> &out);   // drain mailbox

// ---- DNS -----------------------------------------------------------------
bool  ResolveCached(const std::string &host, std::string &ip);
void  RequestDns(const std::string &host);
void  ClearDnsCache();

// ---- master server list ---------------------------------------------------
void  StartMasterDownload();

// ---- game launch ----------------------------------------------------------
void  StartGameLaunch(const std::wstring &exe,
                      const std::wstring &cmdline,
                      const std::wstring &workDir,
                      const std::wstring &sampDll);

// ---- RCON console ----------------------------------------------------------
void  StartRcon(const std::string &host, int port, const std::string &password);

// ---- favorites file (USERDATA.DAT compatible) --------------------------------
bool  ImportFavoritesFile(const std::wstring &path, std::vector<ServerInfo> &out);
bool  ExportFavoritesFile(const std::wstring &path,
                          const std::vector<ServerInfo> &servers,
                          bool exportPasswords,
                          bool saveServerPass,
                          bool saveRconPass);

// ---- misc helpers -----------------------------------------------------------
bool   IsNumericIP(const std::string &s);
bool   HasUnsafeCommandCharacters(const std::string &s);
bool   IsValidEndpoint(const std::string &address, int port);
std::string AnsiToUtf8(const std::string &ansi); // convert ANSI (CP_ACP) bytes -> UTF-8
std::string Utf8ToAnsi(const std::string &utf8);
std::string Trim(const std::string &s);

} // namespace net