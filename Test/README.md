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

Use `python -m sounddevice` to list available devices.
