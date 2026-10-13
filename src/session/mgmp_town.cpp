// mgmp_town.cpp -- see mgmp_town.h.

#include "mgmp_town.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_leave.h"
#include "mgmp_rng.h"
#include "mgmp_follow.h"

#include <windows.h>
#include <cstring>

namespace mgmp {
namespace {

// Button::Click is `impl = *(Button+240); if (impl) (*(*impl + 16))(impl);`
// -- slot 2 of the std::function _Impl is _Do_call, and the lambda's captures
// start at impl+8. CLAUDE.md, "The main menu".
constexpr uintptr_t kBtn_Impl   = 240;
constexpr uintptr_t kImpl_DoCall = 16;
// HouseDrawerPanel::init's lambda_1 (the drawer's tab) captures the panel at
// impl+0x10; the panel keeps its own name at +0x88 ("cats", "build", "items",
// "map"), the std::string HouseDrawerPanel::init copies there.
constexpr uintptr_t kImpl_Panel = 0x10;
constexpr uintptr_t kPanel_Name = 0x88;
// Is a drawer open: sub_140204800 walks the panel's parents (+0x68) to the root,
// then asks the drawer manager -- the first component of type 0x44C in the
// root's scene (sub_1400ED390: *(entity+8) is the scene, entity = root+0x18) --
// whether its +0x58 is that root.
constexpr uintptr_t kPanel_Parent = 0x68;
constexpr uintptr_t kComp_Entity  = 0x18;
constexpr uintptr_t kEntity_Scene = 0x08;
constexpr int       kType_DrawerMgr = 0x44C;
constexpr uintptr_t kMgr_Open     = 0x58;

// NPCDialogController (CLAUDE.md, "Hook points" -> DLGUPDATE / DLGBUBBLE).
constexpr uintptr_t kDlg_Text     = 0x48;    // the bubble's text: +0x1A8 revealing,
constexpr uintptr_t kText_Reveal  = 0x1A8;   //   +0x1AA "finish it now"
constexpr uintptr_t kText_Skip    = 0x1AA;
constexpr uintptr_t kDlg_Auto     = 0x60;    // lines close by themselves
constexpr uintptr_t kDlg_Step     = 0xF4;
constexpr uintptr_t kDlg_Closed   = 0xF8;
constexpr uintptr_t kDlg_WaitFor  = 0x108;   // std::string
constexpr uintptr_t kCmd_Name     = 0x88;    // the command's std::string name
constexpr uintptr_t kScene_NoInput = 0x4DC;
constexpr uintptr_t kComp_Scene    = 0x20;    // the scene a component lives in
constexpr uintptr_t kScene_Name    = 1208;    // std::string (CLAUDE.md, scene list)

constexpr uint32_t  kQueue        = 32;
constexpr uint64_t  kPressGapMs   = 350;
constexpr uint64_t  kPressDropMs  = 20000;
constexpr uint64_t  kDlgDropMs    = 30000;
constexpr uint32_t  kOrdSlots     = 128;
// A dialog line open this long here, with nothing from the host to close it,
// is one the host is not showing: let this player's own click through rather
// than leave them on it forever.
constexpr uint64_t  kDlgValveMs   = 40000;

// Way      leaving the house: the NPC map drawer, an NPC, the depart sign
// Office   inside an NPC's office -- the host presses
// InvOpen  the map's backpack; Inv inside it
// Choose   the class chooser (ClassChooser scene, ClassTagBox collars,
//          CatSelector arrows)
// Start    the class chooser's start / its colorless-cat confirm: the host's
//          starts the adventure; on a client it is never run -- the client
//          loads the host's run when it is published, as after a day
// RunShop  an adventure shop node: the host shops, the client watches
enum class Kind : uint8_t { None, Way, Office, InvOpen, Inv, Choose, Start, RunShop };

// What a callback's class is, read from its own RTTI once per vtable.
enum class Cls : uint8_t { Unknown, Other, Drawer, NpcMap, Chooser, ChooserStart, Shop };
struct ClsCache { uintptr_t vt; Cls cls; };

struct Key {
    uint32_t vt_rva = 0;
    uint16_t ordinal = 0;
    char     name[40] = {};
    char     tag[16]  = {};
};

struct Pending {
    TownPressMsg m;
    ULONGLONG    at = 0;
};

struct DlgPending {
    DialogMsg m;
    ULONGLONG at = 0;
};

using fn_scene_list = void (__fastcall*)(void* scene, int type);

struct State {
    uintptr_t base = 0;
    fn_scene_list scene_list = nullptr;
    uintptr_t cur_scene = 0;          // address of the current-scene global

