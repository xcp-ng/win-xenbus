/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, 
 * with or without modification, are permitted provided 
 * that the following conditions are met:
 * 
 * *   Redistributions of source code must retain the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer in the documentation and/or other 
 *     materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 */

#include <ntddk.h>
#include <ntstrsafe.h>
#include <procgrp.h>
#include <xen.h>

#include "registry.h"
#include "fdo.h"
#include "pdo.h"
#include "driver.h"
#include "emulated.h"
#include "pvdevice.h"
#include "mutex.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"
#include "version.h"

extern PULONG       InitSafeBootMode;

typedef struct _XENFILT_DRIVER {
    PDRIVER_OBJECT              DriverObject;
    HANDLE                      ParametersKey;

    MUTEX                       Mutex;
    LIST_ENTRY                  List;
    ULONG                       References;

    XENFILT_FILTER_STATE        FilterState;

    PXENFILT_EMULATED_CONTEXT   EmulatedContext;
    XENFILT_EMULATED_INTERFACE  EmulatedInterface;

    PXENFILT_PVDEVICE_CONTEXT   PvdeviceContext;
    XENFILT_PVDEVICE_INTERFACE  PvdeviceInterface;
} XENFILT_DRIVER, *PXENFILT_DRIVER;

static XENFILT_DRIVER   Driver;

#define XENFILT_DRIVER_TAG  'VIRD'

static FORCEINLINE PVOID
__DriverAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENFILT_DRIVER_TAG);
}

static FORCEINLINE VOID
__DriverFree(
    IN  PVOID   Buffer
    )
{
    ExFreePoolWithTag(Buffer, XENFILT_DRIVER_TAG);
}

static FORCEINLINE VOID
__DriverSetDriverObject(
    IN  PDRIVER_OBJECT  DriverObject
    )
{
    Driver.DriverObject = DriverObject;
}

static FORCEINLINE PDRIVER_OBJECT
__DriverGetDriverObject(
    VOID
    )
{
    return Driver.DriverObject;
}

PDRIVER_OBJECT
DriverGetDriverObject(
    VOID
    )
{
    return __DriverGetDriverObject();
}

static FORCEINLINE VOID
__DriverSetParametersKey(
    IN  HANDLE  Key
    )
{
    Driver.ParametersKey = Key;
}

static FORCEINLINE HANDLE
__DriverGetParametersKey(
    VOID
    )
{
    return Driver.ParametersKey;
}

HANDLE
DriverGetParametersKey(
    VOID
    )
{
    return __DriverGetParametersKey();
}

static FORCEINLINE VOID
__DriverSetEmulatedContext(
    IN  PXENFILT_EMULATED_CONTEXT   Context
    )
{
    Driver.EmulatedContext = Context;
}

static FORCEINLINE PXENFILT_EMULATED_CONTEXT
__DriverGetEmulatedContext(
    VOID
    )
{
    return Driver.EmulatedContext;
}

PXENFILT_EMULATED_CONTEXT
DriverGetEmulatedContext(
    VOID
    )
{
    return __DriverGetEmulatedContext();
}

static FORCEINLINE VOID
__DriverSetPvdeviceContext(
    IN  PXENFILT_PVDEVICE_CONTEXT   Context
    )
{
    Driver.PvdeviceContext = Context;
}

static FORCEINLINE PXENFILT_PVDEVICE_CONTEXT
__DriverGetPvdeviceContext(
    VOID
    )
{
    return Driver.PvdeviceContext;
}

PXENFILT_PVDEVICE_CONTEXT
DriverGetPvdeviceContext(
    VOID
    )
{
    return __DriverGetPvdeviceContext();
}

static FORCEINLINE VOID
__DriverAcquireMutex(
    VOID
    )
{
    AcquireMutex(&Driver.Mutex);
}

VOID
DriverAcquireMutex(
    VOID
    )
{
    __DriverAcquireMutex();
}

