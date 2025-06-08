# folkaurixsvc

This is a simple user‑mode application that reads audio captured by the
SysVAD loopback device.  The tool opens `\\.\SysVADLoopback` and issues
the `IOCTL_SYSVAD_GET_LOOPBACK_DATA` control code in a loop.  Any data
returned from the driver is optionally written to a file specified on
the command line.

## Building
The project is a standard Visual Studio console application.  Add
`folkaurixsvc.cpp` to a new Win32 project and build for x64 or any
desired architecture.

## Usage
```
folkaurixsvc.exe [output.raw]
```
If an output file is supplied, PCM data from the driver will be written
to that file.  Otherwise the program simply prints the number of bytes
received from each request.