    ClsCache  cls[64];
    uint32_t  ncls = 0;

    // ordinals: this frame's count of buttons per kind
    uint32_t  frame = 0, ord_frame = ~0u;
    uint64_t  ord_key[kOrdSlots] = {};
    uint16_t  ord_n[kOrdSlots] = {};
    uint32_t  ord_used = 0;
    void*     cur_btn = nullptr;
    uint16_t  cur_ord = 0;
    Cls       cur_cls = Cls::Unknown;

    bool      office = false;          // the NPCOffice scene is up here
    // The run's inventory (the CatStatus screen, opened from the map's
    // backpack). Shared only when it was opened as a shared press -- one
    // opened from a level-up screen exists on one machine and must not
    // forward its presses to a peer that has no such screen.
    bool      inv_shared = false;
    ULONGLONG inv_since  = 0;
    // Departing: from the class chooser's start until the equip screen it
    // leads to (MewDirector::DoHouseStorageEquipFlow -> scene StorageItems,
    // an InventoryScreen2 -- the backpack's own screen) has come and gone.
    // The chooser is torn down and a swipe runs before that screen exists, so
    // a 3-second grace was not enough to carry the sharing across.
    ULONGLONG depart_at = 0;
    bool      depart_saw_items = false;
    bool      injecting = false;

    Pending   pq[kQueue];
    uint32_t  pq_n = 0;
    ULONGLONG press_last = 0;

    DlgPending dq[kQueue];
    uint32_t   dq_n = 0;

    // host: what the dialog looked like before the original ran
    bool      pre_wait[2] = {};
    uint8_t   pre_closed = 0, pre_skip = 0;
    bool      pre_dialog = false;

    // client: the input blocks raised around dialog updates
    uintptr_t blk_scene[4] = {};
    uint8_t   blk_saved[4] = {};
    uint32_t  blk_depth = 0;

    // client: a line that has stood open with nothing from the host
    ULONGLONG line_since = 0;

    // client: the host started the adventure from the class chooser; this
    // peer waits for the host's run instead of starting its own
    ULONGLONG awaiting_run = 0;
    // client: a press in the host's shop was refused (for the notice)
    ULONGLONG shop_refused_at = 0;
    uint32_t  shop_refused = 0;
    bool      said_valve = false;

