// engine.cpp - gesture recognition for the Magic Mouse 2.
//
// A "session" lasts from the first finger touching the surface until the last
// one lifts. During a session the engine is in one of these modes:
//
//   Undecided  watching; nothing emitted yet
//   Scroll     exactly one finger moves (others may rest): emit wheel deltas
//   Done       a swipe already fired; ignore the rest of the session
//   Blocked    a button was pressed or the mouse body moved: no gestures
//
// When the session ends in Undecided and it was short and still, it's a tap;
// two taps with the same finger count in quick succession are a double-tap.
//
// Units: touch coordinates are device units (~47 per mm on the MM2 surface).
// y is assumed to grow away from the user (toward the front of the mouse).
#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr double kScrollStart     = 60;     // units a finger must travel to start scrolling
constexpr double kRestingMax      = 35;     // other fingers must stay within this to count as resting
constexpr double kRestingSettle   = 0.15;   // s a finger must be down before it counts as resting
constexpr double kSwipeDistance   = 250;    // average horizontal travel to fire a swipe (~6 mm)
constexpr double kTapMaxTravel    = 50;
constexpr double kTapMaxDuration  = 0.30;   // s
constexpr double kDoubleTapWindow = 0.40;   // s between the two taps
// Mouse-body motion (counts) tolerated before a session is treated as "moving
// the mouse". Swiping nudges the mouse, so this is generous and swipes ignore it.
constexpr double kBodyStill       = 40;     // for scrolling and taps
constexpr double kMultiFingerGrace= 0.12;   // s: a 2nd finger this soon means "not a scroll"
constexpr double kLateFingerWindow= 0.35;   // s: a 2nd finger this soon can still turn a scroll into a swipe

constexpr double kSwitchStep      = 280;    // further travel per app while the switcher is open

constexpr double kBaseGain       = 0.25;   // wheel units per device unit at speed 1.0
constexpr double kAccelRef        = 1500;   // units/s where acceleration adds +1x
constexpr double kAccelMaxBoost   = 3.0;
constexpr double kSmoothing       = 0.5;    // EMA weight of the newest position sample
constexpr double kVelSmoothing    = 0.4;

constexpr double kMomentumStart   = 400;    // units/s needed at lift to coast
constexpr double kMomentumStop    = 60;     // units/s where coasting ends
constexpr double kMomentumFresh   = 0.06;   // s: finger must have been moving this recently

}  // namespace

GestureEngine::Finger *GestureEngine::Find(int id)
{
    for (auto &f : fingers_)
        if (f.id == id)
            return &f;
    return nullptr;
}

void GestureEngine::OnFrame(const TouchFrame &f)
{
    bool anyActive = std::any_of(f.touches.begin(), f.touches.end(),
                                 [](const TouchPoint &p) { return p.active; });

    if (mode_ == Mode::Idle) {
        if (!anyActive)
            return;
        BeginSession(f);
    }

    UpdateFingers(f);
    maxFingers_ = std::max(maxFingers_, (int)fingers_.size());
    if (f.buttons)
        buttonSeen_ = true;
    bodyTravel_ += std::abs(f.dx) + std::abs(f.dy);

    if (fingers_.empty()) {
        EndSession(f.t);
        return;
    }

    // A second finger landing just after the first: this was the start of a
    // swipe, not a scroll. Re-decide (the little scrolling already sent stays).
    if (mode_ == Mode::Scroll && (int)fingers_.size() > scrollStartFingers_ &&
        f.t - sessionStart_ <= kLateFingerWindow) {
        mode_ = Mode::Undecided;
        momentumActive_ = false;
    }

    switch (mode_) {
    case Mode::Undecided: Decide(f); break;
    case Mode::Scroll:    DoScroll(f); break;
    case Mode::Switching: DoSwitch(); break;
    default:              break;
    }
}

