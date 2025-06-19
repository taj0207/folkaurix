#include "loopback.h"

static LOOPBACK_BUFFER g_LoopbackBuffer = {0};

NTSTATUS LoopbackBuffer_Initialize()
{
	DbgPrint(("[%s]", __FUNCTION__));
    if (g_LoopbackBuffer.Buffer) return STATUS_SUCCESS;
    g_LoopbackBuffer.Size = LOOPBACK_BUFFER_SIZE;
    g_LoopbackBuffer.Buffer = (PBYTE)ExAllocatePool2(POOL_FLAG_NON_PAGED, g_LoopbackBuffer.Size, LOOPBACK_POOLTAG);
    if (!g_LoopbackBuffer.Buffer)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    g_LoopbackBuffer.WritePos = g_LoopbackBuffer.ReadPos = 0;
    KeInitializeSpinLock(&g_LoopbackBuffer.Lock);
    KeInitializeEvent(&g_LoopbackBuffer.DataEvent, SynchronizationEvent, FALSE);
    return STATUS_SUCCESS;
}

void LoopbackBuffer_Cleanup()
{
	DbgPrint(("[%s]", __FUNCTION__));
    if (g_LoopbackBuffer.Buffer)
    {
        ExFreePoolWithTag(g_LoopbackBuffer.Buffer, LOOPBACK_POOLTAG);
        g_LoopbackBuffer.Buffer = NULL;
    }
}

static __forceinline ULONG _available()
{
    if (g_LoopbackBuffer.WritePos >= g_LoopbackBuffer.ReadPos)
        return g_LoopbackBuffer.WritePos - g_LoopbackBuffer.ReadPos;
    else
        return g_LoopbackBuffer.Size - g_LoopbackBuffer.ReadPos + g_LoopbackBuffer.WritePos;
}

void LoopbackBuffer_Write(_In_reads_bytes_(Length) PBYTE Data, _In_ ULONG Length)
{
        DbgPrint(("[%s]", __FUNCTION__));
    if (!g_LoopbackBuffer.Buffer) return;
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_LoopbackBuffer.Lock, &oldIrql);
    ULONG capacity = g_LoopbackBuffer.Size - 1;
    if (Length >= g_LoopbackBuffer.Size)
    {
        Data += Length - capacity;
        Length = capacity;
    }

    ULONG available = _available();
    ULONG freeSpace = (capacity > available) ? (capacity - available) : 0;

    if (Length > freeSpace)
    {
        ULONG overrun = Length - freeSpace;
        g_LoopbackBuffer.ReadPos = (g_LoopbackBuffer.ReadPos + overrun) % g_LoopbackBuffer.Size;
    }

    ULONG first = min(Length, g_LoopbackBuffer.Size - g_LoopbackBuffer.WritePos);
    RtlCopyMemory(g_LoopbackBuffer.Buffer + g_LoopbackBuffer.WritePos, Data, first);
    if (Length > first)
    {
        RtlCopyMemory(g_LoopbackBuffer.Buffer, Data + first, Length - first);
    }
    g_LoopbackBuffer.WritePos = (g_LoopbackBuffer.WritePos + Length) % g_LoopbackBuffer.Size;
    KeReleaseSpinLock(&g_LoopbackBuffer.Lock, oldIrql);
    KeSetEvent(&g_LoopbackBuffer.DataEvent, IO_NO_INCREMENT, FALSE);
}

ULONG LoopbackBuffer_Read(_Out_writes_bytes_(Length) PBYTE Data, _In_ ULONG Length)
{
        DbgPrint(("[%s]", __FUNCTION__));
    if (!g_LoopbackBuffer.Buffer) return 0;
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_LoopbackBuffer.Lock, &oldIrql);
    ULONG available = _available();
    if (Length > available) Length = available;
    ULONG first = min(Length, g_LoopbackBuffer.Size - g_LoopbackBuffer.ReadPos);
    RtlCopyMemory(Data, g_LoopbackBuffer.Buffer + g_LoopbackBuffer.ReadPos, first);
    if (Length > first)
    {
        RtlCopyMemory(Data + first, g_LoopbackBuffer.Buffer, Length - first);
    }
    g_LoopbackBuffer.ReadPos = (g_LoopbackBuffer.ReadPos + Length) % g_LoopbackBuffer.Size;
    if (_available() == 0)
    {
        KeClearEvent(&g_LoopbackBuffer.DataEvent);
    }
    KeReleaseSpinLock(&g_LoopbackBuffer.Lock, oldIrql);
    return Length;
}

ULONG LoopbackBuffer_Available()
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_LoopbackBuffer.Lock, &oldIrql);
    ULONG avail = _available();
    KeReleaseSpinLock(&g_LoopbackBuffer.Lock, oldIrql);
    return avail;
}

KEVENT* LoopbackBuffer_GetEvent()
{
    return &g_LoopbackBuffer.DataEvent;
}
