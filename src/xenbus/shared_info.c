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
#include <xen.h>
#include <util.h>

#include "shared_info.h"
#include "fdo.h"
#include "dbg_print.h"
#include "assert.h"

#define XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR     (sizeof (ULONG_PTR) * 8)
#define XENBUS_SHARED_INFO_EVTCHN_SELECTOR_COUNT   (RTL_FIELD_SIZE(shared_info_t, evtchn_pending) / sizeof (ULONG_PTR))

struct _XENBUS_SHARED_INFO_CONTEXT {
    PXENBUS_FDO                 Fdo;
    KSPIN_LOCK                  Lock;
    LONG                        References;
    PHYSICAL_ADDRESS            Address;
    shared_info_t               *Shared;
    ULONG                       Port;
    XENBUS_SUSPEND_INTERFACE    SuspendInterface;
    PXENBUS_SUSPEND_CALLBACK    SuspendCallbackEarly;
    XENBUS_DEBUG_INTERFACE      DebugInterface;
    PXENBUS_DEBUG_CALLBACK      DebugCallback;
};

#define XENBUS_SHARED_INFO_TAG 'OFNI'

static FORCEINLINE PVOID
__SharedInfoAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENBUS_SHARED_INFO_TAG);
}

static FORCEINLINE VOID
__SharedInfoFree(
    IN  PVOID   Buffer
    )
{
    ExFreePoolWithTag(Buffer, XENBUS_SHARED_INFO_TAG);
}

static BOOLEAN
SharedInfoSetBit(
    IN  ULONG_PTR volatile  *Mask,
    IN  ULONG               Bit
    )
{
    ASSERT3U(Bit, <, sizeof (ULONG_PTR) * 8);

    KeMemoryBarrier();

    // return TRUE if we set the bit
    return (InterlockedBitTestAndSet((LONG *)Mask, Bit) == 0) ? TRUE : FALSE;
}

static BOOLEAN
SharedInfoClearBit(
    IN  ULONG_PTR volatile  *Mask,
    IN  ULONG               Bit
    )
{
    ASSERT3U(Bit, <, sizeof (ULONG_PTR) * 8);

    KeMemoryBarrier();

    // return TRUE if we cleared the bit
    return (InterlockedBitTestAndReset((LONG *)Mask, Bit) != 0) ? TRUE : FALSE;
}

static BOOLEAN
SharedInfoClearBitUnlocked(
    IN  ULONG_PTR   *Mask,
    IN  ULONG       Bit
    )
{
    ULONG_PTR       Old;

    ASSERT3U(Bit, <, sizeof (ULONG_PTR) * 8);

    KeMemoryBarrier();

    Old = *Mask;
    *Mask = Old & ~((ULONG_PTR)1 << Bit);

    return (Old & ((ULONG_PTR)1 << Bit)) ? TRUE : FALSE;    // return TRUE if we cleared the bit
}

static BOOLEAN
SharedInfoTestBit(
    IN  ULONG_PTR   *Mask,
    IN  ULONG       Bit
    )
{
    ASSERT3U(Bit, <, sizeof (ULONG_PTR) * 8);

    KeMemoryBarrier();

    return (*Mask & ((ULONG_PTR)1 << Bit)) ? TRUE : FALSE;    // return TRUE if the bit is set
}

static VOID
SharedInfoEvtchnMaskAll(
    IN  PXENBUS_SHARED_INFO_CONTEXT Context
    )
{
    shared_info_t                   *Shared;
    ULONG                           Port;

    Shared = Context->Shared;

    for (Port = 0;
         Port < XENBUS_SHARED_INFO_EVTCHN_SELECTOR_COUNT * XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR;
         Port += XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR) {
        ULONG SelectorBit;

        SelectorBit = Port / XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR;

        Shared->evtchn_mask[SelectorBit] = (ULONG_PTR)-1;
    }
}

static BOOLEAN
SharedInfoUpcallPending(
    IN  PINTERFACE              Interface,
    IN  ULONG                   Cpu
    )
{
    PXENBUS_SHARED_INFO_CONTEXT Context = Interface->Context;
    shared_info_t               *Shared = Context->Shared;
    int                         vcpu_id = SystemVirtualCpuIndex(Cpu);
    UCHAR                       Pending;

    KeMemoryBarrier();

    Pending = _InterlockedExchange8((CHAR *)&Shared->vcpu_info[vcpu_id].evtchn_upcall_pending, 0);

    return (Pending != 0) ? TRUE : FALSE;
}

