// engine.h - Magic Mouse 2 gesture recognizer (pure logic, no Windows calls).
//
// Feed it decoded touch frames (OnFrame) and periodic ticks (OnTick). It calls
// back into GestureOutput for scrolling and discrete actions. Time is passed in
// explicitly (seconds), so the engine can be unit-tested with synthetic input.
#pragma once

#include <cstdint>
#include <vector>

enum class Action {
    None,
    Back,
    Forward,
    DesktopLeft,     // previous virtual desktop
    DesktopRight,    // next virtual desktop
    TaskView,
    ZoomIn,          // smart zoom, first double-tap
    ZoomReset,       // smart zoom, second double-tap
    AppNext,         // app switcher: hold Alt, press Tab (opens the switcher on first use)
    AppPrev,         // app switcher: Alt+Shift+Tab
    AppEnd,          // release Alt: switch to the selected app
};

enum class SwipeMapping { None, BackForward, Desktops, AppSwitch };
enum class TapMapping { None, TaskView, SmartZoom };

struct GestureConfig {
    // Scrolling
    bool   naturalScrolling = false;  // false = classic: finger forward scrolls up
    bool   horizontalScroll = true;
    double scrollSpeed      = 1.0;    // overall multiplier
    double acceleration     = 1.0;    // 0 = linear
    bool   momentum         = true;
    double momentumDecay    = 0.30;   // seconds for velocity to fall to ~37%

    // Gestures
    // One-finger swipe left = Back, right = Forward. While it's on, one finger
    // only scrolls vertically (a sideways slide is the swipe).
    SwipeMapping oneFingerSwipe    = SwipeMapping::BackForward;
    SwipeMapping twoFingerSwipe    = SwipeMapping::AppSwitch;
    SwipeMapping threeFingerSwipe  = SwipeMapping::Desktops;
    TapMapping   twoFingerDoubleTap = TapMapping::TaskView;
    TapMapping   oneFingerDoubleTap = TapMapping::SmartZoom;
    int          zoomSteps          = 5;   // Ctrl+wheel notches for smart zoom (browsers: 5 = ~200%)
};

struct TouchPoint {
    int id;
    int x, y;        // device units; y grows away from the user (see engine.cpp)
    bool active;     // touch state start or drag
};

struct TouchFrame {
    double t;        // seconds
    int buttons;     // bit0 left, bit1 right
    int dx, dy;      // mouse body motion this frame
    std::vector<TouchPoint> touches;
};

class GestureOutput {
public:
    virtual ~GestureOutput() = default;
    // Wheel deltas in WHEEL_DELTA units (120 = one notch). Positive vertical =
    // scroll up, positive horizontal = scroll right (Windows conventions).
    virtual void Scroll(int vertical, int horizontal) = 0;
    virtual void Perform(Action action) = 0;
    // One line per touch session explaining what was recognized (for --verbose).
    virtual void Trace(const char *line) { (void)line; }
};

class GestureEngine {
public:
    GestureEngine(const GestureConfig &config, GestureOutput &out) : cfg_(config), out_(out) {}

    void OnFrame(const TouchFrame &frame);
    void OnTick(double t);          // call every few ms; drives momentum
    bool WantsTicks() const { return momentumActive_; }

private:
    enum class Mode { Idle, Undecided, Scroll, Switching, Done, Blocked };

    struct Finger {
        int id;
        double startX, startY;
        double x, y;                 // smoothed position
        double maxTravel;
        double downT;                // when it touched
    };

    void BeginSession(const TouchFrame &f);
    void EndSession(double t);
    void UpdateFingers(const TouchFrame &f);
    void Decide(const TouchFrame &f);
    void DoScroll(const TouchFrame &f);
    bool TrySwipe(int fingerCount);
    void DoSwitch();
    double AverageDx() const;
    void EmitScroll(double vUnits, double hUnits, double speedUnitsPerSec);
    void HandleTap(double t, int fingers);
    bool StartMomentum(double t);
    Finger *Find(int id);

    GestureConfig cfg_;
    GestureOutput &out_;

    Mode mode_ = Mode::Idle;
    std::vector<Finger> fingers_;
    double sessionStart_ = 0;
    int maxFingers_ = 0;
    bool buttonSeen_ = false;
    double bodyTravel_ = 0;
    double sessionTravel_ = 0;       // largest travel of any finger this session
    const char *outcome_ = "";       // for Trace()
    double scrolled_ = 0;            // wheel units emitted this session

    // Scroll state
    int scrollId_ = -1;
    int scrollStartFingers_ = 0;     // fingers on the surface when scrolling began
    double lastX_ = 0, lastY_ = 0, lastT_ = 0, lastMoveT_ = 0;
    double velX_ = 0, velY_ = 0;     // device units / second
    int axisLock_ = 0;               // 0 free, 1 vertical only, 2 horizontal only
    double accV_ = 0, accH_ = 0;     // fractional wheel units not yet sent

    // App switching (Alt held while the fingers stay down)
    double switchAnchor_ = 0;        // average finger x travel at the last step

    // Momentum
    bool momentumActive_ = false;
    double momentumT_ = 0;

    // Double-tap detection
    double lastTapT_ = -10;
    int lastTapFingers_ = 0;
    bool zoomed_ = false;
};