    uint32_t  sent = 0, done = 0, asked = 0, dropped = 0;
    uint32_t  dlg_sent = 0, dlg_done = 0, dlg_dropped = 0;
    bool      said_block = false;
};

State g;

bool is_host()   { return net_active() && net_role() == NetRole::Host; }
bool is_client() { return net_active() && net_role() == NetRole::Client; }

uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ULL) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

// .?AV<lambda_1>@?6??init@HouseDrawerPanel@glaiel@@... -- the enclosing
// function's class is in the lambda's own type name.
Cls class_of(uintptr_t vt) {
    if (!vt || !g.base) return Cls::Other;
    for (uint32_t i = 0; i < g.ncls; ++i) if (g.cls[i].vt == vt) return g.cls[i].cls;
    Cls c = Cls::Other;
    uintptr_t col = 0;
    uint32_t td_rva = 0;
    if (mem_read((const void*)(vt - 8), &col, sizeof(col)) && col &&
        mem_read((const void*)(col + 12), &td_rva, sizeof(td_rva)) && td_rva) {
        char nm[200] = {};
        if (mem_read((const void*)(g.base + td_rva + 0x10), nm, sizeof(nm) - 1)) {
            nm[sizeof(nm) - 1] = 0;
            if (strstr(nm, "HouseDrawerPanel")) c = Cls::Drawer;
            // HouseDrawerUI::InitAux's two lambdas are the house's own shop
            // buttons (vt 0x140EFE880 -> NPCMapDrawer::jackshop_shortcut,
            // 0x140EFE8F0 -> tracyshop_shortcut): the same GoToNPCOffice the
            // NPC map ends in, so the same way out.
            else if (strstr(nm, "InitAux@HouseDrawerUI")) c = Cls::NpcMap;
            else if (strstr(nm, "NPCMapDrawer")) c = Cls::NpcMap;
            // ClassChooser::init's lambda_1 (the start button, vt 0x140EE2250)
            // and the colorless-cat confirm nested inside it (0x140EE22C0)
            // both carry "init@ClassChooser" in their type names.
            else if (strstr(nm, "init@ClassChooser")) c = Cls::ChooserStart;
            else if (strstr(nm, "ClassChooser") || strstr(nm, "ClassTagBox") ||
                     strstr(nm, "ActSelectionScreen")) c = Cls::Chooser;
            // Shop::refresh_shopitem (Shop_BuyButton), Shop::initShop
            // (Shop_ExitButton), Shop::init (a chest's items).
            else if (strstr(nm, "@Shop@glaiel")) c = Cls::Shop;
        }
    }
    if (g.ncls < 64) g.cls[g.ncls++] = ClsCache{ vt, c };
    return c;
}

// 1 open, 0 closed, -1 could not tell.
int drawer_open(void* impl) {
    void* panel = nullptr;
    if (!g.scene_list || !impl ||
        !mem_read((const uint8_t*)impl + kImpl_Panel, &panel, sizeof(panel)) || !panel)
        return -1;
    for (int i = 0; i < 8; ++i) {
        void* up = nullptr;
        if (!mem_read((const uint8_t*)panel + kPanel_Parent, &up, sizeof(up))) return -1;
        if (!up) break;
        panel = up;
    }
    void* ent = nullptr; void* scene = nullptr; void* table = nullptr; void* list = nullptr;
    void* data = nullptr; void* mgr = nullptr; void* open = nullptr;
    uint32_t n = 0;
    if (!mem_read((const uint8_t*)panel + kComp_Entity, &ent, sizeof(ent)) || !ent) return -1;
    if (!mem_read((const uint8_t*)ent + kEntity_Scene, &scene, sizeof(scene)) || !scene) return -1;
    g.scene_list(scene, kType_DrawerMgr);
    if (!mem_read((const uint8_t*)scene + 0x20, &table, sizeof(table)) || !table) return -1;
    if (!mem_read((const uint8_t*)table + (uintptr_t)kType_DrawerMgr * 0x10, &list, sizeof(list)) ||
        !list) return -1;
    if (!mem_read((const uint8_t*)list + 0xC, &n, sizeof(n)) || !n) return -1;
    if (!mem_read((const uint8_t*)list + 0x10, &data, sizeof(data)) || !data) return -1;
    if (!mem_read(data, &mgr, sizeof(mgr)) || !mgr) return -1;
    if (!mem_read((const uint8_t*)mgr + kMgr_Open, &open, sizeof(open))) return -1;
    return open == panel ? 1 : 0;
}

// The state a press of this drawer tab leaves it in, as TownPressMsg::want.
uint8_t want_after_press(void* impl) {
    const int st = drawer_open(impl);
    return st < 0 ? 0 : (st ? 2 : 1);
}

// Already where the press wants it?
bool drawer_there(void* impl, uint8_t want) {
    if (!want) return false;
    const int st = drawer_open(impl);
    return st >= 0 && (st == 1) == (want == 1);
}

void* impl_of(void* button) {
    void* impl = nullptr;
    if (!mem_read((const uint8_t*)button + kBtn_Impl, &impl, sizeof(impl))) return nullptr;
    return impl;
}

// The kind-identity of a button: callback class, name, tag. No ordinal yet.
bool key_of(void* button, Key& k, Cls& cls) {
    k = Key{};
    cls = Cls::Unknown;
    void* impl = impl_of(button);
    uintptr_t vt = 0;
    if (!impl || !mem_read(impl, &vt, sizeof(vt)) || !vt || vt <= g.base) return false;
    k.vt_rva = (uint32_t)(vt - g.base);
    if (!mem_read_std_string((const uint8_t*)button + kBtn_Name, k.name, sizeof(k.name)))
        k.name[0] = 0;
    cls = class_of(vt);
    if (cls == Cls::Drawer) {
        void* panel = nullptr;
        if (mem_read((const uint8_t*)impl + kImpl_Panel, &panel, sizeof(panel)) && panel)
            mem_read_std_string((const uint8_t*)panel + kPanel_Name, k.tag, sizeof(k.tag));
    }
    return true;
}

uint64_t kind_hash(const Key& k) {
    uint64_t h = fnv(&k.vt_rva, sizeof(k.vt_rva));
    h = fnv(k.name, strlen(k.name), h);
    return fnv(k.tag, strlen(k.tag), h);
}

// Only the office's own buttons: the house's drawers underneath stay each
// player's own even while an office is up.
bool scene_name(void* button, char* nm, size_t cap) {
    void* scene = nullptr;
    nm[0] = 0;
    return mem_read((const uint8_t*)button + kComp_Scene, &scene, sizeof(scene)) && scene &&
           mem_read_std_string((const uint8_t*)scene + kScene_Name, nm, cap);
}

bool in_office_scene(void* button) {
    char nm[24];
    return scene_name(button, nm, sizeof(nm)) && strcmp(nm, "NPCOffice") == 0;
}

// The map's backpack, from the map itself -- the same button name is built on
// the level-up screen too (another callback), and that one is a single
// player's.
bool is_backpack(void* button, const Key& k) {
    if (strcmp(k.name, "Map_Backpack") != 0) return false;
    char nm[24];
    return scene_name(button, nm, sizeof(nm)) && strcmp(nm, "Map") == 0 &&
           !leave_scene_live("LevelUp");
}

bool in_inventory_scene(void* button) {
    char nm[24];
    return scene_name(button, nm, sizeof(nm)) &&
           (strcmp(nm, "CatStatus") == 0 || strcmp(nm, "StorageItems") == 0);
}

// An adventure's shop node, standing on the map with the house gone. A chest
// (treasure/furniturebox/foodbox) is also a Shop scene, and is mgmp_choice's.
bool in_run_shop(void* button) {
    if (follow_here_type() != 12) return false;
    if (leave_scene_live("House") || !leave_scene_live("Map")) return false;
    char nm[24];
    return scene_name(button, nm, sizeof(nm)) && strcmp(nm, "Shop") == 0;
}

// A shop opened from the house (Jack's, Tracy's furniture): one wallet and one
// stock, so the host presses, the same as anything else in an office. Known by
// the button's own callback (Shop::refresh_shopitem, Shop::initShop, ...) as
// well as by its scene -- whatever scene the office builds the shop in.
bool in_house_shop(void* button, Cls cls) {
    if (!leave_scene_live("House")) return false;
    if (cls == Cls::Shop) return true;
    char nm[24];
    return scene_name(button, nm, sizeof(nm)) && strcmp(nm, "Shop") == 0;
}

bool in_chooser_scene(void* button) {
    char nm[24];
    return scene_name(button, nm, sizeof(nm)) &&
           (strcmp(nm, "ClassChooser") == 0 || strcmp(nm, "ActSelection") == 0);
}

Kind kind_of(void* button, const Key& k, Cls cls) {
    if (cls == Cls::Drawer) return strcmp(k.tag, "map") == 0 ? Kind::Way : Kind::None;
    if (cls == Cls::NpcMap) return Kind::Way;
    if (strcmp(k.name, "Depart_Sign") == 0) return Kind::Way;
    if (cls == Cls::ChooserStart) return Kind::Start;
    if (cls == Cls::Chooser || in_chooser_scene(button)) return Kind::Choose;
    if (in_run_shop(button)) return Kind::RunShop;
    if (g.office && in_office_scene(button)) return Kind::Office;
    if (in_house_shop(button, cls)) return Kind::Office;
    if (is_backpack(button, k)) return Kind::InvOpen;
    if (g.inv_shared && in_inventory_scene(button) &&
        strncmp(k.name, "Button_PauseMenu_", 17) != 0)
        return Kind::Inv;
    return Kind::None;
}

void depart_started() {
    if (!g.depart_at)
        log_line("TOWN", "classes chosen -- the cats' items are next, shared: either player"
                         " equips, every press through the host");
    g.depart_at = GetTickCount64();
    g.depart_saw_items = false;
}

void inv_opened() {
    if (!g.inv_shared)
        log_line("TOWN", "the backpack opens for both players -- either can equip; every"
                         " press goes through the host so both see the same");
    g.inv_shared = true;
    g.inv_since = GetTickCount64();
}

bool same(const TownPressMsg& m, const Key& k) {
    return m.vt_rva == k.vt_rva && m.ordinal == k.ordinal &&
           strcmp(m.name, k.name) == 0 && strcmp(m.tag, k.tag) == 0;
}

void fill(TownPressMsg& m, const Key& k) {
    m.vt_rva = k.vt_rva;
    m.ordinal = k.ordinal;
    memcpy(m.name, k.name, sizeof(m.name));
    memcpy(m.tag, k.tag, sizeof(m.tag));
}

void pq_pop() {
    if (!g.pq_n) return;
    memmove(&g.pq[0], &g.pq[1], (g.pq_n - 1) * sizeof(Pending));
    --g.pq_n;
}

void dq_pop() {
    if (!g.dq_n) return;
    memmove(&g.dq[0], &g.dq[1], (g.dq_n - 1) * sizeof(DlgPending));
    --g.dq_n;
}

// Run the button's own callback: exactly what Button::Click does after its
// guards, without the guards -- the press already passed them on the peer that
// made it.
bool run_callback(void* button) {
    void* impl = impl_of(button);
    uintptr_t vt = 0, fn = 0;
    if (!impl || !mem_read(impl, &vt, sizeof(vt)) || !vt) return false;
    if (!mem_read((const void*)(vt + kImpl_DoCall), &fn, sizeof(fn)) || !fn) return false;
    g.injecting = true;
    ((void(__fastcall*)(void*))fn)(impl);
    g.injecting = false;
    return true;
}

void send_press(const Key& k, uint8_t request, bool with_rng, uint8_t want = 0) {
    TownPressMsg m{};
    fill(m, k);
    m.request = request;
    m.want = want;
    if (with_rng) {
        if (uint64_t* st = rng_global_stream()) {
            memcpy(m.rng, st, sizeof(m.rng));
            m.has_rng = 1;
        }
    }
    if (net_send_townpress(m)) {
        if (request) ++g.asked; else ++g.sent;
    }
}

// --- dialogs ------------------------------------------------------------------

uint8_t dlg_class(void* ctrl) {
    uintptr_t vt = 0;
    if (!mem_read(ctrl, &vt, sizeof(vt)) || !vt) return 0;
    uintptr_t col = 0;
    uint32_t td_rva = 0;
    char nm[64] = {};
    if (mem_read((const void*)(vt - 8), &col, sizeof(col)) && col &&
        mem_read((const void*)(col + 12), &td_rva, sizeof(td_rva)) && td_rva &&
        mem_read((const void*)(g.base + td_rva + 0x10), nm, sizeof(nm) - 1))
        return strstr(nm, "Tutorial") ? 1 : 0;
    return 0;
}

bool wait_is_click(void* ctrl) {
    char w[16];
    return mem_read_std_string((const uint8_t*)ctrl + kDlg_WaitFor, w, sizeof(w)) &&
           strcmp(w, "click") == 0;
}

void clear_wait(void* ctrl) {
    uint8_t* s = (uint8_t*)ctrl + kDlg_WaitFor;
    uint64_t cap = 0;
    if (!mem_read(s + 24, &cap, sizeof(cap))) return;
    uint8_t* chars = s;
    if (cap > 15 && !mem_read(s, &chars, sizeof(chars))) return;
    const uint64_t zero = 0;
    const uint8_t nul = 0;
    mem_write(s + 16, &zero, sizeof(zero));
    if (chars) mem_write(chars, &nul, 1);
}

void* text_of(void* ctrl) {
    void* t = nullptr;
    mem_read((const uint8_t*)ctrl + kDlg_Text, &t, sizeof(t));
    return t;
}

uint8_t rd8(const void* p, uintptr_t off) {
    uint8_t v = 0;
    if (p) mem_read((const uint8_t*)p + off, &v, 1);
    return v;
}

void wr8(void* p, uintptr_t off, uint8_t v) {
    if (p) mem_write((uint8_t*)p + off, &v, 1);
}

bool cmd_is_dialog(void* cmd) {
    char n[16];
    return cmd && mem_read_std_string((const uint8_t*)cmd + kCmd_Name, n, sizeof(n)) &&
           strcmp(n, "dialog") == 0;
}

void send_dialog(uint8_t op, uint8_t cls, void* ctrl) {
    DialogMsg m{};
    m.op = op;
    m.cls = cls;
    mem_read((const uint8_t*)ctrl + kDlg_Step, &m.step, sizeof(m.step));
    if (net_send_dialog(m)) ++g.dlg_sent;
}

} // namespace