static BOOLEAN
SharedInfoEvtchnPoll(
    IN  PINTERFACE                  Interface,
    IN  ULONG                       Cpu,
    IN  XENBUS_SHARED_INFO_EVENT    Event,
    IN  PVOID                       Argument OPTIONAL
    )
{
    PXENBUS_SHARED_INFO_CONTEXT     Context = Interface->Context;
    shared_info_t                   *Shared = Context->Shared;
    int                             vcpu_id = SystemVirtualCpuIndex(Cpu);
    ULONG                           Port;
    ULONG_PTR                       SelectorMask;
    BOOLEAN                         DoneSomething;

    DoneSomething = FALSE;

    KeMemoryBarrier();

    SelectorMask = (ULONG_PTR)InterlockedExchangePointer((PVOID *)&Shared->vcpu_info[vcpu_id].evtchn_pending_sel, (PVOID)0);

    KeMemoryBarrier();

    Port = Context->Port;

    while (SelectorMask != 0) {
        ULONG   SelectorBit;
        ULONG   PortBit;


        SelectorBit = Port / XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR;
        PortBit = Port % XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR;

        if (SharedInfoTestBit(&SelectorMask, SelectorBit)) {
            ULONG_PTR   PortMask;

            PortMask = Shared->evtchn_pending[SelectorBit];
            PortMask &= ~Shared->evtchn_mask[SelectorBit];

            while (PortMask != 0 && PortBit < XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR) {
                if (SharedInfoTestBit(&PortMask, PortBit)) {
                    DoneSomething |= Event(Argument, (SelectorBit * XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR) + PortBit);

                    PortMask &= ~((ULONG_PTR)1 << PortBit);
                }

                PortBit++;
            }

            // Are we done with this selector?
            if (PortMask == 0)
                SelectorMask &= ~((ULONG_PTR)1 << SelectorBit);
        }

        Port = (SelectorBit + 1) * XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR;

        if (Port >= XENBUS_SHARED_INFO_EVTCHN_SELECTOR_COUNT * XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR)
            Port = 0;
    }

    Context->Port = Port;

    return DoneSomething;
}

static BOOLEAN
SharedInfoEvtchnPollVersion1(
    IN  PINTERFACE              Interface,
    IN  BOOLEAN                 (*Function)(PVOID, ULONG),
    IN  PVOID                   Argument OPTIONAL
    )
{
    BOOLEAN                     DoneSomething;

    DoneSomething = FALSE;

    while (SharedInfoUpcallPending(Interface, 0))
        DoneSomething |= SharedInfoEvtchnPoll(Interface,
                                              0,
                                              Function,
                                              Argument);

    return DoneSomething;
}

static VOID
SharedInfoEvtchnAck(
    IN  PINTERFACE              Interface,
    IN  ULONG                   Port
    )
{
    PXENBUS_SHARED_INFO_CONTEXT Context = Interface->Context;
    shared_info_t               *Shared;
    ULONG                       SelectorBit;
    ULONG                       PortBit;

    Shared = Context->Shared;

    SelectorBit = Port / XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR;
    PortBit = Port % XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR;

    (VOID) SharedInfoClearBit(&Shared->evtchn_pending[SelectorBit], PortBit);
}

static VOID
SharedInfoEvtchnMask(
    IN  PINTERFACE              Interface,
    IN  ULONG                   Port
    )
{
    PXENBUS_SHARED_INFO_CONTEXT Context = Interface->Context;
    shared_info_t               *Shared;
    ULONG                       SelectorBit;
    ULONG                       PortBit;

    Shared = Context->Shared;

    SelectorBit = Port / XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR;
    PortBit = Port % XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR;

    (VOID) SharedInfoSetBit(&Shared->evtchn_mask[SelectorBit], PortBit);
}

static BOOLEAN
SharedInfoEvtchnUnmask(
    IN  PINTERFACE              Interface,
    IN  ULONG                   Port
    )
{
    PXENBUS_SHARED_INFO_CONTEXT Context = Interface->Context;
    shared_info_t               *Shared;
    ULONG                       SelectorBit;
    ULONG                       PortBit;

    Shared = Context->Shared;

    SelectorBit = Port / XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR;
    PortBit = Port % XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR;

    (VOID) SharedInfoClearBit(&Shared->evtchn_mask[SelectorBit], PortBit);

    KeMemoryBarrier();

    // If we cleared the mask then check whether something was pending
    return SharedInfoTestBit(&Shared->evtchn_pending[SelectorBit], PortBit);
}

