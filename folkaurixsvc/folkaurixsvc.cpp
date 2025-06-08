#include <windows.h>
#include <stdio.h>

#ifndef IOCTL_SYSVAD_GET_LOOPBACK_DATA
#define IOCTL_SYSVAD_GET_LOOPBACK_DATA CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_OUT_DIRECT, FILE_READ_ACCESS)
#endif

int wmain(int argc, wchar_t** argv)
{
    HANDLE hDevice = CreateFileW(L"\\\\.\\SysVADLoopback", GENERIC_READ, 0,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hDevice == INVALID_HANDLE_VALUE)
    {
        wprintf(L"Failed to open device: %lu\n", GetLastError());
        return 1;
    }

    HANDLE hFile = INVALID_HANDLE_VALUE;
    if (argc > 1)
    {
        hFile = CreateFileW(argv[1], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            wprintf(L"Failed to open output file: %lu\n", GetLastError());
            CloseHandle(hDevice);
            return 1;
        }
    }

    BYTE buffer[4096];
    DWORD bytesReturned = 0;

    while (true)
    {
        if (!DeviceIoControl(hDevice,
                             IOCTL_SYSVAD_GET_LOOPBACK_DATA,
                             nullptr,
                             0,
                             buffer,
                             sizeof(buffer),
                             &bytesReturned,
                             nullptr))
        {
            wprintf(L"DeviceIoControl failed: %lu\n", GetLastError());
            break;
        }

        if (bytesReturned == 0)
        {
            Sleep(10);
            continue;
        }

        if (hFile != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(hFile, buffer, bytesReturned, &written, nullptr);
        }

        wprintf(L"Read %lu bytes\n", bytesReturned);
    }

    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);
    CloseHandle(hDevice);
    return 0;
}