void town_set_base(uintptr_t base) {
    g.base = base;
    g.scene_list = (fn_scene_list)addr_of_call(C_SceneList);
    g.cur_scene = addr_of_data(D_CurScene);
    if (!g.cur_scene)
        log_line("TOWN", "!! the current-scene global did not resolve -- the other"
                         " player's clicks on dialogs cannot be held back");
}

bool town_in_office() { return g.office; }
bool town_awaiting_run() { return g.awaiting_run != 0; }
bool town_presses_pending() { return g.pq_n != 0; }

// T_RunStart / T_RunStartScripted. A client goes through the chooser, the
// items and the act screen with the host, but its adventure is always the
// host's, loaded from the host's save once it is on the map -- the same reload
// the end of the day uses. So the press that would start this machine's own
// run does everything up to that, and stops there.
bool town_run_start_allowed() {
    if (!is_client()) return true;
    g.awaiting_run = GetTickCount64();
    g.inv_shared = false;
    g.depart_at = 0;
    log_line("TOWN", "the host is starting the adventure -- this player's own run is not"
                     " started; the host's loads here as soon as it is on the map");
    return false;
}
bool town_shop_refused_recently() {
    return g.shop_refused_at && GetTickCount64() - g.shop_refused_at < 4000;
}

// --- buttons --------------------------------------------------------------------

