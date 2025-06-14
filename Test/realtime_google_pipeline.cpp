#include <iostream>
#include <queue>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <csignal>
#include <map>
#include <string>

// PortAudio is used for cross-platform real-time audio capture and playback.
#include <portaudio.h>

#include <google/cloud/speech/speech_client.h>
#include <google/cloud/translate/translation_client.h>
#include <google/cloud/texttospeech/text_to_speech_client.h>

namespace {

constexpr int kSampleRate = 16000;
constexpr int kChannels = 1;
constexpr unsigned long kFramesPerBuffer = 320; // ~20ms

std::queue<std::vector<int16_t>> g_queue;
std::mutex g_mutex;
std::condition_variable g_cv;
bool g_finished = false;

const std::map<std::string, std::string> kLanguages = {
    {"en", "English"}, {"es", "Spanish"}, {"fr", "French"},
    {"de", "German"},  {"zh", "Chinese"}, {"ja", "Japanese"}
};

void PrintHelp(const char* prog) {
    std::cout << "Usage: " << prog
              << " [--lang <code>] [-list] [-?]\n"
                 "  --lang <code>  target translation language (default es)\n"
                 "  -list          list supported language codes\n"
                 "  -? or --help   display this help message\n";
}

void PrintLanguages() {
    std::cout << "Supported languages:\n";
    for (const auto& kv : kLanguages) {
        std::cout << "  " << kv.first << " - " << kv.second << "\n";
    }
}

int RecordCallback(const void* input, void*, unsigned long frameCount,
                   const PaStreamCallbackTimeInfo*,
                   PaStreamCallbackFlags,
                   void*) {
    const int16_t* samples = static_cast<const int16_t*>(input);
    if (samples) {
        std::vector<int16_t> data(samples, samples + frameCount);
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_queue.push(std::move(data));
        }
        g_cv.notify_one();
    }
    return g_finished ? paComplete : paContinue;
}

void SignalHandler(int) { g_finished = true; }

}  // namespace

int main(int argc, char* argv[]) {
    std::string targetLang = "es";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-?" || arg == "--help") {
            PrintHelp(argv[0]);
            return 0;
        } else if (arg == "-list") {
            PrintLanguages();
            return 0;
        } else if (arg == "--lang" && i + 1 < argc) {
            targetLang = argv[++i];
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            PrintHelp(argv[0]);
            return 1;
        }
    }

    // Initialize PortAudio
    Pa_Initialize();
    PaStream* inStream = nullptr;
    PaStream* outStream = nullptr;
    Pa_OpenDefaultStream(&inStream, kChannels, 0, paInt16, kSampleRate,
                         kFramesPerBuffer, RecordCallback, nullptr);
    Pa_OpenDefaultStream(&outStream, 0, kChannels, paInt16, kSampleRate,
                         paFramesPerBufferUnspecified, nullptr, nullptr);
    Pa_StartStream(inStream);
    Pa_StartStream(outStream);

    std::signal(SIGINT, SignalHandler);

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
    StreamingRecognitionConfig streamConfig;
    auto* recConfig = streamConfig.mutable_config();
    recConfig->set_encoding(RecognitionConfig::LINEAR16);
    recConfig->set_sample_rate_hertz(kSampleRate);
    recConfig->set_language_code("en-US");

    auto streamer = speechClient.StreamingRecognize(streamConfig);

    std::thread writer([&] {
        StreamingRecognizeRequest req;
        *req.mutable_streaming_config() = streamConfig;
        streamer->Write(req, grpc::WriteOptions{});

        while (!g_finished) {
            std::vector<int16_t> block;
            {
                std::unique_lock<std::mutex> lk(g_mutex);
                g_cv.wait(lk, []{ return !g_queue.empty() || g_finished; });
                if (g_finished && g_queue.empty()) break;
                block = std::move(g_queue.front());
                g_queue.pop();
            }
            StreamingRecognizeRequest dataReq;
            dataReq.set_audio_content(
                std::string(reinterpret_cast<char*>(block.data()),
                            block.size() * sizeof(int16_t)));
            streamer->Write(dataReq, grpc::WriteOptions{});
        }
        streamer->WritesDone();
    });

    TranslationServiceClient transClient(
        ::google::cloud::translate_v3::MakeTranslationServiceConnection());
    TextToSpeechClient ttsClient(
        ::google::cloud::texttospeech::MakeTextToSpeechConnection());

    StreamingRecognizeResponse response;
    while (streamer->Read(response)) {
        for (const auto& result : response.results()) {
            if (!result.alternatives().empty() && result.is_final()) {
                std::string transcript = result.alternatives(0).transcript();

                TranslateTextRequest tReq;
                tReq.set_parent("projects/-/locations/global");
                tReq.add_contents(transcript);
                tReq.set_target_language_code(targetLang);
                tReq.set_source_language_code("en");
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
                cfg.set_sample_rate_hertz(kSampleRate);
                auto ttsResp = ttsClient.SynthesizeSpeech(sInput, voice, cfg);
                if (!ttsResp) continue;
                const std::string& pcm = ttsResp->audio_content();
                Pa_WriteStream(outStream, pcm.data(),
                               pcm.size() / sizeof(int16_t));
            }
        }
        if (g_finished) break;
    }

    writer.join();
    Pa_StopStream(inStream);
    Pa_StopStream(outStream);
    Pa_CloseStream(inStream);
    Pa_CloseStream(outStream);
    Pa_Terminate();
    return 0;
}

