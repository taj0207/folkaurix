#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <functiondiscoverykeys_devpkey.h>
#include <string>

#include <string.h>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>

// Cloud client library headers
#define GOOGLE 1
#define Azure 2
#define AWS 3

#define API Azure

#if API == GOOGLE
#include <google/cloud/speech/speech_client.h>
#include <google/cloud/translate/translation_client.h>
#include <google/cloud/texttospeech/text_to_speech_client.h>
#elif API == Azure
#include <speechapi_cxx.h>
#ifndef AZURE_KEY
#define AZURE_KEY "CpW7Vs5l83ABD2wONlEK0Yc64izsptjySuM7EsGHSSj9n1jTEV7RJQQJ99BFAC8vTInXJ3w3AAAYACOGIUHf"
#endif
#ifndef AZURE_REGION
#define AZURE_REGION "westus2"
#endif
#endif
#include <fstream>
#include <vector>
#include <algorithm>


#ifndef IOCTL_SYSVAD_GET_LOOPBACK_DATA
#define IOCTL_SYSVAD_GET_LOOPBACK_DATA CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_OUT_DIRECT, FILE_READ_ACCESS)
#endif

#ifdef _DEBUG
#define DPF_ENTER() wprintf(L"[ENTER] %S\n", __FUNCTION__)
#define DPF_EXIT()  wprintf(L"[EXIT] %S\n", __FUNCTION__)
#define DPF(...)  wprintf(__VA_ARGS__)
#else
#define DPF_ENTER()
#define DPF_EXIT()
#define DPF(...)  ((void)0)
#endif

// Simple thread-safe queues for streaming audio to Google Cloud and
// feeding synthesized audio back to the render device.
static std::queue<std::vector<char>> g_captureQueue;
static std::queue<std::string> g_transcriptQueue;
static std::queue<std::string> g_translationQueue;
static std::queue<std::vector<char>> g_ttsQueue;
static std::mutex g_mutex;
static std::condition_variable g_cv;
static bool g_stop = false;

void ClearAllQueues()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    while (!g_captureQueue.empty()) g_captureQueue.pop();
    while (!g_transcriptQueue.empty()) g_transcriptQueue.pop();
    while (!g_translationQueue.empty()) g_translationQueue.pop();
    while (!g_ttsQueue.empty()) g_ttsQueue.pop();
}

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

// Determine the appropriate Text-to-Speech encoding from a WAVEFORMATEX
// structure. Only a subset of formats are mapped; all others default to
// LINEAR16.
#if API == GOOGLE
google::cloud::texttospeech::v1::AudioEncoding
EncodingFromWaveFormat(const WAVEFORMATEX& fmt)
{
    switch (fmt.wFormatTag)
    {
    case WAVE_FORMAT_ALAW:
        return google::cloud::texttospeech::v1::AudioEncoding::ALAW;
    case WAVE_FORMAT_MULAW:
        return google::cloud::texttospeech::v1::AudioEncoding::MULAW;
    default:
        return google::cloud::texttospeech::v1::AudioEncoding::LINEAR16;
    }
}
#endif

// Thread that performs translation of transcripts pulled from g_transcriptQueue
// and pushes the translated text to g_translationQueue.
#if API == GOOGLE
void TranslationThread(const std::string& targetLang)
{
    using ::google::cloud::translate_v3::TranslationServiceClient;
    using ::google::cloud::translation::v3::TranslateTextRequest;

    DPF_ENTER();
    TranslationServiceClient client(
        ::google::cloud::translate_v3::MakeTranslationServiceConnection());

    while (true) {
        std::string transcript;
        {
            std::unique_lock<std::mutex> lk(g_mutex);
            g_cv.wait(lk, [] { return !g_transcriptQueue.empty() || g_stop; });
            if (g_stop && g_transcriptQueue.empty()) break;
            transcript = std::move(g_transcriptQueue.front());
            g_transcriptQueue.pop();
        }

        TranslateTextRequest req;
        req.set_parent("projects/-/locations/global");
        req.add_contents(transcript);
        req.set_target_language_code(targetLang);
        auto resp = client.TranslateText(req);
        if (!resp) continue;
        std::string translated;
        if (!resp->translations().empty())
            translated = resp->translations(0).translated_text();

        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_translationQueue.push(std::move(translated));
        }
        g_cv.notify_all();
    }
    DPF_EXIT();
}
#endif

