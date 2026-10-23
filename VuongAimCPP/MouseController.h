#pragma once
#include <Windows.h>
#include <cmath>
#include <algorithm>

class MouseController {
public:
    MouseController();
    ~MouseController();
    void Move(int x, int y);
    void Move(float dx, float dy, float smooth = 4.0f);
    void Click(bool down);
    void Reset();

private:
    void SendMove(int x, int y);
    float residualX;
    float residualY;
    float previousErrorX;
    float previousErrorY;
};
