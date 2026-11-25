/*
 * Control.c - sideband control device \\.\MM2Touch.
 *
 * Created when the first mouse attaches and deleted when the last one goes
 * away (the toaster filter sample's pattern for control devices in PnP
 * filter drivers). Raw touch reports land in a small ring. IOCTL_MM2_READ_FRAMES
 * drains the ring, or pends until the next frame arrives.
 */
#include "MM2Filter.h"

MM2_GLOBALS g_Mm2;

static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Mm2EvtControlIoDeviceControl;

/*
 * SYSTEM and Administrators: full access. Interactive users: read/write, so a
 * per-user gesture service can read frames without elevation.
 */
DECLARE_CONST_UNICODE_STRING(g_Mm2ControlSddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)");

/*
 * Attach/Detach run at PASSIVE_LEVEL but take DataLock (a spinlock), so they
 * must stay in nonpaged code. Do not add them to a PAGE section.
 */
#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, Mm2ControlInitialize)
#endif

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
Mm2ControlInitialize(
    _In_ WDFDRIVER Driver)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    RtlZeroMemory(&g_Mm2, sizeof(g_Mm2));

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Driver;
    status = WdfWaitLockCreate(&attributes, &g_Mm2.ControlLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Driver;
    return WdfSpinLockCreate(&attributes, &g_Mm2.DataLock);
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS
Mm2ControlCreate(
    _In_ WDFDRIVER Driver,
    _Out_ WDFDEVICE *ControlDevice,
    _Out_ WDFQUEUE *ReadQueue)
{
    DECLARE_CONST_UNICODE_STRING(name, MM2_CONTROL_DEVICE_NAME);
    DECLARE_CONST_UNICODE_STRING(link, MM2_SYMLINK_NAME);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    PWDFDEVICE_INIT init;
    WDFDEVICE device;
    NTSTATUS status;

    *ControlDevice = NULL;
    *ReadQueue = NULL;

    init = WdfControlDeviceInitAllocate(Driver, &g_Mm2ControlSddl);
    if (init == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WdfDeviceInitSetIoType(init, WdfDeviceIoBuffered);
    WdfDeviceInitSetCharacteristics(init, FILE_DEVICE_SECURE_OPEN, FALSE);
    status = WdfDeviceInitAssignName(init, &name);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(init);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(init);
        return status;
    }

    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = Mm2EvtControlIoDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }

    /* Pended reads. Requests cancelled here are completed by the framework. */
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, ReadQueue);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }

    WdfControlFinishInitializing(device);
    *ControlDevice = device;
    return STATUS_SUCCESS;

