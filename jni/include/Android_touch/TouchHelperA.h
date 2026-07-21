#pragma once

#include <sys/time.h>

enum FingerStatus {
    FINGER_NO,
    FINGER_X_UPDATE,
    FINGER_Y_UPDATE,
    FINGER_XY_UPDATE,
    FINGER_UP,
};

struct TouchFinger {
    int x = -1;
    int y = -1;
    int tracking_id = -1;
    int status = FINGER_NO;
    timeval time{};
};

bool Touch_Down(int slot, int x, int y);
bool Touch_Move(int slot, int x, int y);
bool Touch_Up(int slot);

TouchFinger* getTouchFinger(int slot);
void setTouchCallback(void (*callback)(TouchFinger*));

bool TouchScreenHandle(int mode);
bool IsTouchWriteReady();
void StopTouchScreen();
void PumpTouchInput();
void ConfigureTouchDisplay(int width, int height, int orientation);
void setOrientation(int orientation);