static LARGE_INTEGER
SharedInfoGetTime(
    IN  PINTERFACE              Interface
    )
{
    PXENBUS_SHARED_INFO_CONTEXT Context = Interface->Context;
    shared_info_t               *Shared;
    ULONG                       Version;
    ULONGLONG                   Seconds;
    ULONGLONG                   NanoSeconds;
    LARGE_INTEGER               Now;
    TIME_FIELDS                 Time;
    KIRQL                       Irql;
    NTSTATUS                    status;

    // Make sure we don't suspend
    KeRaiseIrql(DISPATCH_LEVEL, &Irql); 

    Shared = Context->Shared;

    do {
        Version = Shared->wc_version;
        KeMemoryBarrier();

        Seconds = Shared->wc_sec;
        NanoSeconds = Shared->wc_nsec;
        KeMemoryBarrier();
    } while (Shared->wc_version != Version);

    // Get the number of nanoseconds since boot
    status = HvmGetTime(&Now);
    if (!NT_SUCCESS(status))
        Now.QuadPart = Shared->vcpu_info[0].time.system_time;

    KeLowerIrql(Irql);

    Trace("WALLCLOCK: Seconds = %llu NanoSeconds = %llu\n",
          Seconds,
          NanoSeconds);

    Trace("BOOT: Seconds = %llu NanoSeconds = %llu\n",
          Now.QuadPart / 1000000000ull,
          Now.QuadPart % 1000000000ull);

    // Convert wallclock from Unix epoch (1970) to Windows epoch (1601)
    Seconds += 11644473600ull;

    // Add in time since host boot
    Seconds += Now.QuadPart / 1000000000ull;
    NanoSeconds += Now.QuadPart % 1000000000ull;

    // Converto to system time format
    Now.QuadPart = (Seconds * 10000000ull) + (NanoSeconds / 100ull);

    RtlTimeToTimeFields(&Now, &Time);

    Trace("TOD: %04u/%02u/%02u %02u:%02u:%02u\n",
          Time.Year,
          Time.Month,
          Time.Day,
          Time.Hour,
          Time.Minute,
          Time.Second);

    return Now;
}

static VOID
SharedInfoMap(
    IN  PXENBUS_SHARED_INFO_CONTEXT Context
    )
{
    NTSTATUS                        status;

    // This, unfortunately, seems to be a necessary hack to
    // get the domain wallclock updated correctly on older
    // versions of XenServer.
#define HVM_PARAM_32BIT 8

#if defined(__i386__)
    (VOID) HvmSetParam(HVM_PARAM_32BIT, 1);
#elif defined(__x86_64__)
    (VOID) HvmSetParam(HVM_PARAM_32BIT, 0);
#else
#error 'Unrecognised architecture'
#endif

#undef  HVM_PARAM_32BIT

    status = MemoryAddToPhysmap((PFN_NUMBER)(Context->Address.QuadPart >> PAGE_SHIFT),
                                XENMAPSPACE_shared_info,
                                0);
    ASSERT(NT_SUCCESS(status));

    LogPrintf(LOG_LEVEL_INFO,
              "SHARED_INFO: MAP XENMAPSPACE_shared_info @ %08x.%08x\n",
              Context->Address.HighPart,
              Context->Address.LowPart);
}

static VOID
SharedInfoUnmap(
    IN  PXENBUS_SHARED_INFO_CONTEXT Context
    )
{
    UNREFERENCED_PARAMETER(Context);

    LogPrintf(LOG_LEVEL_INFO,
              "SHARED_INFO: UNMAP XENMAPSPACE_shared_info\n");

    // Not clear what to do here
}

static VOID
SharedInfoSuspendCallbackEarly(
    IN  PVOID                   Argument
    )
{
    PXENBUS_SHARED_INFO_CONTEXT Context = Argument;

    SharedInfoMap(Context);
    SharedInfoEvtchnMaskAll(Context);
}

