/*
 * core.cpp
 * C++ / Win32 port of the xlauncher core (no VCL).
 * Licensed under GPL v3. See LICENSE.
 */

#include "core.h"

#include <ws2tcpip.h>
#include <wininet.h>
#include <vector>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cctype>
#include <memory>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "shell32.lib")

namespace net {

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

static bool              g_winsock = false;
static SOCKET            g_socket = INVALID_SOCKET;
static bool              g_shutdown = false;

static std::mutex        g_cmdLock;
static std::deque<QuerySpec> g_pendingSpecs;   // hostname resolution requests

static std::mutex        g_outputLock;
static std::vector<QueryResult> g_drain;

static std::mutex        g_dnsLock;
static std::map<std::string, std::string> g_dnsCache; // lower host -> ip

static std::mutex        g_pingLock;
static std::map<DWORD, LONGLONG> g_pingT0;
static DWORD             g_pingToken = 0;

static HANDLE            g_wakeEvent = nullptr;
static WSAEVENT          g_recvEvent = WSA_INVALID_EVENT;
static HANDLE            g_netThread = nullptr;
static DWORD             g_perfFreq = 0;

// runtime-launched worker objects
static std::vector<HANDLE> g_workers; // master download & game launch handle (kept for cleanup)
static std::atomic<bool>  g_waitingMaster{false};

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

bool IsNumericIP(const std::string &s)
{
    return inet_addr(s.c_str()) != INADDR_NONE || s == "255.255.255.255";
}

static std::string Lower(std::string s)
{
    for (auto &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

std::string Trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b])) b++;
    while (e > b && isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

bool HasUnsafeCommandCharacters(const std::string &s)
{
    for (unsigned char c : s)
        if (c <= 32 || c == '"')
            return true;
    return false;
}

bool IsValidEndpoint(const std::string &address, int port)
{
    if (address.empty() || address.size() > 253)
        return false;
    if (address.find('#') != std::string::npos ||
        address.find('/') != std::string::npos)
        return false;
    if (HasUnsafeCommandCharacters(address))
        return false;
    return port >= 1 && port <= 65535;
}

// ---------------------------------------------------------------------------
// ANSI <-> UTF-8
// ---------------------------------------------------------------------------

std::string AnsiToUtf8(const std::string &ansi)
{
    if (ansi.empty())
        return std::string();
    int wLen = MultiByteToWideChar(CP_ACP, 0, ansi.data(), (int)ansi.size(),
                                   nullptr, 0);
    if (wLen <= 0)
        return ansi;
    std::wstring wstr(wLen, 0);
    MultiByteToWideChar(CP_ACP, 0, ansi.data(), (int)ansi.size(),
                        &wstr[0], wLen);
    int uLen = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), wLen,
                                   nullptr, 0, nullptr, nullptr);
    if (uLen <= 0)
        return ansi;
    std::string out(uLen, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), wLen, &out[0], uLen,
                        nullptr, nullptr);
    return out;
}

std::string Utf8ToAnsi(const std::string &utf8)
{
    if (utf8.empty())
        return std::string();
    int wLen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(),
                                   nullptr, 0);
    if (wLen <= 0)
        return utf8;
    std::wstring wstr(wLen, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(),
                        &wstr[0], wLen);
    int aLen = WideCharToMultiByte(CP_ACP, 0, wstr.data(), wLen,
                                   nullptr, 0, nullptr, nullptr);
    if (aLen <= 0)
        return utf8;
    std::string out(aLen, 0);
    WideCharToMultiByte(CP_ACP, 0, wstr.data(), wLen, &out[0], aLen,
                        nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------------------
// mailbox
// ---------------------------------------------------------------------------

static void PostResult(const QueryResult &r)
{
    std::lock_guard<std::mutex> lock(g_outputLock);
    g_drain.push_back(r);
}

void TakeResults(std::vector<QueryResult> &out)
{
    std::lock_guard<std::mutex> lock(g_outputLock);
    out.swap(g_drain);
}

// ---------------------------------------------------------------------------
// DNS cache
// ---------------------------------------------------------------------------

bool ResolveCached(const std::string &host, std::string &ip)
{
    if (IsNumericIP(host)) {
        ip = host;
        return true;
    }
    std::lock_guard<std::mutex> lock(g_dnsLock);
    auto it = g_dnsCache.find(Lower(host));
    if (it == g_dnsCache.end())
        return false;
    ip = it->second;
    return !ip.empty();
}

void RequestDns(const std::string &host)
{
    if (IsNumericIP(host))
        return;
    std::string ip;
    if (ResolveCached(host, ip))
        return;
    std::lock_guard<std::mutex> lock(g_cmdLock);
    for (const auto &spec : g_pendingSpecs) {
        if (Lower(spec.address) == Lower(host))
            return; // already queued
    }
    QuerySpec spec;
    spec.address = host;
    g_pendingSpecs.push_back(spec);
    if (g_wakeEvent)
        SetEvent(g_wakeEvent);
}

void ClearDnsCache()
{
    std::lock_guard<std::mutex> lock(g_dnsLock);
    g_dnsCache.clear();
}

static DWORD WINAPI DnsWorker(LPVOID param)
{
    std::string host = *static_cast<std::string*>(param);
    delete static_cast<std::string*>(param);

    QueryResult r;
    r.kind = QueryResult::DnsResolved;
    r.host = host;
    r.address = "";

    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
        for (addrinfo *p = res; p; p = p->ai_next) {
            if (p->ai_family == AF_INET) {
                sockaddr_in *sa = reinterpret_cast<sockaddr_in*>(p->ai_addr);
                char buf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
                r.address = buf;
                break;
            }
        }
        freeaddrinfo(res);
    }

    if (!r.address.empty()) {
        std::lock_guard<std::mutex> lock(g_dnsLock);
        g_dnsCache[Lower(host)] = r.address;
    }
    PostResult(r);
    return 0;
}

