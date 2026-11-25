// rawtouch - print Magic Mouse 2 multitouch frames from the MM2Filter sideband.
//
//   rawtouch            show driver status, then stream decoded frames
//   rawtouch info       show driver status only
//   rawtouch diag       status + what the filter has seen (IOCTLs, BRB types)
//   rawtouch enable [n] re-send the multitouch enable (F1 02 01) to mouse slot n
//   rawtouch --hex      also dump the raw report bytes of every frame
//
// Ctrl+C stops streaming.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "mm2_ioctl.h"
#include "mm2_touch.h"

static HANDLE g_device = INVALID_HANDLE_VALUE;
static volatile LONG g_stop = 0;

static BOOL WINAPI OnCtrl(DWORD)
{
    InterlockedExchange(&g_stop, 1);
    if (g_device != INVALID_HANDLE_VALUE)
        CancelIoEx(g_device, nullptr);   // unblocks the pending READ_FRAMES
    return TRUE;
}

static const char *StateName(int state)
{
    switch (state) {
    case MM2_TOUCH_STATE_NONE:  return "none ";
    case MM2_TOUCH_STATE_START: return "start";
    case MM2_TOUCH_STATE_DRAG:  return "drag ";
    default:                    return "0x?? ";
    }
}

static bool PrintInfo()
{
    MM2_INFO info = {};
    DWORD got = 0;
    if (!DeviceIoControl(g_device, IOCTL_MM2_GET_INFO, nullptr, 0, &info, sizeof(info), &got, nullptr)) {
        fprintf(stderr, "IOCTL_MM2_GET_INFO failed: %lu\n", GetLastError());
        return false;
    }
    if (info.Version != MM2_INTERFACE_VERSION || info.Size != sizeof(info)) {
        fprintf(stderr, "Driver interface version %u (size %u), tool expects %u (size %zu). Rebuild both.\n",
                info.Version, info.Size, MM2_INTERFACE_VERSION, sizeof(info));
        return false;
    }

    printf("MM2Filter interface v%u, frames dropped: %u\n", info.Version, info.FramesDropped);
    bool any = false;
    for (unsigned i = 0; i < MM2_MAX_MICE; i++) {
        const MM2_MOUSE_INFO &m = info.Mice[i];
        if (!m.InUse)
            continue;
        any = true;
        uint64_t a = m.BtAddress;
        printf("  slot %u  %02X:%02X:%02X:%02X:%02X:%02X  %s  multitouch=%s (last status 0x%08X)  touch frames=%u  legacy frames=%u\n",
               i,
               (unsigned)(a >> 40) & 0xFF, (unsigned)(a >> 32) & 0xFF, (unsigned)(a >> 24) & 0xFF,
               (unsigned)(a >> 16) & 0xFF, (unsigned)(a >> 8) & 0xFF, (unsigned)a & 0xFF,
               m.Connected ? "connected   " : "disconnected",
               m.MultitouchEnabled ? "on" : "off",
               (unsigned)m.LastEnableStatus, m.TouchFrames, m.LegacyFrames);
        if (m.Connected && m.LegacyFrames > 0 && m.TouchFrames == 0)
            printf("    -> only 8-byte reports so far: multitouch is not enabled. Try 'rawtouch enable %u'.\n", i);
    }
    if (!any)
        printf("  no mouse attached to the filter\n");
    return true;
}

static const char *BrbBucketName(unsigned b)
{
    static const char *names[MM2_DIAG_BRB_BUCKETS] = {
        "HCI_GET_LOCAL_BD_ADDR", "L2CA_REGISTER_SERVER", "L2CA_UNREGISTER_SERVER",
        "L2CA_OPEN_CHANNEL", "L2CA_OPEN_CHANNEL_RESPONSE", "L2CA_CLOSE_CHANNEL",
        "L2CA_ACL_TRANSFER", "L2CA_UPDATE_CHANNEL", "L2CA_PING", "L2CA_INFO_REQUEST",
        "REGISTER_PSM", "UNREGISTER_PSM", "L2CA_OPEN_ENHANCED_CHANNEL",
        "L2CA_OPEN_ENHANCED_CHANNEL_RESPONSE", "0x03xx (ACL mode/link key/iface)", "other",
    };
    return b < MM2_DIAG_BRB_BUCKETS ? names[b] : "?";
}

