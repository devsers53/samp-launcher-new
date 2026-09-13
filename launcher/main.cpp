/*
 * main.cpp
 * xlauncher — SA:MP launcher rewritten on C++ / Win32 / Dear ImGui.
 * No VCL, builds with MinGW-w64. Licensed under GPL v3. See LICENSE.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <d3d11.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlguid.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "core.h"
#include "languages.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <set>
#include <cstdarg>
#include <algorithm>
#include <cmath>

#include <gdiplus.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")

static ID3D11Device *g_pd3dDevice = nullptr;
static ID3D11DeviceContext *g_pd3dDeviceContext = nullptr;

using Gdiplus::Bitmap;
using Gdiplus::BitmapData;
using Gdiplus::ImageLockModeRead;

struct Icon {
    int w = 0, h = 0;
    ID3D11ShaderResourceView *srv = nullptr;
};

static ULONG_PTR g_gdiplusToken = 0;
static std::map<std::string, Icon> g_icons;

static std::wstring GetIconsDir()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        dir = dir.substr(0, slash);
    static const wchar_t *kCands[] = {
        L"icons",
        L"..\\resource\\icons",
        L"..\\..\\resource\\icons",
        L"..\\..\\..\\resource\\icons",
    };
    for (const wchar_t *c : kCands) {
        std::wstring p = dir + L"\\" + c;
        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES &&
            (attr & FILE_ATTRIBUTE_DIRECTORY))
            return p;
    }
    return L"";
}

static void LoadIconFromFile(const std::wstring &path, const std::string &name)
{
    Bitmap bmp(path.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok)
        return;
    UINT iw = bmp.GetWidth();
    UINT ih = bmp.GetHeight();
    if (iw == 0 || ih == 0)
        return;

    Gdiplus::Rect rc(0, 0, (INT)iw, (INT)ih);
    BitmapData data;
    if (bmp.LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &data) !=
        Gdiplus::Ok)
        return;

    Icon ic;
    ic.w = (int)iw;
    ic.h = (int)ih;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = iw;
    td.Height = ih;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D *tex = nullptr;
    if (SUCCEEDED(g_pd3dDevice->CreateTexture2D(&td, nullptr, &tex))) {
        g_pd3dDeviceContext->UpdateSubresource(tex, 0, nullptr, data.Scan0,
                                               data.Stride, 0);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = td.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        g_pd3dDevice->CreateShaderResourceView(tex, &sd, &ic.srv);
        tex->Release();
    }
    bmp.UnlockBits(&data);
    if (ic.srv)
        g_icons[name] = ic;
}

static void IconsInit()
{
    Gdiplus::GdiplusStartupInput gsi;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gsi, nullptr) !=
        Gdiplus::Ok)
        return;

    std::wstring dir = GetIconsDir();
    if (dir.empty())
        return;

    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        std::wstring fn = fd.cFileName;
        std::string name;
        for (wchar_t ch : fn)
            name += (ch <= 0x7F) ? (char)ch : '?';
        size_t dot = name.find_last_of('.');
        if (dot != std::string::npos)
            name.erase(dot);
        if (!name.empty())
            LoadIconFromFile(dir + L"\\" + fn, name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void IconsShutdown()
{
    for (auto &kv : g_icons)
        if (kv.second.srv)
            kv.second.srv->Release();
    g_icons.clear();
    if (g_gdiplusToken) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

static const Icon *FindIcon(const std::string &name)
{
    auto it = g_icons.find(name);
    return (it != g_icons.end() && it->second.srv) ? &it->second : nullptr;
}

static void IconDrawSize(const Icon *ic, float maxSize, float &ow, float &oh)
{
    ow = (float)ic->w;
    oh = (float)ic->h;
    if (maxSize > 0.0f && (ow > maxSize || oh > maxSize)) {
        float k = maxSize / (ow > oh ? ow : oh);
        ow *= k;
        oh *= k;
    }
}

static void DrawIconImage(const Icon *ic, ImVec2 topLeft, float maxSize = 0.0f)
{
    if (!ic || !ic->srv || ic->w <= 0 || ic->h <= 0)
        return;
    float w = (float)ic->w;
    float h = (float)ic->h;
    if (maxSize > 0.0f) {
        float k = maxSize / (w > h ? w : h);
        if (k < 1.0f) {
            w *= k;
            h *= k;
        }
    }
    ImGui::GetWindowDrawList()->AddImage(ic->srv, topLeft,
                                         ImVec2(topLeft.x + w, topLeft.y + h));
}

static bool IconTextButton(const std::string &icon, const char *label)
{
    const Icon *ic = FindIcon(icon);
    ImGui::PushID(label);
    ImVec2 t = ImGui::CalcTextSize(label);
    float pad = ImGui::GetStyle().FramePadding.x;
    float icS = (ic && ic->w > 0) ? 16.0f : 0.0f;
    float w = pad * 2 + icS + (icS > 0 ? 6.0f : 0.0f) + t.x + 4.0f;
    bool pressed = ImGui::Button("##icobtn", ImVec2(w, 0));
    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    if (ic && icS > 0) {
        float y = mn.y + (mx.y - mn.y - icS) * 0.5f;
        DrawIconImage(ic, ImVec2(mn.x + pad, y), icS);
    }
    float ty = mn.y + (mx.y - mn.y - t.y) * 0.5f;
    float tx = mn.x + pad + icS + (icS > 0 ? 6.0f : 0.0f);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(tx, ty), ImGui::GetColorU32(ImGuiCol_Text), label);
    ImGui::PopID();
    return pressed;
}

static bool IconMenuItem(const std::string &icon, const char *label,
                         bool selected = false, bool enabled = true)
{
    const Icon *ic = FindIcon(icon);
    std::string padded;
    const char *text = label;
    if (ic && ic->w > 0) {
        const float ds = 16.0f;
        float sp = ImGui::CalcTextSize(" ").x;
        if (sp < 0.1f) sp = 6.0f;
        padded.assign((size_t)ceilf((ds + 10.0f) / sp), ' ');
        padded += label;
        text = padded.c_str();
    }
    bool pressed = ImGui::MenuItem(text, nullptr, selected, enabled);
    if (ic && ic->w > 0) {
        ImVec2 mn = ImGui::GetItemRectMin();
        ImVec2 mx = ImGui::GetItemRectMax();
        float iw, ih;
        IconDrawSize(ic, 16.0f, iw, ih);
        float y = mn.y + (mx.y - mn.y - ih) * 0.5f;
        DrawIconImage(ic, ImVec2(mn.x + 2, y), 16.0f);
    }
    return pressed;
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

using net::ServerInfo;
using net::SortMode;
using net::SortDir;

struct LaunchReq {
    bool active = false;
    std::string host;
    std::string port;
    std::string password;
};

struct App {
    std::vector<ServerInfo> servers;
    std::deque<net::QuerySpec> queryQueue;
    std::map<std::string, int> queuedKeys;    // "host:port#tag" -> idx
    std::map<std::string, int> lookup;        // "ip:port#tag"  -> idx
    std::set<std::string> dnsFailed;

    std::vector<int> order;
    int selectedIdx = -1;
    std::string selKey;

    int masterFile = 1;                       // 1=Internet, 0=Favorites
    bool batchActive = false;
    int inFlight = 0;
    DWORD batchDeadline = 0;
    bool masterUpdateInProgress = false;
    bool masterCancel = false;

    SortMode sortMode = SortMode::HostName;
    SortMode oldSortMode = SortMode::HostName;
    SortDir  sortDir = SortDir::Up;
    SortDir  oldSortDir = SortDir::Up;

    bool filtered = true;
    bool statusBarVisible = true;

    char filterMode[128] = {};
    char filterMap[128] = {};
    char filterLang[64] = {};
    char search[256] = {};
    bool hideFull = false;
    bool hideEmpty = false;
    bool hidePassworded = false;

    bool windowMax = false;
    RECT normalRect = {0, 0, 0, 0};

    // launcher colors (saved to samp_c.ini)
    int   theme = 0;                            // 0=Dark,1=Dark Blue,2=Dark Purple,3=Light
    float accent[3] = { 0.58f, 0.76f, 0.98f };  // accent RGB (0..1)

    char nickname[64] = {};
    std::vector<std::string> nickHistory;
    std::wstring gtaExe;

    std::string status;
    std::string toast;
    DWORD toastUntil = 0;

    // connect flow
    LaunchReq pendingConnect;
    bool connectRequested = false;
    bool connectPasswordOpen = false;
    bool connectPasswordResult = false;

    // generic input modal
    bool inputOpen = false;
    bool inputResult = false;
    int  inputKind = 0;                       // 0=add server
    char inputBuffer[512] = {};
    std::string inputTitle;
    std::string inputPrompt;

    // rcon modal
    bool rconOpen = false;
    char rconHost[512] = {};
    char rconPass[512] = {};

    // settings / props / about
    bool settingsOpen = false;
    bool propsOpen = false;
    bool aboutOpen = false;
    char propsServerPass[512] = {};
    char propsRconPass[512] = {};
};

static App g;
static bool g_serverCtxDone = false;
static bool g_running = true;
static HWND g_hwnd = nullptr;

// custom window chrome
static const int kTitleBarH = 32;   // custom title bar height
static const int kWinBtnW   = 46;   // min/max/close button width
static const int kEdgeHit   = 6;    // invisible resize edge width

// ---------------------------------------------------------------------------
// registry / folders
// ---------------------------------------------------------------------------

static std::wstring GetUserFilesPath()
{
    wchar_t path[MAX_PATH];
    if (SHGetSpecialFolderPathW(nullptr, path, CSIDL_PERSONAL, FALSE))
        return std::wstring(path) + L"\\GTA San Andreas User Files\\SAMP\\";
    return L"";
}

static std::wstring GetUserDataFile()
{
    return GetUserFilesPath() + L"USERDATA.DAT";
}

static bool EnsureUserFilesFolder()
{
    std::wstring path = GetUserFilesPath();
    if (path.empty())
        return false;
    int rc = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS ||
           rc == ERROR_FILE_EXISTS;
}

static std::wstring ReadRegString(HKEY root, const wchar_t *key,
                                 const wchar_t *name)
{
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, key, 0, KEY_READ, &h) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    LONG rc = RegQueryValueExW(h, name, nullptr, &type, (LPBYTE)buf, &size);
    RegCloseKey(h);
    if (rc != ERROR_SUCCESS || type != REG_SZ)
        return L"";
    return buf;
}

static void WriteRegString(const wchar_t *name, const std::wstring &value)
{
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\SAMP", 0, nullptr, 0,
                        KEY_WRITE, nullptr, &h, nullptr) == ERROR_SUCCESS)
    {
        RegSetValueExW(h, name, 0, REG_SZ, (const BYTE*)value.c_str(),
                       (DWORD)((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(h);
    }
}

static bool ReadRegBool(const wchar_t *name, bool def)
{
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\SAMP", 0, KEY_READ, &h)
            != ERROR_SUCCESS)
        return def;
    DWORD val, size = sizeof(val), type = 0;
    LONG rc = RegQueryValueExW(h, name, nullptr, &type, (LPBYTE)&val, &size);
    RegCloseKey(h);
    return (rc == ERROR_SUCCESS && type == REG_DWORD) ? (val != 0) : def;
}

static void WriteRegBool(const wchar_t *name, bool v)
{
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\SAMP", 0, nullptr, 0,
                        KEY_WRITE, nullptr, &h, nullptr) == ERROR_SUCCESS)
    {
        DWORD val = v ? 1 : 0;
        RegSetValueExW(h, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
        RegCloseKey(h);
    }
}

// ---------------------------------------------------------------------------
// text helpers
// ---------------------------------------------------------------------------

static std::string WideToUtf8(const std::wstring &w)
{
    if (w.empty())
        return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return std::string();
    std::string out(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &out[0], len,
                        nullptr, nullptr);
    return out;
}

static std::wstring Utf8ToWide(const std::string &s)
{
    if (s.empty())
        return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                  nullptr, 0);
    if (len <= 0)
        return std::wstring();
    std::wstring out(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], len);
    return out;
}

static void SetClipText(const std::string &utf8)
{
    if (!OpenClipboard(nullptr))
        return;
    EmptyClipboard();
    std::wstring w = Utf8ToWide(utf8);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (mem) {
        wchar_t *dst = (wchar_t*)GlobalLock(mem);
        if (dst) {
            memcpy(dst, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
    }
    CloseClipboard();
}

static std::string GetClipText()
{
    std::string result;
    if (!OpenClipboard(nullptr))
        return result;
    HANDLE mem = GetClipboardData(CF_UNICODETEXT);
    if (mem) {
        const wchar_t *src = (const wchar_t*)GlobalLock(mem);
        if (src) {
            result = WideToUtf8(src);
            GlobalUnlock(mem);
        }
    }
    CloseClipboard();
    return result;
}

static void SetStatus(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g.status = buf;
}

static void Toast(const char *msg, DWORD ms = 5000)
{
    g.toast = msg;
    g.toastUntil = GetTickCount() + ms;
}

static void CheckAnotherInstance()
{
    CreateMutexW(nullptr, FALSE, L"kyeman and spookie woz 'ere, innit.");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr,
            L"SA:MP is already running.\n\nYou can only run one instance at a time.",
            L"SA:MP Error", MB_ICONERROR);
        ExitProcess(0);
    }
}

// ---------------------------------------------------------------------------
// server list bookkeeping
// ---------------------------------------------------------------------------

static std::string SKey(const ServerInfo &s)
{
    std::string ip = s.dottedAddress.empty() ? s.address : s.dottedAddress;
    unsigned short tag = s.tag ? s.tag : (unsigned short)s.port;
    char buf[64];
    sprintf(buf, "%s:%d#%u", ip.c_str(), s.port, tag);
    return buf;
}

static void RebuildLookup()
{
    g.lookup.clear();
    for (int i = 0; i < (int)g.servers.size(); i++)
        g.lookup[SKey(g.servers[i])] = i;
}

static int FindServer(const std::string &ip, unsigned short port,
                      unsigned short tag)
{
    char buf[64];
    sprintf(buf, "%s:%d#%u", ip.c_str(), port, tag);
    auto it = g.lookup.find(buf);
    return it == g.lookup.end() ? -1 : it->second;
}

static bool ICmp(const std::string &a, const std::string &b)
{
    return stricmp(a.c_str(), b.c_str()) < 0;
}

static void SortServers()
{
    if (g.oldSortMode == g.sortMode && g.oldSortDir == g.sortDir)
        return;
    switch (g.sortMode) {
    case SortMode::HostName:
        std::sort(g.servers.begin(), g.servers.end(),
                  [](const ServerInfo &a, const ServerInfo &b) {
                      return ICmp(a.hostName, b.hostName); });
        break;
    case SortMode::Players:
        std::sort(g.servers.begin(), g.servers.end(),
                  [](const ServerInfo &a, const ServerInfo &b) {
                      return a.players < b.players; });
        break;
    case SortMode::Ping:
        std::sort(g.servers.begin(), g.servers.end(),
                  [](const ServerInfo &a, const ServerInfo &b) {
                      return a.ping < b.ping; });
        break;
    case SortMode::Mode:
        std::sort(g.servers.begin(), g.servers.end(),
                  [](const ServerInfo &a, const ServerInfo &b) {
                      return ICmp(a.mode, b.mode); });
        break;
    case SortMode::Map:
        std::sort(g.servers.begin(), g.servers.end(),
                  [](const ServerInfo &a, const ServerInfo &b) {
                      return ICmp(a.map, b.map); });
        break;
    }
    g.oldSortMode = g.sortMode;
    g.oldSortDir = g.sortDir;
}

static bool LangEq(const char *a, const char *b);

static void BuildOrder()
{
    SortServers();
    RebuildLookup();

    std::string mf = net::Trim(g.filterMode);
    std::string pf = net::Trim(g.filterMap);
    std::string lf = net::Trim(g.filterLang);
    std::string sf = net::Trim(g.search);
    for (char &c : mf) c = (char)tolower((unsigned char)c);
    for (char &c : pf) c = (char)tolower((unsigned char)c);
    for (char &c : lf) c = (char)tolower((unsigned char)c);
    for (char &c : sf) c = (char)tolower((unsigned char)c);

    std::vector<int> order;
    int first = g.sortDir == SortDir::Down ? (int)g.servers.size() - 1 : 0;
    int last  = g.sortDir == SortDir::Down ? -1 : (int)g.servers.size();
    int step  = g.sortDir == SortDir::Down ? -1 : 1;
    for (int i = first; i != last; i += step) {
        const ServerInfo &s = g.servers[i];
        bool skip = g.masterFile != 0 && s.maxPlayers < 1;
        if (g.filtered) {
            std::string m = s.mode, mp = s.map;
            for (char &c : m) c = (char)tolower((unsigned char)c);
            for (char &c : mp) c = (char)tolower((unsigned char)c);
            if (!mf.empty() && m.find(mf) == std::string::npos)
                skip = true;
            if (!pf.empty() && mp.find(pf) == std::string::npos)
                skip = true;
            if (!lf.empty()) {
                bool langOk = false;
                for (int z = 1; z < kLanguageCount; z++) {
                    if (!LangEq(g.filterLang, kLanguages[z].label))
                        continue;
                    const char *als = kLanguages[z].aliases;
                    const char *sub = als;
                    while (sub && *sub) {
                        const char *end = sub;
                        while (*end && *end != '|')
                            ++end;
                        std::string al(sub, (size_t)(end - sub));
                        for (char &c : al) c = (char)tolower((unsigned char)c);
                        if (!al.empty() &&
                            (mp == al ||
                             (al.size() >= 3 &&
                              mp.find(al) != std::string::npos))) {
                            langOk = true;
                            break;
                        }
                        sub = *end ? end + 1 : nullptr;
                    }
                    if (langOk)
                        break;
                }
                if (!langOk)
                    skip = true;
            }
            if (!sf.empty()) {
                std::string h = s.hostName, a = s.address,
                            mm = s.mode, mp2 = s.map,
                            ap = a + ":" + std::to_string(s.port);
                for (auto *t : { &h, &a, &ap, &mm, &mp2 })
                    for (char &c : *t)
                        c = (char)tolower((unsigned char)c);
                if (h.find(sf) == std::string::npos &&
                    a.find(sf) == std::string::npos &&
                    ap.find(sf) == std::string::npos &&
                    mm.find(sf) == std::string::npos &&
                    mp2.find(sf) == std::string::npos)
                    skip = true;
            }
            if (g.hideFull && s.players == s.maxPlayers)
                skip = true;
            if (g.hideEmpty && s.players == 0)
                skip = true;
            if (g.hidePassworded && s.passworded)
                skip = true;
        }
        if (!skip)
            order.push_back(i);
    }
    g.order.swap(order);

    // preserve selection
    int newSel = -1;
    if (!g.selKey.empty()) {
        for (int i : g.order)
            if (g.servers[i].address + ":" + std::to_string(g.servers[i].port)
                    == g.selKey) {
                newSel = i;
                break;
            }
    }
    if (newSel == -1 && !g.order.empty() && g.selectedIdx != -1)
        newSel = g.order[0];
    g.selectedIdx = newSel;
    if (g.selectedIdx >= 0 && g.selectedIdx < (int)g.servers.size())
        g.selKey = g.servers[g.selectedIdx].address + ":" +
                   std::to_string(g.servers[g.selectedIdx].port);
    else
        g.selKey.clear();
}

static void UpdateTotals()
{
    int servers = 0, slots = 0, players = 0;
    for (int i : g.order) {
        servers++;
        slots += g.servers[i].maxPlayers;
        players += g.servers[i].players;
    }
    if (g.masterUpdateInProgress || g.batchActive)
        SetStatus("Master list update in progress...  Servers: %d players, "
                  "playing on %d servers. (%d player slots available)",
                  players, servers, slots);
    else
        SetStatus("Servers: %d players, playing on %d servers. (%d player slots available)",
                  players, servers, slots);
}

// ---------------------------------------------------------------------------
// query scheduling
// ---------------------------------------------------------------------------

static void EnqueueServer(int idx, bool ping, bool info, bool players,
                          bool rules)
{
    ServerInfo &s = g.servers[idx];
    net::QuerySpec spec;
    spec.address = s.address;
    spec.port = (unsigned short)s.port;
    spec.tag = s.tag ? s.tag : (unsigned short)s.port;
    spec.ping = ping;
    spec.info = info;
    spec.players = players;
    spec.rules = rules;
    char key[96];
    sprintf(key, "%s:%d#%u", spec.address.c_str(), s.port, spec.tag);
    g.queuedKeys[key] = idx;
    g.queryQueue.push_back(spec);
}

static void ProcessQueryQueue(int quota)
{
    int sent = 0;
    while (!g.queryQueue.empty() && sent < quota) {
        net::QuerySpec spec = g.queryQueue.front();
        std::string ip;
        if (!net::ResolveCached(spec.address, ip)) {
            if (g.dnsFailed.count(spec.address)) {
                g.queryQueue.pop_front();
                continue;
            }
            net::RequestDns(spec.address);
            break;
        }

        g.queryQueue.pop_front();

        if (!net::IsNumericIP(spec.address)) {
            for (auto &s : g.servers)
                if (s.address == spec.address && s.port == (int)spec.port)
                    s.dottedAddress = ip;
            RebuildLookup();
        }
        spec.address = ip;
        net::EnqueueQuery(spec);
        g.inFlight++;
        sent++;
    }
}

static void CheckBatchComplete()
{
    if (!g.batchActive)
        return;
    if (!g.queryQueue.empty()) {
        g.batchDeadline = 0;
        return;
    }
    if (g.inFlight <= 0) {
        g.batchActive = false;
        g.masterUpdateInProgress = false;
        if (!g.order.empty() && g.selectedIdx == -1)
            g.selectedIdx = g.order[0];
        BuildOrder();
        UpdateTotals();
        return;
    }
    DWORD now = GetTickCount();
    if (g.batchDeadline == 0)
        g.batchDeadline = now + net::MASTER_QUERY_TIMEOUT;
    else if (now >= g.batchDeadline) {
        g.batchActive = false;
        g.masterUpdateInProgress = false;
        if (!g.order.empty() && g.selectedIdx == -1)
            g.selectedIdx = g.order[0];
        BuildOrder();
        UpdateTotals();
    }
}

// ---------------------------------------------------------------------------
// result application
// ---------------------------------------------------------------------------

static void ApplyResult(const net::QueryResult &r)
{
    switch (r.kind) {
    case net::QueryResult::Ping: {
        int idx = FindServer(r.ip, r.port, r.tag);
        if (idx < 0 || idx >= (int)g.servers.size())
            break;
        ServerInfo &s = g.servers[idx];
        s.ping = r.pingMs;
        s.queryPingReceived = true;
        if (s.queryInfoReceived && !s.queryCompleted) {
            s.queryCompleted = true;
            g.inFlight--;
        }
        break;
    }
    case net::QueryResult::Info: {
        int idx = FindServer(r.ip, r.port, r.tag);
        if (idx < 0 || idx >= (int)g.servers.size())
            break;
        ServerInfo &s = g.servers[idx];
        s.passworded = r.passworded;
        s.players = r.players;
        s.maxPlayers = r.maxPlayers;
        s.hostName = net::AnsiToUtf8(r.hostName);
        s.mode = net::AnsiToUtf8(r.mode);
        s.map = net::AnsiToUtf8(r.map);
        s.queryInfoReceived = true;
        if (s.queryPingReceived && !s.queryCompleted) {
            s.queryCompleted = true;
            g.inFlight--;
        }
        break;
    }
    case net::QueryResult::Players: {
        int idx = FindServer(r.ip, r.port, r.tag);
        if (idx < 0 || idx >= (int)g.servers.size())
            break;
        g.servers[idx].playersList = r.playersList;
        g.servers[idx].players = (int)r.playersList.size();
        break;
    }
    case net::QueryResult::Rules: {
        int idx = FindServer(r.ip, r.port, r.tag);
        if (idx < 0 || idx >= (int)g.servers.size())
            break;
        g.servers[idx].rules = r.rules;
        break;
    }
    case net::QueryResult::DnsResolved: {
        if (r.address.empty()) {
            g.dnsFailed.insert(r.host);
            if (g.pendingConnect.active &&
                g.pendingConnect.host == r.host) {
                Toast("Unable to resolve the server address.");
                g.pendingConnect.active = false;
            }
        } else {
            g.dnsFailed.erase(r.host);
        }
        break;
    }
    case net::QueryResult::MasterList: {
        g.masterUpdateInProgress = false;
        if (g.masterCancel || !r.masterOk) {
            if (!r.masterOk && !r.masterError.empty()) {
                g.toast = r.masterError;
                g.toastUntil = GetTickCount() + 6000;
            }
            break;
        }
        g.servers.clear();
        g.queryQueue.clear();
        g.queuedKeys.clear();
        g.dnsFailed.clear();
        g.batchActive = false;
        g.inFlight = 0;
        g.batchDeadline = 0;
        g.selectedIdx = -1;
        g.selKey.clear();

        for (const auto &entry : r.masterList) {
            size_t colon = entry.find(':');
            if (colon <= 1)
                continue;
            std::string addr = entry.substr(0, colon);
            int port = atoi(entry.c_str() + colon + 1);
            if (addr.empty() || port < 1 || port > 65535)
                continue;
            ServerInfo s;
            s.address = addr;
            s.port = port;
            s.hostName = "(Retrieving info...) " + entry;
            s.ping = 9999;
            s.tag = (unsigned short)((rand() % 0xFFFF) + 1);
            g.servers.push_back(s);
            EnqueueServer((int)g.servers.size() - 1, true, true, false, false);
        }
        g.batchActive = !g.queryQueue.empty();
        g.oldSortMode = (SortMode)-1;
        BuildOrder();
        break;
    }
    case net::QueryResult::GameLaunch: {
        if (r.launchError == net::LE_None)
            break;
        const char *msg = "Unable to launch the game.";
        switch (r.launchError) {
        case net::LE_Execute:  msg = "Unable to execute."; break;
        case net::LE_Allocate: msg = "Failed to allocate memory in the game process."; break;
        case net::LE_WritePath: msg = "Failed to write the SA-MP library path."; break;
        case net::LE_CreateRemoteThread: msg = "Failed to create remote thread."; break;
        case net::LE_LoadLibrary: msg = "Failed to load the SA-MP library into the game."; break;
        case net::LE_Resume: msg = "Failed to start the game process."; break;
        default: break;
        }
        Toast(msg);
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// favorites
// ---------------------------------------------------------------------------

static void SaveFavoritesNow()
{
    if (g.masterFile != 0)
        return;
    bool saveServ = ReadRegBool(L"SaveServPasses", false);
    bool saveRcon = ReadRegBool(L"SaveRconPasses", false);
    if (!net::ExportFavoritesFile(GetUserDataFile(), g.servers, true,
                                  saveServ, saveRcon))
        Toast("Unable to save the favorites file.");
}

static void AddServerToFavorites(const std::string &text)
{
    std::string s = net::Trim(text);
    if (s.empty())
        return;
    std::string addr;
    int port = 7777;
    size_t colon = s.find(':');
    if (colon != std::string::npos && colon > 0) {
        addr = net::Trim(s.substr(0, colon));
        port = atoi(net::Trim(s.substr(colon + 1)).c_str());
    } else {
        addr = s;
    }
    if (!net::IsValidEndpoint(addr, port)) {
        Toast("Invalid server address or port.");
        return;
    }
    for (auto &sv : g.servers)
        if (sv.address == addr && sv.port == port)
            return;
    ServerInfo n;
    n.address = addr;
    n.port = port;
    n.hostName = addr + ":" + std::to_string(port);
    n.ping = 9999;
    n.tag = (unsigned short)((rand() % 0xFFFF) + 1);
    g.servers.push_back(n);
    EnqueueServer((int)g.servers.size() - 1, true, true, false, false);
    g.oldSortMode = (SortMode)-1;
    BuildOrder();
    SaveFavoritesNow();
}

static void DeleteSelectedServer()
{
    if (g.selectedIdx < 0 || g.selectedIdx >= (int)g.servers.size())
        return;
    if (g.masterFile != 0)
        return;
    g.servers.erase(g.servers.begin() + g.selectedIdx);
    g.selectedIdx = -1;
    g.selKey.clear();
    g.oldSortMode = (SortMode)-1;
    RebuildLookup();
    BuildOrder();
    SaveFavoritesNow();
}

static void LoadFavorites()
{
    std::vector<ServerInfo> imported;
    if (!net::ImportFavoritesFile(GetUserDataFile(), imported)) {
        g.servers.clear();
        BuildOrder();
        return;
    }
    g.servers.swap(imported);
    for (int i = 0; i < (int)g.servers.size(); i++) {
        g.servers[i].hostName = "(Retrieving info...) " +
            g.servers[i].address + ":" +
            std::to_string(g.servers[i].port);
        EnqueueServer(i, true, true, false, false);
    }
    g.oldSortMode = (SortMode)-1;
    BuildOrder();
}

// ---------------------------------------------------------------------------
// game connect
// ---------------------------------------------------------------------------

static int CALLBACK BrowseCallbackProc(HWND hwnd, UINT uMsg, LPARAM lParam,
                                       LPARAM lpData)
{
    if (uMsg == BFFM_INITIALIZED) {
        SetWindowTextW(hwnd, L"GTA: San Andreas Installation");
        SendMessageW(hwnd, BFFM_SETSELECTION, TRUE, lpData);
    }
    return 0;
}

static bool BrowseForGTAFolder()
{
    BROWSEINFOW bi = {};
    wchar_t dispName[MAX_PATH] = {};
    wchar_t savePath[MAX_PATH] = {};
    std::wstring startDir = ReadRegString(HKEY_CURRENT_USER,
        L"SOFTWARE\\Rockstar Games\\GTA San Andreas\\Installation", L"ExePath");
    std::wstring regExe = ReadRegString(HKEY_CURRENT_USER, L"SOFTWARE\\SAMP",
                                        L"gta_sa_exe");
    std::wstring chosen = startDir;
    if (chosen.empty() && !regExe.empty())
        chosen = regExe.substr(0, regExe.find_last_of(L"\\") + 1);

    bi.hwndOwner = nullptr;
    bi.pszDisplayName = dispName;
    bi.lpszTitle = L"Please locate your GTA: San Andreas installation...";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (!chosen.empty()) {
        bi.lParam = (LPARAM)chosen.c_str();
        bi.lpfn = BrowseCallbackProc;
    }
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl)
        return false;
    bool ok = SHGetPathFromIDListW(pidl, savePath);
    CoTaskMemFree(pidl);
    if (!ok)
        return false;
    g.gtaExe = std::wstring(savePath) + L"\\gta_sa.exe";
    WriteRegString(L"gta_sa_exe", g.gtaExe);
    return true;
}

static bool GameSetupReady(bool prompt)
{
    if (g.gtaExe.empty())
        g.gtaExe = ReadRegString(HKEY_CURRENT_USER, L"SOFTWARE\\SAMP",
                                 L"gta_sa_exe");
    if (!g.gtaExe.empty()) {
        wchar_t full[MAX_PATH];
        DWORD n = GetFullPathNameW(g.gtaExe.c_str(), MAX_PATH, full, nullptr);
        if (n) {
            std::wstring abs(full, n);
            g.gtaExe = abs;
            WriteRegString(L"gta_sa_exe", abs);
        }
    }
    if (g.gtaExe.empty() ||
        GetFileAttributesW(g.gtaExe.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        if (!prompt)
            return false;
        if (!BrowseForGTAFolder())
            return false;
    }
    size_t slash = g.gtaExe.find_last_of(L"\\");
    std::wstring dir = slash == std::wstring::npos
        ? L"" : g.gtaExe.substr(0, slash + 1);
    if (GetFileAttributesW((dir + L"samp.dll").c_str())
            == INVALID_FILE_ATTRIBUTES)
    {
        if (prompt) {
            std::wstring msg =
                L"SA-MP library (samp.dll) not found next to the game "
                L"executable:\n\n" + g.gtaExe +
                L"\n\nInstall SA-MP (or open.mp) into the game folder "
                L"and try again.";
            MessageBoxW(nullptr, msg.c_str(), L"SA:MP Launcher",
                        MB_OK | MB_ICONERROR);
        }
        return false;
    }
    return true;
}

static void LaunchGame(const std::string &ip, const std::string &port,
                       const std::string &pass)
{
    std::wstring exe = g.gtaExe;
    std::wstring dir = exe.substr(0, exe.find_last_of(L"\\") + 1);
    std::wstring samp = dir + L"samp.dll";

    std::wstring cmd = L"\"" + exe + L"\" -c -n " + Utf8ToWide(g.nickname) +
                       L" -h " + Utf8ToWide(ip) + L" -p " + Utf8ToWide(port);
    if (!pass.empty())
        cmd += L" -z " + Utf8ToWide(pass);

    GameSetupReady(false);
    net::StartGameLaunch(exe, cmd, dir, samp);
    g.pendingConnect.active = false;
}

static void TryLaunch()
{
    if (g.pendingConnect.host.empty() || g.pendingConnect.port.empty())
        return;
    if (!GameSetupReady(true)) {
        g.pendingConnect.active = false;
        if (g.gtaExe.empty())
            Toast("GTA: San Andreas executable not found.");
        return;
    }
    if (!g.nickname[0]) {
        g.pendingConnect.active = false;
        Toast("Please enter your nickname first.");
        return;
    }
    std::string nickAnsi = net::Utf8ToAnsi(g.nickname);
    if (net::HasUnsafeCommandCharacters(nickAnsi) ||
        net::HasUnsafeCommandCharacters(g.pendingConnect.password))
    {
        Toast("Nickname and password cannot contain spaces, quotes, or "
              "control characters.");
        g.pendingConnect.active = false;
        return;
    }

    std::string ip;
    if (!net::ResolveCached(g.pendingConnect.host, ip)) {
        if (!g.dnsFailed.count(g.pendingConnect.host))
            net::RequestDns(g.pendingConnect.host); // continue when resolved
        else {
            Toast("Unable to resolve the server address.");
            g.pendingConnect.active = false;
        }
        return;
    }
    LaunchGame(ip, g.pendingConnect.port, g.pendingConnect.password);
}

// ---------------------------------------------------------------------------
// RCON
// ---------------------------------------------------------------------------

static void StartRconSession(const std::string &hostPort,
                             const std::string &password)
{
    std::string host;
    int port = 7777;
    size_t colon = hostPort.find(':');
    if (colon != std::string::npos && colon > 0) {
        host = hostPort.substr(0, colon);
        port = atoi(net::Trim(hostPort.substr(colon + 1)).c_str());
    } else {
        host = hostPort;
    }
    if (host.empty() || host.size() > 253 || port < 1 || port > 65535) {
        Toast("Invalid RCON host, port, or password.");
        return;
    }
    net::StartRcon(net::Utf8ToAnsi(host), port, net::Utf8ToAnsi(password));
}

// ---------------------------------------------------------------------------
// file dialogs (.fav)
// ---------------------------------------------------------------------------

static bool OpenFavFile(std::wstring &path)
{
    IFileOpenDialog *dlg = nullptr;
    if (CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&dlg)) != S_OK || !dlg)
        return false;
    COMDLG_FILTERSPEC spec = { L"SA-MP Favorites List (*.fav)", L"*.fav" };
    dlg->SetFileTypes(1, &spec);
    dlg->SetTitle(L"Import Favorites");
    HRESULT hr = dlg->Show(nullptr);
    if (SUCCEEDED(hr)) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR name = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
                path = name;
                CoTaskMemFree(name);
            }
            item->Release();
        }
    }
    dlg->Release();
    return !path.empty();
}

static bool SaveFavFile(std::wstring &path)
{
    IFileSaveDialog *dlg = nullptr;
    if (CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&dlg)) != S_OK || !dlg)
        return false;
    COMDLG_FILTERSPEC spec = { L"SA-MP Favorites List (*.fav)", L"*.fav" };
    dlg->SetFileTypes(1, &spec);
    dlg->SetDefaultExtension(L"fav");
    dlg->SetTitle(L"Export Favorites");
    HRESULT hr = dlg->Show(nullptr);
    if (SUCCEEDED(hr)) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR name = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
                path = name;
                CoTaskMemFree(name);
            }
            item->Release();
        }
    }
    dlg->Release();
    return !path.empty();
}

// ---------------------------------------------------------------------------
// import / export
// ---------------------------------------------------------------------------

static bool g_favImportOpen = false;
static bool g_favExportOpen = false;
static int g_importAddMode = 1;
static std::wstring g_importPath;
static bool g_exportIncludePass = false;

static void DoImportFavorites()
{
    std::vector<ServerInfo> imported;
    if (!net::ImportFavoritesFile(g_importPath, imported)) {
        Toast("Invalid SA-MP favorites file.");
        return;
    }
    if (!g_importAddMode) {
        g.servers.clear();
        g.queryQueue.clear();
        g.queuedKeys.clear();
    }
    std::set<std::string> endpoints;
    for (auto &s : g.servers)
        endpoints.insert(s.address + ":" + std::to_string(s.port));
    for (auto &s : imported) {
        std::string key = s.address + ":" + std::to_string(s.port);
        if (endpoints.insert(key).second) {
            s.hostName = "(Retrieving info...) " + key;
            g.servers.push_back(s);
            EnqueueServer((int)g.servers.size() - 1, true, true, false, false);
        }
    }
    g.oldSortMode = (SortMode)-1;
    BuildOrder();
    SaveFavoritesNow();
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

static void StartMasterUpdate()
{
    if (g.masterUpdateInProgress)
        return;
    g.masterCancel = false;
    g.masterUpdateInProgress = true;
    net::StartMasterDownload();
}

static void OpenServerProperties()
{
    if (g.selectedIdx < 0 || g.selectedIdx >= (int)g.servers.size())
        return;
    ServerInfo &s = g.servers[g.selectedIdx];
    memset(g.propsServerPass, 0, sizeof(g.propsServerPass));
    memset(g.propsRconPass, 0, sizeof(g.propsRconPass));
    strncpy(g.propsServerPass, s.serverPassword.c_str(),
            sizeof(g.propsServerPass) - 1);
    strncpy(g.propsRconPass, s.rconPassword.c_str(),
            sizeof(g.propsRconPass) - 1);
    g.propsOpen = true;
}

static void CopyServerInfoToClipboard()
{
    if (g.selectedIdx < 0 || g.selectedIdx >= (int)g.servers.size())
        return;
    ServerInfo &s = g.servers[g.selectedIdx];
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "HostName: %s\r\nAddress:  %s:%d\r\nPlayers:  %d / %d\r\n"
        "Ping:     %s\r\nMode:     %s\r\nLanguage: %s",
        s.hostName.c_str(), s.address.c_str(), s.port, s.players,
        s.maxPlayers, s.ping == 9999 ? "-"
            : std::to_string(s.ping).c_str(),
        s.mode.c_str(), s.map.c_str());
    SetClipText(buf);
}

static void AddNickToHistory(const char *nick);
static void LoadNickHistory();
static void SaveNickHistory();

static void DrawMenus()
{
    if (!ImGui::BeginMenuBar())
        return;

    if (ImGui::BeginMenu("File")) {
        if (IconMenuItem("miImportFavoritesList", "Import Favorites..."))
            if (OpenFavFile(g_importPath)) {
                g_importAddMode = 1;
                g_favImportOpen = true;
            }
        if (IconMenuItem("miExportFavoritesList", "Export Favorites..."))
            g_favExportOpen = true;
        ImGui::Separator();
        if (IconMenuItem("miExit", "Exit"))
            g_running = false;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Servers")) {
        bool fav = g.masterFile == 0;
        bool hasSel = g.selectedIdx >= 0 &&
                      g.selectedIdx < (int)g.servers.size();
        if (IconMenuItem("tbConnect", "Connect to Server", false, hasSel))
            g.connectRequested = true;
        if (IconMenuItem("tbAddServer", "Add Server...")) {
            std::string pre = GetClipText();
            if (hasSel)
                pre = g.servers[g.selectedIdx].address + ":" +
                      std::to_string(g.servers[g.selectedIdx].port);
            memset(g.inputBuffer, 0, sizeof(g.inputBuffer));
            if (pre.size() < sizeof(g.inputBuffer))
                strncpy(g.inputBuffer, pre.c_str(), pre.size());
            g.inputTitle = "Add Server";
            g.inputPrompt = "Enter new server HOST:PORT...";
            g.inputKind = 0;
            g.inputResult = false;
            g.inputOpen = true;
        }
        ImGui::Separator();
        if (IconMenuItem("tbDeleteServer", "Delete Server", false,
                         fav && hasSel))
            DeleteSelectedServer();
        if (IconMenuItem("tbRefreshServer", "Refresh Server", false, hasSel))
            EnqueueServer(g.selectedIdx, true, true, true, true);
        if (IconMenuItem("tbServerProperties", "Server Properties", false,
                         hasSel))
            OpenServerProperties();
        if (IconMenuItem("tbCopyServerInfo", "Copy Server Info", false,
                         hasSel))
            CopyServerInfoToClipboard();
        ImGui::Separator();
        if (IconMenuItem("RemoteConsole", "Remote Console...", false, hasSel)) {
            ServerInfo &s = g.servers[g.selectedIdx];
            snprintf(g.rconHost, sizeof(g.rconHost), "%s:%d",
                     s.address.c_str(), s.port);
            memset(g.rconPass, 0, sizeof(g.rconPass));
            strncpy(g.rconPass, s.rconPassword.c_str(),
                    sizeof(g.rconPass) - 1);
            g.rconOpen = true;
        }
        ImGui::Separator();
        if (IconMenuItem("tbSettings", "Settings..."))
            g.settingsOpen = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Show Server Info", nullptr, g.filtered))
            g.filtered = !g.filtered;
        if (ImGui::MenuItem("Status Bar", nullptr, g.statusBarVisible))
            g.statusBarVisible = !g.statusBarVisible;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Tools")) {
        if (IconMenuItem("tbSettings", "Settings..."))
            g.settingsOpen = true;
        if (IconMenuItem("tbMasterServerUpdate", "Master Server Update",
                         false, g.masterFile != 0))
            StartMasterUpdate();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (IconMenuItem("tbHelp", "Help Topics"))
            ShellExecuteW(nullptr, L"open", L"https://wiki.sa-mp.com/",
                          nullptr, nullptr, SW_SHOWNORMAL);
        if (IconMenuItem("miSamp", "SA-MP Website"))
            ShellExecuteW(nullptr, L"open", L"https://www.sa-mp.com/",
                          nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::Separator();
        if (IconMenuItem("tbAbout", "About"))
            g.aboutOpen = true;
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

static void DrawToolbar()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    if (g.masterFile != 0) {
        if (IconTextButton("tbMasterServerUpdate", "Master Server Update"))
            StartMasterUpdate();
    } else {
        if (IconTextButton("miImportFavoritesList", "Import Favorites"))
            if (OpenFavFile(g_importPath)) {
                g_importAddMode = 1;
                g_favImportOpen = true;
            }
    }
    ImGui::SameLine();
    if (IconTextButton("tbCopyServerInfo", "Copy"))
        if (g.selectedIdx >= 0 && g.selectedIdx < (int)g.servers.size()) {
            ServerInfo &s = g.servers[g.selectedIdx];
            char buf[1024];
            snprintf(buf, sizeof(buf),
                "HostName: %s\r\nAddress:  %s:%d\r\nPlayers:  %d / %d\r\n"
                "Ping:     %s\r\nMode:     %s\r\nLanguage: %s",
                s.hostName.c_str(), s.address.c_str(), s.port, s.players,
                s.maxPlayers, s.ping == 9999 ? "-"
                    : std::to_string(s.ping).c_str(),
                s.mode.c_str(), s.map.c_str());
            SetClipText(buf);
        }
    ImGui::SameLine();
    if (IconTextButton("tbAddServer", "Add"))
        if (g.masterFile == 0) {
            std::string pre = GetClipText();
            memset(g.inputBuffer, 0, sizeof(g.inputBuffer));
            if (pre.size() < sizeof(g.inputBuffer))
                strncpy(g.inputBuffer, pre.c_str(), pre.size());
            g.inputTitle = "Add Server";
            g.inputPrompt = "Enter new server HOST:PORT...";
            g.inputKind = 0;
            g.inputResult = false;
            g.inputOpen = true;
        }
    ImGui::SameLine();
    if (IconTextButton("tbDeleteServer", "Delete"))
        DeleteSelectedServer();
    ImGui::SameLine();
    if (IconTextButton("tbRefreshServer", "Refresh"))
        if (g.selectedIdx >= 0 && g.selectedIdx < (int)g.servers.size())
            EnqueueServer(g.selectedIdx, true, true, true, true);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    ImGui::InputText("##nick", g.nickname, sizeof(g.nickname));
    ImGui::SameLine();
    if (ImGui::Button("\xe2\x96\xbc", ImVec2(26, 0)))
        ImGui::OpenPopup("##nickhistory");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Nickname history");
    if (ImGui::BeginPopup("##nickhistory")) {
        if (g.nickHistory.empty())
            ImGui::TextDisabled("(no saved nicknames)");
        for (size_t i = 0; i < g.nickHistory.size(); i++)
            if (ImGui::Selectable(g.nickHistory[i].c_str())) {
                strncpy(g.nickname, g.nickHistory[i].c_str(),
                        sizeof(g.nickname) - 1);
                g.nickname[sizeof(g.nickname) - 1] = 0;
            }
        ImGui::Separator();
        if (ImGui::MenuItem("Clear history")) {
            g.nickHistory.clear();
            SaveNickHistory();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (IconTextButton("tbConnect", "Connect"))
        g.connectRequested = true;
    ImGui::PopStyleVar();

    ImGui::Separator();
}

static void HeaderButton(const char *label, SortMode mode)
{
    ImGui::PushID((int)mode);
    bool sorted = (g.sortMode == mode);
    std::string btext;
    const char *shown = label;
    const Icon *arrow = nullptr;
    float aw = 0.0f, ah = 0.0f;
    if (sorted) {
        arrow = FindIcon(g.sortDir == SortDir::Up ? "imUpArrow"
                                                  : "imDownArrow");
        if (arrow && arrow->w > 0) {
            IconDrawSize(arrow, 10.0f, aw, ah);
            float sp = ImGui::CalcTextSize(" ").x;
            if (sp < 0.1f) sp = 6.0f;
            btext.assign((size_t)ceilf((aw + 8.0f) / sp), ' ');
            btext += label;
            shown = btext.c_str();
        }
    }
    if (ImGui::Button(shown, ImVec2(-FLT_MIN, 0))) {
        if (sorted)
            g.sortDir = (g.sortDir == SortDir::Up) ? SortDir::Down
                                                   : SortDir::Up;
        else {
            g.sortMode = mode;
            g.sortDir = SortDir::Up;
        }
        BuildOrder();
    }
    if (sorted && arrow && aw > 0) {
        ImVec2 mn = ImGui::GetItemRectMin();
        ImVec2 mx = ImGui::GetItemRectMax();
        float y = mn.y + (mx.y - mn.y - ah) * 0.5f;
        ImGui::GetWindowDrawList()->AddImage(
            arrow->srv, ImVec2(mx.x - aw - 3, y),
            ImVec2(mx.x - 3, y + ah));
    }
    ImGui::PopID();
}

static void RightText(const char *text)
{
    float tw = ImGui::CalcTextSize(text).x;
    float pad = ImGui::GetStyle().CellPadding.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPos().x +
                         ImGui::GetColumnWidth() - pad - tw);
    ImGui::TextUnformatted(text);
}

static void DrawPingCell(int ping)
{
    if (ping == 9999) {
        ImGui::TextDisabled("-");
        return;
    }
    ImU32 col = ping <= 60  ? IM_COL32(126, 217, 132, 255)
              : ping <= 180 ? IM_COL32(235, 208, 112, 255)
                            : IM_COL32(238, 108, 101, 255);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%d", ping);
    RightText(tmp);
    ImGui::PopStyleColor();
}

static void DrawServerTable()
{
    ImGuiTableFlags flags =
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_NoPadInnerX;

    if (!ImGui::BeginTable("##servers", 5, flags))
        return;

    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0);
    ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, 70);
    ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed, 52);
    ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 120);
    ImGui::TableSetupColumn("Language", ImGuiTableColumnFlags_WidthFixed, 120);

    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    ImGui::TableSetColumnIndex(0); HeaderButton("Name", SortMode::HostName);
    ImGui::TableSetColumnIndex(1); HeaderButton("Players", SortMode::Players);
    ImGui::TableSetColumnIndex(2); HeaderButton("Ping", SortMode::Ping);
    ImGui::TableSetColumnIndex(3); HeaderButton("Mode", SortMode::Mode);
    ImGui::TableSetColumnIndex(4); HeaderButton("Language", SortMode::Map);

    float rowH = ImGui::GetTextLineHeightWithSpacing();
    ImGuiListClipper clipper;
    clipper.Begin((int)g.order.size(), rowH);

    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
            int idx = g.order[row];
            if (idx < 0 || idx >= (int)g.servers.size())
                continue;
            ServerInfo &s = g.servers[idx];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool sel = g.selectedIdx == idx;
            const Icon *pad = FindIcon(s.passworded ? "imPadlocked"
                                                    : "imPadlock");
            std::string lbl;
            const char *host = s.hostName.c_str();
            float pw = 0.0f, ph = 0.0f;
            if (pad && pad->w > 0) {
                IconDrawSize(pad, 14.0f, pw, ph);
                float sp = ImGui::CalcTextSize(" ").x;
                if (sp < 0.1f) sp = 6.0f;
                lbl.assign((size_t)ceilf((pw + 8.0f) / sp), ' ');
                lbl += host;
                host = lbl.c_str();
            }
            if (ImGui::Selectable(host, sel,
                                  ImGuiSelectableFlags_SpanAllColumns))
                g.selectedIdx = idx;
            if (pad && pw > 0) {
                ImVec2 mn = ImGui::GetItemRectMin();
                ImVec2 mx = ImGui::GetItemRectMax();
                float y = mn.y + (mx.y - mn.y - ph) * 0.5f;
                ImGui::GetWindowDrawList()->AddImage(
                    pad->srv, ImVec2(mn.x + 4, y),
                    ImVec2(mn.x + 4 + pw, y + ph));
            }
            if (ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                g.connectRequested = true;
            bool rcMenu = ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
                      (ImGui::IsItemHovered() &&
                       ImGui::IsMouseReleased(ImGuiMouseButton_Right));
            if (rcMenu) {
                g.selectedIdx = idx;
                ImGui::SetNextWindowPos(ImGui::GetMousePos(),
                                        ImGuiCond_Appearing);
                ImGui::OpenPopup("##serverctx");
            }
            if (!g_serverCtxDone) {
                if (ImGui::BeginPopup("##serverctx")) {
                    g_serverCtxDone = true;
                    if (ImGui::MenuItem("Connect"))
                        g.connectRequested = true;
                    ImGui::Separator();
                    if (ImGui::MenuItem("Server Properties"))
                        OpenServerProperties();
                    if (ImGui::MenuItem("Copy Server Info"))
                        CopyServerInfoToClipboard();
                    if (ImGui::MenuItem("Refresh Server"))
                        EnqueueServer(g.selectedIdx, true, true, true, true);
                    ImGui::EndPopup();
                }
            }

            ImGui::TableSetColumnIndex(1);
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%d / %d", s.players, s.maxPlayers);
            RightText(tmp);
            ImGui::TableSetColumnIndex(2);
            DrawPingCell(s.ping);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(s.mode.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(s.map.c_str());
        }
    }

    ImGui::EndTable();
}

static bool LangEq(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

static bool LangContains(const char *text, const char *q)
{
    if (!text || !q || !*q)
        return true;
    for (const char *t = text; *t; ++t) {
        const char *a = t, *b = q;
        while (*a && *b &&
               tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            ++a;
            ++b;
        }
        if (!*b)
            return true;
    }
    return false;
}

static void DrawFilterRow()
{
    // search by name / ip / mode / language
    ImGui::SetNextItemWidth(300);
    if (ImGui::InputText("##search", g.search, sizeof(g.search)))
        BuildOrder();
    ImGui::SameLine();

    // language dropdown
    ImGui::TextUnformatted("Language:");
    ImGui::SameLine();
    int langIdx = 0;
    for (int i = 1; i < kLanguageCount; i++) {
        if (LangEq(g.filterLang, kLanguages[i].label)) {
            langIdx = i;
            break;
        }
    }
    const char *langLabel = (langIdx > 0) ? kLanguages[langIdx].label : "All";
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo("##fLang", langLabel)) {
        static char langQuery[64] = {};
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##langQuery", "Search language...",
                                 langQuery, sizeof(langQuery));
        ImGui::Separator();
        float listH = 8.0f * ImGui::GetTextLineHeightWithSpacing();
        if (ImGui::BeginChild("##langList", ImVec2(0.0f, listH))) {
            for (int i = 0; i < kLanguageCount; i++) {
                if (!LangContains(kLanguages[i].label, langQuery))
                    continue;
                if (ImGui::Selectable(kLanguages[i].label, langIdx == i)) {
                    if (i == 0)
                        g.filterLang[0] = 0;
                    else {
                        strncpy(g.filterLang, kLanguages[i].label,
                                sizeof(g.filterLang) - 1);
                        g.filterLang[sizeof(g.filterLang) - 1] = 0;
                    }
                    BuildOrder();
                }
            }
            ImGui::EndChild();
        } else {
            ImGui::TextUnformatted("No matches");
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();

    // mode filter
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputText("##fMode", g.filterMode, sizeof(g.filterMode)))
        BuildOrder();
    ImGui::SameLine();

    // map filter
    ImGui::SetNextItemWidth(140);
    if (ImGui::InputText("##fMap", g.filterMap, sizeof(g.filterMap)))
        BuildOrder();
    ImGui::NewLine();

    // show / hide toggles
    if (ImGui::Checkbox("Full games", &g.hideFull))
        BuildOrder();
    ImGui::SameLine();
    if (ImGui::Checkbox("Empty games", &g.hideEmpty))
        BuildOrder();
    ImGui::SameLine();
    if (ImGui::Checkbox("Passworded", &g.hidePassworded))
        BuildOrder();
}

static void DrawInfoPanel()
{
    ServerInfo *s = nullptr;
    if (g.selectedIdx >= 0 && g.selectedIdx < (int)g.servers.size())
        s = &g.servers[g.selectedIdx];

    if (!s) {
        ImGui::TextUnformatted(" - - -");
        return;
    }

    ImGui::TextUnformatted((" Server Info: " + s->hostName).c_str());
    ImGui::Separator();

    ImGui::Text("Address:");
    ImGui::SameLine();
    std::string addr = s->address + ":" + std::to_string(s->port);
    if (ImGui::Button(addr.c_str(), ImVec2(0, 0)))
        SetClipText(addr);

    char tmp[256];
    snprintf(tmp, sizeof(tmp), "Players: %d / %d", s->players, s->maxPlayers);
    ImGui::TextUnformatted(tmp);
    snprintf(tmp, sizeof(tmp), "Ping: %s",
             s->ping == 9999 ? "-" : std::to_string(s->ping).c_str());
    ImGui::TextUnformatted(tmp);
    snprintf(tmp, sizeof(tmp), "Mode: %s", s->mode.c_str());
    ImGui::TextUnformatted(tmp);
    snprintf(tmp, sizeof(tmp), "Language: %s", s->map.c_str());
    ImGui::TextUnformatted(tmp);

    for (const auto &rule : s->rules) {
        if (rule.rule == "weburl") {
            std::string url = net::Trim(rule.value);
            if (!url.empty()) {
                ImGui::Text("Web:");
                ImGui::SameLine();
                if (ImGui::Button(url.c_str(), ImVec2(0, 0)))
                    ShellExecuteW(nullptr, L"open",
                                  Utf8ToWide("http://" + url).c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Players", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginChild("##players",
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing()
                                     * 10 + 8), ImGuiChildFlags_Borders);
        float x = ImGui::GetContentRegionAvail().x - 60;
        for (const auto &p : s->playersList) {
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::SameLine(x);
            ImGui::Text("%d", p.score);
        }
        ImGui::EndChild();
    }

    if (ImGui::CollapsingHeader("Rules")) {
        ImGui::BeginChild("##rules",
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing()
                                     * 8 + 8), ImGuiChildFlags_Borders);
        for (const auto &r : s->rules)
            ImGui::Text("%s  %s", r.rule.c_str(), r.value.c_str());
        ImGui::EndChild();
    }
}

static void HandleInputModalResult()
{
    if (!g.inputResult)
        return;
    g.inputResult = false;
    std::string text(net::Trim(g.inputBuffer));
    if (g.inputKind == 0) {
        if (g.masterFile == 0)
            AddServerToFavorites(text);
        else
            Toast("Switch to the Favorites tab to add servers.");
    }
}

static std::wstring GetIniFile()
{
    return GetUserFilesPath() + L"samp_c.ini";
}

static const char *kThemeNames = "Dark\0Dark Blue\0Dark Purple\0Light\0";
static const float kDefaultAccent[3] = { 0.58f, 0.76f, 0.98f };

static void LoadColorSettings()
{
    std::wstring ini = GetIniFile();
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(L"Launcher", L"Theme", L"0", buf, 32, ini.c_str());
    g.theme = _wtoi(buf);
    if (g.theme < 0 || g.theme > 3)
        g.theme = 0;
    GetPrivateProfileStringW(L"Launcher", L"AccentR", L"148", buf, 32, ini.c_str());
    g.accent[0] = _wtoi(buf) / 255.0f;
    GetPrivateProfileStringW(L"Launcher", L"AccentG", L"194", buf, 32, ini.c_str());
    g.accent[1] = _wtoi(buf) / 255.0f;
    GetPrivateProfileStringW(L"Launcher", L"AccentB", L"250", buf, 32, ini.c_str());
    g.accent[2] = _wtoi(buf) / 255.0f;
    for (int i = 0; i < 3; i++)
        if (g.accent[i] < 0.0f || g.accent[i] > 1.0f)
            g.accent[i] = kDefaultAccent[i];
}

static void SaveColorSettings()
{
    std::wstring ini = GetIniFile();
    wchar_t buf[32] = {};
    _itow(g.theme, buf, 10);
    WritePrivateProfileStringW(L"Launcher", L"Theme", buf, ini.c_str());
    _itow((int)(g.accent[0] * 255.0f), buf, 10);
    WritePrivateProfileStringW(L"Launcher", L"AccentR", buf, ini.c_str());
    _itow((int)(g.accent[1] * 255.0f), buf, 10);
    WritePrivateProfileStringW(L"Launcher", L"AccentG", buf, ini.c_str());
    _itow((int)(g.accent[2] * 255.0f), buf, 10);
    WritePrivateProfileStringW(L"Launcher", L"AccentB", buf, ini.c_str());
}

static std::wstring GetNickHistoryFile()
{
    return GetUserFilesPath() + L"nickhistory.xml";
}

static std::string ReadFileUtf8(const std::wstring &path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::string();
    DWORD size = GetFileSize(h, nullptr);
    std::string data;
    if (size > 0 && size < 64 * 1024 * 1024) {
        data.resize(size);
        DWORD rd = 0;
        ReadFile(h, &data[0], size, &rd, nullptr);
        data.resize(rd);
    }
    CloseHandle(h);
    return data;
}

static void WriteFileUtf8(const std::wstring &path, const std::string &data)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD wr = 0;
    WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(h);
}

static std::string XmlEscape(const std::string &s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;
        }
    }
    return out;
}

static std::string XmlUnescape(const std::string &s)
{
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '&') {
            if (s.compare(i, 5, "&amp;") == 0)  { out += '&';  i += 5; continue; }
            if (s.compare(i, 4, "&lt;") == 0)   { out += '<';  i += 4; continue; }
            if (s.compare(i, 4, "&gt;") == 0)   { out += '>';  i += 4; continue; }
            if (s.compare(i, 6, "&quot;") == 0) { out += '"';  i += 6; continue; }
            if (s.compare(i, 6, "&apos;") == 0) { out += '\''; i += 6; continue; }
        }
        out += s[i++];
    }
    return out;
}

static void AddNickToHistory(const char *nick)
{
    if (!nick || !nick[0])
        return;
    std::string n = nick;
    for (auto it = g.nickHistory.begin(); it != g.nickHistory.end(); ++it) {
        if (*it == n) {
            g.nickHistory.erase(it);
            break;
        }
    }
    g.nickHistory.insert(g.nickHistory.begin(), n);
    if (g.nickHistory.size() > 12)
        g.nickHistory.resize(12);
}

static void LoadNickHistory()
{
    g.nickHistory.clear();
    std::string data = ReadFileUtf8(GetNickHistoryFile());
    size_t pos = 0;
    while (pos < data.size()) {
        size_t a = data.find("<nick>", pos);
        if (a == std::string::npos)
            break;
        size_t b = data.find("</nick>", a + 6);
        if (b == std::string::npos)
            break;
        std::string nick = XmlUnescape(data.substr(a + 6, b - a - 6));
        if (!nick.empty() && g.nickHistory.size() < 12)
            g.nickHistory.push_back(nick);
        pos = b + 7;
    }
}

static void SaveNickHistory()
{
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        "<nickhistory>\r\n";
    for (size_t i = 0; i < g.nickHistory.size(); i++)
        xml += "  <nick>" + XmlEscape(g.nickHistory[i]) + "</nick>\r\n";
    xml += "</nickhistory>\r\n";
    std::wstring path = GetNickHistoryFile();
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
    WriteFileUtf8(path, xml);
}

static float Clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static ImVec4 Tint(const ImVec4 &c, float f)
{
    return ImVec4(Clamp01(c.x * f), Clamp01(c.y * f), Clamp01(c.z * f), c.w);
}

static void ApplyTheme()
{
    ImGuiStyle &st = ImGui::GetStyle();
    ImVec4 *c = st.Colors;
    bool light = (g.theme == 3);

    if (light)
        ImGui::StyleColorsLight(&st);
    else
        ImGui::StyleColorsDark(&st);
    c = st.Colors;

    ImVec4 bg, panel, popup;
    switch (g.theme) {
    case 1: // Dark Blue
        bg    = ImVec4(0.044f, 0.056f, 0.080f, 1.0f);
        panel = ImVec4(0.020f, 0.031f, 0.049f, 1.0f);
        popup = ImVec4(0.088f, 0.110f, 0.145f, 1.0f);
        break;
    case 2: // Dark Purple
        bg    = ImVec4(0.054f, 0.038f, 0.078f, 1.0f);
        panel = ImVec4(0.029f, 0.019f, 0.051f, 1.0f);
        popup = ImVec4(0.105f, 0.082f, 0.145f, 1.0f);
        break;
    case 3: // Light
        bg    = ImVec4(0.943f, 0.949f, 0.955f, 1.0f);
        panel = ImVec4(0.980f, 0.982f, 0.985f, 1.0f);
        popup = ImVec4(0.992f, 0.993f, 0.996f, 1.0f);
        break;
    default: // 0 Dark
        bg    = ImVec4(0.055f, 0.056f, 0.062f, 1.0f);
        panel = ImVec4(0.020f, 0.020f, 0.024f, 1.0f);
        popup = ImVec4(0.098f, 0.100f, 0.110f, 1.0f);
        break;
    }

    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = popup;
    c[ImGuiCol_MenuBarBg] = bg;
    c[ImGuiCol_TitleBg] = bg;
    c[ImGuiCol_TitleBgActive] = bg;
    c[ImGuiCol_TitleBgCollapsed] = bg;

    ImVec4 acc(g.accent[0], g.accent[1], g.accent[2], 1.0f);
    ImVec4 accDim  = Tint(acc, 1.20f);
    ImVec4 accHov  = Tint(acc, 1.45f);
    ImVec4 accSoft = Tint(acc, 0.28f);
    ImVec4 accMid  = Tint(acc, 0.48f);

    c[ImGuiCol_Button]        = accSoft;
    c[ImGuiCol_ButtonHovered] = accDim;
    c[ImGuiCol_ButtonActive]  = accHov;

    c[ImGuiCol_Header]        = accSoft;
    c[ImGuiCol_HeaderHovered] = accMid;
    c[ImGuiCol_HeaderActive]  = accDim;

    c[ImGuiCol_CheckMark]          = acc;
    c[ImGuiCol_TextSelectedBg]     = Tint(acc, 0.35f);
    c[ImGuiCol_NavHighlight]       = acc;
    c[ImGuiCol_Separator]          = Tint(acc, 0.40f);
    c[ImGuiCol_SeparatorHovered]   = accDim;
    c[ImGuiCol_SeparatorActive]    = accHov;
    c[ImGuiCol_Tab]                = accSoft;
    c[ImGuiCol_TabHovered]         = accMid;
    c[ImGuiCol_TabActive]          = accMid;
    c[ImGuiCol_TabUnfocused]       = accSoft;
    c[ImGuiCol_TabUnfocusedActive] = accMid;

    // grid list colors
    c[ImGuiCol_TableRowBg]     = panel;
    c[ImGuiCol_TableRowBgAlt]  = light ? ImVec4(0.908f, 0.914f, 0.922f, 1.0f)
                                       : Tint(panel, 1.40f);
    c[ImGuiCol_TableBorderLight]  = Tint(acc, light ? 0.30f : 0.22f);
    c[ImGuiCol_TableBorderStrong] = Tint(acc, light ? 0.55f : 0.45f);
    c[ImGuiCol_Border]            = Tint(acc, light ? 0.35f : 0.25f);

    st.FrameRounding = 3.0f;
    st.PopupRounding = 4.0f;
    st.ScrollbarRounding = 4.0f;
    st.WindowRounding = 0.0f;
}

static void DrawDialogs()
{
    ImGuiViewport *vp = ImGui::GetMainViewport();

    // generic input
    if (g.inputOpen) {
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::OpenPopup(g.inputTitle.c_str());
    }
    if (g.inputOpen) {
        bool open = true;
        if (ImGui::BeginPopupModal(g.inputTitle.c_str(), &open,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted(g.inputPrompt.c_str());
            ImGui::SetNextItemWidth(320);
            bool entered = ImGui::InputText("##in", g.inputBuffer,
                              sizeof(g.inputBuffer),
                              ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::Button("OK") || entered) {
                g.inputResult = true;
                g.inputOpen = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                g.inputOpen = false;
            ImGui::EndPopup();
        }
        if (!open)
            g.inputOpen = false;
    }

    // server password (connect)
    if (g.connectPasswordOpen) {
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::OpenPopup("Server Password");
    }
    if (g.connectPasswordOpen) {
        bool open = true;
        if (ImGui::BeginPopupModal("Server Password", &open,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("This server requires a password...");
            ImGui::SetNextItemWidth(260);
            bool entered = ImGui::InputText("##pass", g.inputBuffer,
                              sizeof(g.inputBuffer),
                              ImGuiInputTextFlags_Password |
                                  ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::Button("OK") || entered) {
                g.connectPasswordResult = true;
                g.connectPasswordOpen = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                g.connectPasswordOpen = false;
            ImGui::EndPopup();
        }
        if (!open)
            g.connectPasswordOpen = false;
    }

    // import confirm
    if (g_favImportOpen) {
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::OpenPopup("Import Favorites");
    }
    if (g_favImportOpen) {
        bool open = true;
        if (ImGui::BeginPopupModal("Import Favorites", &open,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("How should the servers be added?");
            ImGui::RadioButton("Add to current favorites", &g_importAddMode, 1);
            ImGui::RadioButton("Replace current favorites", &g_importAddMode, 0);
            if (ImGui::Button("OK")) {
                g_favImportOpen = false;
                DoImportFavorites();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                g_favImportOpen = false;
            ImGui::EndPopup();
        }
        if (!open)
            g_favImportOpen = false;
    }

    // export
    if (g_favExportOpen) {
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::OpenPopup("Export Favorites");
    }
    if (g_favExportOpen) {
        bool open = true;
        if (ImGui::BeginPopupModal("Export Favorites", &open,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Checkbox("Include saved passwords", &g_exportIncludePass);
            if (ImGui::Button("OK")) {
                g_favExportOpen = false;
                std::wstring path;
                if (SaveFavFile(path))
                    net::ExportFavoritesFile(path, g.servers,
                        g_exportIncludePass,
                        ReadRegBool(L"SaveServPasses", false),
                        ReadRegBool(L"SaveRconPasses", false));
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                g_favExportOpen = false;
            ImGui::EndPopup();
        }
        if (!open)
            g_favExportOpen = false;
    }

    // RCON
    if (g.rconOpen) {
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::OpenPopup("Remote Console");
    }
    if (g.rconOpen) {
        bool open = true;
        if (ImGui::BeginPopupModal("Remote Console", &open,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("Host:");
            ImGui::SetNextItemWidth(280);
            ImGui::InputText("##rhost", g.rconHost, sizeof(g.rconHost));
            ImGui::TextUnformatted("Password:");
            ImGui::SetNextItemWidth(280);
            ImGui::InputText("##rpass", g.rconPass, sizeof(g.rconPass),
                             ImGuiInputTextFlags_Password);
            if (ImGui::Button("Connect")) {
                StartRconSession(g.rconHost, g.rconPass);
                if (g.selectedIdx >= 0 &&
                    g.selectedIdx < (int)g.servers.size())
                    g.servers[g.selectedIdx].rconPassword = g.rconPass;
                g.rconOpen = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                g.rconOpen = false;
            ImGui::EndPopup();
        }
        if (!open)
            g.rconOpen = false;
    }

    // settings
    if (g.settingsOpen) {
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::OpenPopup("Settings");
    }
    if (g.settingsOpen) {
        static bool loadOnce = false;
        static bool saveServ = false;
        static bool saveRcon = false;
        static char proxy[512] = {};
        static char instDir[1024] = {};
        static int  themeSel = 0;
        static float accentSel[3] = {0, 0, 0};
        static int  themeSaved = 0;
        static float accentSaved[3] = {0, 0, 0};
        if (!loadOnce) {
            saveServ = ReadRegBool(L"SaveServPasses", false);
            saveRcon = ReadRegBool(L"SaveRconPasses", false);
            std::string p = WideToUtf8(ReadRegString(HKEY_CURRENT_USER,
                L"SOFTWARE\\SAMP", L"artwork_proxy"));
            strncpy(proxy, p.c_str(), sizeof(proxy) - 1);
            themeSel = g.theme;
            accentSel[0] = g.accent[0];
            accentSel[1] = g.accent[1];
            accentSel[2] = g.accent[2];
            themeSaved = g.theme;
            accentSaved[0] = g.accent[0];
            accentSaved[1] = g.accent[1];
            accentSaved[2] = g.accent[2];
            loadOnce = true;
        }
        {
            std::string dir = WideToUtf8(g.gtaExe);
            memset(instDir, 0, sizeof(instDir));
            strncpy(instDir, dir.c_str(), sizeof(instDir) - 1);
        }

        bool open = true;
        if (ImGui::BeginPopupModal("Settings", &open,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("GTA: San Andreas installation:");
            ImGui::SetNextItemWidth(320);
            ImGui::InputText("##inst", instDir, sizeof(instDir));
            ImGui::SameLine();
            if (ImGui::Button("Browse..."))
                BrowseForGTAFolder();
            ImGui::Separator();
            ImGui::Checkbox("Save server passwords", &saveServ);
            ImGui::Checkbox("Save RCON passwords", &saveRcon);
            ImGui::Separator();
            ImGui::TextUnformatted(
                "Artwork proxy (http://, https://, socks5://):");
            ImGui::SetNextItemWidth(320);
            ImGui::InputText("##proxy", proxy, sizeof(proxy));
            ImGui::Separator();
            ImGui::TextUnformatted("Launcher colors:");
            ImGui::SetNextItemWidth(220);
            bool thCh = ImGui::Combo("Color theme", &themeSel, kThemeNames);
            ImGui::SetNextItemWidth(220);
            bool acCh = ImGui::ColorEdit3("Accent", accentSel,
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
            ImGui::SameLine();
            bool resetAcc = ImGui::Button("Reset accent");
            if (resetAcc) {
                accentSel[0] = kDefaultAccent[0];
                accentSel[1] = kDefaultAccent[1];
                accentSel[2] = kDefaultAccent[2];
            }
            if (thCh || acCh || resetAcc) {
                g.theme = themeSel;
                g.accent[0] = accentSel[0];
                g.accent[1] = accentSel[1];
                g.accent[2] = accentSel[2];
                ApplyTheme();
            }
            ImGui::TextDisabled("Saved to samp_c.ini in your user files folder.");
            ImGui::Separator();
            if (ImGui::Button("OK")) {
                WriteRegBool(L"SaveServPasses", saveServ);
                WriteRegBool(L"SaveRconPasses", saveRcon);
                WriteRegString(L"artwork_proxy", Utf8ToWide(proxy));
                SaveFavoritesNow();
                SaveColorSettings();
                g.settingsOpen = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                g.theme = themeSaved;
                g.accent[0] = accentSaved[0];
                g.accent[1] = accentSaved[1];
                g.accent[2] = accentSaved[2];
                ApplyTheme();
                g.settingsOpen = false;
            }
            ImGui::EndPopup();
        }
        if (!open)
            g.settingsOpen = false;
    }

    // server properties
    if (g.propsOpen) {
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::OpenPopup("Server Properties");
    }
    if (g.propsOpen) {
        bool open = true;
        if (ImGui::BeginPopupModal("Server Properties", &open,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (g.selectedIdx >= 0 &&
                g.selectedIdx < (int)g.servers.size())
            {
                ServerInfo &s = g.servers[g.selectedIdx];
                char tmp[256];
                snprintf(tmp, sizeof(tmp), "HostName: %s",
                         s.hostName.c_str());
                ImGui::TextUnformatted(tmp);
                snprintf(tmp, sizeof(tmp), "Address: %s:%d",
                         s.address.c_str(), s.port);
                ImGui::TextUnformatted(tmp);
                snprintf(tmp, sizeof(tmp), "Players: %d / %d",
                         s.players, s.maxPlayers);
                ImGui::TextUnformatted(tmp);
                snprintf(tmp, sizeof(tmp), "Ping: %s",
                         s.ping == 9999 ? "-"
                             : std::to_string(s.ping).c_str());
                ImGui::TextUnformatted(tmp);
                snprintf(tmp, sizeof(tmp), "Mode: %s", s.mode.c_str());
                ImGui::TextUnformatted(tmp);
                snprintf(tmp, sizeof(tmp), "Language: %s", s.map.c_str());
                ImGui::TextUnformatted(tmp);
                ImGui::Separator();
                ImGui::TextUnformatted("Server password:");
                ImGui::SetNextItemWidth(300);
                ImGui::InputText("##spass", g.propsServerPass,
                                 sizeof(g.propsServerPass));
                if (!s.passworded) {
                    ImGui::BeginDisabled();
                    ImGui::TextUnformatted("(server is not passworded)");
                    ImGui::EndDisabled();
                }
                ImGui::TextUnformatted("RCON password:");
                ImGui::SetNextItemWidth(300);
                ImGui::InputText("##rconp", g.propsRconPass,
                                 sizeof(g.propsRconPass));
                ImGui::Separator();
                if (ImGui::Button("Connect")) {
                    s.serverPassword = g.propsServerPass;
                    s.rconPassword = g.propsRconPass;
                    SaveFavoritesNow();
                    g.propsOpen = false;
                    g.connectRequested = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("OK")) {
                    s.serverPassword = g.propsServerPass;
                    s.rconPassword = g.propsRconPass;
                    SaveFavoritesNow();
                    g.propsOpen = false;
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                    g.propsOpen = false;
            } else {
                g.propsOpen = false;
            }
            ImGui::EndPopup();
        }
        if (!open)
            g.propsOpen = false;
    }

    // about
    if (g.aboutOpen) {
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::OpenPopup("About");
    }
    if (g.aboutOpen) {
        bool open = true;
        if (ImGui::BeginPopupModal("About", &open,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("SA:MP Launcher");
            ImGui::Separator();
            ImGui::TextUnformatted(
                "Rewritten for C++ / Win32 / Dear ImGui (no VCL).");
            ImGui::TextUnformatted("Server list powered by api.open.mp.");
            ImGui::TextUnformatted("Original project: github.com/1therealcloud/"
                                   "samp-launcher.");
            ImGui::Separator();
            if (ImGui::Button("OK"))
                g.aboutOpen = false;
            ImGui::EndPopup();
        }
        if (!open)
            g.aboutOpen = false;
    }
}

static void WinCtrlButton(const char *label, const char *tip, void (*fn)(),
                          bool closeBtn = false)
{
    ImGui::PushID(label);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,
                        ImVec2(0.5f, 0.5f));

    ImVec4 acc(g.accent[0], g.accent[1], g.accent[2], 1.0f);
    ImVec4 hoverBg = closeBtn ? ImVec4(0.72f, 0.20f, 0.20f, 1.0f)
                              : Tint(acc, 1.45f);
    ImVec4 hoverBorder = closeBtn ? ImVec4(1.00f, 0.35f, 0.35f, 1.0f)
                                  : Tint(acc, 2.40f);

    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tint(hoverBg, 1.2f));

    bool pressed = ImGui::Button(label,
                                 ImVec2((float)kWinBtnW, (float)kTitleBarH));
    bool hov = ImGui::IsItemHovered();
    if (hov) {
        ImVec2 mn = ImGui::GetItemRectMin();
        ImVec2 mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRect(mn, mx,
            ImGui::GetColorU32(hoverBorder), 3.0f, ImDrawFlags_None, 2.0f);
    }
    if (pressed && fn)
        fn();
    if (hov)
        ImGui::SetTooltip("%s", tip);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    ImGui::PopID();
}

static void DrawTitleBar()
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    float w = vp->WorkSize.x;

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(w, (float)kTitleBarH));
    ImGui::Begin("##titlebar", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoBringToFrontOnFocus);

    // brand: app icon + title
    const Icon *appIcon = FindIcon("imAppIcon");
    if (appIcon && appIcon->w > 0 && appIcon->h > 0) {
        float isz = 20.0f;
        ImGui::SetCursorPos(ImVec2(12, (kTitleBarH - isz) / 2.0f));
        ImGui::InvisibleButton("##appIcon", ImVec2(isz, isz));
        DrawIconImage(appIcon, ImGui::GetItemRectMin(), isz);
        ImGui::SameLine(0, 6);
        ImGui::SetCursorPosY((kTitleBarH - ImGui::GetFontSize()) / 2.0f);
    } else {
        ImGui::SetCursorPos(ImVec2(12, (kTitleBarH - ImGui::GetFontSize()) / 2.0f));
    }
    ImGui::TextUnformatted("San Andreas MultiPlayer");

    // window controls, flush to the right edge (no SameLine spacing)
    ImGui::SetCursorPos(ImVec2(w - 2.0f * kWinBtnW, 0));
    WinCtrlButton("-", "Minimize", []() { ShowWindow(g_hwnd, SW_MINIMIZE); });
    ImGui::SetCursorPos(ImVec2(w - 1.0f * kWinBtnW, 0));
    WinCtrlButton("X", "Close", []() { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); },
                  true);

    ImGui::End();
}

static void DrawMainWindow()
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    float barH = (g.statusBarVisible ? 24 : 0) + (float)kTitleBarH;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x,
                                   vp->WorkPos.y + (float)kTitleBarH));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x,
                                    vp->WorkSize.y - barH));
    ImGui::Begin("SA:MP", nullptr,
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar |
                ImGuiWindowFlags_NoBringToFrontOnFocus |
                ImGuiWindowFlags_NoSavedSettings);

    DrawMenus();
    DrawToolbar();
    ImGui::Separator();

    // tabs
    for (int i = 0; i < 2; i++) {
        if (i > 0)
            ImGui::SameLine();
        bool isFav = (i == 0);
        bool active = (g.masterFile == (isFav ? 0 : 1));
        if (ImGui::RadioButton(isFav ? "Favorites" : "Internet", active)) {
            if (!active) {
                if (g.masterFile == 0)
                    SaveFavoritesNow();
                g.masterCancel = true;
                g.masterFile = isFav ? 0 : 1;
                g.servers.clear();
                g.queryQueue.clear();
                g.queuedKeys.clear();
                g.dnsFailed.clear();
                g.batchActive = false;
                g.inFlight = 0;
                g.batchDeadline = 0;
                g.selectedIdx = -1;
                g.selKey.clear();
                g.oldSortMode = (SortMode)-1;
                if (isFav)
                    LoadFavorites();
                else
                    StartMasterUpdate();
                BuildOrder();
            }
        }
    }
    ImGui::Separator();
    DrawFilterRow();
    ImGui::Separator();

    // split layout
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float rightW = 330;
    if (rightW > avail.x * 0.42f)
        rightW = avail.x * 0.42f;
    if (rightW < 180)
        rightW = 180;
    float leftW = avail.x - rightW - 6;
    if (leftW < 200)
        leftW = 200;

    ImGui::BeginChild("##left", ImVec2(leftW, avail.y),
                      ImGuiChildFlags_Borders);
    DrawServerTable();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##right", ImVec2(avail.x - leftW - 6, avail.y),
                      ImGuiChildFlags_Borders);
    DrawInfoPanel();
    ImGui::EndChild();

    ImGui::End();
}

static void DrawStatusBar()
{
    if (!g.statusBarVisible)
        return;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x,
                                   vp->WorkPos.y + vp->WorkSize.y - 24));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, 24));
    ImGui::Begin("##statusbar", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (g.masterUpdateInProgress) {
        float bw = 58;
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - bw - 6,
                                   (24 - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::PushID("mastercancel");
        if (ImGui::Button("Cancel", ImVec2(bw - 4, 0)))
            g.masterCancel = true;
        ImGui::PopID();
        ImGui::SetCursorPos(ImVec2(8, (24 - ImGui::GetTextLineHeight()) * 0.5f));
        ImGui::TextUnformatted("Updating server list...");
        ImGui::SameLine(0, 12);
        if (!g.toast.empty() && GetTickCount() < g.toastUntil)
            ImGui::TextUnformatted(g.toast.c_str());
        else if (GetTickCount() >= g.toastUntil)
            g.toast.clear();
    } else {
        ImGui::TextUnformatted(g.status.c_str());
        if (!g.toast.empty() && GetTickCount() < g.toastUntil) {
            ImGui::SameLine(0, 24);
            ImGui::TextUnformatted(g.toast.c_str());
        } else if (GetTickCount() >= g.toastUntil) {
            g.toast.clear();
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// per-frame logic
// ---------------------------------------------------------------------------

static void Frame()
{
    // 1) drain network results
    std::vector<net::QueryResult> results;
    net::TakeResults(results);
    for (const auto &r : results)
        ApplyResult(r);

    // 2) query queue
    ProcessQueryQueue(g.masterFile != 0 ? 24 : 8);
    CheckBatchComplete();

    // 3) connect flow
    if (g.connectRequested) {
        g.connectRequested = false;
        if (g.selectedIdx >= 0 && g.selectedIdx < (int)g.servers.size()) {
            ServerInfo &s = g.servers[g.selectedIdx];
            if (s.passworded && s.serverPassword.empty()) {
                memset(g.inputBuffer, 0, sizeof(g.inputBuffer));
                g.connectPasswordOpen = true;
            } else {
                g.pendingConnect.active = true;
                g.pendingConnect.host = s.address;
                g.pendingConnect.port = std::to_string(s.port);
                g.pendingConnect.password = s.serverPassword;
            }
        }
    }
    if (g.connectPasswordResult) {
        g.connectPasswordResult = false;
        if (g.selectedIdx >= 0 && g.selectedIdx < (int)g.servers.size()) {
            ServerInfo &s = g.servers[g.selectedIdx];
            s.serverPassword = g.inputBuffer;
            SaveFavoritesNow();
            g.pendingConnect.active = true;
            g.pendingConnect.host = s.address;
            g.pendingConnect.port = std::to_string(s.port);
            g.pendingConnect.password = s.serverPassword;
        }
    }
    if (g.pendingConnect.active)
        TryLaunch();

    // 4) input modal results
    HandleInputModalResult();

    // 5) update totals
    UpdateTotals();

    // 6) draw UI
    ImGui::NewFrame();
    g_serverCtxDone = false;
    DrawTitleBar();
    DrawMainWindow();
    DrawStatusBar();
    DrawDialogs();
    ImGui::EndFrame();
}

// ---------------------------------------------------------------------------
// D3D11 infrastructure
// ---------------------------------------------------------------------------

static IDXGISwapChain *g_pSwapChain = nullptr;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView *g_mainRenderTargetView = nullptr;

static void CreateRenderTarget()
{
    ID3D11Texture2D *pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr,
                                             &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

static void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

static bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    const D3D_FEATURE_LEVEL featureLevels[2] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (hr != S_OK)
        return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) {
        g_pd3dDeviceContext->Release();
        g_pd3dDeviceContext = nullptr;
    }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

// ---------------------------------------------------------------------------
// window
// ---------------------------------------------------------------------------

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
    UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam,
                              LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_ResizeWidth = LOWORD(lParam);
            g_ResizeHeight = HIWORD(lParam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_NCHITTEST: {
        RECT rc;
        GetClientRect(hWnd, &rc);
        POINT p = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ScreenToClient(hWnd, &p);
        int cx = p.x, cy = p.y;
        if (cy < kTitleBarH)
            return cx >= rc.right - 2 * kWinBtnW - 2 ? HTCLIENT : HTCAPTION;
        return HTCLIENT;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void LoadUIFont()
{
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;

    const wchar_t *candidates[] = {
        L"C:\\Windows\\Fonts\\segoeui.ttf",
        L"C:\\Windows\\Fonts\\arial.ttf",
        L"C:\\Windows\\Fonts\\tahoma.ttf",
    };
    for (const wchar_t *cand : candidates) {
        DWORD attr = GetFileAttributesW(cand);
        if (attr == INVALID_FILE_ATTRIBUTES)
            continue;
        std::string utf8 = WideToUtf8(cand);
        ImFontConfig cfg = {};
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        if (io.Fonts->AddFontFromFileTTF(utf8.c_str(), 16.0f, &cfg,
                                         io.Fonts->GetGlyphRangesCyrillic()))
            return;
    }
    io.Fonts->AddFontDefault();
}

// ---------------------------------------------------------------------------
// entry point
// ---------------------------------------------------------------------------

static const wchar_t *kClassName = L"xlauncher_main";

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    CheckAnotherInstance();

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return 1;

    {
        typedef BOOL(WINAPI *Fn)(DPI_AWARENESS_CONTEXT);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            Fn fn = (Fn)GetProcAddress(user32,
                "SetProcessDpiAwarenessContext");
            if (fn)
                fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    HICON hAppIcon = LoadIconW(hInstance, L"MAINICON");
    if (!hAppIcon)
        hAppIcon = LoadIcon(nullptr, IDI_APPLICATION);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = hAppIcon;
    wc.hIconSm = hAppIcon;
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int winW = 940, winH = 620;
    if (winW > sw - 80) winW = sw - 80;
    if (winH > sh - 80) winH = sh - 80;

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kClassName,
        L"San Andreas MultiPlayer",
        WS_POPUP, (sw - winW) / 2, (sh - winH) / 2, winW, winH,
        nullptr, nullptr, hInstance, nullptr);
    g_hwnd = hwnd;

    if (hAppIcon && hwnd) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hAppIcon);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hAppIcon);
    }

    if (!hwnd) {
        MessageBoxW(nullptr, L"Unable to create the main window.",
                    L"SA:MP Error", MB_ICONERROR);
        return 1;
    }

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        MessageBoxW(nullptr, L"Unable to initialize Direct3D 11.",
                    L"SA:MP Error", MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    LoadUIFont();
    LoadColorSettings();
    LoadNickHistory();
    ApplyTheme();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    IconsInit();

    if (!net::Init()) {
        MessageBoxW(nullptr, L"Unable to initialize the server query socket. "
                    L"Make sure winsock is available.",
                    L"SA:MP Error", MB_ICONERROR);
    }

    EnsureUserFilesFolder();
    std::string nick = WideToUtf8(ReadRegString(HKEY_CURRENT_USER,
                                                L"SOFTWARE\\SAMP",
                                                L"PlayerName"));
    strncpy(g.nickname, nick.c_str(), sizeof(g.nickname) - 1);
    g.gtaExe = ReadRegString(HKEY_CURRENT_USER, L"SOFTWARE\\SAMP",
                             L"gta_sa_exe");

    // start on Favorites tab (like the original)
    g.masterFile = 0;
    LoadFavorites();

    {
        bool occluded = false;
        while (g_running) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT)
                    g_running = false;
            }
            if (!g_running)
                break;

            if (occluded &&
                g_pSwapChain->Present(1, 0) == DXGI_STATUS_OCCLUDED)
            {
                Sleep(10);
                continue;
            }
            occluded = false;

            if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
                CleanupRenderTarget();
                g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight,
                                            DXGI_FORMAT_UNKNOWN, 0);
                g_ResizeWidth = 0;
                g_ResizeHeight = 0;
                CreateRenderTarget();
            }

            ImGui_ImplWin32_NewFrame();
            ImGui_ImplDX11_NewFrame();
            Frame();
            ImGui::Render();

            float clear_color[4] = { 0.10f, 0.11f, 0.13f, 1.00f };
            g_pd3dDeviceContext->OMSetRenderTargets(1,
                &g_mainRenderTargetView, nullptr);
            g_pd3dDeviceContext->ClearRenderTargetView(
                g_mainRenderTargetView, clear_color);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            HRESULT hr = g_pSwapChain->Present(1, 0);
            occluded = (hr == DXGI_STATUS_OCCLUDED);
        }
    }

    // save nickname
    WriteRegString(L"PlayerName", Utf8ToWide(g.nickname));
    SaveColorSettings();
    AddNickToHistory(g.nickname);
    SaveNickHistory();
    if (g.masterFile == 0)
        SaveFavoritesNow();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    IconsShutdown();
    net::Shutdown();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(kClassName, hInstance);
    CoUninitialize();
    return 0;
}