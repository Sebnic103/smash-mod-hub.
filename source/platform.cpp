#include "platform.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "sys.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace platform {

#ifdef __SWITCH__

struct Pad::Impl {
    PadState pad;
    uint64_t holdSince[4] = {0, 0, 0, 0};
    uint64_t lastRepeat[4] = {0, 0, 0, 0};
};

Pad::Pad() : impl_(new Impl) {}
Pad::~Pad() { delete impl_; }

void Pad::init() {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&impl_->pad);
}

Input Pad::poll() {
    PadState& p = impl_->pad;
    padUpdate(&p);
    const u64 down = padGetButtonsDown(&p);
    const u64 held = padGetButtons(&p);
    Input in;
    in.a = down & HidNpadButton_A;
    in.b = down & HidNpadButton_B;
    in.x = down & HidNpadButton_X;
    in.y = down & HidNpadButton_Y;
    in.l = down & HidNpadButton_L;
    in.r = down & HidNpadButton_R;
    in.zl = down & HidNpadButton_ZL;
    in.zr = down & HidNpadButton_ZR;
    in.plus = down & HidNpadButton_Plus;
    in.minus = down & HidNpadButton_Minus;

    // Cruceta + stick izquierdo, con repetición (400 ms de espera, luego cada 90 ms).
    const u64 masks[4] = {HidNpadButton_Up | HidNpadButton_StickLUp, HidNpadButton_Down | HidNpadButton_StickLDown, HidNpadButton_Left | HidNpadButton_StickLLeft, HidNpadButton_Right | HidNpadButton_StickLRight};
    bool* out[4] = {&in.up, &in.down, &in.left, &in.right};
    const uint64_t now = sys::nowMs();
    for (int i = 0; i < 4; i++) {
        if (held & masks[i]) {
            if (down & masks[i]) {
                impl_->holdSince[i] = now;
                impl_->lastRepeat[i] = now;
                *out[i] = true;
            } else if (impl_->holdSince[i] && now - impl_->holdSince[i] > 400 && now - impl_->lastRepeat[i] > 90) {
                impl_->lastRepeat[i] = now;
                *out[i] = true;
            }
        } else {
            impl_->holdSince[i] = 0;
        }
    }

    HidAnalogStickState rs = padGetStickPos(&p, 1);
    float y = -(float)rs.y / 32767.f;  // libnx: arriba es positivo
    in.stickY = std::fabs(y) < 0.2f ? 0.f : y;
    return in;
}

std::string firmwareVersion() {
    if (R_FAILED(setsysInitialize())) return "";
    SetSysFirmwareVersion fw;
    std::string out;
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
        char buf[32];
        snprintf(buf, sizeof buf, "%u.%u.%u", (unsigned)fw.major, (unsigned)fw.minor, (unsigned)fw.micro);
        out = buf;
    }
    setsysExit();
    return out;
}

bool textInput(const std::string& header, const std::string& initial, std::string& out, int maxLen, bool numeric) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header.c_str());
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, (u32)maxLen);
    if (numeric) swkbdConfigSetType(&kbd, SwkbdType_NumPad);
    char buf[256];
    memset(buf, 0, sizeof buf);
    Result rc = swkbdShow(&kbd, buf, sizeof buf);
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return false;
    out = buf;
    return true;
}

#else  // Host: sin mando ni teclado de sistema (solo para poder compilar/verificar).

struct Pad::Impl {};
Pad::Pad() : impl_(new Impl) {}
Pad::~Pad() { delete impl_; }
void Pad::init() {}
Input Pad::poll() { return Input(); }
std::string firmwareVersion() { return ""; }
bool textInput(const std::string&, const std::string&, std::string&, int, bool) { return false; }

#endif

}  // namespace platform