fail:
    WdfObjectDelete(device);
    *ReadQueue = NULL;
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
Mm2ControlAttach(
    _In_ WDFDEVICE Mouse,
    _Out_ PULONG Slot)
{
    NTSTATUS status = STATUS_SUCCESS;
    WDFDEVICE control = NULL;
    WDFQUEUE readQueue = NULL;
    ULONG i;

    *Slot = MM2_INVALID_SLOT;

    WdfWaitLockAcquire(g_Mm2.ControlLock, NULL);

    for (i = 0; i < MM2_MAX_MICE; i++) {
        if (g_Mm2.Mice[i] == NULL) {
            break;
        }
    }
    if (i == MM2_MAX_MICE) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

    if (g_Mm2.ControlDevice == NULL) {
        status = Mm2ControlCreate(WdfDeviceGetDriver(Mouse), &control, &readQueue);
        if (!NT_SUCCESS(status)) {
            goto exit;
        }
    }

    WdfSpinLockAcquire(g_Mm2.DataLock);
    if (control != NULL) {
        g_Mm2.ControlDevice = control;
        g_Mm2.ReadQueue = readQueue;
        g_Mm2.RingHead = 0;
        g_Mm2.RingCount = 0;
    }
    g_Mm2.Mice[i] = Mouse;
    WdfSpinLockRelease(g_Mm2.DataLock);
    *Slot = i;

exit:
    WdfWaitLockRelease(g_Mm2.ControlLock);
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
Mm2ControlDetach(
    _In_ ULONG Slot)
{
    WDFDEVICE control = NULL;
    BOOLEAN anyLeft = FALSE;
    ULONG i;

    if (Slot >= MM2_MAX_MICE) {
        return;
    }

    WdfWaitLockAcquire(g_Mm2.ControlLock, NULL);

    WdfSpinLockAcquire(g_Mm2.DataLock);
    g_Mm2.Mice[Slot] = NULL;
    for (i = 0; i < MM2_MAX_MICE; i++) {
        if (g_Mm2.Mice[i] != NULL) {
            anyLeft = TRUE;
        }
    }
    if (!anyLeft) {
        control = g_Mm2.ControlDevice;
        g_Mm2.ControlDevice = NULL;
        g_Mm2.ReadQueue = NULL;
        g_Mm2.RingCount = 0;
    }
    WdfSpinLockRelease(g_Mm2.DataLock);

    if (control != NULL) {
        WdfObjectDelete(control);   /* purges ReadQueue: pending reads complete as cancelled */
    }

    WdfWaitLockRelease(g_Mm2.ControlLock);
}

/* ------------------------------------------------------------------------ */
/* Frame ring                                                                */
/* ------------------------------------------------------------------------ */

/* Caller holds DataLock. Copies queued frames into Out, returns count. */
static ULONG
Mm2RingDrainLocked(
    _Out_writes_to_(Capacity, return) MM2_FRAME *Out,
    _In_ ULONG Capacity)
{
    ULONG n = 0;

    while (n < Capacity && g_Mm2.RingCount > 0) {
        Out[n++] = g_Mm2.Ring[g_Mm2.RingHead];
        g_Mm2.RingHead = (g_Mm2.RingHead + 1) % MM2_RING_FRAMES;
        g_Mm2.RingCount--;
    }
    return n;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
Mm2ControlPushFrame(
    _In_ ULONG Slot,
    _In_reads_bytes_(Length) const UCHAR *Report,
    _In_ ULONG Length)
{
    LARGE_INTEGER now = KeQueryPerformanceCounter(NULL);
    WDFREQUEST request;
    MM2_FRAME *out;
    size_t outLength;
    NTSTATUS status;

    if (Length > MM2_MAX_REPORT_BYTES) {
        Length = MM2_MAX_REPORT_BYTES;
    }

    WdfSpinLockAcquire(g_Mm2.DataLock);

    if (g_Mm2.ControlDevice == NULL) {
        WdfSpinLockRelease(g_Mm2.DataLock);
        return;
    }

    /* Append (overwriting the oldest frame if nobody is reading). */
    {
        ULONG tail;
        MM2_FRAME *frame;

        if (g_Mm2.RingCount == MM2_RING_FRAMES) {
            g_Mm2.RingHead = (g_Mm2.RingHead + 1) % MM2_RING_FRAMES;
            g_Mm2.RingCount--;
            g_Mm2.Dropped++;
        }
        tail = (g_Mm2.RingHead + g_Mm2.RingCount) % MM2_RING_FRAMES;
        frame = &g_Mm2.Ring[tail];
        RtlZeroMemory(frame, sizeof(*frame));
        frame->Size      = sizeof(MM2_FRAME);
        frame->Source    = Slot;
        frame->Sequence  = ++g_Mm2.Sequence;
        frame->Length    = (UINT16)Length;
        frame->Timestamp = now.QuadPart;
        RtlCopyMemory(frame->Report, Report, Length);
        g_Mm2.RingCount++;
    }

    /* Hand the backlog to a pended reader, if there is one. */
    if (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(g_Mm2.ReadQueue, &request))) {
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(MM2_FRAME), (PVOID *)&out, &outLength);
        if (NT_SUCCESS(status)) {
            ULONG n = Mm2RingDrainLocked(out, (ULONG)(outLength / sizeof(MM2_FRAME)));
            WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, n * sizeof(MM2_FRAME));
        } else {
            WdfRequestComplete(request, status);
        }
    }

    WdfSpinLockRelease(g_Mm2.DataLock);
}

/* ------------------------------------------------------------------------ */
/* IOCTLs                                                                    */
/* ------------------------------------------------------------------------ */

