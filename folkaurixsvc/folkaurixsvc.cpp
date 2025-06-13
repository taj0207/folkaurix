#include <windows.h>
#include <stdio.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <string>

#include <string.h>

bool ConvertRawToWav(const wchar_t* rawPath,
                     const wchar_t* wavPath,
                     const WAVEFORMATEX* pwfx)
{
    HANDLE hIn = CreateFileW(rawPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hIn == INVALID_HANDLE_VALUE)
        return false;

    DWORD dataSize = GetFileSize(hIn, nullptr);
    if (dataSize == INVALID_FILE_SIZE)
    {
        CloseHandle(hIn);
        return false;
    }

    HANDLE hOut = CreateFileW(wavPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE)
    {
        CloseHandle(hIn);
        return false;
    }

    DWORD written = 0;
    DWORD riff = 'FFIR';
    DWORD wave = 'EVAW';
    DWORD fmt = ' tmf';
    DWORD data = 'atad';
    DWORD subchunk1Size = sizeof(WAVEFORMATEX) + pwfx->cbSize;
    DWORD chunkSize = 20 + subchunk1Size + dataSize;

    WriteFile(hOut, &riff, 4, &written, nullptr);
    WriteFile(hOut, &chunkSize, 4, &written, nullptr);
    WriteFile(hOut, &wave, 4, &written, nullptr);
    WriteFile(hOut, &fmt, 4, &written, nullptr);
    WriteFile(hOut, &subchunk1Size, 4, &written, nullptr);
    WriteFile(hOut, pwfx, subchunk1Size, &written, nullptr);
    WriteFile(hOut, &data, 4, &written, nullptr);
    WriteFile(hOut, &dataSize, 4, &written, nullptr);

    BYTE buffer[4096];
    DWORD read = 0;
    while (ReadFile(hIn, buffer, sizeof(buffer), &read, nullptr) && read > 0)
    {
        WriteFile(hOut, buffer, read, &written, nullptr);
    }

    CloseHandle(hOut);
    CloseHandle(hIn);
    return true;
}

#ifndef IOCTL_SYSVAD_GET_LOOPBACK_DATA
#define IOCTL_SYSVAD_GET_LOOPBACK_DATA CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_OUT_DIRECT, FILE_READ_ACCESS)
#endif

#define DPF_ENTER() wprintf(L"[ENTER] %S\n", __FUNCTION__)
#define DPF_EXIT()  wprintf(L"[EXIT] %S\n", __FUNCTION__)

