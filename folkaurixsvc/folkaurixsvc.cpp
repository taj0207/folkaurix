// Cloud client library headers
#define GOOGLE 1
#define Azure_API 2
#define AWS 3
#define API Azure_API
#if API == Azure_API
#include <speechapi_cxx.h>
#endif

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
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>
#include <string>

#include <string.h>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>


#if API == GOOGLE
#include <google/cloud/speech/speech_client.h>
#include <google/cloud/translate/translation_client.h>
#include <google/cloud/texttospeech/text_to_speech_client.h>
#elif API == Azure_API
#ifndef AZURE_REGION
#define AZURE_REGION "westus2"
#endif

#ifndef AZURE_ENDPOINT
#define AZURE_ENDPOINT "https://westus2.api.cognitive.microsoft.com"
#endif

#endif
#include <fstream>
#include <vector>
#include <algorithm>
#include <iostream>
#include <limits>
#include <cstdlib>


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
#define DPF_RELEASE(...)  wprintf(__VA_ARGS__)
#endif

// Simple thread-safe queues for streaming audio to Google Cloud and
// delivering transcripts and translations.
static std::queue<std::vector<char>> g_captureQueue;
static std::mutex g_captureMutex;
static std::condition_variable g_captureCv;

static std::queue<std::string> g_transcriptQueue;
static std::mutex g_transcriptMutex;
static std::condition_variable g_transcriptCv;

static std::queue<std::string> g_translationQueue;
static std::mutex g_translationMutex;
static std::condition_variable g_translationCv;


static std::atomic<bool> g_stop(false);
static std::string g_azureKey;

void ClearAllQueues()
{
    {
        std::lock_guard<std::mutex> lk(g_captureMutex);
        while (!g_captureQueue.empty()) g_captureQueue.pop();
    }
    {
        std::lock_guard<std::mutex> lk(g_transcriptMutex);
        while (!g_transcriptQueue.empty()) g_transcriptQueue.pop();
    }
    {
        std::lock_guard<std::mutex> lk(g_translationMutex);
        while (!g_translationQueue.empty()) g_translationQueue.pop();
    }
}

struct ProgramOptions
{
    const wchar_t* outputFile = nullptr;
    const wchar_t* ttsOutputBase = nullptr;
#if API==Azure_API
    const wchar_t* azureInputFile = nullptr;
    bool ttsOnly = false;
    std::string azureKey;
#endif
    std::string targetLang = "zh-Hant";
};

static bool ParseCommandLine(int argc, wchar_t** argv, ProgramOptions& opts)
{
    for (int i = 1; i < argc; ++i)
    {
        if ((wcscmp(argv[i], L"-f") == 0 || wcscmp(argv[i], L"--file") == 0) &&
            i + 1 < argc)
        {
            opts.outputFile = argv[++i];
        }
        else if ((wcscmp(argv[i], L"-tf") == 0 || wcscmp(argv[i], L"--ttsfile") == 0) &&
                 i + 1 < argc)
        {
            opts.ttsOutputBase = argv[++i];
        }
        else if ((wcscmp(argv[i], L"-l") == 0 || wcscmp(argv[i], L"--lang") == 0) &&
                 i + 1 < argc)
        {
            char buf[64] = {};
            wcstombs(buf, argv[++i], sizeof(buf) - 1);
            opts.targetLang = buf;
        }
#if API==Azure_API
        else if ((wcscmp(argv[i], L"-af") == 0 || wcscmp(argv[i], L"--azurefile") == 0) &&
                 i + 1 < argc)
        {
            opts.azureInputFile = argv[++i];
        }
        else if ((wcscmp(argv[i], L"-tts") == 0 || wcscmp(argv[i], L"--texttospeech") == 0))
        {
            opts.ttsOnly = true;
        }
        else if ((wcscmp(argv[i], L"-k") == 0 || wcscmp(argv[i], L"--key") == 0) &&
                 i + 1 < argc)
        {
            char buf[256] = {};
            wcstombs(buf, argv[++i], sizeof(buf) - 1);
            opts.azureKey = buf;
        }
#endif
    }
    return true;
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
    // Use explicit little-endian constants for the RIFF header chunks
    DWORD riff = 0x46464952; // 'RIFF'
    DWORD wave = 0x45564157; // 'WAVE'
    DWORD fmt  = 0x20746d66; // 'fmt '
    DWORD data = 0x61746164; // 'data'
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

struct WaveFileWriter
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    DWORD dataSize = 0;
    DWORD fmtSize = 0;
};

