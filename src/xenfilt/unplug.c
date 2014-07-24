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
#include <stdlib.h>
#include <stdarg.h>
#include <xen.h>
#include <util.h>
#include <version.h>

#include "driver.h"
#include "high.h"
#include "registry.h"
#include "unplug.h"
#include "dbg_print.h"
#include "assert.h"

struct _XENFILT_UNPLUG_CONTEXT {
    KSPIN_LOCK  Lock;
    LONG        References;
    HIGH_LOCK   UnplugLock;
    BOOLEAN     BlackListed;
    BOOLEAN     UnplugDisks;
    BOOLEAN     UnplugNics;
    BOOLEAN     BootEmulated;
};

typedef enum _XENFILT_UNPLUG_TYPE {
    XENFILT_UNPLUG_DISKS = 0,
    XENFILT_UNPLUG_NICS
} XENFILT_UNPLUG_TYPE, *PXENFILT_UNPLUG_TYPE;

#define XENFILT_UNPLUG_TAG  'LPNU'

static FORCEINLINE PVOID
__UnplugAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENFILT_UNPLUG_TAG);
}

static FORCEINLINE VOID
__UnplugFree(
    IN  PVOID   Buffer
    )
{
    ExFreePoolWithTag(Buffer, XENFILT_UNPLUG_TAG);
}

static VOID
UnplugGetFlags(
    IN  PXENFILT_UNPLUG_CONTEXT Context
    )
{
    HANDLE                      Key;
    DWORD                       Value;
    NTSTATUS                    status;

    Context->BootEmulated = FALSE;

    Key = DriverGetParametersKey();

    status = RegistryQueryDwordValue(Key,
                                     "BootEmulated",
                                     &Value);
    if (NT_SUCCESS(status)) {
        LogPrintf(LOG_LEVEL_WARNING,
                  "UNPLUG: BOOT_EMULATED %d\n",
                  Value);

        Context->BootEmulated = (Value == 1) ? TRUE : FALSE;
    }
}

static VOID
UnplugRequest(
    IN  PXENFILT_UNPLUG_CONTEXT Context,
    IN  XENFILT_UNPLUG_TYPE     Type
    )
{
    switch (Type) {
    case XENFILT_UNPLUG_DISKS:
        if (Context->BootEmulated) {
#pragma prefast(suppress:28138)
            WRITE_PORT_USHORT((PUSHORT)0x10, 0x0004);

            LogPrintf(LOG_LEVEL_WARNING, "UNPLUG: AUX DISKS\n");
        } else {
#pragma prefast(suppress:28138)
            WRITE_PORT_USHORT((PUSHORT)0x10, 0x0001);

            LogPrintf(LOG_LEVEL_WARNING, "UNPLUG: DISKS\n");
        }
        break;
    case XENFILT_UNPLUG_NICS:
#pragma prefast(suppress:28138)
        WRITE_PORT_USHORT((PUSHORT)0x10, 0x0002);

        LogPrintf(LOG_LEVEL_WARNING, "UNPLUG: NICS\n");
        break;
    default:
        ASSERT(FALSE);
    }
}

