// mgmp_lobby.cpp -- see mgmp_lobby.h.
//
// Everything a player reads here is Spanish on purpose; the log lines stay in
// English like the rest of the mod, because the log is for whoever fixes it.

#include "mgmp_lobby.h"

// winsock2 before windows.h, or windows.h drags in the old winsock.h and the
// two collide. Only needed for the adapter list (the IP to give your friend).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"

#include "mgmp_config.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_session.h"
#include "mgmp_lockstep.h"
#include "mgmp_leave.h"
#include "mgmp_savefile.h"

#pragma comment(lib, "iphlpapi.lib")

namespace mgmp {
namespace {

// --- look -------------------------------------------------------------------
//
// Mewgenics' own UI is cream card stock with a heavy dark-brown outline and
// chunky black lettering. These are eyeballed to sit next to it without
// pretending to be it.

const ImVec4 kInk      (0.16f, 0.11f, 0.09f, 1.00f);   // dark brown: text on cream
const ImVec4 kPanel    (0.17f, 0.12f, 0.10f, 0.97f);   // window body
const ImVec4 kPanelHi  (0.24f, 0.18f, 0.14f, 1.00f);   // inputs, empty slots
const ImVec4 kCream    (0.96f, 0.90f, 0.78f, 1.00f);   // outline, buttons, text
const ImVec4 kCreamDim (0.80f, 0.73f, 0.60f, 1.00f);
const ImVec4 kOrange   (0.98f, 0.66f, 0.24f, 1.00f);   // hover
const ImVec4 kOrangeDn (0.86f, 0.52f, 0.14f, 1.00f);   // pressed
const ImVec4 kGreen    (0.56f, 0.86f, 0.44f, 1.00f);
const ImVec4 kRed      (1.00f, 0.42f, 0.36f, 1.00f);

ImU32 col(const ImVec4& c, float a = -1.0f) {
    ImVec4 v = c;
    if (a >= 0.0f) v.w = a;
    return ImGui::ColorConvertFloat4ToU32(v);
}

// --- state ------------------------------------------------------------------

struct LocalIp {
    char ip[16]      = {};
    char adapter[64] = {};
    bool radmin      = false;
};

struct Lobby {
    bool inited     = false;
    bool open       = false;    // the party window
    bool on_menu    = false;    // MainMenu scene live, from the throttled poll
    uint32_t tick   = 0;
    bool said_no_scenes = false;

    char name[32]  = {};
    char join_ip[64] = {};

    // What THIS lobby started, so the window can tell "I am hosting" from a
    // session the debug panel or mgmp.json started (it shows those too, it just
    // did not cause them).
    bool started_here = false;

    std::vector<LocalIp> ips;
    ULONGLONG ips_at = 0;

    char copied_ip[16] = {};
    ULONGLONG copied_at = 0;

    wchar_t file[MAX_PATH] = {};

    ImFont* font_title = nullptr;   // Segoe UI Black, if present
    ImFont* font_body  = nullptr;   // Segoe UI Bold, if present
};

Lobby L;

// --- persistence ------------------------------------------------------------
//
// One tiny text file beside the DLL rather than mgmp.json: mgmp.json is the
// engineer's file and is written by hand, and a writer that round-trips JSON
// with comments in it is a bigger thing than a name deserves.

void locate_file() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&locate_file), &self);
    wchar_t dir[MAX_PATH] = {};
    if (!self || !GetModuleFileNameW(self, dir, MAX_PATH)) return;
    if (wchar_t* slash = wcsrchr(dir, L'\\')) *slash = 0;
    swprintf_s(L.file, L"%s\\mgmp_lobby.txt", dir);
}

void trim(char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ')) s[--n] = 0;
}

void load_file() {
    if (!L.file[0]) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, L.file, L"rb") != 0 || !f) return;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!strncmp(line, "name=", 5)) strncpy_s(L.name, line + 5, _TRUNCATE);
        else if (!strncmp(line, "ip=", 3)) strncpy_s(L.join_ip, line + 3, _TRUNCATE);
    }
    fclose(f);
}