bool OpenWaveFile(WaveFileWriter& writer,
                  const wchar_t* path,
                  const WAVEFORMATEX* pwfx)
{
    writer.handle = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (writer.handle == INVALID_HANDLE_VALUE)
        return false;

    writer.fmtSize = sizeof(WAVEFORMATEX) + pwfx->cbSize;

    DWORD written = 0;
    DWORD riff = 0x46464952; // 'RIFF'
    DWORD wave = 0x45564157; // 'WAVE'
    DWORD fmt  = 0x20746d66; // 'fmt '
    DWORD data = 0x61746164; // 'data'
    DWORD chunkSize = 20 + writer.fmtSize; // updated on finalize
    DWORD zero = 0;

    WriteFile(writer.handle, &riff, 4, &written, nullptr);
    WriteFile(writer.handle, &chunkSize, 4, &written, nullptr);
    WriteFile(writer.handle, &wave, 4, &written, nullptr);
    WriteFile(writer.handle, &fmt, 4, &written, nullptr);
    WriteFile(writer.handle, &writer.fmtSize, 4, &written, nullptr);
    WriteFile(writer.handle, pwfx, writer.fmtSize, &written, nullptr);
    WriteFile(writer.handle, &data, 4, &written, nullptr);
    WriteFile(writer.handle, &zero, 4, &written, nullptr);
    return true;
}

bool WriteWaveData(WaveFileWriter& writer, const void* data, DWORD size)
{
    DWORD written = 0;
    if (!WriteFile(writer.handle, data, size, &written, nullptr))
        return false;
    writer.dataSize += written;
    return written == size;
}

void FinalizeWaveFile(WaveFileWriter& writer)
{
    if (writer.handle == INVALID_HANDLE_VALUE)
        return;

    DWORD chunkSize = 20 + writer.fmtSize + writer.dataSize;
    LARGE_INTEGER pos = {};
    DWORD written = 0;

    pos.QuadPart = 4;
    SetFilePointerEx(writer.handle, pos, nullptr, FILE_BEGIN);
    WriteFile(writer.handle, &chunkSize, 4, &written, nullptr);

    pos.QuadPart = 20 + writer.fmtSize + 4;
    SetFilePointerEx(writer.handle, pos, nullptr, FILE_BEGIN);
    WriteFile(writer.handle, &writer.dataSize, 4, &written, nullptr);

    CloseHandle(writer.handle);
    writer.handle = INVALID_HANDLE_VALUE;
}