static FORCEINLINE VOID
__DriverReleaseMutex(
    VOID
    )
{
    ReleaseMutex(&Driver.Mutex);
}

VOID
DriverReleaseMutex(
    VOID
    )
{
    __DriverReleaseMutex();
}

VOID
DriverAddFunctionDeviceObject(
    IN  PXENFILT_FDO    Fdo
    )
{
    PDEVICE_OBJECT      DeviceObject;
    PXENFILT_DX         Dx;

    DeviceObject = FdoGetDeviceObject(Fdo);
    Dx = (PXENFILT_DX)DeviceObject->DeviceExtension;
    ASSERT3U(Dx->Type, ==, FUNCTION_DEVICE_OBJECT);

    InsertTailList(&Driver.List, &Dx->ListEntry);
    Driver.References++;
}

VOID
DriverRemoveFunctionDeviceObject(
    IN  PXENFILT_FDO    Fdo
    )
{
    PDEVICE_OBJECT      DeviceObject;
    PXENFILT_DX         Dx;

    DeviceObject = FdoGetDeviceObject(Fdo);
    Dx = (PXENFILT_DX)DeviceObject->DeviceExtension;
    ASSERT3U(Dx->Type, ==, FUNCTION_DEVICE_OBJECT);

    RemoveEntryList(&Dx->ListEntry);
    ASSERT3U(Driver.References, !=, 0);
    --Driver.References;
}