// ---------------------------------------------------------------------------
// UDP query
// ---------------------------------------------------------------------------

struct OutPacket {
    sockaddr_in to = {};
    int len = 0;
    char data[15] = {};
    DWORD pingToken = 0;
    bool isPing = false;
};

// outgoing queue (protected by g_cmdLock)
static std::deque<OutPacket> g_outgoing;

void EnqueueQuery(const QuerySpec &spec)
{
    std::string ip;
    if (!ResolveCached(spec.address, ip))
        return; // caller should have requested DNS first

    if (ip.size() < 7 || ip.size() > 15)
        return;

    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(spec.port);
    to.sin_addr.s_addr = inet_addr(ip.c_str());

    BYTE buf[15] = {};
    buf[0] = 'S'; buf[1] = 'A'; buf[2] = 'M'; buf[3] = 'P';

    unsigned int a[4] = {};
    sscanf(ip.c_str(), "%u.%u.%u.%u", &a[0], &a[1], &a[2], &a[3]);
    buf[4] = (BYTE)a[0];
    buf[5] = (BYTE)a[1];
    buf[6] = (BYTE)a[2];
    buf[7] = (BYTE)a[3];

    unsigned short tag = spec.tag ? spec.tag : spec.port;
    memcpy(&buf[8], &tag, 2);

    std::deque<OutPacket> packets;
    if (spec.info) {
        buf[10] = 'i';
        OutPacket p; p.to = to; p.len = 11; memcpy(p.data, buf, 11);
        packets.push_back(p);
    }
    if (spec.ping) {
        buf[10] = 'p';
        OutPacket p;
        std::lock_guard<std::mutex> lock(g_pingLock);
        do { ++g_pingToken; } while (g_pingToken == 0 ||
                                     g_pingT0.find(g_pingToken) != g_pingT0.end());
        DWORD token = g_pingToken;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        g_pingT0[token] = now.QuadPart;
        memcpy(&buf[11], &token, 4);
        p.to = to; p.len = 15; memcpy(p.data, buf, 15);
        p.pingToken = token; p.isPing = true;
        packets.push_back(p);
    }
    if (spec.players) {
        buf[10] = 'c';
        OutPacket p; p.to = to; p.len = 11; memcpy(p.data, buf, 11);
        packets.push_back(p);
    }
    if (spec.rules) {
        buf[10] = 'r';
        OutPacket p; p.to = to; p.len = 11; memcpy(p.data, buf, 11);
        packets.push_back(p);
    }
    if (packets.empty())
        return;

    std::lock_guard<std::mutex> lock(g_cmdLock);
    g_outgoing.insert(g_outgoing.end(), packets.begin(), packets.end());
    if (g_wakeEvent)
        SetEvent(g_wakeEvent);
}

static void SendPending()
{
    std::deque<OutPacket> batch;
    {
        std::lock_guard<std::mutex> lock(g_cmdLock);
        batch.swap(g_outgoing);
    }
    for (auto &p : batch) {
        int sent = sendto(g_socket, p.data, p.len, 0,
                          (sockaddr*)&p.to, sizeof(p.to));
        if (sent == SOCKET_ERROR && p.isPing) {
            std::lock_guard<std::mutex> lock(g_pingLock);
            g_pingT0.erase(p.pingToken);
        }
    }
}