static VOID
Mm2EvtControlIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode)
{
    NTSTATUS status;
    size_t length;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_MM2_GET_INFO: {
        MM2_INFO *info;
        ULONG i;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(MM2_INFO), (PVOID *)&info, &length);
        if (!NT_SUCCESS(status)) {
            break;
        }
        if (length < sizeof(MM2_INFO)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        RtlZeroMemory(info, sizeof(*info));
        info->Size = sizeof(MM2_INFO);
        info->Version = MM2_INTERFACE_VERSION;

        WdfSpinLockAcquire(g_Mm2.DataLock);
        info->FramesDropped = g_Mm2.Dropped;
        for (i = 0; i < MM2_MAX_MICE; i++) {
            PMM2_DEVICE_CONTEXT ctx;

            if (g_Mm2.Mice[i] == NULL) {
                continue;
            }
            ctx = Mm2GetDeviceContext(g_Mm2.Mice[i]);
            info->Mice[i].InUse             = 1;
            info->Mice[i].BtAddress         = ctx->BtAddress;
            info->Mice[i].Connected         = (ctx->ControlChannel != NULL && ctx->InterruptChannel != NULL);
            info->Mice[i].MultitouchEnabled = (UINT32)ctx->MultitouchEnabled;
            info->Mice[i].LastEnableStatus  = ctx->LastEnableStatus;
            info->Mice[i].TouchFrames       = (UINT32)ctx->TouchFrames;
            info->Mice[i].LegacyFrames      = (UINT32)ctx->LegacyFrames;
        }
        WdfSpinLockRelease(g_Mm2.DataLock);

        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(MM2_INFO));
        return;
    }

    case IOCTL_MM2_GET_DIAG: {
        MM2_DIAG *diag;
        ULONG i;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(MM2_DIAG), (PVOID *)&diag, &length);
        if (!NT_SUCCESS(status)) {
            break;
        }
        if (length < sizeof(MM2_DIAG)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        RtlZeroMemory(diag, sizeof(*diag));
        diag->Size = sizeof(MM2_DIAG);
        diag->Version = MM2_INTERFACE_VERSION;

        WdfSpinLockAcquire(g_Mm2.DataLock);
        for (i = 0; i < MM2_MAX_MICE; i++) {
            if (g_Mm2.Mice[i] != NULL) {
                diag->Mice[i] = Mm2GetDeviceContext(g_Mm2.Mice[i])->Diag;
            }
        }
        WdfSpinLockRelease(g_Mm2.DataLock);

        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(MM2_DIAG));
        return;
    }

    case IOCTL_MM2_READ_FRAMES: {
        MM2_FRAME *out;
        ULONG n;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(MM2_FRAME), (PVOID *)&out, &length);
        if (!NT_SUCCESS(status)) {
            break;
        }

        WdfSpinLockAcquire(g_Mm2.DataLock);
        n = Mm2RingDrainLocked(out, (ULONG)(length / sizeof(MM2_FRAME)));
        if (n == 0) {
            status = (g_Mm2.ReadQueue != NULL)
                   ? WdfRequestForwardToIoQueue(Request, g_Mm2.ReadQueue)
                   : STATUS_DEVICE_NOT_CONNECTED;
            WdfSpinLockRelease(g_Mm2.DataLock);
            if (NT_SUCCESS(status)) {
                return;   /* pended */
            }
            break;
        }
        WdfSpinLockRelease(g_Mm2.DataLock);

        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, n * sizeof(MM2_FRAME));
        return;
    }

    case IOCTL_MM2_ENABLE_MT: {
        MM2_ENABLE_REQUEST *in;
        WDFDEVICE mouse = NULL;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(MM2_ENABLE_REQUEST), (PVOID *)&in, &length);
        if (!NT_SUCCESS(status)) {
            break;
        }
        if (in->Source >= MM2_MAX_MICE) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        /* ControlLock keeps the mouse from detaching while we use it. */
        WdfWaitLockAcquire(g_Mm2.ControlLock, NULL);
        mouse = g_Mm2.Mice[in->Source];
        status = (mouse != NULL) ? Mm2EnableMultitouch(mouse) : STATUS_NO_SUCH_DEVICE;
        WdfWaitLockRelease(g_Mm2.ControlLock);
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestComplete(Request, status);
}
