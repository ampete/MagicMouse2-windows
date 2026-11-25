// gesture_test.cpp - drives GestureEngine with synthetic touch frames.
// Build: cl /EHsc /std:c++20 /W4 /I tools\gestures tests\gesture_test.cpp tools\gestures\engine.cpp
#include "engine.h"

#include <cmath>
#include <cstdio>
#include <vector>

struct Recorder : GestureOutput {
    long v = 0, h = 0;
    int scrollCalls = 0;
    std::vector<Action> actions;
    void Scroll(int vertical, int horizontal) override { v += vertical; h += horizontal; scrollCalls++; }
    void Perform(Action a) override { actions.push_back(a); }
};

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

constexpr double kFrame = 0.011;   // the mouse reports every ~11 ms

struct Sim {
    GestureEngine &e;
    double t = 1.0;
    void frame(std::vector<TouchPoint> pts, int buttons = 0, int dx = 0, int dy = 0) {
        e.OnFrame({ t, buttons, dx, dy, pts });
        t += kFrame;
    }
    void lift() { frame({}); }
    void idle(double seconds) {          // no touches; momentum ticks every 8 ms
        for (double end = t + seconds; t < end; t += 0.008)
            e.OnTick(t);
    }
    // One or more fingers moving together from (x,y) by (ddx,ddy) over `frames` frames.
    void drag(std::vector<std::pair<int,int>> starts, int ddx, int ddy, int frames, int buttons = 0, int body = 0) {
        for (int i = 0; i <= frames; i++) {
            std::vector<TouchPoint> pts;
            int id = 1;
            for (auto [x, y] : starts)
                pts.push_back({ id++, x + ddx * i / frames, y + ddy * i / frames, true });
            frame(pts, buttons, body, 0);
        }
    }
    void tap(int fingers) {
        std::vector<TouchPoint> pts;
        for (int i = 0; i < fingers; i++)
            pts.push_back({ i + 1, -300 + 250 * i, 0, true });
        for (int i = 0; i < 6; i++) frame(pts);     // ~66 ms touch
        lift();
    }
};

static void Test(const char *name, void (*fn)())
{
    int before = g_failures;
    fn();
    printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name);
}