// Simple helper to check if the destination format uses IEEE float samples.
static bool IsFloatFormat(const WAVEFORMATEX& fmt)
{
    if (fmt.wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return true;
    if (fmt.wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
    {
        const WAVEFORMATEXTENSIBLE* wfex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&fmt);
        return wfex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

// Resamples 16‑bit mono PCM at 16 kHz to the specified render format.
static std::vector<char> ResampleToRenderFormat(const std::vector<char>& input,
                                               const WAVEFORMATEX& outFmt)
{
    const int srcRate = 16000;
    size_t srcSamples = input.size() / sizeof(int16_t);
    const int16_t* src = reinterpret_cast<const int16_t*>(input.data());

    size_t dstSamples = static_cast<size_t>(
        static_cast<double>(srcSamples) * outFmt.nSamplesPerSec / srcRate + 0.5);

    std::vector<char> output(dstSamples * outFmt.nBlockAlign);

    bool useFloat = IsFloatFormat(outFmt);
    float* floatDst = nullptr;
    int32_t* int32Dst = nullptr;
    uint8_t* int24Dst = nullptr;
    int16_t* int16Dst = nullptr;

    if (useFloat)
        floatDst = reinterpret_cast<float*>(output.data());
    else if (outFmt.wBitsPerSample == 32)
        int32Dst = reinterpret_cast<int32_t*>(output.data());
    else if (outFmt.wBitsPerSample == 24)
        int24Dst = reinterpret_cast<uint8_t*>(output.data());
    else
        int16Dst = reinterpret_cast<int16_t*>(output.data());

    for (size_t i = 0; i < dstSamples; ++i)
    {
        double pos = static_cast<double>(i) * srcRate / outFmt.nSamplesPerSec;
        size_t idx = static_cast<size_t>(pos);
        double frac = pos - idx;
        if (idx >= srcSamples - 1) idx = srcSamples - 1;

        int16_t s1 = src[idx];
        int16_t s2 = src[idx + (idx + 1 < srcSamples ? 1 : 0)];
        double sample = s1 + (s2 - s1) * frac;

        for (int ch = 0; ch < outFmt.nChannels; ++ch)
        {
            if (useFloat)
            {
                floatDst[i * outFmt.nChannels + ch] = static_cast<float>(sample / 32768.0);
            }
            else if (outFmt.wBitsPerSample == 32)
            {
                int32Dst[i * outFmt.nChannels + ch] = static_cast<int32_t>(sample) * 65536;
            }
            else if (outFmt.wBitsPerSample == 24)
            {
                int32_t val = static_cast<int32_t>(sample) * 256;
                size_t offset = (i * outFmt.nChannels + ch) * 3;
                int24Dst[offset] = static_cast<uint8_t>(val & 0xFF);
                int24Dst[offset + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
                int24Dst[offset + 2] = static_cast<uint8_t>((val >> 16) & 0xFF);
            }
            else
            {
                int16Dst[i * outFmt.nChannels + ch] = static_cast<int16_t>(sample);
            }
        }
    }

    return output;
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
            std::unique_lock<std::mutex> lk(g_transcriptMutex);
            g_transcriptCv.wait(lk, [] { return !g_transcriptQueue.empty() || g_stop; });
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
            std::lock_guard<std::mutex> lk(g_translationMutex);
            g_translationQueue.push(std::move(translated));
        }
        g_translationCv.notify_one();
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
            std::unique_lock<std::mutex> lk(g_translationMutex);
            g_translationCv.wait(lk, [] { return !g_translationQueue.empty() || g_stop; });
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
                // Playback is handled directly by the SDK when using
                // AudioConfig::FromDefaultSpeakerOutput().
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
    // Configure streaming recognition for 16 kHz mono audio.
    recCfg->set_sample_rate_hertz(16000);
    recCfg->set_language_code("en-US");
    recCfg->set_audio_channel_count(1);
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
                std::unique_lock<std::mutex> lk(g_captureMutex);
                g_captureCv.wait(lk, [] { return !g_captureQueue.empty() || g_stop; });
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
            //DPF(L"Sent block to gcs server stt (%ld)\n", block.size());
            Sleep(1);
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
                            std::lock_guard<std::mutex> lk(g_transcriptMutex);
                            g_transcriptQueue.push(std::move(transcript));
                        }
                        g_transcriptCv.notify_one();
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
    reader.join();

    auto status = streamer->Finish().get();
    if (!status.ok()) {
        std::cerr << "!!! Speech-to-Text stream finished with a FATAL error: "
                  << status.message() << std::endl;
    } else {
        std::wcout << L"Speech-to-Text stream finished successfully (OK)." << std::endl;
    }

    DPF_EXIT();
    return true;
}
#endif

#if API == Azure_API
bool StartAzurePipeline(const std::string& targetLang)
{
    std::cout << GetConsoleOutputCP() <<std::endl;
    SetConsoleOutputCP(CP_UTF8);
    using namespace Microsoft::CognitiveServices::Speech;
    using namespace Microsoft::CognitiveServices::Speech::Translation;
    using namespace Microsoft::CognitiveServices::Speech::Audio;

    DPF_ENTER();

    auto speechConfig = SpeechTranslationConfig::FromSubscription(g_azureKey.c_str(), AZURE_REGION);
    speechConfig->SetSpeechRecognitionLanguage("en-US");
    speechConfig->AddTargetLanguage(targetLang);

    auto pushStream = AudioInputStream::CreatePushStream();
    auto audioInput = AudioConfig::FromStreamInput(pushStream);
    auto recognizer = TranslationRecognizer::FromConfig(speechConfig, audioInput);

    std::thread writer([&] {
        while (!g_stop) {
            std::vector<char> block;
            {
                std::unique_lock<std::mutex> lk(g_captureMutex);
                g_captureCv.wait(lk, []{ return !g_captureQueue.empty() || g_stop; });
                if (g_stop && g_captureQueue.empty()) break;
                block = std::move(g_captureQueue.front());
                g_captureQueue.pop();
            }
           // DPF(L"Sent block to azure\n");
            pushStream->Write(reinterpret_cast<uint8_t*>(block.data()),
                              block.size());
            Sleep(1);
        }
        pushStream->Close();
    });

    recognizer->Recognizing.Connect([&](const TranslationRecognitionEventArgs& e) {
            std::string lidResult = e.Result->Properties.GetProperty(PropertyId::SpeechServiceConnection_AutoDetectSourceLanguageResult);
            //std::cout << "Recognizing in Language = "<< lidResult << ": Text=" << e.Result->Text << std::endl;
            for (const auto& it : e.Result->Translations)
            {
               // std::cout<<"翻譯成: " << it.first.c_str() <<": " ;
               // std::wcout<< it.second.c_str() << std::endl;
            }
        });


    recognizer->Recognized.Connect([&](const TranslationRecognitionEventArgs& e) {
        auto result = e.Result;
        if (result && result->Reason == ResultReason::TranslatedSpeech) {
            std::string lidResult = result->Properties.GetProperty(PropertyId::SpeechServiceConnection_AutoDetectSourceLanguageResult);
            std::cout << "Recognizing in Language = "<< lidResult << ": Text=" << e.Result->Text << std::endl;
            auto it = result->Translations.find(targetLang);
            if(it == result->Translations.end())
            {
                DPF(L"translate to target lang failed.\n");
            }
            else
            {
                auto text = it->second;
                DPF(L"translated:%hs\n", text.c_str());
                auto ttsConfig = SpeechConfig::FromSubscription(g_azureKey.c_str(), AZURE_REGION);
                ttsConfig->SetSpeechSynthesisLanguage("zh-TW");
                auto audioCfg = AudioConfig::FromDefaultSpeakerOutput();
                auto synthesizer = SpeechSynthesizer::FromConfig(ttsConfig, audioCfg);
                auto resultSpeak = synthesizer->SpeakTextAsync(text).get();
                if (resultSpeak->Reason == ResultReason::Canceled)
                {
                    auto cancellation = SpeechSynthesisCancellationDetails::FromResult(resultSpeak);
                    std::cout << "CANCELED: Reason=" << (int)cancellation->Reason << std::endl;
                    if (cancellation->Reason == CancellationReason::Error)
                    {
                        std::cout << "CANCELED: ErrorCode=" << (int)cancellation->ErrorCode << std::endl;
                        std::cout << "CANCELED: ErrorDetails=[" << cancellation->ErrorDetails << "]" << std::endl;
                        std::cout << "CANCELED: Did you update the subscription info?" << std::endl;
                    }
                }
            }
        }
    });

    recognizer->SessionStarted.Connect([&](const SessionEventArgs& e)
    {
        std::cout << "Session started." << std::endl;
    });

    recognizer->SessionStopped.Connect([&](const SessionEventArgs& e)
    {
        std::cout << "Session stopped." << std::endl;
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


// Captures audio from SysVAD loopback using IOCTL_SYSVAD_GET_LOOPBACK_DATA.
static void CaptureThread(HANDLE hDevice, bool useFile,
                          WaveFileWriter* pWriter)
{
    BYTE buffer[4096];
    DWORD bytesReturned = 0;
    while (!g_stop)
    {
        if (GetAsyncKeyState(VK_F9) & 0x8000)
        {
            g_stop = true;
            g_captureCv.notify_all();
            g_transcriptCv.notify_all();
            g_translationCv.notify_all();
            ClearAllQueues();
            break;
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
            if (useFile && pWriter)
            {
                WriteWaveData(*pWriter, buffer, bytesReturned);
            }

            std::vector<char> data(buffer, buffer + bytesReturned);
            {
                std::lock_guard<std::mutex> lk(g_captureMutex);
                g_captureQueue.push(std::move(data));
            }
            g_captureCv.notify_one();
        }

        Sleep(10);
    }
}

void SpeechContinuousRecognitionWithFile()
{

    using namespace Microsoft::CognitiveServices::Speech;
    using namespace Microsoft::CognitiveServices::Speech::Translation;
    using namespace Microsoft::CognitiveServices::Speech::Audio;

    // <SpeechContinuousRecognitionWithFile>
    // Creates an instance of a speech config with specified endpoint and subscription key.
    // Replace with your own endpoint and subscription key.
    auto config = SpeechConfig::FromEndpoint(AZURE_ENDPOINT, g_azureKey.c_str());

    // Creates a speech recognizer using file as audio input.
    // Replace with your own audio file name.
    auto audioInput = AudioConfig::FromWavFileInput("D:\\10.wav");
    auto recognizer = SpeechRecognizer::FromConfig(config, audioInput);

    // promise for synchronization of recognition end.
    std::promise<void> recognitionEnd;

    // Subscribes to events.
    recognizer->Recognizing.Connect([] (const SpeechRecognitionEventArgs& e)
    {
        std::cout << "Recognizing: Text=" << e.Result->Text << std::endl;
    });

    recognizer->Recognized.Connect([] (const SpeechRecognitionEventArgs& e)
    {
        if (e.Result->Reason == ResultReason::RecognizedSpeech)
        {
            std::cout << "RECOGNIZED: Text=" << e.Result->Text << "\n"
                 << "  Offset=" << e.Result->Offset() << "\n"
                 << "  Duration=" << e.Result->Duration() << std::endl;
        }
        else if (e.Result->Reason == ResultReason::NoMatch)
        {
            std::cout << "NOMATCH: Speech could not be recognized." << std::endl;
        }
    });

    recognizer->Canceled.Connect([&recognitionEnd](const SpeechRecognitionCanceledEventArgs& e)
    {
        std::cout << "CANCELED: Reason=" << (int)e.Reason << std::endl;

        if (e.Reason == CancellationReason::Error)
        {
            std::cout << "CANCELED: ErrorCode=" << (int)e.ErrorCode << "\n"
                 << "CANCELED: ErrorDetails=" << e.ErrorDetails << "\n"
                 << "CANCELED: Did you update the subscription info?" << std::endl;

            recognitionEnd.set_value(); // Notify to stop recognition.
        }
    });

    recognizer->SessionStarted.Connect([&recognitionEnd](const SessionEventArgs& e)
    {
        std::cout << "Session started." << std::endl;
    });

    recognizer->SessionStopped.Connect([&recognitionEnd](const SessionEventArgs& e)
    {
        std::cout << "Session stopped." << std::endl;
        recognitionEnd.set_value(); // Notify to stop recognition.
    });

    // Starts continuous recognition. Uses StopContinuousRecognitionAsync() to stop recognition.
    recognizer->StartContinuousRecognitionAsync().get();

    // Waits for recognition end.
    recognitionEnd.get_future().get();

    // Stops recognition.
    recognizer->StopContinuousRecognitionAsync().get();
    // </SpeechContinuousRecognitionWithFile>
}


// Speech synthesis using the default speaker output.
void SpeechSynthesisToDefaultSpeaker()
{
    using namespace Microsoft::CognitiveServices::Speech;
    using namespace Microsoft::CognitiveServices::Speech::Audio;
    auto config = SpeechConfig::FromSubscription(g_azureKey.c_str(), AZURE_REGION);
    auto audioCfg = AudioConfig::FromDefaultSpeakerOutput();
    auto synthesizer = SpeechSynthesizer::FromConfig(config, audioCfg);

    while (true)
    {
        // Receives a text from console input and synthesize it to push audio output stream.
        std::cout << "Enter some text that you want to synthesize, or enter empty text to exit." << std::endl;
        std::cout << "> ";
        std::string text;
        getline(std::cin, text);
        if (text.empty())
        {
            break;
        }

        auto result = synthesizer->SpeakTextAsync(text).get();

        // Checks result.
        if (result->Reason == ResultReason::SynthesizingAudioCompleted)
        {
            std::cout << "Speech synthesized for text [" << text << "], and the audio was written to output stream." << std::endl;
        }
        else if (result->Reason == ResultReason::Canceled)
        {
            auto cancellation = SpeechSynthesisCancellationDetails::FromResult(result);
            std::cout << "CANCELED: Reason=" << (int)cancellation->Reason << std::endl;

            if (cancellation->Reason == CancellationReason::Error)
            {
                std::cout << "CANCELED: ErrorCode=" << (int)cancellation->ErrorCode << std::endl;
                std::cout << "CANCELED: ErrorDetails=[" << cancellation->ErrorDetails << "]" << std::endl;
                std::cout << "CANCELED: Did you update the subscription info?" << std::endl;
            }
        }
    }

    std::cout << "Done." << std::endl;
}

int wmain(int argc, wchar_t** argv)
{
    DPF_ENTER();

    ProgramOptions opts;
    ParseCommandLine(argc, argv, opts);
#if API==Azure_API
    if (opts.azureKey.empty()) {
        const char* envKey = getenv("AZURE_KEY");
        if (envKey)
            opts.azureKey = envKey;
    }
    if (opts.azureKey.empty()) {
        std::cerr << "AZURE_KEY not provided via -k or environment variable." << std::endl;
        return 1;
    }
    g_azureKey = opts.azureKey;
    if (opts.azureInputFile)
    {
        wprintf(L"Process %s\n", opts.azureInputFile);
        SpeechContinuousRecognitionWithFile();
        return 0;
    }
#endif

    g_stop = false;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        DPF(L"CoInitializeEx failed: 0x%08lx\n", hr);
        DPF_EXIT();
        return 1;
    }

    // Playback is handled directly by the Speech SDK so no render device is
    // opened here.

#if API==Azure_API
    if (opts.ttsOnly)
    {
        SpeechSynthesisToDefaultSpeaker();
        CoUninitialize();
        DPF_EXIT();
        return 0;
    }
#endif

    WAVEFORMATEX captureFormat = {};
    captureFormat.wFormatTag = WAVE_FORMAT_PCM;
    captureFormat.nChannels = 1;
    captureFormat.nSamplesPerSec = 16000;
    captureFormat.wBitsPerSample = 16;
    captureFormat.nBlockAlign =
        captureFormat.nChannels * captureFormat.wBitsPerSample / 8;
    captureFormat.nAvgBytesPerSec =
        captureFormat.nSamplesPerSec * captureFormat.nBlockAlign;
    captureFormat.cbSize = 0;

    HANDLE hDevice = CreateFileW(L"\\\\.\\SysVADLoopback", GENERIC_READ, 0,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hDevice == INVALID_HANDLE_VALUE)
    {
        DPF(L"Failed to open device: %lu\n", GetLastError());
        CoUninitialize();
        DPF_EXIT();
        return 1;
    }

    WaveFileWriter wavWriter;
    bool useFile = false;
    if (opts.outputFile)
    {
        if (!OpenWaveFile(wavWriter, opts.outputFile, &captureFormat))
        {
            DPF(L"Failed to open output file: %lu\n", GetLastError());
            CloseHandle(hDevice);
            CoUninitialize();
            DPF_EXIT();
            return 1;
        }
        useFile = true;
    }

    DPF(L"Press F9 to stop recording...\n");

#if API==GOOGLE
    auto ttsEncoding = EncodingFromWaveFormat(captureFormat);
    std::thread pipeline(StartRealtimePipeline, opts.targetLang, ttsEncoding, 16000);
#elif API == Azure_API
    std::thread pipeline(StartAzurePipeline, opts.targetLang);
#endif

    std::thread capture(CaptureThread, hDevice, useFile, useFile ? &wavWriter : nullptr);

    capture.join();

    if (useFile)
        FinalizeWaveFile(wavWriter);

    CloseHandle(hDevice);

    g_stop = true;
    g_captureCv.notify_all();
    g_transcriptCv.notify_all();
    g_translationCv.notify_all();
    pipeline.join();

    ClearAllQueues();
    CoUninitialize();
    DPF_EXIT();
    return 0;
}