static NTSTATUS
UnplugPreamble(
    IN  PXENFILT_UNPLUG_CONTEXT Context
    )
{
    USHORT                      Magic;
    UCHAR                       Version;
    NTSTATUS                    status;

    // See docs/misc/hvm-emulated-unplug.markdown for details of the
    // protocol in use here

#pragma prefast(suppress:28138)
    Magic = READ_PORT_USHORT((PUSHORT)0x10);
    
    if (Magic == 0xd249) {
        Context->BlackListed = TRUE;
        goto done;
    }

    status = STATUS_NOT_SUPPORTED;
    if (Magic != 0x49d2)
        goto fail1;

#pragma prefast(suppress:28138)
    Version = READ_PORT_UCHAR((PUCHAR)0x12);
    if (Version != 0) {
#pragma prefast(suppress:28138)
        WRITE_PORT_USHORT((PUSHORT)0x12, 0xFFFF);   // FIXME

#pragma prefast(suppress:28138)
        WRITE_PORT_ULONG((PULONG)0x10, 
                         (MAJOR_VERSION << 16) |
                         (MINOR_VERSION << 8) |
                         MICRO_VERSION);

#pragma prefast(suppress:28138)
        Magic = READ_PORT_USHORT((PUSHORT)0x10);
        if (Magic == 0xd249)
            Context->BlackListed = TRUE;
    }

done:
    LogPrintf(LOG_LEVEL_WARNING,
              "UNPLUG: PRE-AMBLE (DRIVERS %s)\n",
              (Context->BlackListed) ? "BLACKLISTED" : "NOT BLACKLISTED");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

#define HKEY_LOCAL_MACHINE  "\\Registry\\Machine"
#define SERVICES_KEY        HKEY_LOCAL_MACHINE "\\SYSTEM\\CurrentControlSet\\Services"

static VOID
UnplugCheckForPVDisks(
    IN  PXENFILT_UNPLUG_CONTEXT Context
    )
{
    HANDLE                      UnplugKey;
    PANSI_STRING                ServiceNames;
    ULONG                       Count;
    ULONG                       Index;
    KIRQL                       Irql;
    NTSTATUS                    status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    UnplugKey = DriverGetUnplugKey();

    ServiceNames = NULL;

    status = RegistryQuerySzValue(UnplugKey,
                                  "DISKS",
                                  &ServiceNames);
    if (!NT_SUCCESS(status))
        goto done;

    Count = 0;
    for (Index = 0; ServiceNames[Index].Buffer != NULL; Index++)
        if (_stricmp(ServiceNames[Index].Buffer, "XENVBD") == 0)
            Count++;

    if (Count < 1)
        goto done;

    AcquireHighLock(&Context->UnplugLock, &Irql);
    Context->UnplugDisks = TRUE;
    ReleaseHighLock(&Context->UnplugLock, Irql);

done:
    Info("%s\n", (Context->UnplugDisks) ? "PRESENT" : "NOT PRESENT");

    if (ServiceNames != NULL)
        RegistryFreeSzValue(ServiceNames);
}

static VOID
UnplugCheckForPVNics(
    IN  PXENFILT_UNPLUG_CONTEXT Context
    )
{
    HANDLE                      UnplugKey;
    PANSI_STRING                ServiceNames;
    ULONG                       Count;
    ULONG                       Index;
    KIRQL                       Irql;
    NTSTATUS                    status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    UnplugKey = DriverGetUnplugKey();

    ServiceNames = NULL;

    status = RegistryQuerySzValue(UnplugKey,
                                  "NICS",
                                  &ServiceNames);
    if (!NT_SUCCESS(status))
        goto done;

    Count = 0;
    for (Index = 0; ServiceNames[Index].Buffer != NULL; Index++)
        if (_stricmp(ServiceNames[Index].Buffer, "XENVIF") == 0 ||
            _stricmp(ServiceNames[Index].Buffer, "XENNET") == 0)
            Count++;

    if (Count < 2)
        goto done;

    AcquireHighLock(&Context->UnplugLock, &Irql);
    Context->UnplugNics = TRUE;
    ReleaseHighLock(&Context->UnplugLock, Irql);

done:
    Info("%s\n", (Context->UnplugNics) ? "PRESENT" : "NOT PRESENT");

    if (ServiceNames != NULL)
        RegistryFreeSzValue(ServiceNames);
}

static VOID
UnplugReplay(
    IN  PINTERFACE          Interface
    )
{
    PXENFILT_UNPLUG_CONTEXT Context = Interface->Context;
    KIRQL                   Irql;
    NTSTATUS                status;

    AcquireHighLock(&Context->UnplugLock, &Irql);

    status = UnplugPreamble(Context);
    ASSERT(NT_SUCCESS(status));

    if (Context->UnplugDisks)
        UnplugRequest(Context, XENFILT_UNPLUG_DISKS);

    if (Context->UnplugNics)
        UnplugRequest(Context, XENFILT_UNPLUG_NICS);
    
    ReleaseHighLock(&Context->UnplugLock, Irql);
}

NTSTATUS
UnplugAcquire(
    IN  PINTERFACE          Interface
    )
{
    PXENFILT_UNPLUG_CONTEXT Context = Interface->Context;
    KIRQL                   Irql;
    NTSTATUS                status;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (Context->References++ != 0)
        goto done;

    Trace("====>\n");

    (VOID)__AcquireHighLock(&Context->UnplugLock);

    status = UnplugPreamble(Context);
    if (!NT_SUCCESS(status))
        goto fail1;

    if (Context->UnplugDisks)
        UnplugRequest(Context, XENFILT_UNPLUG_DISKS);

    if (Context->UnplugNics)
        UnplugRequest(Context, XENFILT_UNPLUG_NICS);
    
    ReleaseHighLock(&Context->UnplugLock, DISPATCH_LEVEL);

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    ReleaseHighLock(&Context->UnplugLock, DISPATCH_LEVEL);

    KeReleaseSpinLock(&Context->Lock, Irql);

    return status;
}

VOID
UnplugRelease(
    IN  PINTERFACE              Interface
    )
{
    PXENFILT_UNPLUG_CONTEXT     Context = Interface->Context;
    KIRQL                       Irql;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (--Context->References > 0)
        goto done;

    Trace("====>\n");

    Context->BlackListed = FALSE;

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);
}