int main()
{
    Test("classic: finger forward scrolls up, no horizontal", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        s.drag({ { 0, -400 } }, 0, 600, 30);
        s.lift();
        CHECK(r.v > 100);
        CHECK(r.h == 0);
    });

    Test("classic: finger right scrolls right (one-finger swipe off)", [] {
        GestureConfig c; c.oneFingerSwipe = SwipeMapping::None;
        Recorder r; GestureEngine e(c, r); Sim s{ e };
        s.drag({ { -500, 0 } }, 600, 0, 30);
        s.lift();
        CHECK(r.h > 100);
        CHECK(r.v == 0);
    });

    Test("natural scrolling inverts both axes", [] {
        GestureConfig c; c.naturalScrolling = true;
        Recorder r; GestureEngine e(c, r); Sim s{ e };
        s.drag({ { 0, -400 } }, 0, 600, 30);
        s.lift();
        CHECK(r.v < -100);
    });

    Test("momentum coasts after a flick, then stops", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        s.drag({ { 0, -600 } }, 0, 900, 8);          // fast: ~10000 units/s
        long atLift = r.v;
        s.lift();
        CHECK(e.WantsTicks());
        s.idle(3.0);
        CHECK(r.v > atLift + 50);
        CHECK(!e.WantsTicks());
    });

    Test("slow drag does not coast", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        s.drag({ { 0, -400 } }, 0, 200, 60);          // ~300 units/s
        s.lift();
        CHECK(!e.WantsTicks());
    });

    Test("touching the surface stops momentum", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        s.drag({ { 0, -600 } }, 0, 900, 8);
        s.lift();
        s.idle(0.05);
        s.frame({ { 5, 0, 0, true } });
        CHECK(!e.WantsTicks());
    });

    Test("no momentum when disabled", [] {
        GestureConfig c; c.momentum = false;
        Recorder r; GestureEngine e(c, r); Sim s{ e };
        s.drag({ { 0, -600 } }, 0, 900, 8);
        s.lift();
        CHECK(!e.WantsTicks());
    });

    Test("tiny jitter does not scroll", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        s.drag({ { 0, 0 } }, 20, 25, 30);
        s.lift();
        CHECK(r.scrollCalls == 0);
    });

    Test("clicking blocks scrolling", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        s.drag({ { 0, -400 } }, 0, 600, 30, /*buttons*/ 1);
        s.lift();
        CHECK(r.scrollCalls == 0);
    });

    Test("moving the mouse body blocks scrolling", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        s.drag({ { 0, -400 } }, 0, 600, 30, 0, /*body dx per frame*/ 10);
        s.lift();
        CHECK(r.scrollCalls == 0);
    });

    Test("scroll with one finger while another rests", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        for (int i = 0; i <= 30; i++)
            s.frame({ { 1, -600, 0, true }, { 2, 300, -400 + 20 * i, true } });
        s.lift();
        CHECK(r.v > 100);
    });

    Test("one-finger swipe left = Back, right = Forward, no scrolling", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        s.drag({ { 500, 0 } }, -600, 0, 15);
        s.lift();
        s.drag({ { -500, 0 } }, 600, 0, 15);
        s.lift();
        CHECK(r.actions.size() == 2 && r.actions[0] == Action::Back && r.actions[1] == Action::Forward);
        CHECK(r.scrollCalls == 0);
    });

    Test("one-finger swipe works while the mouse body moves a bit", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        s.drag({ { 500, 0 } }, -600, 0, 15, 0, /*body per frame*/ 8);
        s.lift();
        CHECK(r.actions.size() == 1 && r.actions[0] == Action::Back);
    });

    Test("slightly diagonal vertical scroll still scrolls, no swipe", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        s.drag({ { 0, -400 } }, 150, 600, 30);
        s.lift();
        CHECK(r.v > 100);
        CHECK(r.h == 0);
        CHECK(r.actions.empty());
    });

    Test("two-finger swipe (when enabled) left = Back, right = Forward", [] {
        GestureConfig c; c.twoFingerSwipe = SwipeMapping::BackForward;
        Recorder r; GestureEngine e(c, r); Sim s{ e };
        s.drag({ { 500, 0 }, { 500, 300 } }, -600, 0, 15);
        s.lift();
        s.drag({ { -700, 0 }, { -700, 300 } }, 600, 0, 15);
        s.lift();
        CHECK(r.actions.size() == 2 && r.actions[0] == Action::Back && r.actions[1] == Action::Forward);
        CHECK(r.scrollCalls == 0);
    });

    Test("two-finger swipe still works while the mouse body moves a bit", [] {
        GestureConfig c; c.twoFingerSwipe = SwipeMapping::BackForward;
        Recorder r; GestureEngine e(c, r); Sim s{ e };
        s.drag({ { -700, 0 }, { -700, 300 } }, 600, 0, 15, 0, /*body per frame*/ 8);
        s.lift();
        CHECK(r.actions.size() == 1 && r.actions[0] == Action::Forward);
    });

    Test("two-finger swipe where the second finger lands 150 ms late", [] {
        GestureConfig c; c.oneFingerSwipe = SwipeMapping::None; c.twoFingerSwipe = SwipeMapping::BackForward;
        Recorder r; GestureEngine e(c, r); Sim s{ e };
        // First finger alone, already moving right (would start a scroll)...
        for (int i = 0; i < 14; i++)
            s.frame({ { 1, -700 + 12 * i, 0, true } });
        // ...then the second finger joins and both keep moving.
        for (int i = 0; i <= 15; i++)
            s.frame({ { 1, -532 + 40 * i, 0, true }, { 2, -700 + 40 * i, 300, true } });
        s.lift();
        CHECK(r.actions.size() == 1 && r.actions[0] == Action::Forward);
    });

    Test("two-finger swipe right = app switcher, Alt released on lift", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        s.drag({ { -700, 0 }, { -700, 300 } }, 400, 0, 15);
        s.lift();
        CHECK(r.actions.size() == 2 && r.actions[0] == Action::AppNext && r.actions[1] == Action::AppEnd);
        CHECK(r.scrollCalls == 0);
    });

    Test("long two-finger swipe steps through apps, both directions", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        s.drag({ { -900, 0 }, { -900, 300 } }, 1100, 0, 40);     // open + 3 more steps right
        int next = 0;
        for (Action a : r.actions) next += a == Action::AppNext;
        CHECK(next >= 3);
        size_t before = r.actions.size();
        s.drag({ { 200, 0 }, { 200, 300 } }, -600, 0, 20);       // fingers stay down: back left
        bool prev = false;
        for (size_t i = before; i < r.actions.size(); i++) prev |= r.actions[i] == Action::AppPrev;
        CHECK(prev);
        s.lift();
        CHECK(!r.actions.empty() && r.actions.back() == Action::AppEnd);
        int ends = 0;
        for (Action a : r.actions) ends += a == Action::AppEnd;
        CHECK(ends == 1);
    });

    Test("two-finger swipe left opens the switcher backwards", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        s.drag({ { 500, 0 }, { 500, 300 } }, -400, 0, 15);
        s.lift();
        CHECK(r.actions.size() == 2 && r.actions[0] == Action::AppPrev && r.actions[1] == Action::AppEnd);
    });

    Test("three-finger swipe left = next desktop", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        s.drag({ { 500, -300 }, { 500, 0 }, { 500, 300 } }, -600, 0, 15);
        s.lift();
        CHECK(r.actions.size() == 1 && r.actions[0] == Action::DesktopRight);
    });

    Test("two fingers moving vertically do nothing", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        s.drag({ { -300, -400 }, { 300, -400 } }, 0, 600, 30);
        s.lift();
        CHECK(r.actions.empty());
        CHECK(r.scrollCalls == 0);
    });

    Test("two-finger double-tap = Task View; single tap = nothing", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        s.tap(2);
        CHECK(r.actions.empty());
        s.idle(0.1);
        s.tap(2);
        CHECK(r.actions.size() == 1 && r.actions[0] == Action::TaskView);
    });

    Test("taps too far apart are not a double-tap", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        s.tap(2);
        s.idle(0.6);
        s.tap(2);
        CHECK(r.actions.empty());
    });

    Test("one-finger double-tap toggles zoom in / reset", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        s.tap(1); s.idle(0.1); s.tap(1);
        s.idle(1.0);
        s.tap(1); s.idle(0.1); s.tap(1);
        CHECK(r.actions.size() == 2 && r.actions[0] == Action::ZoomIn && r.actions[1] == Action::ZoomReset);
    });

    Test("tap while clicking is not a tap", [] {
        Recorder r; GestureEngine e({}, r); Sim s{ e };
        for (int k = 0; k < 2; k++) {
            for (int i = 0; i < 6; i++) s.frame({ { 1, 0, 0, true } }, 1);
            s.lift();
            s.idle(0.1);
        }
        CHECK(r.actions.empty());
    });

    printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
