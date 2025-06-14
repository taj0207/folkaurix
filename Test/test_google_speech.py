#!/usr/bin/env python3
"""Send a WAV file to Google Cloud Speech-to-Text and print the transcript."""

import argparse
import io
import os

from google.cloud import speech


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
        "--rate", type=int, default=16000, help="Sample rate in Hz (default: 16000)"
    )
    args = parser.parse_args()

    os.environ["GOOGLE_APPLICATION_CREDENTIALS"] = args.credentials

    client = speech.SpeechClient()

    with io.open(args.audio, "rb") as f:
        content = f.read()

    audio = speech.RecognitionAudio(content=content)
    config = speech.RecognitionConfig(
        encoding=speech.RecognitionConfig.AudioEncoding.LINEAR16,
        language_code=args.language,
        sample_rate_hertz=args.rate,
    )

    response = client.recognize(config=config, audio=audio)

    for result in response.results:
        if result.alternatives:
            print(result.alternatives[0].transcript)


if __name__ == "__main__":
    main()
