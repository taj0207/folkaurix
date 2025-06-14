#!/usr/bin/env python3
"""Send a WAV file to Google Cloud Speech-to-Text and print the transcript."""

import argparse
import os
import wave
import audioop

from google.cloud import speech


def load_wav_mono(path: str) -> tuple[bytes, int]:
    """Read a WAV file and return mono PCM data and sample rate."""
    with wave.open(path, "rb") as wf:
        params = wf.getparams()
        frames = wf.readframes(params.nframes)

    if params.nchannels != 1:
        frames = audioop.tomono(frames, params.sampwidth, 0.5, 0.5)

    return frames, params.framerate


def main() -> None:
    parser = argparse.ArgumentParser(description="Send a WAV file to Google Cloud STT")
    parser.add_argument("audio", help="Path to WAV file")
    parser.add_argument(
        "credentials",
        help="Path to Google Cloud service account JSON key",
    )
    parser.add_argument(
        "--language", default="en-US", help="Language code of the audio (default: en-US)"
    )
    parser.add_argument(
        "--rate", type=int, default=None,
        help="Sample rate in Hz. Defaults to the rate in the WAV file"
    )
    args = parser.parse_args()

    os.environ["GOOGLE_APPLICATION_CREDENTIALS"] = args.credentials

    client = speech.SpeechClient()

    audio_data, file_rate = load_wav_mono(args.audio)

    audio = speech.RecognitionAudio(content=audio_data)
    config = speech.RecognitionConfig(
        encoding=speech.RecognitionConfig.AudioEncoding.LINEAR16,
        language_code=args.language,
        sample_rate_hertz=args.rate or file_rate,
    )

    response = client.recognize(config=config, audio=audio)

    for result in response.results:
        if result.alternatives:
            print(result.alternatives[0].transcript)


if __name__ == "__main__":
    main()