static void PrintCodes(const char *title, const MM2_DIAG_CODES_SEEN &t)
{
    printf("    %s:\n", title);
    bool any = false;
    for (unsigned i = 0; i < MM2_DIAG_CODES; i++) {
        if (!t.Count[i])
            continue;
        any = true;
        printf("      0x%08X  x%u\n", t.Code[i], t.Count[i]);
    }
    if (!any)
        printf("      (none)\n");
    if (t.Overflow)
        printf("      + %u with other codes\n", t.Overflow);
}

static int Diag()
{
    MM2_DIAG d = {};
    DWORD got = 0;
    if (!DeviceIoControl(g_device, IOCTL_MM2_GET_DIAG, nullptr, 0, &d, sizeof(d), &got, nullptr)) {
        fprintf(stderr, "IOCTL_MM2_GET_DIAG failed: %lu (driver older than this tool?)\n", GetLastError());
        return 1;
    }
    for (unsigned i = 0; i < MM2_MAX_MICE; i++) {
        const MM2_DIAG_MOUSE &m = d.Mice[i];
        bool used = m.OpenCompleted || m.AclIn || m.AclOut;
        for (unsigned k = 0; k < MM2_DIAG_CODES && !used; k++)
            used = m.InternalIoctls.Count[k] || m.DeviceIoctls.Count[k];
        if (!used && i > 0)
            continue;
        printf("  slot %u\n", i);
        PrintCodes("internal IOCTLs (0x00410003 = SUBMIT_BRB)", m.InternalIoctls);
        PrintCodes("device IOCTLs (0x004102xx = SDP)", m.DeviceIoctls);
        printf("    BRB types:\n");
        for (unsigned b = 0; b < MM2_DIAG_BRB_BUCKETS; b++)
            if (m.BrbTypes[b])
                printf("      %-36s x%u\n", BrbBucketName(b), m.BrbTypes[b]);
        if (m.BrbTypes[15])
            printf("      (last 'other' type 0x%04X)\n", m.LastOtherBrbType);
        printf("    opens completed=%u accepted=%u  last: type=0x%04X irp=0x%08X brb=0x%08X response/psm=0x%04X/0x%04X\n",
               m.OpenCompleted, m.OpenAccepted, m.LastOpenType, (unsigned)m.LastOpenIrpStatus,
               (unsigned)m.LastOpenBrbStatus, m.LastOpenResponse, m.LastOpenPsm);
        printf("    ACL in=%u out=%u  interrupt reads intercepted=%u\n", m.AclIn, m.AclOut, m.AclInIntercepted);
        auto hexbytes = [](const UINT8 *b, UINT32 len) {
            for (UINT32 i = 0; i < len && i < 8; i++)
                printf(" %02X", b[i]);
            if (!len)
                printf(" (none)");
        };
        printf("    enable: attempts=%u  write status=0x%08X\n", m.EnableAttempts, (unsigned)m.EnableWriteStatus);
        printf("    reply after enable (len %u):", m.ControlInAfterEnableLength);
        hexbytes(m.ControlInAfterEnable, m.ControlInAfterEnableLength);
        printf("   [00 = success, 01 not ready, 02 bad report ID, 03 unsupported, 04 bad parameter]\n");
        printf("    control in=%u (last status 0x%08X, len %u):", m.ControlIn, (unsigned)m.LastControlInStatus, m.LastControlInLength);
        hexbytes(m.LastControlIn, m.LastControlInLength);
        printf("\n    control out (hidbth)=%u, last (len %u):", m.ControlOut, m.LastControlOutLength);
        hexbytes(m.LastControlOut, m.LastControlOutLength);
        printf("\n");
    }
    return 0;
}