static bool ParsePacket(const char *buf, int len, sockaddr_in *from,
                        QueryResult &r)
{
    if (len < 11)
        return false;
    if (memcmp(buf, "SAMP", 4) != 0)
        return false;

    char pktIP[32];
    sprintf(pktIP, "%d.%d.%d.%d",
            (BYTE)buf[4], (BYTE)buf[5], (BYTE)buf[6], (BYTE)buf[7]);
    char srcIP[32];
    inet_ntop(AF_INET, &from->sin_addr, srcIP, sizeof(srcIP));
    if (strcmp(srcIP, pktIP) != 0)
        return false;

    unsigned short tag;
    memcpy(&tag, &buf[8], 2);

    r.ip = srcIP;
    r.port = ntohs(from->sin_port);
    r.tag = tag;

    switch (buf[10]) {
    case 'p': {
        if (len != 15)
            return false;
        DWORD token;
        memcpy(&token, &buf[11], 4);
        LONGLONG start;
        {
            std::lock_guard<std::mutex> lock(g_pingLock);
            auto it = g_pingT0.find(token);
            if (it == g_pingT0.end())
                return false;
            start = it->second;
            g_pingT0.erase(it);
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG elapsed = now.QuadPart - start;
        if (elapsed < 0)
            return false;
        int ms = (int)((elapsed * 1000 + g_perfFreq / 2) / g_perfFreq);
        if (ms < 1)
            ms = 1;
        r.kind = QueryResult::Ping;
        r.pingMs = ms;
        return true;
    }
    case 'i': {
        int pos = 11;
        if (pos + 1 > len)
            return false;
        BYTE passworded;
        memcpy(&passworded, &buf[pos], 1); pos += 1;
        unsigned short players, maxPlayers;
        if (pos + 2 > len)
            return false;
        memcpy(&players, &buf[pos], 2); pos += 2;
        players = (players > 1000) ? 1000 : players;
        if (pos + 2 > len)
            return false;
        memcpy(&maxPlayers, &buf[pos], 2); pos += 2;
        maxPlayers = (maxPlayers > 1000) ? 1000 : maxPlayers;

        const int maxLens[3] = { 63, 39, 39 };
        std::string fields[3];
        for (int f = 0; f < 3; f++) {
            DWORD flen = 0;
            if (pos + 4 > len)
                return false;
            memcpy(&flen, &buf[pos], 4); pos += 4;
            if (flen > (DWORD)maxLens[f] || flen > (DWORD)(len - pos))
                return false;
            fields[f] = flen ? std::string(buf + pos, flen) : "-";
            pos += flen;
        }

        r.kind = QueryResult::Info;
        r.passworded = (passworded != 0);
        r.players = (int)(players > maxPlayers ? maxPlayers : players);
        r.maxPlayers = maxPlayers;
        r.hostName = fields[0];
        r.mode = fields[1];
        r.map = fields[2];
        return true;
    }
    case 'c': {
        int pos = 11;
        if (pos + 2 > len)
            return false;
        unsigned short count;
        memcpy(&count, &buf[pos], 2); pos += 2;
        if (count > 100)
            count = 100;
        for (int i = 0; i < count; i++) {
            if (pos >= len)
                return false;
            BYTE nl;
            memcpy(&nl, &buf[pos], 1); pos += 1;
            if (pos + nl > len)
                return false;
            PlayerInfo pl;
            pl.name = std::string(buf + pos, nl);
            pos += nl;
            if (pos + 4 > len)
                return false;
            int score;
            memcpy(&score, &buf[pos], 4); pos += 4;
            if (score > 1000000)
                score = 1000000;
            if (score < 0)
                score = 0;
            pl.score = score;
            r.playersList.push_back(pl);
        }
        r.kind = QueryResult::Players;
        return true;
    }
    case 'r': {
        int pos = 11;
        if (pos + 2 > len)
            return false;
        unsigned short count;
        memcpy(&count, &buf[pos], 2); pos += 2;
        if (count > 30)
            count = 30;
        for (int i = 0; i < count; i++) {
            if (pos >= len)
                return false;
            BYTE l;
            memcpy(&l, &buf[pos], 1); pos += 1;
            if (pos + l > len)
                return false;
            RuleInfo rule;
            rule.rule = std::string(buf + pos, l);
            pos += l;
            if (pos >= len)
                return false;
            memcpy(&l, &buf[pos], 1); pos += 1;
            if (pos + l > len)
                return false;
            rule.value = std::string(buf + pos, l);
            pos += l;
            r.rules.push_back(rule);
        }
        r.kind = QueryResult::Rules;
        return true;
    }
    }
    return false;
}

static DWORD WINAPI NetThread(LPVOID)
{
    HANDLE waiters[2] = { g_wakeEvent, (HANDLE)g_recvEvent };
    char recvBuf[2048];

    while (!g_shutdown) {
        DWORD wait = WSAWaitForMultipleEvents(2, waiters, FALSE, 100, FALSE);
        if (g_shutdown)
            break;

        // drain DNS requests -> spawn workers
        {
            std::vector<std::string> hosts;
            {
                std::lock_guard<std::mutex> lock(g_cmdLock);
                while (!g_pendingSpecs.empty()) {
                    if (g_pendingSpecs.front().address.empty()) {
                        g_pendingSpecs.pop_front();
                        continue;
                    }
                    hosts.push_back(g_pendingSpecs.front().address);
                    g_pendingSpecs.pop_front();
                }
            }
            for (auto &h : hosts) {
                HANDLE t = CreateThread(nullptr, 0, DnsWorker,
                                        new std::string(h), 0, nullptr);
                if (t)
                    CloseHandle(t);
            }
        }

        SendPending();

        // receive everything available
        while (!g_shutdown) {
            sockaddr_in from = {};
            int fromLen = sizeof(from);
            int len = recvfrom(g_socket, recvBuf, sizeof(recvBuf), 0,
                               (sockaddr*)&from, &fromLen);
            if (len == SOCKET_ERROR)
                break;
            QueryResult r;
            if (ParsePacket(recvBuf, len, &from, r))
                PostResult(r);
        }
        if (wait == WSA_WAIT_EVENT_0 + 1)
            WSAResetEvent(g_recvEvent);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// master server list (wininet, https://api.open.mp/servers)
// ---------------------------------------------------------------------------

// minimal JSON array-of-objects scanner: extracts values of "ip" keys
static bool ExtractIps(const std::string &raw, std::vector<std::string> &ips)
{
    size_t i = 0;
    bool inString = false;
    while (i < raw.size() && raw[i] != '[')
        i++;
    if (i >= raw.size() - 1)
        return false;
    i++;

    std::map<std::string, bool> seen;
    while (i < raw.size()) {
        // find next '"'
        while (i < raw.size() && raw[i] != '"')
            i++;
        if (i >= raw.size() - 1)
            break;
        i++;
        // read key
        std::string key;
        while (i < raw.size() && raw[i] != '"') {
            if (raw[i] == '\\' && i + 1 < raw.size())
                i++;
            key += raw[i];
            i++;
        }
        if (i >= raw.size())
            break;
        i++;
        // skip to ':'
        while (i < raw.size() && raw[i] != ':')
            i++;
        if (i >= raw.size())
            break;
        i++;
        // skip spaces
        while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t'))
            i++;
        if (i < raw.size() && raw[i] == '"') {
            i++;
            std::string value;
            while (i < raw.size() && raw[i] != '"') {
                if (raw[i] == '\\' && i + 1 < raw.size()) {
                    char c = raw[i + 1];
                    if (c == 'n') value += '\n';
                    else if (c == 't') value += '\t';
                    else if (c == 'r') value += '\r';
                    else value += c;
                    i += 2;
                    continue;
                }
                value += raw[i];
                i++;
            }
            if (i < raw.size())
                i++;
            if (Lower(key) == "ip" && !value.empty()) {
                std::string entry = Trim(value);
                if (!entry.empty() && seen.find(entry) == seen.end()) {
                    seen[entry] = true;
                    ips.push_back(entry);
                }
            }
        }
    }
    return true;
}

static DWORD WINAPI MasterWorker(LPVOID)
{
    QueryResult r;
    r.kind = QueryResult::MasterList;
    r.masterOk = false;

    const int MAX_RESPONSE = 16 * 1024 * 1024;

    HINTERNET hInet = InternetOpenW(
        L"Mozilla/5.0 (compatible; SA:MP v0.3.7)",
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) {
        r.masterError = "Unable to initialize the master-list connection.";
        PostResult(r);
        g_waitingMaster = false;
        return 0;
    }

    DWORD timeout = 10000;
    InternetSetOptionW(hInet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout,
                       sizeof(timeout));
    InternetSetOptionW(hInet, INTERNET_OPTION_SEND_TIMEOUT, &timeout,
                       sizeof(timeout));
    InternetSetOptionW(hInet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout,
                       sizeof(timeout));

    HINTERNET hUrl = InternetOpenUrlW(hInet, L"https://api.open.mp/servers",
        nullptr, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE |
                    INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hUrl) {
        r.masterError = "Unable to download the master server list.";
        InternetCloseHandle(hInet);
        PostResult(r);
        g_waitingMaster = false;
        return 0;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!HttpQueryInfoW(hUrl,
                        HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                        &statusCode, &statusSize, nullptr) ||
        statusCode < 200 || statusCode >= 300)
    {
        r.masterError = "The master server returned an invalid HTTP response.";
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInet);
        PostResult(r);
        g_waitingMaster = false;
        return 0;
    }

    std::string raw;
    char buffer[4096];
    bool ok = true;
    for (;;) {
        DWORD bytesRead = 0;
        if (!InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead)) {
            ok = false;
            break;
        }
        if (bytesRead == 0)
            break;
        if (raw.size() > MAX_RESPONSE - bytesRead) {
            ok = false;
            r.masterError = "The master server response is too large.";
            break;
        }
        raw.append(buffer, bytesRead);
    }
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInet);

    if (!ok) {
        if (r.masterError.empty())
            r.masterError = "Unable to read the master server response.";
        PostResult(r);
        g_waitingMaster = false;
        return 0;
    }

    if (!ExtractIps(raw, r.masterList)) {
        r.masterError = "The master server returned invalid JSON.";
        PostResult(r);
        g_waitingMaster = false;
        return 0;
    }

    // keep only well-formed host:port entries, dedupe
    std::vector<std::string> cleaned;
    std::set<std::string> seen;
    for (auto &entry : r.masterList) {
        if (cleaned.size() >= MAX_MASTER_SERVERS)
            break;
        size_t colon = entry.find(':');
        if (colon <= 1 || colon == std::string::npos || entry.size() > 300)
            continue;
        std::string host = entry.substr(0, colon);
        long port = atol(entry.c_str() + colon + 1);
        if (host.empty() || port < 1 || port > 65535)
            continue;
        if (!seen.insert(Lower(entry)).second)
            continue;
        cleaned.push_back(entry);
    }

    r.masterList.swap(cleaned);
    r.masterOk = true;
    PostResult(r);
    g_waitingMaster = false;
    return 0;
}