void save_file() {
    if (!L.file[0]) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, L.file, L"wb") != 0 || !f) {
        log_line("LOBBY", "could not write mgmp_lobby.txt -- the name will not be "
                          "remembered next time");
        return;
    }
    fprintf(f, "name=%s\nip=%s\n", L.name, L.join_ip);
    fclose(f);
}

// --- the IP to give your friend ---------------------------------------------

void refresh_ips() {
    L.ips.clear();
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                    (PIP_ADAPTER_ADDRESSES)buf.data(), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                  (PIP_ADAPTER_ADDRESSES)buf.data(), &size);
    }
    if (rc != NO_ERROR) return;

    for (auto* a = (PIP_ADAPTER_ADDRESSES)buf.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const sockaddr* sa = u->Address.lpSockaddr;
            if (!sa || sa->sa_family != AF_INET) continue;
            const uint8_t* b = (const uint8_t*)&((const sockaddr_in*)sa)->sin_addr;
            if (b[0] == 127) continue;                    // loopback
            if (b[0] == 169 && b[1] == 254) continue;     // no DHCP answer
            LocalIp ip;
            _snprintf_s(ip.ip, sizeof(ip.ip), _TRUNCATE, "%u.%u.%u.%u",
                        b[0], b[1], b[2], b[3]);
            WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, ip.adapter,
                                sizeof(ip.adapter), nullptr, nullptr);
            // Radmin VPN hands out 26.x.x.x and names its adapter after itself;
            // either reading is enough.
            ip.radmin = (b[0] == 26) || strstr(ip.adapter, "Radmin") != nullptr;
            L.ips.push_back(ip);
        }
    }
    // Radmin first: that is the one a friend across the internet can reach.
    for (size_t i = 1; i < L.ips.size(); ++i)
        for (size_t j = i; j > 0 && L.ips[j].radmin && !L.ips[j - 1].radmin; --j)
            std::swap(L.ips[j], L.ips[j - 1]);
}

// --- geometry ---------------------------------------------------------------
//
// The game letterboxes to 16:9 (see "How big is the window" in CLAUDE.md), so
// everything is placed against the CONTENT rectangle and sized by its height,
// which keeps the button in the same spot over the art at any window size.

struct Content { float x, y, w, h, s; };

Content content() {
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    Content c{};
    float w = d.x, h = d.x * 9.0f / 16.0f;
    if (h > d.y) { h = d.y; w = d.y * 16.0f / 9.0f; }
    c.w = w; c.h = h;
    c.x = (d.x - w) * 0.5f;
    c.y = (d.y - h) * 0.5f;
    c.s = h / 720.0f;
    if (c.s < 0.5f) c.s = 0.5f;
    return c;
}

// --- widgets ----------------------------------------------------------------

void push_font(ImFont* f, float px) { ImGui::PushFont(f, px); }

// A cream button with dark lettering that turns orange under the mouse -- the
// one control the whole lobby is built from.
bool big_button(const char* label, ImVec2 size, bool enabled = true, ImFont* font = nullptr,
                float px = 0.0f) {
    ImGui::PushStyleColor(ImGuiCol_Button,        kCream);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kOrange);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kOrangeDn);
    ImGui::PushStyleColor(ImGuiCol_Text,          kInk);
    ImGui::PushStyleColor(ImGuiCol_Border,        kInk);
    if (font) push_font(font, px);
    ImGui::BeginDisabled(!enabled);
    const bool hit = ImGui::Button(label, size);
    ImGui::EndDisabled();
    if (font) ImGui::PopFont();
    ImGui::PopStyleColor(5);
    return hit && enabled;
}