void town_on_button_pre(void* button) {
    g.cur_btn = nullptr;
    if (!button || !net_active()) return;
    if (g.ord_frame != g.frame) {
        g.ord_frame = g.frame;
        g.ord_used = 0;
    }
    Key k; Cls cls;
    if (!key_of(button, k, cls)) return;
    const uint64_t h = kind_hash(k);
    uint16_t ord = 0;
    uint32_t i = 0;
    for (; i < g.ord_used; ++i) if (g.ord_key[i] == h) break;
    if (i < g.ord_used) ord = g.ord_n[i]++;
    else if (g.ord_used < kOrdSlots) { g.ord_key[i] = h; g.ord_n[i] = 1; ++g.ord_used; }
    g.cur_btn = button;
    g.cur_ord = ord;
    g.cur_cls = cls;
}

bool town_on_button_click(void* button) {
    if (!button || !net_active() || g.injecting) return true;
    Key k; Cls cls;
    if (!key_of(button, k, cls)) return true;
    const Kind kind = kind_of(button, k, cls);
    if (kind == Kind::None) return true;
    k.ordinal = (g.cur_btn == button) ? g.cur_ord : 0;
    const uint8_t want = cls == Cls::Drawer ? want_after_press(impl_of(button)) : 0;

    if (kind == Kind::InvOpen && is_host()) inv_opened();
    // The class chooser's start leads to the house storage's equip screen
    // (MewDirector::DoHouseStorageEquipFlow, scene StorageItems): shared like
    // the backpack, so either player picks the cats' items.
    if (kind == Kind::Start && is_host()) { inv_opened(); depart_started(); }
    if (kind == Kind::RunShop && !is_host()) {
        // The host shops for both: one wallet, one stock, one purchase.
        g.shop_refused_at = GetTickCount64();
        if (g.shop_refused++ % 10 == 0)
            log_line("TOWN", "a press in the shop was refused -- the host runs the"
                             " adventure's shops; its purchases happen here too");
        return false;
    }
    if (is_host()) {
        send_press(k, 0, true, want);
        log_line_lvl(LogLevel::Trace, "TOWN", "-> pressed '%s' %s#%u (%s)", k.name, k.tag,
                     (unsigned)k.ordinal,
                     kind == Kind::Way ? "the way out" : kind == Kind::RunShop ? "the shop" :
                     kind == Kind::Choose || kind == Kind::Start ? "the class chooser" :
                     "an office");
        if (kind == Kind::Start)
            log_line("TOWN", "classes chosen -- both players go on to the cats' items");
        return true;
    }
    // A client asks; the host presses; the press comes back here.
    send_press(k, 1, false, want);
    log_line("TOWN", "asked the host to press '%s'%s%s -- %s", k.name[0] ? k.name : "?",
             k.tag[0] ? " " : "", k.tag,
             kind == Kind::Way ? "both players go" : "the host runs this screen");
    return false;
}

