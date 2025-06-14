#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <string>

#include <string.h>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>

// Google Cloud C++ client library headers (optional, may require additional
// dependencies when building)
#include <google/cloud/speech/speech_client.h>
#include <google/cloud/translate/translation_client.h>
#include <google/cloud/texttospeech/text_to_speech_client.h>
#include <fstream>
#include <vector>
#include <algorithm>

// Simple thread-safe queues for streaming audio to Google Cloud and
// feeding synthesized audio back to the render device.
static std::queue<std::vector<char>> g_captureQueue;
static std::queue<std::vector<char>> g_ttsQueue;
static std::mutex g_mutex;
static std::condition_variable g_cv;
static bool g_stop = false;

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
    // Updated namespaces for Google Cloud C++ client libraries.  Recent
    // versions place the service clients outside the versioned proto
    // namespaces.
    using ::google::cloud::speech::SpeechClient;
    using ::google::cloud::speech::v1::RecognitionConfig;
    using ::google::cloud::speech::v1::RecognitionAudio;
    using ::google::cloud::translate_v3::TranslationServiceClient;
    using ::google::cloud::texttospeech::TextToSpeechClient;

    // Read WAV data from disk
    std::ifstream file(wavPath, std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    auto size = file.tellg();
    file.seekg(0);
    std::vector<char> wavData(static_cast<size_t>(size));
    file.read(wavData.data(), size);

    // ---- Speech to Text ----
    SpeechClient speechClient(
        ::google::cloud::speech::MakeSpeechConnection());
    RecognitionConfig recConfig;
    recConfig.set_encoding(RecognitionConfig::LINEAR16);
    recConfig.set_sample_rate_hertz(48000);
    recConfig.set_language_code("en-US");
    // Enable automatic language detection with a set of defaults.
    // The recognizer will pick the best match among these languages.
    recConfig.add_alternative_language_codes("es-ES"); // Spanish
    recConfig.add_alternative_language_codes("fr-FR"); // French
    recConfig.add_alternative_language_codes("de-DE"); // German
    recConfig.add_alternative_language_codes("ja-JP"); // Japanese
    recConfig.add_alternative_language_codes("cmn-Hans-CN"); // Mandarin
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
    TranslationServiceClient transClient(
        ::google::cloud::translate_v3::MakeTranslationServiceConnection());
    ::google::cloud::translation::v3::TranslateTextRequest request;
    request.set_parent("projects/-/locations/global");
    request.add_contents(transcript);
    request.set_target_language_code(targetLanguage);
    auto transResp = transClient.TranslateText(request);
    if (!transResp) return false;
    std::string translatedText;
    if (!transResp->translations().empty()) {
        translatedText = transResp->translations(0).translated_text();
    }

    // ---- Text to Speech ----
    TextToSpeechClient ttsClient(
        ::google::cloud::texttospeech::MakeTextToSpeechConnection());
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
        DWORD copyBytes =
            static_cast<DWORD>(std::min<size_t>(bytesNeeded, remaining));
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

// Real-time pipeline. Captured audio blocks are streamed to Google Cloud
// Speech-to-Text. Final transcripts are translated and synthesized back to
// audio. The resulting PCM blocks are queued for playback on the render device.
bool StartRealtimePipeline(const std::string& targetLang)
{
    using ::google::cloud::speech::SpeechClient;
    using ::google::cloud::speech::v1::RecognitionConfig;
    using ::google::cloud::speech::v1::StreamingRecognitionConfig;
    using ::google::cloud::speech::v1::StreamingRecognizeRequest;
    using ::google::cloud::speech::v1::StreamingRecognizeResponse;
    using ::google::cloud::translate_v3::TranslationServiceClient;
    using ::google::cloud::translation::v3::TranslateTextRequest;
    using ::google::cloud::texttospeech::TextToSpeechClient;
    using ::google::cloud::texttospeech::v1::SynthesisInput;
    using ::google::cloud::texttospeech::v1::VoiceSelectionParams;
    using ::google::cloud::texttospeech::v1::AudioConfig;

    SpeechClient speechClient(
        ::google::cloud::speech::MakeSpeechConnection());
    StreamingRecognitionConfig streamCfg;
    auto* recCfg = streamCfg.mutable_config();
    recCfg->set_encoding(RecognitionConfig::LINEAR16);
    recCfg->set_sample_rate_hertz(48000);
    recCfg->set_language_code("en-US");
    recCfg->add_alternative_language_codes("es-ES");
    recCfg->add_alternative_language_codes("fr-FR");
    recCfg->add_alternative_language_codes("de-DE");
    recCfg->add_alternative_language_codes("ja-JP");
    recCfg->add_alternative_language_codes("cmn-Hans-CN");

    // google-cloud-cpp 2.37+ uses AsyncStreamingRecognize for bidirectional
    // streaming. Start the stream without an initial request and send the
    // configuration as the first message in the writer thread below.
    auto streamer = speechClient.AsyncStreamingRecognize();

    std::thread writer([&] {
        StreamingRecognizeRequest req;
        *req.mutable_streaming_config() = streamCfg;
        streamer->Write(req, grpc::WriteOptions{});
        while (true) {
            std::vector<char> block;
            {
                std::unique_lock<std::mutex> lk(g_mutex);
                g_cv.wait(lk, [] { return !g_captureQueue.empty() || g_stop; });
                if (g_stop && g_captureQueue.empty()) break;
                block = std::move(g_captureQueue.front());
                g_captureQueue.pop();
            }
            StreamingRecognizeRequest dataReq;
            dataReq.set_audio_content(std::string(block.begin(), block.end()));
            streamer->Write(dataReq, grpc::WriteOptions{});
        }
        streamer->WritesDone();
    });

    TranslationServiceClient transClient(
        ::google::cloud::translate_v3::MakeTranslationServiceConnection());
    TextToSpeechClient ttsClient(
        ::google::cloud::texttospeech::MakeTextToSpeechConnection());

    StreamingRecognizeResponse resp;
    while (streamer->Read(resp)) {
        for (auto const& result : resp.results()) {
            if (!result.alternatives().empty() && result.is_final()) {
                std::string transcript = result.alternatives(0).transcript();
                TranslateTextRequest tReq;
                tReq.set_parent("projects/-/locations/global");
                tReq.add_contents(transcript);
                tReq.set_target_language_code(targetLang);
                auto tResp = transClient.TranslateText(tReq);
                if (!tResp) continue;
                std::string translated;
                if (!tResp->translations().empty())
                    translated = tResp->translations(0).translated_text();
                SynthesisInput sInput;
                sInput.set_text(translated);
                VoiceSelectionParams voice;
                voice.set_language_code(targetLang);
                AudioConfig cfg;
                cfg.set_audio_encoding(
                    ::google::cloud::texttospeech::v1::AudioEncoding::LINEAR16);
                cfg.set_sample_rate_hertz(48000);
                auto ttsResp = ttsClient.SynthesizeSpeech(sInput, voice, cfg);
                if (!ttsResp) continue;
                std::vector<char> pcm(ttsResp->audio_content().begin(),
                                     ttsResp->audio_content().end());
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    g_ttsQueue.push(std::move(pcm));
                }
                g_cv.notify_one();
            }
        }
        if (g_stop) break;
    }

    writer.join();
    return true;
}

