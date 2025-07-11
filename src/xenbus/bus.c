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
#include <stdarg.h>
#include <xen.h>

#include "bus.h"
#include "dma.h"
#include "fdo.h"
#include "pdo.h"
#include "registry.h"
#include "sync.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

typedef struct _XENBUS_BUS_CONTEXT {
    LONG                    References;
    PXENBUS_PDO             Pdo;
    ULONG                   InterceptDmaAdapter;
} XENBUS_BUS_CONTEXT, *PXENBUS_BUS_CONTEXT;

#define BUS_TAG 'SUB'

static FORCEINLINE PVOID
__BusAllocate(
    _In_ ULONG  Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, BUS_TAG);
}

static FORCEINLINE VOID
__BusFree(
    _In_ PVOID  Buffer
    )
{
    __FreePoolWithTag(Buffer, BUS_TAG);
}

static VOID
BusReference(
    _In_ PVOID          _Context
    )
{
    PXENBUS_BUS_CONTEXT Context = _Context;

    InterlockedIncrement(&Context->References);
}

static VOID
BusDereference(
    _In_ PVOID          _Context
    )
{
    PXENBUS_BUS_CONTEXT Context = _Context;

    ASSERT(Context->References != 0);
    InterlockedDecrement(&Context->References);
}

static TRANSLATE_BUS_ADDRESS BusTranslateAddress;

_Use_decl_annotations_
static BOOLEAN
BusTranslateAddress(
    PVOID                       _Context,
    PHYSICAL_ADDRESS            BusAddress,
    ULONG                       Length,
    PULONG                      AddressSpace,
    PPHYSICAL_ADDRESS           TranslatedAddress
    )
{
    PXENBUS_BUS_CONTEXT         Context = _Context;

    ASSERT(Context != NULL);

    return PdoTranslateBusAddress(Context->Pdo,
                                  BusAddress,
                                  Length,
                                  AddressSpace,
                                  TranslatedAddress);
}

static GET_DMA_ADAPTER BusGetDmaAdapter;

_Use_decl_annotations_
static PDMA_ADAPTER
BusGetDmaAdapter(
    PVOID                       _Context,
    PDEVICE_DESCRIPTION         DeviceDescriptor,
    PULONG                      NumberOfMapRegisters
    )
{
    PXENBUS_BUS_CONTEXT         Context = _Context;
    XENBUS_DMA_ADAPTER_TYPE     Type;

    ASSERT(Context != NULL);

    if (Context->InterceptDmaAdapter != 0) {
        RTL_OSVERSIONINFOEXW    VersionInformation;
        NTSTATUS                status;

        status = RtlGetVersion((PRTL_OSVERSIONINFOW)&VersionInformation);
        ASSERT(NT_SUCCESS(status));

        if (VersionInformation.dwMajorVersion == 6 &&
            VersionInformation.dwMinorVersion == 0) {
            // Windows Vista & Server 2008
            Type = XENBUS_DMA_ADAPTER_SUBSTITUTE;
        } else {
            Type = XENBUS_DMA_ADAPTER_PASSTHRU;
        }
    } else {
        Type = XENBUS_DMA_ADAPTER_NO_INTERCEPT;
    }

    return DmaGetAdapter(Context->Pdo,
                         Type,
                         DeviceDescriptor,
                         NumberOfMapRegisters);
}

static GET_SET_DEVICE_DATA BusSetData;

_Use_decl_annotations_
static ULONG
BusSetData(
    PVOID               _Context,
    ULONG               DataType,
    PVOID               Buffer,
    ULONG               Offset,
    ULONG               Length
    )
{
    PXENBUS_BUS_CONTEXT Context = _Context;

    ASSERT(Context != NULL);

    return PdoSetBusData(Context->Pdo,
                         DataType,
                         Buffer,
                         Offset,
                         Length);
}

static GET_SET_DEVICE_DATA BusGetData;

_Use_decl_annotations_
static ULONG
BusGetData(
    PVOID               _Context,
    ULONG               DataType,
    PVOID               Buffer,
    ULONG               Offset,
    ULONG               Length
    )
{
    PXENBUS_BUS_CONTEXT Context = _Context;

    ASSERT(Context != NULL);

    return PdoGetBusData(Context->Pdo,
                         DataType,
                         Buffer,
                         Offset,
                         Length);
}

NTSTATUS
BusInitialize(
    _In_ PXENBUS_PDO                Pdo,
    _Out_ PBUS_INTERFACE_STANDARD   Interface
    )
{
    PXENBUS_BUS_CONTEXT             Context;
    HANDLE                          ParametersKey;
    ULONG                           InterceptDmaAdapter;
    NTSTATUS                        status;

    Trace("====>\n");

    Context = __BusAllocate(sizeof (XENBUS_BUS_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (Context == NULL)
        goto fail1;

    Context->Pdo = Pdo;

    ParametersKey = DriverGetParametersKey();

    Context->InterceptDmaAdapter = 0;

    status = RegistryQueryDwordValue(ParametersKey,
                                     "InterceptDmaAdapter",
                                     &InterceptDmaAdapter);
    if (NT_SUCCESS(status))
        Context->InterceptDmaAdapter = InterceptDmaAdapter;

    Interface->Size = sizeof (BUS_INTERFACE_STANDARD);
    Interface->Version = 1;
    Interface->Context = Context;
    Interface->InterfaceReference = BusReference;
    Interface->InterfaceDereference = BusDereference;
    Interface->TranslateBusAddress = BusTranslateAddress;
    Interface->GetDmaAdapter = BusGetDmaAdapter;
    Interface->SetBusData = BusSetData;
    Interface->GetBusData = BusGetData;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
BusTeardown(
    _Inout_ PBUS_INTERFACE_STANDARD Interface
    )
{
    PXENBUS_BUS_CONTEXT             Context = Interface->Context;

    Trace("====>\n");

    Context->Pdo = NULL;

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_BUS_CONTEXT)));
    __BusFree(Context);

    RtlZeroMemory(Interface, sizeof (BUS_INTERFACE_STANDARD));

    Trace("<====\n");
}
