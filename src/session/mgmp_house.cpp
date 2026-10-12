// mgmp_house.cpp -- see mgmp_house.h.

#include "mgmp_house.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"

#include <windows.h>
#include <cmath>
#include <cstring>

namespace mgmp {
namespace {

// Component+0x20 is the scene the component lives in: House::update passes it
// straight to the camera lookup (mov rcx,[rsi+20h] / call sub_140978330).
constexpr uintptr_t kComp_Scene = 0x20;

using fn_scene_cam   = void*   (__fastcall*)(void* scene);
using fn_mouse_world = double* (__fastcall*)(void* cam, double* out);
using fn_viewproj    = float*  (__fastcall*)(void* cam, float* out);

struct State {
    bool           resolved    = false;
    fn_scene_cam   scene_cam   = nullptr;
    fn_mouse_world mouse_world = nullptr;
    fn_viewproj    viewproj    = nullptr;
    const float*   ndc         = nullptr;   // D_NdcRange, four floats

    ULONGLONG tick       = 0;
    double    mx = 0, my = 0;
    bool      have_mouse = false;
    float     vp[16]     = {};
    bool      have_vp    = false;

    bool said_live = false;
    bool said_proj = false;
};

State g;

} // namespace

void house_set_base(uintptr_t) {
    g.resolved    = false;
    g.scene_cam   = (fn_scene_cam)  addr_of_call(C_SceneCamera);
    g.mouse_world = (fn_mouse_world)addr_of_call(C_CameraMouseWorld);
    g.viewproj    = (fn_viewproj)   addr_of_call(C_CameraViewProj);
    g.ndc         = (const float*)  addr_of_data(D_NdcRange);
    g.resolved = g.scene_cam && g.mouse_world && g.viewproj && g.ndc;
    if (!g.resolved)
        log_line("HOUSE", "!! the house camera functions did not resolve -- the other"
                          " player's pointer will not be shown in the house");
}

void house_on_update(void* house) {
    if (!g.resolved || !house) return;
    g.tick = GetTickCount64();

    void* scene = nullptr;
    if (!mem_read((const uint8_t*)house + kComp_Scene, &scene, sizeof(scene)) || !scene)
        return;
    void* cam = g.scene_cam(scene);
    if (!cam) return;

    // Both outputs are the game's own stack shapes; aligned the way its own
    // callers' locals are, in case either is ever read with an aligned load.
    alignas(16) double w[2] = {};
    g.mouse_world(cam, w);
    if (std::isfinite(w[0]) && std::isfinite(w[1])) {
        g.mx = w[0]; g.my = w[1]; g.have_mouse = true;
    }
    alignas(16) float m[16] = {};
    g.viewproj(cam, m);
    memcpy(g.vp, m, sizeof(g.vp));
    g.have_vp = true;

    if (!g.said_live) {
        g.said_live = true;
        log_line_lvl(LogLevel::Trace, "HOUSE", "house camera live: mouse at world %.1f,%.1f",
                     g.mx, g.my);
    }
}

bool house_active() {
    return g.tick && (GetTickCount64() - g.tick) < 250;
}

bool house_mouse_world(double& x, double& y) {
    if (!house_active() || !g.have_mouse) return false;
    x = g.mx; y = g.my;
    return true;
}

bool house_world_to_frac(double x, double y, float& nx, float& ny) {
    if (!house_active() || !g.have_vp || !g.ndc) return false;
    float r[4];
    if (!mem_read(g.ndc, r, sizeof(r))) return false;

    // Forward through VP, column-major, at z = 0 -- the exact inverse of what
    // sub_1409796D0 applies to the mouse.
    double ex = g.vp[0] * x + g.vp[4] * y + g.vp[12];
    double ey = g.vp[1] * x + g.vp[5] * y + g.vp[13];
    const double ew = g.vp[3] * x + g.vp[7] * y + g.vp[15];
    if (std::fabs(ew) > 1e-9) { ex /= ew; ey /= ew; }

    // sub_140986EE0: ndc_x = (1-tx)*r[0] + tx*r[1];  ndc_y = (1-ty)*r[3] + ty*r[2].
    const double dx = (double)r[1] - r[0];
    const double dy = (double)r[2] - r[3];
    if (std::fabs(dx) < 1e-9 || std::fabs(dy) < 1e-9) {
        if (!g.said_proj) {
            g.said_proj = true;
            log_line("HOUSE", "!! the NDC range reads %.3f %.3f %.3f %.3f -- cannot place"
                              " the other player's pointer", r[0], r[1], r[2], r[3]);
        }
        return false;
    }
    const double fx = (ex - r[0]) / dx;
    const double fy = (ey - r[3]) / dy;
    if (!std::isfinite(fx) || !std::isfinite(fy)) return false;
    nx = (float)fx;
    ny = (float)fy;
    return true;
}

} // namespace mgmp
