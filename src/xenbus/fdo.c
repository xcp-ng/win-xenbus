/* Copyright (c) Xen Project.
 * Copyright (c) Cloud Software Group, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source 1and binary forms,
 * with or without modification, are permitted provided
 * that the following conditions are met:
 *
 * *   Redistributions of source code must retain the above
 *     copyright notice, this list of conditions and the23
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
#include <procgrp.h>
#include <wdmguid.h>
#include <ntstrsafe.h>
#include <stdlib.h>
#include <xen.h>

#include <version.h>

#include "names.h"
#include "registry.h"
#include "fdo.h"
#include "pdo.h"
#include "thread.h"
#include "high.h"
#include "mutex.h"
#include "emulated_interface.h"
#include "shared_info.h"
#include "evtchn.h"
#include "debug.h"
#include "store.h"
#include "console.h"
#include "cache.h"
#include "gnttab.h"
#include "suspend.h"
#include "sync.h"
#include "balloon.h"
#include "driver.h"
#include "range_set.h"
#include "unplug.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

#define XENBUS_FDO_TAG 'ODF'

#define MAXNAMELEN  128

struct _XENBUS_INTERRUPT {
    PXENBUS_FDO         Fdo;
    LIST_ENTRY          ListEntry;
    KINTERRUPT_MODE     InterruptMode;
    PKINTERRUPT         InterruptObject;
    PROCESSOR_NUMBER    ProcNumber;
    UCHAR               Vector;
    ULONG               Line;
    PKSERVICE_ROUTINE   Callback;
    PVOID               Argument;
    ULONG               Count;
};

typedef struct _XENBUS_VIRQ {
    PXENBUS_FDO             Fdo;
    LIST_ENTRY              ListEntry;
    ULONG                   Type;
    PXENBUS_EVTCHN_CHANNEL  Channel;
    ULONG                   Cpu;
    ULONG                   Count;
} XENBUS_VIRQ, *PXENBUS_VIRQ;

typedef struct _XENBUS_PCI_HOLE {
    PXENBUS_RANGE_SET   RangeSet;
    ULONG               Count;
    PVOID               VirtualAddress;
    PHYSICAL_ADDRESS    PhysicalAddress;
} XENBUS_PCI_HOLE, *PXENBUS_PCI_HOLE;

struct _XENBUS_FDO {
    PXENBUS_DX                      Dx;
    PDEVICE_OBJECT                  LowerDeviceObject;
    PDEVICE_OBJECT                  PhysicalDeviceObject;
    DEVICE_CAPABILITIES             LowerDeviceCapabilities;
    PBUS_INTERFACE_STANDARD         LowerBusInterface;
    ULONG                           Usage[DeviceUsageTypeDumpFile + 1];
    BOOLEAN                         NotDisableable;

    PIO_WORKITEM                    SystemPowerWorkItem;
    PIRP                            SystemPowerIrp;
    PIO_WORKITEM                    DevicePowerWorkItem;
    PIRP                            DevicePowerIrp;

    CHAR                            VendorName[MAXNAMELEN];

    MUTEX                           Mutex;
    LIST_ENTRY                      List;
    ULONG                           References;

    PXENBUS_THREAD                  ScanThread;
    KEVENT                          ScanEvent;
    PXENBUS_STORE_WATCH             ScanWatch;

    PXENBUS_THREAD                  SuspendThread;
    KEVENT                          SuspendEvent;
    PXENBUS_STORE_WATCH             SuspendWatch;

    PXENBUS_THREAD                  BalloonThread;
    KEVENT                          BalloonEvent;
    PXENBUS_STORE_WATCH             BalloonWatch;
    MUTEX                           BalloonSuspendMutex;

    PCM_PARTIAL_RESOURCE_LIST       RawResourceList;
    PCM_PARTIAL_RESOURCE_LIST       TranslatedResourceList;

    XENFILT_EMULATED_INTERFACE      EmulatedInterface;

    BOOLEAN                         Active;

    PXENBUS_SUSPEND_CONTEXT         SuspendContext;
    PXENBUS_SHARED_INFO_CONTEXT     SharedInfoContext;
    PXENBUS_EVTCHN_CONTEXT          EvtchnContext;
    PXENBUS_DEBUG_CONTEXT           DebugContext;
    PXENBUS_STORE_CONTEXT           StoreContext;
    PXENBUS_CONSOLE_CONTEXT         ConsoleContext;
    PXENBUS_RANGE_SET_CONTEXT       RangeSetContext;
    PXENBUS_CACHE_CONTEXT           CacheContext;
    PXENBUS_GNTTAB_CONTEXT          GnttabContext;
    PXENBUS_UNPLUG_CONTEXT          UnplugContext;
    PXENBUS_BALLOON_CONTEXT         BalloonContext;

    XENBUS_DEBUG_INTERFACE          DebugInterface;
    XENBUS_SUSPEND_INTERFACE        SuspendInterface;
    XENBUS_EVTCHN_INTERFACE         EvtchnInterface;
    XENBUS_STORE_INTERFACE          StoreInterface;
    XENBUS_CONSOLE_INTERFACE        ConsoleInterface;
    XENBUS_RANGE_SET_INTERFACE      RangeSetInterface;
    XENBUS_BALLOON_INTERFACE        BalloonInterface;

    ULONG                           UseMemoryHole;
    XENBUS_PCI_HOLE                 PciHole;
    LIST_ENTRY                      InterruptList;

    LIST_ENTRY                      VirqList;
    HIGH_LOCK                       VirqLock;

    ULONG                           Watchdog;

    PXENBUS_DEBUG_CALLBACK          DebugCallback;
    PXENBUS_SUSPEND_CALLBACK        SuspendCallbackLate;
    BOOLEAN                         ConsoleAcquired;
    PLOG_DISPOSITION                LogDisposition;
};

static FORCEINLINE PVOID
__FdoAllocate(
    _In_ ULONG  Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENBUS_FDO_TAG);
}

static FORCEINLINE VOID
__FdoFree(
    _In_ PVOID  Buffer
    )
{
    __FreePoolWithTag(Buffer, XENBUS_FDO_TAG);
}

static FORCEINLINE VOID
__FdoSetDevicePnpState(
    _In_ PXENBUS_FDO        Fdo,
    _In_ DEVICE_PNP_STATE   State
    )
{
    PXENBUS_DX              Dx = Fdo->Dx;

    // We can never transition out of the deleted state
    ASSERT(Dx->DevicePnpState != Deleted || State == Deleted);

    Dx->PreviousDevicePnpState = Dx->DevicePnpState;
    Dx->DevicePnpState = State;
}

static FORCEINLINE VOID
__FdoRestoreDevicePnpState(
    _In_ PXENBUS_FDO        Fdo,
    _In_ DEVICE_PNP_STATE   State
    )
{
    PXENBUS_DX              Dx = Fdo->Dx;

    if (Dx->DevicePnpState == State)
        Dx->DevicePnpState = Dx->PreviousDevicePnpState;
}

static FORCEINLINE DEVICE_PNP_STATE
__FdoGetDevicePnpState(
    _In_ PXENBUS_FDO    Fdo
    )
{
    PXENBUS_DX          Dx = Fdo->Dx;

    return Dx->DevicePnpState;
}

static FORCEINLINE DEVICE_PNP_STATE
__FdoGetPreviousDevicePnpState(
    _In_ PXENBUS_FDO    Fdo
    )
{
    PXENBUS_DX          Dx = Fdo->Dx;

    return Dx->PreviousDevicePnpState;
}

static FORCEINLINE VOID
__FdoSetDevicePowerState(
    _In_ PXENBUS_FDO        Fdo,
    _In_ DEVICE_POWER_STATE State
    )
{
    PXENBUS_DX              Dx = Fdo->Dx;

    Dx->DevicePowerState = State;
}

static FORCEINLINE DEVICE_POWER_STATE
__FdoGetDevicePowerState(
    _In_ PXENBUS_FDO    Fdo
    )
{
    PXENBUS_DX          Dx = Fdo->Dx;

    return Dx->DevicePowerState;
}

static FORCEINLINE VOID
__FdoSetSystemPowerState(
    _In_ PXENBUS_FDO        Fdo,
    _In_ SYSTEM_POWER_STATE State
    )
{
    PXENBUS_DX              Dx = Fdo->Dx;

    Dx->SystemPowerState = State;
}

static FORCEINLINE SYSTEM_POWER_STATE
__FdoGetSystemPowerState(
    _In_ PXENBUS_FDO    Fdo
    )
{
    PXENBUS_DX          Dx = Fdo->Dx;

    return Dx->SystemPowerState;
}

static FORCEINLINE PDEVICE_OBJECT
__FdoGetDeviceObject(
    _In_ PXENBUS_FDO    Fdo
    )
{
    PXENBUS_DX          Dx = Fdo->Dx;

    return Dx->DeviceObject;
}

PDEVICE_OBJECT
FdoGetDeviceObject(
    _In_ PXENBUS_FDO    Fdo
    )
{
    return __FdoGetDeviceObject(Fdo);
}

static FORCEINLINE PDEVICE_OBJECT
__FdoGetPhysicalDeviceObject(
    _In_ PXENBUS_FDO    Fdo
    )
{
    return Fdo->PhysicalDeviceObject;
}

PDEVICE_OBJECT
FdoGetPhysicalDeviceObject(
    _In_ PXENBUS_FDO    Fdo
    )
{
    return __FdoGetPhysicalDeviceObject(Fdo);
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS
FdoAcquireLowerBusInterface(
    _In_ PXENBUS_FDO        Fdo
    )
{
    PBUS_INTERFACE_STANDARD BusInterface;
    KEVENT                  Event;
    IO_STATUS_BLOCK         StatusBlock;
    PIRP                    Irp;
    PIO_STACK_LOCATION      StackLocation;
    NTSTATUS                status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    BusInterface = __FdoAllocate(sizeof (BUS_INTERFACE_STANDARD));

    status = STATUS_NO_MEMORY;
    if (BusInterface == NULL)
        goto fail1;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    RtlZeroMemory(&StatusBlock, sizeof(IO_STATUS_BLOCK));

    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP,
                                       Fdo->LowerDeviceObject,
                                       NULL,
                                       0,
                                       NULL,
                                       &Event,
                                       &StatusBlock);

    status = STATUS_UNSUCCESSFUL;
    if (Irp == NULL)
        goto fail2;

    StackLocation = IoGetNextIrpStackLocation(Irp);
    StackLocation->MinorFunction = IRP_MN_QUERY_INTERFACE;

    StackLocation->Parameters.QueryInterface.InterfaceType = &GUID_BUS_INTERFACE_STANDARD;
    StackLocation->Parameters.QueryInterface.Size = sizeof (BUS_INTERFACE_STANDARD);
    StackLocation->Parameters.QueryInterface.Version = 1;
    StackLocation->Parameters.QueryInterface.Interface = (PINTERFACE)BusInterface;

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject(&Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        status = StatusBlock.Status;
    }

    if (!NT_SUCCESS(status))
        goto fail3;

    status = STATUS_INVALID_PARAMETER;
    if (BusInterface->Version != 1)
        goto fail4;

    Fdo->LowerBusInterface = BusInterface;

    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

    __FdoFree(BusInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
FdoReleaseLowerBusInterface(
    _In_ PXENBUS_FDO        Fdo
    )
{
    PBUS_INTERFACE_STANDARD BusInterface;

    BusInterface = Fdo->LowerBusInterface;

    if (BusInterface == NULL)
        return;

    Fdo->LowerBusInterface = NULL;

    BusInterface->InterfaceDereference(BusInterface->Context);

    __FdoFree(BusInterface);
}

PDMA_ADAPTER
FdoGetDmaAdapter(
    _In_ PXENBUS_FDO            Fdo,
    _In_ PDEVICE_DESCRIPTION    DeviceDescriptor,
    _Out_ PULONG                NumberOfMapRegisters
    )
{
    PBUS_INTERFACE_STANDARD     BusInterface;

    BusInterface = Fdo->LowerBusInterface;
    ASSERT(BusInterface != NULL);

    return BusInterface->GetDmaAdapter(BusInterface->Context,
                                       DeviceDescriptor,
                                       NumberOfMapRegisters);
}

BOOLEAN
FdoTranslateBusAddress(
    _In_ PXENBUS_FDO        Fdo,
    _In_ PHYSICAL_ADDRESS   BusAddress,
    _In_ ULONG              Length,
    _Out_ PULONG            AddressSpace,
    _Out_ PPHYSICAL_ADDRESS TranslatedAddress
    )
{
    PBUS_INTERFACE_STANDARD BusInterface;

    BusInterface = Fdo->LowerBusInterface;
    ASSERT(BusInterface != NULL);

    return BusInterface->TranslateBusAddress(BusInterface->Context,
                                             BusAddress,
                                             Length,
                                             AddressSpace,
                                             TranslatedAddress);
}

ULONG
FdoSetBusData(
    _In_ PXENBUS_FDO                Fdo,
    _In_ ULONG                      DataType,
    _In_reads_bytes_(Length) PVOID  Buffer,
    _In_ ULONG                      Offset,
    _In_range_(!=, 0) ULONG         Length
    )
{
    PBUS_INTERFACE_STANDARD BusInterface;

    BusInterface = Fdo->LowerBusInterface;
    ASSERT(BusInterface != NULL);

    return BusInterface->SetBusData(BusInterface->Context,
                                    DataType,
                                    Buffer,
                                    Offset,
                                    Length);
}

ULONG
FdoGetBusData(
    _In_ PXENBUS_FDO                    Fdo,
    _In_ ULONG                          DataType,
    _Out_writes_bytes_(Length) PVOID    Buffer,
    _In_ ULONG                          Offset,
    _In_range_(!=, 0) ULONG             Length
    )
{
    PBUS_INTERFACE_STANDARD             BusInterface;

    BusInterface = Fdo->LowerBusInterface;
    ASSERT(BusInterface != NULL);

#pragma prefast(suppress:6001) // imprecise GetBusData annotations
    return BusInterface->GetBusData(BusInterface->Context,
                                    DataType,
                                    Buffer,
                                    Offset,
                                    Length);
}

static FORCEINLINE NTSTATUS
__FdoSetVendorName(
    _In_ PXENBUS_FDO    Fdo,
    _In_ USHORT         VendorID,
    _In_ USHORT         DeviceID
    )
{
    NTSTATUS            status;

    status = STATUS_NOT_SUPPORTED;
    if (VendorID != 'XS')
        goto fail1;

    status = RtlStringCbPrintfA(Fdo->VendorName,
                                MAXNAMELEN,
                                "%s%04X",
                                VENDOR_PREFIX_STR,
                                DeviceID);
    ASSERT(NT_SUCCESS(status));

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE PSTR
__FdoGetVendorName(
    _In_ PXENBUS_FDO    Fdo
    )
{
    return Fdo->VendorName;
}

PSTR
FdoGetVendorName(
    _In_ PXENBUS_FDO    Fdo
    )
{
    return __FdoGetVendorName(Fdo);
}

static FORCEINLINE VOID
__FdoSetName(
    _In_ PXENBUS_FDO    Fdo
    )
{
    PXENBUS_DX          Dx = Fdo->Dx;
    NTSTATUS            status;

    status = RtlStringCbPrintfA(Dx->Name,
                                MAXNAMELEN,
                                "%s XENBUS",
                                __FdoGetVendorName(Fdo));
    ASSERT(NT_SUCCESS(status));
}

static FORCEINLINE PSTR
__FdoGetName(
    _In_ PXENBUS_FDO    Fdo
    )
{
    PXENBUS_DX      Dx = Fdo->Dx;

    return Dx->Name;
}

PSTR
FdoGetName(
    _In_ PXENBUS_FDO    Fdo
    )
{
    return __FdoGetName(Fdo);
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS
FdoQueryId(
    _In_ PXENBUS_FDO        Fdo,
    _In_ BUS_QUERY_ID_TYPE  Type,
    _Outptr_result_z_ PSTR  *Id
    )
{
    KEVENT                  Event;
    IO_STATUS_BLOCK         StatusBlock;
    PIRP                    Irp;
    PIO_STACK_LOCATION      StackLocation;
    PWSTR                   Buffer;
    ULONG                   Length;
    NTSTATUS                status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    RtlZeroMemory(&StatusBlock, sizeof(IO_STATUS_BLOCK));

    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP,
                                       Fdo->LowerDeviceObject,
                                       NULL,
                                       0,
                                       NULL,
                                       &Event,
                                       &StatusBlock);

    status = STATUS_UNSUCCESSFUL;
    if (Irp == NULL)
        goto fail1;

    StackLocation = IoGetNextIrpStackLocation(Irp);
    StackLocation->MinorFunction = IRP_MN_QUERY_ID;

    StackLocation->Parameters.QueryId.IdType = Type;

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject(&Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        status = StatusBlock.Status;
    }

    if (!NT_SUCCESS(status))
        goto fail2;

    Buffer = (PWSTR)StatusBlock.Information;
    Length = (ULONG)(wcslen(Buffer) + 1) * sizeof (CHAR);

    *Id = __AllocatePoolWithTag(NonPagedPool, Length, 'SUB');

    status = STATUS_NO_MEMORY;
    if (*Id == NULL)
        goto fail3;

    status = RtlStringCbPrintfA(*Id, Length, "%ws", Buffer);
    ASSERT(NT_SUCCESS(status));

    ExFreePool(Buffer);

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    ExFreePool((PVOID)StatusBlock.Information);

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS
FdoQueryDeviceText(
    _In_ PXENBUS_FDO        Fdo,
    _In_ DEVICE_TEXT_TYPE   Type,
    _Outptr_result_z_ PSTR  *Text
    )
{
    KEVENT                  Event;
    IO_STATUS_BLOCK         StatusBlock;
    PIRP                    Irp;
    PIO_STACK_LOCATION      StackLocation;
    PWSTR                   Buffer;
    ULONG                   Length;
    NTSTATUS                status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    RtlZeroMemory(&StatusBlock, sizeof(IO_STATUS_BLOCK));

    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP,
                                       Fdo->LowerDeviceObject,
                                       NULL,
                                       0,
                                       NULL,
                                       &Event,
                                       &StatusBlock);

    status = STATUS_UNSUCCESSFUL;
    if (Irp == NULL)
        goto fail1;

    StackLocation = IoGetNextIrpStackLocation(Irp);
    StackLocation->MinorFunction = IRP_MN_QUERY_DEVICE_TEXT;

    StackLocation->Parameters.QueryDeviceText.DeviceTextType = Type;

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject(&Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        status = StatusBlock.Status;
    }

    if (!NT_SUCCESS(status))
        goto fail2;

    Buffer = (PWSTR)StatusBlock.Information;
    Length = (ULONG)(wcslen(Buffer) + 1) * sizeof (CHAR);

    *Text = __AllocatePoolWithTag(PagedPool, Length, 'SUB');

    status = STATUS_NO_MEMORY;
    if (*Text == NULL)
        goto fail3;

    status = RtlStringCbPrintfA(*Text, Length, "%ws", Buffer);
    ASSERT(NT_SUCCESS(status));

    ExFreePool(Buffer);

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    ExFreePool((PVOID)StatusBlock.Information);

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static NTSTATUS
FdoSetActive(
    _In_ PXENBUS_FDO                    Fdo
    )
{
    PSTR                                DeviceID;
    PSTR                                InstanceID;
    PSTR                                ActiveDeviceID;
    PSTR                                LocationInformation;
    BOOLEAN                             Present;
    XENBUS_EMULATED_ACTIVATION_STATUS   IsForceActivated;
    NTSTATUS                            status;

    status = FdoQueryId(Fdo,
                        BusQueryDeviceID,
                        &DeviceID);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = FdoQueryId(Fdo,
                        BusQueryInstanceID,
                        &InstanceID);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = FdoQueryDeviceText(Fdo,
                                DeviceTextLocationInformation,
                                &LocationInformation);
    if (!NT_SUCCESS(status))
        goto fail3;

    if (Fdo->EmulatedInterface.Interface.Context == NULL)
        goto fallback;

    status = XENFILT_EMULATED(Acquire, &Fdo->EmulatedInterface);
    if (!NT_SUCCESS(status))
        goto fallback;

    Present = XENFILT_EMULATED(IsDevicePresent,
                               &Fdo->EmulatedInterface,
                               DeviceID,
                               NULL,
                               &IsForceActivated);
    BUG_ON(!Present);

    XENFILT_EMULATED(Release, &Fdo->EmulatedInterface);

    if (IsForceActivated == XENBUS_EMULATED_ACTIVATE_NEUTRAL)
        goto fallback;

    Fdo->Active = IsForceActivated == XENBUS_EMULATED_FORCE_ACTIVATED;
    Info("FDO %s force %sactivated\n", DeviceID, Fdo->Active ? "" : "de");

    if (Fdo->Active)
        (VOID) ConfigSetActive(DeviceID, InstanceID, LocationInformation);

    goto done;

fallback:
    status = ConfigGetActive("DeviceID", &ActiveDeviceID);
    if (NT_SUCCESS(status)) {
        Fdo->Active = (_stricmp(DeviceID, ActiveDeviceID) == 0) ? TRUE : FALSE;

        if (Fdo->Active)
            (VOID) ConfigUpdateActive(DeviceID, InstanceID, LocationInformation);

        ExFreePool(ActiveDeviceID);
    } else {
        status = ConfigSetActive(DeviceID, InstanceID, LocationInformation);
        if (NT_SUCCESS(status))
            Fdo->Active = TRUE;
    }

done:
    ExFreePool(LocationInformation);
    ExFreePool(InstanceID);
    ExFreePool(DeviceID);

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    ExFreePool(InstanceID);

fail2:
    Error("fail2\n");

    ExFreePool(DeviceID);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
FdoClearActive(
    _In_ PXENBUS_FDO    Fdo
    )
{
    (VOID) ConfigClearActive();

    Fdo->Active = FALSE;
}

static FORCEINLINE BOOLEAN
__FdoIsActive(
    _In_ PXENBUS_FDO    Fdo
    )
{
    return Fdo->Active;
}

#define DEFINE_FDO_GET_CONTEXT(_Interface, _Type)               \
static FORCEINLINE _Type                                        \
__FdoGet ## _Interface ## Context(                              \
    _In_ PXENBUS_FDO    Fdo                                     \
    )                                                           \
{                                                               \
    return Fdo-> ## _Interface ## Context;                      \
}                                                               \
                                                                \
_Type                                                           \
FdoGet ## _Interface ## Context(                                \
    _In_ PXENBUS_FDO    Fdo                                     \
    )                                                           \
{                                                               \
    return __FdoGet ## _Interface ## Context(Fdo);              \
}

DEFINE_FDO_GET_CONTEXT(Suspend, PXENBUS_SUSPEND_CONTEXT)
DEFINE_FDO_GET_CONTEXT(SharedInfo, PXENBUS_SHARED_INFO_CONTEXT)
DEFINE_FDO_GET_CONTEXT(Evtchn, PXENBUS_EVTCHN_CONTEXT)
DEFINE_FDO_GET_CONTEXT(Debug, PXENBUS_DEBUG_CONTEXT)
DEFINE_FDO_GET_CONTEXT(Store, PXENBUS_STORE_CONTEXT)
DEFINE_FDO_GET_CONTEXT(Console, PXENBUS_CONSOLE_CONTEXT)
DEFINE_FDO_GET_CONTEXT(RangeSet, PXENBUS_RANGE_SET_CONTEXT)
DEFINE_FDO_GET_CONTEXT(Cache, PXENBUS_CACHE_CONTEXT)
DEFINE_FDO_GET_CONTEXT(Gnttab, PXENBUS_GNTTAB_CONTEXT)
DEFINE_FDO_GET_CONTEXT(Unplug, PXENBUS_UNPLUG_CONTEXT)
DEFINE_FDO_GET_CONTEXT(Balloon, PXENBUS_BALLOON_CONTEXT)

static IO_COMPLETION_ROUTINE FdoDelegateIrpCompletion;

_Use_decl_annotations_
static NTSTATUS
FdoDelegateIrpCompletion(
    PDEVICE_OBJECT      DeviceObject,
    PIRP                Irp,
    PVOID               Context
    )
{
    PKEVENT             Event = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    ASSERT(Event != NULL);

    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
FdoDelegateIrp(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    )
{
    PDEVICE_OBJECT      DeviceObject;
    PIO_STACK_LOCATION  StackLocation;
    PIRP                SubIrp;
    KEVENT              Event;
    PIO_STACK_LOCATION  SubStackLocation;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    // Find the top of the FDO stack and hold a reference
    DeviceObject = IoGetAttachedDeviceReference(Fdo->Dx->DeviceObject);

    // Get a new IRP for the FDO stack
    SubIrp = IoAllocateIrp(DeviceObject->StackSize, FALSE);

    status = STATUS_NO_MEMORY;
    if (SubIrp == NULL)
        goto done;

    // Copy in the information from the original IRP
    SubStackLocation = IoGetNextIrpStackLocation(SubIrp);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    RtlCopyMemory(SubStackLocation, StackLocation,
                  FIELD_OFFSET(IO_STACK_LOCATION, CompletionRoutine));
    SubStackLocation->Control = 0;

    IoSetCompletionRoutine(SubIrp,
                           FdoDelegateIrpCompletion,
                           &Event,
                           TRUE,
                           TRUE,
                           TRUE);

    // Default completion status
    SubIrp->IoStatus.Status = Irp->IoStatus.Status;

    status = IoCallDriver(DeviceObject, SubIrp);
    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject(&Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        status = SubIrp->IoStatus.Status;
    } else {
        ASSERT3U(status, ==, SubIrp->IoStatus.Status);
    }

    IoFreeIrp(SubIrp);

done:
    ObDereferenceObject(DeviceObject);

    return status;
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

    ASSERT(Event != NULL);

    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
FdoForwardIrpSynchronously(
    _In_ PXENBUS_FDO    Fdo,
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

VOID
FdoAddPhysicalDeviceObject(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PXENBUS_PDO    Pdo
    )
{
    PDEVICE_OBJECT      DeviceObject;
    PXENBUS_DX          Dx;

    DeviceObject = PdoGetDeviceObject(Pdo);
    Dx = (PXENBUS_DX)DeviceObject->DeviceExtension;
    ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

    InsertTailList(&Fdo->List, &Dx->ListEntry);
    ASSERT3U(Fdo->References, !=, 0);
    Fdo->References++;

    if (__FdoGetDevicePowerState(Fdo) == PowerDeviceD0)
        PdoResume(Pdo);
}

VOID
FdoRemovePhysicalDeviceObject(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PXENBUS_PDO    Pdo
    )
{
    PDEVICE_OBJECT      DeviceObject;
    PXENBUS_DX          Dx;

    DeviceObject = PdoGetDeviceObject(Pdo);
    Dx = (PXENBUS_DX)DeviceObject->DeviceExtension;
    ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

    if (__FdoGetDevicePowerState(Fdo) == PowerDeviceD0)
        PdoSuspend(Pdo);

    RemoveEntryList(&Dx->ListEntry);
    ASSERT3U(Fdo->References, !=, 0);
    --Fdo->References;

    if (Fdo->ScanThread)
        ThreadWake(Fdo->ScanThread);
}

static FORCEINLINE VOID
__FdoAcquireMutex(
    _In_ PXENBUS_FDO    Fdo
    )
{
    AcquireMutex(&Fdo->Mutex);
}

VOID
FdoAcquireMutex(
    _In_ PXENBUS_FDO    Fdo
    )
{
    __FdoAcquireMutex(Fdo);
}

static FORCEINLINE VOID
__FdoReleaseMutex(
    _In_ PXENBUS_FDO    Fdo
    )
{
    ReleaseMutex(&Fdo->Mutex);
}

VOID
FdoReleaseMutex(
    _In_ PXENBUS_FDO    Fdo
    )
{
    __FdoReleaseMutex(Fdo);

    if (Fdo->References == 0) {
        DriverAcquireMutex();
        FdoDestroy(Fdo);
        DriverReleaseMutex();
    }
}

static BOOLEAN
FdoEnumerate(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PANSI_STRING   Classes
    )
{
    BOOLEAN             NeedInvalidate;
    HANDLE              ParametersKey;
    ULONG               Enumerate;
    PLIST_ENTRY         ListEntry;
    ULONG               Index;
    NTSTATUS            status;

    Trace("====>\n");

    NeedInvalidate = FALSE;

    ParametersKey = DriverGetParametersKey();

    status = RegistryQueryDwordValue(ParametersKey,
                                     "Enumerate",
                                     &Enumerate);
    if (!NT_SUCCESS(status))
        Enumerate = 1;

    if (Enumerate == 0)
        goto done;

    __FdoAcquireMutex(Fdo);

    ListEntry = Fdo->List.Flink;
    while (ListEntry != &Fdo->List) {
        PLIST_ENTRY     Next = ListEntry->Flink;
        PXENBUS_DX      Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO     Pdo = Dx->Pdo;

        if (!PdoIsMissing(Pdo) && PdoGetDevicePnpState(Pdo) != Deleted) {
            PSTR            Name;
            BOOLEAN         Missing;

            Name = PdoGetName(Pdo);
            Missing = TRUE;

            // If the PDO already exists and its name is in the class list
            // then we don't want to remove it.
            for (Index = 0; Classes[Index].Buffer != NULL; Index++) {
                PANSI_STRING Class = &Classes[Index];

                if (Class->Length == 0)
                    continue;

                if (strcmp(Name, Class->Buffer) == 0) {
                    Missing = FALSE;
                    Class->Length = 0;  // avoid duplication
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
                } else {
                    NeedInvalidate = TRUE;
                }
            }
        }

        ListEntry = Next;
    }

    // Walk the class list and create PDOs for any new classes
    for (Index = 0; Classes[Index].Buffer != NULL; Index++) {
        PANSI_STRING Class = &Classes[Index];

        if (Class->Length != 0) {
            status = PdoCreate(Fdo, Class);
            if (NT_SUCCESS(status))
                NeedInvalidate = TRUE;
        }
    }

    __FdoReleaseMutex(Fdo);

done:
    Trace("<====\n");

    return NeedInvalidate;
}

static PANSI_STRING
FdoMultiSzToUpcaseAnsi(
    _In_ PSTR       Buffer
    )
{
    PANSI_STRING    Ansi;
    LONG            Index;
    LONG            Count;
    NTSTATUS        status;

    Index = 0;
    Count = 0;
    for (;;) {
        if (Buffer[Index] == '\0') {
            Count++;
            Index++;

            // Check for double NUL
            if (Buffer[Index] == '\0')
                break;
        } else {
            Buffer[Index] = __toupper(Buffer[Index]);
            Index++;
        }
    }

    Ansi = __FdoAllocate(sizeof (ANSI_STRING) * (Count + 1));

    status = STATUS_NO_MEMORY;
    if (Ansi == NULL)
        goto fail1;

    for (Index = 0; Index < Count; Index++) {
        ULONG   Length;

        Length = (ULONG)strlen(Buffer);
        Ansi[Index].MaximumLength = (USHORT)(Length + 1);
        Ansi[Index].Buffer = __FdoAllocate(Ansi[Index].MaximumLength);

        status = STATUS_NO_MEMORY;
        if (Ansi[Index].Buffer == NULL)
            goto fail2;

        RtlCopyMemory(Ansi[Index].Buffer, Buffer, Length);
        Ansi[Index].Length = (USHORT)Length;

        Buffer += Length + 1;
    }

    return Ansi;

fail2:
    Error("fail2\n");

    while (--Index >= 0)
        __FdoFree(Ansi[Index].Buffer);

    __FdoFree(Ansi);

fail1:
    Error("fail1 (%08x)\n", status);

    return NULL;
}

static VOID
FdoFreeAnsi(
    _In_ PANSI_STRING   Ansi
    )
{
    ULONG               Index;

    for (Index = 0; Ansi[Index].Buffer != NULL; Index++)
        __FdoFree(Ansi[Index].Buffer);

    __FdoFree(Ansi);
}

static PANSI_STRING
FdoCombineAnsi(
    _In_opt_ PANSI_STRING   AnsiA,
    _In_opt_ PANSI_STRING   AnsiB
    )
{
    LONG                    Count;
    ULONG                   Index;
    PANSI_STRING            Ansi;
    NTSTATUS                status;

    Count = 0;

    for (Index = 0;
         AnsiA != NULL && AnsiA[Index].Buffer != NULL;
         Index++)
        Count++;

    for (Index = 0;
         AnsiB != NULL && AnsiB[Index].Buffer != NULL;
         Index++)
        Count++;

    Ansi = __FdoAllocate(sizeof (ANSI_STRING) * (Count + 1));

    status = STATUS_NO_MEMORY;
    if (Ansi == NULL)
        goto fail1;

    Count = 0;

    for (Index = 0;
         AnsiA != NULL && AnsiA[Index].Buffer != NULL;
         Index++) {
        USHORT  Length;

        Length = AnsiA[Index].MaximumLength;

        Ansi[Count].MaximumLength = Length;
        Ansi[Count].Buffer = __FdoAllocate(Length);

        status = STATUS_NO_MEMORY;
        if (Ansi[Count].Buffer == NULL)
            goto fail2;

        RtlCopyMemory(Ansi[Count].Buffer, AnsiA[Index].Buffer, Length);
        Ansi[Count].Length = AnsiA[Index].Length;

        Count++;
    }

    for (Index = 0;
         AnsiB != NULL && AnsiB[Index].Buffer != NULL;
         Index++) {
        USHORT  Length;

        Length = AnsiB[Index].MaximumLength;

        Ansi[Count].MaximumLength = Length;
        Ansi[Count].Buffer = __FdoAllocate(Length);

        status = STATUS_NO_MEMORY;
        if (Ansi[Count].Buffer == NULL)
            goto fail3;

        RtlCopyMemory(Ansi[Count].Buffer, AnsiB[Index].Buffer, Length);
        Ansi[Count].Length = AnsiB[Index].Length;

        Count++;
    }

    return Ansi;

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

    while (--Count >= 0)
        __FdoFree(Ansi[Count].Buffer);

    __FdoFree(Ansi);

fail1:
    Error("fail1 (%08x)\n", status);

    return NULL;
}

static NTSTATUS
FdoScan(
    _In_ PXENBUS_THREAD Self,
    _In_ PVOID          Context
    )
{
    PXENBUS_FDO         Fdo = Context;
    PKEVENT             Event;
    HANDLE              ParametersKey;
    NTSTATUS            status;

    Info("====>\n");

    Event = ThreadGetEvent(Self);

    ParametersKey = DriverGetParametersKey();

    for (;;) {
        PSTR                    Buffer;
        PANSI_STRING            StoreClasses;
        PANSI_STRING            SyntheticClasses;
        PANSI_STRING            SupportedClasses;
        PANSI_STRING            Classes;
        ULONG                   Index;
        BOOLEAN                 NeedInvalidate;

        Trace("waiting...\n");

        (VOID) KeWaitForSingleObject(Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        KeClearEvent(Event);

        Trace("awake\n");

        if (ThreadIsAlerted(Self))
            break;

        // It is not safe to use interfaces before this point
        if (__FdoGetDevicePnpState(Fdo) != Started)
            goto loop;

        status = XENBUS_STORE(Directory,
                              &Fdo->StoreInterface,
                              NULL,
                              NULL,
                              "device",
                              &Buffer);
        if (NT_SUCCESS(status)) {
            StoreClasses = FdoMultiSzToUpcaseAnsi(Buffer);

            XENBUS_STORE(Free,
                         &Fdo->StoreInterface,
                         Buffer);
        } else {
            StoreClasses = NULL;
        }

        if (ParametersKey != NULL) {
            status = RegistryQuerySzValue(ParametersKey,
                                          "SyntheticClasses",
                                          NULL,
                                          &SyntheticClasses);
            if (!NT_SUCCESS(status))
                SyntheticClasses = NULL;
        } else {
            SyntheticClasses = NULL;
        }

        Classes = FdoCombineAnsi(StoreClasses, SyntheticClasses);

        if (StoreClasses != NULL)
            FdoFreeAnsi(StoreClasses);

        if (SyntheticClasses != NULL)
            RegistryFreeSzValue(SyntheticClasses);

        if (Classes == NULL)
            goto loop;

        if (ParametersKey != NULL) {
            status = RegistryQuerySzValue(ParametersKey,
                                          "SupportedClasses",
                                          NULL,
                                          &SupportedClasses);
            if (!NT_SUCCESS(status))
                SupportedClasses = NULL;
        } else {
            SupportedClasses = NULL;
        }

        // NULL out anything in the Classes list that not in the
        // SupportedClasses list
        for (Index = 0; Classes[Index].Buffer != NULL; Index++) {
            PANSI_STRING    Class = &Classes[Index];
            ULONG           Entry;
            BOOLEAN         Supported;

            Supported = FALSE;

            for (Entry = 0;
                 SupportedClasses != NULL && SupportedClasses[Entry].Buffer != NULL;
                 Entry++) {
                if (strncmp(Class->Buffer,
                            SupportedClasses[Entry].Buffer,
                            Class->Length) == 0) {
                    Supported = TRUE;
                    break;
                }
            }

            if (!Supported)
                Class->Length = 0;
        }

        if (SupportedClasses != NULL)
            RegistryFreeSzValue(SupportedClasses);

        NeedInvalidate = FdoEnumerate(Fdo, Classes);

        FdoFreeAnsi(Classes);

        if (NeedInvalidate) {
            NeedInvalidate = FALSE;
            IoInvalidateDeviceRelations(__FdoGetPhysicalDeviceObject(Fdo),
                                        BusRelations);
        }

loop:
        KeSetEvent(&Fdo->ScanEvent, IO_NO_INCREMENT, FALSE);
    }

    KeSetEvent(&Fdo->ScanEvent, IO_NO_INCREMENT, FALSE);

    Info("<====\n");
    return STATUS_SUCCESS;
}

static FORCEINLINE NTSTATUS
__FdoSuspendSetActive(
    _In_ PXENBUS_FDO    Fdo
    )
{
    if (!TryAcquireMutex(&Fdo->BalloonSuspendMutex))
        goto fail1;

    Trace("<===>\n");

    return STATUS_SUCCESS;

fail1:
    return STATUS_UNSUCCESSFUL;
}

static FORCEINLINE VOID
__FdoSuspendClearActive(
    _In_ PXENBUS_FDO    Fdo
    )
{
    ReleaseMutex(&Fdo->BalloonSuspendMutex);

    Trace("<===>\n");

    //
    // We may have missed initiating a balloon
    // whilst suspending/resuming.
    //
    if (Fdo->BalloonInterface.Interface.Context != NULL)
        ThreadWake(Fdo->BalloonThread);
}

static NTSTATUS
FdoSuspend(
    _In_ PXENBUS_THREAD Self,
    _In_ PVOID          Context
    )
{
    PXENBUS_FDO         Fdo = Context;
    GROUP_AFFINITY      Affinity;
    PKEVENT             Event;

    Info("====>\n");

    // We really want to know what CPU this thread will run on
    Affinity.Group = 0;
    Affinity.Mask = (KAFFINITY)1;
    KeSetSystemGroupAffinityThread(&Affinity, NULL);

    (VOID) KeSetPriorityThread(KeGetCurrentThread(),
                               LOW_PRIORITY);

    Event = ThreadGetEvent(Self);

    for (;;) {
        PSTR        Buffer;
        BOOLEAN     Suspend;
        NTSTATUS    status;

        Trace("waiting...\n");

        (VOID) KeWaitForSingleObject(Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        KeClearEvent(Event);

        Trace("awake\n");

        if (ThreadIsAlerted(Self))
            break;

        // It is not safe to use interfaces before this point
        if (__FdoGetDevicePowerState(Fdo) != PowerDeviceD0)
            goto loop;

        status = XENBUS_STORE(Read,
                              &Fdo->StoreInterface,
                              NULL,
                              "control",
                              "shutdown",
                              &Buffer);
        if (NT_SUCCESS(status)) {
            Suspend = (strcmp(Buffer, "suspend") == 0) ? TRUE : FALSE;

            XENBUS_STORE(Free,
                         &Fdo->StoreInterface,
                         Buffer);
        } else {
            Suspend = FALSE;
        }

        if (!Suspend) {
            Trace("nothing to do\n");
            goto loop;
        }

        status = __FdoSuspendSetActive(Fdo);
        if (!NT_SUCCESS(status))
            goto loop;

        (VOID) XENBUS_STORE(Printf,
                            &Fdo->StoreInterface,
                            NULL,
                            "control",
                            "shutdown",
                            "");

        (VOID) XENBUS_SUSPEND(Trigger, &Fdo->SuspendInterface);

        __FdoSuspendClearActive(Fdo);

        KeFlushQueuedDpcs();

loop:
        KeSetEvent(&Fdo->SuspendEvent, IO_NO_INCREMENT, FALSE);
    }

    KeSetEvent(&Fdo->SuspendEvent, IO_NO_INCREMENT, FALSE);

    Info("<====\n");
    return STATUS_SUCCESS;
}

#define TIME_US(_us)            ((_us) * 10ll)
#define TIME_MS(_ms)            (TIME_US((_ms) * 1000ll))
#define TIME_S(_s)              (TIME_MS((_s) * 1000ll))
#define TIME_RELATIVE(_t)       (-(_t))

static FORCEINLINE NTSTATUS
__FdoBalloonSetActive(
    _In_ PXENBUS_FDO        Fdo
    )
{
    if (!TryAcquireMutex(&Fdo->BalloonSuspendMutex))
        goto fail1;

    Trace("<===>\n");

    (VOID) XENBUS_STORE(Printf,
                        &Fdo->StoreInterface,
                        NULL,
                        "control",
                        "balloon-active",
                        "%u",
                        1);

    return STATUS_SUCCESS;

fail1:
    return STATUS_UNSUCCESSFUL;
}

static FORCEINLINE VOID
__FdoBalloonClearActive(
    _In_ PXENBUS_FDO    Fdo
    )
{
    (VOID) XENBUS_STORE(Printf,
                        &Fdo->StoreInterface,
                        NULL,
                        "control",
                        "balloon-active",
                        "%u",
                        0);

    ReleaseMutex(&Fdo->BalloonSuspendMutex);

    Trace("<===>\n");

    //
    // We may have missed initiating a suspend
    // whilst the balloon was active.
    //
    ThreadWake(Fdo->SuspendThread);
}

#define XENBUS_BALLOON_RETRY_PERIOD 1

static NTSTATUS
FdoBalloon(
    _In_ PXENBUS_THREAD Self,
    _In_ PVOID          Context
    )
{
    PXENBUS_FDO         Fdo = Context;
    PKEVENT             Event;
    LARGE_INTEGER       Timeout;
    ULONGLONG           StaticMax;
    BOOLEAN             Initialized;
    BOOLEAN             Active;
    NTSTATUS            status;

    Info("====>\n");

    Event = ThreadGetEvent(Self);

    Timeout.QuadPart = TIME_RELATIVE(TIME_S(XENBUS_BALLOON_RETRY_PERIOD));

    StaticMax = 0;
    Initialized = FALSE;
    Active = FALSE;

    for (;;) {
        PSTR                    Buffer;
        ULONGLONG               Target;
        ULONGLONG               Size;

        Trace("waiting%s...\n", (Active) ? " (Active)" : "");

        (VOID) KeWaitForSingleObject(Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     (Active) ?
                                     &Timeout :
                                     NULL);
        KeClearEvent(Event);

        Trace("awake\n");

        if (ThreadIsAlerted(Self))
            break;

        // It is not safe to use interfaces before this point
        if (__FdoGetDevicePowerState(Fdo) != PowerDeviceD0) {
            if (Active) {
                Active = FALSE;

                __FdoBalloonClearActive(Fdo);
            }

            goto loop;
        }

        if (!Initialized) {
            ULONGLONG   VideoRAM;

            ASSERT(!Active);

            status = XENBUS_STORE(Read,
                                  &Fdo->StoreInterface,
                                  NULL,
                                  "memory",
                                  "static-max",
                                  &Buffer);
            if (!NT_SUCCESS(status))
                goto loop;

            StaticMax = _strtoui64(Buffer, NULL, 10);

            XENBUS_STORE(Free,
                         &Fdo->StoreInterface,
                         Buffer);

            if (StaticMax == 0)
                goto loop;

            status = XENBUS_STORE(Read,
                                  &Fdo->StoreInterface,
                                  NULL,
                                  "memory",
                                  "videoram",
                                  &Buffer);
            if (NT_SUCCESS(status)) {
                VideoRAM = _strtoui64(Buffer, NULL, 10);

                XENBUS_STORE(Free,
                             &Fdo->StoreInterface,
                             Buffer);
            } else {
                VideoRAM = 0;
            }

            if (StaticMax < VideoRAM)
                goto loop;

            StaticMax -= VideoRAM;
            StaticMax /= 4;   // We need the value in pages

            Initialized = TRUE;
        }

        ASSERT(Initialized);

        status = XENBUS_STORE(Read,
                              &Fdo->StoreInterface,
                              NULL,
                              "memory",
                              "target",
                              &Buffer);
        if (!NT_SUCCESS(status))
            goto loop;

        Target = _strtoui64(Buffer, NULL, 10) / 4;

        XENBUS_STORE(Free,
                     &Fdo->StoreInterface,
                     Buffer);

        if (Target > StaticMax)
            Target = StaticMax;

        Size = StaticMax - Target;

        if (XENBUS_BALLOON(GetSize,
                           &Fdo->BalloonInterface) == Size) {
            Trace("nothing to do\n");
            goto loop;
        }

        if (!Active) {
            status = __FdoBalloonSetActive(Fdo);
            if (!NT_SUCCESS(status))
                goto loop;

            Active = TRUE;
        }

        status = XENBUS_BALLOON(Adjust,
                                &Fdo->BalloonInterface,
                                Size);
        if (!NT_SUCCESS(status))
            goto loop;

        ASSERT(Active);
        Active = FALSE;

        __FdoBalloonClearActive(Fdo);

loop:
        if (!Active)
            KeSetEvent(&Fdo->BalloonEvent, IO_NO_INCREMENT, FALSE);
    }

    ASSERT3U(XENBUS_BALLOON(GetSize,
                            &Fdo->BalloonInterface), ==, 0);

    KeSetEvent(&Fdo->BalloonEvent, IO_NO_INCREMENT, FALSE);

    Info("<====\n");
    return STATUS_SUCCESS;
}

static VOID
FdoDumpIoResourceDescriptor(
    _In_ PXENBUS_FDO                Fdo,
    _In_ PIO_RESOURCE_DESCRIPTOR    Descriptor
    )
{
    Trace("%s: %s\n",
          __FdoGetName(Fdo),
          ResourceDescriptorTypeName(Descriptor->Type));

    if (Descriptor->Option == 0)
        Trace("Required\n");
    else if (Descriptor->Option == IO_RESOURCE_ALTERNATIVE)
        Trace("Alternative\n");
    else if (Descriptor->Option == IO_RESOURCE_PREFERRED)
        Trace("Preferred\n");
    else if (Descriptor->Option == (IO_RESOURCE_ALTERNATIVE | IO_RESOURCE_PREFERRED))
        Trace("Preferred Alternative\n");

    Trace("ShareDisposition = %s Flags = %04x\n",
          ResourceDescriptorShareDispositionName(Descriptor->ShareDisposition),
          Descriptor->Flags);

    switch (Descriptor->Type) {
    case CmResourceTypeMemory:
        Trace("Length = %08x Alignment = %08x\n MinimumAddress = %08x.%08x MaximumAddress = %08x.%08x\n",
              Descriptor->u.Memory.Length,
              Descriptor->u.Memory.Alignment,
              Descriptor->u.Memory.MinimumAddress.HighPart,
              Descriptor->u.Memory.MinimumAddress.LowPart,
              Descriptor->u.Memory.MaximumAddress.HighPart,
              Descriptor->u.Memory.MaximumAddress.LowPart);
        break;

    case CmResourceTypeInterrupt:
        Trace("MinimumVector = %08x MaximumVector = %08x AffinityPolicy = %s PriorityPolicy = %s Group = %u TargettedProcessors = %p\n",
              Descriptor->u.Interrupt.MinimumVector,
              Descriptor->u.Interrupt.MaximumVector,
              IrqDevicePolicyName(Descriptor->u.Interrupt.AffinityPolicy),
              IrqPriorityName(Descriptor->u.Interrupt.PriorityPolicy),
              Descriptor->u.Interrupt.Group,
              (PVOID)Descriptor->u.Interrupt.TargetedProcessors);
        break;

    default:
        break;
    }
}

static VOID
FdoDumpIoResourceList(
    _In_ PXENBUS_FDO        Fdo,
    _In_ PIO_RESOURCE_LIST  List
    )
{
    ULONG                   Index;

    for (Index = 0; Index < List->Count; Index++) {
        PIO_RESOURCE_DESCRIPTOR Descriptor = &List->Descriptors[Index];

        Trace("%s: %d\n",
              __FdoGetName(Fdo),
              Index);

        FdoDumpIoResourceDescriptor(Fdo, Descriptor);
    }
}

static NTSTATUS
FdoFilterResourceRequirements(
    _In_ PXENBUS_FDO                Fdo,
    _In_ PIRP                       Irp
    )
{
    PIO_RESOURCE_REQUIREMENTS_LIST  Old;
    ULONG                           Size;
    PIO_RESOURCE_REQUIREMENTS_LIST  New;
    IO_RESOURCE_DESCRIPTOR          Interrupt;
    PIO_RESOURCE_LIST               List;
    ULONG                           Index;
    ULONG                           Count;
    NTSTATUS                        status;

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    if (!__FdoIsActive(Fdo))
        goto not_active;

    Old = (PIO_RESOURCE_REQUIREMENTS_LIST)Irp->IoStatus.Information;
    ASSERT3U(Old->AlternativeLists, ==, 1);

    Count = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);

    Size = Old->ListSize +
        (sizeof (IO_RESOURCE_DESCRIPTOR) * Count);

    New = __AllocatePoolWithTag(PagedPool, Size, 'SUB');

    status = STATUS_NO_MEMORY;
    if (New == NULL)
        goto fail2;

    RtlCopyMemory(New, Old, Old->ListSize);
    New->ListSize = Size;

    List = &New->List[0];

    for (Index = 0; Index < List->Count; Index++) {
        PIO_RESOURCE_DESCRIPTOR Descriptor = &List->Descriptors[Index];

        if (Descriptor->Type != CmResourceTypeInterrupt)
            continue;

        Descriptor->Flags |= CM_RESOURCE_INTERRUPT_POLICY_INCLUDED;
        Descriptor->u.Interrupt.AffinityPolicy = IrqPolicySpecifiedProcessors;
        Descriptor->u.Interrupt.Group = 0;
        Descriptor->u.Interrupt.TargetedProcessors = (KAFFINITY)1;
    }

    RtlZeroMemory(&Interrupt, sizeof (IO_RESOURCE_DESCRIPTOR));
    Interrupt.Option = 0; // Required
    Interrupt.Type = CmResourceTypeInterrupt;
    Interrupt.ShareDisposition = CmResourceShareDeviceExclusive;
    Interrupt.Flags = CM_RESOURCE_INTERRUPT_LATCHED |
                      CM_RESOURCE_INTERRUPT_MESSAGE |
                      CM_RESOURCE_INTERRUPT_POLICY_INCLUDED;

    Interrupt.u.Interrupt.MinimumVector = CM_RESOURCE_INTERRUPT_MESSAGE_TOKEN;
    Interrupt.u.Interrupt.MaximumVector = CM_RESOURCE_INTERRUPT_MESSAGE_TOKEN;
    Interrupt.u.Interrupt.AffinityPolicy = IrqPolicySpecifiedProcessors;
    Interrupt.u.Interrupt.PriorityPolicy = IrqPriorityUndefined;

    for (Index = 0; Index < Count; Index++) {
        PROCESSOR_NUMBER    ProcNumber;

        status = KeGetProcessorNumberFromIndex(Index, &ProcNumber);
        ASSERT(NT_SUCCESS(status));

        if (RtlIsNtDdiVersionAvailable(NTDDI_WIN7))
            Interrupt.u.Interrupt.Group = ProcNumber.Group;

        Interrupt.u.Interrupt.TargetedProcessors = (KAFFINITY)1 << ProcNumber.Number;
        List->Descriptors[List->Count++] = Interrupt;
    }

    FdoDumpIoResourceList(Fdo, List);

    Irp->IoStatus.Information = (ULONG_PTR)New;
    status = STATUS_SUCCESS;

    ExFreePool(Old);

not_active:
    status = Irp->IoStatus.Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static VOID
FdoDumpCmPartialResourceDescriptor(
    _In_ PXENBUS_FDO                        Fdo,
    _In_ BOOLEAN                            Translated,
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR    Descriptor
    )
{
    Trace("%s: %s: %s SharedDisposition=%s Flags=%04x\n",
          __FdoGetName(Fdo),
          (Translated) ? "TRANSLATED" : "RAW",
          ResourceDescriptorTypeName(Descriptor->Type),
          ResourceDescriptorShareDispositionName(Descriptor->ShareDisposition),
          Descriptor->Flags);

    switch (Descriptor->Type) {
    case CmResourceTypeMemory:
        Trace("%s: %s: Start = %08x.%08x Length = %08x\n",
              __FdoGetName(Fdo),
              (Translated) ? "TRANSLATED" : "RAW",
              Descriptor->u.Memory.Start.HighPart,
              Descriptor->u.Memory.Start.LowPart,
              Descriptor->u.Memory.Length);
        break;

    case CmResourceTypeInterrupt:
        if (Descriptor->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) {
            if (Translated)
                Trace("%s: TRANSLATED: Level = %08x Vector = %08x Affinity = %p\n",
                      __FdoGetName(Fdo),
                      Descriptor->u.MessageInterrupt.Translated.Level,
                      Descriptor->u.MessageInterrupt.Translated.Vector,
                      (PVOID)Descriptor->u.MessageInterrupt.Translated.Affinity);
            else
                Trace("%s: RAW: MessageCount = %08x Vector = %08x Affinity = %p\n",
                      __FdoGetName(Fdo),
                      Descriptor->u.MessageInterrupt.Raw.MessageCount,
                      Descriptor->u.MessageInterrupt.Raw.Vector,
                      (PVOID)Descriptor->u.MessageInterrupt.Raw.Affinity);
        } else {
            Trace("%s: %s: Level = %08x Vector = %08x Affinity = %p\n",
                  __FdoGetName(Fdo),
                  (Translated) ? "TRANSLATED" : "RAW",
                  Descriptor->u.Interrupt.Level,
                  Descriptor->u.Interrupt.Vector,
                  (PVOID)Descriptor->u.Interrupt.Affinity);
        }
        break;
    default:
        break;
    }
}

static VOID
FdoDumpCmPartialResourceList(
    _In_ PXENBUS_FDO                Fdo,
    _In_ BOOLEAN                    Translated,
    _In_ PCM_PARTIAL_RESOURCE_LIST  List
    )
{
    ULONG                           Index;

    Trace("%s: %s: Version = %d Revision = %d Count = %d\n",
          __FdoGetName(Fdo),
          (Translated) ? "TRANSLATED" : "RAW",
          List->Version,
          List->Revision,
          List->Count);

    for (Index = 0; Index < List->Count; Index++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor = &List->PartialDescriptors[Index];

        Trace("%s: %s: %d\n",
              __FdoGetName(Fdo),
              (Translated) ? "TRANSLATED" : "RAW",
              Index);

        FdoDumpCmPartialResourceDescriptor(Fdo, Translated, Descriptor);
    }
}

static VOID
FdoDumpCmFullResourceDescriptor(
    _In_ PXENBUS_FDO                    Fdo,
    _In_ BOOLEAN                        Translated,
    _In_ PCM_FULL_RESOURCE_DESCRIPTOR   Descriptor
    )
{
    Trace("%s: %s: InterfaceType = %s BusNumber = %d\n",
          __FdoGetName(Fdo),
          (Translated) ? "TRANSLATED" : "RAW",
          InterfaceTypeName(Descriptor->InterfaceType),
          Descriptor->BusNumber);

    FdoDumpCmPartialResourceList(Fdo, Translated, &Descriptor->PartialResourceList);
}

static VOID
FdoDumpCmResourceList(
    _In_ PXENBUS_FDO        Fdo,
    _In_ BOOLEAN            Translated,
    _In_ PCM_RESOURCE_LIST  List
    )
{
    FdoDumpCmFullResourceDescriptor(Fdo, Translated, &List->List[0]);
}

_IRQL_requires_max_(HIGH_LEVEL)
_IRQL_saves_
_IRQL_raises_(HIGH_LEVEL)
KIRQL
FdoAcquireInterruptLock(
    _In_ PXENBUS_FDO        Fdo,
    _In_ PXENBUS_INTERRUPT  Interrupt
    )
{
    UNREFERENCED_PARAMETER(Fdo);

    return KeAcquireInterruptSpinLock(Interrupt->InterruptObject);
}

_IRQL_requires_(HIGH_LEVEL)
VOID
FdoReleaseInterruptLock(
    _In_ PXENBUS_FDO                Fdo,
    _In_ PXENBUS_INTERRUPT          Interrupt,
    _In_ _IRQL_restores_ KIRQL      Irql
    )
{
    UNREFERENCED_PARAMETER(Fdo);

    KeReleaseInterruptSpinLock(Interrupt->InterruptObject, Irql);
}

static
_Function_class_(KSERVICE_ROUTINE)
_IRQL_requires_(HIGH_LEVEL)
BOOLEAN
FdoInterruptCallback(
    _In_ PKINTERRUPT            InterruptObject,
    _In_ PVOID                  Context
    )
{
    PXENBUS_INTERRUPT           Interrupt = Context;

    if (Interrupt->Callback == NULL)
        return FALSE;

    if (Interrupt->Count++ == 0)
        LogPrintf(LOG_LEVEL_INFO,
                  "XENBUS: %u:%u INTERRUPT\n",
                  Interrupt->ProcNumber.Group,
                  Interrupt->ProcNumber.Number);

    return Interrupt->Callback(InterruptObject,
                               Interrupt->Argument);
}

static NTSTATUS
FdoConnectInterrupt(
    _In_ PXENBUS_FDO                        Fdo,
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR    Raw,
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR    Translated,
    _Outptr_ PXENBUS_INTERRUPT              *Interrupt
    )
{
    IO_CONNECT_INTERRUPT_PARAMETERS         Connect;
    BOOLEAN                                 Found;
    ULONG                                   Number;
    NTSTATUS                                status;

    Trace("====>\n");

    *Interrupt = __FdoAllocate(sizeof (XENBUS_INTERRUPT));

    status = STATUS_NO_MEMORY;
    if (*Interrupt == NULL)
        goto fail1;

    (*Interrupt)->Fdo = Fdo;
    (*Interrupt)->InterruptMode = (Translated->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ?
                                  Latched :
                                  LevelSensitive;

    if (~Translated->Flags & CM_RESOURCE_INTERRUPT_MESSAGE)
        (*Interrupt)->Line = Raw->u.Interrupt.Vector;

    RtlZeroMemory(&Connect, sizeof (IO_CONNECT_INTERRUPT_PARAMETERS));
    Connect.FullySpecified.PhysicalDeviceObject = __FdoGetPhysicalDeviceObject(Fdo);
    Connect.FullySpecified.ShareVector = (BOOLEAN)(Translated->ShareDisposition == CmResourceShareShared);
    Connect.FullySpecified.InterruptMode = (*Interrupt)->InterruptMode;
    Connect.FullySpecified.InterruptObject = &(*Interrupt)->InterruptObject;
    Connect.FullySpecified.ServiceRoutine = FdoInterruptCallback;
    Connect.FullySpecified.ServiceContext = *Interrupt;

    if (Translated->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) {
        Connect.FullySpecified.Vector = Translated->u.MessageInterrupt.Translated.Vector;
        Connect.FullySpecified.Irql = (KIRQL)Translated->u.MessageInterrupt.Translated.Level;
        Connect.FullySpecified.SynchronizeIrql = (KIRQL)Translated->u.MessageInterrupt.Translated.Level;
        Connect.FullySpecified.Group = Translated->u.MessageInterrupt.Translated.Group;
        Connect.FullySpecified.ProcessorEnableMask = Translated->u.MessageInterrupt.Translated.Affinity;
    } else {
        Connect.FullySpecified.Vector = Translated->u.Interrupt.Vector;
        Connect.FullySpecified.Irql = (KIRQL)Translated->u.Interrupt.Level;
        Connect.FullySpecified.SynchronizeIrql = (KIRQL)Translated->u.Interrupt.Level;
        Connect.FullySpecified.Group = Translated->u.Interrupt.Group;
        Connect.FullySpecified.ProcessorEnableMask = Translated->u.Interrupt.Affinity;
    }

    Connect.Version = (Connect.FullySpecified.Group != 0) ?
                      CONNECT_FULLY_SPECIFIED_GROUP :
                      CONNECT_FULLY_SPECIFIED;

    status = IoConnectInterruptEx(&Connect);
    if (!NT_SUCCESS(status))
        goto fail2;

    (*Interrupt)->Vector = (UCHAR)Connect.FullySpecified.Vector;

    (*Interrupt)->ProcNumber.Group = Connect.FullySpecified.Group;

#if defined(__i386__)
    Found = _BitScanReverse(&Number, Connect.FullySpecified.ProcessorEnableMask);
#elif defined(__x86_64__)
    Found = _BitScanReverse64(&Number, Connect.FullySpecified.ProcessorEnableMask);
#else
#error 'Unrecognised architecture'
#endif
    ASSERT(Found);

    (*Interrupt)->ProcNumber.Number = (UCHAR)Number;

    Info("%p: %s %s CPU %u:%u VECTOR %02x\n",
         (*Interrupt)->InterruptObject,
         ResourceDescriptorShareDispositionName(Translated->ShareDisposition),
         InterruptModeName((*Interrupt)->InterruptMode),
         (*Interrupt)->ProcNumber.Group,
         (*Interrupt)->ProcNumber.Number,
         (*Interrupt)->Vector);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    __FdoFree(*Interrupt);
    *Interrupt = NULL;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
FdoDisconnectInterrupt(
    _In_ PXENBUS_FDO                    Fdo,
    _In_ PXENBUS_INTERRUPT              Interrupt
    )
{
    IO_DISCONNECT_INTERRUPT_PARAMETERS  Disconnect;

    UNREFERENCED_PARAMETER(Fdo);

    Trace("====>\n");

    Info("%p: CPU %u:%u VECTOR %02x\n",
         Interrupt->InterruptObject,
         Interrupt->ProcNumber.Group,
         Interrupt->ProcNumber.Number,
         Interrupt->Vector);

    RtlZeroMemory(&Interrupt->ProcNumber, sizeof (PROCESSOR_NUMBER));
    Interrupt->Vector = 0;

    RtlZeroMemory(&Disconnect, sizeof (IO_DISCONNECT_INTERRUPT_PARAMETERS));
    Disconnect.Version = CONNECT_FULLY_SPECIFIED;
    Disconnect.ConnectionContext.InterruptObject = Interrupt->InterruptObject;

    IoDisconnectInterruptEx(&Disconnect);

    Interrupt->Count = 0;

    Interrupt->Line = 0;
    Interrupt->InterruptObject = NULL;
    Interrupt->InterruptMode = 0;
    Interrupt->Fdo = NULL;

    ASSERT(IsZeroMemory(Interrupt, sizeof (XENBUS_INTERRUPT)));
    __FdoFree(Interrupt);

    Trace("<====\n");
}

static NTSTATUS
FdoCreateInterrupt(
    _In_ PXENBUS_FDO    Fdo
    )
{
    ULONG               Index;
    PXENBUS_INTERRUPT   Interrupt;
    NTSTATUS            status;

    InitializeListHead(&Fdo->InterruptList);

    for (Index = 0; Index < Fdo->TranslatedResourceList->Count; Index++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Raw = &Fdo->RawResourceList->PartialDescriptors[Index];
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Translated = &Fdo->TranslatedResourceList->PartialDescriptors[Index];

        if (Translated->Type != CmResourceTypeInterrupt)
            continue;

        status = FdoConnectInterrupt(Fdo, Raw, Translated, &Interrupt);
        if (!NT_SUCCESS(status))
            goto fail1;

        InsertTailList(&Fdo->InterruptList, &Interrupt->ListEntry);
    }

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    while (!IsListEmpty(&Fdo->InterruptList)) {
        PLIST_ENTRY ListEntry;

        ListEntry = RemoveHeadList(&Fdo->InterruptList);
        ASSERT(ListEntry != &Fdo->InterruptList);

        RtlZeroMemory(ListEntry, sizeof (LIST_ENTRY));

        Interrupt = CONTAINING_RECORD(ListEntry, XENBUS_INTERRUPT, ListEntry);

        FdoDisconnectInterrupt(Fdo, Interrupt);
    }

    RtlZeroMemory(&Fdo->InterruptList, sizeof (LIST_ENTRY));

    return status;
}

PXENBUS_INTERRUPT
FdoAllocateInterrupt(
    _In_ PXENBUS_FDO        Fdo,
    _In_ KINTERRUPT_MODE    InterruptMode,
    _In_ USHORT             Group,
    _In_ UCHAR              Number,
    _In_ KSERVICE_ROUTINE   Callback,
    _In_opt_ PVOID          Argument
    )
{
    PLIST_ENTRY             ListEntry;
    PXENBUS_INTERRUPT       Interrupt;
    KIRQL                   Irql;

    for (ListEntry = Fdo->InterruptList.Flink;
         ListEntry != &Fdo->InterruptList;
         ListEntry = ListEntry->Flink) {
        Interrupt = CONTAINING_RECORD(ListEntry, XENBUS_INTERRUPT, ListEntry);

        if (Interrupt->Callback == NULL &&
            Interrupt->InterruptMode == InterruptMode &&
            Interrupt->ProcNumber.Group == Group &&
            Interrupt->ProcNumber.Number == Number)
            goto found;
    }

    goto fail1;

found:
    Irql = FdoAcquireInterruptLock(Fdo, Interrupt);
    Interrupt->Callback = Callback;
    Interrupt->Argument = Argument;
    FdoReleaseInterruptLock(Fdo, Interrupt, Irql);

    return Interrupt;

fail1:
    return NULL;
}

UCHAR
FdoGetInterruptVector(
    _In_ PXENBUS_FDO        Fdo,
    _In_ PXENBUS_INTERRUPT  Interrupt
    )
{
    UNREFERENCED_PARAMETER(Fdo);

    return Interrupt->Vector;
}

ULONG
FdoGetInterruptLine(
    _In_ PXENBUS_FDO        Fdo,
    _In_ PXENBUS_INTERRUPT  Interrupt
    )
{
    UNREFERENCED_PARAMETER(Fdo);

    return Interrupt->Line;
}

VOID
FdoFreeInterrupt(
    _In_ PXENBUS_FDO        Fdo,
    _In_ PXENBUS_INTERRUPT  Interrupt
    )
{
    KIRQL                   Irql;

    Irql = FdoAcquireInterruptLock(Fdo, Interrupt);
    Interrupt->Callback = NULL;
    Interrupt->Argument = NULL;
    FdoReleaseInterruptLock(Fdo, Interrupt, Irql);
}

static VOID
FdoDestroyInterrupt(
    _In_ PXENBUS_FDO    Fdo
    )
{
    while (!IsListEmpty(&Fdo->InterruptList)) {
        PLIST_ENTRY         ListEntry;
        PXENBUS_INTERRUPT   Interrupt;

        ListEntry = RemoveHeadList(&Fdo->InterruptList);
        ASSERT(ListEntry != &Fdo->InterruptList);

        RtlZeroMemory(ListEntry, sizeof (LIST_ENTRY));

        Interrupt = CONTAINING_RECORD(ListEntry, XENBUS_INTERRUPT, ListEntry);

#pragma warning(push)
#pragma warning(disable:4054)   // 'type cast' : from function pointer to data pointer
        ASSERT3P(Interrupt->Callback, ==, NULL);
#pragma warning(pop)

        ASSERT3P(Interrupt->Argument, ==, NULL);

        FdoDisconnectInterrupt(Fdo, Interrupt);
    }

    RtlZeroMemory(&Fdo->InterruptList, sizeof (LIST_ENTRY));
}

static FORCEINLINE BOOLEAN
__FdoMatchDistribution(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PSTR           Buffer
    )
{
    PSTR                Vendor;
    PSTR                Product;
    PSTR                Context;
    PCSTR               Text;
    BOOLEAN             Match;
    ULONG               Index;
    NTSTATUS            status;

    UNREFERENCED_PARAMETER(Fdo);

    status = STATUS_INVALID_PARAMETER;

    Vendor = __strtok_r(Buffer, " ", &Context);
    if (Vendor == NULL)
        goto fail1;

    Product = __strtok_r(NULL, " ", &Context);
    if (Product == NULL)
        goto fail2;

    Match = TRUE;

    Text = VENDOR_NAME_STR;

    for (Index = 0; Text[Index] != 0; Index++) {
        if (!isalnum((UCHAR)Text[Index])) {
            if (Vendor[Index] != '_') {
                Match = FALSE;
                break;
            }
        } else {
            if (Vendor[Index] != Text[Index]) {
                Match = FALSE;
                break;
            }
        }
    }

    Text = "XENBUS";

    if (_stricmp(Product, Text) != 0)
        Match = FALSE;

    return Match;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return FALSE;
}

static VOID
FdoClearDistribution(
    _In_ PXENBUS_FDO    Fdo
    )
{
    PSTR                Buffer;
    PANSI_STRING        Distributions;
    ULONG               Index;
    NTSTATUS            status;

    Trace("====>\n");

    status = XENBUS_STORE(Directory,
                          &Fdo->StoreInterface,
                          NULL,
                          NULL,
                          "drivers",
                          &Buffer);
    if (NT_SUCCESS(status)) {
        Distributions = FdoMultiSzToUpcaseAnsi(Buffer);

        XENBUS_STORE(Free,
                     &Fdo->StoreInterface,
                     Buffer);
    } else {
        Distributions = NULL;
    }

    if (Distributions == NULL)
        goto done;

    for (Index = 0; Distributions[Index].Buffer != NULL; Index++) {
        PANSI_STRING    Distribution = &Distributions[Index];

        status = XENBUS_STORE(Read,
                              &Fdo->StoreInterface,
                              NULL,
                              "drivers",
                              Distribution->Buffer,
                              &Buffer);
        if (!NT_SUCCESS(status))
            continue;

        if (__FdoMatchDistribution(Fdo, Buffer))
            (VOID) XENBUS_STORE(Remove,
                                &Fdo->StoreInterface,
                                NULL,
                                "drivers",
                                Distribution->Buffer);

        XENBUS_STORE(Free,
                     &Fdo->StoreInterface,
                     Buffer);
    }

    FdoFreeAnsi(Distributions);

done:
    Trace("<====\n");
}

#define MAXIMUM_INDEX   255

static NTSTATUS
FdoSetDistribution(
    _In_ PXENBUS_FDO    Fdo
    )
{
    ULONG               Index;
    CHAR                Distribution[MAXNAMELEN];
    CHAR                Vendor[MAXNAMELEN];
    PCSTR               Product;
    NTSTATUS            status;

    Trace("====>\n");

    Index = 0;
    while (Index <= MAXIMUM_INDEX) {
        PSTR    Buffer;

        status = RtlStringCbPrintfA(Distribution,
                                    MAXNAMELEN,
                                    "%u",
                                    Index);
        ASSERT(NT_SUCCESS(status));

        status = XENBUS_STORE(Read,
                              &Fdo->StoreInterface,
                              NULL,
                              "drivers",
                              Distribution,
                              &Buffer);
        if (!NT_SUCCESS(status)) {
            if (status == STATUS_OBJECT_NAME_NOT_FOUND)
                goto update;

            goto fail1;
        }

        XENBUS_STORE(Free,
                     &Fdo->StoreInterface,
                     Buffer);

        Index++;
    }

    status = STATUS_UNSUCCESSFUL;
    goto fail2;

update:
    status = RtlStringCbPrintfA(Vendor,
                                MAXNAMELEN,
                                "%s",
                                VENDOR_NAME_STR);
    ASSERT(NT_SUCCESS(status));

    for (Index  = 0; Vendor[Index] != '\0'; Index++)
        if (!isalnum((UCHAR)Vendor[Index]))
            Vendor[Index] = '_';

    Product = "XENBUS";

#if DBG
#define ATTRIBUTES   "(DEBUG)"
#else
#define ATTRIBUTES   ""
#endif

    (VOID) XENBUS_STORE(Printf,
                        &Fdo->StoreInterface,
                        NULL,
                        "drivers",
                        Distribution,
                        "%s %s %u.%u.%u.%u %s",
                        Vendor,
                        Product,
                        MAJOR_VERSION,
                        MINOR_VERSION,
                        MICRO_VERSION,
                        BUILD_NUMBER,
                        ATTRIBUTES
                        );

#undef  ATTRIBUTES

    Trace("<====\n");
    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

#define FDO_OUT_BUFFER_SIZE 1024

CHAR FdoOutBuffer[FDO_OUT_BUFFER_SIZE];

static VOID
FdoOutputBuffer(
    _In_ PVOID  Argument,
    _In_ PSTR   Buffer,
    _In_ ULONG  Length
    )
{
    PXENBUS_FDO Fdo = Argument;
    ULONG       Index;
    PSTR        Cursor;

    Cursor = FdoOutBuffer;
    for (Index = 0; Index < Length; Index++) {
        if (Cursor - FdoOutBuffer >= FDO_OUT_BUFFER_SIZE)
            break;

        *Cursor++ = Buffer[Index];

        if (Buffer[Index] != '\n')
            continue;

        if (Cursor - FdoOutBuffer >= FDO_OUT_BUFFER_SIZE)
            break;

        *(Cursor - 1) = '\r';
        *Cursor++ = '\n';
    }

    (VOID) XENBUS_CONSOLE(Write,
                          &Fdo->ConsoleInterface,
                          FdoOutBuffer,
                          (ULONG)(Cursor - FdoOutBuffer));
}

static FORCEINLINE BOOLEAN
__FdoVirqPatWatchdog(
    _In_ PXENBUS_VIRQ   Virq
    )
{
    PXENBUS_FDO         Fdo = Virq->Fdo;
    ULONG               Cpu;
    ULONG               Count;
    BOOLEAN             Pat;
    KIRQL               Irql;
    PLIST_ENTRY         ListEntry;

    AcquireHighLock(&Fdo->VirqLock, &Irql);

    Cpu = Virq->Cpu;
    Count = Virq->Count++;
    Pat = TRUE;

    if (Virq->Count == 0) // wrapped
        goto out;

    for (ListEntry = Fdo->VirqList.Flink;
         ListEntry != &Fdo->VirqList;
         ListEntry = ListEntry->Flink) {
        Virq = CONTAINING_RECORD(ListEntry, XENBUS_VIRQ, ListEntry);

        if (Virq->Type != VIRQ_TIMER || Virq->Cpu == Cpu)
            continue;

        if (Virq->Count <= Count)
            Pat = FALSE;
    }

out:
    ReleaseHighLock(&Fdo->VirqLock, Irql);

    return Pat;
}

static KSERVICE_ROUTINE FdoVirqCallback;

_Use_decl_annotations_
static BOOLEAN
FdoVirqCallback(
    PKINTERRUPT         InterruptObject,
    PVOID               Argument
    )
{
    PXENBUS_VIRQ        Virq = Argument;
    PXENBUS_FDO         Fdo;

    UNREFERENCED_PARAMETER(InterruptObject);

    ASSERT(Virq != NULL);
    Fdo = Virq->Fdo;

    switch (Virq->Type) {
    case VIRQ_DEBUG:
        Virq->Count++;
        XENBUS_DEBUG(Trigger, &Fdo->DebugInterface, NULL);
        break;

    case VIRQ_TIMER:
        if (__FdoVirqPatWatchdog(Virq))
            SystemSetWatchdog(Fdo->Watchdog);

        break;

    default:
        ASSERT(FALSE);
        break;
    }

    return TRUE;
}

static FORCEINLINE VOID
__FdoVirqDestroy(
    _In_ PXENBUS_VIRQ   Virq
    )
{
    PXENBUS_FDO         Fdo = Virq->Fdo;

    Info("%s\n", VirqName(Virq->Type));

    if (Virq->Type == VIRQ_TIMER) {
        unsigned int    vcpu_id;
        NTSTATUS        status;

        status = SystemProcessorVcpuId(Virq->Cpu, &vcpu_id);
        ASSERT(NT_SUCCESS(status));

        (VOID) VcpuSetPeriodicTimer(vcpu_id, NULL);
    }

    XENBUS_EVTCHN(Close,
                  &Fdo->EvtchnInterface,
                  Virq->Channel);

    __FdoFree(Virq);
}

static FORCEINLINE NTSTATUS
__FdoVirqCreate(
    _In_ PXENBUS_FDO        Fdo,
    _In_ ULONG              Type,
    _In_ ULONG              Cpu,
    _Outptr_ PXENBUS_VIRQ   *Virq
    )
{
    PROCESSOR_NUMBER        ProcNumber;
    unsigned int            vcpu_id;
    NTSTATUS                status;

    *Virq = __FdoAllocate(sizeof (XENBUS_VIRQ));

    status = STATUS_NO_MEMORY;
    if (*Virq == NULL)
        goto fail1;

    (*Virq)->Fdo = Fdo;
    (*Virq)->Type = Type;
    (*Virq)->Cpu = Cpu;

    status = KeGetProcessorNumberFromIndex(Cpu, &ProcNumber);
    ASSERT(NT_SUCCESS(status));

    (*Virq)->Channel = XENBUS_EVTCHN(Open,
                                     &Fdo->EvtchnInterface,
                                     XENBUS_EVTCHN_TYPE_VIRQ,
                                     FdoVirqCallback,
                                     *Virq,
                                     Type,
                                     ProcNumber.Group,
                                     ProcNumber.Number);

    status = STATUS_UNSUCCESSFUL;
    if ((*Virq)->Channel == NULL)
        goto fail2;

    if (Type == VIRQ_TIMER) {
        LARGE_INTEGER   Period;

        status = SystemProcessorVcpuId(Cpu, &vcpu_id);
        ASSERT(NT_SUCCESS(status));

        BUG_ON(Fdo->Watchdog == 0);
        Period.QuadPart = TIME_S(Fdo->Watchdog / 2);

        status = VcpuSetPeriodicTimer(vcpu_id, &Period);
        if (!NT_SUCCESS(status))
            goto fail3;
    }

    (VOID) XENBUS_EVTCHN(Unmask,
                         &Fdo->EvtchnInterface,
                         (*Virq)->Channel,
                         FALSE,
                         TRUE);

    Info("%s: CPU %u:%u\n", VirqName((*Virq)->Type),
         ProcNumber.Group, ProcNumber.Number);

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    XENBUS_EVTCHN(Close,
                  &Fdo->EvtchnInterface,
                  (*Virq)->Channel);

fail2:
    Error("fail2\n");

    __FdoFree(*Virq);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
FdoVirqTeardown(
    _In_ PXENBUS_FDO    Fdo
    )
{
    if (Fdo->Watchdog != 0)
        SystemStopWatchdog();

    while (!IsListEmpty(&Fdo->VirqList)) {
        PLIST_ENTRY     ListEntry;
        PXENBUS_VIRQ    Virq;

        ListEntry = RemoveHeadList(&Fdo->VirqList);
        ASSERT(ListEntry != &Fdo->VirqList);

        RtlZeroMemory(ListEntry, sizeof (LIST_ENTRY));

        Virq = CONTAINING_RECORD(ListEntry, XENBUS_VIRQ, ListEntry);

        __FdoVirqDestroy(Virq);
    }

    RtlZeroMemory(&Fdo->VirqLock, sizeof (HIGH_LOCK));
    RtlZeroMemory(&Fdo->VirqList, sizeof (LIST_ENTRY));
}

static NTSTATUS
FdoVirqInitialize(
    _In_ PXENBUS_FDO    Fdo
    )
{
    PXENBUS_VIRQ        Virq;
    ULONG               Count;
    ULONG               Index;
    ULONG               Timer;
    NTSTATUS            status;

    InitializeListHead(&Fdo->VirqList);
    InitializeHighLock(&Fdo->VirqLock);

    status = __FdoVirqCreate(Fdo, VIRQ_DEBUG, 0, &Virq);
    if (!NT_SUCCESS(status))
        goto fail1;

    InsertTailList(&Fdo->VirqList, &Virq->ListEntry);

    if (Fdo->Watchdog == 0)
        goto done;

    Count = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);

    Timer = 0;
    for (Index = 0; Index < Count; Index++) {
        status = __FdoVirqCreate(Fdo, VIRQ_TIMER, Index, &Virq);
        if (!NT_SUCCESS(status))
            continue;

        InsertTailList(&Fdo->VirqList, &Virq->ListEntry);
        Timer++;
    }

    if (Timer != 0) {
        status = SystemSetWatchdog(Fdo->Watchdog);
        if (!NT_SUCCESS(status))
            goto fail2;
    }

done:
    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    FdoVirqTeardown(Fdo);

    return status;
}

static FORCEINLINE NTSTATUS
__FdoD3ToD0(
    _In_ PXENBUS_FDO    Fdo
    )
{
    NTSTATUS            status;

    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);

    (VOID) FdoSetDistribution(Fdo);

    status = FdoVirqInitialize(Fdo);
    if (!NT_SUCCESS(status))
        goto fail1;

    if (Fdo->ConsoleAcquired) {
        status = LogAddDisposition(DriverGetConsoleLogLevel(),
                                   FdoOutputBuffer,
                                   Fdo,
                                   &Fdo->LogDisposition);
        ASSERT(NT_SUCCESS(status));
    }

    status = XENBUS_STORE(WatchAdd,
                          &Fdo->StoreInterface,
                          NULL,
                          "device",
                          ThreadGetEvent(Fdo->ScanThread),
                          &Fdo->ScanWatch);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = XENBUS_STORE(WatchAdd,
                          &Fdo->StoreInterface,
                          "control",
                          "shutdown",
                          ThreadGetEvent(Fdo->SuspendThread),
                          &Fdo->SuspendWatch);
    if (!NT_SUCCESS(status))
        goto fail3;

    (VOID) XENBUS_STORE(Printf,
                        &Fdo->StoreInterface,
                        NULL,
                        "control",
                        "feature-suspend",
                        "%u",
                        1);

    if (Fdo->BalloonInterface.Interface.Context != NULL) {
        status = XENBUS_STORE(WatchAdd,
                              &Fdo->StoreInterface,
                              "memory",
                              "target",
                              ThreadGetEvent(Fdo->BalloonThread),
                              &Fdo->BalloonWatch);
        if (!NT_SUCCESS(status))
            goto fail4;

        (VOID) XENBUS_STORE(Printf,
                            &Fdo->StoreInterface,
                            NULL,
                            "control",
                            "feature-balloon",
                            "%u",
                            1);
    }

    Trace("<====\n");

    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

    (VOID) XENBUS_STORE(Remove,
                        &Fdo->StoreInterface,
                        NULL,
                        "control",
                        "feature-suspend");

    (VOID) XENBUS_STORE(WatchRemove,
                        &Fdo->StoreInterface,
                        Fdo->SuspendWatch);
    Fdo->SuspendWatch = NULL;

fail3:
    Error("fail3\n");

    (VOID) XENBUS_STORE(WatchRemove,
                        &Fdo->StoreInterface,
                        Fdo->ScanWatch);
    Fdo->ScanWatch = NULL;

fail2:
    Error("fail2\n");

    if (Fdo->ConsoleAcquired) {
        LogRemoveDisposition(Fdo->LogDisposition);
        Fdo->LogDisposition = NULL;
    }

    FdoVirqTeardown(Fdo);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE VOID
__FdoD0ToD3(
    _In_ PXENBUS_FDO    Fdo
    )
{
    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);

    if (Fdo->BalloonInterface.Interface.Context != NULL) {
        (VOID) XENBUS_STORE(Remove,
                            &Fdo->StoreInterface,
                            NULL,
                            "control",
                            "feature-balloon");

        (VOID) XENBUS_STORE(WatchRemove,
                            &Fdo->StoreInterface,
                            Fdo->BalloonWatch);
        Fdo->BalloonWatch = NULL;
    }

    (VOID) XENBUS_STORE(Remove,
                        &Fdo->StoreInterface,
                        NULL,
                        "control",
                        "feature-suspend");

    (VOID) XENBUS_STORE(WatchRemove,
                        &Fdo->StoreInterface,
                        Fdo->SuspendWatch);
    Fdo->SuspendWatch = NULL;

    (VOID) XENBUS_STORE(WatchRemove,
                        &Fdo->StoreInterface,
                        Fdo->ScanWatch);
    Fdo->ScanWatch = NULL;

    if (Fdo->ConsoleAcquired) {
        LogRemoveDisposition(Fdo->LogDisposition);
        Fdo->LogDisposition = NULL;
    }

    FdoVirqTeardown(Fdo);

    FdoClearDistribution(Fdo);

    Trace("<====\n");
}

static VOID
FdoSuspendCallbackLate(
    _In_ PVOID  Argument
    )
{
    PXENBUS_FDO Fdo = Argument;
    NTSTATUS    status;

    __FdoD0ToD3(Fdo);

    status = __FdoD3ToD0(Fdo);
    ASSERT(NT_SUCCESS(status));
}

static NTSTATUS
FdoPciHoleCreate(
    _In_ PXENBUS_FDO                Fdo
    )
{
    PXENBUS_PCI_HOLE                Hole = &Fdo->PciHole;
    ULONG                           Index;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Translated;
    PFN_NUMBER                      Pfn;
    NTSTATUS                        status;

    status = XENBUS_RANGE_SET(Create,
                              &Fdo->RangeSetInterface,
                              "PCI",
                              &Hole->RangeSet);
    if (!NT_SUCCESS(status))
        goto fail1;

    for (Index = 0; Index < Fdo->TranslatedResourceList->Count; Index++) {
        Translated = &Fdo->TranslatedResourceList->PartialDescriptors[Index];

        if (Translated->Type == CmResourceTypeMemory)
            goto found;
    }

    status = STATUS_OBJECT_NAME_NOT_FOUND;
    goto fail2;

found:
    Hole->VirtualAddress = MmMapIoSpace(Translated->u.Memory.Start,
                                        Translated->u.Memory.Length,
                                        MmCached);

    status = STATUS_UNSUCCESSFUL;
    if (Hole->VirtualAddress == NULL)
        goto fail3;

    Hole->PhysicalAddress = Translated->u.Memory.Start;
    Hole->Count = (ULONG)(Translated->u.Memory.Length >> PAGE_SHIFT);

    status = XENBUS_RANGE_SET(Put,
                              &Fdo->RangeSetInterface,
                              Hole->RangeSet,
                              0,
                              Hole->Count);
    if (!NT_SUCCESS(status))
        goto fail4;

    Pfn = (PFN_NUMBER)(Hole->PhysicalAddress.QuadPart >> PAGE_SHIFT);
    Info("%08x - %08x\n",
         Pfn,
         Pfn + Hole->Count - 1);

    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

    MmUnmapIoSpace(Hole->VirtualAddress, Hole->Count << PAGE_SHIFT);

    Hole->VirtualAddress = NULL;
    Hole->Count = 0;
    Hole->PhysicalAddress.QuadPart = 0;

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

    XENBUS_RANGE_SET(Destroy,
                     &Fdo->RangeSetInterface,
                     Hole->RangeSet);
    Hole->RangeSet = NULL;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
FdoPciHoleDestroy(
    _In_ PXENBUS_FDO    Fdo
    )
{
    PXENBUS_PCI_HOLE    Hole = &Fdo->PciHole;
    NTSTATUS            status;

    BUG_ON(Hole->Count == 0);

    status = XENBUS_RANGE_SET(Get,
                              &Fdo->RangeSetInterface,
                              Hole->RangeSet,
                              0,
                              Hole->Count);
    ASSERT(NT_SUCCESS(status));

    MmUnmapIoSpace(Hole->VirtualAddress, Hole->Count << PAGE_SHIFT);

    Hole->VirtualAddress = NULL;
    Hole->Count = 0;
    Hole->PhysicalAddress.QuadPart = 0;

    XENBUS_RANGE_SET(Destroy,
                     &Fdo->RangeSetInterface,
                     Hole->RangeSet);
    Hole->RangeSet = NULL;
}

static PMDL
FdoPciHoleAllocate(
    _In_ PXENBUS_FDO        Fdo,
    _In_ ULONG              Count
    )
{
    PXENBUS_PCI_HOLE        Hole = &Fdo->PciHole;
    LONGLONG                Index;
    PVOID                   VirtualAddress;
    PHYSICAL_ADDRESS        PhysicalAddress;
    PMDL                    Mdl;
    PPFN_NUMBER             PfnArray;
    NTSTATUS                status;

    BUG_ON(Hole->Count == 0);

    status = XENBUS_RANGE_SET(Pop,
                              &Fdo->RangeSetInterface,
                              Hole->RangeSet,
                              Count,
                              &Index);
    if (!NT_SUCCESS(status))
        goto fail1;

    VirtualAddress = (PUCHAR)Hole->VirtualAddress + (Index << PAGE_SHIFT);

    Mdl = IoAllocateMdl(VirtualAddress,
                        Count << PAGE_SHIFT,
                        FALSE,
                        FALSE,
                        NULL);

    status = STATUS_NO_MEMORY;
    if (Mdl == NULL)
        goto fail2;

    ASSERT3P(Mdl->StartVa, ==, VirtualAddress);
    ASSERT3U(Mdl->ByteCount, ==, Count << PAGE_SHIFT);

    PhysicalAddress.QuadPart = Hole->PhysicalAddress.QuadPart + (Index << PAGE_SHIFT);

    PfnArray = MmGetMdlPfnArray(Mdl);
    PfnArray[0] = (PFN_NUMBER)(PhysicalAddress.QuadPart >> PAGE_SHIFT);

    for (Index = 0; Index < (LONGLONG)Count; Index++)
        PfnArray[Index] = PfnArray[0] + (ULONG)Index;

    return Mdl;

fail2:
    Error("fail2\n");

    (VOID) XENBUS_RANGE_SET(Put,
                            &Fdo->RangeSetInterface,
                            Hole->RangeSet,
                            Index,
                            Count);

fail1:
    Error("fail1 (%08x)\n", status);

    return NULL;
}

static VOID
FdoPciHoleFree(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PMDL           Mdl
    )
{
    PXENBUS_PCI_HOLE    Hole = &Fdo->PciHole;
    ULONG               Count;
    PPFN_NUMBER         PfnArray;
    LONGLONG            Index;
    PHYSICAL_ADDRESS    PhysicalAddress;
    NTSTATUS            status;

    BUG_ON(Hole->Count == 0);

    Count = Mdl->ByteCount >> PAGE_SHIFT;
    ASSERT3U(Count, <, Hole->Count);

    PfnArray = MmGetMdlPfnArray(Mdl);

    // Verify that the PFNs are contiguous
    for (Index = 0; Index < (LONGLONG)Count; Index++)
        BUG_ON(PfnArray[Index] != PfnArray[0] + Index);

    PhysicalAddress.QuadPart = PfnArray[0] << PAGE_SHIFT;

    Index = (PhysicalAddress.QuadPart - Hole->PhysicalAddress.QuadPart) >> PAGE_SHIFT;

    ASSERT3U(Index, <, Hole->Count);
    ASSERT3U(Index + Count, <=, Hole->Count);

    status = XENBUS_RANGE_SET(Put,
                              &Fdo->RangeSetInterface,
                              Hole->RangeSet,
                              Index,
                              Count);
    ASSERT(NT_SUCCESS(status));

    ExFreePool(Mdl);
}

static PMDL
FdoMemoryHoleAllocate(
    _In_ PXENBUS_FDO    Fdo,
    _In_ ULONG          Count
    )
{
    PMDL                Mdl;
    PPFN_NUMBER         PfnArray;
    NTSTATUS            status;

    UNREFERENCED_PARAMETER(Fdo);

    Mdl = __AllocatePages(Count, TRUE);

    status = STATUS_NO_MEMORY;
    if (Mdl == NULL)
        goto fail1;

    PfnArray = MmGetMdlPfnArray(Mdl);

    status = STATUS_UNSUCCESSFUL;
    if (MemoryDecreaseReservation(PAGE_ORDER_4K, Count, PfnArray) != Count)
        goto fail2;

    return Mdl;

fail2:
    Error("fail2\n");

    __FreePages(Mdl);

fail1:
    Error("fail1 (%08x)\n", status);

    return NULL;
}

static VOID
FdoMemoryHoleFree(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PMDL           Mdl
    )
{
    ULONG               Count;
    PPFN_NUMBER         PfnArray;

    UNREFERENCED_PARAMETER(Fdo);

    Count = Mdl->ByteCount >> PAGE_SHIFT;
    PfnArray = MmGetMdlPfnArray(Mdl);

    if (MemoryPopulatePhysmap(PAGE_ORDER_4K, Count, PfnArray) != Count)
        BUG("FAILED TO RE-POPULATE HOLE");

    __FreePages(Mdl);
}

PMDL
FdoHoleAllocate(
    _In_ PXENBUS_FDO    Fdo,
    _In_ ULONG      Count
    )
{
    return (Fdo->UseMemoryHole != 0) ?
        FdoMemoryHoleAllocate(Fdo, Count) :
        FdoPciHoleAllocate(Fdo, Count);
}

VOID
FdoHoleFree(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PMDL       Mdl
    )
{
    if (Fdo->UseMemoryHole != 0)
        FdoMemoryHoleFree(Fdo, Mdl);
    else
        FdoPciHoleFree(Fdo, Mdl);
}


static VOID
FdoDebugCallback(
    _In_ PVOID      Argument,
    _In_ BOOLEAN    Crashing
    )
{
    PXENBUS_FDO     Fdo = Argument;

    UNREFERENCED_PARAMETER(Crashing);

    if (!IsListEmpty(&Fdo->VirqList)) {
        PLIST_ENTRY ListEntry;

        XENBUS_DEBUG(Printf,
                     &Fdo->DebugInterface,
                     "VIRQS:\n");

        for (ListEntry = Fdo->VirqList.Flink;
             ListEntry != &Fdo->VirqList;
             ListEntry = ListEntry->Flink) {
            PXENBUS_VIRQ        Virq;
            PROCESSOR_NUMBER    ProcNumber;
            NTSTATUS            status;

            Virq = CONTAINING_RECORD(ListEntry, XENBUS_VIRQ, ListEntry);

            status = KeGetProcessorNumberFromIndex(Virq->Cpu, &ProcNumber);
            ASSERT(NT_SUCCESS(status));

            XENBUS_DEBUG(Printf,
                         &Fdo->DebugInterface,
                         "- %s: (%u:%u) Count = %u\n",
                         VirqName(Virq->Type),
                         ProcNumber.Group,
                         ProcNumber.Number,
                         Virq->Count);
        }
    }
}

// This function must not touch pageable code or data
static NTSTATUS
FdoD3ToD0(
    _In_ PXENBUS_FDO            Fdo
    )
{
    POWER_STATE                 PowerState;
    KIRQL                       Irql;
    PLIST_ENTRY                 ListEntry;
    NTSTATUS                    status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);
    ASSERT3U(__FdoGetDevicePowerState(Fdo), ==, PowerDeviceD3);

    Trace("====>\n");

    if (!__FdoIsActive(Fdo))
        goto not_active;

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    status = XENBUS_DEBUG(Acquire, &Fdo->DebugInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = XENBUS_SUSPEND(Acquire, &Fdo->SuspendInterface);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = XENBUS_RANGE_SET(Acquire, &Fdo->RangeSetInterface);
    if (!NT_SUCCESS(status))
        goto fail3;

    if (Fdo->UseMemoryHole == 0) {
        status = FdoPciHoleCreate(Fdo);
        if (!NT_SUCCESS(status))
            goto fail4;
    }

    status = XENBUS_EVTCHN(Acquire, &Fdo->EvtchnInterface);
    if (!NT_SUCCESS(status))
        goto fail5;

    status = XENBUS_STORE(Acquire, &Fdo->StoreInterface);
    if (!NT_SUCCESS(status))
        goto fail6;

    status = XENBUS_CONSOLE(Acquire, &Fdo->ConsoleInterface);
    if (NT_SUCCESS(status))
        Fdo->ConsoleAcquired = TRUE;

    if (Fdo->BalloonInterface.Interface.Context != NULL) {
        status = XENBUS_BALLOON(Acquire, &Fdo->BalloonInterface);
        if (!NT_SUCCESS(status))
            goto fail7;
    }

    status = __FdoD3ToD0(Fdo);
    if (!NT_SUCCESS(status))
        goto fail8;

    status = XENBUS_SUSPEND(Register,
                            &Fdo->SuspendInterface,
                            SUSPEND_CALLBACK_LATE,
                            FdoSuspendCallbackLate,
                            Fdo,
                            &Fdo->SuspendCallbackLate);
    if (!NT_SUCCESS(status))
        goto fail9;

    status = XENBUS_DEBUG(Register,
                          &Fdo->DebugInterface,
                          __MODULE__ "|FDO",
                          FdoDebugCallback,
                          Fdo,
                          &Fdo->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail10;

    KeLowerIrql(Irql);

not_active:
    __FdoSetDevicePowerState(Fdo, PowerDeviceD0);

    PowerState.DeviceState = PowerDeviceD0;
    PoSetPowerState(Fdo->Dx->DeviceObject,
                    DevicePowerState,
                    PowerState);

    __FdoAcquireMutex(Fdo);

    for (ListEntry = Fdo->List.Flink;
         ListEntry != &Fdo->List;
         ListEntry = ListEntry->Flink) {
        PXENBUS_DX  Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        PdoResume(Pdo);
    }

    __FdoReleaseMutex(Fdo);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail10:
    Error("fail10\n");

    XENBUS_SUSPEND(Deregister,
                   &Fdo->SuspendInterface,
                   Fdo->SuspendCallbackLate);
    Fdo->SuspendCallbackLate = NULL;

fail9:
    Error("fail9\n");

    __FdoD0ToD3(Fdo);

fail8:
    Error("fail8\n");

    if (Fdo->BalloonInterface.Interface.Context != NULL)
        XENBUS_BALLOON(Release, &Fdo->BalloonInterface);

fail7:
    Error("fail7\n");

    if (Fdo->ConsoleAcquired) {
        XENBUS_CONSOLE(Release, &Fdo->ConsoleInterface);
        Fdo->ConsoleAcquired = FALSE;
    }

    XENBUS_STORE(Release, &Fdo->StoreInterface);

fail6:
    Error("fail6\n");

    XENBUS_EVTCHN(Release, &Fdo->EvtchnInterface);

fail5:
    Error("fail5\n");

    if (Fdo->UseMemoryHole == 0)
        FdoPciHoleDestroy(Fdo);

fail4:
    Error("fail4\n");

    XENBUS_RANGE_SET(Release, &Fdo->RangeSetInterface);

fail3:
    Error("fail3\n");

    XENBUS_SUSPEND(Release, &Fdo->SuspendInterface);

fail2:
    Error("fail2\n");

    XENBUS_DEBUG(Release, &Fdo->DebugInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    KeLowerIrql(Irql);

    return status;
}

// This function must not touch pageable code or data
static VOID
FdoD0ToD3(
    _In_ PXENBUS_FDO    Fdo
    )
{
    POWER_STATE         PowerState;
    PLIST_ENTRY         ListEntry;
    KIRQL               Irql;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);
    ASSERT3U(__FdoGetDevicePowerState(Fdo), ==, PowerDeviceD0);

    Trace("====>\n");

    __FdoAcquireMutex(Fdo);

    for (ListEntry = Fdo->List.Flink;
         ListEntry != &Fdo->List;
         ListEntry = ListEntry->Flink) {
        PXENBUS_DX  Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        if (PdoGetDevicePnpState(Pdo) == Deleted ||
            PdoIsMissing(Pdo))
            continue;

        PdoSuspend(Pdo);
    }

    __FdoReleaseMutex(Fdo);

    PowerState.DeviceState = PowerDeviceD3;
    PoSetPowerState(Fdo->Dx->DeviceObject,
                    DevicePowerState,
                    PowerState);

    __FdoSetDevicePowerState(Fdo, PowerDeviceD3);

    if (!__FdoIsActive(Fdo))
        goto not_active;

    if (Fdo->BalloonInterface.Interface.Context != NULL) {
        Trace("waiting for balloon thread...\n");

        KeClearEvent(&Fdo->BalloonEvent);
        ThreadWake(Fdo->BalloonThread);

        (VOID) KeWaitForSingleObject(&Fdo->BalloonEvent,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);

        Trace("done\n");
    }

    Trace("waiting for suspend thread...\n");

    KeClearEvent(&Fdo->SuspendEvent);
    ThreadWake(Fdo->SuspendThread);

    (VOID) KeWaitForSingleObject(&Fdo->SuspendEvent,
                                 Executive,
                                 KernelMode,
                                 FALSE,
                                 NULL);

    Trace("done\n");

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    XENBUS_DEBUG(Deregister,
                 &Fdo->DebugInterface,
                 Fdo->DebugCallback);
    Fdo->DebugCallback = NULL;

    XENBUS_SUSPEND(Deregister,
                   &Fdo->SuspendInterface,
                   Fdo->SuspendCallbackLate);
    Fdo->SuspendCallbackLate = NULL;

    __FdoD0ToD3(Fdo);

    if (Fdo->BalloonInterface.Interface.Context != NULL)
        XENBUS_BALLOON(Release, &Fdo->BalloonInterface);

    if (Fdo->ConsoleAcquired) {
        XENBUS_CONSOLE(Release, &Fdo->ConsoleInterface);
        Fdo->ConsoleAcquired = FALSE;
    }

    XENBUS_STORE(Release, &Fdo->StoreInterface);

    XENBUS_EVTCHN(Release, &Fdo->EvtchnInterface);

    if (Fdo->UseMemoryHole == 0)
        FdoPciHoleDestroy(Fdo);

    XENBUS_RANGE_SET(Release, &Fdo->RangeSetInterface);

    XENBUS_SUSPEND(Release, &Fdo->SuspendInterface);

    XENBUS_DEBUG(Release, &Fdo->DebugInterface);

    KeLowerIrql(Irql);

not_active:
    Trace("<====\n");
}

// This function must not touch pageable code or data
static VOID
FdoS4ToS3(
    _In_ PXENBUS_FDO    Fdo
    )
{
    KIRQL               Irql;

    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);
    ASSERT3U(__FdoGetSystemPowerState(Fdo), ==, PowerSystemHibernate);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    if (!__FdoIsActive(Fdo))
        goto not_active;

    LogResume();

    HypercallPopulate();

    UnplugDevices();

not_active:
    KeLowerIrql(Irql);

    __FdoSetSystemPowerState(Fdo, PowerSystemSleeping3);

    Trace("<====\n");
}

// This function must not touch pageable code or data
static VOID
FdoS3ToS4(
    _In_ PXENBUS_FDO    Fdo
    )
{
    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);
    ASSERT3U(__FdoGetSystemPowerState(Fdo), ==, PowerSystemSleeping3);

    if (!__FdoIsActive(Fdo))
        goto not_active;

    BUG_ON(SuspendGetReferences(Fdo->SuspendContext) != 0);
    BUG_ON(SharedInfoGetReferences(Fdo->SharedInfoContext) != 0);
    BUG_ON(EvtchnGetReferences(Fdo->EvtchnContext) != 0);
    BUG_ON(StoreGetReferences(Fdo->StoreContext) != 0);
    BUG_ON(ConsoleGetReferences(Fdo->ConsoleContext) != 0);
    BUG_ON(GnttabGetReferences(Fdo->GnttabContext) != 0);
    BUG_ON(BalloonGetReferences(Fdo->BalloonContext) != 0);

not_active:
    __FdoSetSystemPowerState(Fdo, PowerSystemHibernate);

    Trace("<====\n");
}

static VOID
FdoFilterCmPartialResourceList(
    _In_ PXENBUS_FDO                Fdo,
    _In_ PCM_PARTIAL_RESOURCE_LIST  List
    )
{
    ULONG                           Index;

    UNREFERENCED_PARAMETER(Fdo);

    for (Index = 0; Index < List->Count; Index++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor = &List->PartialDescriptors[Index];

        //
        // These are additional resources that XENBUS requested, so they must
        // be filtered out before the underlying PCI bus driver sees them. Happily
        // it appears that swapping the type to DevicePrivate causes PCI.SYS to ignore
        // them.
        //
        if (Descriptor->Type == CmResourceTypeInterrupt &&
            (Descriptor->Flags & CM_RESOURCE_INTERRUPT_MESSAGE))
            Descriptor->Type = CmResourceTypeDevicePrivate;
    }
}

#define BALLOON_WARN_TIMEOUT        10
#define BALLOON_BUGCHECK_TIMEOUT    1200

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS
FdoStartDevice(
    _In_ PXENBUS_FDO                Fdo,
    _In_ PIRP                       Irp
    )
{
    PIO_STACK_LOCATION              StackLocation;
    PCM_RESOURCE_LIST               ResourceList;
    PCM_FULL_RESOURCE_DESCRIPTOR    Descriptor;
    ULONG                           Size;
    NTSTATUS                        status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    // Raw

    ResourceList = StackLocation->Parameters.StartDevice.AllocatedResources;
    FdoDumpCmResourceList(Fdo, FALSE, ResourceList);

    ASSERT3U(ResourceList->Count, ==, 1);
    Descriptor = &ResourceList->List[0];

    ASSERT3U(Descriptor->InterfaceType, ==, PCIBus);
    ASSERT3U(Descriptor->BusNumber, ==, 0);

    Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors) +
           (Descriptor->PartialResourceList.Count) * sizeof (CM_PARTIAL_RESOURCE_DESCRIPTOR);

    Fdo->RawResourceList = __FdoAllocate(Size);

    status = STATUS_NO_MEMORY;
    if (Fdo->RawResourceList == NULL)
        goto fail1;

    RtlCopyMemory(Fdo->RawResourceList, &Descriptor->PartialResourceList, Size);

    FdoFilterCmPartialResourceList(Fdo, &Descriptor->PartialResourceList);

    // Translated

    ResourceList = StackLocation->Parameters.StartDevice.AllocatedResourcesTranslated;
    FdoDumpCmResourceList(Fdo, TRUE, ResourceList);

    ASSERT3U(ResourceList->Count, ==, 1);
    Descriptor = &ResourceList->List[0];

    ASSERT3U(Descriptor->InterfaceType, ==, PCIBus);
    ASSERT3U(Descriptor->BusNumber, ==, 0);

    Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors) +
           (Descriptor->PartialResourceList.Count) * sizeof (CM_PARTIAL_RESOURCE_DESCRIPTOR);

    Fdo->TranslatedResourceList = __FdoAllocate(Size);

    status = STATUS_NO_MEMORY;
    if (Fdo->TranslatedResourceList == NULL)
        goto fail2;

    RtlCopyMemory(Fdo->TranslatedResourceList, &Descriptor->PartialResourceList, Size);

    FdoFilterCmPartialResourceList(Fdo, &Descriptor->PartialResourceList);

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail3;

    if (!__FdoIsActive(Fdo))
        goto not_active;

    status = FdoCreateInterrupt(Fdo);
    if (!NT_SUCCESS(status))
        goto fail4;

    KeInitializeEvent(&Fdo->ScanEvent, NotificationEvent, FALSE);

    status = ThreadCreate(FdoScan, Fdo, &Fdo->ScanThread);
    if (!NT_SUCCESS(status))
        goto fail5;

    InitializeMutex(&Fdo->BalloonSuspendMutex);

    KeInitializeEvent(&Fdo->SuspendEvent, NotificationEvent, FALSE);

    status = ThreadCreate(FdoSuspend, Fdo, &Fdo->SuspendThread);
    if (!NT_SUCCESS(status))
        goto fail6;

    if (Fdo->BalloonInterface.Interface.Context != NULL) {
        KeInitializeEvent(&Fdo->BalloonEvent, NotificationEvent, FALSE);

        status = ThreadCreate(FdoBalloon, Fdo, &Fdo->BalloonThread);
        if (!NT_SUCCESS(status))
            goto fail7;
    }

not_active:
    status = FdoD3ToD0(Fdo);
    if (!NT_SUCCESS(status))
        goto fail8;

    if (Fdo->BalloonInterface.Interface.Context != NULL) {
        LARGE_INTEGER   Timeout;

        ASSERT(__FdoIsActive(Fdo));

        KeClearEvent(&Fdo->BalloonEvent);
        ThreadWake(Fdo->BalloonThread);

        //
        // Balloon inflation should complete within a reasonable
        // time (otherwise the target is probably unreasonable).
        //
        Timeout.QuadPart = TIME_RELATIVE(TIME_S(BALLOON_WARN_TIMEOUT));

        status = KeWaitForSingleObject(&Fdo->BalloonEvent,
                                        Executive,
                                        KernelMode,
                                        FALSE,
                                        &Timeout);
        if (status == STATUS_TIMEOUT) {
            Warning("waiting for balloon\n");

            //
            // If inflation does not complete after a lengthy timeout
            // then it is unlikely that it ever will. In this case we
            // cause a bugcheck.
            //
            Timeout.QuadPart = TIME_RELATIVE(TIME_S((BALLOON_BUGCHECK_TIMEOUT - BALLOON_WARN_TIMEOUT)));

            status = KeWaitForSingleObject(&Fdo->BalloonEvent,
                                            Executive,
                                            KernelMode,
                                            FALSE,
                                            &Timeout);
            if (status == STATUS_TIMEOUT)
                BUG("BALLOON INFLATION TIMEOUT\n");
        }
    }

    __FdoSetDevicePnpState(Fdo, Started);

    if (__FdoIsActive(Fdo))
        ThreadWake(Fdo->ScanThread);

    status = Irp->IoStatus.Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;

fail8:
    Error("fail8\n");

    if (!__FdoIsActive(Fdo))
        goto fail4;

    if (Fdo->BalloonInterface.Interface.Context != NULL) {
        ThreadAlert(Fdo->BalloonThread);
        ThreadJoin(Fdo->BalloonThread);
        Fdo->BalloonThread = NULL;
    }

fail7:
    Error("fail7\n");

    if (Fdo->BalloonInterface.Interface.Context != NULL)
        RtlZeroMemory(&Fdo->BalloonEvent, sizeof (KEVENT));

    ThreadAlert(Fdo->SuspendThread);
    ThreadJoin(Fdo->SuspendThread);
    Fdo->SuspendThread = NULL;

fail6:
    Error("fail6\n");

    RtlZeroMemory(&Fdo->SuspendEvent, sizeof (KEVENT));

    RtlZeroMemory(&Fdo->BalloonSuspendMutex, sizeof (MUTEX));

    ThreadAlert(Fdo->ScanThread);
    ThreadJoin(Fdo->ScanThread);
    Fdo->ScanThread = NULL;

fail5:
    Error("fail5\n");

    RtlZeroMemory(&Fdo->ScanEvent, sizeof (KEVENT));

    FdoDestroyInterrupt(Fdo);

fail4:
    Error("fail4\n");

fail3:
    Error("fail3\n");

    __FdoFree(Fdo->TranslatedResourceList);
    Fdo->TranslatedResourceList = NULL;

fail2:
    Error("fail2\n");

    __FdoFree(Fdo->RawResourceList);
    Fdo->RawResourceList = NULL;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoQueryStopDevice(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP       Irp
    )
{
    NTSTATUS        status;

    status = STATUS_UNSUCCESSFUL;
    if (Fdo->BalloonInterface.Interface.Context != NULL &&
        XENBUS_BALLOON(GetSize,
                       &Fdo->BalloonInterface) != 0)
        goto fail1;

    __FdoSetDevicePnpState(Fdo, StopPending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoCancelStopDevice(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    )
{
    NTSTATUS            status;

    Irp->IoStatus.Status = STATUS_SUCCESS;

    __FdoRestoreDevicePnpState(Fdo, StopPending);

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

static NTSTATUS
FdoStopDevice(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    )
{
    NTSTATUS            status;

    if (__FdoGetDevicePowerState(Fdo) == PowerDeviceD0)
        FdoD0ToD3(Fdo);

    if (!__FdoIsActive(Fdo))
        goto not_active;

    if (Fdo->BalloonInterface.Interface.Context != NULL) {
        ThreadAlert(Fdo->BalloonThread);
        ThreadJoin(Fdo->BalloonThread);
        Fdo->BalloonThread = NULL;

        RtlZeroMemory(&Fdo->BalloonEvent, sizeof (KEVENT));
    }

    ThreadAlert(Fdo->SuspendThread);
    ThreadJoin(Fdo->SuspendThread);
    Fdo->SuspendThread = NULL;

    RtlZeroMemory(&Fdo->SuspendEvent, sizeof (KEVENT));

    RtlZeroMemory(&Fdo->BalloonSuspendMutex, sizeof (MUTEX));

    ThreadAlert(Fdo->ScanThread);
    ThreadJoin(Fdo->ScanThread);
    Fdo->ScanThread = NULL;

    RtlZeroMemory(&Fdo->ScanEvent, sizeof (KEVENT));

    FdoDestroyInterrupt(Fdo);

not_active:
    __FdoFree(Fdo->TranslatedResourceList);
    Fdo->TranslatedResourceList = NULL;

    __FdoFree(Fdo->RawResourceList);
    Fdo->RawResourceList = NULL;

    __FdoSetDevicePnpState(Fdo, Stopped);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

static NTSTATUS
FdoQueryRemoveDevice(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    )
{
    NTSTATUS            status;

    status = STATUS_UNSUCCESSFUL;
    if (Fdo->BalloonInterface.Interface.Context != NULL &&
        XENBUS_BALLOON(GetSize,
                       &Fdo->BalloonInterface) != 0)
        goto fail1;

    __FdoSetDevicePnpState(Fdo, RemovePending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoCancelRemoveDevice(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    )
{
    NTSTATUS            status;

    __FdoRestoreDevicePnpState(Fdo, RemovePending);

    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

static NTSTATUS
FdoSurpriseRemoval(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    )
{
    PLIST_ENTRY         ListEntry;
    NTSTATUS            status;

    __FdoSetDevicePnpState(Fdo, SurpriseRemovePending);

    __FdoAcquireMutex(Fdo);

    for (ListEntry = Fdo->List.Flink;
         ListEntry != &Fdo->List;
         ListEntry = ListEntry->Flink) {
        PXENBUS_DX  Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        if (!PdoIsMissing(Pdo))
            PdoSetMissing(Pdo, "FDO surprise removed");
    }

    __FdoReleaseMutex(Fdo);

    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS
FdoRemoveDevice(
    _In_ PXENBUS_FDO                    Fdo,
    _In_ PIRP                           Irp
    )
{
    PLIST_ENTRY                         ListEntry;
    NTSTATUS                            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    if (__FdoGetPreviousDevicePnpState(Fdo) != Started)
        goto done;

    if (__FdoIsActive(Fdo)) {
        Trace("waiting for scan thread...\n");

        KeClearEvent(&Fdo->ScanEvent);
        ThreadWake(Fdo->ScanThread);

        (VOID) KeWaitForSingleObject(&Fdo->ScanEvent,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);

        Trace("done\n");
    }

    __FdoAcquireMutex(Fdo);

    ListEntry = Fdo->List.Flink;
    while (ListEntry != &Fdo->List) {
        PLIST_ENTRY Flink = ListEntry->Flink;
        PXENBUS_DX  Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        if (!PdoIsMissing(Pdo))
            PdoSetMissing(Pdo, "FDO removed");

        if (PdoGetDevicePnpState(Pdo) != SurpriseRemovePending)
            PdoSetDevicePnpState(Pdo, Deleted);

        if (PdoGetDevicePnpState(Pdo) == Deleted)
            PdoDestroy(Pdo);

        ListEntry = Flink;
    }

    __FdoReleaseMutex(Fdo);

    if (__FdoGetDevicePowerState(Fdo) == PowerDeviceD0)
        FdoD0ToD3(Fdo);

    if (!__FdoIsActive(Fdo))
        goto not_active;

    if (Fdo->BalloonInterface.Interface.Context != NULL) {
        ThreadAlert(Fdo->BalloonThread);
        ThreadJoin(Fdo->BalloonThread);
        Fdo->BalloonThread = NULL;

        RtlZeroMemory(&Fdo->BalloonEvent, sizeof (KEVENT));
    }

    ThreadAlert(Fdo->SuspendThread);
    ThreadJoin(Fdo->SuspendThread);
    Fdo->SuspendThread = NULL;

    RtlZeroMemory(&Fdo->SuspendEvent, sizeof (KEVENT));

    RtlZeroMemory(&Fdo->BalloonSuspendMutex, sizeof (MUTEX));

    ThreadAlert(Fdo->ScanThread);
    ThreadJoin(Fdo->ScanThread);
    Fdo->ScanThread = NULL;

    RtlZeroMemory(&Fdo->ScanEvent, sizeof (KEVENT));

    FdoDestroyInterrupt(Fdo);

not_active:
    __FdoFree(Fdo->TranslatedResourceList);
    Fdo->TranslatedResourceList = NULL;

    __FdoFree(Fdo->RawResourceList);
    Fdo->RawResourceList = NULL;

done:
    __FdoSetDevicePnpState(Fdo, Deleted);

    // We must release our reference before the PDO is destroyed
    FdoReleaseLowerBusInterface(Fdo);

    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    __FdoAcquireMutex(Fdo);
    ASSERT3U(Fdo->References, !=, 0);
    --Fdo->References;
    __FdoReleaseMutex(Fdo);

    if (Fdo->References == 0) {
        DriverAcquireMutex();
        FdoDestroy(Fdo);
        DriverReleaseMutex();
    }

    return status;
}

#define SCAN_PAUSE  10

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS
FdoQueryDeviceRelations(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    ULONG               Size;
    PDEVICE_RELATIONS   Relations;
    ULONG               Count;
    PLIST_ENTRY         ListEntry;
    BOOLEAN             Warned;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    status = Irp->IoStatus.Status;

    if (StackLocation->Parameters.QueryDeviceRelations.Type != BusRelations) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

        goto done;
    }

    Warned = FALSE;

    for (;;) {
        LARGE_INTEGER   Timeout;

        if (!__FdoIsActive(Fdo))
            break;

        Timeout.QuadPart = TIME_RELATIVE(TIME_S(SCAN_PAUSE));

        status = KeWaitForSingleObject(&Fdo->ScanEvent,
                                       Executive,
                                       KernelMode,
                                       FALSE,
                                       &Timeout);
        if (status != STATUS_TIMEOUT)
            break;

        if (!Warned) {
            Warning("waiting for device enumeration\n");
            Warned = TRUE;
        }
    }

    __FdoAcquireMutex(Fdo);

    Count = 0;
    for (ListEntry = Fdo->List.Flink;
         ListEntry != &Fdo->List;
         ListEntry = ListEntry->Flink)
        Count++;

    Size = FIELD_OFFSET(DEVICE_RELATIONS, Objects) + (sizeof (PDEVICE_OBJECT) * __max(Count, 1));

    Relations = __AllocatePoolWithTag(PagedPool, Size, 'SUB');

    status = STATUS_NO_MEMORY;
    if (Relations == NULL)
        goto fail1;

    for (ListEntry = Fdo->List.Flink;
         ListEntry != &Fdo->List;
         ListEntry = ListEntry->Flink) {
        PXENBUS_DX  Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        if (PdoIsMissing(Pdo))
            continue;

        if (PdoGetDevicePnpState(Pdo) == Present)
            PdoSetDevicePnpState(Pdo, Enumerated);

        ObReferenceObject(Dx->DeviceObject);
        Relations->Objects[Relations->Count++] = Dx->DeviceObject;
    }

    ASSERT3U(Relations->Count, <=, Count);

    Trace("%d PDO(s)\n", Relations->Count);

    __FdoReleaseMutex(Fdo);

    Irp->IoStatus.Information = (ULONG_PTR)Relations;
    Irp->IoStatus.Status = STATUS_SUCCESS;

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail2;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    __FdoAcquireMutex(Fdo);

    ListEntry = Fdo->List.Flink;
    while (ListEntry != &Fdo->List) {
        PXENBUS_DX  Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO Pdo = Dx->Pdo;
        PLIST_ENTRY Next = ListEntry->Flink;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        if (PdoGetDevicePnpState(Pdo) == Deleted &&
            PdoIsMissing(Pdo))
            PdoDestroy(Pdo);

        ListEntry = Next;
    }

    __FdoReleaseMutex(Fdo);

done:
    return status;

fail2:
    Error("fail2\n");

    __FdoAcquireMutex(Fdo);

fail1:
    Error("fail1 (%08x)\n", status);

    __FdoReleaseMutex(Fdo);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoQueryCapabilities(
    _In_ PXENBUS_FDO        Fdo,
    _In_ PIRP               Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    PDEVICE_CAPABILITIES    Capabilities;
    SYSTEM_POWER_STATE      SystemPowerState;
    NTSTATUS                status;

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Capabilities = StackLocation->Parameters.DeviceCapabilities.Capabilities;

    Fdo->LowerDeviceCapabilities = *Capabilities;

    // Make sure that the FDO is non-removable
    Capabilities->Removable = 0;

    for (SystemPowerState = 0; SystemPowerState < PowerSystemMaximum; SystemPowerState++) {
        DEVICE_POWER_STATE  DevicePowerState;

        DevicePowerState = Fdo->LowerDeviceCapabilities.DeviceState[SystemPowerState];
    }

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoDeviceUsageNotification(
    _In_ PXENBUS_FDO                Fdo,
    _In_ PIRP                       Irp
    )
{
    PIO_STACK_LOCATION              StackLocation;
    DEVICE_USAGE_NOTIFICATION_TYPE  Type;
    BOOLEAN                         InPath;
    BOOLEAN                         NotDisableable;
    NTSTATUS                        status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Type = StackLocation->Parameters.UsageNotification.Type;
    InPath = StackLocation->Parameters.UsageNotification.InPath;

    if (InPath) {
        Trace("%s: ADDING %s\n",
              __FdoGetName(Fdo),
              DeviceUsageNotificationTypeName(Type));
        Fdo->Usage[Type]++;
    } else {
        if (Fdo->Usage[Type] != 0) {
            Trace("%s: REMOVING %s\n",
                  __FdoGetName(Fdo),
                  DeviceUsageNotificationTypeName(Type));
            --Fdo->Usage[Type];
        }
    }

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    NotDisableable = FALSE;
    for (Type = 0; Type <= DeviceUsageTypeDumpFile; Type++) {
        if (Fdo->Usage[Type] != 0) {
            NotDisableable = TRUE;
            break;
        }
    }

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    if (Fdo->NotDisableable != NotDisableable) {
        Fdo->NotDisableable = NotDisableable;

        IoInvalidateDeviceState(__FdoGetPhysicalDeviceObject(Fdo));
    }

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoQueryPnpDeviceState(
    _In_ PXENBUS_FDO                Fdo,
    _In_ PIRP                       Irp
    )
{
    ULONG_PTR                       State;
    NTSTATUS                        status;

    if (Irp->IoStatus.Status == STATUS_SUCCESS)
        State = Irp->IoStatus.Information;
    else if (Irp->IoStatus.Status == STATUS_NOT_SUPPORTED)
        State = 0;
    else
        goto done;

    if (Fdo->NotDisableable) {
        Trace("%s: not disableable\n", __FdoGetName(Fdo));
        State |= PNP_DEVICE_NOT_DISABLEABLE;
    }

    Irp->IoStatus.Information = State;
    Irp->IoStatus.Status = STATUS_SUCCESS;

done:
    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

static NTSTATUS
FdoDispatchPnp(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

    Trace("====> (%02x:%s)\n",
          MinorFunction,
          PnpMinorFunctionName(MinorFunction));

    switch (StackLocation->MinorFunction) {
    case IRP_MN_FILTER_RESOURCE_REQUIREMENTS:
        status = FdoFilterResourceRequirements(Fdo, Irp);
        break;

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

    case IRP_MN_QUERY_CAPABILITIES:
        status = FdoQueryCapabilities(Fdo, Irp);
        break;

    case IRP_MN_DEVICE_USAGE_NOTIFICATION:
        status = FdoDeviceUsageNotification(Fdo, Irp);
        break;

    case IRP_MN_QUERY_PNP_DEVICE_STATE:
        status = FdoQueryPnpDeviceState(Fdo, Irp);
        break;

    default:
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
        break;
    }

    Trace("<==== (%02x:%s)(%08x)\n",
          MinorFunction,
          PnpMinorFunctionName(MinorFunction),
          status);

    return status;
}

static IO_WORKITEM_ROUTINE FdoSetDevcePowerUpWorker;

_Use_decl_annotations_
static VOID
FdoSetDevcePowerUpWorker(
    PDEVICE_OBJECT      DeviceObject,
    PVOID               Context
    )
{
    PXENBUS_FDO         Fdo = (PXENBUS_FDO) Context;
    PIRP                Irp;

    UNREFERENCED_PARAMETER(DeviceObject);

    ASSERT(Fdo != NULL);

    Irp = InterlockedExchangePointer(&Fdo->DevicePowerIrp, NULL);
    ASSERT(Irp != NULL);

    (VOID) FdoD3ToD0(Fdo);

    // Cannot change Irp->IoStatus. Continue completion chain.
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
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
    PXENBUS_FDO         Fdo = (PXENBUS_FDO) Context;
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;

    UNREFERENCED_PARAMETER(DeviceObject);

    ASSERT(Fdo != NULL);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;

    Info("%s -> %s\n",
         DevicePowerStateName(__FdoGetDevicePowerState(Fdo)),
         DevicePowerStateName(DeviceState));

    ASSERT3U(DeviceState, ==, PowerDeviceD0);

    (VOID) InterlockedExchangePointer(&Fdo->DevicePowerIrp, Irp);

    IoQueueWorkItem(Fdo->DevicePowerWorkItem,
                    FdoSetDevcePowerUpWorker,
                    DelayedWorkQueue,
                    Fdo);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
FdoSetDevicePowerUp(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    )
{
    IoMarkIrpPending(Irp);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           FdoSetDevicePowerUpComplete,
                           Fdo,
                           TRUE,
                           TRUE,
                           TRUE);
    IoCallDriver(Fdo->LowerDeviceObject, Irp);
    return STATUS_PENDING;
}

static IO_WORKITEM_ROUTINE FdoSetDevicePowerDownWorker;

_Use_decl_annotations_
static VOID
FdoSetDevicePowerDownWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID      Context
    )
{
    PXENBUS_FDO         Fdo = (PXENBUS_FDO)Context;
    PIRP                Irp;

    UNREFERENCED_PARAMETER(DeviceObject);

    ASSERT(Fdo != NULL);

    Irp = InterlockedExchangePointer(&Fdo->DevicePowerIrp, NULL);
    ASSERT(Irp != NULL);

    FdoD0ToD3(Fdo);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoCallDriver(Fdo->LowerDeviceObject, Irp);
}

static NTSTATUS
FdoSetDevicePowerDown(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;

    ASSERT3U(DeviceState, >,  __FdoGetDevicePowerState(Fdo));

    Info("%s: %s -> %s\n",
         __FdoGetName(Fdo),
         DevicePowerStateName(__FdoGetDevicePowerState(Fdo)),
         DevicePowerStateName(DeviceState));

    ASSERT3U(DeviceState, ==, PowerDeviceD3);

    if (__FdoGetDevicePowerState(Fdo) != PowerDeviceD0) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

        goto done;
    }

    IoMarkIrpPending(Irp);
    status = STATUS_PENDING;

    (VOID) InterlockedExchangePointer(&Fdo->DevicePowerIrp, Irp);

    IoQueueWorkItem(Fdo->DevicePowerWorkItem,
                    FdoSetDevicePowerDownWorker,
                    DelayedWorkQueue,
                    Fdo);

done:
    return status;
}

static NTSTATUS
FdoSetDevicePower(
    _In_ PXENBUS_FDO    Fdo,
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

    Trace("====> (%s:%s)\n",
          DevicePowerStateName(DeviceState),
          PowerActionName(PowerAction));

    if (DeviceState == __FdoGetDevicePowerState(Fdo)) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

        goto done;
    }

    status = (DeviceState < __FdoGetDevicePowerState(Fdo)) ?
             FdoSetDevicePowerUp(Fdo, Irp) :
             FdoSetDevicePowerDown(Fdo, Irp);

done:
    Trace("<==== (%s:%s)(%08x)\n",
          DevicePowerStateName(DeviceState),
          PowerActionName(PowerAction),
          status);
    return status;
}

static REQUEST_POWER_COMPLETE FdoRequestDevicePowerUpComplete;

_Use_decl_annotations_
static VOID
FdoRequestDevicePowerUpComplete(
    _In_ PDEVICE_OBJECT     DeviceObject,
    _In_ UCHAR              MinorFunction,
    _In_ POWER_STATE        PowerState,
    _In_opt_ PVOID          Context,
    _In_ PIO_STATUS_BLOCK   IoStatus
    )
{
    PIRP                    Irp = (PIRP) Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);
    UNREFERENCED_PARAMETER(IoStatus);

    ASSERT(Irp != NULL);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static IO_WORKITEM_ROUTINE FdoSetSystemPowerUpWorker;

_Use_decl_annotations_
static VOID
FdoSetSystemPowerUpWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID      Context
    )
{
    PXENBUS_FDO         Fdo = (PXENBUS_FDO)Context;
    PIRP                Irp;
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    POWER_STATE         PowerState;
    NTSTATUS            status;

    UNREFERENCED_PARAMETER(DeviceObject);

    ASSERT(Fdo != NULL);

    Irp = InterlockedExchangePointer(&Fdo->SystemPowerIrp, NULL);
    ASSERT(Irp != NULL);

    __FdoSetSystemPowerState(Fdo, PowerSystemHibernate);
    FdoS4ToS3(Fdo);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    Info("%s -> %s\n",
         SystemPowerStateName(__FdoGetSystemPowerState(Fdo)),
         SystemPowerStateName(SystemState));

    __FdoSetSystemPowerState(Fdo, SystemState);

    PowerState.DeviceState = Fdo->LowerDeviceCapabilities.DeviceState[SystemState];

    status = PoRequestPowerIrp(Fdo->LowerDeviceObject,
                               IRP_MN_SET_POWER,
                               PowerState,
                               FdoRequestDevicePowerUpComplete,
                               Irp,
                               NULL);
    if (!NT_SUCCESS(status))
        goto fail1;

    return;

fail1:
    Error("fail1 (%08x)\n", status);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
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
    PXENBUS_FDO         Fdo = (PXENBUS_FDO) Context;
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    POWER_STATE         PowerState;
    NTSTATUS            status;

    UNREFERENCED_PARAMETER(DeviceObject);

    ASSERT(Fdo != NULL);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT(SystemState >= PowerSystemUnspecified &&
           SystemState < PowerSystemMaximum);

    if (SystemState < PowerSystemHibernate &&
        __FdoGetSystemPowerState(Fdo) >= PowerSystemHibernate) {

        (VOID) InterlockedExchangePointer(&Fdo->SystemPowerIrp, Irp);

        IoQueueWorkItem(Fdo->SystemPowerWorkItem,
                        FdoSetSystemPowerUpWorker,
                        DelayedWorkQueue,
                        Fdo);

        goto done;
    }

    Info("%s -> %s\n",
         SystemPowerStateName(__FdoGetSystemPowerState(Fdo)),
         SystemPowerStateName(SystemState));

    __FdoSetSystemPowerState(Fdo, SystemState);

    PowerState.DeviceState = Fdo->LowerDeviceCapabilities.DeviceState[SystemState];

    status = PoRequestPowerIrp(Fdo->LowerDeviceObject,
                               IRP_MN_SET_POWER,
                               PowerState,
                               FdoRequestDevicePowerUpComplete,
                               Irp,
                               NULL);
    if (!NT_SUCCESS(status))
        goto fail1;

done:
    return STATUS_MORE_PROCESSING_REQUIRED;

fail1:
    Error("fail1 (%08x)\n", status);
    return STATUS_CONTINUE_COMPLETION;
}

static NTSTATUS
FdoSetSystemPowerUp(
    _In_ PXENBUS_FDO    Fdo,
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

static IO_WORKITEM_ROUTINE FdoSetSystemPowerDownWorker;

_Use_decl_annotations_
static VOID
FdoSetSystemPowerDownWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID      Context
    )
{
    PXENBUS_FDO         Fdo = (PXENBUS_FDO)Context;
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    PIRP                Irp;

    UNREFERENCED_PARAMETER(DeviceObject);

    ASSERT(Fdo != NULL);

    Irp = InterlockedExchangePointer(&Fdo->SystemPowerIrp, NULL);
    ASSERT(Irp != NULL);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    __FdoSetSystemPowerState(Fdo, PowerSystemSleeping3);
    FdoS3ToS4(Fdo);
    __FdoSetSystemPowerState(Fdo, SystemState);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoCallDriver(Fdo->LowerDeviceObject, Irp);
}

static REQUEST_POWER_COMPLETE FdoRequestDevicePowerDownComplete;

_Use_decl_annotations_
static VOID
FdoRequestDevicePowerDownComplete(
    _In_ PDEVICE_OBJECT     DeviceObject,
    _In_ UCHAR              MinorFunction,
    _In_ POWER_STATE        PowerState,
    _In_opt_ PVOID          Context,
    _In_ PIO_STATUS_BLOCK   IoStatus
    )
{
    PIRP                    Irp = (PIRP) Context;
    PIO_STACK_LOCATION      StackLocation;
    PDEVICE_OBJECT          UpperDeviceObject;
    PXENBUS_DX              Dx;
    PXENBUS_FDO             Fdo;
    SYSTEM_POWER_STATE      SystemState;
    NTSTATUS                status = IoStatus->Status;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);

    ASSERT(Irp != NULL);
    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    UpperDeviceObject = StackLocation->DeviceObject;
    Dx = (PXENBUS_DX)UpperDeviceObject->DeviceExtension;
    Fdo = Dx->Fdo;
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    if (!NT_SUCCESS(status))
        goto fail1;

    Info("%s: %s -> %s\n",
         __FdoGetName(Fdo),
         SystemPowerStateName(__FdoGetSystemPowerState(Fdo)),
         SystemPowerStateName(SystemState));

    if (SystemState >= PowerSystemHibernate &&
        __FdoGetSystemPowerState(Fdo) < PowerSystemHibernate) {

        (VOID) InterlockedExchangePointer(&Fdo->SystemPowerIrp, Irp);

        IoQueueWorkItem(Fdo->SystemPowerWorkItem,
                        FdoSetSystemPowerDownWorker,
                        DelayedWorkQueue,
                        Fdo);
        goto done;
    }

    __FdoSetSystemPowerState(Fdo, SystemState);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoCallDriver(Fdo->LowerDeviceObject, Irp);

done:
    return;

fail1:
    Error("fail1 (%08x)\n", status);
}

static NTSTATUS
FdoSetSystemPowerDown(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    POWER_STATE         PowerState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, >,  __FdoGetSystemPowerState(Fdo));

    PowerState.DeviceState = Fdo->LowerDeviceCapabilities.DeviceState[SystemState];

    if (SystemState >= PowerSystemShutdown) {
        IoCopyCurrentIrpStackLocationToNext(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

        goto done;
    }

    status = PoRequestPowerIrp(Fdo->LowerDeviceObject,
                               IRP_MN_SET_POWER,
                               PowerState,
                               FdoRequestDevicePowerDownComplete,
                               Irp,
                               NULL);
    if (!NT_SUCCESS(status))
        goto fail1;

done:
    return status;

fail1:
    Error("fail1 (%08x)\n", status);
    return status;
}

static NTSTATUS
FdoSetSystemPower(
    _In_ PXENBUS_FDO    Fdo,
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

    IoMarkIrpPending(Irp);

    Trace("====> (%s:%s)\n",
          SystemPowerStateName(SystemState),
          PowerActionName(PowerAction));

    if (SystemState == __FdoGetSystemPowerState(Fdo)) {
        IoCopyCurrentIrpStackLocationToNext(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

        goto done;
    }

    status = (SystemState < __FdoGetSystemPowerState(Fdo)) ?
             FdoSetSystemPowerUp(Fdo, Irp) :
             FdoSetSystemPowerDown(Fdo, Irp);

done:
    Trace("<==== (%s:%s)(%08x)\n",
          SystemPowerStateName(SystemState),
          PowerActionName(PowerAction),
          status);

    return status;
}

static REQUEST_POWER_COMPLETE FdoRequestQuerySystemPowerUpComplete;

_Use_decl_annotations_
static VOID
FdoRequestQuerySystemPowerUpComplete(
    _In_ PDEVICE_OBJECT     DeviceObject,
    _In_ UCHAR              MinorFunction,
    _In_ POWER_STATE        PowerState,
    _In_opt_ PVOID          Context,
    _In_ PIO_STATUS_BLOCK   IoStatus
    )
{
    PIRP                    Irp = (PIRP) Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);

    ASSERT(Irp != NULL);

    if (!NT_SUCCESS(IoStatus->Status))
        Irp->IoStatus.Status = IoStatus->Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static IO_COMPLETION_ROUTINE FdoQuerySystemPowerUpComplete;

_Use_decl_annotations_
static NTSTATUS
FdoQuerySystemPowerUpComplete(
    PDEVICE_OBJECT      DeviceObject,
    PIRP                Irp,
    PVOID               Context
    )
{
    PXENBUS_FDO         Fdo = (PXENBUS_FDO) Context;
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    POWER_STATE         PowerState;
    NTSTATUS            status;

    UNREFERENCED_PARAMETER(DeviceObject);

    ASSERT(Fdo != NULL);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;
    PowerState.DeviceState = Fdo->LowerDeviceCapabilities.DeviceState[SystemState];

    status = PoRequestPowerIrp(Fdo->LowerDeviceObject,
                               IRP_MN_QUERY_POWER,
                               PowerState,
                               FdoRequestQuerySystemPowerUpComplete,
                               Irp,
                               NULL);
    if (!NT_SUCCESS(status))
        goto fail1;

    return STATUS_MORE_PROCESSING_REQUIRED;

fail1:
    Error("fail1 (%08x)\n", status);
    Irp->IoStatus.Status = status;

    return STATUS_CONTINUE_COMPLETION;
}

static NTSTATUS
FdoQuerySystemPowerUp(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    )
{
    IoMarkIrpPending(Irp);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           FdoQuerySystemPowerUpComplete,
                           Fdo,
                           TRUE,
                           TRUE,
                           TRUE);
    return IoCallDriver(Fdo->LowerDeviceObject, Irp);
}

static REQUEST_POWER_COMPLETE FdoRequestQuerySystemPowerDownComplete;

_Use_decl_annotations_
static VOID
FdoRequestQuerySystemPowerDownComplete(
    PDEVICE_OBJECT          DeviceObject,
    UCHAR                   MinorFunction,
    POWER_STATE             PowerState,
    PVOID                   Context,
    PIO_STATUS_BLOCK        IoStatus
    )
{
    PIRP                    Irp = (PIRP) Context;
    PIO_STACK_LOCATION      StackLocation;
    PDEVICE_OBJECT          UpperDeviceObject;
    PXENBUS_DX              Dx;
    PXENBUS_FDO             Fdo;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);

    ASSERT(Irp != NULL);
    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    UpperDeviceObject = StackLocation->DeviceObject;
    Dx = (PXENBUS_DX)UpperDeviceObject->DeviceExtension;
    Fdo = Dx->Fdo;

    if (!NT_SUCCESS(IoStatus->Status))
        goto fail1;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return;

fail1:
    Error("fail1 (%08x)\n", IoStatus->Status);
    Irp->IoStatus.Status = IoStatus->Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static NTSTATUS
FdoQuerySystemPowerDown(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    POWER_STATE         PowerState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, >,  __FdoGetSystemPowerState(Fdo));

    PowerState.DeviceState = Fdo->LowerDeviceCapabilities.DeviceState[SystemState];

    status = PoRequestPowerIrp(Fdo->LowerDeviceObject,
                               IRP_MN_QUERY_POWER,
                               PowerState,
                               FdoRequestQuerySystemPowerDownComplete,
                               Irp,
                               NULL);
    if (!NT_SUCCESS(status))
        goto fail1;

    IoMarkIrpPending(Irp);
    return STATUS_PENDING;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoQuerySystemPower(
    _In_ PXENBUS_FDO    Fdo,
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

    Trace("====> (%s:%s)\n",
          SystemPowerStateName(SystemState),
          PowerActionName(PowerAction));

    if (SystemState == __FdoGetSystemPowerState(Fdo)) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

        goto done;
    }

    status = (SystemState < __FdoGetSystemPowerState(Fdo)) ?
             FdoQuerySystemPowerUp(Fdo, Irp) :
             FdoQuerySystemPowerDown(Fdo, Irp);

done:
    Trace("<==== (%s:%s)(%08x)\n",
          SystemPowerStateName(SystemState),
          PowerActionName(PowerAction),
          status);

    return status;
}

static NTSTATUS
FdoDispatchDevicePower(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->MinorFunction) {
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

static NTSTATUS
FdoDispatchSystemPower(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->MinorFunction) {
    case IRP_MN_SET_POWER:
        status = FdoSetSystemPower(Fdo, Irp);
        break;

    case IRP_MN_QUERY_POWER:
        status = FdoQuerySystemPower(Fdo, Irp);
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
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    POWER_STATE_TYPE    PowerType;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    PowerType = StackLocation->Parameters.Power.Type;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    switch (PowerType) {
    case DevicePowerState:
        status = FdoDispatchDevicePower(Fdo, Irp);
        break;

    case SystemPowerState:
        status = FdoDispatchSystemPower(Fdo, Irp);
        break;

    default:
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
        break;
    }

    return status;
}


static NTSTATUS
FdoDispatchDefault(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    )
{
    NTSTATUS            status;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

NTSTATUS
FdoDispatch(
    _In_ PXENBUS_FDO    Fdo,
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

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS
FdoQueryInterface(
    _In_ PXENBUS_FDO    Fdo,
    _In_ const GUID     *Guid,
    _In_ ULONG          Version,
    _Out_ PINTERFACE    Interface,
    _In_ ULONG          Size,
    _In_ BOOLEAN        Optional
    )
{
    KEVENT              Event;
    IO_STATUS_BLOCK     StatusBlock;
    PIRP                Irp;
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    RtlZeroMemory(&StatusBlock, sizeof(IO_STATUS_BLOCK));

    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP,
                                       Fdo->LowerDeviceObject,
                                       NULL,
                                       0,
                                       NULL,
                                       &Event,
                                       &StatusBlock);

    status = STATUS_UNSUCCESSFUL;
    if (Irp == NULL)
        goto fail1;

    // suppress "uninitialized *Interface" warning when IoCallDriver succeeds
    RtlZeroMemory(Interface, sizeof (*Interface));

    StackLocation = IoGetNextIrpStackLocation(Irp);
    StackLocation->MinorFunction = IRP_MN_QUERY_INTERFACE;

    StackLocation->Parameters.QueryInterface.InterfaceType = Guid;
    StackLocation->Parameters.QueryInterface.Size = (USHORT)Size;
    StackLocation->Parameters.QueryInterface.Version = (USHORT)Version;
    StackLocation->Parameters.QueryInterface.Interface = Interface;

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject(&Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        status = StatusBlock.Status;
    }

    if (!NT_SUCCESS(status)) {
        if (status == STATUS_NOT_SUPPORTED && Optional)
            goto done;

        goto fail2;
    }

done:
    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

#define FDO_QUERY_INTERFACE(                                                            \
    _Fdo,                                                                               \
    _ProviderName,                                                                      \
    _InterfaceName,                                                                     \
    _Interface,                                                                         \
    _Size,                                                                              \
    _Optional)                                                                          \
    FdoQueryInterface((_Fdo),                                                           \
                      &GUID_ ## _ProviderName ## _ ## _InterfaceName ## _INTERFACE,     \
                      _ProviderName ## _ ## _InterfaceName ## _INTERFACE_VERSION_MAX,   \
                      (_Interface),                                                     \
                      (_Size),                                                          \
                      (_Optional))

static NTSTATUS
FdoBalloonInitialize(
    _In_ PXENBUS_FDO    Fdo
    )
{
    CHAR                Key[] = "XEN:BALLOON=";
    PANSI_STRING        Option;
    PSTR                Value;
    BOOLEAN             Enabled;
    NTSTATUS            status;

    Enabled = TRUE;

    status = ConfigQuerySystemStartOption(Key, &Option);
    if (!NT_SUCCESS(status))
        goto done;

    Value = Option->Buffer + sizeof (Key) - 1;

    if (strcmp(Value, "OFF") == 0)
        Enabled = FALSE;
    else if (strcmp(Value, "ON") != 0)
        Warning("UNRECOGNIZED VALUE OF %s: %s\n", Key, Value);

    RegistryFreeSzValue(Option);

done:
    return Enabled ?
           BalloonInitialize(Fdo, &Fdo->BalloonContext) :
           STATUS_SUCCESS;
}

static VOID
FdoBalloonTeardown(
    _In_ PXENBUS_FDO    Fdo
    )
{
    if (Fdo->BalloonContext == NULL)
        return;

    BalloonTeardown(Fdo->BalloonContext);
    Fdo->BalloonContext = NULL;
}

static VOID
FdoSetWatchdog(
    _In_ PXENBUS_FDO    Fdo
    )
{
    CHAR                Key[] = "XEN:WATCHDOG=";
    PANSI_STRING        Option;
    ULONG               Value;
    NTSTATUS            status;

    status = ConfigQuerySystemStartOption(Key, &Option);
    if (!NT_SUCCESS(status))
        return;

    Value = strtoul(Option->Buffer + sizeof (Key) - 1, NULL, 0);

    RegistryFreeSzValue(Option);

    if (Value && Value < 10) {
        Warning("%us TOO SHORT (ROUNDING UP TO 10s)\n");
        Value = 10;
    }

    Fdo->Watchdog = Value;

    if (Fdo->Watchdog != 0)
        Info("WATCHDOG ENABLED (%us)\n", Fdo->Watchdog);
    else
        Info("WATCHDOG DISABLED\n");
}

NTSTATUS
FdoCreate(
    _In_ PDEVICE_OBJECT         PhysicalDeviceObject
    )
{
    PDEVICE_OBJECT              FunctionDeviceObject;
    PXENBUS_DX                  Dx;
    PXENBUS_FDO                 Fdo;
    PCI_COMMON_HEADER           Header;
    HANDLE                      ParametersKey;
    ULONG                       UseMemoryHole;
    NTSTATUS                    status;

#pragma prefast(suppress:28197) // Possibly leaking memory 'FunctionDeviceObject'
    status = IoCreateDevice(DriverGetDriverObject(),
                            sizeof (XENBUS_DX),
                            NULL,
                            FILE_DEVICE_BUS_EXTENDER,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &FunctionDeviceObject);
    if (!NT_SUCCESS(status))
        goto fail1;

    Dx = (PXENBUS_DX)FunctionDeviceObject->DeviceExtension;
    RtlZeroMemory(Dx, sizeof (XENBUS_DX));

    Dx->Type = FUNCTION_DEVICE_OBJECT;
    Dx->DeviceObject = FunctionDeviceObject;
    Dx->DevicePnpState = Added;
    Dx->SystemPowerState = PowerSystemWorking;
    Dx->DevicePowerState = PowerDeviceD3;

    Fdo = __FdoAllocate(sizeof (XENBUS_FDO));

    status = STATUS_NO_MEMORY;
    if (Fdo == NULL)
        goto fail2;

    Fdo->Dx = Dx;
    Fdo->PhysicalDeviceObject = PhysicalDeviceObject;
    Fdo->LowerDeviceObject = IoAttachDeviceToDeviceStack(FunctionDeviceObject,
                                                         PhysicalDeviceObject);
    if (Fdo->LowerDeviceObject == NULL)
        goto fail3;

    Fdo->SystemPowerWorkItem = IoAllocateWorkItem(FunctionDeviceObject);
    if (Fdo->SystemPowerWorkItem == NULL)
        goto fail4;

    Fdo->DevicePowerWorkItem = IoAllocateWorkItem(FunctionDeviceObject);
    if (Fdo->DevicePowerWorkItem == NULL)
        goto fail5;

    status = FdoAcquireLowerBusInterface(Fdo);
    if (!NT_SUCCESS(status))
        goto fail6;

    if (FdoGetBusData(Fdo,
                      PCI_WHICHSPACE_CONFIG,
                      &Header,
                      0,
                      sizeof (PCI_COMMON_HEADER)) == 0)
        goto fail7;

    status = __FdoSetVendorName(Fdo,
                                Header.VendorID,
                                Header.DeviceID);
    if (!NT_SUCCESS(status))
        goto fail8;

    __FdoSetName(Fdo);

    status = FDO_QUERY_INTERFACE(Fdo,
                                 XENFILT,
                                 EMULATED,
                                 (PINTERFACE)&Fdo->EmulatedInterface,
                                 sizeof (Fdo->EmulatedInterface),
                                 TRUE);
    if (!NT_SUCCESS(status))
        goto fail9;

    status = FdoSetActive(Fdo);
    if (!NT_SUCCESS(status))
        goto fail10;

    if (!__FdoIsActive(Fdo))
        goto done;

    ParametersKey = DriverGetParametersKey();

    status = RegistryQueryDwordValue(ParametersKey,
                                     "UseMemoryHole",
                                     &UseMemoryHole);
    if (!NT_SUCCESS(status))
        UseMemoryHole = 1;

    Fdo->UseMemoryHole = UseMemoryHole;

    status = DebugInitialize(Fdo, &Fdo->DebugContext);
    if (!NT_SUCCESS(status))
        goto fail11;

    status = SuspendInitialize(Fdo, &Fdo->SuspendContext);
    if (!NT_SUCCESS(status))
        goto fail12;

    status = SharedInfoInitialize(Fdo, &Fdo->SharedInfoContext);
    if (!NT_SUCCESS(status))
        goto fail13;

    status = EvtchnInitialize(Fdo, &Fdo->EvtchnContext);
    if (!NT_SUCCESS(status))
        goto fail14;

    status = RangeSetInitialize(Fdo, &Fdo->RangeSetContext);
    if (!NT_SUCCESS(status))
        goto fail15;

    status = CacheInitialize(Fdo, &Fdo->CacheContext);
    if (!NT_SUCCESS(status))
        goto fail16;

    status = GnttabInitialize(Fdo, &Fdo->GnttabContext);
    if (!NT_SUCCESS(status))
        goto fail17;

    status = StoreInitialize(Fdo, &Fdo->StoreContext);
    if (!NT_SUCCESS(status))
        goto fail18;

    status = ConsoleInitialize(Fdo, &Fdo->ConsoleContext);
    if (!NT_SUCCESS(status))
        goto fail19;

    status = UnplugInitialize(Fdo, &Fdo->UnplugContext);
    if (!NT_SUCCESS(status))
        goto fail20;

    status = FdoBalloonInitialize(Fdo);
    if (!NT_SUCCESS(status))
        goto fail21;

    status = DebugGetInterface(__FdoGetDebugContext(Fdo),
                               XENBUS_DEBUG_INTERFACE_VERSION_MAX,
                               (PINTERFACE)&Fdo->DebugInterface,
                               sizeof (Fdo->DebugInterface));
    BUG_ON(!NT_SUCCESS(status));
    ASSERT(Fdo->DebugInterface.Interface.Context != NULL);

    status = SuspendGetInterface(__FdoGetSuspendContext(Fdo),
                                 XENBUS_SUSPEND_INTERFACE_VERSION_MAX,
                                 (PINTERFACE)&Fdo->SuspendInterface,
                                 sizeof (Fdo->SuspendInterface));
    BUG_ON(!NT_SUCCESS(status));
    ASSERT(Fdo->SuspendInterface.Interface.Context != NULL);

    status = EvtchnGetInterface(__FdoGetEvtchnContext(Fdo),
                                XENBUS_EVTCHN_INTERFACE_VERSION_MAX,
                                (PINTERFACE)&Fdo->EvtchnInterface,
                                sizeof (Fdo->EvtchnInterface));
    BUG_ON(!NT_SUCCESS(status));
    ASSERT(Fdo->EvtchnInterface.Interface.Context != NULL);

    status = RangeSetGetInterface(__FdoGetRangeSetContext(Fdo),
                                  XENBUS_RANGE_SET_INTERFACE_VERSION_MAX,
                                  (PINTERFACE)&Fdo->RangeSetInterface,
                                  sizeof (Fdo->RangeSetInterface));
    BUG_ON(!NT_SUCCESS(status));
    ASSERT(Fdo->RangeSetInterface.Interface.Context != NULL);

    status = StoreGetInterface(__FdoGetStoreContext(Fdo),
                               XENBUS_STORE_INTERFACE_VERSION_MAX,
                               (PINTERFACE)&Fdo->StoreInterface,
                               sizeof (Fdo->StoreInterface));
    BUG_ON(!NT_SUCCESS(status));
    ASSERT(Fdo->StoreInterface.Interface.Context != NULL);

    status = ConsoleGetInterface(__FdoGetConsoleContext(Fdo),
                                 XENBUS_CONSOLE_INTERFACE_VERSION_MAX,
                                 (PINTERFACE)&Fdo->ConsoleInterface,
                                 sizeof (Fdo->ConsoleInterface));
    BUG_ON(!NT_SUCCESS(status));
    ASSERT(Fdo->ConsoleInterface.Interface.Context != NULL);

    status = BalloonGetInterface(__FdoGetBalloonContext(Fdo),
                                 XENBUS_BALLOON_INTERFACE_VERSION_MAX,
                                 (PINTERFACE)&Fdo->BalloonInterface,
                                 sizeof (Fdo->BalloonInterface));
    BUG_ON(!NT_SUCCESS(status));

done:
    InitializeMutex(&Fdo->Mutex);
    InitializeListHead(&Fdo->List);
    Fdo->References = 1;

    FdoSetWatchdog(Fdo);

    Info("%p (%s) %s\n",
         FunctionDeviceObject,
         __FdoGetName(Fdo),
         (__FdoIsActive(Fdo)) ? "[ACTIVE]" : "");

    Dx->Fdo = Fdo;
    FunctionDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    DriverAddFunctionDeviceObject(Fdo);

    return STATUS_SUCCESS;

fail21:
    Error("fail21\n");

    UnplugTeardown(Fdo->UnplugContext);
    Fdo->UnplugContext = NULL;

fail20:
    Error("fail20\n");

    ConsoleTeardown(Fdo->ConsoleContext);
    Fdo->ConsoleContext = NULL;

fail19:
    Error("fail19\n");

    StoreTeardown(Fdo->StoreContext);
    Fdo->StoreContext = NULL;

fail18:
    Error("fail18\n");

    GnttabTeardown(Fdo->GnttabContext);
    Fdo->GnttabContext = NULL;

fail17:
    Error("fail17\n");

    CacheTeardown(Fdo->CacheContext);
    Fdo->CacheContext = NULL;

fail16:
    Error("fail16\n");

    RangeSetTeardown(Fdo->RangeSetContext);
    Fdo->RangeSetContext = NULL;

fail15:
    Error("fail15\n");

    EvtchnTeardown(Fdo->EvtchnContext);
    Fdo->EvtchnContext = NULL;

fail14:
    Error("fail14\n");

    SharedInfoTeardown(Fdo->SharedInfoContext);
    Fdo->SharedInfoContext = NULL;

fail13:
    Error("fail13\n");

    SuspendTeardown(Fdo->SuspendContext);
    Fdo->SuspendContext = NULL;

fail12:
    Error("fail12\n");

    DebugTeardown(Fdo->DebugContext);
    Fdo->DebugContext = NULL;

fail11:
    Error("fail11\n");

    Fdo->UseMemoryHole = 0;

    //
    // We don't want to call DriverClearActive() so just
    // clear the FDO flag.
    //
    Fdo->Active = FALSE;

fail10:
    Error("fail10\n");

    RtlZeroMemory(&Fdo->EmulatedInterface,
                  sizeof (Fdo->EmulatedInterface));

fail9:
    Error("fail9\n");

    RtlZeroMemory(Fdo->VendorName, MAXNAMELEN);

fail8:
    Error("fail8\n");

fail7:
    Error("fail7\n");

    FdoReleaseLowerBusInterface(Fdo);

fail6:
    Error("fail6\n");

    IoFreeWorkItem(Fdo->DevicePowerWorkItem);
    Fdo->DevicePowerWorkItem = NULL;

fail5:
    Error("fail5\n");

    IoFreeWorkItem(Fdo->SystemPowerWorkItem);
    Fdo->SystemPowerWorkItem = NULL;

fail4:
    Error("fail4\n");

    if (Fdo->LowerDeviceObject)
        IoDetachDevice(Fdo->LowerDeviceObject);

fail3:
    Error("fail3\n");

    Fdo->PhysicalDeviceObject = NULL;
    Fdo->LowerDeviceObject = NULL;
    Fdo->Dx = NULL;

    ASSERT(IsZeroMemory(Fdo, sizeof (XENBUS_FDO)));
    __FdoFree(Fdo);

fail2:
    Error("fail2\n");

    IoDeleteDevice(FunctionDeviceObject);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
FdoDestroy(
    _In_ PXENBUS_FDO    Fdo
    )
{
    PXENBUS_DX          Dx = Fdo->Dx;
    PDEVICE_OBJECT      FunctionDeviceObject = Dx->DeviceObject;

    ASSERT(IsListEmpty(&Fdo->List));
    ASSERT3U(Fdo->References, ==, 0);
    ASSERT3U(__FdoGetDevicePnpState(Fdo), ==, Deleted);

    DriverRemoveFunctionDeviceObject(Fdo);

    Fdo->NotDisableable = FALSE;

    Info("%p (%s)\n",
         FunctionDeviceObject,
         __FdoGetName(Fdo));

    Dx->Fdo = NULL;

    Fdo->Watchdog = 0;

    RtlZeroMemory(&Fdo->List, sizeof (LIST_ENTRY));
    RtlZeroMemory(&Fdo->Mutex, sizeof (MUTEX));

    if (__FdoIsActive(Fdo)) {
        RtlZeroMemory(&Fdo->BalloonInterface,
                      sizeof (XENBUS_BALLOON_INTERFACE));

        RtlZeroMemory(&Fdo->ConsoleInterface,
                      sizeof (XENBUS_CONSOLE_INTERFACE));

        RtlZeroMemory(&Fdo->StoreInterface,
                      sizeof (XENBUS_STORE_INTERFACE));

        RtlZeroMemory(&Fdo->RangeSetInterface,
                      sizeof (XENBUS_RANGE_SET_INTERFACE));

        RtlZeroMemory(&Fdo->EvtchnInterface,
                      sizeof (XENBUS_EVTCHN_INTERFACE));

        RtlZeroMemory(&Fdo->SuspendInterface,
                      sizeof (XENBUS_SUSPEND_INTERFACE));

        RtlZeroMemory(&Fdo->DebugInterface,
                      sizeof (XENBUS_DEBUG_INTERFACE));

        FdoBalloonTeardown(Fdo);

        UnplugTeardown(Fdo->UnplugContext);
        Fdo->UnplugContext = NULL;

        ConsoleTeardown(Fdo->ConsoleContext);
        Fdo->ConsoleContext = NULL;

        StoreTeardown(Fdo->StoreContext);
        Fdo->StoreContext = NULL;

        GnttabTeardown(Fdo->GnttabContext);
        Fdo->GnttabContext = NULL;

        CacheTeardown(Fdo->CacheContext);
        Fdo->CacheContext = NULL;

        RangeSetTeardown(Fdo->RangeSetContext);
        Fdo->RangeSetContext = NULL;

        EvtchnTeardown(Fdo->EvtchnContext);
        Fdo->EvtchnContext = NULL;

        SharedInfoTeardown(Fdo->SharedInfoContext);
        Fdo->SharedInfoContext = NULL;

        SuspendTeardown(Fdo->SuspendContext);
        Fdo->SuspendContext = NULL;

        DebugTeardown(Fdo->DebugContext);
        Fdo->DebugContext = NULL;

        Fdo->UseMemoryHole = 0;

        FdoClearActive(Fdo);
    }

    RtlZeroMemory(&Fdo->EmulatedInterface,
                  sizeof (Fdo->EmulatedInterface));

    RtlZeroMemory(Fdo->VendorName, MAXNAMELEN);

    FdoReleaseLowerBusInterface(Fdo);

    IoFreeWorkItem(Fdo->DevicePowerWorkItem);
    Fdo->DevicePowerWorkItem = NULL;

    IoFreeWorkItem(Fdo->SystemPowerWorkItem);
    Fdo->SystemPowerWorkItem = NULL;

    IoDetachDevice(Fdo->LowerDeviceObject);

    RtlZeroMemory(&Fdo->LowerDeviceCapabilities, sizeof (DEVICE_CAPABILITIES));
    Fdo->LowerDeviceObject = NULL;
    Fdo->PhysicalDeviceObject = NULL;
    Fdo->Dx = NULL;

    ASSERT(IsZeroMemory(Fdo, sizeof (XENBUS_FDO)));
    __FdoFree(Fdo);

    ASSERT3U(Dx->DevicePowerState, ==, PowerDeviceD3);
    ASSERT3U(Dx->SystemPowerState, ==, PowerSystemWorking);

    IoDeleteDevice(FunctionDeviceObject);
}
