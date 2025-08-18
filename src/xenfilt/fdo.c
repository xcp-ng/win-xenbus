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
#include <xen.h>

#include "emulated.h"
#include "names.h"
#include "fdo.h"
#include "pdo.h"
#include "thread.h"
#include "driver.h"
#include "registry.h"
#include "mutex.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

#define FDO_TAG 'ODF'

#define MAXNAMELEN  128

struct _XENFILT_FDO {
    PXENFILT_DX                     Dx;
    PDEVICE_OBJECT                  LowerDeviceObject;
    PDEVICE_OBJECT                  PhysicalDeviceObject;
    CHAR                            Name[MAXNAMELEN];

    MUTEX                           Mutex;
    LIST_ENTRY                      List;
    ULONG                           References;

    BOOLEAN                         Enumerated;

    XENFILT_EMULATED_OBJECT_TYPE    Type;
};

static FORCEINLINE PVOID
__FdoAllocate(
    _In_ ULONG  Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, FDO_TAG);
}

static FORCEINLINE VOID
__FdoFree(
    _In_ PVOID  Buffer
    )
{
    __FreePoolWithTag(Buffer, FDO_TAG);
}

static FORCEINLINE VOID
__FdoSetDevicePnpState(
    _In_ PXENFILT_FDO       Fdo,
    _In_ DEVICE_PNP_STATE   State
    )
{
    PXENFILT_DX             Dx = Fdo->Dx;

    // We can never transition out of the deleted state
    ASSERT(Dx->DevicePnpState != Deleted || State == Deleted);

    Dx->PreviousDevicePnpState = Dx->DevicePnpState;
    Dx->DevicePnpState = State;
}

static FORCEINLINE VOID
__FdoRestoreDevicePnpState(
    _In_ PXENFILT_FDO       Fdo,
    _In_ DEVICE_PNP_STATE   State
    )
{
    PXENFILT_DX             Dx = Fdo->Dx;

    if (Dx->DevicePnpState == State)
        Dx->DevicePnpState = Dx->PreviousDevicePnpState;
}

static FORCEINLINE DEVICE_PNP_STATE
__FdoGetDevicePnpState(
    _In_ PXENFILT_FDO   Fdo
    )
{
    PXENFILT_DX         Dx = Fdo->Dx;

    return Dx->DevicePnpState;
}

static FORCEINLINE DEVICE_PNP_STATE
__FdoGetPreviousDevicePnpState(
    _In_ PXENFILT_FDO   Fdo
    )
{
    PXENFILT_DX         Dx = Fdo->Dx;

    return Dx->PreviousDevicePnpState;
}

static FORCEINLINE VOID
__FdoSetDevicePowerState(
    _In_ PXENFILT_FDO       Fdo,
    _In_ DEVICE_POWER_STATE State
    )
{
    PXENFILT_DX             Dx = Fdo->Dx;

    Dx->DevicePowerState = State;
}

static FORCEINLINE DEVICE_POWER_STATE
__FdoGetDevicePowerState(
    _In_ PXENFILT_FDO   Fdo
    )
{
    PXENFILT_DX         Dx = Fdo->Dx;

    return Dx->DevicePowerState;
}

static FORCEINLINE VOID
__FdoSetSystemPowerState(
    _In_ PXENFILT_FDO       Fdo,
    _In_ SYSTEM_POWER_STATE State
    )
{
    PXENFILT_DX              Dx = Fdo->Dx;

    Dx->SystemPowerState = State;
}

static FORCEINLINE SYSTEM_POWER_STATE
__FdoGetSystemPowerState(
    _In_ PXENFILT_FDO   Fdo
    )
{
    PXENFILT_DX         Dx = Fdo->Dx;

    return Dx->SystemPowerState;
}

static FORCEINLINE PDEVICE_OBJECT
__FdoGetDeviceObject(
    _In_ PXENFILT_FDO   Fdo
    )
{
    PXENFILT_DX         Dx = Fdo->Dx;

    return Dx->DeviceObject;
}

PDEVICE_OBJECT
FdoGetDeviceObject(
    _In_ PXENFILT_FDO   Fdo
    )
{
    return __FdoGetDeviceObject(Fdo);
}

static FORCEINLINE PDEVICE_OBJECT
__FdoGetPhysicalDeviceObject(
    _In_ PXENFILT_FDO   Fdo
    )
{
    return Fdo->PhysicalDeviceObject;
}

PDEVICE_OBJECT
FdoGetPhysicalDeviceObject(
    _In_ PXENFILT_FDO   Fdo
    )
{
    return __FdoGetPhysicalDeviceObject(Fdo);
}

