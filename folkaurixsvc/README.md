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
folkaurixsvc.exe [output.raw]
```
When started, the program lists all active speaker devices and lets the
user choose one. Audio from the SysVAD loopback driver is streamed to
the selected device. If an output file is supplied, the same PCM data is
also written to that file.

## Output format
The optional `output.raw` file contains raw PCM samples with no WAV header.
`folkaurixsvc` prints the sample rate, bit depth and channel count on
startup, which you can use when converting the file.

To convert the raw PCM to a WAV file with `ffmpeg`:

```sh
ffmpeg -f s16le -ar 48000 -ac 2 -i output.raw output.wav
```

Replace the sample format, rate and channel count with the values printed
by the application when it starts.