void GestureEngine::BeginSession(const TouchFrame &f)
{
    mode_ = Mode::Undecided;
    fingers_.clear();
    sessionStart_ = f.t;
    maxFingers_ = 0;
    buttonSeen_ = false;
    bodyTravel_ = 0;
    sessionTravel_ = 0;
    outcome_ = "undecided";
    scrolled_ = 0;
    scrollId_ = -1;
    axisLock_ = 0;
    momentumActive_ = false;    // touching the surface stops coasting, as on a Mac
    accV_ = accH_ = 0;
}

void GestureEngine::UpdateFingers(const TouchFrame &f)
{
    // Drop fingers that lifted.
    fingers_.erase(std::remove_if(fingers_.begin(), fingers_.end(), [&](const Finger &g) {
        return std::none_of(f.touches.begin(), f.touches.end(),
                            [&](const TouchPoint &p) { return p.active && p.id == g.id; });
    }), fingers_.end());

    for (const auto &p : f.touches) {
        if (!p.active)
            continue;
        Finger *g = Find(p.id);
        if (!g) {
            fingers_.push_back({ p.id, (double)p.x, (double)p.y, (double)p.x, (double)p.y, 0, f.t });
            continue;
        }
        g->x += kSmoothing * (p.x - g->x);
        g->y += kSmoothing * (p.y - g->y);
        g->maxTravel = std::max(g->maxTravel, std::hypot(g->x - g->startX, g->y - g->startY));
        sessionTravel_ = std::max(sessionTravel_, g->maxTravel);
    }
}

void GestureEngine::Decide(const TouchFrame &f)
{
    if (buttonSeen_) {
        mode_ = Mode::Blocked;
        outcome_ = "blocked: button pressed";
        return;
    }

    int n = (int)fingers_.size();
    bool oneFingerSwipe = cfg_.oneFingerSwipe != SwipeMapping::None;
    if ((n >= 2 || (n == 1 && maxFingers_ == 1)) && TrySwipe(n))   // swipes tolerate the mouse moving
        return;

    if (bodyTravel_ > kBodyStill) {
        if (maxFingers_ >= 2 || oneFingerSwipe)
            return;                         // keep waiting: may still become a swipe
        mode_ = Mode::Blocked;
        outcome_ = "blocked: mouse moved";
        return;
    }

    // Right after the first touch a second finger may still be arriving (a
    // two-finger swipe). Wait briefly unless the finger is clearly scrolling.
    if (n == 1 && maxFingers_ == 1 && f.t - sessionStart_ < kMultiFingerGrace &&
        fingers_[0].maxTravel < kScrollStart * 2)
        return;

    // Scroll: exactly one finger travelling, any others resting.

    Finger *mover = nullptr;
    for (auto &g : fingers_) {
        if (g.maxTravel >= kScrollStart) {
            if (mover)
                return;                     // two fingers moving: a swipe in progress
            mover = &g;
        } else if (g.maxTravel > kRestingMax || f.t - g.downT < kRestingSettle) {
            return;                         // not (yet) resting: could be a swipe starting
        }
    }
    if (!mover || maxFingers_ > n)
        return;

    double dx = mover->x - mover->startX, dy = mover->y - mover->startY;
    if (oneFingerSwipe && std::abs(dx) >= std::abs(dy))
        return;                             // sideways: a one-finger swipe, not a scroll
    axisLock_ = std::abs(dy) > 2 * std::abs(dx) ? 1 : std::abs(dx) > 2 * std::abs(dy) ? 2 : 0;
    if (!cfg_.horizontalScroll || oneFingerSwipe)
        axisLock_ = 1;

    mode_ = Mode::Scroll;
    outcome_ = "scroll";
    scrollId_ = mover->id;
    scrollStartFingers_ = n;
    lastX_ = mover->x;
    lastY_ = mover->y;
    lastT_ = lastMoveT_ = f.t;
    velX_ = velY_ = 0;
}