static FORCEINLINE NTSTATUS
__FdoSetDeviceID(
    _In_ PXENFILT_FDO   Fdo
    )
{
    PXENFILT_DX         Dx = Fdo->Dx;

    return DriverQueryId(Fdo->PhysicalDeviceObject,
                         BusQueryDeviceID,
                         &Dx->DeviceID);

}

static FORCEINLINE PSTR
__FdoGetDeviceID(
    _In_ PXENFILT_FDO   Fdo
    )
{
    PXENFILT_DX         Dx = Fdo->Dx;

    return Dx->DeviceID;
}

static FORCEINLINE VOID
__FdoClearDeviceID(
    _In_ PXENFILT_FDO   Fdo
    )
{
    PXENFILT_DX         Dx = Fdo->Dx;

    ExFreePool(Dx->DeviceID);
    Dx->DeviceID = NULL;
}

static FORCEINLINE NTSTATUS
__FdoSetInstanceID(
    _In_ PXENFILT_FDO   Fdo
    )
{
    PXENFILT_DX         Dx = Fdo->Dx;

    return DriverQueryId(Fdo->PhysicalDeviceObject,
                         BusQueryInstanceID,
                         &Dx->InstanceID);
}

static FORCEINLINE PSTR
__FdoGetInstanceID(
    _In_ PXENFILT_FDO   Fdo
    )
{
    PXENFILT_DX         Dx = Fdo->Dx;

    return Dx->InstanceID;
}

static FORCEINLINE VOID
__FdoClearInstanceID(
    _In_ PXENFILT_FDO   Fdo
    )
{
    PXENFILT_DX         Dx = Fdo->Dx;

    ExFreePool(Dx->InstanceID);
    Dx->InstanceID = NULL;
}

static FORCEINLINE VOID
__FdoSetName(
    _In_ PXENFILT_FDO   Fdo
    )
{
    NTSTATUS            status;

    status = RtlStringCbPrintfA(Fdo->Name,
                                MAXNAMELEN,
                                "%s\\%s",
                                __FdoGetDeviceID(Fdo),
                                __FdoGetInstanceID(Fdo));
    ASSERT(NT_SUCCESS(status));
}

static FORCEINLINE PSTR
__FdoGetName(
    _In_ PXENFILT_FDO   Fdo
    )
{
    return Fdo->Name;
}

VOID
FdoAddPhysicalDeviceObject(
    _In_ PXENFILT_FDO   Fdo,
    _In_ PXENFILT_PDO   Pdo
    )
{
    PDEVICE_OBJECT      DeviceObject;
    PXENFILT_DX         Dx;

    DeviceObject = PdoGetDeviceObject(Pdo);
    Dx = (PXENFILT_DX)DeviceObject->DeviceExtension;
    ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

    InsertTailList(&Fdo->List, &Dx->ListEntry);
    ASSERT3U(Fdo->References, !=, 0);
    Fdo->References++;

    PdoResume(Pdo);
}

VOID
FdoRemovePhysicalDeviceObject(
    _In_ PXENFILT_FDO   Fdo,
    _In_ PXENFILT_PDO   Pdo
    )
{
    PDEVICE_OBJECT      DeviceObject;
    PXENFILT_DX         Dx;

    DeviceObject = PdoGetDeviceObject(Pdo);
    Dx = (PXENFILT_DX)DeviceObject->DeviceExtension;
    ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

    PdoSuspend(Pdo);

    RemoveEntryList(&Dx->ListEntry);
    ASSERT3U(Fdo->References, !=, 0);
    --Fdo->References;
}

static FORCEINLINE VOID
__FdoAcquireMutex(
    _In_ PXENFILT_FDO    Fdo
    )
{
    AcquireMutex(&Fdo->Mutex);
}

VOID
FdoAcquireMutex(
    _In_ PXENFILT_FDO    Fdo
    )
{
    __FdoAcquireMutex(Fdo);
}

static FORCEINLINE VOID
__FdoReleaseMutex(
    _In_ PXENFILT_FDO    Fdo
    )
{
    ReleaseMutex(&Fdo->Mutex);
}

VOID
FdoReleaseMutex(
    _In_ PXENFILT_FDO    Fdo
    )
{
    __FdoReleaseMutex(Fdo);

    if (Fdo->References == 0) {
        DriverAcquireMutex();
        FdoDestroy(Fdo);
        DriverReleaseMutex();
    }
}