void StartMasterDownload()
{
    bool expected = false;
    if (!g_waitingMaster.compare_exchange_strong(expected, true))
        return;
    HANDLE t = CreateThread(nullptr, 0, MasterWorker, nullptr, 0, nullptr);
    if (!t) {
        g_waitingMaster = false;
        return;
    }
    CloseHandle(t);
}

// ---------------------------------------------------------------------------
// game launch (CreateProcess suspended + remote LoadLibraryW of samp.dll)
// ---------------------------------------------------------------------------

struct LaunchParams {
    std::wstring exe;
    std::wstring cmdline;
    std::wstring workDir;
    std::wstring sampDll;
};

static DWORD WINAPI LaunchWorker(LPVOID param)
{
    std::unique_ptr<LaunchParams> p(static_cast<LaunchParams*>(param));

    QueryResult r;
    r.kind = QueryResult::GameLaunch;
    r.launchError = LE_None;

    STARTUPINFOW si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);

    std::vector<wchar_t> cmd(p->cmdline.begin(), p->cmdline.end());
    cmd.push_back(0);

    LPVOID remotePath = nullptr;
    HANDLE remoteThread = nullptr;
    bool processCreated = false;
    SIZE_T written = 0;

    if (!CreateProcessW(p->exe.c_str(), &cmd[0], nullptr, nullptr, FALSE,
                        CREATE_NEW_PROCESS_GROUP | NORMAL_PRIORITY_CLASS |
                            CREATE_SUSPENDED,
                        nullptr, p->workDir.c_str(), &si, &pi))
    {
        r.launchError = LE_Execute;
        PostResult(r);
        return 0;
    }
    processCreated = true;

    SIZE_T dllBytes = (p->sampDll.size() + 1) * sizeof(wchar_t);
    remotePath = VirtualAllocEx(pi.hProcess, nullptr, dllBytes,
                                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        r.launchError = LE_Allocate;
        goto Finish;
    }
    if (!WriteProcessMemory(pi.hProcess, remotePath, p->sampDll.c_str(),
                            dllBytes, &written) || written != dllBytes)
    {
        r.launchError = LE_WritePath;
        goto Finish;
    }

    {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        FARPROC loadLib = kernel
            ? GetProcAddress(kernel, "LoadLibraryW") : nullptr;
        remoteThread = loadLib
            ? CreateRemoteThread(pi.hProcess, nullptr, 0,
                  reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLib),
                  remotePath, 0, nullptr)
            : nullptr;
    }
    if (!remoteThread) {
        r.launchError = LE_CreateRemoteThread;
        goto Finish;
    }

    {
        DWORD waitStarted = GetTickCount();
        for (;;) {
            DWORD w = WaitForSingleObject(remoteThread, 50);
            if (w == WAIT_OBJECT_0)
                break;
            if (w == WAIT_FAILED || GetTickCount() - waitStarted >= 10000)
                break;
        }
        DWORD exitCode = 0;
        if (!GetExitCodeThread(remoteThread, &exitCode) || exitCode == 0) {
            r.launchError = LE_LoadLibrary;
            goto Finish;
        }
    }

    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        r.launchError = LE_Resume;
        goto Finish;
    }
    processCreated = false;

