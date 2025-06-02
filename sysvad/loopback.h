#pragma once

#include <ntddk.h>

#define IOCTL_SYSVAD_GET_LOOPBACK_DATA CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_OUT_DIRECT, FILE_READ_ACCESS)

#define LOOPBACK_BUFFER_SIZE 65536
#define LOOPBACK_POOLTAG 'LPBK'

typedef struct _LOOPBACK_BUFFER {
    PBYTE Buffer;
    ULONG Size;
    ULONG WritePos;
    ULONG ReadPos;
    KSPIN_LOCK Lock;
    KEVENT DataEvent;
} LOOPBACK_BUFFER, *PLOOPBACK_BUFFER;

NTSTATUS LoopbackBuffer_Initialize();
void LoopbackBuffer_Cleanup();
void LoopbackBuffer_Write(_In_reads_bytes_(Length) PBYTE Data, _In_ ULONG Length);
ULONG LoopbackBuffer_Read(_Out_writes_bytes_(Length) PBYTE Data, _In_ ULONG Length);
ULONG LoopbackBuffer_Available();
KEVENT* LoopbackBuffer_GetEvent();