static FORCEINLINE VOID
__FdoSetEnumerated(
    _In_ PXENFILT_FDO   Fdo
    )
{
    Fdo->Enumerated = TRUE;

    DriverSetFilterState();
}

BOOLEAN
FdoHasEnumerated(
    _In_ PXENFILT_FDO   Fdo
    )
{
    return Fdo->Enumerated;
}

static VOID
FdoEnumerate(
    _In_ PXENFILT_FDO       Fdo,
    _In_ PDEVICE_RELATIONS  Relations
    )
{
    PDEVICE_OBJECT          *PhysicalDeviceObject;
    ULONG                   Count;
    PLIST_ENTRY             ListEntry;
    ULONG                   Index;
    ULONG                   ActiveIndex;
    ULONG                   Precedence;
    NTSTATUS                status;

    Count = Relations->Count;
    ASSERT(Count != 0);

    PhysicalDeviceObject = __FdoAllocate(sizeof (PDEVICE_OBJECT) * Count);

    status = STATUS_NO_MEMORY;
    if (PhysicalDeviceObject == NULL)
        goto fail1;

    RtlCopyMemory(PhysicalDeviceObject,
                  Relations->Objects,
                  sizeof (PDEVICE_OBJECT) * Count);

    // Remove any PDOs that do not appear in the device list
    ListEntry = Fdo->List.Flink;
    while (ListEntry != &Fdo->List) {
        PLIST_ENTRY     Next = ListEntry->Flink;
        PXENFILT_DX     Dx = CONTAINING_RECORD(ListEntry, XENFILT_DX, ListEntry);
        PXENFILT_PDO    Pdo = Dx->Pdo;

        if (!PdoIsMissing(Pdo) && PdoGetDevicePnpState(Pdo) != Deleted) {
            BOOLEAN         Missing;

            Missing = TRUE;

            for (Index = 0; Index < Count; Index++) {
                if (PdoGetPhysicalDeviceObject(Pdo) == PhysicalDeviceObject[Index]) {
                    Missing = FALSE;
#pragma prefast(suppress:6387)  // PhysicalDeviceObject[Index] could be NULL
                    ObDereferenceObject(PhysicalDeviceObject[Index]);
                    PhysicalDeviceObject[Index] = NULL; // avoid duplication
                    break;
                }
            }

            if (Missing) {
                PdoSetMissing(Pdo, "device disappeared");

                // If the PDO has not yet been enumerated then we can
                // go ahead and mark it as deleted, otherwise we need
                // to notify PnP manager and wait for the REMOVE_DEVICE
                // IRP.
                if (PdoGetDevicePnpState(Pdo) == Present) {
                    PdoSetDevicePnpState(Pdo, Deleted);
                    PdoDestroy(Pdo);
                }
            }
        }

        ListEntry = Next;
    }

    ActiveIndex = Precedence = 0;
    for (Index = 0; Index < Count; Index++) {
        ULONG   ThisPrecedence;

        if (PhysicalDeviceObject[Index] != NULL) {
            status = DriverGetPrecedence(PhysicalDeviceObject[Index],
                                         &ThisPrecedence);
            if (ThisPrecedence > Precedence) {
                ActiveIndex = Index;
                Precedence = ThisPrecedence;
            }
        }
    }

    // Walk the list and create PDO filters for any new devices
    for (Index = 0; Index < Count; Index++) {
#pragma warning(suppress:6385)  // Reading invalid data from 'PhysicalDeviceObject'
        if (PhysicalDeviceObject[Index] != NULL) {
            XENBUS_EMULATED_ACTIVATION_STATUS   ForceActivate;

            if (Precedence > 0)
                ForceActivate = Index == ActiveIndex ?
                                XENBUS_EMULATED_FORCE_ACTIVATED :
                                XENBUS_EMULATED_FORCE_DEACTIVATED;
            else
                ForceActivate = XENBUS_EMULATED_ACTIVATE_NEUTRAL;

            (VOID) PdoCreate(Fdo,
                             PhysicalDeviceObject[Index],
                             Fdo->Type,
                             ForceActivate);
            ObDereferenceObject(PhysicalDeviceObject[Index]);
        }
    }

    __FdoSetEnumerated(Fdo);

    __FdoFree(PhysicalDeviceObject);
    return;

fail1:
    Error("fail1 (%08x)\n", status);
}

static IO_COMPLETION_ROUTINE FdoForwardIrpSynchronouslyCompletion;