Finish:
    if (processCreated) {
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    if (remoteThread)
        CloseHandle(remoteThread);
    if (remotePath)
        VirtualFreeEx(pi.hProcess, remotePath, 0, MEM_RELEASE);
    if (pi.hThread)
        CloseHandle(pi.hThread);
    if (pi.hProcess)
        CloseHandle(pi.hProcess);

    PostResult(r);
    return 0;
}

void StartGameLaunch(const std::wstring &exe, const std::wstring &cmdline,
                     const std::wstring &workDir, const std::wstring &sampDll)
{
    LaunchParams *p = new LaunchParams();
    p->exe = exe;
    p->cmdline = cmdline;
    p->workDir = workDir;
    p->sampDll = sampDll;
    HANDLE t = CreateThread(nullptr, 0, LaunchWorker, p, 0, nullptr);
    if (!t)
        delete p;
    else
        CloseHandle(t);
}

// ---------------------------------------------------------------------------
// RCON console (command line, like original AllocRconConsole)
// ---------------------------------------------------------------------------

struct RconParams {
    std::string host;
    int port;
    std::string pass;
};

static volatile LONG g_rconQuit = 0;
static SOCKET g_rconSocket = INVALID_SOCKET;
static sockaddr_in g_rconTo = {};
static std::string g_rconPass;