static BOOLEAN
DriverIsActivePresent(
    VOID
    )
{
    CHAR        ActiveDeviceID[MAX_DEVICE_ID_LEN];
    CHAR        ActiveInstanceID[MAX_DEVICE_ID_LEN];
    BOOLEAN     Present;
    NTSTATUS    status;

    status = XENFILT_PVDEVICE(Acquire, &Driver.PvdeviceInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = XENFILT_EMULATED(Acquire, &Driver.EmulatedInterface);
    if (!NT_SUCCESS(status))
        goto fail2;

    Present = FALSE;

    status = XENFILT_PVDEVICE(GetActive,
                              &Driver.PvdeviceInterface,
                              ActiveDeviceID,
                              ActiveInstanceID);
    if (!NT_SUCCESS(status))
        goto done;

    Present = XENFILT_EMULATED(IsDevicePresent,
                               &Driver.EmulatedInterface,
                               ActiveDeviceID,
                               NULL);

done:
    XENFILT_EMULATED(Release, &Driver.EmulatedInterface);
    XENFILT_PVDEVICE(Release, &Driver.PvdeviceInterface);

    return Present;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return FALSE;
}

VOID
DriverSetFilterState(
    VOID
    )
{
    __DriverAcquireMutex();

    switch (Driver.FilterState) {
    case XENFILT_FILTER_ENABLED: {
        PLIST_ENTRY ListEntry;

        // Assume all FDOs have enumerated until we know otherwise
        Driver.FilterState = XENFILT_FILTER_PENDING;

        for (ListEntry = Driver.List.Flink;
             ListEntry != &Driver.List;
             ListEntry = ListEntry->Flink) {
            PXENFILT_DX     Dx = CONTAINING_RECORD(ListEntry, XENFILT_DX, ListEntry);
            PXENFILT_FDO    Fdo = Dx->Fdo;

            ASSERT3U(Dx->Type, ==, FUNCTION_DEVICE_OBJECT);

            if (!FdoHasEnumerated(Fdo))
                Driver.FilterState = XENFILT_FILTER_ENABLED;
        }

        if (Driver.FilterState != XENFILT_FILTER_PENDING)
            break;

        if (DriverIsActivePresent()) {
            Info("ACTIVE DEVICE %sPRESENT\n", (!Present) ? "NOT " : "");

            UnplugDevices();
        }

        Info("PENDING\n");
        break;
    }
    case XENFILT_FILTER_PENDING:
        Driver.FilterState = XENFILT_FILTER_DISABLED;

        Info("DISABLED\n");
        break;

    case XENFILT_FILTER_DISABLED:
        break;

    default:
        ASSERT(FALSE);
        break;
    }

    __DriverReleaseMutex();
}

XENFILT_FILTER_STATE
DriverGetFilterState(
    VOID
    )
{
    XENFILT_FILTER_STATE    State;

    __DriverAcquireMutex();
    State = Driver.FilterState;
    __DriverReleaseMutex();

    return State;
}

DRIVER_UNLOAD   DriverUnload;

VOID
DriverUnload(
    IN  PDRIVER_OBJECT  DriverObject
    )
{
    HANDLE              ParametersKey;

    ASSERT3P(DriverObject, ==, __DriverGetDriverObject());

    Trace("====>\n");

    if (*InitSafeBootMode > 0)
        goto done;

    ASSERT(IsListEmpty(&Driver.List));
    ASSERT3U(Driver.References, ==, 1);
    --Driver.References;

    RtlZeroMemory(&Driver.List, sizeof (LIST_ENTRY));
    RtlZeroMemory(&Driver.Mutex, sizeof (MUTEX));

    RtlZeroMemory(&Driver.PvdeviceInterface,
                  sizeof (XENFILT_PVDEVICE_INTERFACE));

    RtlZeroMemory(&Driver.EmulatedInterface,
                  sizeof (XENFILT_EMULATED_INTERFACE));

    PvdeviceTeardown(Driver.PvdeviceContext);
    Driver.PvdeviceContext = NULL;

    EmulatedTeardown(Driver.EmulatedContext);
    Driver.EmulatedContext = NULL;

    ParametersKey = __DriverGetParametersKey();
    __DriverSetParametersKey(NULL);
    RegistryCloseKey(ParametersKey);

    RegistryTeardown();

    Info("XENFILT %d.%d.%d (%d) (%02d.%02d.%04d)\n",
         MAJOR_VERSION,
         MINOR_VERSION,
         MICRO_VERSION,
         BUILD_NUMBER,
         DAY,
         MONTH,
         YEAR);

done:
    __DriverSetDriverObject(NULL);

    ASSERT(IsZeroMemory(&Driver, sizeof (XENFILT_DRIVER)));

    Trace("<====\n");
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
DriverQueryIdCompletion(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PKEVENT             Event = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static FORCEINLINE NTSTATUS
DriverQueryId(
    IN  PDEVICE_OBJECT      PhysicalDeviceObject,
    IN  BUS_QUERY_ID_TYPE   Type,
    OUT PCHAR               Id
    )
{
    PDEVICE_OBJECT          DeviceObject;
    PIRP                    Irp;
    KEVENT                  Event;
    PIO_STACK_LOCATION      StackLocation;
    NTSTATUS                status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    DeviceObject = IoGetAttachedDeviceReference(PhysicalDeviceObject);

    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);

    status = STATUS_INSUFFICIENT_RESOURCES;
    if (Irp == NULL)
        goto fail1;

    StackLocation = IoGetNextIrpStackLocation(Irp);

    StackLocation->MajorFunction = IRP_MJ_PNP;
    StackLocation->MinorFunction = IRP_MN_QUERY_ID;
    StackLocation->Flags = 0;
    StackLocation->Parameters.QueryId.IdType = Type;
    StackLocation->DeviceObject = DeviceObject;
    StackLocation->FileObject = NULL;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    IoSetCompletionRoutine(Irp,
                           DriverQueryIdCompletion,
                           &Event,
                           TRUE,
                           TRUE,
                           TRUE);

    // Default completion status
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    status = IoCallDriver(DeviceObject, Irp);
    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject(&Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        status = Irp->IoStatus.Status;
    } else {
        ASSERT3U(status, ==, Irp->IoStatus.Status);
    }

    if (!NT_SUCCESS(status))
        goto fail2;

    status = RtlStringCbPrintfA(Id,
                                MAX_DEVICE_ID_LEN,
                                "%ws",
                                (PWCHAR)Irp->IoStatus.Information);
    ASSERT(NT_SUCCESS(status));

    ExFreePool((PVOID)Irp->IoStatus.Information);

    IoFreeIrp(Irp);
    ObDereferenceObject(DeviceObject);

    return STATUS_SUCCESS;

fail2:
    IoFreeIrp(Irp);

fail1:
    ObDereferenceObject(DeviceObject);

    return status;
}

static XENFILT_EMULATED_OBJECT_TYPE
DriverGetEmulatedType(
    IN  PANSI_STRING                Ansi
    )
{
    XENFILT_EMULATED_OBJECT_TYPE    Type;

    if (_strnicmp(Ansi->Buffer, "DEVICE", Ansi->Length) == 0)
        Type = XENFILT_EMULATED_OBJECT_TYPE_DEVICE;
    else if (_strnicmp(Ansi->Buffer, "DISK", Ansi->Length) == 0)
        Type = XENFILT_EMULATED_OBJECT_TYPE_DISK;
    else
        Type = XENFILT_EMULATED_OBJECT_TYPE_INVALID;

    return Type;
}

DRIVER_ADD_DEVICE   DriverAddDevice;

NTSTATUS
#pragma prefast(suppress:28152) // Does not clear DO_DEVICE_INITIALIZING
DriverAddDevice(
    IN  PDRIVER_OBJECT  DriverObject,
    IN  PDEVICE_OBJECT  PhysicalDeviceObject
    )
{
    HANDLE              ParametersKey;
    CHAR                DeviceID[MAX_DEVICE_ID_LEN];
    CHAR                InstanceID[MAX_DEVICE_ID_LEN];
    PANSI_STRING        Type;
    NTSTATUS            status;

    ASSERT3P(DriverObject, ==, __DriverGetDriverObject());

    ParametersKey = __DriverGetParametersKey();

    status = DriverQueryId(PhysicalDeviceObject,
                           BusQueryDeviceID,
                           DeviceID);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = DriverQueryId(PhysicalDeviceObject,
                           BusQueryInstanceID,
                           InstanceID);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = RegistryQuerySzValue(ParametersKey,
                                  DeviceID,
                                  NULL,
                                  &Type);
    if (NT_SUCCESS(status)) {
        __DriverAcquireMutex();

        status = FdoCreate(PhysicalDeviceObject,
                           DeviceID,
                           InstanceID,
                           DriverGetEmulatedType(Type));

        if (!NT_SUCCESS(status))
            goto fail3;

        __DriverReleaseMutex();

        RegistryFreeSzValue(Type);
    }

    return STATUS_SUCCESS;

fail3:
        __DriverReleaseMutex();

fail2:
fail1:
    return status;
}

DRIVER_DISPATCH DriverDispatch;

NTSTATUS 
DriverDispatch(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
    )
{
    PXENFILT_DX         Dx;
    NTSTATUS            status;

    Dx = (PXENFILT_DX)DeviceObject->DeviceExtension;
    ASSERT3P(Dx->DeviceObject, ==, DeviceObject);

    if (Dx->DevicePnpState == Deleted) {
        status = STATUS_NO_SUCH_DEVICE;

        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        goto done;
    }

    status = STATUS_NOT_SUPPORTED;
    switch (Dx->Type) {
    case PHYSICAL_DEVICE_OBJECT: {
        PXENFILT_PDO    Pdo = Dx->Pdo;

        status = PdoDispatch(Pdo, Irp);
        break;
    }
    case FUNCTION_DEVICE_OBJECT: {
        PXENFILT_FDO    Fdo = Dx->Fdo;

        status = FdoDispatch(Fdo, Irp);
        break;
    }
    default:
        ASSERT(FALSE);
        break;
    }

done:
    return status;
}

DRIVER_INITIALIZE   DriverEntry;

NTSTATUS
DriverEntry(
    IN  PDRIVER_OBJECT          DriverObject,
    IN  PUNICODE_STRING         RegistryPath
    )
{
    HANDLE                      ServiceKey;
    HANDLE                      ParametersKey;
    PXENFILT_EMULATED_CONTEXT   EmulatedContext;
    PXENFILT_PVDEVICE_CONTEXT   PvdeviceContext;
    ULONG                       Index;
    NTSTATUS                    status;

    ASSERT3P(__DriverGetDriverObject(), ==, NULL);

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
    WdmlibProcgrpInitialize();

    __DbgPrintEnable();

    Trace("====>\n");

    __DriverSetDriverObject(DriverObject);

    DriverObject->DriverUnload = DriverUnload;

    if (*InitSafeBootMode > 0)
        goto done;

    Info("%d.%d.%d (%d) (%02d.%02d.%04d)\n",
         MAJOR_VERSION,
         MINOR_VERSION,
         MICRO_VERSION,
         BUILD_NUMBER,
         DAY,
         MONTH,
         YEAR);

    status = XenTouch(__MODULE__,
                      MAJOR_VERSION,
                      MINOR_VERSION,
                      MICRO_VERSION,
                      BUILD_NUMBER);
    if (!NT_SUCCESS(status))
        goto done;

    status = RegistryInitialize(RegistryPath);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RegistryOpenServiceKey(KEY_READ, &ServiceKey);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = RegistryOpenSubKey(ServiceKey, "Parameters", KEY_READ, &ParametersKey);
    if (!NT_SUCCESS(status))
        goto fail3;

    __DriverSetParametersKey(ParametersKey);

    status = EmulatedInitialize(&EmulatedContext);
    if (!NT_SUCCESS(status))
        goto fail4;

    __DriverSetEmulatedContext(EmulatedContext);

    status = PvdeviceInitialize(&PvdeviceContext);
    if (!NT_SUCCESS(status))
        goto fail5;

    __DriverSetPvdeviceContext(PvdeviceContext);

    status = EmulatedGetInterface(__DriverGetEmulatedContext(),
                                  XENFILT_EMULATED_INTERFACE_VERSION_MAX,
                                  (PINTERFACE)&Driver.EmulatedInterface,
                                  sizeof (Driver.EmulatedInterface));
    ASSERT(NT_SUCCESS(status));

    status = PvdeviceGetInterface(__DriverGetPvdeviceContext(),
                                  XENFILT_PVDEVICE_INTERFACE_VERSION_MAX,
                                  (PINTERFACE)&Driver.PvdeviceInterface,
                                  sizeof (Driver.PvdeviceInterface));
    ASSERT(NT_SUCCESS(status));

    RegistryCloseKey(ServiceKey);

    DriverObject->DriverExtension->AddDevice = DriverAddDevice;

    for (Index = 0; Index <= IRP_MJ_MAXIMUM_FUNCTION; Index++) {
#pragma prefast(suppress:28169) // No __drv_dispatchType annotation
#pragma prefast(suppress:28168) // No matching __drv_dispatchType annotation for IRP_MJ_CREATE
        DriverObject->MajorFunction[Index] = DriverDispatch;
    }

    InitializeMutex(&Driver.Mutex);
    InitializeListHead(&Driver.List);
    Driver.References = 1;

done:
    Trace("<====\n");
    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");

    EmulatedTeardown(Driver.EmulatedContext);
    Driver.EmulatedContext = NULL;

fail4:
    Error("fail4\n");

    __DriverSetParametersKey(NULL);
    RegistryCloseKey(ParametersKey);

fail3:
    Error("fail3\n");

    RegistryCloseKey(ServiceKey);

fail2:
    Error("fail2\n");

    RegistryTeardown();

fail1:
    Error("fail1 (%08x)\n", status);

    __DriverSetDriverObject(NULL);

    ASSERT(IsZeroMemory(&Driver, sizeof (XENFILT_DRIVER)));

    return status;
}
