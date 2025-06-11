import argparse
import sounddevice as sd
import soundfile as sf


def main():
    parser = argparse.ArgumentParser(
        description="Playback an audio file to a specified speaker")
    parser.add_argument("filename", help="Path to audio file")
    parser.add_argument(
        "-d",
        "--device",
        type=str,
        default=None,
        help="Output device name or index (see sounddevice.query_devices())",
    )
    args = parser.parse_args()

    data, samplerate = sf.read(args.filename, dtype="float32")
    sd.play(data, samplerate, device=args.device)
    sd.wait()


if __name__ == "__main__":
    main()