static void rconPrint(const char *fmt, ...)
{
    char buffer[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    buffer[sizeof(buffer) - 1] = 0;
    va_end(ap);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written;
    WriteConsoleA(hOut, buffer, (DWORD)strlen(buffer), &written, nullptr);
    WriteConsoleA(hOut, "\n", 1, &written, nullptr);
}

static BOOL WINAPI RconCtrlHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT) {
        rconPrint("Wait for the console to close.");
        InterlockedExchange(&g_rconQuit, 1);
        return TRUE;
    }
    return FALSE;
}

static void RconSendCommand(const std::string &command)
{
    if (g_rconSocket == INVALID_SOCKET || command.empty())
        return;
    int cmdLen = (int)command.size();
    int passLen = (int)g_rconPass.size();
    if (cmdLen > 0xFFFF || passLen > 0xFFFF)
        return;

    int total = 4 + 4 + 2 + 1 + 2 + passLen + 2 + cmdLen;
    std::vector<char> data(total);
    char *ptr = &data[0];

    DWORD sig = 0x504D4153; // "SAMP"
    memcpy(ptr, &sig, 4); ptr += 4;
    memcpy(ptr, &g_rconTo.sin_addr.s_addr, 4); ptr += 4;
    WORD port = g_rconTo.sin_port;
    memcpy(ptr, &port, 2); ptr += 2;
    *ptr = 'x'; ptr++;
    WORD plen = (WORD)passLen;
    memcpy(ptr, &plen, 2); ptr += 2;
    memcpy(ptr, g_rconPass.data(), passLen); ptr += passLen;
    WORD clen = (WORD)cmdLen;
    memcpy(ptr, &clen, 2); ptr += 2;
    memcpy(ptr, command.data(), cmdLen);

    sendto(g_rconSocket, &data[0], (int)(ptr - &data[0]), 0,
           (sockaddr*)&g_rconTo, sizeof(g_rconTo));
}

static volatile LONG g_rconResponse = 0;

static DWORD WINAPI RconNetPump(LPVOID)
{
    char buf[1024];
    while (InterlockedCompareExchange(&g_rconQuit, 0, 0) == 0) {
        sockaddr_in from = {};
        int fromSize = sizeof(from);
        int len = recvfrom(g_rconSocket, buf, sizeof(buf), 0,
                           (sockaddr*)&from, &fromSize);
        if (len < 13)
            continue;
        if (from.sin_addr.s_addr != g_rconTo.sin_addr.s_addr ||
            from.sin_port != g_rconTo.sin_port ||
            memcmp(buf, "SAMP", 4) != 0 || buf[10] != 'x')
            continue;
        WORD msgLen = 0;
        memcpy(&msgLen, &buf[11], 2);
        if ((int)msgLen > len - 13)
            continue;
        InterlockedExchange(&g_rconResponse, 1);
        char msg[1025];
        int clen = msgLen;
        if (clen > 1024)
            clen = 1024;
        memcpy(msg, &buf[13], clen);
        msg[clen] = 0;
        rconPrint("%s", msg);
    }
    return 0;
}

static DWORD WINAPI RconInput(LPVOID)
{
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    char buf[512];
    while (InterlockedCompareExchange(&g_rconQuit, 0, 0) == 0) {
        Sleep(50);
        if (hIn == INVALID_HANDLE_VALUE)
            break;
        DWORD read = 0;
        if (!ReadConsoleA(hIn, buf, sizeof(buf) - 1, &read, nullptr) || read == 0)
            continue;
        buf[read] = 0;
        while (read > 0 && (buf[read - 1] == '\r' || buf[read - 1] == '\n' ||
                            buf[read - 1] == ' '))
            buf[--read] = 0;
        if (read > 0 && InterlockedCompareExchange(&g_rconQuit, 0, 0) == 0)
            RconSendCommand(buf);
    }
    return 0;
}

