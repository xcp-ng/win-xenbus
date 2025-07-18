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
#include <stdarg.h>
#include <xen.h>

#include "evtchn_fifo.h"
#include "shared_info.h"
#include "fdo.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

typedef struct _XENBUS_EVTCHN_FIFO_CONTEXT {
    PXENBUS_FDO                     Fdo;
    KSPIN_LOCK                      Lock;
    LONG                            References;
    PMDL                            ControlBlockMdl[HVM_MAX_VCPUS];
    PMDL                            *EventPageMdl;
    ULONG                           EventPageCount;
    ULONG                           Head[HVM_MAX_VCPUS][EVTCHN_FIFO_MAX_QUEUES];
} XENBUS_EVTCHN_FIFO_CONTEXT, *PXENBUS_EVTCHN_FIFO_CONTEXT;

#define EVENT_WORDS_PER_PAGE    (PAGE_SIZE / sizeof (event_word_t))

#define XENBUS_EVTCHN_FIFO_TAG  'OFIF'

static FORCEINLINE PVOID
__EvtchnFifoAllocate(
    _In_ ULONG  Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENBUS_EVTCHN_FIFO_TAG);
}

static FORCEINLINE VOID
__EvtchnFifoFree(
    _In_ PVOID  Buffer
    )
{
    __FreePoolWithTag(Buffer, XENBUS_EVTCHN_FIFO_TAG);
}

static event_word_t *
EvtchnFifoEventWord(
    _In_ PXENBUS_EVTCHN_FIFO_CONTEXT    Context,
    _In_ ULONG                          Port
    )
{
    ULONG                               Index;
    PMDL                                Mdl;
    event_word_t                        *EventWord;

    Index = Port / EVENT_WORDS_PER_PAGE;
    ASSERT3U(Index, <, Context->EventPageCount);

    Mdl = Context->EventPageMdl[Index];

    EventWord = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
    ASSERT(EventWord != NULL);

    ASSERT3U(Port, >=, Index * EVENT_WORDS_PER_PAGE);
    Port -= Index * EVENT_WORDS_PER_PAGE;

    return &EventWord[Port];
}

static FORCEINLINE BOOLEAN
__EvtchnFifoTestFlag(
    _In_ event_word_t   *EventWord,
    _In_ ULONG          Flag
    )
{
    KeMemoryBarrier();
    return (*EventWord & (1 << Flag)) ? TRUE : FALSE;
}

static FORCEINLINE BOOLEAN
__EvtchnFifoTestAndSetFlag(
    _In_ event_word_t   *EventWord,
    _In_ ULONG          Flag
    )
{
    return (InterlockedBitTestAndSet((LONG *)EventWord, Flag) != 0) ? TRUE : FALSE;
}

static FORCEINLINE BOOLEAN
__EvtchnFifoTestAndClearFlag(
    _In_ event_word_t   *EventWord,
    _In_ ULONG          Flag
    )
{
    return (InterlockedBitTestAndReset((LONG *)EventWord, Flag) != 0) ? TRUE : FALSE;
}

static FORCEINLINE VOID
__EvtchnFifoSetFlag(
    _In_ event_word_t   *EventWord,
    _In_ ULONG          Flag
    )
{
    (VOID) InterlockedBitTestAndSet((LONG *)EventWord, Flag);
}

static FORCEINLINE VOID
__EvtchnFifoClearFlag(
    _In_ event_word_t   *EventWord,
    _In_ ULONG          Flag
    )
{
    (VOID) InterlockedBitTestAndReset((LONG *)EventWord, Flag);
}

static FORCEINLINE ULONG
__EvtchnFifoUnlink(
    _In_ event_word_t   *EventWord
    )
{
    LONG                Old;
    LONG                New;

    do {
        Old = *EventWord;

        // Clear linked bit and link value
        New = Old & ~((1 << EVTCHN_FIFO_LINKED) | EVTCHN_FIFO_LINK_MASK);
    } while (InterlockedCompareExchange((LONG *)EventWord, New, Old) != Old);

    return Old & EVTCHN_FIFO_LINK_MASK;
}

