# folkaurixsvc

This is a simple user‑mode application that reads audio captured by the
SysVAD loopback device.  The tool opens `\\.\SysVADLoopback` and issues
the `IOCTL_SYSVAD_GET_LOOPBACK_DATA` control code in a loop.  Any data
returned from the driver can be written to a file and is also played
back through a user selected audio render device.

## Building
The project is a standard Visual Studio console application.  Add
`folkaurixsvc.cpp` to a new Win32 project and build for x64 or any
desired architecture.

## Usage
```
folkaurixsvc.exe [-f output.raw] [-l target-language]
```
When started, the program lists all active speaker devices and lets the
user choose one. Audio from the SysVAD loopback driver is streamed to
the selected device. Use `-f` to specify an optional file where the PCM
data will be written. Press **F9** during capture to stop the program.
If a file was specified, it is automatically converted from raw PCM to a
`.wav` file when capture stops. After conversion the recorded audio is
sent to Google Cloud for speech recognition, translation and
text‑to‑speech synthesis. The synthesized audio is played back to the
previously selected render device. `-l` allows specifying the ISO
language code used for translation (default `zh`). The speech recognizer
now attempts to automatically detect the input language. By default it
listens for English, Spanish, French, German, Japanese and Mandarin.
The WAV header is written using the driver stream's fixed format
(48 kHz, 16‑bit stereo) so it can be played directly.
