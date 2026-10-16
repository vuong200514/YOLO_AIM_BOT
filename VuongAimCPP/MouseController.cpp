#include "MouseController.h"

MouseController::MouseController() : residualX(0.0f), residualY(0.0f) {}

MouseController::~MouseController() {}

void MouseController::Click(bool down) {
    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseController::Move(int x, int y) {
    SendMove(x, y);
}

void MouseController::Move(float dx, float dy, float smooth) {
    float moveX = (dx / smooth) + residualX;
    float moveY = (dy / smooth) + residualY;

    int actualMoveX = (int)moveX;
    int actualMoveY = (int)moveY;

    residualX = moveX - actualMoveX;
    residualY = moveY - actualMoveY;

    if (actualMoveX != 0 || actualMoveY != 0) {
        SendMove(actualMoveX, actualMoveY);
    }
}

void MouseController::SendMove(int x, int y) {
    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx = x;
    input.mi.dy = y;
    SendInput(1, &input, sizeof(INPUT));
}