static NTSTATUS
EvtchnFifoExpand(
    _In_ PXENBUS_EVTCHN_FIFO_CONTEXT    Context,
    _In_ ULONG                          Port
    )
{
    LONG                                Index;
    ULONG                               EventPageCount;
    PMDL                                *EventPageMdl;
    PMDL                                Mdl;
    ULONG                               Start;
    ULONG                               End;
    NTSTATUS                            status;

    Index = Port / EVENT_WORDS_PER_PAGE;
    ASSERT3U(Index, >=, (LONG)Context->EventPageCount);

    EventPageCount = Index + 1;
    EventPageMdl = __EvtchnFifoAllocate(sizeof (PMDL) * EventPageCount);

    status = STATUS_NO_MEMORY;
    if (EventPageMdl == NULL)
        goto fail1;

    for (Index = 0; Index < (LONG)Context->EventPageCount; Index++)
        EventPageMdl[Index] = Context->EventPageMdl[Index];

    Index = Context->EventPageCount;
    while (Index < (LONG)EventPageCount) {
        event_word_t        *EventWord;
        PFN_NUMBER          Pfn;
        PHYSICAL_ADDRESS    Address;

        Mdl = __AllocatePage();

        status = STATUS_NO_MEMORY;
        if (Mdl == NULL)
            goto fail2;

        EventWord = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
        ASSERT(EventWord != NULL);

        for (Port = 0; Port < EVENT_WORDS_PER_PAGE; Port++)
            __EvtchnFifoSetFlag(&EventWord[Port], EVTCHN_FIFO_MASKED);

        Pfn = MmGetMdlPfnArray(Mdl)[0];

        status = EventChannelExpandArray(Pfn);
        if (!NT_SUCCESS(status))
            goto fail3;

        Address.QuadPart = (ULONGLONG)Pfn << PAGE_SHIFT;

        LogPrintf(LOG_LEVEL_INFO,
                  "EVTCHN_FIFO: EVENTARRAY[%u] @ %08x.%08x\n",
                  Index,
                  Address.HighPart,
                  Address.LowPart);

        EventPageMdl[Index++] = Mdl;
    }

    Start = Context->EventPageCount * EVENT_WORDS_PER_PAGE;
    End = (EventPageCount * EVENT_WORDS_PER_PAGE) - 1;

    Info("added ports [%08x - %08x]\n", Start, End);

    if (Context->EventPageMdl != NULL)
        __EvtchnFifoFree(Context->EventPageMdl);

    Context->EventPageMdl = EventPageMdl;
    Context->EventPageCount = EventPageCount;

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    __FreePage(Mdl);

fail2:
    Error("fail2\n");

    while (--Index >= (LONG)Context->EventPageCount) {
        Mdl = EventPageMdl[Index];

        __FreePage(Mdl);
    }

    __EvtchnFifoFree(EventPageMdl);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
EvtchnFifoContract(
    _In_ PXENBUS_EVTCHN_FIFO_CONTEXT    Context
    )
{
    LONG                                Index;

    Index = Context->EventPageCount;
    while (--Index >= 0) {
        PMDL    Mdl;

        Mdl = Context->EventPageMdl[Index];

        __FreePage(Mdl);
    }

    if (Context->EventPageMdl != NULL)
        __EvtchnFifoFree(Context->EventPageMdl);

    Context->EventPageMdl = NULL;
    Context->EventPageCount = 0;
}

static BOOLEAN
EvtchnFifoIsProcessorEnabled(
    _In_ PXENBUS_EVTCHN_ABI_CONTEXT _Context,
    _In_ ULONG                      Index
    )
{
    PXENBUS_EVTCHN_FIFO_CONTEXT     Context = (PVOID)_Context;
    unsigned int                    vcpu_id;
    NTSTATUS                        status;

    status = SystemProcessorVcpuId(Index, &vcpu_id);
    if (!NT_SUCCESS(status))
        return FALSE;

    return (Context->ControlBlockMdl[vcpu_id] != NULL) ? TRUE : FALSE;
}

static BOOLEAN
EvtchnFifoPollPriority(
    _In_ PXENBUS_EVTCHN_FIFO_CONTEXT    Context,
    _In_ unsigned int                   vcpu_id,
    _In_ ULONG                          Priority,
    _In_ PULONG                         Ready,
    _In_ XENBUS_EVTCHN_ABI_EVENT        Event,
    _In_ PVOID                          Argument
    )
{
    ULONG                               Head;
    ULONG                               Port;
    event_word_t                        *EventWord;
    BOOLEAN                             DoneSomething;

    Head = Context->Head[vcpu_id][Priority];

    if (Head == 0) {
        PMDL                        Mdl;
        evtchn_fifo_control_block_t *ControlBlock;

        Mdl = Context->ControlBlockMdl[vcpu_id];

        ControlBlock = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
        ASSERT(ControlBlock != NULL);

        KeMemoryBarrier();
        Head = ControlBlock->head[Priority];
    }

    Port = Head;
    EventWord = EvtchnFifoEventWord(Context, Port);

    Head = __EvtchnFifoUnlink(EventWord);

    if (Head == 0)
        *Ready &= ~(1ull << Priority);

    DoneSomething = FALSE;

    if (!__EvtchnFifoTestFlag(EventWord, EVTCHN_FIFO_MASKED) &&
        __EvtchnFifoTestFlag(EventWord, EVTCHN_FIFO_PENDING))
        DoneSomething = Event(Argument, Port);

    Context->Head[vcpu_id][Priority] = Head;

    return DoneSomething;
}

static BOOLEAN
EvtchnFifoPoll(
    _In_ PXENBUS_EVTCHN_ABI_CONTEXT _Context,
    _In_ ULONG                      Index,
    _In_ XENBUS_EVTCHN_ABI_EVENT    Event,
    _In_ PVOID                      Argument
    )
{
    PXENBUS_EVTCHN_FIFO_CONTEXT     Context = (PVOID)_Context;
    unsigned int                    vcpu_id;
    PMDL                            Mdl;
    evtchn_fifo_control_block_t     *ControlBlock;
    ULONG                           Ready;
    ULONG                           Priority;
    BOOLEAN                         DoneSomething;
    NTSTATUS                        status;

    DoneSomething = FALSE;

    status = SystemProcessorVcpuId(Index, &vcpu_id);
    if (!NT_SUCCESS(status))
        goto done;

    Mdl = Context->ControlBlockMdl[vcpu_id];
    if (Mdl == NULL)
        goto done;

    ControlBlock = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
    ASSERT(ControlBlock != NULL);

    Ready = InterlockedExchange((LONG *)&ControlBlock->ready, 0);

    while (_BitScanReverse(&Priority, Ready)) {
        DoneSomething |= EvtchnFifoPollPriority(Context,
                                                vcpu_id,
                                                Priority,
                                                &Ready,
                                                Event,
                                                Argument);
        Ready |= InterlockedExchange((LONG *)&ControlBlock->ready, 0);
    }

done:
    return DoneSomething;
}

static NTSTATUS
EvtchnFifoPortEnable(
    _In_ PXENBUS_EVTCHN_ABI_CONTEXT _Context,
    _In_ ULONG                      Port
    )
{
    PXENBUS_EVTCHN_FIFO_CONTEXT     Context = (PVOID)_Context;
    KIRQL                           Irql;
    NTSTATUS                        status;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (Port / EVENT_WORDS_PER_PAGE >= Context->EventPageCount) {
        status = EvtchnFifoExpand(Context, Port);

        if (!NT_SUCCESS(status))
            goto fail1;
    }

    KeReleaseSpinLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    KeReleaseSpinLock(&Context->Lock, Irql);

    return status;
}

static VOID
EvtchnFifoPortAck(
    _In_ PXENBUS_EVTCHN_ABI_CONTEXT _Context,
    _In_ ULONG                      Port
    )
{
    PXENBUS_EVTCHN_FIFO_CONTEXT     Context = (PVOID)_Context;
    event_word_t                    *EventWord;

    EventWord = EvtchnFifoEventWord(Context, Port);
    __EvtchnFifoClearFlag(EventWord, EVTCHN_FIFO_PENDING);
}

static VOID
EvtchnFifoPortMask(
    _In_ PXENBUS_EVTCHN_ABI_CONTEXT _Context,
    _In_ ULONG                      Port
    )
{
    PXENBUS_EVTCHN_FIFO_CONTEXT     Context = (PVOID)_Context;
    event_word_t                    *EventWord;

    EventWord = EvtchnFifoEventWord(Context, Port);
    __EvtchnFifoSetFlag(EventWord, EVTCHN_FIFO_MASKED);
}

static BOOLEAN
EvtchnFifoPortUnmask(
    _In_ PXENBUS_EVTCHN_ABI_CONTEXT _Context,
    _In_ ULONG                      Port
    )
{
    PXENBUS_EVTCHN_FIFO_CONTEXT     Context = (PVOID)_Context;
    event_word_t                    *EventWord;
    LONG                            Old;
    LONG                            New;

    EventWord = EvtchnFifoEventWord(Context, Port);

    // Clear masked bit, spinning if busy
    do {
        Old = *EventWord & ~(1 << EVTCHN_FIFO_BUSY);
        New = Old & ~(1 << EVTCHN_FIFO_MASKED);
    } while (InterlockedCompareExchange((LONG *)EventWord, New, Old) != Old);

    // If we cleared the mask then check whether something is pending
    return __EvtchnFifoTestFlag(EventWord, EVTCHN_FIFO_PENDING);
}

static VOID
EvtchnFifoPortDisable(
    _In_ PXENBUS_EVTCHN_ABI_CONTEXT _Context,
    _In_ ULONG                      Port
    )
{
    EvtchnFifoPortMask(_Context, Port);
}

static NTSTATUS
EvtchnFifoAcquire(
    _In_ PXENBUS_EVTCHN_ABI_CONTEXT _Context
    )
{
    PXENBUS_EVTCHN_FIFO_CONTEXT     Context = (PVOID)_Context;
    KIRQL                           Irql;
    LONG                            ProcessorCount;
    LONG                            Index;
    PMDL                            Mdl;
    NTSTATUS                        status;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (Context->References++ != 0)
        goto done;

    Trace("====>\n");

    ProcessorCount = KeQueryMaximumProcessorCountEx(ALL_PROCESSOR_GROUPS);

    for (Index = 0; Index < ProcessorCount; Index++) {
        unsigned int        vcpu_id;
        PFN_NUMBER          Pfn;
        PHYSICAL_ADDRESS    Address;

        status = SystemProcessorVcpuId(Index, &vcpu_id);

        if (status == STATUS_NOT_SUPPORTED)
            continue;

        if (!NT_SUCCESS(status))
            goto fail1;

        Mdl = __AllocatePage();

        status = STATUS_NO_MEMORY;
        if (Mdl == NULL)
            goto fail2;

        Pfn = MmGetMdlPfnArray(Mdl)[0];

        status = EventChannelInitControl(Pfn, vcpu_id);
        if (!NT_SUCCESS(status))
            goto fail3;

        Address.QuadPart = (ULONGLONG)Pfn << PAGE_SHIFT;

        LogPrintf(LOG_LEVEL_INFO,
                  "EVTCHN_FIFO: CONTROLBLOCK[%u] @ %08x.%08x\n",
                  vcpu_id,
                  Address.HighPart,
                  Address.LowPart);

        Context->ControlBlockMdl[vcpu_id] = Mdl;
    }

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    __FreePage(Mdl);

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    EvtchnReset();

    while (--Index >= 0) {
        unsigned int    vcpu_id;

        status = SystemProcessorVcpuId(Index, &vcpu_id);
        if (status == STATUS_NOT_SUPPORTED)
            continue;

        BUG_ON(!NT_SUCCESS(status));

        Mdl = Context->ControlBlockMdl[vcpu_id];
        Context->ControlBlockMdl[vcpu_id] = NULL;

        __FreePage(Mdl);
    }

    --Context->References;
    ASSERT3U(Context->References, ==, 0);
    KeReleaseSpinLock(&Context->Lock, Irql);

    return status;
}

VOID
EvtchnFifoRelease(
    _In_ PXENBUS_EVTCHN_ABI_CONTEXT _Context
    )
{
    PXENBUS_EVTCHN_FIFO_CONTEXT     Context = (PVOID)_Context;
    KIRQL                           Irql;
    int                             vcpu_id;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (--Context->References > 0)
        goto done;

    Trace("====>\n");

    EvtchnReset();

    EvtchnFifoContract(Context);

    vcpu_id = HVM_MAX_VCPUS;
    while (--vcpu_id >= 0) {
        PMDL            Mdl;

        Mdl = Context->ControlBlockMdl[vcpu_id];
        if (Mdl == NULL)
            continue;

        Context->ControlBlockMdl[vcpu_id] = NULL;

        __FreePage(Mdl);
    }

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);
}

