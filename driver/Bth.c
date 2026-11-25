/*
 * Bth.c - L2CAP (BRB) interception below hidbth.
 *
 * hidbth talks to the mouse through IOCTL_INTERNAL_BTH_SUBMIT_BRB. We look at
 * three BRB kinds and pass everything else through untouched:
 *
 *   OPEN_CHANNEL[_RESPONSE]  learn the control/interrupt channel handles;
 *                            when the interrupt channel opens, hold hidbth's
 *                            request, enable multitouch, then complete it.
 *   CLOSE_CHANNEL            forget the handle.
 *   ACL_TRANSFER (IN, intr.) redirect into a bounce buffer, copy touch
 *                            reports to the sideband, give hidbth the native
 *                            8-byte mouse report.
 */
#include "MM2Filter.h"

static EVT_WDF_REQUEST_COMPLETION_ROUTINE Mm2OpenChannelCompletion;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE Mm2AclReadCompletion;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE Mm2ControlReadCompletion;

_IRQL_requires_max_(DISPATCH_LEVEL)
static PUCHAR Mm2MapBuffer(_In_opt_ PVOID Buffer, _In_opt_ PMDL Mdl);

_IRQL_requires_max_(DISPATCH_LEVEL)
static VOID
Mm2ForwardAndForget(
    _In_ PMM2_DEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request)
{
    WDF_REQUEST_SEND_OPTIONS options;

    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
    if (!WdfRequestSend(Request, Ctx->Target, &options)) {
        WdfRequestComplete(Request, WdfRequestGetStatus(Request));
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static BOOLEAN
Mm2ForwardWithCompletion(
    _In_ PMM2_DEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _In_ PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion)
{
    WdfRequestFormatRequestUsingCurrentType(Request);
    WdfRequestSetCompletionRoutine(Request, Completion, Ctx);
    return WdfRequestSend(Request, Ctx->Target, WDF_NO_SEND_OPTIONS);
}

/*
 * Returns TRUE if Handle is the interrupt channel, which is the moment to
 * enable multitouch. For OPEN_CHANNEL_RESPONSE (mouse-initiated connect, the
 * normal reconnect case) the BRB carries no PSM, so we rely on the HID spec's
 * ordering: control (0x11) is always opened before interrupt (0x13).
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
static BOOLEAN
Mm2OnChannelOpened(
    _In_ PMM2_DEVICE_CONTEXT Ctx,
    _In_ L2CAP_CHANNEL_HANDLE Handle,
    _In_ BOOLEAN PsmKnown,
    _In_ USHORT Psm,
    _In_ BTH_ADDR Address)
{
    BOOLEAN isInterrupt;

    WdfSpinLockAcquire(Ctx->Lock);
    Ctx->BtAddress = Address;
    if (PsmKnown) {
        if (Psm == MM2_PSM_HID_CONTROL) {
            Ctx->ControlChannel = Handle;
            Ctx->InterruptChannel = NULL;
        } else if (Psm == MM2_PSM_HID_INTERRUPT) {
            Ctx->InterruptChannel = Handle;
        }
    } else if (Ctx->ControlChannel == NULL || Ctx->InterruptChannel != NULL) {
        Ctx->ControlChannel = Handle;
        Ctx->InterruptChannel = NULL;
    } else {
        Ctx->InterruptChannel = Handle;
    }
    isInterrupt = (Ctx->InterruptChannel == Handle && Ctx->ControlChannel != NULL);
    WdfSpinLockRelease(Ctx->Lock);

    if (isInterrupt) {
        InterlockedExchange(&Ctx->MultitouchEnabled, 0);
    }
    return isInterrupt;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static VOID
Mm2OnChannelClosed(
    _In_ PMM2_DEVICE_CONTEXT Ctx,
    _In_ L2CAP_CHANNEL_HANDLE Handle)
{
    WdfSpinLockAcquire(Ctx->Lock);
    if (Ctx->ControlChannel == Handle) {
        Ctx->ControlChannel = NULL;
    }
    if (Ctx->InterruptChannel == Handle) {
        Ctx->InterruptChannel = NULL;
    }
    WdfSpinLockRelease(Ctx->Lock);
    InterlockedExchange(&Ctx->MultitouchEnabled, 0);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static BOOLEAN
Mm2IsInterruptRead(
    _In_ PMM2_DEVICE_CONTEXT Ctx,
    _In_ const struct _BRB_L2CA_ACL_TRANSFER *Acl)
{
    BOOLEAN match;

    if ((Acl->TransferFlags & ACL_TRANSFER_DIRECTION_IN) == 0) {
        return FALSE;
    }
    WdfSpinLockAcquire(Ctx->Lock);
    match = (Ctx->InterruptChannel != NULL) && (Acl->ChannelHandle == Ctx->InterruptChannel);
    WdfSpinLockRelease(Ctx->Lock);
    return match;
}

/* ------------------------------------------------------------------------ */
/* Diagnostics                                                               */
/* ------------------------------------------------------------------------ */

_IRQL_requires_max_(DISPATCH_LEVEL)
static VOID
Mm2DiagCountCode(
    _In_ PMM2_DEVICE_CONTEXT Ctx,
    _Inout_ MM2_DIAG_CODES_SEEN *Table,
    _In_ ULONG Code)
{
    ULONG i;

    WdfSpinLockAcquire(Ctx->Lock);
    for (i = 0; i < MM2_DIAG_CODES; i++) {
        if (Table->Count[i] == 0) {
            Table->Code[i] = Code;
        }
        if (Table->Code[i] == Code) {
            Table->Count[i]++;
            break;
        }
    }
    if (i == MM2_DIAG_CODES) {
        Table->Overflow++;
    }
    WdfSpinLockRelease(Ctx->Lock);
}

static ULONG
Mm2BrbBucket(
    _In_ USHORT Type)
{
    if (Type == BRB_HCI_GET_LOCAL_BD_ADDR) {
        return 0;
    }
    if (Type >= BRB_L2CA_REGISTER_SERVER && Type <= BRB_UNREGISTER_PSM) {
        return 1 + (Type - BRB_L2CA_REGISTER_SERVER);
    }
    if (Type == BRB_L2CA_OPEN_ENHANCED_CHANNEL) {
        return 12;
    }
    if (Type == BRB_L2CA_OPEN_ENHANCED_CHANNEL_RESPONSE) {
        return 13;
    }
    if ((Type & 0xFF00) == 0x0300) {
        return 14;
    }
    return 15;
}

VOID
Mm2EvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode)
{
    PMM2_DEVICE_CONTEXT ctx = Mm2GetDeviceContext(WdfIoQueueGetDevice(Queue));

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    /* Observe only (e.g. hidbth's IOCTL_BTH_SDP_* descriptor query). */
    Mm2DiagCountCode(ctx, &ctx->Diag.DeviceIoctls, IoControlCode);
    Mm2ForwardAndForget(ctx, Request);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static PUCHAR
Mm2MapBuffer(
    _In_opt_ PVOID Buffer,
    _In_opt_ PMDL Mdl)
{
    if (Buffer != NULL) {
        return (PUCHAR)Buffer;
    }
    if (Mdl != NULL) {
        return (PUCHAR)MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority | MdlMappingNoExecute);
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Dispatch                                                                  */
/* ------------------------------------------------------------------------ */

VOID
Mm2EvtIoInternalDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode)
{
    PMM2_DEVICE_CONTEXT ctx = Mm2GetDeviceContext(WdfIoQueueGetDevice(Queue));
    WDF_REQUEST_PARAMETERS params;
    PBRB brb;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    Mm2DiagCountCode(ctx, &ctx->Diag.InternalIoctls, IoControlCode);

    if (IoControlCode != IOCTL_INTERNAL_BTH_SUBMIT_BRB) {
        Mm2ForwardAndForget(ctx, Request);
        return;
    }

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);
    brb = (PBRB)params.Parameters.Others.Arg1;
    if (brb == NULL) {
        Mm2ForwardAndForget(ctx, Request);
        return;
    }

    {
        ULONG bucket = Mm2BrbBucket(brb->BrbHeader.Type);
        InterlockedIncrement((volatile LONG *)&ctx->Diag.BrbTypes[bucket]);
        if (bucket == 15) {
            ctx->Diag.LastOtherBrbType = brb->BrbHeader.Type;
        }
        if (brb->BrbHeader.Type == BRB_L2CA_ACL_TRANSFER) {
            if (brb->BrbL2caAclTransfer.TransferFlags & ACL_TRANSFER_DIRECTION_IN) {
                InterlockedIncrement((volatile LONG *)&ctx->Diag.AclIn);
            } else {
                InterlockedIncrement((volatile LONG *)&ctx->Diag.AclOut);
            }
        }
    }

    switch (brb->BrbHeader.Type) {
    case BRB_L2CA_OPEN_CHANNEL:
    case BRB_L2CA_OPEN_CHANNEL_RESPONSE:
    case BRB_L2CA_OPEN_ENHANCED_CHANNEL:
    case BRB_L2CA_OPEN_ENHANCED_CHANNEL_RESPONSE:
        if (!Mm2ForwardWithCompletion(ctx, Request, Mm2OpenChannelCompletion)) {
            WdfRequestComplete(Request, WdfRequestGetStatus(Request));
        }
        return;

    case BRB_L2CA_CLOSE_CHANNEL:
        Mm2OnChannelClosed(ctx, brb->BrbL2caCloseChannel.ChannelHandle);
        break;

    case BRB_L2CA_ACL_TRANSFER: {
        struct _BRB_L2CA_ACL_TRANSFER *acl = &brb->BrbL2caAclTransfer;
        PMM2_REQUEST_CONTEXT rctx;

        if (!Mm2IsInterruptRead(ctx, acl)) {
            BOOLEAN isControl;

            WdfSpinLockAcquire(ctx->Lock);
            isControl = (ctx->ControlChannel != NULL && acl->ChannelHandle == ctx->ControlChannel);
            WdfSpinLockRelease(ctx->Lock);

            if (isControl && (acl->TransferFlags & ACL_TRANSFER_DIRECTION_IN)) {
                /* Observe hidbth's control read: the mouse's HANDSHAKE arrives here. */
                if (!Mm2ForwardWithCompletion(ctx, Request, Mm2ControlReadCompletion)) {
                    WdfRequestComplete(Request, WdfRequestGetStatus(Request));
                }
                return;
            }
            if (isControl) {
                PUCHAR out = Mm2MapBuffer(acl->Buffer, acl->BufferMDL);
                ULONG n = min(acl->BufferSize, (ULONG)sizeof(ctx->Diag.LastControlOut));

                InterlockedIncrement((volatile LONG *)&ctx->Diag.ControlOut);
                ctx->Diag.LastControlOutLength = acl->BufferSize;
                if (out != NULL) {
                    RtlCopyMemory(ctx->Diag.LastControlOut, out, n);
                }
            }
            break;
        }
        InterlockedIncrement((volatile LONG *)&ctx->Diag.AclInIntercepted);

        rctx = Mm2GetRequestContext(Request);
        rctx->Brb = brb;
        rctx->Bounced = FALSE;

        /*
         * hidbth sizes its read buffer for the descriptor's largest input
         * report (8 bytes), so a touch report would be cut off before we ever
         * see it. Read into our own buffer and copy back what hidbth expects.
         */
        if (acl->BufferSize < MM2_BOUNCE_BYTES) {
            rctx->Bounced    = TRUE;
            rctx->OrigBuffer = acl->Buffer;
            rctx->OrigMdl    = acl->BufferMDL;
            rctx->OrigSize   = acl->BufferSize;
            rctx->OrigFlags  = acl->TransferFlags;

            acl->Buffer        = rctx->Bounce;
            acl->BufferMDL     = NULL;
            acl->BufferSize    = MM2_BOUNCE_BYTES;
            acl->TransferFlags |= ACL_SHORT_TRANSFER_OK;
        }

        if (!Mm2ForwardWithCompletion(ctx, Request, Mm2AclReadCompletion)) {
            if (rctx->Bounced) {
                acl->Buffer        = rctx->OrigBuffer;
                acl->BufferMDL     = rctx->OrigMdl;
                acl->BufferSize    = rctx->OrigSize;
                acl->TransferFlags = rctx->OrigFlags;
            }
            WdfRequestComplete(Request, WdfRequestGetStatus(Request));
        }
        return;
    }

    default:
        break;
    }

    Mm2ForwardAndForget(ctx, Request);
}

/* ------------------------------------------------------------------------ */
/* Channel open                                                              */
/* ------------------------------------------------------------------------ */

static VOID
Mm2OpenChannelCompletion(
    _In_ WDFREQUEST                     Request,
    _In_ WDFIOTARGET                    Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT                     Context)
{
    PMM2_DEVICE_CONTEXT ctx = (PMM2_DEVICE_CONTEXT)Context;
    NTSTATUS status = Params->IoStatus.Status;
    WDF_REQUEST_PARAMETERS params;
    PBRB brb;
    BOOLEAN hold = FALSE;

    UNREFERENCED_PARAMETER(Target);

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);
    brb = (PBRB)params.Parameters.Others.Arg1;

    InterlockedIncrement((volatile LONG *)&ctx->Diag.OpenCompleted);
    ctx->Diag.LastOpenIrpStatus = status;
    if (brb != NULL) {
        ctx->Diag.LastOpenBrbStatus = brb->BrbHeader.Status;
        ctx->Diag.LastOpenType      = brb->BrbHeader.Type;
        ctx->Diag.LastOpenResponse  = brb->BrbL2caOpenChannel.Response;   /* union with Psm */
        ctx->Diag.LastOpenPsm       = brb->BrbL2caOpenChannel.Psm;
    }

    if (NT_SUCCESS(status) && brb != NULL && NT_SUCCESS(brb->BrbHeader.Status)) {
        L2CAP_CHANNEL_HANDLE handle;
        BTH_ADDR address;
        BOOLEAN psmKnown = FALSE;
        USHORT psm = 0;
        BOOLEAN opened = TRUE;

        /*
         * Psm and Response share a union. Measured on Windows 11 26300: after a
         * successful OPEN_CHANNEL_RESPONSE from hidbth the field holds the PSM
         * (0x11/0x13), not a CONNECT_RSP_RESULT_* code. So: a HID PSM value
         * identifies the channel, 0 means "accepted, PSM unknown" (fall back
         * to open order), and anything else is treated as a refusal.
         */
        USHORT field;

        switch (brb->BrbHeader.Type) {
        case BRB_L2CA_OPEN_CHANNEL:
        case BRB_L2CA_OPEN_CHANNEL_RESPONSE:
            handle  = brb->BrbL2caOpenChannel.ChannelHandle;
            address = brb->BrbL2caOpenChannel.BtAddress;
            field   = brb->BrbL2caOpenChannel.Psm;
            break;
        default: /* BRB_L2CA_OPEN_ENHANCED_CHANNEL[_RESPONSE] */
            handle  = brb->BrbL2caOpenEnhancedChannel.ChannelHandle;
            address = brb->BrbL2caOpenEnhancedChannel.BtAddress;
            field   = brb->BrbL2caOpenEnhancedChannel.Psm;
            break;
        }

        if (field == MM2_PSM_HID_CONTROL || field == MM2_PSM_HID_INTERRUPT) {
            psmKnown = TRUE;
            psm = field;
        } else if (brb->BrbHeader.Type == BRB_L2CA_OPEN_CHANNEL ||
                   brb->BrbHeader.Type == BRB_L2CA_OPEN_ENHANCED_CHANNEL) {
            psmKnown = TRUE;          /* outgoing open to some other PSM: ignored below */
            psm = field;
        } else if (field != CONNECT_RSP_RESULT_SUCCESS) {
            opened = FALSE;           /* hidbth refused the connection */
        }

        if (opened && handle != NULL) {
            InterlockedIncrement((volatile LONG *)&ctx->Diag.OpenAccepted);
        }
        if (opened && handle != NULL &&
            Mm2OnChannelOpened(ctx, handle, psmKnown, psm, address)) {
            /*
             * Interrupt channel is up. Keep hidbth waiting until multitouch is
             * on, so nothing else is using the control channel while we wait
             * for the mouse's HANDSHAKE.
             */
            WdfSpinLockAcquire(ctx->Lock);
            if (ctx->HeldOpenRequest == NULL) {
                ctx->HeldOpenRequest     = Request;
                ctx->HeldOpenStatus      = status;
                ctx->HeldOpenInformation = Params->IoStatus.Information;
                hold = TRUE;
            }
            WdfSpinLockRelease(ctx->Lock);
        }
    }

    if (hold) {
        WdfWorkItemEnqueue(ctx->EnableWorkItem);
        return;
    }

    WdfRequestCompleteWithInformation(Request, status, Params->IoStatus.Information);
}

