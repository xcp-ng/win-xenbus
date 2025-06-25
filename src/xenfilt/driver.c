/* Copyright (c) Xen Project.
 * Copyright (c) Cloud Software Group, Inc.
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
#include "mutex.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"
#include "version.h"

typedef struct _XENFILT_DRIVER {
    PDRIVER_OBJECT              DriverObject;

    MUTEX                       Mutex;
    LIST_ENTRY                  List;
    ULONG                       References;

    XENFILT_FILTER_STATE        FilterState;

    PXENFILT_EMULATED_CONTEXT   EmulatedContext;
    XENFILT_EMULATED_INTERFACE  EmulatedInterface;
} XENFILT_DRIVER, *PXENFILT_DRIVER;

static XENFILT_DRIVER   Driver;

#define XENFILT_DRIVER_TAG  'VIRD'

static FORCEINLINE PVOID
__DriverAllocate(
    _In_ ULONG  Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENFILT_DRIVER_TAG);
}

static FORCEINLINE VOID
__DriverFree(
    _In_ PVOID  Buffer
    )
{
    __FreePoolWithTag(Buffer, XENFILT_DRIVER_TAG);
}

extern PULONG   InitSafeBootMode;

static FORCEINLINE BOOLEAN
__DriverSafeMode(
    VOID
    )
{
    return (*InitSafeBootMode > 0) ? TRUE : FALSE;
}

static FORCEINLINE VOID
__DriverSetDriverObject(
    _In_opt_ PDRIVER_OBJECT DriverObject
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
__DriverSetEmulatedContext(
    _In_ PXENFILT_EMULATED_CONTEXT  Context
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
    _In_ PXENFILT_FDO   Fdo
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
    _In_ PXENFILT_FDO   Fdo
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

#define MAXNAMELEN  128

static FORCEINLINE NTSTATUS
__DriverGetActive(
    _In_ const CHAR *Key,
    _Out_ PCHAR     *Value
    )
{
    HANDLE          ParametersKey;
    CHAR            Name[MAXNAMELEN];
    PANSI_STRING    Ansi;
    ULONG           Length;
    NTSTATUS        status;

    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    status = RegistryOpenParametersKey(KEY_READ, &ParametersKey);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RtlStringCbPrintfA(Name, MAXNAMELEN, "Active%s", Key);
    ASSERT(NT_SUCCESS(status));

    status = RegistryQuerySzValue(ParametersKey,
                                  Name,
                                  NULL,
                                  &Ansi);
    if (!NT_SUCCESS(status))
        goto fail2;

    Length = Ansi[0].Length + sizeof (CHAR);
    *Value = __AllocatePoolWithTag(NonPagedPool, Length, 'TLIF');

    status = STATUS_NO_MEMORY;
    if (*Value == NULL)
        goto fail3;

    status = RtlStringCbPrintfA(*Value,
                                Length,
                                "%Z",
                                &Ansi[0]);
    ASSERT(NT_SUCCESS(status));

    RegistryFreeSzValue(Ansi);

    RegistryCloseKey(ParametersKey);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

    RegistryCloseKey(ParametersKey);

fail1:
    if (status != STATUS_OBJECT_NAME_NOT_FOUND)
        Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
DriverGetActive(
    _In_ const CHAR *Key,
    _Out_ PCHAR     *Value
    )
{
    return __DriverGetActive(Key, Value);
}

static BOOLEAN
DriverIsActivePresent(
    VOID
    )
{
    PCHAR       ActiveDeviceID;
    BOOLEAN     Present;
    NTSTATUS    status;

    status = XENFILT_EMULATED(Acquire, &Driver.EmulatedInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    Present = FALSE;

    status = __DriverGetActive("DeviceID",
                               &ActiveDeviceID);
    if (!NT_SUCCESS(status))
        goto done;

    Present = XENFILT_EMULATED(IsDevicePresent,
                               &Driver.EmulatedInterface,
                               ActiveDeviceID,
                               NULL);

    ExFreePool(ActiveDeviceID);

done:
    XENFILT_EMULATED(Release, &Driver.EmulatedInterface);

    Info("ACTIVE DEVICE %sPRESENT\n", (!Present) ? "NOT " : "");

    return Present;

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
            if (!__DriverSafeMode())
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
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    ASSERT3P(DriverObject, ==, __DriverGetDriverObject());

    Trace("====>\n");

    ASSERT(IsListEmpty(&Driver.List));
    ASSERT3U(Driver.References, ==, 1);
    --Driver.References;

    RtlZeroMemory(&Driver.List, sizeof (LIST_ENTRY));
    RtlZeroMemory(&Driver.Mutex, sizeof (MUTEX));

    RtlZeroMemory(&Driver.EmulatedInterface,
                  sizeof (XENFILT_EMULATED_INTERFACE));

    EmulatedTeardown(Driver.EmulatedContext);
    Driver.EmulatedContext = NULL;

    RegistryTeardown();

    Info("XENFILT %d.%d.%d (%d) (%02d.%02d.%04d)\n",
         MAJOR_VERSION,
         MINOR_VERSION,
         MICRO_VERSION,
         BUILD_NUMBER,
         DAY,
         MONTH,
         YEAR);

    __DriverSetDriverObject(NULL);

    ASSERT(IsZeroMemory(&Driver, sizeof (XENFILT_DRIVER)));

    Trace("<====\n");
}

static IO_COMPLETION_ROUTINE DriverQueryCompletion;

_Use_decl_annotations_
static NTSTATUS
DriverQueryCompletion(
    PDEVICE_OBJECT      DeviceObject,
    PIRP                Irp,
    PVOID               Context
    )
{
    PKEVENT             Event = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
DriverQueryId(
    _In_ PDEVICE_OBJECT     DeviceObject,
    _In_ BUS_QUERY_ID_TYPE  Type,
    _Out_ PCHAR             *Id
    )
{
    PIRP                    Irp;
    KEVENT                  Event;
    PIO_STACK_LOCATION      StackLocation;
    PWCHAR                  Buffer;
    NTSTATUS                status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    switch (Type) {
    case BusQueryDeviceID:
    case BusQueryInstanceID:
    case BusQueryHardwareIDs:
    case BusQueryCompatibleIDs:
        status = STATUS_SUCCESS;
        break;

    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    if (!NT_SUCCESS(status))
        goto fail1;

    ObReferenceObject(DeviceObject);

    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);

    status = STATUS_INSUFFICIENT_RESOURCES;
    if (Irp == NULL)
        goto fail2;

    StackLocation = IoGetNextIrpStackLocation(Irp);

    StackLocation->MajorFunction = IRP_MJ_PNP;
    StackLocation->MinorFunction = IRP_MN_QUERY_ID;
    StackLocation->Flags = 0;
    StackLocation->Parameters.QueryId.IdType = Type;
    StackLocation->DeviceObject = DeviceObject;
    StackLocation->FileObject = NULL;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    IoSetCompletionRoutine(Irp,
                           DriverQueryCompletion,
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
        goto fail3;

    Buffer = (PWCHAR)Irp->IoStatus.Information;

    switch (Type) {
    case BusQueryDeviceID:
    case BusQueryInstanceID: {
        ULONG   Length;
        ULONG   Size;

        Length = (ULONG)(wcslen(Buffer));
        Size = (Length + 1) * sizeof (CHAR);

        *Id = __AllocatePoolWithTag(PagedPool, Size, 'TLIF');
        if (*Id == NULL)
            break;

        status = RtlStringCbPrintfA(*Id, Size, "%ws", Buffer);
        ASSERT(NT_SUCCESS(status));

        break;
    }
    case BusQueryHardwareIDs:
    case BusQueryCompatibleIDs: {
        ULONG   Index;
        ULONG   Size;

        Index = 0;
        for (;;) {
            ULONG   Length;

            Length = (ULONG)wcslen(&Buffer[Index]);
            if (Length == 0)
                break;

            Index += Length + 1;
        }

        Size = (Index + 1) * sizeof (CHAR);

        *Id = __AllocatePoolWithTag(PagedPool, Size, 'TLIF');
        if (*Id == NULL)
            break;

        Index = 0;
        for (;;) {
            ULONG   Length;

            Length = (ULONG)wcslen(&Buffer[Index]);
            if (Length == 0)
                break;

            status = RtlStringCbPrintfA(*Id + Index, Size, "%ws",
                                        &Buffer[Index]);
            ASSERT(NT_SUCCESS(status));

            Index += Length + 1;
            Size -= Length + 1;
        }

        break;
    }
    default:
        ASSERT(FALSE);
        *Id = NULL;
        break;
    }

    status = STATUS_NO_MEMORY;
    if (*Id == NULL)
        goto fail4;

    ExFreePool(Buffer);
    IoFreeIrp(Irp);
    ObDereferenceObject(DeviceObject);

    return STATUS_SUCCESS;

fail4:
    ExFreePool(Buffer);

fail3:
    IoFreeIrp(Irp);

fail2:
    ObDereferenceObject(DeviceObject);

fail1:
    return status;
}

NTSTATUS
DriverQueryDeviceText(
    _In_ PDEVICE_OBJECT     DeviceObject,
    _In_ DEVICE_TEXT_TYPE   Type,
    _Out_ PCHAR             *Text
    )
{
    PIRP                    Irp;
    KEVENT                  Event;
    PIO_STACK_LOCATION      StackLocation;
    PWCHAR                  Buffer;
    ULONG                   Length;
    NTSTATUS                status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    ObReferenceObject(DeviceObject);

    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);

    status = STATUS_INSUFFICIENT_RESOURCES;
    if (Irp == NULL)
        goto fail1;

    StackLocation = IoGetNextIrpStackLocation(Irp);

    StackLocation->MajorFunction = IRP_MJ_PNP;
    StackLocation->MinorFunction = IRP_MN_QUERY_DEVICE_TEXT;
    StackLocation->Flags = 0;
    StackLocation->Parameters.QueryDeviceText.DeviceTextType = Type;
    StackLocation->DeviceObject = DeviceObject;
    StackLocation->FileObject = NULL;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    IoSetCompletionRoutine(Irp,
                           DriverQueryCompletion,
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

    Buffer = (PWCHAR)Irp->IoStatus.Information;
    Length = (ULONG)(wcslen(Buffer) + 1) * sizeof (CHAR);

    *Text = __AllocatePoolWithTag(PagedPool, Length, 'TLIF');

    status = STATUS_NO_MEMORY;
    if (*Text == NULL)
        goto fail3;

    status = RtlStringCbPrintfA(*Text, Length, "%ws", Buffer);
    ASSERT(NT_SUCCESS(status));

    ExFreePool(Buffer);
    IoFreeIrp(Irp);
    ObDereferenceObject(DeviceObject);

    return STATUS_SUCCESS;

fail3:
    ExFreePool(Buffer);

fail2:
    IoFreeIrp(Irp);

fail1:
    ObDereferenceObject(DeviceObject);

    return status;
}

static FORCEINLINE PCHAR
__EmulatedTypeName(
    _In_ XENFILT_EMULATED_OBJECT_TYPE   Type
    )
{
    switch (Type) {
    case XENFILT_EMULATED_OBJECT_TYPE_UNKNOWN:  return "UNKNOWN";
    case XENFILT_EMULATED_OBJECT_TYPE_PCI:      return "PCI";
    case XENFILT_EMULATED_OBJECT_TYPE_IDE:      return "IDE";
    default:                                    return "InvalidType";
    }
}

static XENFILT_EMULATED_OBJECT_TYPE
DriverGetEmulatedType(
    _In_ PCHAR                      Id
    )
{
    HANDLE                          ParametersKey;
    XENFILT_EMULATED_OBJECT_TYPE    Type;
    ULONG                           Index;
    NTSTATUS                        status;

    status = RegistryOpenParametersKey(KEY_READ, &ParametersKey);
    if (!NT_SUCCESS(status))
        goto fail1;

    Type = XENFILT_EMULATED_OBJECT_TYPE_UNKNOWN;
    Index = 0;

    do {
        ULONG           Length;
        PANSI_STRING    Ansi;

        Length = (ULONG)strlen(&Id[Index]);
        if (Length == 0)
            break;

        status = RegistryQuerySzValue(ParametersKey,
                                      &Id[Index],
                                      NULL,
                                      &Ansi);
        if (NT_SUCCESS(status)) {
            Info("MATCH: %s -> %Z\n", &Id[Index], Ansi);

            if (_strnicmp(Ansi->Buffer, "PCI", Ansi->Length) == 0)
                Type = XENFILT_EMULATED_OBJECT_TYPE_PCI;
            else if (_strnicmp(Ansi->Buffer, "IDE", Ansi->Length) == 0)
                Type = XENFILT_EMULATED_OBJECT_TYPE_IDE;

            RegistryFreeSzValue(Ansi);
        } else {
            Trace("NO MATCH: %s\n", &Id[Index]);
        }

        Index += Length + 1;
    } while (Type == XENFILT_EMULATED_OBJECT_TYPE_UNKNOWN);

    RegistryCloseKey(ParametersKey);

    return Type;

fail1:
    Error("fail1 %08x\n", status);

    return XENFILT_EMULATED_OBJECT_TYPE_UNKNOWN;
}

DRIVER_ADD_DEVICE   DriverAddDevice;

NTSTATUS
#pragma prefast(suppress:28152) // Does not clear DO_DEVICE_INITIALIZING
DriverAddDevice(
    _In_ PDRIVER_OBJECT             DriverObject,
    _In_ PDEVICE_OBJECT             PhysicalDeviceObject
    )
{
    PCHAR                           Id;
    XENFILT_EMULATED_OBJECT_TYPE    Type;
    NTSTATUS                        status;

    ASSERT3P(DriverObject, ==, __DriverGetDriverObject());

    Type = XENFILT_EMULATED_OBJECT_TYPE_UNKNOWN;

    status = DriverQueryId(PhysicalDeviceObject,
                           BusQueryHardwareIDs,
                           &Id);
    if (NT_SUCCESS(status)) {
        Type = DriverGetEmulatedType(Id);
        ExFreePool(Id);
    }

    if (Type == XENFILT_EMULATED_OBJECT_TYPE_UNKNOWN) {
        status = DriverQueryId(PhysicalDeviceObject,
                               BusQueryCompatibleIDs,
                               &Id);
        if (NT_SUCCESS(status)) {
            Type = DriverGetEmulatedType(Id);
            ExFreePool(Id);
        }
    }

    Info("%p %s\n",
         PhysicalDeviceObject,
         __EmulatedTypeName(Type));

    status = STATUS_SUCCESS;
    if (Type == XENFILT_EMULATED_OBJECT_TYPE_UNKNOWN)
        goto done;

    __DriverAcquireMutex();
    status = FdoCreate(PhysicalDeviceObject, Type);
    __DriverReleaseMutex();

done:
    return status;
}

DRIVER_DISPATCH DriverDispatch;

_Use_decl_annotations_
NTSTATUS
DriverDispatch(
    PDEVICE_OBJECT      DeviceObject,
    PIRP                Irp
    )
{
    PXENFILT_DX         Dx;
    NTSTATUS            status;

    Dx = (PXENFILT_DX)DeviceObject->DeviceExtension;
    ASSERT3P(Dx->DeviceObject, ==, DeviceObject);

    if (Dx->DevicePnpState == Deleted) {
        PIO_STACK_LOCATION  StackLocation = IoGetCurrentIrpStackLocation(Irp);
        UCHAR               MajorFunction = StackLocation->MajorFunction;
        UCHAR               MinorFunction = StackLocation->MinorFunction;

        status = STATUS_NO_SUCH_DEVICE;

        if (MajorFunction == IRP_MJ_PNP) {
            /* FDO and PDO deletions can block after being marked deleted, but before IoDeleteDevice */
            if (MinorFunction == IRP_MN_SURPRISE_REMOVAL || MinorFunction == IRP_MN_REMOVE_DEVICE)
                status = STATUS_SUCCESS;

            ASSERT((MinorFunction != IRP_MN_CANCEL_REMOVE_DEVICE) && (MinorFunction != IRP_MN_CANCEL_STOP_DEVICE));
        }

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
    _In_ PDRIVER_OBJECT         DriverObject,
    _In_ PUNICODE_STRING        RegistryPath
    )
{
    PXENFILT_EMULATED_CONTEXT   EmulatedContext;
    ULONG                       Index;
    NTSTATUS                    status;

    ASSERT3P(__DriverGetDriverObject(), ==, NULL);

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
    WdmlibProcgrpInitialize();

    __DbgPrintEnable();

    Trace("====>\n");

    __DriverSetDriverObject(DriverObject);

    DriverObject->DriverUnload = DriverUnload;

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

    status = RegistryInitialize(DriverObject, RegistryPath);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = EmulatedInitialize(&EmulatedContext);
    if (!NT_SUCCESS(status))
        goto fail2;

    __DriverSetEmulatedContext(EmulatedContext);

    status = EmulatedGetInterface(__DriverGetEmulatedContext(),
                                  XENFILT_EMULATED_INTERFACE_VERSION_MAX,
                                  (PINTERFACE)&Driver.EmulatedInterface,
                                  sizeof (Driver.EmulatedInterface));
    ASSERT(NT_SUCCESS(status));

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

fail2:
    Error("fail2\n");

    RegistryTeardown();

fail1:
    Error("fail1 (%08x)\n", status);

    __DriverSetDriverObject(NULL);

    ASSERT(IsZeroMemory(&Driver, sizeof (XENFILT_DRIVER)));

    return status;
}