static XENBUS_EVTCHN_ABI EvtchnAbiFifo = {
    NULL,
    EvtchnFifoAcquire,
    EvtchnFifoRelease,
    EvtchnFifoIsProcessorEnabled,
    EvtchnFifoPoll,
    EvtchnFifoPortEnable,
    EvtchnFifoPortDisable,
    EvtchnFifoPortAck,
    EvtchnFifoPortMask,
    EvtchnFifoPortUnmask
};

NTSTATUS
EvtchnFifoInitialize(
    _In_ PXENBUS_FDO                    Fdo,
    _Outptr_ PXENBUS_EVTCHN_ABI_CONTEXT *_Context
    )
{
    PXENBUS_EVTCHN_FIFO_CONTEXT         Context;
    NTSTATUS                            status;

    Trace("====>\n");

    Context = __EvtchnFifoAllocate(sizeof (XENBUS_EVTCHN_FIFO_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (Context == NULL)
        goto fail1;

    KeInitializeSpinLock(&Context->Lock);

    Context->Fdo = Fdo;

    *_Context = (PVOID)Context;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
EvtchnFifoGetAbi(
    _In_ PXENBUS_EVTCHN_ABI_CONTEXT     _Context,
    _Out_ PXENBUS_EVTCHN_ABI            Abi)
{
    *Abi = EvtchnAbiFifo;

    Abi->Context = (PVOID)_Context;
}

VOID
EvtchnFifoTeardown(
    _In_ PXENBUS_EVTCHN_ABI_CONTEXT     _Context
    )
{
    PXENBUS_EVTCHN_FIFO_CONTEXT    Context = (PVOID)_Context;

    Trace("====>\n");

    Context->Fdo = NULL;

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_EVTCHN_FIFO_CONTEXT)));
    __EvtchnFifoFree(Context);

    Trace("<====\n");
}