int wmain(int argc, wchar_t** argv)
{
    DPF_ENTER();
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        wprintf(L"CoInitializeEx failed: 0x%08lx\n", hr);
        DPF_EXIT();
        return 1;
    }

    IMMDeviceEnumerator* pEnumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(&pEnumerator));
    if (FAILED(hr))
    {
        wprintf(L"Failed to create device enumerator: 0x%08lx\n", hr);
        CoUninitialize();
        DPF_EXIT();
        return 1;
    }

    IMMDeviceCollection* pCollection = nullptr;
    hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr))
    {
        wprintf(L"EnumAudioEndpoints failed: 0x%08lx\n", hr);
        pEnumerator->Release();
        CoUninitialize();
        DPF_EXIT();
        return 1;
    }

    UINT count = 0;
    hr = pCollection->GetCount(&count);
    if (FAILED(hr))
    {
        wprintf(L"GetCount failed: 0x%08lx\n", hr);
        pCollection->Release();
        pEnumerator->Release();
        CoUninitialize();
        DPF_EXIT();
        return 1;
    }
    for (UINT i = 0; i < count; ++i)
    {
        IMMDevice* pDevice = nullptr;
        hr = pCollection->Item(i, &pDevice);
        if (SUCCEEDED(hr))
        {
            IPropertyStore* pStore = nullptr;
            hr = pDevice->OpenPropertyStore(STGM_READ, &pStore);
            if (SUCCEEDED(hr))
            {
                PROPVARIANT varName;
                PropVariantInit(&varName);
                hr = pStore->GetValue(PKEY_Device_FriendlyName, &varName);
                if (SUCCEEDED(hr))
                {
                    wprintf(L"%u: %s\n", i, varName.pwszVal);
                    PropVariantClear(&varName);
                }
                else
                {
                    wprintf(L"GetValue for device %u failed: 0x%08lx\n", i, hr);
                }
                pStore->Release();
            }
            else
            {
                wprintf(L"OpenPropertyStore for device %u failed: 0x%08lx\n", i, hr);
            }
            pDevice->Release();
        }
        else
        {
            wprintf(L"Failed to get device at index %u: 0x%08lx\n", i, hr);
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
        DPF_EXIT();
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
        DPF_EXIT();
        return 1;
    }

    IAudioClient* pAudioClient = nullptr;
    hr = pRenderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
    if (FAILED(hr))
    {
        wprintf(L"Activate IAudioClient failed: 0x%08lx\n", hr);
        pRenderDevice->Release();
        CoUninitialize();
        DPF_EXIT();
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
        DPF_EXIT();
        return 1;
    }

    wprintf(L"Render device format: %u channels, %u-bit, %u Hz\n",
            pwfx->nChannels, pwfx->wBitsPerSample, pwfx->nSamplesPerSec);

    // SysVAD loopback streams audio in a fixed 2ch/16-bit/48kHz format.
    WAVEFORMATEX loopbackFormat = {};
    loopbackFormat.wFormatTag = WAVE_FORMAT_PCM;
    loopbackFormat.nChannels = 2;
    loopbackFormat.nSamplesPerSec = 48000;
    loopbackFormat.wBitsPerSample = 16;
    loopbackFormat.nBlockAlign =
        loopbackFormat.nChannels * loopbackFormat.wBitsPerSample / 8;
    loopbackFormat.nAvgBytesPerSec =
        loopbackFormat.nSamplesPerSec * loopbackFormat.nBlockAlign;
    loopbackFormat.cbSize = 0;

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
        DPF_EXIT();
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
        DPF_EXIT();
        return 1;
    }

    UINT32 bufferFrameCount = 0;
    pAudioClient->GetBufferSize(&bufferFrameCount);
    wprintf(L"GetBufferSize: 0x%08lx\n", bufferFrameCount);
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
        DPF_EXIT();
        return 1;
    }

    HANDLE hDevice = CreateFileW(L"\\\\.\\SysVADLoopback", GENERIC_READ, 0,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hDevice == INVALID_HANDLE_VALUE)
    {
        wprintf(L"Failed to open device: %lu\n", GetLastError());
        DPF_EXIT();
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
            DPF_EXIT();
            return 1;
        }
    }

    wprintf(L"Press F9 to stop recording...\n");

    BYTE buffer[4096];
    DWORD bytesReturned = 0;

    bool stopRequested = false;
    while (!stopRequested)
    {
        WaitForSingleObject(hAudioEvent, 100);
        if (GetAsyncKeyState(VK_F9) & 0x8000)
        {
            stopRequested = true;
            continue;
        }
        if (stopRequested)
            break;

        UINT32 padding = 0;
        pAudioClient->GetCurrentPadding(&padding);
        wprintf(L"GetCurrentPadding: 0x%08lx\n", padding);
        UINT32 framesToWrite = bufferFrameCount - padding;
        wprintf(L"padding=%u, framesToWrite=%u\n", padding, framesToWrite);

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
            wprintf(L"DeviceIoControl failed: 0x%08lx\n", GetLastError());
            bytesReturned = 0;
        }


        DWORD copyBytes = min(bytesReturned, bytesNeeded);
        CopyMemory(pData, buffer, copyBytes);
        if (copyBytes < bytesNeeded)
            ZeroMemory(pData + copyBytes, bytesNeeded - copyBytes);

        if (hFile != INVALID_HANDLE_VALUE && bytesReturned > 0)
        {
            DWORD written = 0;
            WriteFile(hFile, buffer, bytesReturned, &written, nullptr);
        }

        pRenderClient->ReleaseBuffer(framesToWrite, 0);
    }

    std::wstring wavPath;
    if (hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hFile);
        wavPath = argv[1];
        size_t pos = wavPath.find_last_of(L'.');
        if (pos == std::wstring::npos)
            wavPath += L".wav";
        else
            wavPath.replace(pos, wavPath.length() - pos, L".wav");
    }
    CloseHandle(hDevice);

    pAudioClient->Stop();
    pRenderClient->Release();
    CloseHandle(hAudioEvent);

    if (!wavPath.empty())
    {
        if (ConvertRawToWav(argv[1], wavPath.c_str(), &loopbackFormat))
            wprintf(L"Converted %s to %s\n", argv[1], wavPath.c_str());
        else
            wprintf(L"Failed to convert %s\n", argv[1]);
    }

    CoTaskMemFree(pwfx);
    pAudioClient->Release();
    pRenderDevice->Release();
    CoUninitialize();
    DPF_EXIT();
    return 0;
}

