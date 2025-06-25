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

#ifndef _XENBUS_FDO_H
#define _XENBUS_FDO_H

#include <ntddk.h>

#include "driver.h"
#include "types.h"

typedef struct _XENBUS_INTERRUPT XENBUS_INTERRUPT, *PXENBUS_INTERRUPT;

extern NTSTATUS
FdoCreate(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject
    );

extern VOID
FdoDestroy(
    _In_ PXENBUS_FDO    Fdo
    );

extern NTSTATUS
FdoDelegateIrp(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    );

extern VOID
FdoAddPhysicalDeviceObject(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PXENBUS_PDO    Pdo
    );

extern VOID
FdoRemovePhysicalDeviceObject(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PXENBUS_PDO    Pdo
    );

extern VOID
FdoAcquireMutex(
    _In_ PXENBUS_FDO    Fdo
    );

extern VOID
FdoReleaseMutex(
    _In_ PXENBUS_FDO    Fdo
    );

extern PDEVICE_OBJECT
FdoGetDeviceObject(
    _In_ PXENBUS_FDO    Fdo
    );

extern PDEVICE_OBJECT
FdoGetPhysicalDeviceObject(
    _In_ PXENBUS_FDO    Fdo
    );

extern PDMA_ADAPTER
FdoGetDmaAdapter(
    _In_ PXENBUS_FDO            Fdo,
    _In_ PDEVICE_DESCRIPTION    DeviceDescriptor,
    _Out_ PULONG                NumberOfMapRegisters
    );

extern BOOLEAN
FdoTranslateBusAddress(
    _In_ PXENBUS_FDO        Fdo,
    _In_ PHYSICAL_ADDRESS   BusAddress,
    _In_ ULONG              Length,
    _Out_ PULONG            AddressSpace,
    _Out_ PPHYSICAL_ADDRESS TranslatedAddress
    );

extern ULONG
FdoSetBusData(
    _In_ PXENBUS_FDO                Fdo,
    _In_ ULONG                      DataType,
    _In_reads_bytes_(Length) PVOID  Buffer,
    _In_ ULONG                      Offset,
    _In_range_(!=, 0) ULONG         Length
    );

extern ULONG
FdoGetBusData(
    _In_ PXENBUS_FDO                    Fdo,
    _In_ ULONG                          DataType,
    _Out_writes_bytes_(Length) PVOID    Buffer,
    _In_ ULONG                          Offset,
    _In_range_(!=, 0) ULONG             Length
    );

extern PSTR
FdoGetVendorName(
    _In_ PXENBUS_FDO    Fdo
    );

extern PSTR
FdoGetName(
    _In_ PXENBUS_FDO    Fdo
    );

extern PMDL
FdoHoleAllocate(
    _In_ PXENBUS_FDO    Fdo,
    _In_ ULONG          Count
    );

extern VOID
FdoHoleFree(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PMDL           Mdl
    );

// Disable erroneous SAL warnings around use of interrupt locks
#pragma warning(disable:28230)
#pragma warning(disable:28285)

extern
_IRQL_requires_max_(HIGH_LEVEL)
_IRQL_saves_
_IRQL_raises_(HIGH_LEVEL)
KIRQL
FdoAcquireInterruptLock(
    _In_ PXENBUS_FDO        Fdo,
    _In_ PXENBUS_INTERRUPT  Interrupt
    );

extern
_IRQL_requires_(HIGH_LEVEL)
VOID
FdoReleaseInterruptLock(
    _In_ PXENBUS_FDO            Fdo,
    _In_ PXENBUS_INTERRUPT      Interrupt,
    _In_ _IRQL_restores_ KIRQL  Irql
    );

extern PXENBUS_INTERRUPT
FdoAllocateInterrupt(
    _In_ PXENBUS_FDO        Fdo,
    _In_ KINTERRUPT_MODE    InterruptMode,
    _In_ USHORT             Group,
    _In_ UCHAR              Number,
    _In_ KSERVICE_ROUTINE   Callback,
    _In_opt_ PVOID          Argument
    );

extern UCHAR
FdoGetInterruptVector(
    _In_ PXENBUS_FDO        Fdo,
    _In_ PXENBUS_INTERRUPT  Interrupt
    );

extern ULONG
FdoGetInterruptLine(
    _In_ PXENBUS_FDO        Fdo,
    _In_ PXENBUS_INTERRUPT  Interrupt
    );

extern VOID
FdoFreeInterrupt(
    _In_ PXENBUS_FDO        Fdo,
    _In_ PXENBUS_INTERRUPT  Interrupt
    );

#include "suspend.h"

extern PXENBUS_SUSPEND_CONTEXT
FdoGetSuspendContext(
    _In_ PXENBUS_FDO    Fdo
    );

#include "shared_info.h"

extern PXENBUS_SHARED_INFO_CONTEXT
FdoGetSharedInfoContext(
    _In_ PXENBUS_FDO    Fdo
    );

#include "evtchn.h"

extern PXENBUS_EVTCHN_CONTEXT
FdoGetEvtchnContext(
    _In_ PXENBUS_FDO    Fdo
    );

#include "debug.h"

extern PXENBUS_DEBUG_CONTEXT
FdoGetDebugContext(
    _In_ PXENBUS_FDO    Fdo
    );

#include "store.h"

extern PXENBUS_STORE_CONTEXT
FdoGetStoreContext(
    _In_ PXENBUS_FDO    Fdo
    );

#include "range_set.h"

extern PXENBUS_RANGE_SET_CONTEXT
FdoGetRangeSetContext(
    _In_ PXENBUS_FDO    Fdo
    );

#include "cache.h"

extern PXENBUS_CACHE_CONTEXT
FdoGetCacheContext(
    _In_ PXENBUS_FDO    Fdo
    );

#include "gnttab.h"

extern PXENBUS_GNTTAB_CONTEXT
FdoGetGnttabContext(
    _In_ PXENBUS_FDO    Fdo
    );

#include "unplug.h"

extern PXENBUS_UNPLUG_CONTEXT
FdoGetUnplugContext(
    _In_ PXENBUS_FDO    Fdo
    );

#include "console.h"

extern PXENBUS_CONSOLE_CONTEXT
FdoGetConsoleContext(
    _In_ PXENBUS_FDO    Fdo
    );

extern NTSTATUS
FdoDispatch(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PIRP           Irp
    );

#endif  // _XENBUS_FDO_H
