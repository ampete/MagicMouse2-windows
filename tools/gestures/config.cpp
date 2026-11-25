// config.cpp - mm2gestures.ini loading.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "config.h"

#include <cstdio>
#include <cwchar>

static const char kDefaultIni[] =
    "; mm2gestures.ini - settings for the Magic Mouse 2 gesture service.\r\n"
    "; Restart mm2gestures.exe after editing.\r\n"
    "\r\n"
    "[scroll]\r\n"
    "; 0 = classic (finger forward scrolls up, like a mouse wheel)\r\n"
    "; 1 = natural (macOS default: content follows the finger)\r\n"
    "natural=0\r\n"
    "; 1 = sideways finger movement scrolls horizontally\r\n"
    "; (only with one_finger_swipe=none; otherwise a sideways slide is back/forward)\r\n"
    "horizontal=1\r\n"
    "; overall scroll speed multiplier\r\n"
    "speed=1.0\r\n"
    "; extra speed for fast movements (0 = off)\r\n"
    "acceleration=1.0\r\n"
    "; keep gliding after a quick flick\r\n"
    "momentum=1\r\n"
    "; how long the glide lasts, in seconds (larger = longer)\r\n"
    "momentum_decay=0.30\r\n"
    "\r\n"
    "[gestures]\r\n"
    "; back_forward | desktops | app_switch | none\r\n"
    "; back_forward: swipe left = back, swipe right = forward\r\n"
    "; app_switch: opens Alt+Tab; keep sliding to pick an app, lift to switch\r\n"
    "one_finger_swipe=back_forward\r\n"
    "two_finger_swipe=app_switch\r\n"
    "three_finger_swipe=desktops\r\n"
    "; task_view | smart_zoom | none\r\n"
    "two_finger_double_tap=task_view\r\n"
    "one_finger_double_tap=smart_zoom\r\n"
    "; how far smart zoom zooms in (zoom levels; in browsers 5 = about 200%)\r\n"
    "zoom_steps=5\r\n";

static std::wstring ReadString(const std::wstring &path, const wchar_t *section, const wchar_t *key,
                               const wchar_t *def)
{
    wchar_t buf[128];
    GetPrivateProfileStringW(section, key, def, buf, (DWORD)std::size(buf), path.c_str());
    return buf;
}

static double ReadDouble(const std::wstring &path, const wchar_t *section, const wchar_t *key, double def)
{
    std::wstring s = ReadString(path, section, key, L"");
    if (s.empty())
        return def;
    wchar_t *end = nullptr;
    double v = wcstod(s.c_str(), &end);
    return end != s.c_str() ? v : def;
}

static bool ReadBool(const std::wstring &path, const wchar_t *section, const wchar_t *key, bool def)
{
    return GetPrivateProfileIntW(section, key, def ? 1 : 0, path.c_str()) != 0;
}

static SwipeMapping ReadSwipe(const std::wstring &path, const wchar_t *key, SwipeMapping def)
{
    std::wstring s = ReadString(path, L"gestures", key, L"");
    if (s == L"back_forward") return SwipeMapping::BackForward;
    if (s == L"desktops")     return SwipeMapping::Desktops;
    if (s == L"app_switch")   return SwipeMapping::AppSwitch;
    if (s == L"none")         return SwipeMapping::None;
    return def;
}

static TapMapping ReadTap(const std::wstring &path, const wchar_t *key, TapMapping def)
{
    std::wstring s = ReadString(path, L"gestures", key, L"");
    if (s == L"task_view")  return TapMapping::TaskView;
    if (s == L"smart_zoom") return TapMapping::SmartZoom;
    if (s == L"none")       return TapMapping::None;
    return def;
}

GestureConfig LoadConfig(const std::wstring &path)
{
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, kDefaultIni, (DWORD)(sizeof(kDefaultIni) - 1), &written, nullptr);
            CloseHandle(h);
        }
    }

    GestureConfig c;
    c.naturalScrolling   = ReadBool(path, L"scroll", L"natural", c.naturalScrolling);
    c.horizontalScroll   = ReadBool(path, L"scroll", L"horizontal", c.horizontalScroll);
    c.scrollSpeed        = ReadDouble(path, L"scroll", L"speed", c.scrollSpeed);
    c.acceleration       = ReadDouble(path, L"scroll", L"acceleration", c.acceleration);
    c.momentum           = ReadBool(path, L"scroll", L"momentum", c.momentum);
    c.momentumDecay      = ReadDouble(path, L"scroll", L"momentum_decay", c.momentumDecay);
    c.oneFingerSwipe     = ReadSwipe(path, L"one_finger_swipe", c.oneFingerSwipe);
    c.twoFingerSwipe     = ReadSwipe(path, L"two_finger_swipe", c.twoFingerSwipe);
    c.threeFingerSwipe   = ReadSwipe(path, L"three_finger_swipe", c.threeFingerSwipe);
    c.twoFingerDoubleTap = ReadTap(path, L"two_finger_double_tap", c.twoFingerDoubleTap);
    c.oneFingerDoubleTap = ReadTap(path, L"one_finger_double_tap", c.oneFingerDoubleTap);
    c.zoomSteps          = (int)GetPrivateProfileIntW(L"gestures", L"zoom_steps", c.zoomSteps, path.c_str());
    if (c.zoomSteps < 1)  c.zoomSteps = 1;
    if (c.zoomSteps > 15) c.zoomSteps = 15;
    return c;
}

static const char *Name(SwipeMapping m)
{
    return m == SwipeMapping::BackForward ? "back/forward" : m == SwipeMapping::Desktops ? "switch desktop"
         : m == SwipeMapping::AppSwitch ? "switch apps" : "off";
}

static const char *Name(TapMapping m)
{
    return m == TapMapping::TaskView ? "Task View" : m == TapMapping::SmartZoom ? "smart zoom" : "off";
}

std::string DescribeConfig(const GestureConfig &c)
{
    char buf[512];
    snprintf(buf, sizeof(buf),
             "  scrolling: %s, horizontal %s, speed %.2f, acceleration %.2f, momentum %s (%.2fs)\n"
             "  1-finger swipe: %s   2-finger swipe: %s   3-finger swipe: %s\n"
             "  2-finger double-tap: %s   1-finger double-tap: %s\n",
             c.naturalScrolling ? "natural" : "classic",
             c.horizontalScroll && c.oneFingerSwipe == SwipeMapping::None ? "on" : "off",
             c.scrollSpeed, c.acceleration, c.momentum ? "on" : "off", c.momentumDecay,
             Name(c.oneFingerSwipe), Name(c.twoFingerSwipe), Name(c.threeFingerSwipe),
             Name(c.twoFingerDoubleTap), Name(c.oneFingerDoubleTap));
    return buf;
}
