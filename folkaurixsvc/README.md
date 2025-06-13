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

also written to that file. Press **F9** during capture to stop the
program. If an output file was specified, it will automatically be
converted from raw PCM to a `.wav` file when capture stops.