void town_on_press(uint8_t from, const TownPressMsg& m) {
    if (!net_active()) return;
    // A request goes to the host; a press comes from it. Nothing else.
    if (m.request ? !is_host() : !is_client()) return;
    if (g.pq_n >= kQueue) {
        ++g.dropped;
        log_line("TOWN", "!! %u presses already waiting -- dropping '%s' from peer %u",
                 g.pq_n, m.name, (unsigned)from);
        return;
    }
    Pending& p = g.pq[g.pq_n++];
    p.m = m;
    p.at = GetTickCount64();
}

void town_on_button_post(void* button) {
    if (!g.pq_n || !button || g.cur_btn != button) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g.press_last < kPressGapMs) return;
    Key k; Cls cls;
    if (!key_of(button, k, cls)) return;
    k.ordinal = g.cur_ord;
    Pending& p = g.pq[0];
    if (!same(p.m, k)) return;

    g.press_last = now;
    const TownPressMsg m = p.m;
    pq_pop();
    // A drawer already where the press wants it is not pressed -- that would
    // toggle it the other way.
    const bool there = drawer_there(impl_of(button), m.want);
    if (m.request) {
        // The host presses for the client, and says so like any press of its
        // own -- even when its own drawer is already there, because the
        // client's is not.
        send_press(k, 0, true, m.want);
        log_line("TOWN", "pressing '%s' %s for the other player%s", k.name, k.tag,
                 there ? " (already so here)" : "");
        if (there) return;
    } else {
        if (there) return;
        if (m.has_rng)
            if (uint64_t* st = rng_global_stream()) memcpy(st, m.rng, sizeof(m.rng));
        log_line_lvl(LogLevel::Trace, "TOWN", "pressing '%s' %s#%u -- the host did",
                     k.name, k.tag, (unsigned)k.ordinal);
    }
    if (strcmp(k.name, "Map_Backpack") == 0 || cls == Cls::ChooserStart) inv_opened();
    if (cls == Cls::ChooserStart) depart_started();
    if (run_callback(button)) ++g.done;
}

