// mm2gestures - Magic Mouse 2 gestures for Windows (user mode).
//
// Reads raw touch frames from MM2Filter (\\.\MM2Touch), recognizes gestures
// and injects scrolling / shortcuts with SendInput.
//
//   mm2gestures                 run (Ctrl+C to quit)
//   mm2gestures --verbose       also print recognized gestures
//   mm2gestures --config <ini>  use another settings file
//                               (default: mm2gestures.ini next to the exe)
//   mm2gestures --stop          stop the running instance (e.g. the hidden
//                               mm2gesturesw.exe started at login)
//
// mm2gesturesw.exe is this same program built without a console window
// (MM2_BACKGROUND); it takes the same options.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "mm2_ioctl.h"
#include "mm2_touch.h"

#include "config.h"
#include "engine.h"
#include "inject.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Named, so "mm2gestures --stop" can signal a running (possibly hidden) instance.
static const wchar_t kQuitEventName[] = L"Local\\MM2GesturesQuit";
static HANDLE g_quit;

static BOOL WINAPI OnCtrl(DWORD)
{
    SetEvent(g_quit);
    return TRUE;
}

static double Seconds(int64_t qpc)
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    return (double)qpc / (double)freq.QuadPart;
}

static double Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return Seconds(t.QuadPart);
}

static TouchFrame ToTouchFrame(const MM2_FRAME &f, const mm2_report &r)
{
    TouchFrame out;
    out.t = Seconds(f.Timestamp);
    out.buttons = r.buttons;
    out.dx = r.dx;
    out.dy = r.dy;
    for (int i = 0; i < r.touch_count; i++) {
        const mm2_touch &t = r.touches[i];
        bool active = t.state == MM2_TOUCH_STATE_START || t.state == MM2_TOUCH_STATE_DRAG;
        // Measured on the device: the decoder's y grows toward the user. The
        // engine wants y growing away from the user (forward), so flip it.
        out.touches.push_back({ t.id, t.x, -t.y, active });
    }
    return out;
}

static std::wstring DefaultConfigPath()
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe, n);
    size_t slash = path.find_last_of(L"\\/");
    return path.substr(0, slash + 1) + L"mm2gestures.ini";
}

// Runs until the device goes away (returns true) or the user quits (false).
static bool RunSession(HANDLE device, GestureEngine &engine, HANDLE timer)
{
    std::vector<MM2_FRAME> frames(32);
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool timerRunning = false;
    bool deviceGone = false;

    for (;;) {
        ResetEvent(ov.hEvent);
        DWORD got = 0;
        BOOL ok = DeviceIoControl(device, IOCTL_MM2_READ_FRAMES, nullptr, 0, frames.data(),
                                  (DWORD)(frames.size() * sizeof(MM2_FRAME)), &got, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            deviceGone = true;
            break;
        }

        // Wait for frames, the momentum timer, or quit.
        bool readDone = false;
        while (!readDone) {
            bool wantTicks = engine.WantsTicks();
            if (wantTicks && !timerRunning) {
                LARGE_INTEGER due;
                due.QuadPart = -80000;          // 8 ms, then every 8 ms
                SetWaitableTimer(timer, &due, 8, nullptr, nullptr, FALSE);
                timerRunning = true;
            } else if (!wantTicks && timerRunning) {
                CancelWaitableTimer(timer);
                timerRunning = false;
            }

            HANDLE handles[3] = { ov.hEvent, g_quit, timer };
            DWORD w = WaitForMultipleObjects(timerRunning ? 3 : 2, handles, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0 + 1) {       // quit
                CancelIoEx(device, &ov);
                GetOverlappedResult(device, &ov, &got, TRUE);
                CloseHandle(ov.hEvent);
                return false;
            }
            if (w == WAIT_OBJECT_0 + 2) {       // momentum tick
                engine.OnTick(Now());
                continue;
            }
            readDone = true;
        }

        if (!GetOverlappedResult(device, &ov, &got, FALSE)) {
            deviceGone = true;
            break;
        }
        for (DWORD k = 0; k < got / sizeof(MM2_FRAME); k++) {
            mm2_report r;
            if (mm2_decode(frames[k].Report, frames[k].Length, &r) == 0)
                engine.OnFrame(ToTouchFrame(frames[k], r));
        }
    }

    if (timerRunning)
        CancelWaitableTimer(timer);
    CloseHandle(ov.hEvent);
    return deviceGone;
}

static int StopRunning()
{
    HANDLE quit = OpenEventW(EVENT_MODIFY_STATE, FALSE, kQuitEventName);
    if (!quit) {
        printf("mm2gestures is not running.\n");
        return 1;
    }
    SetEvent(quit);
    CloseHandle(quit);
    printf("asked mm2gestures to stop.\n");
    return 0;
}

static int Run(int argc, wchar_t **argv)
{
    bool verbose = false;
    std::wstring configPath = DefaultConfigPath();

    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--verbose")) {
            verbose = true;
        } else if (!wcscmp(argv[i], L"--config") && i + 1 < argc) {
            configPath = argv[++i];
        } else if (!wcscmp(argv[i], L"--stop")) {
            return StopRunning();
        } else {
            fprintf(stderr, "usage: mm2gestures [--verbose] [--config <file.ini>] | --stop\n");
            return 2;
        }
    }

    // One instance only, or every scroll would be sent twice.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\MM2Gestures");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "mm2gestures is already running (maybe started at login).\n"
                        "Stop it first with: mm2gestures --stop\n");
        return 1;
    }

    GestureConfig config = LoadConfig(configPath);
    printf("mm2gestures - settings from %ls\n%s", configPath.c_str(), DescribeConfig(config).c_str());

    g_quit = CreateEventW(nullptr, TRUE, FALSE, kQuitEventName);
    ResetEvent(g_quit);
    SetConsoleCtrlHandler(OnCtrl, TRUE);

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    WindowsOutput output(verbose, config.zoomSteps);
    bool announcedWait = false;

    for (;;) {
        HANDLE device = CreateFileW(MM2_USER_DEVICE_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (device == INVALID_HANDLE_VALUE) {
            if (!announcedWait) {
                printf("waiting for the Magic Mouse 2 (MM2Filter not attached yet)...\n");
                announcedWait = true;
            }
            if (WaitForSingleObject(g_quit, 2000) == WAIT_OBJECT_0)
                break;
            continue;
        }

        printf("connected - gestures active. Ctrl+C to quit.\n");
        announcedWait = false;
        GestureEngine engine(config, output);   // fresh state per connection
        bool gone = RunSession(device, engine, timer);
        output.ReleaseHeldKeys();               // never leave Alt stuck down
        CloseHandle(device);
        if (!gone)
            break;
        printf("mouse went away; waiting for it to come back...\n");
    }

    printf("bye\n");
    CloseHandle(timer);
    CloseHandle(g_quit);
    ReleaseMutex(single);
    CloseHandle(single);
    return 0;
}

#ifdef MM2_BACKGROUND
// No console: printf output goes nowhere, which is fine for the login copy.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return Run(__argc, __wargv);
}
#else
int wmain(int argc, wchar_t **argv)
{
    return Run(argc, argv);
}
#endif
