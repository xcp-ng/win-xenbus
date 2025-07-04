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
#include <procgrp.h>
#include <xen.h>

#include "shared_info.h"
#include "fdo.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

#define XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR     (sizeof (ULONG_PTR) * 8)
#define XENBUS_SHARED_INFO_EVTCHN_SELECTOR_COUNT   (RTL_FIELD_SIZE(shared_info_t, evtchn_pending) / sizeof (ULONG_PTR))

typedef struct _XENBUS_SHARED_INFO_PROCESSOR {
    unsigned int    vcpu_id;
    vcpu_info_t     *Vcpu;
    ULONG           Port;
} XENBUS_SHARED_INFO_PROCESSOR, *PXENBUS_SHARED_INFO_PROCESSOR;

struct _XENBUS_SHARED_INFO_CONTEXT {
    PXENBUS_FDO                     Fdo;
    KSPIN_LOCK                      Lock;
    LONG                            References;
    PMDL                            Mdl;
    shared_info_t                   *Shared;
    PXENBUS_SHARED_INFO_PROCESSOR   Processor;
    ULONG                           ProcessorCount;
    XENBUS_SUSPEND_INTERFACE        SuspendInterface;
    PXENBUS_SUSPEND_CALLBACK        SuspendCallbackEarly;
    XENBUS_DEBUG_INTERFACE          DebugInterface;
    PXENBUS_DEBUG_CALLBACK          DebugCallback;
};

#define XENBUS_SHARED_INFO_TAG 'OFNI'

static FORCEINLINE PVOID
__SharedInfoAllocate(
    _In_ ULONG  Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENBUS_SHARED_INFO_TAG);
}

static FORCEINLINE VOID
__SharedInfoFree(
    _In_ PVOID  Buffer
    )
{
    __FreePoolWithTag(Buffer, XENBUS_SHARED_INFO_TAG);
}

static BOOLEAN
SharedInfoSetBit(
    _In_ ULONG_PTR volatile *Mask,
    _In_ ULONG              Bit
    )
{
    ASSERT3U(Bit, <, sizeof (ULONG_PTR) * 8);

    KeMemoryBarrier();

    // return TRUE if we set the bit
    return (InterlockedBitTestAndSet((LONG *)Mask, Bit) == 0) ? TRUE : FALSE;
}

static BOOLEAN
SharedInfoClearBit(
    _In_ ULONG_PTR volatile *Mask,
    _In_ ULONG              Bit
    )
{
    ASSERT3U(Bit, <, sizeof (ULONG_PTR) * 8);

    KeMemoryBarrier();

    // return TRUE if we cleared the bit
    return (InterlockedBitTestAndReset((LONG *)Mask, Bit) != 0) ? TRUE : FALSE;
}

static BOOLEAN
SharedInfoClearBitUnlocked(
    _In_ ULONG_PTR  *Mask,
    _In_ ULONG      Bit
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
    _In_ ULONG_PTR  *Mask,
    _In_ ULONG      Bit
    )
{
    ASSERT3U(Bit, <, sizeof (ULONG_PTR) * 8);

    KeMemoryBarrier();

    return (*Mask & ((ULONG_PTR)1 << Bit)) ? TRUE : FALSE;    // return TRUE if the bit is set
}