// Synthesizes translated text using the device's playback format.
#if API == GOOGLE
void TtsThread(const std::string& targetLang,
               google::cloud::texttospeech::v1::AudioEncoding encoding,
               int sampleRate)
{
    DPF_ENTER();
    ::google::cloud::texttospeech_v1::TextToSpeechClient client(
        google::cloud::texttospeech::MakeTextToSpeechConnection());

    while (true) {
        std::string text;
        {
            std::unique_lock<std::mutex> lk(g_mutex);
            g_cv.wait(lk, [] { return !g_translationQueue.empty() || g_stop; });
            if (g_stop && g_translationQueue.empty()) break;
            text = std::move(g_translationQueue.front());
            g_translationQueue.pop();
        }

        auto stream = client.AsyncStreamingSynthesize();

        if (!stream->Start().get()) {
            std::cerr << "TTS Stream failed to start." << std::endl;
            continue;
        }

        // --- First Write: Send the configuration ---
        ::google::cloud::texttospeech::v1::StreamingSynthesizeRequest config_req;
        const auto stream_config = config_req.mutable_streaming_config();
        auto* config = stream_config->mutable_streaming_audio_config();
        stream_config->mutable_voice()->set_language_code(targetLang);
        config->set_audio_encoding(encoding);
        config->set_sample_rate_hertz(sampleRate);
        
        if (!stream->Write(config_req, grpc::WriteOptions{}).get()) {
            std::cerr << "TTS Stream failed to write config." << std::endl;
            continue;
        }

        // --- Second Write: Send the text ---
        ::google::cloud::texttospeech::v1::StreamingSynthesizeRequest text_req;
        
        text_req.mutable_input()->set_text(text);
        
        if (!stream->Write(text_req, grpc::WriteOptions{}).get()) {
            std::cerr << "TTS Stream failed to write text." << std::endl;
            continue;
        }

        if (!stream->WritesDone().get()) {
             std::cerr << "TTS Stream WritesDone failed." << std::endl;
             continue;
        }

        // --- Read the audio chunks from the server ---
        while (true) {
            if (g_stop) {
                stream->Cancel();
                break;
            }

            auto chunk_opt = stream->Read().get();
            if (!chunk_opt) break;

            ::google::cloud::texttospeech::v1::StreamingSynthesizeResponse const& response = *chunk_opt;
            if (!response.audio_content().empty()) {
                std::vector<char> pcm(response.audio_content().begin(),
                                      response.audio_content().end());
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    g_ttsQueue.push(std::move(pcm));
                }
                g_cv.notify_all();
            }
        }

        auto status = stream->Finish().get();
        if (!status.ok()) {
            std::cerr << "TTS Stream finished with error: " << status << std::endl;
        }
    }
    DPF_EXIT();
}
#endif