// One party slot, World War Z style: a filled card for a player, a dashed
// empty one with a "+" for a seat nobody has taken yet.
void party_slot(const char* name, const char* tag, bool filled, ImVec2 size, float s) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    const float r = 10.0f * s;

    if (filled) {
        dl->AddRectFilled(p, q, col(kPanelHi), r);
        dl->AddRect(p, q, col(kCream), r, 0, 3.0f * s);
        // A round "avatar" with the initial, standing in for a Steam picture.
        const float ar = size.y * 0.30f;
        const ImVec2 ac(p.x + size.y * 0.42f, p.y + size.y * 0.5f);
        dl->AddCircleFilled(ac, ar, col(kCream));
        char initial[8] = "?";
        if (name && name[0]) {
            // First UTF-8 sequence, so an accented initial survives.
            int n = 1;
            const unsigned char c0 = (unsigned char)name[0];
            if (c0 >= 0xF0) n = 4; else if (c0 >= 0xE0) n = 3; else if (c0 >= 0xC0) n = 2;
            memcpy(initial, name, n); initial[n] = 0;
        }
        ImFont* f = L.font_title ? L.font_title : ImGui::GetFont();
        const float fpx = ar * 1.3f;
        const ImVec2 isz = f->CalcTextSizeA(fpx, FLT_MAX, 0.0f, initial);
        dl->AddText(f, fpx, ImVec2(ac.x - isz.x * 0.5f, ac.y - isz.y * 0.5f),
                    col(kInk), initial);

        const float tx = p.x + size.y * 0.85f;
        ImFont* fb = L.font_body ? L.font_body : ImGui::GetFont();
        dl->AddText(fb, 22.0f * s, ImVec2(tx, p.y + size.y * 0.20f), col(kCream),
                    name && name[0] ? name : "?");
        if (tag && tag[0])
            dl->AddText(fb, 15.0f * s, ImVec2(tx, p.y + size.y * 0.58f),
                        col(kOrange), tag);
    } else {
        // Dashed outline, drawn as short segments around the rounded box.
        const ImU32 c = col(kCreamDim, 0.8f);
        const float dash = 9.0f * s, gap = 7.0f * s, th = 2.0f * s;
        auto dashed = [&](ImVec2 a, ImVec2 b) {
            const float len = (a.x == b.x) ? (b.y - a.y) : (b.x - a.x);
            for (float t = 0; t < len; t += dash + gap) {
                const float e = (t + dash < len) ? t + dash : len;
                if (a.x == b.x) dl->AddLine(ImVec2(a.x, a.y + t), ImVec2(a.x, a.y + e), c, th);
                else            dl->AddLine(ImVec2(a.x + t, a.y), ImVec2(a.x + e, a.y), c, th);
            }
        };
        dashed(p, ImVec2(q.x, p.y));
        dashed(ImVec2(p.x, q.y), q);
        dashed(p, ImVec2(p.x, q.y));
        dashed(ImVec2(q.x, p.y), q);
        const ImVec2 m((p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f);
        const float a = size.y * 0.16f;
        dl->AddLine(ImVec2(m.x - a, m.y), ImVec2(m.x + a, m.y), c, 3.0f * s);
        dl->AddLine(ImVec2(m.x, m.y - a), ImVec2(m.x, m.y + a), c, 3.0f * s);
        if (tag && tag[0]) {
            ImFont* fb = L.font_body ? L.font_body : ImGui::GetFont();
            const float fpx = 14.0f * s;
            const ImVec2 ts = fb->CalcTextSizeA(fpx, FLT_MAX, 0.0f, tag);
            dl->AddText(fb, fpx, ImVec2(m.x - ts.x * 0.5f, q.y - ts.y - 6.0f * s),
                        col(kCreamDim), tag);
        }
    }
    ImGui::Dummy(size);
}

// --- what the session is doing, in a player's words -------------------------

enum class Mode { Idle, Hosting, Joining, Joined, Failed };

