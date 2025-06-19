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
        help="Output device name or index",
    )
    parser.add_argument(
        "-l",
        "--list-devices",
        action="store_true",
        help="List available input and output devices and exit",
    )
    args = parser.parse_args()

    if args.list_devices:
        devices = sd.query_devices()
        default_in, default_out = sd.default.device
        print("Output devices:")
        for idx, dev in enumerate(devices):
            if dev["max_output_channels"] > 0:
                marker = " (default)" if idx == default_out else ""
                print(f"{idx}: {dev['name']}{marker}")

        print("\nInput devices:")
        for idx, dev in enumerate(devices):
            if dev["max_input_channels"] > 0:
                marker = " (default)" if idx == default_in else ""
                print(f"{idx}: {dev['name']}{marker}")
        return

    data, samplerate = sf.read(args.filename, dtype="float32")
    sd.play(data, samplerate, device=args.device)
    sd.wait()


if __name__ == "__main__":
    main()