// --- dialogs ----------------------------------------------------------------------

void town_dlg_update_pre(void* ctrl) {
    if (!ctrl || !net_active()) return;
    const uint8_t cls = dlg_class(ctrl);
    if (is_host()) {
        g.pre_wait[cls] = wait_is_click(ctrl);
        return;
    }
    if (!is_client()) return;
    if (g.dq_n && g.dq[0].m.op == kDlgWake && g.dq[0].m.cls == cls && wait_is_click(ctrl)) {
        clear_wait(ctrl);
        dq_pop();
        ++g.dlg_done;
    }
    // Hold this player's clicks back for the length of the dialog's tick.
    if (g.line_since && !g.dq_n && GetTickCount64() - g.line_since > kDlgValveMs) {
        if (!g.said_valve) {
            g.said_valve = true;
            log_line("TOWN", "!! a dialog line has been open here for %llu s and the host"
                             " has not moved it -- letting this player's own click close it",
                     (unsigned long long)(kDlgValveMs / 1000));
        }
        return;
    }
    uintptr_t scene = 0;
    if (g.cur_scene && mem_read((const void*)g.cur_scene, &scene, sizeof(scene)) && scene &&
        g.blk_depth < 4) {
        g.blk_scene[g.blk_depth] = scene;
        g.blk_saved[g.blk_depth] = rd8((void*)scene, kScene_NoInput);
        ++g.blk_depth;
        wr8((void*)scene, kScene_NoInput, 1);
        if (!g.said_block) {
            g.said_block = true;
            log_line("TOWN", "dialogs are advanced by the host -- this player's clicks on"
                             " them are held back");
        }
    }
}

void town_dlg_update_post(void* ctrl) {
    if (!ctrl || !net_active()) return;
    if (is_client()) {
        if (g.blk_depth) {
            --g.blk_depth;
            wr8((void*)g.blk_scene[g.blk_depth], kScene_NoInput, g.blk_saved[g.blk_depth]);
        }
        return;
    }
    if (!is_host()) return;
    const uint8_t cls = dlg_class(ctrl);
    if (g.pre_wait[cls] && !wait_is_click(ctrl)) send_dialog(kDlgWake, cls, ctrl);
    g.pre_wait[cls] = false;
}

void town_dlg_bubble_pre(void* ctrl, void* cmd) {
    if (!ctrl || !net_active()) return;
    void* text = text_of(ctrl);
    const bool dialog = cmd_is_dialog(cmd);
    if (is_host()) {
        g.pre_dialog = dialog && !rd8(ctrl, kDlg_Auto);
        g.pre_closed = rd8(ctrl, kDlg_Closed);
        g.pre_skip   = rd8(text, kText_Skip);
        return;
    }
    if (!is_client()) return;
    if (!dialog || rd8(ctrl, kDlg_Closed)) g.line_since = 0;
    else if (!g.line_since) g.line_since = GetTickCount64();
    if (!g.dq_n || !dialog) return;
    DlgPending& d = g.dq[0];
    if (d.m.cls != dlg_class(ctrl)) return;
    if (d.m.op == kDlgSkip) {
        if (rd8(text, kText_Reveal)) wr8(text, kText_Skip, 1);
        dq_pop();
        ++g.dlg_done;
        return;
    }
    if (d.m.op == kDlgClose && !rd8(ctrl, kDlg_Closed)) {
        // Still typing here: finish the text first, close it next tick.
        if (rd8(text, kText_Reveal)) { wr8(text, kText_Skip, 1); return; }
        wr8(ctrl, kDlg_Closed, 1);
        g.line_since = 0;
        dq_pop();
        ++g.dlg_done;
    }
}