static VOID
SharedInfoDebugCallback(
    IN  PVOID                   Argument,
    IN  BOOLEAN                 Crashing
    )
{
    PXENBUS_SHARED_INFO_CONTEXT Context = Argument;

    XENBUS_DEBUG(Printf,
                 &Context->DebugInterface,
                 "Address = %08x.%08x\n",
                 Context->Address.HighPart,
                 Context->Address.LowPart);

    if (!Crashing) {
        shared_info_t   *Shared;
        LONG            Cpu;
        ULONG           Selector;

        Shared = Context->Shared;

        KeMemoryBarrier();

        for (Cpu = 0; Cpu < KeNumberProcessors; Cpu++) {
            int vcpu_id = SystemVirtualCpuIndex(Cpu);

            XENBUS_DEBUG(Printf,
                         &Context->DebugInterface,
                         "CPU %u: PENDING: %s\n",
                         Cpu,
                         Shared->vcpu_info[vcpu_id].evtchn_upcall_pending ?
                         "TRUE" :
                         "FALSE");

            XENBUS_DEBUG(Printf,
                         &Context->DebugInterface,
                         "CPU %u: SELECTOR MASK: %p\n",
                         Cpu,
                         (PVOID)Shared->vcpu_info[vcpu_id].evtchn_pending_sel);
        }

        for (Selector = 0; Selector < XENBUS_SHARED_INFO_EVTCHN_SELECTOR_COUNT; Selector += 4) {
            XENBUS_DEBUG(Printf,
                         &Context->DebugInterface,
                         " PENDING: [%04x - %04x]: %p %p %p %p\n",
                         Selector * XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR,
                         ((Selector + 4) * XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR) - 1,
                         (PVOID)Shared->evtchn_pending[Selector],
                         (PVOID)Shared->evtchn_pending[Selector + 1],
                         (PVOID)Shared->evtchn_pending[Selector + 2],
                         (PVOID)Shared->evtchn_pending[Selector + 3]);

            XENBUS_DEBUG(Printf,
                         &Context->DebugInterface,
                         "UNMASKED: [%04x - %04x]: %p %p %p %p\n",
                         Selector * XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR,
                         ((Selector + 4) * XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR) - 1,
                         (PVOID)(~Shared->evtchn_mask[Selector]),
                         (PVOID)(~Shared->evtchn_mask[Selector + 1]),
                         (PVOID)(~Shared->evtchn_mask[Selector + 2]),
                         (PVOID)(~Shared->evtchn_mask[Selector + 3]));
        }
    }
}

static NTSTATUS
SharedInfoAcquire(
    IN  PINTERFACE              Interface
    )
{
    PXENBUS_SHARED_INFO_CONTEXT Context = Interface->Context;
    PXENBUS_FDO                 Fdo = Context->Fdo;
    KIRQL                       Irql;
    NTSTATUS                    status;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (Context->References++ != 0)
        goto done;

    Trace("====>\n");

    status = FdoAllocateIoSpace(Fdo, PAGE_SIZE, &Context->Address);
    if (!NT_SUCCESS(status))
        goto fail1;

    SharedInfoMap(Context);

    Context->Shared = (shared_info_t *)MmMapIoSpace(Context->Address,
                                                    PAGE_SIZE,
                                                    MmCached);

    status = STATUS_UNSUCCESSFUL;
    if (Context->Shared == NULL)
        goto fail2;

    SharedInfoEvtchnMaskAll(Context);

    status = XENBUS_SUSPEND(Acquire, &Context->SuspendInterface);
    if (!NT_SUCCESS(status))
        goto fail3;   

    status = XENBUS_SUSPEND(Register,
                            &Context->SuspendInterface,
                            SUSPEND_CALLBACK_EARLY,
                            SharedInfoSuspendCallbackEarly,
                            Context,
                            &Context->SuspendCallbackEarly);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = XENBUS_DEBUG(Acquire, &Context->DebugInterface);
    if (!NT_SUCCESS(status))
        goto fail5;

    status = XENBUS_DEBUG(Register,
                          &Context->DebugInterface,
                          __MODULE__ "|SHARED_INFO",
                          SharedInfoDebugCallback,
                          Context,
                          &Context->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail6;

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;

fail6:
    Error("fail6\n");

    XENBUS_DEBUG(Release, &Context->DebugInterface);

fail5:
    Error("fail5\n");

    XENBUS_SUSPEND(Deregister,
                   &Context->SuspendInterface,
                   Context->SuspendCallbackEarly);
    Context->SuspendCallbackEarly = NULL;

fail4:
    Error("fail4\n");

    XENBUS_SUSPEND(Release, &Context->SuspendInterface);

fail3:
    Error("fail3\n");

    MmUnmapIoSpace(Context->Shared, PAGE_SIZE);
    Context->Shared = NULL;

fail2:
    Error("fail2\n");

    SharedInfoUnmap(Context);

    FdoFreeIoSpace(Fdo, Context->Address, PAGE_SIZE);
    Context->Address.QuadPart = 0;

fail1:
    Error("fail1 (%08x)\n", status);

    --Context->References;
    ASSERT3U(Context->References, ==, 0);
    KeReleaseSpinLock(&Context->Lock, Irql);

    return status;
}

static VOID
SharedInfoRelease (
    IN  PINTERFACE              Interface
    )
{
    PXENBUS_SHARED_INFO_CONTEXT Context = Interface->Context;
    PXENBUS_FDO                 Fdo = Context->Fdo;
    KIRQL                       Irql;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (--Context->References > 0)
        goto done;

    Trace("====>\n");

    Context->Port = 0;

    XENBUS_DEBUG(Deregister,
                 &Context->DebugInterface,
                 Context->DebugCallback);
    Context->DebugCallback = NULL;

    XENBUS_DEBUG(Release, &Context->DebugInterface);

    XENBUS_SUSPEND(Deregister,
                   &Context->SuspendInterface,
                   Context->SuspendCallbackEarly);
    Context->SuspendCallbackEarly = NULL;

    XENBUS_SUSPEND(Release, &Context->SuspendInterface);

    MmUnmapIoSpace(Context->Shared, PAGE_SIZE);
    Context->Shared = NULL;

    SharedInfoUnmap(Context);

    FdoFreeIoSpace(Fdo, Context->Address, PAGE_SIZE);
    Context->Address.QuadPart = 0;

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);
}