bool GestureEngine::TrySwipe(int n)
{
    SwipeMapping map = n == 1 ? cfg_.oneFingerSwipe : n == 2 ? cfg_.twoFingerSwipe
                     : n == 3 ? cfg_.threeFingerSwipe : SwipeMapping::None;
    if (map == SwipeMapping::None)
        return false;

    double sumX = 0, sumY = 0;
    int dir = 0;
    for (const auto &g : fingers_) {
        double dx = g.x - g.startX;
        if (std::abs(dx) < kSwipeDistance * 0.4)
            return false;                   // every finger must take part
        int d = dx > 0 ? 1 : -1;
        if (dir && d != dir)
            return false;
        dir = d;
        sumX += dx;
        sumY += g.y - g.startY;
    }
    double avgX = sumX / n, avgY = sumY / n;
    if (std::abs(avgX) < kSwipeDistance || std::abs(avgX) < 2 * std::abs(avgY))
        return false;

    if (map == SwipeMapping::AppSwitch) {
        // Opens the Alt+Tab switcher; it stays open while the fingers are down.
        out_.Perform(avgX > 0 ? Action::AppNext : Action::AppPrev);
        mode_ = Mode::Switching;
        switchAnchor_ = avgX;
        outcome_ = "app switch";
        return true;
    }

    // Swipe left = Back, right = Forward (user preference).
    // Desktops: swipe right = previous desktop, left = next.
    Action a = map == SwipeMapping::BackForward
             ? (avgX < 0 ? Action::Back : Action::Forward)
             : (avgX > 0 ? Action::DesktopLeft : Action::DesktopRight);
    out_.Perform(a);
    mode_ = Mode::Done;
    outcome_ = n == 1 ? "one-finger swipe" : n == 2 ? "two-finger swipe" : "three-finger swipe";
    return true;
}

double GestureEngine::AverageDx() const
{
    double sum = 0;
    for (const auto &g : fingers_)
        sum += g.x - g.startX;
    return fingers_.empty() ? 0 : sum / (double)fingers_.size();
}

// Switcher open: every further kSwitchStep of travel moves the selection one
// app (right = next, left = previous). Lifting a finger picks the app.
void GestureEngine::DoSwitch()
{
    if (fingers_.size() < 2) {
        out_.Perform(Action::AppEnd);
        mode_ = Mode::Done;
        return;
    }
    double x = AverageDx();
    while (x - switchAnchor_ >= kSwitchStep) {
        out_.Perform(Action::AppNext);
        switchAnchor_ += kSwitchStep;
    }
    while (switchAnchor_ - x >= kSwitchStep) {
        out_.Perform(Action::AppPrev);
        switchAnchor_ -= kSwitchStep;
    }
}

void GestureEngine::DoScroll(const TouchFrame &f)
{
    if (f.buttons) {                        // clicking ends scrolling, no coasting
        mode_ = Mode::Blocked;
        outcome_ = "scroll, then click";
        return;
    }
    Finger *g = Find(scrollId_);
    if (!g) {                               // scroll finger lifted; others still resting
        StartMomentum(f.t);
        mode_ = Mode::Done;
        return;
    }

    double dt = std::max(f.t - lastT_, 1e-4);
    double ddx = g->x - lastX_, ddy = g->y - lastY_;
    lastX_ = g->x;
    lastY_ = g->y;
    lastT_ = f.t;

    if (axisLock_ == 1) ddx = 0;
    if (axisLock_ == 2) ddy = 0;

    velX_ += kVelSmoothing * (ddx / dt - velX_);
    velY_ += kVelSmoothing * (ddy / dt - velY_);
    if (ddx != 0 || ddy != 0)
        lastMoveT_ = f.t;

    EmitScroll(ddy, ddx, std::hypot(velX_, velY_));
}

