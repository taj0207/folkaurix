# FolkAurix

FolkAurix brings together a virtual audio driver based on Microsoft's SysVAD sample with a user-mode application that streams captured audio to cloud services for speech recognition and translation. The repository also contains small utilities for testing audio capture and playback.

## Directory layout

- **sysvad/** – Modified [SysVAD Virtual Audio Device Driver](sysvad/README.md) sample providing loopback and other virtual endpoints.
- **folkaurixsvc/** – [folkaurixsvc](folkaurixsvc/README.md) console application that reads audio from the SysVAD loopback device and sends it to the cloud.
- **Test/** – [Example scripts](Test/README.md) for playing audio files, running real-time translation pipelines and testing Google Speech APIs.

## Quick start

1. Build and install the driver contained in `sysvad` using Visual Studio. Detailed instructions and prerequisites are covered in [sysvad/README.md](sysvad/README.md).
2. Build the service found in `folkaurixsvc` (open the solution file and compile for the desired architecture). See [folkaurixsvc/README.md](folkaurixsvc/README.md) for usage options.
3. Run `folkaurixsvc.exe` and follow the prompts to select an audio render device. Captured speech is recognised, translated and played back immediately.
4. Optional: use the utilities in the `Test` directory to play audio or experiment with the real-time translation pipeline. See [Test/README.md](Test/README.md) for command examples.

This repository assumes you already have the necessary credentials for Azure/Google Cloud services when using the speech features.