static struct _XENBUS_SHARED_INFO_INTERFACE_V1 SharedInfoInterfaceVersion1 = {
    { sizeof (struct _XENBUS_SHARED_INFO_INTERFACE_V1), 1, NULL, NULL, NULL },
    SharedInfoAcquire,
    SharedInfoRelease,
    SharedInfoEvtchnPollVersion1,
    SharedInfoEvtchnAck,
    SharedInfoEvtchnMask,
    SharedInfoEvtchnUnmask,
    SharedInfoGetTime
};

static struct _XENBUS_SHARED_INFO_INTERFACE_V2 SharedInfoInterfaceVersion2 = {
    { sizeof (struct _XENBUS_SHARED_INFO_INTERFACE_V2), 2, NULL, NULL, NULL },
    SharedInfoAcquire,
    SharedInfoRelease,
    SharedInfoUpcallPending,
    SharedInfoEvtchnPoll,
    SharedInfoEvtchnAck,
    SharedInfoEvtchnMask,
    SharedInfoEvtchnUnmask,
    SharedInfoGetTime
};
                     
NTSTATUS
SharedInfoInitialize(
    IN  PXENBUS_FDO                 Fdo,
    OUT PXENBUS_SHARED_INFO_CONTEXT *Context
    )
{
    NTSTATUS                        status;

    Trace("====>\n");

    *Context = __SharedInfoAllocate(sizeof (XENBUS_SHARED_INFO_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (*Context == NULL)
        goto fail1;

    KeInitializeSpinLock(&(*Context)->Lock);

    status = SuspendGetInterface(FdoGetSuspendContext(Fdo),
                                 XENBUS_SUSPEND_INTERFACE_VERSION_MAX,
                                 (PINTERFACE)&(*Context)->SuspendInterface,
                                 sizeof ((*Context)->SuspendInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT((*Context)->SuspendInterface.Interface.Context != NULL);

    status = DebugGetInterface(FdoGetDebugContext(Fdo),
                               XENBUS_DEBUG_INTERFACE_VERSION_MAX,
                               (PINTERFACE)&(*Context)->DebugInterface,
                               sizeof ((*Context)->DebugInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT((*Context)->DebugInterface.Interface.Context != NULL);

    (*Context)->Fdo = Fdo;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
SharedInfoGetInterface(
    IN      PXENBUS_SHARED_INFO_CONTEXT Context,
    IN      ULONG                       Version,
    IN OUT  PINTERFACE                  Interface,
    IN      ULONG                       Size
    )
{
    NTSTATUS                            status;

    ASSERT(Context != NULL);

    switch (Version) {
    case 1: {
        struct _XENBUS_SHARED_INFO_INTERFACE_V1 *SharedInfoInterface;

        SharedInfoInterface = (struct _XENBUS_SHARED_INFO_INTERFACE_V1 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_SHARED_INFO_INTERFACE_V1))
            break;

        *SharedInfoInterface = SharedInfoInterfaceVersion1;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    case 2: {
        struct _XENBUS_SHARED_INFO_INTERFACE_V2 *SharedInfoInterface;

        SharedInfoInterface = (struct _XENBUS_SHARED_INFO_INTERFACE_V2 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_SHARED_INFO_INTERFACE_V2))
            break;

        *SharedInfoInterface = SharedInfoInterfaceVersion2;

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
SharedInfoTeardown(
    IN  PXENBUS_SHARED_INFO_CONTEXT Context
    )
{
    Trace("====>\n");

    Context->Fdo = NULL;

    RtlZeroMemory(&Context->DebugInterface,
                  sizeof (XENBUS_DEBUG_INTERFACE));

    RtlZeroMemory(&Context->SuspendInterface,
                  sizeof (XENBUS_SUSPEND_INTERFACE));

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_SHARED_INFO_CONTEXT)));
    __SharedInfoFree(Context);

    Trace("<====\n");
}

