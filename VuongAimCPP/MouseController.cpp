#include "MouseController.h"

MouseController::MouseController()
    : residualX(0.0f), residualY(0.0f), previousErrorX(0.0f), previousErrorY(0.0f) {
}

MouseController::~MouseController() {}

void MouseController::Reset() {
    residualX = 0.0f;
    residualY = 0.0f;
    previousErrorX = 0.0f;
    previousErrorY = 0.0f;
}

void MouseController::Click(bool down) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseController::Move(int x, int y) {
    SendMove(x, y);
}

void MouseController::Move(float dx, float dy, float smooth) {
    if (!std::isfinite(dx) || !std::isfinite(dy)) {
        Reset();
        return;
    }

    const float distance = std::hypot(dx, dy);
    if (distance <= 1.25f) {
        Reset();
        return;
    }

    smooth = std::clamp(smooth, 0.1f, 64.0f);

    if (dx * previousErrorX < 0.0f) residualX = 0.0f;
    if (dy * previousErrorY < 0.0f) residualY = 0.0f;

    const float nearScale = std::clamp(distance / 8.0f, 0.55f, 1.0f);
    const float maxStep = std::clamp(distance * 1.80f, 2.0f, 128.0f) * nearScale;
    const float stepX = std::clamp((dx / smooth) * nearScale, -maxStep, maxStep);
    const float stepY = std::clamp((dy / smooth) * nearScale, -maxStep, maxStep);

    const float valueX = stepX + residualX;
    const float valueY = stepY + residualY;
    const int moveX = static_cast<int>(std::trunc(valueX));
    const int moveY = static_cast<int>(std::trunc(valueY));

    residualX = valueX - static_cast<float>(moveX);
    residualY = valueY - static_cast<float>(moveY);
    previousErrorX = dx;
    previousErrorY = dy;

    if (moveX != 0 || moveY != 0) SendMove(moveX, moveY);
}

void MouseController::SendMove(int x, int y) {
    if (x == 0 && y == 0) return;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx = x;
    input.mi.dy = y;
    SendInput(1, &input, sizeof(INPUT));
}
