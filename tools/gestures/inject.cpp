// inject.cpp - SendInput wrappers.
//
// Scrolling uses high-resolution wheel deltas (smaller than WHEEL_DELTA = 120),
// which modern apps (browsers, Explorer, Office, VS Code) scroll smoothly.
// Note: Windows does not let a normal process inject input into elevated
// (admin) windows, so gestures do nothing there unless this runs elevated.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "inject.h"

#include <cstdio>
#include <initializer_list>
#include <vector>

const char *ActionName(Action a)
{
    switch (a) {
    case Action::Back:         return "Back";
    case Action::Forward:      return "Forward";
    case Action::DesktopLeft:  return "Previous desktop";
    case Action::DesktopRight: return "Next desktop";
    case Action::TaskView:     return "Task View";
    case Action::ZoomIn:       return "Zoom in";
    case Action::ZoomReset:    return "Zoom reset";
    case Action::AppNext:      return "App switcher: next";
    case Action::AppPrev:      return "App switcher: previous";
    case Action::AppEnd:       return "App switcher: switch";
    default:                   return "None";
    }
}

static bool IsExtendedKey(WORD vk)
{
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_LWIN: case VK_RWIN: case VK_RCONTROL: case VK_RMENU:
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        return true;
    default:
        return false;
    }
}

static INPUT Key(WORD vk, bool up)
{
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (IsExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    return in;
}

// Presses modifiers, taps `key`, releases modifiers in reverse order.
static void Chord(std::initializer_list<WORD> modifiers, WORD key)
{
    std::vector<INPUT> seq;
    for (WORD m : modifiers) seq.push_back(Key(m, false));
    seq.push_back(Key(key, false));
    seq.push_back(Key(key, true));
    for (auto it = std::rbegin(modifiers); it != std::rend(modifiers); ++it) seq.push_back(Key(*it, true));
    SendInput((UINT)seq.size(), seq.data(), sizeof(INPUT));
}

static INPUT Mouse(DWORD flags, DWORD data)
{
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    return in;
}

static void XButton(DWORD which)
{
    INPUT seq[2] = { Mouse(MOUSEEVENTF_XDOWN, which), Mouse(MOUSEEVENTF_XUP, which) };
    SendInput(2, seq, sizeof(INPUT));
}

void WindowsOutput::Scroll(int vertical, int horizontal)
{
    INPUT seq[2];
    UINT n = 0;
    if (vertical)
        seq[n++] = Mouse(MOUSEEVENTF_WHEEL, (DWORD)vertical);
    if (horizontal)
        seq[n++] = Mouse(MOUSEEVENTF_HWHEEL, (DWORD)horizontal);
    if (n)
        SendInput(n, seq, sizeof(INPUT));
}

void WindowsOutput::Trace(const char *line)
{
    if (verbose_)
        printf("%s\n", line);
}

void WindowsOutput::Perform(Action action)
{
    if (verbose_)
        printf("gesture: %s\n", ActionName(action));

    switch (action) {
    case Action::Back:         XButton(XBUTTON1); break;   // same as a mouse's side "back" button
    case Action::Forward:      XButton(XBUTTON2); break;
    case Action::DesktopLeft:  Chord({ VK_LCONTROL, VK_LWIN }, VK_LEFT); break;
    case Action::DesktopRight: Chord({ VK_LCONTROL, VK_LWIN }, VK_RIGHT); break;
    case Action::TaskView:     Chord({ VK_LWIN }, VK_TAB); break;
    case Action::ZoomIn: {
        // Ctrl + wheel zooms around the pointer in browsers, Office, Explorer.
        // One notch per step: apps zoom one level per notch, not per delta.
        std::vector<INPUT> seq;
        seq.push_back(Key(VK_LCONTROL, false));
        for (int i = 0; i < zoomSteps_; i++)
            seq.push_back(Mouse(MOUSEEVENTF_WHEEL, WHEEL_DELTA));
        seq.push_back(Key(VK_LCONTROL, true));
        SendInput((UINT)seq.size(), seq.data(), sizeof(INPUT));
        break;
    }
    case Action::ZoomReset:    Chord({ VK_LCONTROL }, '0'); break;
    case Action::AppNext:
    case Action::AppPrev: {
        // Alt stays down until AppEnd, so the Alt+Tab switcher stays open.
        std::vector<INPUT> seq;
        if (!altHeld_) {
            seq.push_back(Key(VK_LMENU, false));
            altHeld_ = true;
        }
        bool back = action == Action::AppPrev;
        if (back) seq.push_back(Key(VK_LSHIFT, false));
        seq.push_back(Key(VK_TAB, false));
        seq.push_back(Key(VK_TAB, true));
        if (back) seq.push_back(Key(VK_LSHIFT, true));
        SendInput((UINT)seq.size(), seq.data(), sizeof(INPUT));
        break;
    }
    case Action::AppEnd:       ReleaseHeldKeys(); break;
    default:                   break;
    }
}

void WindowsOutput::ReleaseHeldKeys()
{
    if (!altHeld_)
        return;
    INPUT up = Key(VK_LMENU, true);
    SendInput(1, &up, sizeof(INPUT));
    altHeld_ = false;
}
