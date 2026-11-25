// inject.h - turns engine output into Windows input (SendInput).
#pragma once
#include "engine.h"

class WindowsOutput : public GestureOutput {
public:
    WindowsOutput(bool verbose, int zoomSteps) : verbose_(verbose), zoomSteps_(zoomSteps) {}
    ~WindowsOutput() override { ReleaseHeldKeys(); }
    void Scroll(int vertical, int horizontal) override;
    void Perform(Action action) override;
    void Trace(const char *line) override;
    // Lets go of Alt if the app switcher is still open (e.g. the mouse disconnected).
    void ReleaseHeldKeys();

private:
    bool verbose_;
    int zoomSteps_;
    bool altHeld_ = false;
};

const char *ActionName(Action a);
