/*
 * MM2Filter - KMDF lower filter under HidBth for the Apple Magic Mouse 2.
 *
 * Position: LowerFilters of the BTHENUM\{00001124-...}_VID&0001004c_PID&0269
 * devnode, i.e. between hidbth.sys (above) and BTHENUM/BTHPORT (below). All
 * traffic here is IOCTL_INTERNAL_BTH_SUBMIT_BRB (L2CAP), not HID IOCTLs.
 *
 * What it does, and nothing more:
 *   1. Tracks the HID control (PSM 0x11) and interrupt (PSM 0x13) channels.
 *   2. When the interrupt channel opens, sends SET_REPORT(Feature) F1 02 01 on
 *      the control channel to switch the mouse to multitouch reports, before
 *      hidbth is told the channel is open.
 *   3. For every multitouch input report 0x12 (14 + 8*N bytes) it copies the
 *      raw report to the sideband control device \\.\MM2Touch, and hands
 *      hidbth only the first 8 bytes, which is exactly the native mouse report
 *      its SDP descriptor declares. Pointer movement and clicks keep working.
 *
 * No touch decoding or gesture logic lives in the kernel.
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <bthdef.h>
#include <bthguid.h>
#include <bthddi.h>
#include <bthioctl.h>

#include "mm2_ioctl.h"

#define MM2_POOL_TAG                 'f2MM'

#define MM2_PSM_HID_CONTROL          0x0011
#define MM2_PSM_HID_INTERRUPT        0x0013

/* Bluetooth HID (HIDP) transaction headers. */
#define MM2_HIDP_HANDSHAKE_SUCCESS   0x00
#define MM2_HIDP_DATA_INPUT          0xA1   /* DATA | Input */
#define MM2_HIDP_SET_REPORT_FEATURE  0x53   /* SET_REPORT | Feature */

#define MM2_REPORT_ID_TOUCH          0x12
/* Native 0x12 mouse report incl. report ID: buttons(1) X(2) Y(2) const(2). */
#define MM2_NATIVE_REPORT_BYTES      8

/* Retry gap for the multitouch enable while the mouse sends plain mouse frames.
 * Right after connecting the mouse ignores a few attempts; 2000 ms made that
 * take ~10 s. The enable is a 4-byte control write, so retrying often is cheap. */
#define MM2_REENABLE_INTERVAL_MS     500

#define MM2_BOUNCE_BYTES             256
#define MM2_RING_FRAMES              64
#define MM2_INVALID_SLOT             ((ULONG)-1)

typedef struct _MM2_DEVICE_CONTEXT {
    WDFDEVICE       Device;
    WDFIOTARGET     Target;
    ULONG           Slot;               /* index in g_Mm2.Mice, or MM2_INVALID_SLOT */

    BTH_PROFILE_DRIVER_INTERFACE BthIf;
    BOOLEAN         BthIfValid;

    /* Channel state, protected by Lock. */
    WDFSPINLOCK     Lock;
    BTH_ADDR        BtAddress;
    L2CAP_CHANNEL_HANDLE ControlChannel;
    L2CAP_CHANNEL_HANDLE InterruptChannel;

    /* Interrupt-channel open request held back while we enable multitouch. */
    WDFREQUEST      HeldOpenRequest;
    NTSTATUS        HeldOpenStatus;
    ULONG_PTR       HeldOpenInformation;
    WDFWORKITEM     EnableWorkItem;

    /* Multitouch enable transaction (PASSIVE_LEVEL, serialized). */
    WDFWAITLOCK     EnableLock;
    UCHAR           ControlBuffer[64];  /* nonpaged: device contexts are nonpaged */

    /* Status for IOCTL_MM2_GET_INFO. */
    volatile LONG   MultitouchEnabled;
    volatile LONG   LastEnableStatus;
    volatile LONG   TouchFrames;
    volatile LONG   LegacyFrames;

    /* Set after our enable write; the next control-channel read is the reply. */
    volatile LONG   AwaitingHandshake;

    /*
     * Self-healing: a plain 8-byte report means the mouse is in mouse mode
     * (hidbth's own setup after connect can reset it). Re-send the enable from
     * this work item, at most once per MM2_REENABLE_INTERVAL_MS.
     */
    WDFWORKITEM     ReenableWorkItem;
    volatile LONG64 LastEnableTime;     /* KeQueryInterruptTime, 100 ns units */

    /* Diagnostics for IOCTL_MM2_GET_DIAG. Code tables are protected by Lock. */
    MM2_DIAG_MOUSE  Diag;
} MM2_DEVICE_CONTEXT, *PMM2_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MM2_DEVICE_CONTEXT, Mm2GetDeviceContext)

/* Per-request state for interrupt-channel reads we redirect to a bounce buffer. */
typedef struct _MM2_REQUEST_CONTEXT {
    PBRB            Brb;
    BOOLEAN         Bounced;
    PVOID           OrigBuffer;
    PMDL            OrigMdl;
    ULONG           OrigSize;
    ULONG           OrigFlags;
    UCHAR           Bounce[MM2_BOUNCE_BYTES];
} MM2_REQUEST_CONTEXT, *PMM2_REQUEST_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MM2_REQUEST_CONTEXT, Mm2GetRequestContext)

/* Driver-wide state shared by all filter instances and the control device. */
typedef struct _MM2_GLOBALS {
    WDFWAITLOCK     ControlLock;        /* PASSIVE: control device lifetime, slot table writes */
    WDFSPINLOCK     DataLock;           /* frame ring, read queue, slot table reads */
    WDFDEVICE       ControlDevice;
    WDFQUEUE        ReadQueue;          /* pended IOCTL_MM2_READ_FRAMES */
    WDFDEVICE       Mice[MM2_MAX_MICE];
    ULONG           Sequence;
    ULONG           Dropped;
    ULONG           RingHead;           /* index of oldest frame */
    ULONG           RingCount;
    MM2_FRAME       Ring[MM2_RING_FRAMES];
} MM2_GLOBALS;

extern MM2_GLOBALS g_Mm2;

/* Driver.c */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD Mm2EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE Mm2EvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE Mm2EvtDeviceReleaseHardware;
EVT_WDF_OBJECT_CONTEXT_CLEANUP Mm2EvtDeviceCleanup;

/* Bth.c */
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL Mm2EvtIoInternalDeviceControl;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Mm2EvtIoDeviceControl;
EVT_WDF_WORKITEM Mm2EvtEnableWorkItem;
EVT_WDF_WORKITEM Mm2EvtReenableWorkItem;

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Mm2EnableMultitouch(_In_ WDFDEVICE Device);

/* Control.c */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Mm2ControlInitialize(_In_ WDFDRIVER Driver);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Mm2ControlAttach(_In_ WDFDEVICE Mouse, _Out_ PULONG Slot);

_IRQL_requires_(PASSIVE_LEVEL)
VOID Mm2ControlDetach(_In_ ULONG Slot);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID Mm2ControlPushFrame(_In_ ULONG Slot, _In_reads_bytes_(Length) const UCHAR *Report, _In_ ULONG Length);
