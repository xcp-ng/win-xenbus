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

#define INITGUID 1

#include <ntddk.h>
#include <wdmguid.h>
#include <ntstrsafe.h>
#include <stdlib.h>

#include "emulated.h"
#include "names.h"
#include "fdo.h"
#include "pdo.h"
#include "thread.h"
#include "driver.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

#define PDO_TAG 'ODP'

#define MAXNAMELEN  128

struct _XENFILT_PDO {
    PXENFILT_DX                     Dx;
    PDEVICE_OBJECT                  LowerDeviceObject;
    PDEVICE_OBJECT                  PhysicalDeviceObject;
    CHAR                            Name[MAXNAMELEN];

    PXENFILT_FDO                    Fdo;
    BOOLEAN                         Missing;
    PCSTR                           Reason;

    XENFILT_EMULATED_OBJECT_TYPE    Type;
    PXENFILT_EMULATED_OBJECT        EmulatedObject;
    BOOLEAN                         Active;
};

static FORCEINLINE PVOID
__PdoAllocate(
    _In_ ULONG  Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, PDO_TAG);
}

static FORCEINLINE VOID
__PdoFree(
    _In_ PVOID  Buffer
    )
{
    __FreePoolWithTag(Buffer, PDO_TAG);
}

static FORCEINLINE VOID
__PdoSetDevicePnpState(
    _In_ PXENFILT_PDO       Pdo,
    _In_ DEVICE_PNP_STATE   State
    )
{
    PXENFILT_DX             Dx = Pdo->Dx;

    // We can never transition out of the deleted state
    ASSERT(Dx->DevicePnpState != Deleted || State == Deleted);

    Dx->PreviousDevicePnpState = Dx->DevicePnpState;
    Dx->DevicePnpState = State;
}

VOID
PdoSetDevicePnpState(
    _In_ PXENFILT_PDO       Pdo,
    _In_ DEVICE_PNP_STATE   State
    )
{
    __PdoSetDevicePnpState(Pdo, State);
}

static FORCEINLINE VOID
__PdoRestoreDevicePnpState(
    _In_ PXENFILT_PDO       Pdo,
    _In_ DEVICE_PNP_STATE   State
    )
{
    PXENFILT_DX             Dx = Pdo->Dx;

    if (Dx->DevicePnpState == State)
        Dx->DevicePnpState = Dx->PreviousDevicePnpState;
}

static FORCEINLINE DEVICE_PNP_STATE
__PdoGetDevicePnpState(
    _In_ PXENFILT_PDO   Pdo
    )
{
    PXENFILT_DX         Dx = Pdo->Dx;

    return Dx->DevicePnpState;
}

DEVICE_PNP_STATE
PdoGetDevicePnpState(
    _In_ PXENFILT_PDO   Pdo
    )
{
    return __PdoGetDevicePnpState(Pdo);
}

static FORCEINLINE VOID
__PdoSetDevicePowerState(
    _In_ PXENFILT_PDO       Pdo,
    _In_ DEVICE_POWER_STATE State
    )
{
    PXENFILT_DX             Dx = Pdo->Dx;

    Dx->DevicePowerState = State;
}

static FORCEINLINE DEVICE_POWER_STATE
__PdoGetDevicePowerState(
    _In_ PXENFILT_PDO   Pdo
    )
{
    PXENFILT_DX         Dx = Pdo->Dx;

    return Dx->DevicePowerState;
}

static FORCEINLINE VOID
__PdoSetSystemPowerState(
    _In_ PXENFILT_PDO       Pdo,
    _In_ SYSTEM_POWER_STATE State
    )
{
    PXENFILT_DX             Dx = Pdo->Dx;

    Dx->SystemPowerState = State;
}

static FORCEINLINE SYSTEM_POWER_STATE
__PdoGetSystemPowerState(
    _In_ PXENFILT_PDO   Pdo
    )
{
    PXENFILT_DX         Dx = Pdo->Dx;

    return Dx->SystemPowerState;
}

PDEVICE_OBJECT
PdoGetPhysicalDeviceObject(
    _In_ PXENFILT_PDO   Pdo
    )
{
    return Pdo->PhysicalDeviceObject;
}

static FORCEINLINE VOID
__PdoSetMissing(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PCSTR          Reason
    )
{
    Pdo->Reason = Reason;
    Pdo->Missing = TRUE;
}

VOID
PdoSetMissing(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PCSTR          Reason
    )
{
    __PdoSetMissing(Pdo, Reason);
}

static FORCEINLINE BOOLEAN
__PdoIsMissing(
    _In_ PXENFILT_PDO   Pdo
    )
{
    return Pdo->Missing;
}

BOOLEAN
PdoIsMissing(
    _In_ PXENFILT_PDO   Pdo
    )
{
    return __PdoIsMissing(Pdo);
}

