#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdio.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <string>

#include <string.h>

// Google Cloud C++ client library headers (optional, may require additional
// dependencies when building)
#include <google/cloud/speech/speech_client.h>
#include <google/cloud/translate/translation_client.h>
#include <google/cloud/text_to_speech/text_to_speech_client.h>
#include <fstream>
#include <vector>
#include <algorithm>

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
    // Write a standard WAV header using little-endian FOURCC codes.
    DWORD riff = 'RIFF';
    DWORD wave = 'WAVE';
    DWORD fmt = 'fmt ';
    DWORD data = 'data';
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

// Process captured audio using Google Cloud services. This is a simplified
// example that assumes the Google Cloud C++ client libraries are installed and
// properly configured. The function performs Speech-to-Text on the recorded
// audio, translates the result, synthesizes speech for the translation and
// plays it back to the selected render device.
bool ProcessAudioWithGoogle(const std::wstring& wavPath,
                            const std::string& targetLanguage,
                            IMMDevice* pRenderDevice)
{
    using ::google::cloud::speech::v1::SpeechClient;
    using ::google::cloud::speech::v1::RecognitionConfig;
    using ::google::cloud::speech::v1::RecognitionAudio;
    using ::google::cloud::translate::v3::TranslationServiceClient;
    using ::google::cloud::texttospeech::v1::TextToSpeechClient;

    // Read WAV data from disk
    std::ifstream file(wavPath, std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    auto size = file.tellg();
    file.seekg(0);
    std::vector<char> wavData(static_cast<size_t>(size));
    file.read(wavData.data(), size);

    // ---- Speech to Text ----
    SpeechClient speechClient;
    RecognitionConfig recConfig;
    recConfig.set_encoding(RecognitionConfig::LINEAR16);
    recConfig.set_sample_rate_hertz(48000);
    recConfig.set_language_code("en-US");
    RecognitionAudio audio;
    audio.set_content(std::string(wavData.begin(), wavData.end()));

    auto recResponse = speechClient.Recognize(recConfig, audio);
    if (!recResponse) return false;
    std::string transcript;
    for (auto const& result : recResponse->results()) {
        if (!result.alternatives().empty()) {
            transcript += result.alternatives(0).transcript();
        }
    }

    // ---- Translation ----
    TranslationServiceClient transClient;
    auto transResp = transClient.TranslateText(transcript, targetLanguage, "en");
    if (!transResp) return false;
    std::string translatedText = (*transResp)[0].translated_text();

    // ---- Text to Speech ----
    TextToSpeechClient ttsClient;
    ::google::cloud::texttospeech::v1::SynthesisInput input;
    input.set_text(translatedText);
    ::google::cloud::texttospeech::v1::VoiceSelectionParams voice;
    voice.set_language_code(targetLanguage);
    ::google::cloud::texttospeech::v1::AudioConfig audioCfg;
    audioCfg.set_audio_encoding(
        ::google::cloud::texttospeech::v1::AudioEncoding::LINEAR16);
    audioCfg.set_sample_rate_hertz(48000);
    auto ttsResp = ttsClient.SynthesizeSpeech(input, voice, audioCfg);
    if (!ttsResp) return false;

    auto const& ttsData = ttsResp->audio_content();

    // Simple playback using the existing render device. This example reuses the
    // same audio client configuration used for the loopback playback.
    IAudioClient* pAudioClient = nullptr;
    HRESULT hr = pRenderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                         nullptr, (void**)&pAudioClient);
    if (FAILED(hr))
        return false;

    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    REFERENCE_TIME dur = 10000000; // 1 second buffer
    hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, dur, 0, &format,
                                  nullptr);
    if (FAILED(hr)) {
        pAudioClient->Release();
        return false;
    }

    IAudioRenderClient* pRenderClient = nullptr;
    hr = pAudioClient->GetService(__uuidof(IAudioRenderClient),
                                  (void**)&pRenderClient);
    if (FAILED(hr)) {
        pAudioClient->Release();
        return false;
    }

    UINT32 bufferFrames = 0;
    pAudioClient->GetBufferSize(&bufferFrames);
    hr = pAudioClient->Start();
    if (FAILED(hr)) {
        pRenderClient->Release();
        pAudioClient->Release();
        return false;
    }

    const BYTE* dataPtr = reinterpret_cast<const BYTE*>(ttsData.data());
    size_t remaining = ttsData.size();
    while (remaining > 0) {
        UINT32 padding = 0;
        pAudioClient->GetCurrentPadding(&padding);
        UINT32 framesToWrite = bufferFrames - padding;
        BYTE* pData = nullptr;
        if (FAILED(pRenderClient->GetBuffer(framesToWrite, &pData)))
            break;

        DWORD bytesNeeded = framesToWrite * format.nBlockAlign;
        DWORD copyBytes = static_cast<DWORD>(min<size_t>(bytesNeeded, remaining));
        CopyMemory(pData, dataPtr, copyBytes);
        if (copyBytes < bytesNeeded)
            ZeroMemory(pData + copyBytes, bytesNeeded - copyBytes);
        dataPtr += copyBytes;
        remaining -= copyBytes;
        pRenderClient->ReleaseBuffer(framesToWrite, 0);
        Sleep(10);
    }

    pAudioClient->Stop();
    pRenderClient->Release();
    pAudioClient->Release();
    return true;
}

#ifndef IOCTL_SYSVAD_GET_LOOPBACK_DATA
#define IOCTL_SYSVAD_GET_LOOPBACK_DATA CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_OUT_DIRECT, FILE_READ_ACCESS)
#endif