Mode mode() {
    const NetRole r = net_role();
    const NetState st = net_state();
    if (r == NetRole::None) return Mode::Idle;
    if (st == NetState::Failed || st == NetState::Closed) return Mode::Failed;
    if (r == NetRole::Host) return Mode::Hosting;
    return session_ready() ? Mode::Joined : Mode::Joining;
}

void leave_session() {
    session_request_disconnect();
    config_clear_role();
    L.started_here = false;
    log_line("LOBBY", "left the session from the lobby -- back to single-player");
}

// --- the title-screen button ------------------------------------------------

void draw_menu_button(const Content& c) {
    const float s = c.s;
    const ImVec2 size(250.0f * s, 62.0f * s);
    const float margin = 28.0f * s;
    ImGui::SetNextWindowPos(ImVec2(c.x + c.w - margin - size.x, c.y + margin));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_AlwaysAutoResize |
                          ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 4.0f * s);
    if (ImGui::Begin("##mgmp_mp_button", nullptr, fl)) {
        // In a session already (say the window was closed), the button reopens
        // it and says so, rather than inviting a second host.
        const Mode m = mode();
        const char* label = (m == Mode::Hosting || m == Mode::Joined || m == Mode::Joining)
                                ? "PARTY ABIERTA##mp" : "MULTIJUGADOR##mp";
        if (big_button(label, size, true, L.font_title, 30.0f * s)) L.open = true;
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
}

// --- the party window -------------------------------------------------------

void text_col(const ImVec4& c, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextWrappedV(fmt, ap);
    ImGui::PopStyleColor();
    va_end(ap);
}