// Continuously writes PCM samples from g_ttsQueue to the render client.
void PlaybackThread(IAudioClient* pClient, IAudioRenderClient* pRender,
                    const WAVEFORMATEX& fmt)
{
    UINT32 bufferFrames = 0;
    pClient->GetBufferSize(&bufferFrames);
    pClient->Start();
    while (true) {
        UINT32 padding = 0;
        pClient->GetCurrentPadding(&padding);
        UINT32 framesToWrite = bufferFrames - padding;
        if (framesToWrite == 0) {
            if (g_stop && g_ttsQueue.empty()) break;
            Sleep(10);
            continue;
        }

        BYTE* pData = nullptr;
        if (FAILED(pRender->GetBuffer(framesToWrite, &pData))) break;

        DWORD bytesNeeded = framesToWrite * fmt.nBlockAlign;
        std::vector<char> chunk;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (!g_ttsQueue.empty()) {
                chunk = std::move(g_ttsQueue.front());
                g_ttsQueue.pop();
            }
        }

        // Use std::min to clamp the number of bytes copied to the available
        // data in the current TTS chunk.
        size_t copyBytes = std::min<size_t>(bytesNeeded, chunk.size());
        if (copyBytes) memcpy(pData, chunk.data(), copyBytes);
        if (copyBytes < bytesNeeded)
            ZeroMemory(pData + copyBytes, bytesNeeded - copyBytes);

        if (copyBytes < chunk.size()) {
            std::vector<char> remain(chunk.begin() + copyBytes, chunk.end());
            std::lock_guard<std::mutex> lk(g_mutex);
            g_ttsQueue.push(std::move(remain));
        }

        pRender->ReleaseBuffer(framesToWrite, 0);
        if (g_stop && g_ttsQueue.empty()) {
            Sleep(10);
        }
    }
    pClient->Stop();
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
    g_stop = false;
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

    std::thread playback(PlaybackThread, pAudioClient, pRenderClient, loopbackFormat);
    std::thread pipeline(StartRealtimePipeline, targetLang);

    BYTE buffer[4096];
    DWORD bytesReturned = 0;
    bool stopRequested = false;
    while (!stopRequested)
    {
        if (GetAsyncKeyState(VK_F9) & 0x8000)
        {
            stopRequested = true;
            continue;
        }

        if (!DeviceIoControl(hDevice,
                             IOCTL_SYSVAD_GET_LOOPBACK_DATA,
                             nullptr,
                             0,
                             buffer,
                             static_cast<DWORD>(sizeof(buffer)),
                             &bytesReturned,
                             nullptr))
        {
            bytesReturned = 0;
        }

        if (bytesReturned > 0)
        {
            if (hFile != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                WriteFile(hFile, buffer, bytesReturned, &written, nullptr);
            }

            std::vector<char> data(buffer, buffer + bytesReturned);
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                g_captureQueue.push(std::move(data));
            }
            g_cv.notify_one();
        }

        Sleep(10);
    }

    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);
    CloseHandle(hDevice);

    g_stop = true;
    g_cv.notify_all();
    pipeline.join();
    playback.join();

    pRenderClient->Release();
    CloseHandle(hAudioEvent);

    pAudioClient->Release();
    pRenderDevice->Release();
    CoUninitialize();
    DPF_EXIT();
    return 0;
}