void town_dlg_bubble_post(void* ctrl, void* cmd) {
    if (!ctrl || !is_host()) return;
    (void)cmd;
    const uint8_t cls = dlg_class(ctrl);
    if (g.pre_dialog && !g.pre_closed && rd8(ctrl, kDlg_Closed))
        send_dialog(kDlgClose, cls, ctrl);
    else if (g.pre_dialog && !g.pre_skip && rd8(text_of(ctrl), kText_Skip))
        send_dialog(kDlgSkip, cls, ctrl);
}

void town_on_dialog(uint8_t, const DialogMsg& m) {
    if (!is_client()) return;
    if (g.dq_n >= kQueue) {
        ++g.dlg_dropped;
        dq_pop();                       // the oldest is the least likely to matter
    }
    DlgPending& d = g.dq[g.dq_n++];
    d.m = m;
    d.at = GetTickCount64();
}

// --- once a frame ---------------------------------------------------------------

void town_pump() {
    ++g.frame;
    if (!net_active()) { g.pq_n = 0; g.dq_n = 0; g.office = false; return; }
    const bool office = leave_scene_live("NPCOffice");
    if (office != g.office) {
        g.office = office;
        log_line_lvl(LogLevel::Trace, "TOWN", office ? "in an NPC's office" : "out of the office");
        if (!office && is_client() && g.dq_n) {
            // What was said in there is over.
            g.dq_n = 0;
        }
    }
    const ULONGLONG now = GetTickCount64();
    // Out of the house and the chooser: the host's run is loading, or never
    // came (2 minutes), and either way there is nothing left to wait for here.
    if (g.awaiting_run && ((!leave_scene_live("House") && !leave_scene_live("ClassChooser")) ||
                           now - g.awaiting_run > 120000))
        g.awaiting_run = 0;
    // Departing: the equip screen is shared for as long as it is up, however
    // long the way to it took.
    if (g.depart_at) {
        const bool items = leave_scene_live("StorageItems");
        if (items) {
            if (!g.depart_saw_items)
                log_line_lvl(LogLevel::Trace, "TOWN", "the cats' items screen is up -- shared");
            g.depart_saw_items = true;
            g.inv_shared = true;
            g.inv_since = now;
        } else if (g.depart_saw_items || now - g.depart_at > 60000) {
            g.depart_at = 0;
        } else if (leave_scene_live("ClassChooser") || leave_scene_live("ActSelection")) {
            g.depart_at = now;          // still on the way there
        }
    }
    if (g.inv_shared && (leave_scene_live("ClassChooser") || leave_scene_live("ActSelection")))
        g.inv_since = now;
    if (g.inv_shared && now - g.inv_since > 3000 && !leave_scene_live("CatStatus") &&
        !leave_scene_live("StorageItems")) {
        g.inv_shared = false;
        log_line_lvl(LogLevel::Trace, "TOWN", "the shared backpack is closed");
    }
    if (g.pq_n && now - g.pq[0].at > kPressDropMs) {
        ++g.dropped;
        log_line("TOWN", "!! '%s' %s never appeared on this peer in %llu s -- dropped",
                 g.pq[0].m.name, g.pq[0].m.tag, (unsigned long long)(kPressDropMs / 1000));
        pq_pop();
    }
    if (g.dq_n && now - g.dq[0].at > kDlgDropMs) {
        ++g.dlg_dropped;
        log_line("TOWN", "!! the host moved a dialog on (step %d) and this peer never"
                         " reached that line -- dropped", g.dq[0].m.step);
        dq_pop();
    }
}

void town_shutdown() {
    if (g.sent || g.asked || g.done || g.dlg_sent || g.dlg_done)
        log_line_lvl(g.dropped || g.dlg_dropped ? LogLevel::Warn : LogLevel::Trace, "TOWN",
                     "done: %u press(es) sent, %u asked of the host, %u done here, %u"
                     " dropped; dialogs %u sent, %u applied, %u dropped",
                     g.sent, g.asked, g.done, g.dropped, g.dlg_sent, g.dlg_done,
                     g.dlg_dropped);
    g.pq_n = 0; g.dq_n = 0; g.blk_depth = 0;
}

} // namespace mgmp