void draw_party(const Content& c) {
    const float s = c.s;
    const float W = 640.0f * s;
    ImGui::SetNextWindowPos(ImVec2(c.x + c.w * 0.5f, c.y + c.h * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(W, 0), ImVec2(W, c.h * 0.95f));

    ImGui::PushStyleColor(ImGuiCol_WindowBg,       kPanel);
    ImGui::PushStyleColor(ImGuiCol_Border,         kCream);
    ImGui::PushStyleColor(ImGuiCol_Text,           kCream);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled,   kCreamDim);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        kPanelHi);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.30f, 0.23f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0.34f, 0.26f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Separator,      kCreamDim);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,   ImVec2(26.0f * s, 22.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,  16.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 4.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,   9.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,    ImVec2(12.0f * s, 8.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,     ImVec2(12.0f * s, 12.0f * s));
    if (L.font_body) push_font(L.font_body, 20.0f * s);

    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##mgmp_party", nullptr, fl)) {
        const float inner = W - 52.0f * s;

        // Title and the close cross.
        push_font(L.font_title, 38.0f * s);
        ImGui::TextUnformatted("MULTIJUGADOR");
        ImGui::PopFont();
        // SameLine's x is from the window's left edge, padding included.
        ImGui::SameLine(26.0f * s + inner - 44.0f * s);
        if (big_button("X##close", ImVec2(44.0f * s, 44.0f * s), true, L.font_title, 22.0f * s))
            L.open = false;
        ImGui::Separator();

        const Mode m = mode();
        const bool in_session = (m == Mode::Hosting || m == Mode::Joining || m == Mode::Joined);
        const bool busy = session_request_pending();

        // --- name ---
        ImGui::TextUnformatted("Tu nombre");
        ImGui::SetNextItemWidth(inner);
        ImGui::BeginDisabled(in_session);
        if (ImGui::InputText("##name", L.name, sizeof(L.name)))
            session_set_local_name(L.name);
        ImGui::EndDisabled();
        if (in_session)
            ImGui::TextDisabled("(para cambiarlo, sal de la party)");

        const ImVec2 slot(inner * 0.5f - 6.0f * s, 86.0f * s);
        char other[64] = {};
        const bool have_other = session_other_player(other, sizeof(other));

        if (m == Mode::Idle || m == Mode::Failed) {
            if (m == Mode::Failed) {
                const char* err = net_error();
                if (net_state() == NetState::Closed)
                    text_col(kRed, "Se perdió la conexión con %s.",
                             net_role() == NetRole::Client ? "el host" : "tu amigo");
                else
                    text_col(kRed, "No se pudo conectar%s%s", err && err[0] ? ": " : ".",
                             err && err[0] ? err : "");
                if (big_button("VOLVER", ImVec2(inner, 46.0f * s), !busy)) leave_session();
            } else {
                ImGui::Dummy(ImVec2(0, 4.0f * s));
                if (big_button("CREAR PARTIDA", ImVec2(inner, 58.0f * s), !busy && L.name[0],
                               L.font_title, 26.0f * s)) {
                    session_set_local_name(L.name);
                    save_file();
                    session_request_host(config().net_port);
                    L.started_here = true;
                    L.ips_at = 0;          // refresh the list right away
                    log_line("LOBBY", "hosting from the lobby as '%s'", L.name);
                }
                ImGui::Dummy(ImVec2(0, 2.0f * s));
                ImGui::TextUnformatted("...o únete a la partida de tu amigo:");
                ImGui::SetNextItemWidth(inner - 150.0f * s);
                ImGui::InputTextWithHint("##ip", "IP del host (ej. 26.123.45.67)",
                                         L.join_ip, sizeof(L.join_ip),
                                         ImGuiInputTextFlags_CharsNoBlank);
                ImGui::SameLine();
                if (big_button("UNIRSE", ImVec2(138.0f * s, 0), !busy && L.join_ip[0] && L.name[0])) {
                    session_set_local_name(L.name);
                    save_file();
                    session_request_join(L.join_ip, config().net_port);
                    L.started_here = true;
                    log_line("LOBBY", "joining %s from the lobby as '%s'", L.join_ip, L.name);
                }
                if (!L.name[0]) text_col(kOrange, "Escribe tu nombre primero.");
                ImGui::TextDisabled("Con Radmin VPN, usa la IP de Radmin del host (empieza por 26.).");
            }
        } else if (m == Mode::Hosting) {
            party_slot(session_local_name(), "TÚ  ·  HOST", true, slot, s);
            ImGui::SameLine();
            party_slot(have_other ? other : nullptr, have_other ? "CONECTADO" : "ESPERANDO...",
                       have_other, slot, s);

            if (have_other) text_col(kGreen, "¡%s se unió a la party!", other);
            else            ImGui::TextUnformatted("Pásale una de estas IP a tu amigo para que se una:");

            const ULONGLONG now = GetTickCount64();
            if (!L.ips_at || now - L.ips_at > 3000) { refresh_ips(); L.ips_at = now; }
            if (!have_other) {
                if (L.ips.empty())
                    text_col(kOrange, "No encontré ninguna red activa.");
                for (size_t i = 0; i < L.ips.size(); ++i) {
                    const LocalIp& ip = L.ips[i];
                    ImGui::PushID((int)i);
                    push_font(L.font_title, 22.0f * s);
                    ImGui::TextUnformatted(ip.ip);
                    ImGui::PopFont();
                    ImGui::SameLine();
                    if (ip.radmin) text_col(kGreen, "Radmin");
                    else           ImGui::TextDisabled("%s", ip.adapter);
                    ImGui::SameLine(26.0f * s + inner - 110.0f * s);
                    const bool just = !strcmp(L.copied_ip, ip.ip) && now - L.copied_at < 1500;
                    if (big_button(just ? "¡Copiada!" : "Copiar", ImVec2(110.0f * s, 0))) {
                        ImGui::SetClipboardText(ip.ip);
                        strncpy_s(L.copied_ip, ip.ip, _TRUNCATE);
                        L.copied_at = now;
                    }
                    ImGui::PopID();
                }
                ImGui::TextDisabled("Puedes empezar ya; tu amigo puede unirse después.");
            }

            ImGui::Dummy(ImVec2(0, 4.0f * s));
            const float bw = (inner - 12.0f * s) * 0.5f;
            if (big_button("COMENZAR", ImVec2(bw, 56.0f * s), !busy, L.font_title, 26.0f * s)) {
                savefile_request_local_play();
                L.open = false;
                log_line("LOBBY", "Comenzar pressed -- clicking Play for the host");
            }
            ImGui::SameLine();
            if (big_button("SALIR", ImVec2(bw, 56.0f * s), !busy, L.font_title, 26.0f * s))
                leave_session();
        } else {   // Joining / Joined
            char host[64] = {};
            const bool have_host = session_other_player(host, sizeof(host));
            party_slot(have_host ? host : nullptr, have_host ? "HOST" : "CONECTANDO...",
                       have_host, slot, s);
            ImGui::SameLine();
            party_slot(session_local_name(), "TÚ", true, slot, s);

            if (m == Mode::Joining)
                ImGui::Text("Conectando con %s...", L.join_ip);
            else
                text_col(kGreen, "¡Conectado! Espera a que %s empiece la partida; "
                                 "el juego entrará solo.", have_host ? host : "el host");

            ImGui::Dummy(ImVec2(0, 4.0f * s));
            if (big_button("SALIR", ImVec2(inner, 52.0f * s), !busy, L.font_title, 24.0f * s))
                leave_session();
        }
    }
    ImGui::End();

    if (L.font_body) ImGui::PopFont();
    ImGui::PopStyleVar(7);
    ImGui::PopStyleColor(8);
}