static FORCEINLINE PDEVICE_OBJECT
__PdoGetDeviceObject(
    _In_ PXENFILT_PDO   Pdo
    )
{
    PXENFILT_DX         Dx = Pdo->Dx;

    return Dx->DeviceObject;
}

PDEVICE_OBJECT
PdoGetDeviceObject(
    _In_ PXENFILT_PDO   Pdo
    )
{
    return __PdoGetDeviceObject(Pdo);
}

static FORCEINLINE PXENFILT_FDO
__PdoGetFdo(
    _In_ PXENFILT_PDO   Pdo
    )
{
    return Pdo->Fdo;
}

static NTSTATUS
PdoSetDeviceInformation(
    _In_ PXENFILT_PDO   Pdo
    )
{
    PXENFILT_DX         Dx = Pdo->Dx;
    PSTR                DeviceID;
    PSTR                ActiveDeviceID;
    PSTR                InstanceID;
    PSTR                LocationInformation;
    NTSTATUS            status;

    status = DriverQueryId(Pdo->LowerDeviceObject,
                           BusQueryDeviceID,
                           &DeviceID);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = DriverGetActive("DeviceID",
                             &ActiveDeviceID);
    if (NT_SUCCESS(status)) {
        Pdo->Active = (_stricmp(DeviceID, ActiveDeviceID) == 0) ?
                      TRUE :
                      FALSE;

        ExFreePool(ActiveDeviceID);
    } else {
        Pdo->Active = FALSE;
    }

    if (Pdo->Active) {
        status = DriverGetActive("InstanceID",
                                 &InstanceID);
        if (!NT_SUCCESS(status))
            goto fail2;

        status = DriverGetActive("LocationInformation",
                                 &LocationInformation);
        if (!NT_SUCCESS(status)) {
            status = DriverQueryDeviceText(Pdo->LowerDeviceObject,
                                           DeviceTextLocationInformation,
                                           &LocationInformation);
            if (!NT_SUCCESS(status))
                LocationInformation = NULL;
        }
    } else {
        status = DriverQueryId(Pdo->LowerDeviceObject,
                               BusQueryInstanceID,
                               &InstanceID);
        if (!NT_SUCCESS(status))
            InstanceID = NULL;

        status = DriverQueryDeviceText(Pdo->LowerDeviceObject,
                                       DeviceTextLocationInformation,
                                       &LocationInformation);
        if (!NT_SUCCESS(status))
            LocationInformation = NULL;
    }

    Dx->DeviceID = DeviceID;
    Dx->InstanceID = InstanceID;
    Dx->LocationInformation = LocationInformation;

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    ASSERT(Pdo->Active);
    ExFreePool(DeviceID);

    Pdo->Active = FALSE;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
PdoClearDeviceInformation(
    _In_ PXENFILT_PDO   Pdo
    )
{
    PXENFILT_DX         Dx = Pdo->Dx;

    if (Dx->LocationInformation != NULL) {
        ExFreePool(Dx->LocationInformation);
        Dx->LocationInformation = NULL;
    }

    if (Dx->InstanceID != NULL) {
        ExFreePool(Dx->InstanceID);
        Dx->InstanceID = NULL;
    }

    ASSERT(Dx->DeviceID != NULL);
    ExFreePool(Dx->DeviceID);
    Dx->DeviceID = NULL;

    Pdo->Active = FALSE;
}

static FORCEINLINE PSTR
__PdoGetDeviceID(
    _In_ PXENFILT_PDO   Pdo
    )
{
    PXENFILT_DX         Dx = Pdo->Dx;

    ASSERT(Dx->DeviceID != NULL);
    return Dx->DeviceID;
}

static FORCEINLINE PSTR
__PdoGetInstanceID(
    _In_ PXENFILT_PDO   Pdo
    )
{
    PXENFILT_DX         Dx = Pdo->Dx;

    return (Dx->InstanceID != NULL) ?
           Dx->InstanceID : "";
}

static FORCEINLINE XENFILT_EMULATED_OBJECT_TYPE
__PdoGetType(
    _In_ PXENFILT_PDO   Pdo
    )
{
    return Pdo->Type;
}

static FORCEINLINE PSTR
__PdoGetLocationInformation(
    _In_ PXENFILT_PDO   Pdo
    )
{
    PXENFILT_DX         Dx = Pdo->Dx;

    return (Dx->LocationInformation != NULL) ?
           Dx->LocationInformation : "";
}

static FORCEINLINE VOID
__PdoSetName(
    _In_ PXENFILT_PDO   Pdo
    )
{
    NTSTATUS            status;

    if (strlen(__PdoGetInstanceID(Pdo)) == 0)
        status = RtlStringCbPrintfA(Pdo->Name,
                                    MAXNAMELEN,
                                    "%s",
                                    __PdoGetDeviceID(Pdo));
    else
        status = RtlStringCbPrintfA(Pdo->Name,
                                    MAXNAMELEN,
                                    "%s\\%s",
                                    __PdoGetDeviceID(Pdo),
                                    __PdoGetInstanceID(Pdo));

    ASSERT(NT_SUCCESS(status));
}

static FORCEINLINE PSTR
__PdoGetName(
    _In_ PXENFILT_PDO   Pdo
    )
{
    return Pdo->Name;
}

static IO_COMPLETION_ROUTINE PdoForwardIrpSynchronouslyCompletion;

_Use_decl_annotations_
static NTSTATUS
PdoForwardIrpSynchronouslyCompletion(
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

static NTSTATUS
PdoForwardIrpSynchronously(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    KEVENT              Event;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           PdoForwardIrpSynchronouslyCompletion,
                           &Event,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Pdo->LowerDeviceObject, Irp);
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

    return status;
}

static NTSTATUS
PdoStartDevice(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    POWER_STATE         PowerState;
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = PdoForwardIrpSynchronously(Pdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail2;

    PowerState.DeviceState = PowerDeviceD0;
    PoSetPowerState(__PdoGetDeviceObject(Pdo),
                    DevicePowerState,
                    PowerState);

    __PdoSetDevicePowerState(Pdo, PowerDeviceD0);

    __PdoSetDevicePnpState(Pdo, Started);

    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;

fail2:
    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoQueryStopDevice(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    __PdoSetDevicePnpState(Pdo, StopPending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);

    status = IoCallDriver(Pdo->LowerDeviceObject, Irp);
    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoCancelStopDevice(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    Irp->IoStatus.Status = STATUS_SUCCESS;

    __PdoRestoreDevicePnpState(Pdo, StopPending);

    IoSkipCurrentIrpStackLocation(Irp);

    status = IoCallDriver(Pdo->LowerDeviceObject, Irp);
    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoStopDevice(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    POWER_STATE         PowerState;
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    if (__PdoGetDevicePowerState(Pdo) != PowerDeviceD0)
        goto done;

    PowerState.DeviceState = PowerDeviceD3;
    PoSetPowerState(__PdoGetDeviceObject(Pdo),
                    DevicePowerState,
                    PowerState);

    __PdoSetDevicePowerState(Pdo, PowerDeviceD3);

done:
    __PdoSetDevicePnpState(Pdo, Stopped);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);

    status = IoCallDriver(Pdo->LowerDeviceObject, Irp);
    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoQueryRemoveDevice(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    __PdoSetDevicePnpState(Pdo, RemovePending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);

    status = IoCallDriver(Pdo->LowerDeviceObject, Irp);
    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoCancelRemoveDevice(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    __PdoRestoreDevicePnpState(Pdo, RemovePending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);

    status = IoCallDriver(Pdo->LowerDeviceObject, Irp);
    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoSurpriseRemoval(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    __PdoSetDevicePnpState(Pdo, SurpriseRemovePending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);

    status = IoCallDriver(Pdo->LowerDeviceObject, Irp);
    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoRemoveDevice(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    PXENFILT_FDO        Fdo = __PdoGetFdo(Pdo);
    POWER_STATE         PowerState;
    BOOLEAN             NeedInvalidate;
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    if (__PdoGetDevicePowerState(Pdo) != PowerDeviceD0)
        goto done;

    PowerState.DeviceState = PowerDeviceD3;
    PoSetPowerState(__PdoGetDeviceObject(Pdo),
                    DevicePowerState,
                    PowerState);

    __PdoSetDevicePowerState(Pdo, PowerDeviceD3);

done:
    status = PdoForwardIrpSynchronously(Pdo, Irp);

    FdoAcquireMutex(Fdo);

    NeedInvalidate = FALSE;

    if (__PdoIsMissing(Pdo)) {
        DEVICE_PNP_STATE    State = __PdoGetDevicePnpState(Pdo);

        __PdoSetDevicePnpState(Pdo, Deleted);
        IoReleaseRemoveLockAndWait(&Pdo->Dx->RemoveLock, Irp);

        if (State == SurpriseRemovePending)
            PdoDestroy(Pdo);
        else
            NeedInvalidate = TRUE;
    } else {
        __PdoSetDevicePnpState(Pdo, Enumerated);
        IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    }

    FdoReleaseMutex(Fdo);

    if (NeedInvalidate)
        IoInvalidateDeviceRelations(FdoGetPhysicalDeviceObject(Fdo),
                                    BusRelations);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

#define DEFINE_PDO_QUERY_INTERFACE(_Interface)                      \
static NTSTATUS                                                     \
PdoQuery ## _Interface ## Interface(                                \
    _In_ PXENFILT_PDO   Pdo,                                        \
    _In_ PIRP           Irp                                         \
    )                                                               \
{                                                                   \
    PIO_STACK_LOCATION  StackLocation;                              \
    USHORT              Size;                                       \
    USHORT              Version;                                    \
    PINTERFACE          Interface;                                  \
    PVOID               Context;                                    \
    NTSTATUS            status;                                     \
                                                                    \
    UNREFERENCED_PARAMETER(Pdo);                                    \
                                                                    \
    status = Irp->IoStatus.Status;                                  \
                                                                    \
    StackLocation = IoGetCurrentIrpStackLocation(Irp);              \
    Size = StackLocation->Parameters.QueryInterface.Size;           \
    Version = StackLocation->Parameters.QueryInterface.Version;     \
    Interface = StackLocation->Parameters.QueryInterface.Interface; \
                                                                    \
    Context = DriverGet ## _Interface ## Context();                 \
                                                                    \
    status = _Interface ## GetInterface(Context,                    \
                                        Version,                    \
                                        Interface,                  \
                                        Size);                      \
    if (!NT_SUCCESS(status))                                        \
        goto done;                                                  \
                                                                    \
    Irp->IoStatus.Information = 0;                                  \
    status = STATUS_SUCCESS;                                        \
                                                                    \
done:                                                               \
    return status;                                                  \
}                                                                   \

DEFINE_PDO_QUERY_INTERFACE(Emulated)

struct _INTERFACE_ENTRY {
    const GUID  *Guid;
    PCSTR       Name;
    NTSTATUS    (*Query)(PXENFILT_PDO, PIRP);
};

#define DEFINE_INTERFACE_ENTRY(_Guid, _Interface)   \
    { &GUID_XENFILT_ ## _Guid, #_Guid, PdoQuery ## _Interface ## Interface }

struct _INTERFACE_ENTRY PdoInterfaceTable[] = {
    DEFINE_INTERFACE_ENTRY(EMULATED_INTERFACE, Emulated),
    { NULL, NULL, NULL }
};

static NTSTATUS
PdoQueryInterface(
    _In_ PXENFILT_PDO       Pdo,
    _In_ PIRP               Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    const GUID              *InterfaceType;
    struct _INTERFACE_ENTRY *Entry;
    USHORT                  Version;
    NTSTATUS                status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    if (Irp->IoStatus.Status != STATUS_NOT_SUPPORTED)
        goto done;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    InterfaceType = StackLocation->Parameters.QueryInterface.InterfaceType;
    Version = StackLocation->Parameters.QueryInterface.Version;

    for (Entry = PdoInterfaceTable; Entry->Guid != NULL; Entry++) {
        if (IsEqualGUID(InterfaceType, Entry->Guid)) {
            Info("%s: %s (VERSION %d)\n",
                 __PdoGetName(Pdo),
                 Entry->Name,
                 Version);
            Irp->IoStatus.Status = Entry->Query(Pdo, Irp);
            goto done;
        }
    }

done:
    IoSkipCurrentIrpStackLocation(Irp);

    status = IoCallDriver(Pdo->LowerDeviceObject, Irp);
    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoQueryDeviceText(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UNICODE_STRING      Text;
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = PdoForwardIrpSynchronously(Pdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail2;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    RtlZeroMemory(&Text, sizeof (UNICODE_STRING));

    switch (StackLocation->Parameters.QueryDeviceText.DeviceTextType) {
    case DeviceTextLocationInformation:
        Text.MaximumLength =
            (USHORT)(strlen(__PdoGetLocationInformation(Pdo)) *
                     sizeof (WCHAR));

        Trace("DeviceTextLocationInformation\n");
        break;

    default:
        goto done;
    }

    status = STATUS_OBJECT_NAME_NOT_FOUND;
    if (Text.MaximumLength == 0)
        goto fail3;

    Text.MaximumLength += sizeof (WCHAR);
    Text.Buffer = __AllocatePoolWithTag(PagedPool,
                                        Text.MaximumLength,
                                        'TLIF');

    status = STATUS_NO_MEMORY;
    if (Text.Buffer == NULL)
        goto fail4;

    switch (StackLocation->Parameters.QueryDeviceText.DeviceTextType) {
    case DeviceTextLocationInformation:
        status = RtlStringCbPrintfW(Text.Buffer,
                                    Text.MaximumLength,
                                    L"%hs",
                                    __PdoGetLocationInformation(Pdo));
        ASSERT(NT_SUCCESS(status));

        break;

    default:
        ASSERT(FALSE);
        break;
    }

    Text.Length = (USHORT)(wcslen(Text.Buffer) * sizeof (WCHAR));

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    Trace("- %wZ\n", &Text);

    ExFreePool((PVOID)Irp->IoStatus.Information);
    Irp->IoStatus.Information = (ULONG_PTR)Text.Buffer;

done:
    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;

fail4:
fail3:
fail2:
    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoQueryId(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UNICODE_STRING      Id;
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = PdoForwardIrpSynchronously(Pdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail2;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    RtlZeroMemory(&Id, sizeof (UNICODE_STRING));

    switch (StackLocation->Parameters.QueryId.IdType) {
    case BusQueryInstanceID:
        Id.MaximumLength = (USHORT)(strlen(__PdoGetInstanceID(Pdo)) *
                                    sizeof (WCHAR));

        Trace("BusQueryInstanceID\n");
        break;

    case BusQueryDeviceID:
        Id.MaximumLength = (USHORT)(strlen(__PdoGetDeviceID(Pdo)) *
                                    sizeof (WCHAR));

        Trace("BusQueryDeviceID\n");
        break;

    default:
        goto done;
    }

    status = STATUS_OBJECT_NAME_NOT_FOUND;
    if (Id.MaximumLength == 0)
        goto fail3;

    Id.MaximumLength += sizeof (WCHAR);
    Id.Buffer = __AllocatePoolWithTag(PagedPool, Id.MaximumLength, 'TLIF');

    status = STATUS_NO_MEMORY;
    if (Id.Buffer == NULL)
        goto fail4;

    switch (StackLocation->Parameters.QueryId.IdType) {
    case BusQueryInstanceID:
        status = RtlStringCbPrintfW(Id.Buffer,
                                    Id.MaximumLength,
                                    L"%hs",
                                    __PdoGetInstanceID(Pdo));
        ASSERT(NT_SUCCESS(status));

        break;

    case BusQueryDeviceID:
        status = RtlStringCbPrintfW(Id.Buffer,
                                    Id.MaximumLength,
                                    L"%hs",
                                    __PdoGetDeviceID(Pdo));
        ASSERT(NT_SUCCESS(status));

        break;

    default:
        ASSERT(FALSE);
        break;
    }

    Id.Length = (USHORT)(wcslen(Id.Buffer) * sizeof (WCHAR));

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    Trace("- %wZ\n", &Id);

    ExFreePool((PVOID)Irp->IoStatus.Information);
    Irp->IoStatus.Information = (ULONG_PTR)Id.Buffer;

done:
    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;

fail4:
fail3:
fail2:
    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoEject(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    PXENFILT_FDO        Fdo = __PdoGetFdo(Pdo);
    NTSTATUS            status;

    FdoAcquireMutex(Fdo);
    __PdoSetMissing(Pdo, "Ejected");
    __PdoSetDevicePnpState(Pdo, Deleted);
    FdoReleaseMutex(Fdo);

    status = PdoForwardIrpSynchronously(Pdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    FdoAcquireMutex(Fdo);
    PdoDestroy(Pdo);
    FdoReleaseMutex(Fdo);

    return status;
}

static NTSTATUS
PdoDispatchPnp(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

    switch (StackLocation->MinorFunction) {
    case IRP_MN_START_DEVICE:
        status = PdoStartDevice(Pdo, Irp);
        break;

    case IRP_MN_QUERY_STOP_DEVICE:
        status = PdoQueryStopDevice(Pdo, Irp);
        break;

    case IRP_MN_CANCEL_STOP_DEVICE:
        status = PdoCancelStopDevice(Pdo, Irp);
        break;

    case IRP_MN_STOP_DEVICE:
        status = PdoStopDevice(Pdo, Irp);
        break;

    case IRP_MN_QUERY_REMOVE_DEVICE:
        status = PdoQueryRemoveDevice(Pdo, Irp);
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        status = PdoSurpriseRemoval(Pdo, Irp);
        break;

    case IRP_MN_REMOVE_DEVICE:
        status = PdoRemoveDevice(Pdo, Irp);
        break;

    case IRP_MN_CANCEL_REMOVE_DEVICE:
        status = PdoCancelRemoveDevice(Pdo, Irp);
        break;

    case IRP_MN_QUERY_INTERFACE:
        status = PdoQueryInterface(Pdo, Irp);
        break;

    case IRP_MN_QUERY_DEVICE_TEXT:
        status = PdoQueryDeviceText(Pdo, Irp);
        break;

    case IRP_MN_QUERY_ID:
        status = PdoQueryId(Pdo, Irp);
        break;

    case IRP_MN_EJECT:
        status = PdoEject(Pdo, Irp);
        break;

    default:
        status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
        if (!NT_SUCCESS(status))
            goto fail1;

        IoSkipCurrentIrpStackLocation(Irp);

        status = IoCallDriver(Pdo->LowerDeviceObject, Irp);
        IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);
        break;
    }

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static IO_COMPLETION_ROUTINE PdoSetDevicePowerUpComplete;

_Use_decl_annotations_
static NTSTATUS
PdoSetDevicePowerUpComplete(
    PDEVICE_OBJECT      DeviceObject,
    PIRP                Irp,
    PVOID               Context
    )
{
    PXENFILT_PDO        Pdo = (PXENFILT_PDO)Context;
    PIO_STACK_LOCATION  StackLocation;
    POWER_STATE         PowerState;

    UNREFERENCED_PARAMETER(DeviceObject);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    PowerState = StackLocation->Parameters.Power.State;

    ASSERT3U(PowerState.DeviceState, <,  __PdoGetDevicePowerState(Pdo));

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    Trace("%s: %s -> %s\n",
          __PdoGetName(Pdo),
          DevicePowerStateName(__PdoGetDevicePowerState(Pdo)),
          DevicePowerStateName(PowerState.DeviceState));

    PoSetPowerState(__PdoGetDeviceObject(Pdo),
                    DevicePowerState,
                    PowerState);

    __PdoSetDevicePowerState(Pdo, PowerState.DeviceState);

    return STATUS_CONTINUE_COMPLETION;
}

static FORCEINLINE NTSTATUS
__PdoSetDevicePowerUp(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           PdoSetDevicePowerUpComplete,
                           Pdo,
                           TRUE,
                           TRUE,
                           TRUE);
    return IoCallDriver(Pdo->LowerDeviceObject, Irp);
}

static NTSTATUS
__PdoSetDevicePowerDown(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    POWER_STATE         PowerState;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    PowerState = StackLocation->Parameters.Power.State;

    ASSERT3U(PowerState.DeviceState, >,  __PdoGetDevicePowerState(Pdo));

    Trace("%s: %s -> %s\n",
          __PdoGetName(Pdo),
          DevicePowerStateName(__PdoGetDevicePowerState(Pdo)),
          DevicePowerStateName(PowerState.DeviceState));

    PoSetPowerState(__PdoGetDeviceObject(Pdo),
                    DevicePowerState,
                    PowerState);

    __PdoSetDevicePowerState(Pdo, PowerState.DeviceState);

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Pdo->LowerDeviceObject, Irp);
}

static NTSTATUS
PdoSetDevicePower(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    Trace("%s: ====> (%s:%s)\n",
          __PdoGetName(Pdo),
          DevicePowerStateName(DeviceState),
          PowerActionName(PowerAction));

    if (DeviceState == __PdoGetDevicePowerState(Pdo)) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Pdo->LowerDeviceObject, Irp);

        goto done;
    }

    status = (DeviceState < __PdoGetDevicePowerState(Pdo)) ?
             __PdoSetDevicePowerUp(Pdo, Irp) :
             __PdoSetDevicePowerDown(Pdo, Irp);

done:
    Trace("%s: <==== (%s:%s)(%08x)\n",
          __PdoGetName(Pdo),
          DevicePowerStateName(DeviceState),
          PowerActionName(PowerAction),
          status);
    return status;
}

static FORCEINLINE NTSTATUS
__PdoDispatchDevicePower(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

    switch (MinorFunction) {
    case IRP_MN_SET_POWER:
        status = PdoSetDevicePower(Pdo, Irp);
        break;

    default:
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Pdo->LowerDeviceObject, Irp);
        break;
    }

    return status;
}

static IO_COMPLETION_ROUTINE PdoSetSystemPowerUpComplete;

_Use_decl_annotations_
static NTSTATUS
PdoSetSystemPowerUpComplete(
    PDEVICE_OBJECT      DeviceObject,
    PIRP                Irp,
    PVOID               Context
    )
{
    PXENFILT_PDO        Pdo = (PXENFILT_PDO)Context;
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;

    UNREFERENCED_PARAMETER(DeviceObject);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, <,  __PdoGetSystemPowerState(Pdo));

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    Trace("%s: %s -> %s\n",
          __PdoGetName(Pdo),
          SystemPowerStateName(__PdoGetSystemPowerState(Pdo)),
          SystemPowerStateName(SystemState));

    __PdoSetSystemPowerState(Pdo, SystemState);

    return STATUS_CONTINUE_COMPLETION;
}

static FORCEINLINE NTSTATUS
__PdoSetSystemPowerUp(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           PdoSetSystemPowerUpComplete,
                           Pdo,
                           TRUE,
                           TRUE,
                           TRUE);
    return IoCallDriver(Pdo->LowerDeviceObject, Irp);
}

static FORCEINLINE NTSTATUS
__PdoSetSystemPowerDown(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, >,  __PdoGetSystemPowerState(Pdo));

    Trace("%s: %s -> %s\n",
          __PdoGetName(Pdo),
          SystemPowerStateName(__PdoGetSystemPowerState(Pdo)),
          SystemPowerStateName(SystemState));

    __PdoSetSystemPowerState(Pdo, SystemState);

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Pdo->LowerDeviceObject, Irp);
}

static NTSTATUS
PdoSetSystemPower(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    Trace("%s: ====> (%s:%s)\n",
          __PdoGetName(Pdo),
          SystemPowerStateName(SystemState),
          PowerActionName(PowerAction));

    if (SystemState == __PdoGetSystemPowerState(Pdo)) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Pdo->LowerDeviceObject, Irp);

        goto done;
    }

    status = (SystemState < __PdoGetSystemPowerState(Pdo)) ?
             __PdoSetSystemPowerUp(Pdo, Irp) :
             __PdoSetSystemPowerDown(Pdo, Irp);

done:
    Trace("%s: <==== (%s:%s)(%08x)\n",
          __PdoGetName(Pdo),
          SystemPowerStateName(SystemState),
          PowerActionName(PowerAction),
          status);
    return status;
}

static FORCEINLINE NTSTATUS
__PdoDispatchSystemPower(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

    switch (MinorFunction) {
    case IRP_MN_SET_POWER:
        status = PdoSetSystemPower(Pdo, Irp);
        break;

    default:
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Pdo->LowerDeviceObject, Irp);
        break;
    }

    return status;
}

static NTSTATUS
PdoDispatchPower(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    POWER_STATE_TYPE    PowerType;
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

    if (MinorFunction != IRP_MN_SET_POWER) {
        IoSkipCurrentIrpStackLocation(Irp);

        status = IoCallDriver(Pdo->LowerDeviceObject, Irp);
        IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

        goto done;
    }

    PowerType = StackLocation->Parameters.Power.Type;

    Trace("%s: ====> (%02x:%s)\n",
          __PdoGetName(Pdo),
          MinorFunction,
          PowerMinorFunctionName(MinorFunction));

    switch (PowerType) {
    case DevicePowerState:
        status = __PdoDispatchDevicePower(Pdo, Irp);
        IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);
        break;

    case SystemPowerState:
        status = __PdoDispatchSystemPower(Pdo, Irp);
        IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);
        break;

    default:
        IoSkipCurrentIrpStackLocation(Irp);

        status = IoCallDriver(Pdo->LowerDeviceObject, Irp);
        IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);
        break;
    }

    Trace("%s: <==== (%02x:%s) (%08x)\n",
          __PdoGetName(Pdo),
          MinorFunction,
          PowerMinorFunctionName(MinorFunction),
          status);