_Use_decl_annotations_
static NTSTATUS
FdoForwardIrpSynchronouslyCompletion(
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
FdoForwardIrpSynchronously(
    _In_ PXENFILT_FDO   Fdo,
    _In_ PIRP           Irp
    )
{
    KEVENT              Event;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           FdoForwardIrpSynchronouslyCompletion,
                           &Event,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
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
FdoStartDevice(
    _In_ PXENFILT_FDO           Fdo,
    _In_ PIRP                   Irp
    )
{
    POWER_STATE                 PowerState;
    NTSTATUS                    status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail2;

    PowerState.DeviceState = PowerDeviceD0;
    PoSetPowerState(Fdo->Dx->DeviceObject,
                    DevicePowerState,
                    PowerState);

    __FdoSetDevicePowerState(Fdo, PowerDeviceD0);

    __FdoSetDevicePnpState(Fdo, Started);

    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;

fail2:
    Error("fail2\n");

    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoQueryStopDevice(
    _In_ PXENFILT_FDO   Fdo,
    _In_ PIRP           Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    __FdoSetDevicePnpState(Fdo, StopPending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoCancelStopDevice(
    _In_ PXENFILT_FDO   Fdo,
    _In_ PIRP           Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    Irp->IoStatus.Status = STATUS_SUCCESS;

    __FdoRestoreDevicePnpState(Fdo, StopPending);

    IoSkipCurrentIrpStackLocation(Irp);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoStopDevice(
    _In_ PXENFILT_FDO           Fdo,
    _In_ PIRP                   Irp
    )
{
    NTSTATUS                    status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    if (__FdoGetDevicePowerState(Fdo) == PowerDeviceD0) {
        POWER_STATE PowerState;

        PowerState.DeviceState = PowerDeviceD3;
        PoSetPowerState(Fdo->Dx->DeviceObject,
                        DevicePowerState,
                        PowerState);

        __FdoSetDevicePowerState(Fdo, PowerDeviceD3);
    }

    __FdoSetDevicePnpState(Fdo, Stopped);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoQueryRemoveDevice(
    _In_ PXENFILT_FDO   Fdo,
    _In_ PIRP           Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    __FdoSetDevicePnpState(Fdo, RemovePending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoCancelRemoveDevice(
    _In_ PXENFILT_FDO   Fdo,
    _In_ PIRP           Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    __FdoRestoreDevicePnpState(Fdo, RemovePending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoSurpriseRemoval(
    _In_ PXENFILT_FDO   Fdo,
    _In_ PIRP           Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    __FdoSetDevicePnpState(Fdo, SurpriseRemovePending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoRemoveDevice(
    _In_ PXENFILT_FDO           Fdo,
    _In_ PIRP                   Irp
    )
{
    PLIST_ENTRY                 ListEntry;
    NTSTATUS                    status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    if (__FdoGetPreviousDevicePnpState(Fdo) != Started)
        goto done;

    __FdoAcquireMutex(Fdo);

    ListEntry = Fdo->List.Flink;
    while (ListEntry != &Fdo->List) {
        PLIST_ENTRY     Flink = ListEntry->Flink;
        PXENFILT_DX     Dx = CONTAINING_RECORD(ListEntry, XENFILT_DX, ListEntry);
        PXENFILT_PDO    Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        if (!PdoIsMissing(Pdo))
            PdoSetMissing(Pdo, "FDO removed");

        PdoSetDevicePnpState(Pdo, Deleted);
        PdoDestroy(Pdo);

        ListEntry = Flink;
    }

    __FdoReleaseMutex(Fdo);

    if (__FdoGetDevicePowerState(Fdo) == PowerDeviceD0) {
        POWER_STATE PowerState;

        PowerState.DeviceState = PowerDeviceD3;
        PoSetPowerState(Fdo->Dx->DeviceObject,
                        DevicePowerState,
                        PowerState);

        __FdoSetDevicePowerState(Fdo, PowerDeviceD3);
    }

done:
    __FdoSetDevicePnpState(Fdo, Deleted);

    status = FdoForwardIrpSynchronously(Fdo, Irp);

    IoReleaseRemoveLockAndWait(&Fdo->Dx->RemoveLock, Irp);

    __FdoAcquireMutex(Fdo);
    ASSERT3U(Fdo->References, !=, 0);
    --Fdo->References;
    __FdoReleaseMutex(Fdo);

    if (Fdo->References == 0) {
        DriverAcquireMutex();
        FdoDestroy(Fdo);
        DriverReleaseMutex();
    }

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static IO_COMPLETION_ROUTINE FdoQueryDeviceRelationsCompletion;

_Use_decl_annotations_
static NTSTATUS
FdoQueryDeviceRelationsCompletion(
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
FdoQueryDeviceRelations(
    _In_ PXENFILT_FDO       Fdo,
    _In_ PIRP               Irp
    )
{
    KEVENT                  Event;
    PIO_STACK_LOCATION      StackLocation;
    ULONG                   Size;
    PDEVICE_RELATIONS       Relations;
    PLIST_ENTRY             ListEntry;
    XENFILT_FILTER_STATE    State;
    ULONG                   Count;
    NTSTATUS                status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           FdoQueryDeviceRelationsCompletion,
                           &Event,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
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

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    if (StackLocation->Parameters.QueryDeviceRelations.Type != BusRelations)
        goto done;

    __FdoAcquireMutex(Fdo);

    Relations = (PDEVICE_RELATIONS)Irp->IoStatus.Information;

    if (Relations->Count != 0)
        FdoEnumerate(Fdo, Relations);

    ExFreePool(Relations);

    State = DriverGetFilterState();
    Count = 0;

    if (State == XENFILT_FILTER_DISABLED) {
        for (ListEntry = Fdo->List.Flink;
             ListEntry != &Fdo->List;
             ListEntry = ListEntry->Flink)
            Count++;
    }

    Size = FIELD_OFFSET(DEVICE_RELATIONS, Objects) +
           (sizeof (PDEVICE_OBJECT) * __max(Count, 1));

    Relations = __AllocatePoolWithTag(PagedPool, Size, 'TLIF');

    status = STATUS_NO_MEMORY;
    if (Relations == NULL)
        goto fail3;

    if (State == XENFILT_FILTER_DISABLED) {
        ListEntry = Fdo->List.Flink;
        while (ListEntry != &Fdo->List) {
            PXENFILT_DX     Dx = CONTAINING_RECORD(ListEntry, XENFILT_DX, ListEntry);
            PXENFILT_PDO    Pdo = Dx->Pdo;
            PLIST_ENTRY     Next = ListEntry->Flink;

            ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

            if (PdoIsMissing(Pdo)) {
                if (PdoGetDevicePnpState(Pdo) == Deleted)
                    PdoDestroy(Pdo);

                goto next;
            }

            if (PdoGetDevicePnpState(Pdo) == Present)
                PdoSetDevicePnpState(Pdo, Enumerated);

            ObReferenceObject(PdoGetPhysicalDeviceObject(Pdo));
            Relations->Objects[Relations->Count++] = PdoGetPhysicalDeviceObject(Pdo);

next:
            ListEntry = Next;
        }

        ASSERT3U(Relations->Count, <=, Count);

        Trace("%s: %d PDO(s)\n",
              __FdoGetName(Fdo),
              Relations->Count);
    } else {
        Trace("%s: FILTERED\n",
              __FdoGetName(Fdo));

        IoInvalidateDeviceRelations(__FdoGetPhysicalDeviceObject(Fdo),
                                    BusRelations);
    }

    __FdoReleaseMutex(Fdo);

    Irp->IoStatus.Information = (ULONG_PTR)Relations;
    status = STATUS_SUCCESS;

done:
    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;

fail3:
    __FdoReleaseMutex(Fdo);

fail2:
    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoDispatchPnp(
    _In_ PXENFILT_FDO   Fdo,
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
        status = FdoStartDevice(Fdo, Irp);
        break;

    case IRP_MN_QUERY_STOP_DEVICE:
        status = FdoQueryStopDevice(Fdo, Irp);
        break;

    case IRP_MN_CANCEL_STOP_DEVICE:
        status = FdoCancelStopDevice(Fdo, Irp);
        break;

    case IRP_MN_STOP_DEVICE:
        status = FdoStopDevice(Fdo, Irp);
        break;

    case IRP_MN_QUERY_REMOVE_DEVICE:
        status = FdoQueryRemoveDevice(Fdo, Irp);
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        status = FdoSurpriseRemoval(Fdo, Irp);
        break;

    case IRP_MN_REMOVE_DEVICE:
        status = FdoRemoveDevice(Fdo, Irp);
        break;

    case IRP_MN_CANCEL_REMOVE_DEVICE:
        status = FdoCancelRemoveDevice(Fdo, Irp);
        break;

    case IRP_MN_QUERY_DEVICE_RELATIONS:
        status = FdoQueryDeviceRelations(Fdo, Irp);
        break;

    default:
        status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
        if (!NT_SUCCESS(status))
            goto fail1;

        IoSkipCurrentIrpStackLocation(Irp);

        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
        IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);
        break;
    }

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static IO_COMPLETION_ROUTINE FdoSetDevicePowerUpComplete;

_Use_decl_annotations_
static NTSTATUS
FdoSetDevicePowerUpComplete(
    PDEVICE_OBJECT      DeviceObject,
    PIRP                Irp,
    PVOID               Context
    )
{
    PXENFILT_FDO        Fdo = (PXENFILT_FDO)Context;
    PIO_STACK_LOCATION  StackLocation;
    POWER_STATE         PowerState;

    UNREFERENCED_PARAMETER(DeviceObject);

    ASSERT(Fdo != NULL);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    PowerState = StackLocation->Parameters.Power.State;

    ASSERT3U(PowerState.DeviceState, <,  __FdoGetDevicePowerState(Fdo));

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    Trace("%s: %s -> %s\n",
          __FdoGetName(Fdo),
          DevicePowerStateName(__FdoGetDevicePowerState(Fdo)),
          DevicePowerStateName(PowerState.DeviceState));

    PoSetPowerState(Fdo->Dx->DeviceObject,
                    DevicePowerState,
                    PowerState);

    __FdoSetDevicePowerState(Fdo, PowerState.DeviceState);

    return STATUS_CONTINUE_COMPLETION;
}

static FORCEINLINE NTSTATUS
__FdoSetDevicePowerUp(
    _In_ PXENFILT_FDO   Fdo,
    _In_ PIRP           Irp
    )
{
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           FdoSetDevicePowerUpComplete,
                           Fdo,
                           TRUE,
                           TRUE,
                           TRUE);

    return IoCallDriver(Fdo->LowerDeviceObject, Irp);
}

static NTSTATUS
__FdoSetDevicePowerDown(
    _In_ PXENFILT_FDO   Fdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    POWER_STATE         PowerState;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    PowerState = StackLocation->Parameters.Power.State;

    ASSERT3U(PowerState.DeviceState, >,  __FdoGetDevicePowerState(Fdo));

    Trace("%s: %s -> %s\n",
          __FdoGetName(Fdo),
          DevicePowerStateName(__FdoGetDevicePowerState(Fdo)),
          DevicePowerStateName(PowerState.DeviceState));

    PoSetPowerState(Fdo->Dx->DeviceObject,
                    DevicePowerState,
                    PowerState);

    __FdoSetDevicePowerState(Fdo, PowerState.DeviceState);

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Fdo->LowerDeviceObject, Irp);
}

static NTSTATUS
FdoSetDevicePower(
    _In_ PXENFILT_FDO   Fdo,
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
          __FdoGetName(Fdo),
          DevicePowerStateName(DeviceState),
          PowerActionName(PowerAction));

    if (DeviceState == __FdoGetDevicePowerState(Fdo)) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

        goto done;
    }

    status = (DeviceState < __FdoGetDevicePowerState(Fdo)) ?
             __FdoSetDevicePowerUp(Fdo, Irp) :
             __FdoSetDevicePowerDown(Fdo, Irp);

done:
    Trace("%s: <==== (%s:%s)(%08x)\n",
          __FdoGetName(Fdo),
          DevicePowerStateName(DeviceState),
          PowerActionName(PowerAction),
          status);
    return status;
}

static FORCEINLINE NTSTATUS
__FdoDispatchDevicePower(
    _In_ PXENFILT_FDO   Fdo,
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
        status = FdoSetDevicePower(Fdo, Irp);
        break;

    default:
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
        break;
    }

    return status;
}

static IO_COMPLETION_ROUTINE FdoSetSystemPowerUpComplete;

_Use_decl_annotations_
static NTSTATUS
FdoSetSystemPowerUpComplete(
    PDEVICE_OBJECT      DeviceObject,
    PIRP                Irp,
    PVOID               Context
    )
{
    PXENFILT_FDO        Fdo = (PXENFILT_FDO)Context;
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;

    UNREFERENCED_PARAMETER(DeviceObject);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, <,  __FdoGetSystemPowerState(Fdo));

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    Trace("%s: %s -> %s\n",
          __FdoGetName(Fdo),
          SystemPowerStateName(__FdoGetSystemPowerState(Fdo)),
          SystemPowerStateName(SystemState));

    __FdoSetSystemPowerState(Fdo, SystemState);

    return STATUS_CONTINUE_COMPLETION;
}

static FORCEINLINE NTSTATUS
__FdoSetSystemPowerUp(
    _In_ PXENFILT_FDO   Fdo,
    _In_ PIRP           Irp
    )
{
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           FdoSetSystemPowerUpComplete,
                           Fdo,
                           TRUE,
                           TRUE,
                           TRUE);
    return IoCallDriver(Fdo->LowerDeviceObject, Irp);
}

static FORCEINLINE NTSTATUS
__FdoSetSystemPowerDown(
    _In_ PXENFILT_FDO   Fdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, >,  __FdoGetSystemPowerState(Fdo));

    Trace("%s: %s -> %s\n",
          __FdoGetName(Fdo),
          SystemPowerStateName(__FdoGetSystemPowerState(Fdo)),
          SystemPowerStateName(SystemState));

    __FdoSetSystemPowerState(Fdo, SystemState);

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Fdo->LowerDeviceObject, Irp);
}

static NTSTATUS
FdoSetSystemPower(
    _In_ PXENFILT_FDO   Fdo,
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
          __FdoGetName(Fdo),
          SystemPowerStateName(SystemState),
          PowerActionName(PowerAction));

    if (SystemState == __FdoGetSystemPowerState(Fdo)) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

        goto done;
    }

    status = (SystemState < __FdoGetSystemPowerState(Fdo)) ?
             __FdoSetSystemPowerUp(Fdo, Irp) :
             __FdoSetSystemPowerDown(Fdo, Irp);

done:
    Trace("%s: <==== (%s:%s)(%08x)\n",
          __FdoGetName(Fdo),
          SystemPowerStateName(SystemState),
          PowerActionName(PowerAction),
          status);
    return status;
}

static FORCEINLINE NTSTATUS
__FdoDispatchSystemPower(
    _In_ PXENFILT_FDO   Fdo,
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
        status = FdoSetSystemPower(Fdo, Irp);
        break;

    default:
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
        break;
    }

    return status;
}

static NTSTATUS
FdoDispatchPower(
    _In_ PXENFILT_FDO   Fdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    POWER_STATE_TYPE    PowerType;
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

    if (MinorFunction != IRP_MN_SET_POWER) {
        IoSkipCurrentIrpStackLocation(Irp);

        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
        IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

        goto done;
    }

    PowerType = StackLocation->Parameters.Power.Type;

    Trace("%s: ====> (%02x:%s)\n",
          __FdoGetName(Fdo),
          MinorFunction,
          PowerMinorFunctionName(MinorFunction));

    switch (PowerType) {
    case DevicePowerState:
        status = __FdoDispatchDevicePower(Fdo, Irp);
        IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);
        break;

    case SystemPowerState:
        status = __FdoDispatchSystemPower(Fdo, Irp);
        IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);
        break;

    default:
        IoSkipCurrentIrpStackLocation(Irp);

        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
        IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);
        break;
    }

    Trace("%s: <==== (%02x:%s) (%08x)\n",
          __FdoGetName(Fdo),
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
FdoDispatchDefault(
    _In_ PXENFILT_FDO   Fdo,
    _In_ PIRP           Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    IoSkipCurrentIrpStackLocation(Irp);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

NTSTATUS
FdoDispatch(
    _In_ PXENFILT_FDO   Fdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->MajorFunction) {
    case IRP_MJ_PNP:
        status = FdoDispatchPnp(Fdo, Irp);
        break;

    case IRP_MJ_POWER:
        status = FdoDispatchPower(Fdo, Irp);
        break;

    default:
        status = FdoDispatchDefault(Fdo, Irp);
        break;
    }

    return status;
}

NTSTATUS
FdoCreate(
    _In_ PDEVICE_OBJECT                 PhysicalDeviceObject,
    _In_ XENFILT_EMULATED_OBJECT_TYPE   Type
    )
{
    PDEVICE_OBJECT                      LowerDeviceObject;
    ULONG                               DeviceType;
    PDEVICE_OBJECT                      FilterDeviceObject;
    PXENFILT_DX                         Dx;
    PXENFILT_FDO                        Fdo;
    NTSTATUS                            status;

    ASSERT(Type != XENFILT_EMULATED_OBJECT_TYPE_UNKNOWN);

    LowerDeviceObject = IoGetAttachedDeviceReference(PhysicalDeviceObject);
    DeviceType = LowerDeviceObject->DeviceType;
    ObDereferenceObject(LowerDeviceObject);

#pragma prefast(suppress:28197) // Possibly leaking memory 'FilterDeviceObject'
    status = IoCreateDevice(DriverGetDriverObject(),
                            sizeof (XENFILT_DX),
                            NULL,
                            DeviceType,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &FilterDeviceObject);
    if (!NT_SUCCESS(status))
        goto fail1;

    Dx = (PXENFILT_DX)FilterDeviceObject->DeviceExtension;
    RtlZeroMemory(Dx, sizeof (XENFILT_DX));

    Dx->Type = FUNCTION_DEVICE_OBJECT;
    Dx->DeviceObject = FilterDeviceObject;
    Dx->DevicePnpState = Added;
    Dx->SystemPowerState = PowerSystemWorking;
    Dx->DevicePowerState = PowerDeviceD3;

    IoInitializeRemoveLock(&Dx->RemoveLock, FDO_TAG, 0, 0);

    Fdo = __FdoAllocate(sizeof (XENFILT_FDO));

    status = STATUS_NO_MEMORY;
    if (Fdo == NULL)
        goto fail2;

    LowerDeviceObject = IoAttachDeviceToDeviceStack(FilterDeviceObject,
                                                    PhysicalDeviceObject);

    status = STATUS_UNSUCCESSFUL;
    if (LowerDeviceObject == NULL)
        goto fail3;

    Fdo->Dx = Dx;
    Fdo->PhysicalDeviceObject = PhysicalDeviceObject;
    Fdo->LowerDeviceObject = LowerDeviceObject;
    Fdo->Type = Type;

    status = __FdoSetDeviceID(Fdo);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = __FdoSetInstanceID(Fdo);
    if (!NT_SUCCESS(status))
        goto fail5;

    __FdoSetName(Fdo);

    InitializeMutex(&Fdo->Mutex);
    InitializeListHead(&Fdo->List);
    Fdo->References = 1;

    Info("%p (%s)\n",
         FilterDeviceObject,
         __FdoGetName(Fdo));

    Dx->Fdo = Fdo;

#pragma prefast(suppress:28182)  // Dereferencing NULL pointer
    FilterDeviceObject->DeviceType = LowerDeviceObject->DeviceType;
    FilterDeviceObject->Characteristics = LowerDeviceObject->Characteristics;

    FilterDeviceObject->Flags |= LowerDeviceObject->Flags;
    FilterDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    DriverAddFunctionDeviceObject(Fdo);

    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");

    __FdoClearDeviceID(Fdo);

fail4:
    Error("fail4\n");

    Fdo->Type = XENFILT_EMULATED_OBJECT_TYPE_UNKNOWN;
    Fdo->PhysicalDeviceObject = NULL;
    Fdo->LowerDeviceObject = NULL;
    Fdo->Dx = NULL;

    IoDetachDevice(LowerDeviceObject);

fail3:
    Error("fail3\n");

    ASSERT(IsZeroMemory(Fdo, sizeof (XENFILT_FDO)));
    __FdoFree(Fdo);

fail2:
    Error("fail2\n");

    IoDeleteDevice(FilterDeviceObject);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
FdoDestroy(
    _In_ PXENFILT_FDO   Fdo
    )
{
    PDEVICE_OBJECT      LowerDeviceObject = Fdo->LowerDeviceObject;
    PXENFILT_DX         Dx = Fdo->Dx;
    PDEVICE_OBJECT      FilterDeviceObject = Dx->DeviceObject;

    ASSERT(IsListEmpty(&Fdo->List));
    ASSERT3U(Fdo->References, ==, 0);
    ASSERT3U(__FdoGetDevicePnpState(Fdo), ==, Deleted);

    DriverRemoveFunctionDeviceObject(Fdo);

    Fdo->Enumerated = FALSE;

    Dx->Fdo = NULL;

    Info("%p (%s)\n",
         FilterDeviceObject,
         __FdoGetName(Fdo));

    RtlZeroMemory(&Fdo->List, sizeof (LIST_ENTRY));
    RtlZeroMemory(&Fdo->Mutex, sizeof (MUTEX));

    RtlZeroMemory(Fdo->Name, sizeof (Fdo->Name));

    __FdoClearInstanceID(Fdo);
    __FdoClearDeviceID(Fdo);

    Fdo->Type = XENFILT_EMULATED_OBJECT_TYPE_UNKNOWN;
    Fdo->LowerDeviceObject = NULL;
    Fdo->PhysicalDeviceObject = NULL;
    Fdo->Dx = NULL;

    IoDetachDevice(LowerDeviceObject);

    ASSERT(IsZeroMemory(Fdo, sizeof (XENFILT_FDO)));
    __FdoFree(Fdo);

    ASSERT3U(Dx->DevicePowerState, ==, PowerDeviceD3);
    ASSERT3U(Dx->SystemPowerState, ==, PowerSystemWorking);

    IoDeleteDevice(FilterDeviceObject);
}
