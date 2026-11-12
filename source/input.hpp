#pragma once

// Estado del mando para un frame. Los botones son "pulsado ahora" (flanco);
// las direcciones incluyen repetición al mantenerlas.
struct Input {
    bool a = false, b = false, x = false, y = false;
    bool l = false, r = false, zl = false, zr = false;
    bool plus = false, minus = false;
    bool up = false, down = false, left = false, right = false;
    float stickY = 0.f;  // stick derecho vertical: -1 (arriba) .. 1 (abajo)
};