static void RconRun(const std::string &host, int port, const std::string &pass)
{
    InterlockedExchange(&g_rconQuit, 0);
    InterlockedExchange(&g_rconResponse, 0);

    if (!AllocConsole()) {
        // console may already exist (edge case); still proceed
    }
    SetConsoleCtrlHandler(RconCtrlHandler, TRUE);

    rconPrint("");
    rconPrint(" SA:MP Command Line Remote Console Client");
    rconPrint(" ----------------------------------------");
    rconPrint(" (C) Copyright 2005-2006 SA:MP Team, v1.0");
    rconPrint("");
    rconPrint("Press Ctrl + C to exit");
    rconPrint("");

    in_addr in = {};
    unsigned long ul = inet_addr(host.c_str());
    if (ul != INADDR_NONE) {
        in.s_addr = ul;
    } else {
        addrinfo hints = {};
        hints.ai_family = AF_INET;
        addrinfo *res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
            sockaddr_in *sa = reinterpret_cast<sockaddr_in*>(res->ai_addr);
            in.s_addr = sa->sin_addr.s_addr;
            freeaddrinfo(res);
        } else {
            rconPrint("ERROR: Bad host.");
            SetConsoleCtrlHandler(RconCtrlHandler, FALSE);
            FreeConsole();
            return;
        }
    }

    ZeroMemory(&g_rconTo, sizeof(g_rconTo));
    g_rconTo.sin_family = AF_INET;
    g_rconTo.sin_port = htons((WORD)port);
    g_rconTo.sin_addr = in;
    g_rconPass = pass;

    rconPrint("Remote Console: %s:%d...", inet_ntoa(in), port);

    g_rconSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_rconSocket == INVALID_SOCKET) {
        rconPrint("ERROR: Unable to create RCON socket.");
        SetConsoleCtrlHandler(RconCtrlHandler, FALSE);
        FreeConsole();
        return;
    }
    DWORD timeout = 500;
    setsockopt(g_rconSocket, SOL_SOCKET, SO_RCVTIMEO,
               (const char*)&timeout, sizeof(timeout));

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    if (bind(g_rconSocket, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        rconPrint("ERROR: Unable to bind RCON socket.");
        closesocket(g_rconSocket);
        g_rconSocket = INVALID_SOCKET;
        SetConsoleCtrlHandler(RconCtrlHandler, FALSE);
        FreeConsole();
        return;
    }

    DWORD tid;
    HANDLE hInput = CreateThread(nullptr, 0, RconInput, nullptr, 0, &tid);
    HANDLE hPump = CreateThread(nullptr, 0, RconNetPump, nullptr, 0, &tid);
    if (!hInput || !hPump) {
        InterlockedExchange(&g_rconQuit, 1);
    }

    RconSendCommand("echo RCON admin connected.");

    const DWORD CONNECT_TIMEOUT = 3000;
    DWORD start = GetTickCount();
    while (InterlockedCompareExchange(&g_rconResponse, 0, 0) == 0 &&
           InterlockedCompareExchange(&g_rconQuit, 0, 0) == 0)
    {
        if (GetTickCount() - start > CONNECT_TIMEOUT) {
            rconPrint("");
            rconPrint("ERROR: Server did not respond. Check that RCON is enabled "
                      "and the password/port are correct.");
            InterlockedExchange(&g_rconQuit, 1);
            break;
        }
        Sleep(50);
    }

    while (InterlockedCompareExchange(&g_rconQuit, 0, 0) == 0)
        Sleep(100);

    SetConsoleCtrlHandler(RconCtrlHandler, FALSE);

    if (g_rconSocket != INVALID_SOCKET) {
        shutdown(g_rconSocket, SD_BOTH);
        closesocket(g_rconSocket);
        g_rconSocket = INVALID_SOCKET;
    }

    if (hInput) { WaitForSingleObject(hInput, INFINITE); CloseHandle(hInput); }
    if (hPump)  { WaitForSingleObject(hPump, INFINITE);  CloseHandle(hPump); }

    FreeConsole();
    g_rconPass.clear();
}

static DWORD WINAPI RconWorker(LPVOID param)
{
    std::unique_ptr<RconParams> p(static_cast<RconParams*>(param));
    RconRun(p->host, p->port, p->pass);
    return 0;
}

void StartRcon(const std::string &host, int port, const std::string &password)
{
    RconParams *p = new RconParams();
    p->host = host;
    p->port = port;
    p->pass = password;
    HANDLE t = CreateThread(nullptr, 0, RconWorker, p, 0, nullptr);
    if (!t)
        delete p;
    else
        CloseHandle(t);
}

// ---------------------------------------------------------------------------
// favorites file (USERDATA.DAT)
// ---------------------------------------------------------------------------

static bool ReadExact(HANDLE f, void *buf, DWORD size)
{
    DWORD read = 0;
    return ReadFile(f, buf, size, &read, nullptr) && read == size;
}

static bool ReadFavString(HANDLE f, std::string &value, int maxLen)
{
    int len = 0;
    if (!ReadExact(f, &len, sizeof(len)) || len < 0 || len > maxLen)
        return false;
    value.resize(len);
    return len == 0 || ReadExact(f, &value[0], (DWORD)len);
}

static bool WriteExact(HANDLE f, const void *buf, DWORD size)
{
    DWORD written = 0;
    return WriteFile(f, buf, size, &written, nullptr) && written == size;
}

static bool WriteFavString(HANDLE f, const std::string &value)
{
    int len = (int)value.size();
    if (!WriteExact(f, &len, sizeof(len)))
        return false;
    return len == 0 || WriteExact(f, value.data(), (DWORD)len);
}

