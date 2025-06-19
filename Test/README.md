# Test Audio Playback Application

This folder contains a simple Python command-line application that plays an
audio file to a specified output device using the `sounddevice` module.

## Requirements

- Python 3
- [`sounddevice`](https://python-sounddevice.readthedocs.io/)
- [`soundfile`](https://pysoundfile.readthedocs.io/)

Install dependencies with:

```sh
pip install sounddevice soundfile
```

## Usage

```sh
python play_audio.py <audio-file> --device <device-name-or-id>
```

Run `python play_audio.py --list-devices` to list the available input and output
devices. You can also use `python -m sounddevice` for detailed information.

## Real-time Speech Translation Example

The `realtime_google_pipeline.cpp` source demonstrates how to capture live PCM
data from the default input device, send it to Google Cloud for speech
recognition, translate the recognised text and synthesize the translation back
to audio in real time. The generated audio is played immediately.

Additional requirements:

- PortAudio
- Google Cloud C++ client libraries (`speech`, `translate`, `texttospeech`)

Build and run the example (simplified command):

```sh
g++ realtime_google_pipeline.cpp -o realtime_pipeline \
    `pkg-config --cflags --libs portaudio-2.0` \
    -lgoogle_cloud_cpp_speech -lgoogle_cloud_cpp_translate \
    -lgoogle_cloud_cpp_texttospeech
./realtime_pipeline [--lang <code>] [-list] [-?]
```

Options:
* `--lang <code>`  Target translation language (default `es`)
* `-list`           List supported language codes
* `-?` or `--help`  Show help

Press `Ctrl+C` to stop the program.

## Google Speech Test Script

`test_google_speech.py` sends a WAV file to Google Cloud Speech-to-Text and
prints the recognised text. The script automatically converts stereo input to
mono so you can use a regular two-channel recording.

```sh
python test_google_speech.py <audio.wav> <credentials.json> [--language CODE]
```

The sample rate is detected from the WAV header unless `--rate` is supplied.
