#include <windows.h>
#include <stdio.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#ifndef IOCTL_SYSVAD_GET_LOOPBACK_DATA
#define IOCTL_SYSVAD_GET_LOOPBACK_DATA CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_OUT_DIRECT, FILE_READ_ACCESS)
#endif

int wmain(int argc, wchar_t** argv)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        wprintf(L"CoInitializeEx failed: 0x%08lx\n", hr);
        return 1;
    }

    IMMDeviceEnumerator* pEnumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(&pEnumerator));
    if (FAILED(hr))
    {
        wprintf(L"Failed to create device enumerator: 0x%08lx\n", hr);
        CoUninitialize();
        return 1;
    }

    IMMDeviceCollection* pCollection = nullptr;
    hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr))
    {
        wprintf(L"EnumAudioEndpoints failed: 0x%08lx\n", hr);
        pEnumerator->Release();
        CoUninitialize();
        return 1;
    }

    UINT count = 0;
    pCollection->GetCount(&count);
    for (UINT i = 0; i < count; ++i)
    {
        IMMDevice* pDevice = nullptr;
        if (SUCCEEDED(pCollection->Item(i, &pDevice)))
        {
            IPropertyStore* pStore = nullptr;
            if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pStore)))
            {
                PROPVARIANT varName;
                PropVariantInit(&varName);
                if (SUCCEEDED(pStore->GetValue(PKEY_Device_FriendlyName, &varName)))
                {
                    wprintf(L"%u: %s\n", i, varName.pwszVal);
                    PropVariantClear(&varName);
                }
                pStore->Release();
            }
            pDevice->Release();
        }
    }

    wprintf(L"Select device index: ");
    UINT choice = 0;
    wscanf(L"%u", &choice);
    if (choice >= count)
    {
        wprintf(L"Invalid choice\n");
        pCollection->Release();
        pEnumerator->Release();
        CoUninitialize();
        return 1;
    }

    IMMDevice* pRenderDevice = nullptr;
    hr = pCollection->Item(choice, &pRenderDevice);
    pCollection->Release();
    pEnumerator->Release();
    if (FAILED(hr))
    {
        wprintf(L"Failed to get device: 0x%08lx\n", hr);
        CoUninitialize();
        return 1;
    }

    IAudioClient* pAudioClient = nullptr;
    hr = pRenderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
    if (FAILED(hr))
    {
        wprintf(L"Activate IAudioClient failed: 0x%08lx\n", hr);
        pRenderDevice->Release();
        CoUninitialize();
        return 1;
    }

    WAVEFORMATEX* pwfx = nullptr;
    hr = pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr))
    {
        wprintf(L"GetMixFormat failed: 0x%08lx\n", hr);
        pAudioClient->Release();
        pRenderDevice->Release();
        CoUninitialize();
        return 1;
    }

    REFERENCE_TIME bufferDuration = 10000000; // 1 second
    hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  bufferDuration, 0, pwfx, nullptr);
    if (FAILED(hr))
    {
        wprintf(L"Audio client initialize failed: 0x%08lx\n", hr);
        CoTaskMemFree(pwfx);
        pAudioClient->Release();
        pRenderDevice->Release();
        CoUninitialize();
        return 1;
    }

    HANDLE hAudioEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    pAudioClient->SetEventHandle(hAudioEvent);

    IAudioRenderClient* pRenderClient = nullptr;
    hr = pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&pRenderClient);
    if (FAILED(hr))
    {
        wprintf(L"GetService IAudioRenderClient failed: 0x%08lx\n", hr);
        CloseHandle(hAudioEvent);
        CoTaskMemFree(pwfx);
        pAudioClient->Release();
        pRenderDevice->Release();
        CoUninitialize();
        return 1;
    }

    UINT32 bufferFrameCount = 0;
    pAudioClient->GetBufferSize(&bufferFrameCount);

    hr = pAudioClient->Start();
    if (FAILED(hr))
    {
        wprintf(L"Audio client start failed: 0x%08lx\n", hr);
        pRenderClient->Release();
        CloseHandle(hAudioEvent);
        CoTaskMemFree(pwfx);
        pAudioClient->Release();
        pRenderDevice->Release();
        CoUninitialize();
        return 1;
    }

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
        WaitForSingleObject(hAudioEvent, INFINITE);

        UINT32 padding = 0;
        pAudioClient->GetCurrentPadding(&padding);
        UINT32 framesToWrite = bufferFrameCount - padding;

        BYTE* pData = nullptr;
        if (FAILED(pRenderClient->GetBuffer(framesToWrite, &pData)))
            break;

        DWORD bytesNeeded = framesToWrite * pwfx->nBlockAlign;
        if (!DeviceIoControl(hDevice,
                             IOCTL_SYSVAD_GET_LOOPBACK_DATA,
                             nullptr,
                             0,
                             buffer,
                             min(sizeof(buffer), bytesNeeded),
                             &bytesReturned,
                             nullptr))
        {
            wprintf(L"DeviceIoControl failed: %lu\n", GetLastError());
            bytesReturned = 0;
        }

        if (bytesReturned < bytesNeeded)
            ZeroMemory(buffer + bytesReturned, bytesNeeded - bytesReturned);

        CopyMemory(pData, buffer, bytesNeeded);

        if (hFile != INVALID_HANDLE_VALUE && bytesReturned > 0)
        {
            DWORD written = 0;
            WriteFile(hFile, buffer, bytesReturned, &written, nullptr);
        }

        pRenderClient->ReleaseBuffer(framesToWrite, 0);
    }

    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);
    CloseHandle(hDevice);

    pAudioClient->Stop();
    pRenderClient->Release();
    CloseHandle(hAudioEvent);
    CoTaskMemFree(pwfx);
    pAudioClient->Release();
    pRenderDevice->Release();
    CoUninitialize();
    return 0;
}