bool ImportFavoritesFile(const std::wstring &path, std::vector<ServerInfo> &out)
{
    const int MAX_FAV = 100000;
    const int MAX_ADDR = 1024;
    const int MAX_HOST = 4096;
    const int MAX_PASS = 4096;
    const LONGLONG MAX_SIZE = 64LL * 1024 * 1024;

    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;

    char tag[4];
    int version = 0;
    int count = 0;
    bool ok = ReadExact(f, tag, 4) &&
              memcmp(tag, "SAMP", 4) == 0 &&
              ReadExact(f, &version, 4) &&
              version == FAVORITES_FILE_VERSION &&
              ReadExact(f, &count, 4) &&
              count >= 0 && count <= MAX_FAV;

    if (ok) {
        LARGE_INTEGER size = {};
        GetFileSizeEx(f, &size);
        if (size.QuadPart > MAX_SIZE || size.QuadPart < count * 20LL)
            ok = false;
    }

    std::vector<ServerInfo> imported;
    if (ok) {
        for (int i = 0; i < count; i++) {
            ServerInfo s;
            std::string addr;
            if (!ReadFavString(f, addr, MAX_ADDR) ||
                !ReadExact(f, &s.port, sizeof(s.port)) ||
                !ReadFavString(f, s.hostName, MAX_HOST) ||
                !ReadFavString(f, s.serverPassword, MAX_PASS) ||
                !ReadFavString(f, s.rconPassword, MAX_PASS))
            {
                ok = false;
                break;
            }
            if (!IsValidEndpoint(addr, s.port)) {
                ok = false;
                break;
            }
            s.address = addr;
            s.ping = 9999;
            s.tag = (unsigned short)(GetTickCount() & 0xFFFF);
            imported.push_back(s);
        }
    }

    CloseHandle(f);
    if (!ok)
        return false;
    out.swap(imported);
    return true;
}

bool ExportFavoritesFile(const std::wstring &path,
                         const std::vector<ServerInfo> &servers,
                         bool exportPasswords,
                         bool saveServerPass,
                         bool saveRconPass)
{
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;

    int version = FAVORITES_FILE_VERSION;
    int count = (int)servers.size();
    bool ok = WriteExact(f, "SAMP", 4) &&
              WriteExact(f, &version, sizeof(version)) &&
              WriteExact(f, &count, sizeof(count));

    for (int i = 0; ok && i < count; i++) {
        const ServerInfo &s = servers[i];
        ok = WriteFavString(f, s.address) &&
             WriteExact(f, &s.port, sizeof(s.port)) &&
             WriteFavString(f, s.hostName) &&
             WriteFavString(f, exportPasswords && saveServerPass
                                  ? s.serverPassword : std::string()) &&
             WriteFavString(f, exportPasswords && saveRconPass
                                  ? s.rconPassword : std::string());
    }

    if (ok)
        ok = FlushFileBuffers(f) != FALSE;
    CloseHandle(f);

    if (!ok ||
        !MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// init / shutdown
// ---------------------------------------------------------------------------

bool Init()
{
    WSADATA wsData;
    if (WSAStartup(0x0202, &wsData) != 0)
        return false;
    g_winsock = true;

    LARGE_INTEGER freq = {};
    if (QueryPerformanceFrequency(&freq))
        g_perfFreq = (DWORD)freq.QuadPart;

    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET)
        return false;

    int rcv = 1024 * 1024;
    setsockopt(g_socket, SOL_SOCKET, SO_RCVBUF,
               (const char*)&rcv, sizeof(rcv));

    sockaddr_in s_in = {};
    s_in.sin_family = AF_INET;
    s_in.sin_addr.s_addr = INADDR_ANY;
    s_in.sin_port = 0;
    u_long nonBlocking = 1;
    if (bind(g_socket, (sockaddr*)&s_in, sizeof(s_in)) == SOCKET_ERROR ||
        ioctlsocket(g_socket, FIONBIO, &nonBlocking) == SOCKET_ERROR)
    {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return false;
    }

    g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_recvEvent = WSACreateEvent();
    if (g_wakeEvent && g_recvEvent != WSA_INVALID_EVENT)
        WSAEventSelect(g_socket, g_recvEvent, FD_READ);

    if (g_wakeEvent && g_recvEvent != WSA_INVALID_EVENT) {
        g_netThread = CreateThread(nullptr, 0, NetThread, nullptr, 0, nullptr);
        if (!g_netThread) {
            WSACloseEvent(g_recvEvent);
            g_recvEvent = WSA_INVALID_EVENT;
            CloseHandle(g_wakeEvent);
            g_wakeEvent = nullptr;
        }
    }

    if (!g_netThread) {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return false;
    }
    return true;
}

bool SocketOK()
{
    return g_socket != INVALID_SOCKET;
}

void Shutdown()
{
    g_shutdown = true;
    if (g_wakeEvent)
        SetEvent(g_wakeEvent);
    if (g_netThread) {
        WaitForSingleObject(g_netThread, 2000);
        CloseHandle(g_netThread);
        g_netThread = nullptr;
    }
    while (g_waitingMaster.load())
        Sleep(10);
    if (g_recvEvent != WSA_INVALID_EVENT) {
        WSAEventSelect(g_socket, nullptr, 0);
        WSACloseEvent(g_recvEvent);
        g_recvEvent = WSA_INVALID_EVENT;
    }
    if (g_wakeEvent) {
        CloseHandle(g_wakeEvent);
        g_wakeEvent = nullptr;
    }
    if (g_socket != INVALID_SOCKET) {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
    if (g_winsock) {
        WSACleanup();
        g_winsock = false;
    }
}

} // namespace net