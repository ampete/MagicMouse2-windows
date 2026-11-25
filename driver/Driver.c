/*
 * Driver.c - DriverEntry and filter device setup.
 */
#include <ntddk.h>
#include <initguid.h>   /* instantiate GUID_BTHDDI_PROFILE_DRIVER_INTERFACE in this TU only */
#include "MM2Filter.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, Mm2EvtDeviceAdd)
#pragma alloc_text(PAGE, Mm2EvtDevicePrepareHardware)
#pragma alloc_text(PAGE, Mm2EvtDeviceReleaseHardware)
#endif

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDFDRIVER driver;
    NTSTATUS status;

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

    WDF_DRIVER_CONFIG_INIT(&config, Mm2EvtDeviceAdd);
    config.DriverPoolTag = MM2_POOL_TAG;

    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return Mm2ControlInitialize(driver);
}

NTSTATUS
Mm2EvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_WORKITEM_CONFIG workItemConfig;
    PMM2_DEVICE_CONTEXT ctx;
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    WdfFdoInitSetFilter(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = Mm2EvtDevicePrepareHardware;
    pnp.EvtDeviceReleaseHardware = Mm2EvtDeviceReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    /* Every request reaching us gets room for a bounce buffer. */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MM2_REQUEST_CONTEXT);
    WdfDeviceInitSetRequestAttributes(DeviceInit, &attributes);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MM2_DEVICE_CONTEXT);
    attributes.EvtCleanupCallback = Mm2EvtDeviceCleanup;

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ctx = Mm2GetDeviceContext(device);
    ctx->Device = device;
    ctx->Target = WdfDeviceGetIoTarget(device);
    ctx->Slot = MM2_INVALID_SLOT;
    ctx->LastEnableStatus = STATUS_PENDING;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &ctx->Lock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &ctx->EnableLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_WORKITEM_CONFIG_INIT(&workItemConfig, Mm2EvtEnableWorkItem);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWorkItemCreate(&workItemConfig, &attributes, &ctx->EnableWorkItem);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_WORKITEM_CONFIG_INIT(&workItemConfig, Mm2EvtReenableWorkItem);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWorkItemCreate(&workItemConfig, &attributes, &ctx->ReenableWorkItem);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /*
     * Only internal IOCTLs are inspected. A filter's default queue forwards
     * every other request type to the lower driver automatically. BRBs may be
     * submitted at DISPATCH_LEVEL, so the queue keeps the default execution level.
     */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoInternalDeviceControl = Mm2EvtIoInternalDeviceControl;
    queueConfig.EvtIoDeviceControl = Mm2EvtIoDeviceControl;   /* observe + forward */
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /*
     * A failure here only disables the sideband feed. The mouse itself must
     * keep working, so it is not fatal.
     */
    status = Mm2ControlAttach(device, &ctx->Slot);
    if (!NT_SUCCESS(status)) {
        ctx->Slot = MM2_INVALID_SLOT;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
Mm2EvtDevicePrepareHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PMM2_DEVICE_CONTEXT ctx = Mm2GetDeviceContext(Device);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    if (ctx->BthIfValid) {
        return STATUS_SUCCESS;
    }

    /* BRB allocation helpers, as used by the WDK bthecho sample. */
    status = WdfFdoQueryForInterface(Device,
                                     &GUID_BTHDDI_PROFILE_DRIVER_INTERFACE,
                                     (PINTERFACE)&ctx->BthIf,
                                     sizeof(ctx->BthIf),
                                     BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI,
                                     NULL);
    ctx->BthIfValid = NT_SUCCESS(status);

    /* Without it we cannot enable multitouch, but the mouse still works. */
    return STATUS_SUCCESS;
}

NTSTATUS
Mm2EvtDeviceReleaseHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PMM2_DEVICE_CONTEXT ctx = Mm2GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    if (ctx->BthIfValid) {
        ctx->BthIfValid = FALSE;
        if (ctx->BthIf.Interface.InterfaceDereference != NULL) {
            ctx->BthIf.Interface.InterfaceDereference(ctx->BthIf.Interface.Context);
        }
    }
    return STATUS_SUCCESS;
}

VOID
Mm2EvtDeviceCleanup(
    _In_ WDFOBJECT Object)
{
    PMM2_DEVICE_CONTEXT ctx = Mm2GetDeviceContext((WDFDEVICE)Object);

    if (ctx->Slot != MM2_INVALID_SLOT) {
        Mm2ControlDetach(ctx->Slot);
        ctx->Slot = MM2_INVALID_SLOT;
    }
}