static int Enable(unsigned slot)
{
    MM2_ENABLE_REQUEST req = { slot };
    DWORD got = 0;
    if (!DeviceIoControl(g_device, IOCTL_MM2_ENABLE_MT, &req, sizeof(req), nullptr, 0, &got, nullptr)) {
        fprintf(stderr, "IOCTL_MM2_ENABLE_MT(slot %u) failed: %lu\n", slot, GetLastError());
        return 1;
    }
    printf("multitouch enable acknowledged by mouse in slot %u\n", slot);
    return 0;
}

static int Stream(bool hex)
{
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    std::vector<MM2_FRAME> frames(32);
    uint32_t lastSeq = 0;
    int64_t lastTs = 0;

    printf("streaming, Ctrl+C to stop\n");
    while (!g_stop) {
        DWORD got = 0;
        if (!DeviceIoControl(g_device, IOCTL_MM2_READ_FRAMES, nullptr, 0,
                             frames.data(), (DWORD)(frames.size() * sizeof(MM2_FRAME)), &got, nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED && g_stop)
                break;
            fprintf(stderr, "IOCTL_MM2_READ_FRAMES failed: %lu\n", err);
            return 1;
        }

        for (DWORD k = 0; k < got / sizeof(MM2_FRAME); k++) {
            const MM2_FRAME &f = frames[k];
            if (lastSeq != 0 && f.Sequence != lastSeq + 1)
                printf("  ** %u frame(s) dropped\n", f.Sequence - lastSeq - 1);
            double dtMs = lastTs ? (double)(f.Timestamp - lastTs) * 1000.0 / (double)freq.QuadPart : 0.0;
            lastSeq = f.Sequence;
            lastTs = f.Timestamp;

            mm2_report r;
            if (mm2_decode(f.Report, f.Length, &r) != 0) {
                printf("#%u src%u len=%u (not a multitouch 0x12 report)\n", f.Sequence, f.Source, f.Length);
                continue;
            }

            printf("#%-6u +%5.1fms src%u btn=%c%c d=(%4d,%4d) touches=%d\n",
                   f.Sequence, dtMs, f.Source,
                   (r.buttons & 1) ? 'L' : '-', (r.buttons & 2) ? 'R' : '-',
                   r.dx, r.dy, r.touch_count);
            for (int i = 0; i < r.touch_count; i++) {
                const mm2_touch &t = r.touches[i];
                printf("    id=%-2d %s raw=0x%02X x=%5d y=%5d major=%3d minor=%3d size=%2d orient=%3d\n",
                       t.id, StateName(t.state), t.state, t.x, t.y,
                       t.touch_major, t.touch_minor, t.size, t.orientation);
            }
            if (hex) {
                printf("    raw:");
                for (unsigned b = 0; b < f.Length; b++)
                    printf(" %02X", f.Report[b]);
                printf("\n");
            }
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    bool hex = false, infoOnly = false, enable = false, diag = false;
    unsigned slot = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hex")) hex = true;
        else if (!strcmp(argv[i], "info")) infoOnly = true;
        else if (!strcmp(argv[i], "diag")) diag = true;
        else if (!strcmp(argv[i], "enable")) {
            enable = true;
            if (i + 1 < argc) slot = (unsigned)strtoul(argv[++i], nullptr, 10);
        } else {
            fprintf(stderr, "usage: rawtouch [info | diag | enable [slot]] [--hex]\n");
            return 2;
        }
    }

    g_device = CreateFileW(MM2_USER_DEVICE_PATH, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_device == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND)
            fprintf(stderr, "\\\\.\\MM2Touch not found: MM2Filter is not loaded or no Magic Mouse 2 is attached.\n");
        else
            fprintf(stderr, "cannot open \\\\.\\MM2Touch: error %lu\n", err);
        return 1;
    }
    SetConsoleCtrlHandler(OnCtrl, TRUE);

    int rc;
    if (enable) {
        rc = Enable(slot);
    } else if (diag) {
        rc = PrintInfo() ? Diag() : 1;
    } else if (!PrintInfo()) {
        rc = 1;
    } else {
        rc = infoOnly ? 0 : Stream(hex);
    }

    CloseHandle(g_device);
    return rc;
}
