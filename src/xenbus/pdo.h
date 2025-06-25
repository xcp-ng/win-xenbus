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

#ifndef _XENBUS_PDO_H
#define _XENBUS_PDO_H

#include <ntddk.h>

#include "driver.h"
#include "types.h"

extern VOID
PdoSetDevicePnpState(
    _In_ PXENBUS_PDO        Pdo,
    _In_ DEVICE_PNP_STATE   State
    );

extern DEVICE_PNP_STATE
PdoGetDevicePnpState(
    _In_ PXENBUS_PDO    Pdo
    );

extern BOOLEAN
PdoIsMissing(
    _In_ PXENBUS_PDO    Pdo
    );

extern VOID
PdoSetMissing(
    _In_ PXENBUS_PDO    Pdo,
    _In_ PCSTR          Reason
    );

extern PSTR
PdoGetName(
    _In_ PXENBUS_PDO    Pdo
    );

extern PDEVICE_OBJECT
PdoGetDeviceObject(
    _In_ PXENBUS_PDO    Pdo
    );

extern PXENBUS_FDO
PdoGetFdo(
    _In_ PXENBUS_PDO    Pdo
    );

extern PDMA_ADAPTER
PdoGetDmaAdapter(
    _In_ PXENBUS_PDO            Pdo,
    _In_ PDEVICE_DESCRIPTION    DeviceDescriptor,
    _Out_ PULONG                NumberOfMapRegisters
    );

extern BOOLEAN
PdoTranslateBusAddress(
    _In_ PXENBUS_PDO        Pdo,
    _In_ PHYSICAL_ADDRESS   BusAddress,
    _In_ ULONG              Length,
    _Out_ PULONG            AddressSpace,
    _Out_ PPHYSICAL_ADDRESS TranslatedAddress
    );

extern ULONG
PdoSetBusData(
    _In_ PXENBUS_PDO    Pdo,
    _In_ ULONG      DataType,
    _In_ PVOID      Buffer,
    _In_ ULONG      Offset,
    _In_ ULONG      Length
    );

extern ULONG
PdoGetBusData(
    _In_ PXENBUS_PDO    Pdo,
    _In_ ULONG      DataType,
    _In_ PVOID      Buffer,
    _In_ ULONG      Offset,
    _In_ ULONG      Length
    );

extern NTSTATUS
PdoCreate(
    _In_ PXENBUS_FDO    Fdo,
    _In_ PANSI_STRING   Name
    );

extern VOID
PdoResume(
    _In_ PXENBUS_PDO    Pdo
    );

extern VOID
PdoSuspend(
    _In_ PXENBUS_PDO    Pdo
    );

extern VOID
PdoDestroy(
    _In_ PXENBUS_PDO    Pdo
    );

extern NTSTATUS
PdoDispatch(
    _In_ PXENBUS_PDO    Pdo,
    _In_ PIRP       Irp
    );

#endif  // _XENBUS_PDO_H
