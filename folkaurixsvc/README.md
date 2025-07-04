# folkaurixsvc

This is a simple user‑mode application that reads audio captured by the
SysVAD loopback device.  The tool opens `\\.\SysVADLoopback` and issues
the `IOCTL_SYSVAD_GET_LOOPBACK_DATA` control code in a loop.  Captured
PCM blocks are streamed to Google Cloud for speech recognition,
translation and text‑to‑speech synthesis.  The translated speech is
played back through the system default speaker using the Azure Speech
SDK. Optionally the raw PCM data can also be written to a file.

The driver also supports the `IOCTL_SYSVAD_SET_LOOPBACK_ENABLED`
control code which toggles whether captured audio is written to the
loopback buffer. `folkaurixsvc` sends this IOCTL with `TRUE` when it
starts recording and with `FALSE` just before exiting.

## Building
The project is a standard Visual Studio console application.  Add
`folkaurixsvc.cpp` to a new Win32 project and build for x64 or any
desired architecture.

## Usage
```
folkaurixsvc.exe [-f output.raw] [-tf output.wav] [-l target-language] [-k azure-key]
```
If `-k` is omitted, the program reads the Azure Speech subscription key from
the `AZURE_KEY` environment variable.
Captured audio is streamed to Google Cloud and the translated speech is
played immediately on the default speaker. Use
`-f` to specify an optional file where the raw PCM data will be saved. Use
`-tf` to specify a base filename for saving synthesized speech. Two files
`output1.wav` and `output2.wav` will be created where `output1.wav` contains
the audio before resampling and `output2.wav` contains the resampled audio
that is also played on the default speaker.
Press **F9** during capture to stop the program. `-l` allows specifying
the ISO language code used for translation (default `zh`). The speech
recognizer automatically detects the input language from a set of
English, Spanish, French, German, Japanese and Mandarin.