// --- in-game indicators -----------------------------------------------------

// A small pill in the corner so nobody has to wonder whether the other player
// is still there, and a banner while one of THEIR cats is acting.
void draw_hud(const Content& c) {
    const Mode m = mode();
    if (m == Mode::Idle) return;
    const float s = c.s;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* fb = L.font_body ? L.font_body : ImGui::GetFont();

    char other[64] = {};
    const bool have_other = session_other_player(other, sizeof(other));
    char line[128];
    ImVec4 dot = kGreen;
    if (m == Mode::Failed) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "Co-op: conexión perdida");
        dot = kRed;
    } else if (have_other) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "Co-op con %s", other);
    } else if (m == Mode::Hosting) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "Co-op: esperando a tu amigo");
        dot = kOrange;
    } else {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "Co-op: conectando...");
        dot = kOrange;
    }

    const float px = 16.0f * s;
    const ImVec2 ts = fb->CalcTextSizeA(px, FLT_MAX, 0.0f, line);
    const ImVec2 p(c.x + 12.0f * s, c.y + c.h - ts.y - 26.0f * s);
    const ImVec2 q(p.x + ts.x + 40.0f * s, p.y + ts.y + 14.0f * s);
    dl->AddRectFilled(p, q, col(kPanel, 0.80f), (q.y - p.y) * 0.5f);
    dl->AddCircleFilled(ImVec2(p.x + 15.0f * s, (p.y + q.y) * 0.5f), 5.0f * s, col(dot));
    dl->AddText(fb, px, ImVec2(p.x + 28.0f * s, p.y + 7.0f * s), col(kCream), line);

    // Whose turn. Only the OTHER player's is announced in big letters -- your
    // own turn is obvious from the game's own UI, which is waiting for you.
    if (lockstep_peer_actor()) {
        char banner[128];
        _snprintf_s(banner, sizeof(banner), _TRUNCATE, "Es el turno de %s",
                    have_other ? other : "tu amigo");
        ImFont* ft = L.font_title ? L.font_title : fb;
        const float bpx = 30.0f * s;
        const ImVec2 bs = ft->CalcTextSizeA(bpx, FLT_MAX, 0.0f, banner);
        const ImVec2 a(c.x + (c.w - bs.x) * 0.5f - 24.0f * s, c.y + 70.0f * s);
        const ImVec2 b(a.x + bs.x + 48.0f * s, a.y + bs.y + 18.0f * s);
        dl->AddRectFilled(a, b, col(kPanel, 0.88f), 12.0f * s);
        dl->AddRect(a, b, col(kCream), 12.0f * s, 0, 3.0f * s);
        dl->AddText(ft, bpx, ImVec2(a.x + 24.0f * s, a.y + 9.0f * s), col(kCream), banner);
    } else if (lockstep_local_actor() && have_other) {
        const char* mine = "Tu turno";
        ImFont* ft = L.font_title ? L.font_title : fb;
        const float bpx = 22.0f * s;
        const ImVec2 bs = ft->CalcTextSizeA(bpx, FLT_MAX, 0.0f, mine);
        const ImVec2 a(c.x + (c.w - bs.x) * 0.5f - 18.0f * s, c.y + 74.0f * s);
        const ImVec2 b(a.x + bs.x + 36.0f * s, a.y + bs.y + 12.0f * s);
        dl->AddRectFilled(a, b, col(kPanel, 0.75f), 10.0f * s);
        dl->AddText(ft, bpx, ImVec2(a.x + 18.0f * s, a.y + 6.0f * s), col(kGreen), mine);
    }
}

} // namespace

