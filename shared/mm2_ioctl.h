/*
 * mm2_ioctl.h - contract between MM2Filter.sys and user mode.
 *
 * The filter exposes a control device \\.\MM2Touch. User mode reads raw
 * Magic Mouse 2 multitouch frames from it. Frames are passed through
 * byte-for-byte: no decoding happens in the kernel (see mm2_touch.h for that).
 *
 * Included by both kernel (ntddk.h) and user mode (windows.h + winioctl.h).
 */
#pragma once

#define MM2_CONTROL_DEVICE_NAME  L"\\Device\\MM2Touch"
#define MM2_SYMLINK_NAME         L"\\DosDevices\\MM2Touch"
#define MM2_USER_DEVICE_PATH     L"\\\\.\\MM2Touch"

#define MM2_INTERFACE_VERSION    1u

/* Largest MM2 touch report: 14-byte header + 15 touches * 8 bytes = 134. */
#define MM2_MAX_REPORT_BYTES     160u
#define MM2_MAX_MICE             4u

/*
 * IOCTL_MM2_GET_INFO     out: MM2_INFO
 * IOCTL_MM2_READ_FRAMES  out: as many MM2_FRAMEs as fit (at least one).
 *                        Pends until a frame is available. Cancel with
 *                        CancelIoEx or close the handle.
 * IOCTL_MM2_ENABLE_MT    in:  MM2_ENABLE_REQUEST. Re-sends F1 02 01.
 *                        Debug aid; the driver already does this on every
 *                        connect.
 */
#define IOCTL_MM2_GET_INFO     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_MM2_READ_FRAMES  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_MM2_ENABLE_MT    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS)
/* IOCTL_MM2_GET_DIAG     out: MM2_DIAG. What the filter has seen pass through it. */
#define IOCTL_MM2_GET_DIAG     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS)

#define MM2_DIAG_CODES         8u   /* distinct IOCTL codes remembered per kind */
#define MM2_DIAG_BRB_BUCKETS   16u  /* see Mm2BrbBucket() in Bth.c / rawtouch */

#pragma pack(push, 8)

typedef struct _MM2_FRAME {
    UINT32 Size;          /* sizeof(MM2_FRAME) */
    UINT32 Source;        /* mouse slot, index into MM2_INFO.Mice */
    UINT32 Sequence;      /* global counter; a gap means frames were dropped */
    UINT16 Length;        /* valid bytes in Report */
    UINT16 Reserved;
    INT64  Timestamp;     /* KeQueryPerformanceCounter == user-mode QueryPerformanceCounter */
    UINT8  Report[MM2_MAX_REPORT_BYTES]; /* starts at the report ID (0x12), no BT HIDP header */
} MM2_FRAME;

typedef struct _MM2_MOUSE_INFO {
    UINT64 BtAddress;           /* 0 if slot unused */
    UINT32 InUse;
    UINT32 Connected;           /* both L2CAP channels (control + interrupt) open */
    UINT32 MultitouchEnabled;   /* last F1 02 01 was acknowledged by the mouse */
    INT32  LastEnableStatus;    /* NTSTATUS of the last enable attempt */
    UINT32 TouchFrames;         /* 0x12 frames with touch data */
    UINT32 LegacyFrames;        /* 8-byte 0x12 frames (multitouch not enabled) */
} MM2_MOUSE_INFO;

typedef struct _MM2_INFO {
    UINT32 Size;                /* sizeof(MM2_INFO) */
    UINT32 Version;             /* MM2_INTERFACE_VERSION */
    UINT32 FramesDropped;       /* ring overflowed because nobody was reading */
    UINT32 Reserved;
    MM2_MOUSE_INFO Mice[MM2_MAX_MICE];
} MM2_INFO;

typedef struct _MM2_ENABLE_REQUEST {
    UINT32 Source;              /* mouse slot */
} MM2_ENABLE_REQUEST;

typedef struct _MM2_DIAG_CODES_SEEN {
    UINT32 Code[MM2_DIAG_CODES];
    UINT32 Count[MM2_DIAG_CODES];
    UINT32 Overflow;            /* requests whose code did not fit the table */
} MM2_DIAG_CODES_SEEN;

/*
 * BRB type buckets: 0 HCI_GET_LOCAL_BD_ADDR, 1..11 = 0x0100..0x010A
 * (REGISTER_SERVER, UNREGISTER_SERVER, OPEN_CHANNEL, OPEN_CHANNEL_RESPONSE,
 * CLOSE_CHANNEL, ACL_TRANSFER, UPDATE_CHANNEL, PING, INFO_REQUEST,
 * REGISTER_PSM, UNREGISTER_PSM), 12 OPEN_ENHANCED_CHANNEL,
 * 13 OPEN_ENHANCED_CHANNEL_RESPONSE, 14 0x03xx (ACL mode / link key / iface),
 * 15 anything else.
 */
typedef struct _MM2_DIAG_MOUSE {
    MM2_DIAG_CODES_SEEN InternalIoctls;  /* IRP_MJ_INTERNAL_DEVICE_CONTROL */
    MM2_DIAG_CODES_SEEN DeviceIoctls;    /* IRP_MJ_DEVICE_CONTROL (e.g. IOCTL_BTH_SDP_*) */
    UINT32 BrbTypes[MM2_DIAG_BRB_BUCKETS];
    UINT32 LastOtherBrbType;
    UINT32 OpenCompleted;                /* open/response BRBs that came back */
    UINT32 OpenAccepted;                 /* ... that we recorded as an open channel */
    INT32  LastOpenIrpStatus;
    INT32  LastOpenBrbStatus;
    UINT32 LastOpenType;
    UINT32 LastOpenResponse;
    UINT32 LastOpenPsm;
    UINT32 AclIn;
    UINT32 AclOut;
    UINT32 AclInIntercepted;             /* interrupt-channel reads we bounced */

    /* Multitouch enable (our SET_REPORT F1 02 01 on the control channel). */
    UINT32 EnableAttempts;
    INT32  EnableWriteStatus;            /* NTSTATUS of our ACL write */

    /* hidbth's own control-channel traffic (the mouse's HANDSHAKE lands here). */
    UINT32 ControlIn;                    /* completed control-channel reads */
    INT32  LastControlInStatus;
    UINT32 LastControlInLength;
    UINT8  LastControlIn[8];             /* first bytes of the last control read */
    UINT8  ControlInAfterEnable[8];      /* first control read completed after our write */
    UINT32 ControlInAfterEnableLength;
    UINT32 ControlOut;                   /* hidbth's control-channel writes */
    UINT8  LastControlOut[8];
    UINT32 LastControlOutLength;
} MM2_DIAG_MOUSE;

typedef struct _MM2_DIAG {
    UINT32 Size;                /* sizeof(MM2_DIAG) */
    UINT32 Version;             /* MM2_INTERFACE_VERSION */
    MM2_DIAG_MOUSE Mice[MM2_MAX_MICE];
} MM2_DIAG;

#pragma pack(pop)