// Real-time pipeline. Captured audio blocks are streamed to Google Cloud
// Speech-to-Text. Final transcripts are sent to a translation thread and the
// resulting translations are synthesized in a separate text-to-speech thread.
#if API == GOOGLE
bool StartRealtimePipeline(const std::string& targetLang,
                           google::cloud::texttospeech::v1::AudioEncoding ttsEncoding,
                           int ttsRate)
{
    DPF_ENTER();
    using ::google::cloud::speech::SpeechClient;
    using ::google::cloud::speech::v1::RecognitionConfig;
    using ::google::cloud::speech::v1::StreamingRecognitionConfig;
    using ::google::cloud::speech::v1::StreamingRecognizeRequest;
    using ::google::cloud::speech::v1::StreamingRecognizeResponse;

    SpeechClient speechClient(
        ::google::cloud::speech::MakeSpeechConnection());
    StreamingRecognitionConfig streamCfg;
    streamCfg.set_interim_results(true);
    streamCfg.set_enable_voice_activity_events(true);
    auto* recCfg = streamCfg.mutable_config();
    recCfg->set_encoding(RecognitionConfig::LINEAR16);
    // Configure streaming recognition for 48 kHz stereo audio.
    recCfg->set_sample_rate_hertz(48000);
    recCfg->set_language_code("en-US");
    recCfg->set_audio_channel_count(2);
    recCfg->set_enable_separate_recognition_per_channel(false);
    recCfg->add_alternative_language_codes("es-ES");
    recCfg->add_alternative_language_codes("fr-FR");
    recCfg->add_alternative_language_codes("de-DE");
    recCfg->add_alternative_language_codes("ja-JP");
    recCfg->add_alternative_language_codes("cmn-Hans-CN");

    // google-cloud-cpp 2.37+ uses AsyncStreamingRecognize for bidirectional
    // streaming. Start the stream without an initial request and send the
    // configuration as the first message in the writer thread below.
    auto streamer = speechClient.AsyncStreamingRecognize();

    // Start the stream and verify success.
    if (!streamer->Start().get()) {
        std::cerr << "Stream failed to start" << std::endl;
        return 1;
    }

    std::thread writer([&] 
    {
        DPF(L"Writer thread started\n");
        StreamingRecognizeRequest req;
        *req.mutable_streaming_config() = streamCfg;
        if (!streamer->Write(req, grpc::WriteOptions{}).get()) {
            std::cerr << "Failed to write config" << std::endl;
            return;
        }

        while (true) 
        {
            std::vector<char> block;
            {
                std::unique_lock<std::mutex> lk(g_mutex);
                g_cv.wait(lk, [] { return !g_captureQueue.empty() || g_stop; });
                if (g_stop) 
                {
                    while(!g_captureQueue.empty())
                        g_captureQueue.pop();
                    break;
                }
                block = std::move(g_captureQueue.front());
                g_captureQueue.pop();
            }

            StreamingRecognizeRequest dataReq;
            dataReq.set_audio_content(std::string(block.begin(), block.end()));
            if (!streamer->Write(dataReq, grpc::WriteOptions{}).get()) {
                std::cerr << "Failed to write audio block, stream may have closed." << std::endl;
                break;
            }
            DPF(L"Sent block to gcs server stt (%ld)\n", block.size());
            Sleep(10);
        }
        streamer->WritesDone();
        DPF(L"Writer thread finished\n");
    });

    // This lambda replaces the old `StreamingReaderThread` function.
    // It captures `streamer` by reference.
    std::thread reader([&] {
        DPF(L"Reader thread started\n");
        while (true) {
            auto resp_opt = streamer->Read().get();
            DPF(L"got returned from read()\n");
            if (!resp_opt) break; // Stream is done

            using StreamingResp =
                google::cloud::speech::v1::StreamingRecognizeResponse;
            auto event = resp_opt->speech_event_type();
            if (event != StreamingResp::SPEECH_EVENT_UNSPECIFIED) {
                std::string name;
                switch (event) {
                    case StreamingResp::END_OF_SINGLE_UTTERANCE:
                        name = "END_OF_SINGLE_UTTERANCE";
                        break;
                    case StreamingResp::SPEECH_ACTIVITY_BEGIN:
                        name = "SPEECH_ACTIVITY_BEGIN";
                        break;
                    case StreamingResp::SPEECH_ACTIVITY_END:
                        name = "SPEECH_ACTIVITY_END";
                        break;
                    default:
                        name = std::to_string(event);
                        break;
                }
                std::cout << "Voice activity event: " << name << std::endl;
            }

            for (auto const& result : resp_opt->results()) {
                if (!result.alternatives().empty()) {
                    std::string transcript = result.alternatives(0).transcript();
                    if (result.is_final()) {
                        std::cout << "Transcript: " << transcript << std::endl;
                        {
                            std::lock_guard<std::mutex> lk(g_mutex);
                            g_transcriptQueue.push(std::move(transcript));
                        }
                        g_cv.notify_all();
                    } else {
                        std::cout << "Interim: " << transcript << std::endl;
                    }
                }
            }
        }
        DPF(L"Reader thread finished\n");
    });

    DPF(L"Translate loop is on going\n");
    std::thread translator(TranslationThread, targetLang);
    std::thread tts(TtsThread, targetLang, ttsEncoding, ttsRate);


    translator.join();
    tts.join();

    writer.join();
    // Cancel the stream to unblock the reader if it is waiting
   // streamer->Cancel();
    reader.join();

    auto status = streamer->Finish().get();
    if (!status.ok()) {
        // status.message() 會告訴你詳細的錯誤原因
        std::cerr << "!!! Speech-to-Text stream finished with a FATAL error: "
                  << status.message() << std::endl;
    } else {
        std::wcout << L"Speech-to-Text stream finished successfully (OK)." << std::endl;
    }


   // g_cv.notify_all();
    DPF_EXIT();
    return true;
}
#endif