static VOID
SharedInfoEvtchnMaskAll(
    _In_ PXENBUS_SHARED_INFO_CONTEXT    Context
    )
{
    shared_info_t                       *Shared;
    ULONG                               Port;

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
SharedInfoUpcallSupported(
    _In_ PINTERFACE                 Interface,
    _In_ ULONG                      Index
    )
{
    PXENBUS_SHARED_INFO_CONTEXT     Context = Interface->Context;
    PXENBUS_SHARED_INFO_PROCESSOR   Processor = &Context->Processor[Index];

    ASSERT3U(Index, <, Context->ProcessorCount);

    return (Processor->Vcpu != NULL) ? TRUE : FALSE;
}

static BOOLEAN
SharedInfoUpcallPending(
    _In_ PINTERFACE                 Interface,
    _In_ ULONG                      Index
    )
{
    PXENBUS_SHARED_INFO_CONTEXT     Context = Interface->Context;
    PXENBUS_SHARED_INFO_PROCESSOR   Processor = &Context->Processor[Index];
    vcpu_info_t                     *Vcpu;
    UCHAR                           Pending;

    ASSERT3U(Index, <, Context->ProcessorCount);

    if (Processor->Vcpu == NULL)
        return FALSE;

    Vcpu = Processor->Vcpu;

    KeMemoryBarrier();

    Pending = _InterlockedExchange8((CHAR *)&Vcpu->evtchn_upcall_pending, 0);

    return (Pending != 0) ? TRUE : FALSE;
}

static BOOLEAN
SharedInfoEvtchnPoll(
    _In_ PINTERFACE                 Interface,
    _In_ ULONG                      Index,
    _In_ XENBUS_SHARED_INFO_EVENT   Event,
    _In_ PVOID                      Argument
    )
{
    PXENBUS_SHARED_INFO_CONTEXT     Context = Interface->Context;
    PXENBUS_SHARED_INFO_PROCESSOR   Processor = &Context->Processor[Index];
    shared_info_t                   *Shared = Context->Shared;
    unsigned int                    vcpu_id;
    vcpu_info_t                     *Vcpu;
    ULONG                           Port;
    ULONG_PTR                       SelectorMask;
    BOOLEAN                         DoneSomething;

    DoneSomething = FALSE;

    ASSERT3U(Index, <, Context->ProcessorCount);

    if (Processor->Vcpu == NULL)
        goto done;

    vcpu_id = Processor->vcpu_id;
    Vcpu = Processor->Vcpu;

    KeMemoryBarrier();

    SelectorMask = (ULONG_PTR)InterlockedExchangePointer((PVOID *)&Vcpu->evtchn_pending_sel, (PVOID)0);

    KeMemoryBarrier();

    Port = Processor->Port;

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

    Processor->Port = Port;

done:
    return DoneSomething;
}

static VOID
SharedInfoEvtchnAck(
    _In_ PINTERFACE             Interface,
    _In_ ULONG                  Port
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
    _In_ PINTERFACE             Interface,
    _In_ ULONG                  Port
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
    _In_ PINTERFACE             Interface,
    _In_ ULONG                  Port
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

static VOID
SharedInfoGetTime(
    _In_ PINTERFACE                 Interface,
    _Out_ PLARGE_INTEGER            Time,
    _Out_opt_ PBOOLEAN              Local
    )
{
#define NS_PER_S 1000000000ull

    PXENBUS_SHARED_INFO_CONTEXT     Context = Interface->Context;
    PXENBUS_SHARED_INFO_PROCESSOR   Processor = &Context->Processor[0];
    shared_info_t                   *Shared;
    vcpu_info_t                     *Vcpu;
    ULONG                           WcVersion;
    ULONG                           TimeVersion;
    ULONGLONG                       Seconds;
    ULONGLONG                       NanoSeconds;
    ULONGLONG                       Timestamp;
    ULONGLONG                       Tsc;
    ULONGLONG                       SystemTime;
    ULONG                           TscSystemMul;
    CHAR                            TscShift;
    TIME_FIELDS                     TimeFields;
    KIRQL                           Irql;

    // Make sure we don't suspend
    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    Shared = Context->Shared;
    Vcpu = Processor->Vcpu;
    ASSERT(Vcpu != NULL);

    // Loop until we can read a consistent set of values from the same update
    do {
        WcVersion = Shared->wc_version;
        TimeVersion = Vcpu->time.version;
        KeMemoryBarrier();

        // Wallclock time at system time zero (guest boot or resume)
        Seconds = Shared->wc_sec;
        NanoSeconds = Shared->wc_nsec;

        // Cached time in nanoseconds since guest boot
        SystemTime = Vcpu->time.system_time;

        // Timestamp counter value when these time values were last updated
        Timestamp = Vcpu->time.tsc_timestamp;

        // Timestamp modifiers
        TscShift = Vcpu->time.tsc_shift;
        TscSystemMul = Vcpu->time.tsc_to_system_mul;
        KeMemoryBarrier();

    // Version is incremented to indicate update in progress.
    // LSB of version is set if update in progress.
    // Version is incremented again once update has completed.
    } while (Shared->wc_version != WcVersion ||
             Vcpu->time.version != TimeVersion ||
             (WcVersion & 1) ||
             (TimeVersion & 1));

    // Read counter ticks
    Tsc = __rdtsc();

    KeLowerIrql(Irql);

    // Number of elapsed ticks since timestamp was captured
    Tsc -= Timestamp;

    // Time in nanoseconds since boot
    SystemTime += ((Tsc << TscShift) * TscSystemMul) >> 32;

    Trace("WALLCLOCK TIME AT BOOT: Seconds = %llu NanoSeconds = %llu\n",
          Seconds,
          NanoSeconds);

    Trace("TIME SINCE BOOT: Seconds = %llu NanoSeconds = %llu\n",
          SystemTime / NS_PER_S,
          SystemTime % NS_PER_S);

    // Convert wallclock from Unix epoch (1970) to Windows epoch (1601)
    Seconds += 11644473600ull;

    // Add in time since host boot
    Seconds += SystemTime / NS_PER_S;
    NanoSeconds += SystemTime % NS_PER_S;

    Time->QuadPart = ((Seconds * NS_PER_S) + NanoSeconds) / 100;

    RtlTimeToTimeFields(Time, &TimeFields);

    Trace("TOD: %04u/%02u/%02u %02u:%02u:%02u\n",
          TimeFields.Year,
          TimeFields.Month,
          TimeFields.Day,
          TimeFields.Hour,
          TimeFields.Minute,
          TimeFields.Second);

    if ( Local )
        *Local = !SystemRealTimeIsUniversal();

#undef NS_PER_S
}

static LARGE_INTEGER
SharedInfoGetTimeVersion2(
    _In_ PINTERFACE Interface
    )
{
    LARGE_INTEGER   Time;

    SharedInfoGetTime(Interface, &Time, NULL);

    return Time;
}

static VOID
SharedInfoMap(
    _In_ PXENBUS_SHARED_INFO_CONTEXT    Context
    )
{
    PFN_NUMBER                          Pfn;
    PHYSICAL_ADDRESS                    Address;
    NTSTATUS                            status;

    Pfn = MmGetMdlPfnArray(Context->Mdl)[0];

    status = MemoryAddToPhysmap(Pfn, XENMAPSPACE_shared_info, 0);
    ASSERT(NT_SUCCESS(status));

    Address.QuadPart = Pfn << PAGE_SHIFT;

    LogPrintf(LOG_LEVEL_INFO,
              "SHARED_INFO: MAP XENMAPSPACE_shared_info @ %08x.%08x\n",
              Address.HighPart,
              Address.LowPart);
}

static VOID
SharedInfoUnmap(
    _In_ PXENBUS_SHARED_INFO_CONTEXT    Context
    )
{
    PFN_NUMBER                          Pfn;

    LogPrintf(LOG_LEVEL_INFO,
              "SHARED_INFO: UNMAP XENMAPSPACE_shared_info\n");

    Pfn = MmGetMdlPfnArray(Context->Mdl)[0];

    (VOID) MemoryRemoveFromPhysmap(Pfn);
}

static VOID
SharedInfoSuspendCallbackEarly(
    _In_ PVOID                  Argument
    )
{
    PXENBUS_SHARED_INFO_CONTEXT Context = Argument;

    SharedInfoMap(Context);
    SharedInfoEvtchnMaskAll(Context);
}

static VOID
SharedInfoDebugCallback(
    _In_ PVOID                  Argument,
    _In_ BOOLEAN                Crashing
    )
{
    PXENBUS_SHARED_INFO_CONTEXT Context = Argument;
    PFN_NUMBER                  Pfn;
    PHYSICAL_ADDRESS            Address;

    Pfn = MmGetMdlPfnArray(Context->Mdl)[0];
    Address.QuadPart = Pfn << PAGE_SHIFT;

    XENBUS_DEBUG(Printf,
                 &Context->DebugInterface,
                 "Address = %08x.%08x\n",
                 Address.HighPart,
                 Address.LowPart);

    if (!Crashing) {
        shared_info_t   *Shared;
        ULONG           Index;
        ULONG           Selector;

        Shared = Context->Shared;

        KeMemoryBarrier();

        for (Index = 0;
             Index < KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
             Index++) {
            PROCESSOR_NUMBER    ProcNumber;
            vcpu_info_t         *Vcpu;
            NTSTATUS            status;

            status = SystemProcessorVcpuInfo(Index, &Vcpu);
            if (!NT_SUCCESS(status))
                continue;

            status = KeGetProcessorNumberFromIndex(Index, &ProcNumber);
            ASSERT(NT_SUCCESS(status));

            XENBUS_DEBUG(Printf,
                         &Context->DebugInterface,
                         "CPU %u:%u: PENDING: %s\n",
                         ProcNumber.Group,
                         ProcNumber.Number,
                         Vcpu->evtchn_upcall_pending ?
                         "TRUE" :
                         "FALSE");

            XENBUS_DEBUG(Printf,
                         &Context->DebugInterface,
                         "CPU %u:%u: SELECTOR MASK: %p\n",
                         ProcNumber.Group,
                         ProcNumber.Number,
                         (PVOID)Vcpu->evtchn_pending_sel);
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
    _In_ PINTERFACE                 Interface
    )
{
    PXENBUS_SHARED_INFO_CONTEXT     Context = Interface->Context;
    PXENBUS_FDO                     Fdo = Context->Fdo;
    KIRQL                           Irql;
    shared_info_t                   *Shared;
    LONG                            Index;
    PXENBUS_SHARED_INFO_PROCESSOR   Processor;
    NTSTATUS                        status;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (Context->References++ != 0)
        goto done;

    Trace("====>\n");

    Context->Mdl = FdoHoleAllocate(Fdo, 1);

    status = STATUS_NO_MEMORY;
    if (Context->Mdl == NULL)
        goto fail1;

    Context->Shared = Context->Mdl->StartVa;

    SharedInfoMap(Context);
    SharedInfoEvtchnMaskAll(Context);

    status = XENBUS_SUSPEND(Acquire, &Context->SuspendInterface);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = XENBUS_SUSPEND(Register,
                            &Context->SuspendInterface,
                            SUSPEND_CALLBACK_EARLY,
                            SharedInfoSuspendCallbackEarly,
                            Context,
                            &Context->SuspendCallbackEarly);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = XENBUS_DEBUG(Acquire, &Context->DebugInterface);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = XENBUS_DEBUG(Register,
                          &Context->DebugInterface,
                          __MODULE__ "|SHARED_INFO",
                          SharedInfoDebugCallback,
                          Context,
                          &Context->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail5;

    Context->ProcessorCount = KeQueryMaximumProcessorCountEx(ALL_PROCESSOR_GROUPS);
    Context->Processor = __SharedInfoAllocate(sizeof (XENBUS_SHARED_INFO_PROCESSOR) * Context->ProcessorCount);

    status = STATUS_NO_MEMORY;
    if (Context->Processor == NULL)
        goto fail6;

    Shared = Context->Shared;

    for (Index = 0; Index < (LONG)Context->ProcessorCount; Index++) {
        Processor = &Context->Processor[Index];

        status = SystemProcessorVcpuId(Index, &Processor->vcpu_id);
        if (status == STATUS_NOT_SUPPORTED)
            continue;
        if (!NT_SUCCESS(status))
            goto fail7;

        status = SystemProcessorVcpuInfo(Index, &Processor->Vcpu);
        if (!NT_SUCCESS(status)) {
            if (status != STATUS_NOT_SUPPORTED)
                goto fail8;

            if (Processor->vcpu_id >= ARRAYSIZE(Shared->vcpu_info))
                continue;

            Processor->Vcpu = &Shared->vcpu_info[Processor->vcpu_id];
        }
    }

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;

fail8:
    Error("fail8\n");

    Processor->vcpu_id = 0;

fail7:
    Error("fail7\n");

    while (--Index >= 0) {
        Processor = &Context->Processor[Index];

        Processor->Vcpu = NULL;
        Processor->vcpu_id = 0;
    }

    ASSERT(IsZeroMemory(Context->Processor, sizeof (XENBUS_SHARED_INFO_PROCESSOR) * Context->ProcessorCount));
    __SharedInfoFree(Context->Processor);
    Context->Processor = NULL;

fail6:
    Error("fail6\n");

    Context->ProcessorCount = 0;

    XENBUS_DEBUG(Deregister,
                 &Context->DebugInterface,
                 Context->DebugCallback);
    Context->DebugCallback = NULL;

fail5:
    Error("fail5\n");

    XENBUS_DEBUG(Release, &Context->DebugInterface);

fail4:
    Error("fail4\n");

    XENBUS_SUSPEND(Deregister,
                   &Context->SuspendInterface,
                   Context->SuspendCallbackEarly);
    Context->SuspendCallbackEarly = NULL;

fail3:
    Error("fail3\n");

    XENBUS_SUSPEND(Release, &Context->SuspendInterface);

fail2:
    Error("fail2\n");

    SharedInfoUnmap(Context);

    Context->Shared = NULL;

    FdoHoleFree(Fdo, Context->Mdl);
    Context->Mdl = NULL;

fail1:
    Error("fail1 (%08x)\n", status);

    --Context->References;
    ASSERT3U(Context->References, ==, 0);
    KeReleaseSpinLock(&Context->Lock, Irql);

    return status;
}

static VOID
SharedInfoRelease (
    _In_ PINTERFACE                 Interface
    )
{
    PXENBUS_SHARED_INFO_CONTEXT     Context = Interface->Context;
    PXENBUS_FDO                     Fdo = Context->Fdo;
    KIRQL                           Irql;
    LONG                            Index;
    PXENBUS_SHARED_INFO_PROCESSOR   Processor;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (--Context->References > 0)
        goto done;

    Trace("====>\n");

    Index = (LONG)Context->ProcessorCount;
    while (--Index >= 0) {
        Processor = &Context->Processor[Index];

        Processor->Port = 0;
        Processor->Vcpu = NULL;
        Processor->vcpu_id = 0;
    }

    ASSERT(IsZeroMemory(Context->Processor, sizeof (XENBUS_SHARED_INFO_PROCESSOR) * Context->ProcessorCount));
    __SharedInfoFree(Context->Processor);
    Context->Processor = NULL;
    Context->ProcessorCount = 0;

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

    SharedInfoUnmap(Context);

    Context->Shared = NULL;

    FdoHoleFree(Fdo, Context->Mdl);
    Context->Mdl = NULL;

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);
}

static struct _XENBUS_SHARED_INFO_INTERFACE_V2 SharedInfoInterfaceVersion2 = {
    { sizeof (struct _XENBUS_SHARED_INFO_INTERFACE_V2), 2, NULL, NULL, NULL },
    SharedInfoAcquire,
    SharedInfoRelease,
    SharedInfoUpcallPending,
    SharedInfoEvtchnPoll,
    SharedInfoEvtchnAck,
    SharedInfoEvtchnMask,
    SharedInfoEvtchnUnmask,
    SharedInfoGetTimeVersion2
};

static struct _XENBUS_SHARED_INFO_INTERFACE_V3 SharedInfoInterfaceVersion3 = {
    { sizeof (struct _XENBUS_SHARED_INFO_INTERFACE_V3), 3, NULL, NULL, NULL },
    SharedInfoAcquire,
    SharedInfoRelease,
    SharedInfoUpcallPending,
    SharedInfoEvtchnPoll,
    SharedInfoEvtchnAck,
    SharedInfoEvtchnMask,
    SharedInfoEvtchnUnmask,
    SharedInfoGetTime
};

static struct _XENBUS_SHARED_INFO_INTERFACE_V4 SharedInfoInterfaceVersion4 = {
    { sizeof (struct _XENBUS_SHARED_INFO_INTERFACE_V4), 4, NULL, NULL, NULL },
    SharedInfoAcquire,
    SharedInfoRelease,
    SharedInfoUpcallSupported,
    SharedInfoUpcallPending,
    SharedInfoEvtchnPoll,
    SharedInfoEvtchnAck,
    SharedInfoEvtchnMask,
    SharedInfoEvtchnUnmask,
    SharedInfoGetTime
};

NTSTATUS
SharedInfoInitialize(
    _In_ PXENBUS_FDO                        Fdo,
    _Outptr_ PXENBUS_SHARED_INFO_CONTEXT    *Context
    )
{
    NTSTATUS                                status;

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
    _In_ PXENBUS_SHARED_INFO_CONTEXT    Context,
    _In_ ULONG                          Version,
    _Inout_ PINTERFACE                  Interface,
    _In_ ULONG                          Size
    )
{
    NTSTATUS                            status;

    ASSERT(Context != NULL);

    switch (Version) {
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
    case 3: {
        struct _XENBUS_SHARED_INFO_INTERFACE_V3 *SharedInfoInterface;

        SharedInfoInterface = (struct _XENBUS_SHARED_INFO_INTERFACE_V3 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_SHARED_INFO_INTERFACE_V3))
            break;

        *SharedInfoInterface = SharedInfoInterfaceVersion3;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    case 4: {
        struct _XENBUS_SHARED_INFO_INTERFACE_V4 *SharedInfoInterface;

        SharedInfoInterface = (struct _XENBUS_SHARED_INFO_INTERFACE_V4 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_SHARED_INFO_INTERFACE_V4))
            break;

        *SharedInfoInterface = SharedInfoInterfaceVersion4;

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

ULONG
SharedInfoGetReferences(
    _In_ PXENBUS_SHARED_INFO_CONTEXT    Context
    )
{
    return Context->References;
}

VOID
SharedInfoTeardown(
    _In_ PXENBUS_SHARED_INFO_CONTEXT    Context
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