static struct _XENFILT_UNPLUG_INTERFACE_V1 UnplugInterfaceVersion1 = {
    { sizeof (struct _XENFILT_UNPLUG_INTERFACE_V1), 1, NULL, NULL, NULL },
    UnplugAcquire,
    UnplugRelease,
    UnplugReplay
};
                     
NTSTATUS
UnplugInitialize(
    OUT PXENFILT_UNPLUG_CONTEXT *Context
    )
{
    NTSTATUS                    status;

    Trace("====>\n");

    *Context = __UnplugAllocate(sizeof (XENFILT_UNPLUG_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (*Context == NULL)
        goto fail1;

    UnplugCheckForPVDisks(*Context);
    UnplugCheckForPVNics(*Context);
    UnplugGetFlags(*Context);

    KeInitializeSpinLock(&(*Context)->Lock);
    InitializeHighLock(&(*Context)->UnplugLock);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
UnplugGetInterface(
    IN      PXENFILT_UNPLUG_CONTEXT Context,
    IN      ULONG                   Version,
    IN OUT  PINTERFACE              Interface,
    IN      ULONG                   Size
    )
{
    NTSTATUS                        status;

    ASSERT(Context != NULL);

    switch (Version) {
    case 1: {
        struct _XENFILT_UNPLUG_INTERFACE_V1 *UnplugInterface;

        UnplugInterface = (struct _XENFILT_UNPLUG_INTERFACE_V1 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENFILT_UNPLUG_INTERFACE_V1))
            break;

        *UnplugInterface = UnplugInterfaceVersion1;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    return status;
}   

VOID
UnplugTeardown(
    IN  PXENFILT_UNPLUG_CONTEXT Context
    )
{
    Trace("====>\n");

    Context->BootEmulated = FALSE;
    Context->UnplugNics = FALSE;
    Context->UnplugDisks = FALSE;

    RtlZeroMemory(&Context->UnplugLock, sizeof (HIGH_LOCK));
    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));

    ASSERT(IsZeroMemory(Context, sizeof (XENFILT_UNPLUG_CONTEXT)));
    __UnplugFree(Context);

    Trace("<====\n");
}