#if API == Azure
bool StartAzurePipeline(const std::string& targetLang)
{
    using namespace Microsoft::CognitiveServices::Speech;
    using namespace Microsoft::CognitiveServices::Speech::Translation;
    using namespace Microsoft::CognitiveServices::Speech::Audio;

    DPF_ENTER();

    auto speechConfig = SpeechTranslationConfig::FromSubscription(AZURE_KEY, AZURE_REGION);
    speechConfig->SetSpeechRecognitionLanguage("en-US");
    speechConfig->AddTargetLanguage(targetLang);

    auto pushStream = AudioInputStream::CreatePushStream();
    auto audioInput = AudioConfig::FromStreamInput(pushStream);
    auto recognizer = TranslationRecognizer::FromConfig(speechConfig, audioInput);

    std::thread writer([&] {
        while (!g_stop) {
            std::vector<char> block;
            {
                std::unique_lock<std::mutex> lk(g_mutex);
                g_cv.wait(lk, []{ return !g_captureQueue.empty() || g_stop; });
                if (g_stop && g_captureQueue.empty()) break;
                block = std::move(g_captureQueue.front());
                g_captureQueue.pop();
            }
            pushStream->Write(reinterpret_cast<uint8_t*>(block.data()),
                              block.size());
        }
        pushStream->Close();
    });

    recognizer->Recognized.Connect([&](const TranslationRecognitionEventArgs& e) {
        auto result = e.Result;
        if (result && result->Reason == ResultReason::TranslatedSpeech) {
            auto it = result->Translations.find(targetLang);
            if (it != result->Translations.end()) {
            auto text = it->second;
                auto ttsConfig = SpeechConfig::FromSubscription(AZURE_KEY, AZURE_REGION);
                ttsConfig->SetSpeechSynthesisLanguage(targetLang.c_str());
                ttsConfig->SetSpeechSynthesisOutputFormat(SpeechSynthesisOutputFormat::Riff16Khz16BitMonoPcm);
                auto outStream = AudioOutputStream::CreatePullStream();
                auto audioCfg = AudioConfig::FromStreamOutput(outStream);
                auto synthesizer = SpeechSynthesizer::FromConfig(ttsConfig, audioCfg);
                synthesizer->SpeakTextAsync(text).get();

                std::vector<char> pcm;
                uint8_t buffer[1024];
                while (auto read = outStream->Read(buffer, sizeof(buffer))) {
                    pcm.insert(pcm.end(), buffer, buffer + read);
                    if (read < sizeof(buffer)) break;
                }
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    g_ttsQueue.push(std::move(pcm));
                }
                g_cv.notify_all();
            }
        }
    });

    recognizer->StartContinuousRecognitionAsync().get();
    while (!g_stop) {
        Sleep(10);
    }
    recognizer->StopContinuousRecognitionAsync().get();

    writer.join();

    DPF_EXIT();
    return true;
}
#endif