VOID
Mm2EvtEnableWorkItem(
    _In_ WDFWORKITEM WorkItem)
{
    WDFDEVICE device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
    PMM2_DEVICE_CONTEXT ctx = Mm2GetDeviceContext(device);
    WDFREQUEST request;
    NTSTATUS status;
    ULONG_PTR information;

    (VOID)Mm2EnableMultitouch(device);

    WdfSpinLockAcquire(ctx->Lock);
    request     = ctx->HeldOpenRequest;
    status      = ctx->HeldOpenStatus;
    information = ctx->HeldOpenInformation;
    ctx->HeldOpenRequest = NULL;
    WdfSpinLockRelease(ctx->Lock);

    if (request != NULL) {
        WdfRequestCompleteWithInformation(request, status, information);
    }
}

VOID
Mm2EvtReenableWorkItem(
    _In_ WDFWORKITEM WorkItem)
{
    (VOID)Mm2EnableMultitouch((WDFDEVICE)WdfWorkItemGetParentObject(WorkItem));
}

/* ------------------------------------------------------------------------ */
/* Multitouch enable: SET_REPORT(Feature) F1 02 01 on the control channel    */
/* ------------------------------------------------------------------------ */

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS
Mm2SubmitBrbSync(
    _In_ PMM2_DEVICE_CONTEXT Ctx,
    _In_ PBRB Brb,
    _In_ LONG TimeoutMs)
{
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFREQUEST request;
    NTSTATUS status;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Ctx->Device;
    status = WdfRequestCreate(&attributes, Ctx->Target, &request);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Same pattern as the WDK bthecho sample: format, then set Argument1. */
    status = WdfIoTargetFormatRequestForInternalIoctlOthers(Ctx->Target, request,
                                                           IOCTL_INTERNAL_BTH_SUBMIT_BRB,
                                                           NULL, NULL, NULL, NULL, NULL, NULL);
    if (NT_SUCCESS(status)) {
        IoGetNextIrpStackLocation(WdfRequestWdmGetIrp(request))->Parameters.Others.Argument1 = Brb;

        WDF_REQUEST_SEND_OPTIONS_INIT(&options,
                                      WDF_REQUEST_SEND_OPTION_SYNCHRONOUS | WDF_REQUEST_SEND_OPTION_TIMEOUT);
        options.Timeout = WDF_REL_TIMEOUT_IN_MS(TimeoutMs);

        (VOID)WdfRequestSend(request, Ctx->Target, &options);
        status = WdfRequestGetStatus(request);
        if (NT_SUCCESS(status)) {
            status = Brb->BrbHeader.Status;
        }
    }

    WdfObjectDelete(request);
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
Mm2EnableMultitouch(
    _In_ WDFDEVICE Device)
{
    PMM2_DEVICE_CONTEXT ctx = Mm2GetDeviceContext(Device);
    struct _BRB_L2CA_ACL_TRANSFER *acl;
    L2CAP_CHANNEL_HANDLE control;
    BTH_ADDR address;
    NTSTATUS status;
    PBRB brb;

    /* Not pageable: takes ctx->Lock (spinlock). */
    if (!ctx->BthIfValid) {
        status = STATUS_DEVICE_NOT_READY;
        InterlockedExchange(&ctx->LastEnableStatus, status);
        return status;
    }

    WdfWaitLockAcquire(ctx->EnableLock, NULL);

    WdfSpinLockAcquire(ctx->Lock);
    control = ctx->ControlChannel;
    address = ctx->BtAddress;
    WdfSpinLockRelease(ctx->Lock);

    if (control == NULL) {
        status = STATUS_DEVICE_NOT_CONNECTED;
        goto exit;
    }

    brb = ctx->BthIf.BthAllocateBrb(BRB_L2CA_ACL_TRANSFER, MM2_POOL_TAG);
    if (brb == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }
    acl = &brb->BrbL2caAclTransfer;

    /* SET_REPORT | Feature, report F1, payload 02 01 (hid-magicmouse.c: feature_mt_mouse2). */
    ctx->ControlBuffer[0] = MM2_HIDP_SET_REPORT_FEATURE;
    ctx->ControlBuffer[1] = 0xF1;
    ctx->ControlBuffer[2] = 0x02;
    ctx->ControlBuffer[3] = 0x01;

    acl->BtAddress     = address;
    acl->ChannelHandle = control;
    acl->TransferFlags = ACL_TRANSFER_DIRECTION_OUT;
    acl->Buffer        = ctx->ControlBuffer;
    acl->BufferMDL     = NULL;
    acl->BufferSize    = 4;

    /*
     * hidbth keeps a read posted on the control channel, so the mouse's
     * one-byte HANDSHAKE reply goes to hidbth, not to us (measured: our own
     * read always timed out). We only send, and Mm2ControlReadCompletion
     * records the reply. "Enabled" is decided by touch frames actually arriving.
     */
    InterlockedIncrement((volatile LONG *)&ctx->Diag.EnableAttempts);
    InterlockedExchange64(&ctx->LastEnableTime, (LONG64)KeQueryInterruptTime());
    InterlockedExchange(&ctx->AwaitingHandshake, 1);

    status = Mm2SubmitBrbSync(ctx, brb, 2000);
    ctx->Diag.EnableWriteStatus = status;
    if (!NT_SUCCESS(status)) {
        InterlockedExchange(&ctx->AwaitingHandshake, 0);
    }

    ctx->BthIf.BthFreeBrb(brb);

exit:
    InterlockedExchange(&ctx->LastEnableStatus, status);
    WdfWaitLockRelease(ctx->EnableLock);
    return status;
}

/* ------------------------------------------------------------------------ */
/* Interrupt channel reads                                                   */
/* ------------------------------------------------------------------------ */

static VOID
Mm2AclReadCompletion(
    _In_ WDFREQUEST                     Request,
    _In_ WDFIOTARGET                    Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT                     Context)
{
    PMM2_DEVICE_CONTEXT ctx = (PMM2_DEVICE_CONTEXT)Context;
    PMM2_REQUEST_CONTEXT rctx = Mm2GetRequestContext(Request);
    struct _BRB_L2CA_ACL_TRANSFER *acl = &rctx->Brb->BrbL2caAclTransfer;
    NTSTATUS status = Params->IoStatus.Status;
    ULONG capacity, received, deliver;
    PUCHAR data;

    UNREFERENCED_PARAMETER(Target);

    if (rctx->Bounced) {
        data = rctx->Bounce;
        capacity = MM2_BOUNCE_BYTES;
    } else {
        data = Mm2MapBuffer(acl->Buffer, acl->BufferMDL);
        capacity = acl->BufferSize;
    }

    received = NT_SUCCESS(status) ? min(acl->BufferSize, capacity) : 0;
    deliver = received;

    /* Interrupt-channel payload: [0] = HIDP header (0xA1), [1..] = report. */
    if (data != NULL && received >= 2 &&
        data[0] == MM2_HIDP_DATA_INPUT && data[1] == MM2_REPORT_ID_TOUCH) {
        ULONG reportLength = received - 1;

        if (reportLength > MM2_NATIVE_REPORT_BYTES) {
            InterlockedIncrement(&ctx->TouchFrames);
            InterlockedExchange(&ctx->MultitouchEnabled, 1);
            if (ctx->Slot != MM2_INVALID_SLOT) {
                Mm2ControlPushFrame(ctx->Slot, data + 1, reportLength);
            }
            deliver = 1 + MM2_NATIVE_REPORT_BYTES;
        } else {
            LONG64 now = (LONG64)KeQueryInterruptTime();
            LONG64 last = ctx->LastEnableTime;

            InterlockedIncrement(&ctx->LegacyFrames);
            InterlockedExchange(&ctx->MultitouchEnabled, 0);

            /* Mouse is in plain mouse mode: ask it again, rate-limited. */
            if (now - last >= (LONG64)MM2_REENABLE_INTERVAL_MS * 10000 &&
                InterlockedCompareExchange64(&ctx->LastEnableTime, now, last) == last) {
                WdfWorkItemEnqueue(ctx->ReenableWorkItem);
            }
        }
    }

    if (rctx->Bounced) {
        ULONG copy = min(deliver, rctx->OrigSize);
        PUCHAR dst;

        acl->Buffer        = rctx->OrigBuffer;
        acl->BufferMDL     = rctx->OrigMdl;
        acl->TransferFlags = rctx->OrigFlags;

        if (copy > 0) {
            dst = Mm2MapBuffer(rctx->OrigBuffer, rctx->OrigMdl);
            if (dst != NULL) {
                RtlCopyMemory(dst, data, copy);
            } else {
                copy = 0;
                status = STATUS_INSUFFICIENT_RESOURCES;
            }
        }
        acl->BufferSize = copy;
        acl->RemainingBufferSize = NT_SUCCESS(status) ? (deliver - copy) : 0;
        rctx->Bounced = FALSE;
    } else if (NT_SUCCESS(status)) {
        acl->BufferSize = deliver;
    }

    WdfRequestCompleteWithInformation(Request, status, Params->IoStatus.Information);
}

/* ------------------------------------------------------------------------ */
/* Control channel reads (observe only)                                      */
/* ------------------------------------------------------------------------ */

static VOID
Mm2ControlReadCompletion(
    _In_ WDFREQUEST                     Request,
    _In_ WDFIOTARGET                    Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT                     Context)
{
    PMM2_DEVICE_CONTEXT ctx = (PMM2_DEVICE_CONTEXT)Context;
    NTSTATUS status = Params->IoStatus.Status;
    WDF_REQUEST_PARAMETERS params;
    PBRB brb;

    UNREFERENCED_PARAMETER(Target);

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);
    brb = (PBRB)params.Parameters.Others.Arg1;

    InterlockedIncrement((volatile LONG *)&ctx->Diag.ControlIn);
    ctx->Diag.LastControlInStatus = status;

    if (brb != NULL && NT_SUCCESS(status)) {
        struct _BRB_L2CA_ACL_TRANSFER *acl = &brb->BrbL2caAclTransfer;
        PUCHAR data = Mm2MapBuffer(acl->Buffer, acl->BufferMDL);
        ULONG n = min(acl->BufferSize, (ULONG)sizeof(ctx->Diag.LastControlIn));

        ctx->Diag.LastControlInLength = acl->BufferSize;
        if (data != NULL) {
            RtlCopyMemory(ctx->Diag.LastControlIn, data, n);
            if (InterlockedExchange(&ctx->AwaitingHandshake, 0) != 0) {
                RtlCopyMemory(ctx->Diag.ControlInAfterEnable, data, n);
                ctx->Diag.ControlInAfterEnableLength = acl->BufferSize;
            }
        }
    }

    WdfRequestCompleteWithInformation(Request, status, Params->IoStatus.Information);
}