#ifdef _DEBUG
#define DPF_ENTER() wprintf(L"[ENTER] %S\n", __FUNCTION__)
#define DPF_EXIT()  wprintf(L"[EXIT] %S\n", __FUNCTION__)
#define DPF(...)  wprintf(__VA_ARGS__x)
#else
#define DPF_ENTER()
#define DPF_EXIT()
#define DPF(...)  ((void)0)
#endif

int wmain(int argc, wchar_t** argv)
{
    DPF_ENTER();
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        DPF(L"CoInitializeEx failed: 0x%08lx\n", hr);
        DPF_EXIT();
        return 1;
    }

    IMMDeviceEnumerator* pEnumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(&pEnumerator));
    if (FAILED(hr))
    {
        DPF(L"Failed to create device enumerator: 0x%08lx\n", hr);
        CoUninitialize();
        DPF_EXIT();
        return 1;
    }

    IMMDeviceCollection* pCollection = nullptr;
    hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr))
    {
        DPF(L"EnumAudioEndpoints failed: 0x%08lx\n", hr);
        pEnumerator->Release();
        CoUninitialize();
        DPF_EXIT();
        return 1;
    }

    UINT count = 0;
    hr = pCollection->GetCount(&count);
    if (FAILED(hr))
    {
        DPF(L"GetCount failed: 0x%08lx\n", hr);
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

    const wchar_t* outputFile = nullptr;
    std::string targetLang = "zh"; // default translation target
    for (int i = 1; i < argc; ++i)
    {
        if ((wcscmp(argv[i], L"-f") == 0 || wcscmp(argv[i], L"--file") == 0) &&
            i + 1 < argc)
        {
            outputFile = argv[++i];
        }
        else if ((wcscmp(argv[i], L"-l") == 0 ||
                  wcscmp(argv[i], L"--lang") == 0) &&
                 i + 1 < argc)
        {
            char buf[64] = {};
            wcstombs(buf, argv[++i], sizeof(buf) - 1);
            targetLang = buf;
        }
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
        DPF(L"Activate IAudioClient failed: 0x%08lx\n", hr);
        pRenderDevice->Release();
        CoUninitialize();
        DPF_EXIT();
        return 1;
    }

    WAVEFORMATEX* pwfx = nullptr;
    hr = pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr))
    {
        DPF(L"GetMixFormat failed: 0x%08lx\n", hr);
        pAudioClient->Release();
        pRenderDevice->Release();
        CoUninitialize();
        DPF_EXIT();
        return 1;
    }

    DPF(L"Render device format: %u channels, %u-bit, %u Hz\n",
            pwfx->nChannels, pwfx->wBitsPerSample, pwfx->nSamplesPerSec);

    // We only needed the mix format for informational purposes.
    CoTaskMemFree(pwfx);
    pwfx = nullptr;

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
    hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  bufferDuration,
                                  0,
                                  &loopbackFormat,
                                  nullptr);
    if (FAILED(hr))
    {
        DPF(L"Audio client initialize failed: 0x%08lx\n", hr);
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
        DPF(L"GetService IAudioRenderClient failed: 0x%08lx\n", hr);
        CloseHandle(hAudioEvent);
        pAudioClient->Release();
        pRenderDevice->Release();
        CoUninitialize();
        DPF_EXIT();
        return 1;
    }

    UINT32 bufferFrameCount = 0;
    pAudioClient->GetBufferSize(&bufferFrameCount);
    DPF(L"GetBufferSize: 0x%08lx\n", bufferFrameCount);
    hr = pAudioClient->Start();
    if (FAILED(hr))
    {
        DPF(L"Audio client start failed: 0x%08lx\n", hr);
        pRenderClient->Release();
        CloseHandle(hAudioEvent);
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
        DPF(L"Failed to open device: %lu\n", GetLastError());
        DPF_EXIT();
        return 1;
    }

    HANDLE hFile = INVALID_HANDLE_VALUE;
    if (outputFile)
    {
        hFile = CreateFileW(outputFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            DPF(L"Failed to open output file: %lu\n", GetLastError());
            CloseHandle(hDevice);
            DPF_EXIT();
            return 1;
        }
    }

    DPF(L"Press F9 to stop recording...\n");

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
        DPF(L"GetCurrentPadding: 0x%08lx\n", padding);
        UINT32 framesToWrite = bufferFrameCount - padding;
        DPF(L"padding=%u, framesToWrite=%u\n", padding, framesToWrite);

        BYTE* pData = nullptr;
        if (FAILED(pRenderClient->GetBuffer(framesToWrite, &pData)))
            break;

        DWORD bytesNeeded = framesToWrite * loopbackFormat.nBlockAlign;
        DWORD queryBytes = std::min<DWORD>(static_cast<DWORD>(sizeof(buffer)), bytesNeeded);
        if (!DeviceIoControl(hDevice,
                             IOCTL_SYSVAD_GET_LOOPBACK_DATA,
                             nullptr,
                             0,
                             buffer,
                             queryBytes,
                             &bytesReturned,
                             nullptr))
        {
            DPF(L"DeviceIoControl failed: 0x%08lx\n", GetLastError());
            bytesReturned = 0;
        }

        DWORD copyBytes = std::min(bytesReturned, bytesNeeded);
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
        wavPath = outputFile;
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
        if (ConvertRawToWav(outputFile, wavPath.c_str(), &loopbackFormat))
            DPF(L"Converted %s to %s\n", outputFile, wavPath.c_str());
        else
            DPF(L"Failed to convert %s\n", outputFile);

        // Send the recorded audio to Google Cloud for processing and playback
        ProcessAudioWithGoogle(wavPath, targetLang, pRenderDevice);
    }

    pAudioClient->Release();
    pRenderDevice->Release();
    CoUninitialize();
    DPF_EXIT();
    return 0;
}