// Continuously writes PCM samples from g_ttsQueue to the render client.
void PlaybackThread(IAudioClient* pClient, IAudioRenderClient* pRender,
                    const WAVEFORMATEX& fmt)
{
    DPF_ENTER();
    UINT32 bufferFrames = 0;
    pClient->GetBufferSize(&bufferFrames);
    pClient->Start();
    while (true) {
        UINT32 padding = 0;
        pClient->GetCurrentPadding(&padding);
        UINT32 framesToWrite = bufferFrames - padding;
        if (g_stop && g_ttsQueue.empty()) break;
        if (framesToWrite == 0) {
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
        if (!g_stop && g_ttsQueue.empty()) {
            Sleep(10);
        }
    }
    pClient->Stop();
    DPF_EXIT();
}

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

    WAVEFORMATEX renderFormat = *pwfx;

    // SysVAD loopback streams audio in a fixed 2ch/16-bit/48kHz format.
    WAVEFORMATEX captureFormat = {};
    captureFormat.wFormatTag = WAVE_FORMAT_PCM;
    captureFormat.nChannels = 2;
    captureFormat.nSamplesPerSec = 48000;
    captureFormat.wBitsPerSample = 16;
    captureFormat.nBlockAlign =
        captureFormat.nChannels * captureFormat.wBitsPerSample / 8;
    captureFormat.nAvgBytesPerSec =
        captureFormat.nSamplesPerSec * captureFormat.nBlockAlign;
    captureFormat.cbSize = 0;

    REFERENCE_TIME bufferDuration = 10000000; // 1 second
    hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  bufferDuration,
                                  0,
                                  pwfx,
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
    CoTaskMemFree(pwfx);
    pwfx = nullptr;

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

    std::thread playback(PlaybackThread, pAudioClient, pRenderClient, renderFormat);
#if API==GOOGLE
    auto ttsEncoding = EncodingFromWaveFormat(renderFormat);
    std::thread pipeline(StartRealtimePipeline, targetLang, ttsEncoding, renderFormat.nSamplesPerSec);
#elif API == Azure
    std::thread pipeline(StartAzurePipeline, targetLang);
#endif

    BYTE buffer[4096];
    DWORD bytesReturned = 0;
    bool stopRequested = false;
    while (!stopRequested)
    {
        if (GetAsyncKeyState(VK_F9) & 0x8000)
        {
            stopRequested = true;
            g_stop = true;
            g_cv.notify_all();
            ClearAllQueues();
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
            g_cv.notify_all();
        }

        Sleep(10);
    }

    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);

    // If an output RAW file was specified, convert it to WAV after
    // all recording is done. The captureFormat used for capture
    // matches the data written to the RAW file, so reuse it for the
    // WAV header.
    if (outputFile)
    {
        std::wstring rawPath(outputFile);
        std::wstring wavPath = rawPath;
        size_t dot = wavPath.find_last_of(L'.');
        if (dot != std::wstring::npos)
            wavPath.replace(dot, wavPath.size() - dot, L".wav");
        else
            wavPath += L".wav";

        ConvertRawToWav(rawPath.c_str(), wavPath.c_str(), &captureFormat);
    }

    CloseHandle(hDevice);

    g_stop = true;
    g_cv.notify_all();
    pipeline.join();
    playback.join();
    ClearAllQueues();

    pRenderClient->Release();
    CloseHandle(hAudioEvent);

    pAudioClient->Release();
    pRenderDevice->Release();
    CoUninitialize();
    DPF_EXIT();
    return 0;
}