// ---------------------------------------------------------------------------

void lobby_init() {
    if (L.inited) return;
    L.inited = true;
    locate_file();
    load_file();
    if (!L.name[0]) {
        char user[32] = {};
        DWORD n = GetEnvironmentVariableA("USERNAME", user, sizeof(user));
        if (n && n < sizeof(user)) strncpy_s(L.name, user, _TRUNCATE);
    }
    session_set_local_name(L.name);
}

void lobby_setup_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    // The built-in font stays FIRST, so it stays the default -- the debug panel
    // is laid out for it and should not change because the lobby exists.
    io.Fonts->AddFontDefault();

    wchar_t win[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(win, MAX_PATH)) return;
    auto add = [&](const wchar_t* file, float px) -> ImFont* {
        wchar_t w[MAX_PATH];
        swprintf_s(w, L"%s\\Fonts\\%s", win, file);
        if (GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES) return nullptr;
        char u8[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, sizeof(u8), nullptr, nullptr);
        return io.Fonts->AddFontFromFileTTF(u8, px);
    };
    L.font_title = add(L"seguibl.ttf", 30.0f);            // Segoe UI Black
    if (!L.font_title) L.font_title = add(L"ariblk.ttf", 30.0f);   // Arial Black
    L.font_body  = add(L"segoeuib.ttf", 20.0f);           // Segoe UI Bold
    if (!L.font_body) L.font_body = add(L"arialbd.ttf", 20.0f);
    log_line("LOBBY", "fonts: title %s, body %s", L.font_title ? "ok" : "built-in",
             L.font_body ? "ok" : "built-in");
}

bool lobby_wants_frame() {
    // A scene walk is a pointer chase per loaded scene; a few times a second is
    // plenty for a button that appears when a menu does.
    if ((L.tick++ % 15) == 0) {
        L.on_menu = leave_scene_live("MainMenu");
        if (!L.on_menu && !L.said_no_scenes && L.tick > 60 * 60 && net_role() == NetRole::None) {
            // A minute in and never a MainMenu: either nobody went back to the
            // title, or the scene walk does not read on this build. Say it once
            // so "where is the button" has an answer in the log.
            L.said_no_scenes = true;
            log_line("LOBBY", "no MainMenu scene seen in the first minute -- if the "
                              "title screen IS up, the scene walk is not reading and the "
                              "MULTIJUGADOR button cannot appear (F1 has host/join)");
        }
    }
    return L.on_menu || L.open || net_role() != NetRole::None;
}

void lobby_draw() {
    const Content c = content();
    if (L.on_menu) {
        if (!L.open) draw_menu_button(c);
        else         draw_party(c);
    } else {
        // Off the title screen the window has no business being open: the next
        // time the menu comes back it starts closed.
        L.open = false;
    }
    if (!L.on_menu) draw_hud(c);
}

} // namespace mgmp