done:
    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoDispatchDefault(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    IoSkipCurrentIrpStackLocation(Irp);

    status = IoCallDriver(Pdo->LowerDeviceObject, Irp);
    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

NTSTATUS
PdoDispatch(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->MajorFunction) {
    case IRP_MJ_PNP:
        status = PdoDispatchPnp(Pdo, Irp);
        break;

    case IRP_MJ_POWER:
        status = PdoDispatchPower(Pdo, Irp);
        break;

    default:
        status = PdoDispatchDefault(Pdo, Irp);
        break;
    }

    return status;
}

VOID
PdoResume(
    _In_ PXENFILT_PDO   Pdo
    )
{
    UNREFERENCED_PARAMETER(Pdo);
}

VOID
PdoSuspend(
    _In_ PXENFILT_PDO   Pdo
    )
{
    UNREFERENCED_PARAMETER(Pdo);
}

NTSTATUS
PdoCreate(
    _In_ PXENFILT_FDO                   Fdo,
    _In_ PDEVICE_OBJECT                 PhysicalDeviceObject,
    _In_ XENFILT_EMULATED_OBJECT_TYPE   Type
    )
{
    PDEVICE_OBJECT                      LowerDeviceObject;
    ULONG                               DeviceType;
    PDEVICE_OBJECT                      FilterDeviceObject;
    PXENFILT_DX                         Dx;
    PXENFILT_PDO                        Pdo;
    PSTR                                CompatibleIDs;
    NTSTATUS                            status;

    ASSERT(Type != XENFILT_EMULATED_OBJECT_TYPE_UNKNOWN);

    LowerDeviceObject = IoGetAttachedDeviceReference(PhysicalDeviceObject);
    DeviceType = LowerDeviceObject->DeviceType;
    ObDereferenceObject(LowerDeviceObject);

#pragma prefast(suppress:28197) // Possibly leaking memory 'PhysicalDeviceObject'
    status = IoCreateDevice(DriverGetDriverObject(),
                            sizeof(XENFILT_DX),
                            NULL,
                            DeviceType,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &FilterDeviceObject);
    if (!NT_SUCCESS(status))
        goto fail1;

    Dx = (PXENFILT_DX)FilterDeviceObject->DeviceExtension;
    RtlZeroMemory(Dx, sizeof (XENFILT_DX));

    Dx->Type = PHYSICAL_DEVICE_OBJECT;
    Dx->DeviceObject = FilterDeviceObject;
    Dx->DevicePnpState = Present;
    Dx->SystemPowerState = PowerSystemWorking;
    Dx->DevicePowerState = PowerDeviceD3;

    IoInitializeRemoveLock(&Dx->RemoveLock, PDO_TAG, 0, 0);

    Pdo = __PdoAllocate(sizeof (XENFILT_PDO));

    status = STATUS_NO_MEMORY;
    if (Pdo == NULL)
        goto fail2;

    LowerDeviceObject = IoAttachDeviceToDeviceStack(FilterDeviceObject,
                                                    PhysicalDeviceObject);

    status = STATUS_UNSUCCESSFUL;
    if (LowerDeviceObject == NULL)
        goto fail3;

    Pdo->Dx = Dx;
    Pdo->Fdo = Fdo;
    Pdo->PhysicalDeviceObject = PhysicalDeviceObject;
    Pdo->LowerDeviceObject = LowerDeviceObject;
    Pdo->Type = Type;

    status = PdoSetDeviceInformation(Pdo);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = DriverQueryId(Pdo->LowerDeviceObject,
                           BusQueryCompatibleIDs,
                           &CompatibleIDs);
    if (!NT_SUCCESS(status))
        CompatibleIDs = NULL;

    status = EmulatedAddObject(DriverGetEmulatedContext(),
                               __PdoGetDeviceID(Pdo),
                               __PdoGetInstanceID(Pdo),
                               CompatibleIDs,
                               __PdoGetType(Pdo),
                               &Pdo->EmulatedObject);
    if (!NT_SUCCESS(status))
        goto fail5;

    if (CompatibleIDs)
        ExFreePool(CompatibleIDs);

    __PdoSetName(Pdo);

    Info("%p (%s) %s\n",
         FilterDeviceObject,
         __PdoGetName(Pdo),
         Pdo->Active ? "[ACTIVE]" : "");

    Dx->Pdo = Pdo;

#pragma prefast(suppress:28182) // Dereferencing NULL pointer
    FilterDeviceObject->DeviceType = LowerDeviceObject->DeviceType;
    FilterDeviceObject->Characteristics = LowerDeviceObject->Characteristics;

    FilterDeviceObject->Flags |= LowerDeviceObject->Flags;
    FilterDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    FdoAddPhysicalDeviceObject(Fdo, Pdo);

    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");

    if (CompatibleIDs)
        ExFreePool(CompatibleIDs);

    PdoClearDeviceInformation(Pdo);

fail4:
    Error("fail4\n");

    Pdo->Type = XENFILT_EMULATED_OBJECT_TYPE_UNKNOWN;
    Pdo->PhysicalDeviceObject = NULL;
    Pdo->LowerDeviceObject = NULL;
    Pdo->Fdo = NULL;
    Pdo->Dx = NULL;

    IoDetachDevice(LowerDeviceObject);

fail3:
    Error("fail3\n");

    ASSERT(IsZeroMemory(Pdo, sizeof (XENFILT_PDO)));
    __PdoFree(Pdo);

fail2:
    Error("fail2\n");

    IoDeleteDevice(FilterDeviceObject);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
PdoDestroy(
    _In_ PXENFILT_PDO           Pdo
    )
{
    PDEVICE_OBJECT              LowerDeviceObject = Pdo->LowerDeviceObject;
    PXENFILT_DX                 Dx = Pdo->Dx;
    PDEVICE_OBJECT              FilterDeviceObject = Dx->DeviceObject;
    PXENFILT_FDO                Fdo = __PdoGetFdo(Pdo);

    ASSERT3U(__PdoGetDevicePnpState(Pdo), ==, Deleted);

    ASSERT(__PdoIsMissing(Pdo));
    Pdo->Missing = FALSE;

    FdoRemovePhysicalDeviceObject(Fdo, Pdo);

    Dx->Pdo = NULL;

    Info("%p (%s) (%s)\n",
         FilterDeviceObject,
         __PdoGetName(Pdo),
         Pdo->Reason);
    Pdo->Reason = NULL;

    RtlZeroMemory(Pdo->Name, sizeof (Pdo->Name));

    EmulatedRemoveObject(DriverGetEmulatedContext(),
                         Pdo->EmulatedObject);
    Pdo->EmulatedObject = NULL;

    PdoClearDeviceInformation(Pdo);

    Pdo->Type = XENFILT_EMULATED_OBJECT_TYPE_UNKNOWN;
    Pdo->PhysicalDeviceObject = NULL;
    Pdo->LowerDeviceObject = NULL;
    Pdo->Fdo = NULL;
    Pdo->Dx = NULL;

    IoDetachDevice(LowerDeviceObject);

    ASSERT(IsZeroMemory(Pdo, sizeof (XENFILT_PDO)));
    __PdoFree(Pdo);

    IoDeleteDevice(FilterDeviceObject);
}
