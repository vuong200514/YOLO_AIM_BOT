#pragma once
#include <Windows.h>
#include <cmath>
#include <algorithm>

class MouseController {
public:
    MouseController();
    ~MouseController();

    void Move(int x, int y);
    void Move(float dx, float dy, float smooth = 2.0f);
    void Click(bool down);

private:
    void SendMove(int x, int y);

    float residualX;
    float residualY;
};