void GestureEngine::EmitScroll(double vUnits, double hUnits, double speed)
{
    double gain = kBaseGain * cfg_.scrollSpeed *
                  (1.0 + cfg_.acceleration * std::min(speed / kAccelRef, kAccelMaxBoost));
    // Classic: finger forward (+y) scrolls up (+wheel); finger right scrolls right.
    double sign = cfg_.naturalScrolling ? -1.0 : 1.0;

    accV_ += vUnits * gain * sign;
    if (cfg_.horizontalScroll)
        accH_ += hUnits * gain * sign;

    int v = (int)accV_, h = (int)accH_;     // truncate toward zero, keep the remainder
    accV_ -= v;
    accH_ -= h;
    if (v || h) {
        scrolled_ += std::abs(v) + std::abs(h);
        out_.Scroll(v, h);
    }
}

bool GestureEngine::StartMomentum(double t)
{
    if (!cfg_.momentum || t - lastMoveT_ > kMomentumFresh ||
        std::hypot(velX_, velY_) < kMomentumStart)
        return false;
    momentumActive_ = true;
    momentumT_ = t;
    return true;
}

void GestureEngine::EndSession(double t)
{
    bool coasting = false;
    if (mode_ == Mode::Switching) {
        out_.Perform(Action::AppEnd);
    } else if (mode_ == Mode::Scroll) {
        coasting = StartMomentum(t);
    } else if (mode_ == Mode::Undecided && !buttonSeen_ && bodyTravel_ <= kBodyStill &&
               t - sessionStart_ <= kTapMaxDuration) {
        HandleTap(t, maxFingers_);
    } else if (mode_ == Mode::Undecided && maxFingers_ >= 2) {
        outcome_ = "no swipe: fingers did not move far enough together";
    } else if (mode_ == Mode::Undecided && bodyTravel_ > kBodyStill) {
        outcome_ = "nothing: mouse moved";
    } else if (mode_ == Mode::Undecided && sessionTravel_ >= kScrollStart) {
        outcome_ = "no swipe: finger did not move far enough sideways";
    }

    char line[256];
    snprintf(line, sizeof(line),
             "touch: %d finger(s), %.0f ms, max travel %.0f, mouse moved %.0f -> %s%s (scrolled %.0f)",
             maxFingers_, (t - sessionStart_) * 1000, sessionTravel_, bodyTravel_, outcome_,
             coasting ? " + glide" : "", scrolled_);
    out_.Trace(line);

    mode_ = Mode::Idle;
    fingers_.clear();
}

void GestureEngine::HandleTap(double t, int fingers)
{
    if (sessionTravel_ > kTapMaxTravel) {   // a tap must stay put
        lastTapT_ = -10;
        outcome_ = "not a tap: finger moved";
        return;
    }
    outcome_ = "tap";
    if (fingers == lastTapFingers_ && t - lastTapT_ <= kDoubleTapWindow) {
        TapMapping map = fingers == 1 ? cfg_.oneFingerDoubleTap
                       : fingers == 2 ? cfg_.twoFingerDoubleTap : TapMapping::None;
        outcome_ = "double-tap";
        if (map == TapMapping::TaskView) {
            out_.Perform(Action::TaskView);
        } else if (map == TapMapping::SmartZoom) {
            out_.Perform(zoomed_ ? Action::ZoomReset : Action::ZoomIn);
            zoomed_ = !zoomed_;
        }
        lastTapT_ = -10;                    // a third tap starts over
        lastTapFingers_ = 0;
        return;
    }
    lastTapT_ = t;
    lastTapFingers_ = fingers;
}

void GestureEngine::OnTick(double t)
{
    if (!momentumActive_)
        return;
    double dt = t - momentumT_;
    if (dt <= 0)
        return;
    momentumT_ = t;

    double tau = std::max(cfg_.momentumDecay, 0.05);
    double decay = std::exp(-dt / tau);
    // Exact distance travelled while velocity decays over dt.
    double travel = tau * (1.0 - decay);
    EmitScroll(velY_ * travel, velX_ * travel, std::hypot(velX_, velY_));
    velX_ *= decay;
    velY_ *= decay;
    if (std::hypot(velX_, velY_) < kMomentumStop)
        momentumActive_ = false;
}
