#include <ntddk.h>
#include "loopback.h"

static PDEVICE_OBJECT g_LoopbackDevice = NULL;

static NTSTATUS LoopbackDispatch(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;

    switch (irpSp->MajorFunction)
    {
    case IRP_MJ_CREATE:
    case IRP_MJ_CLOSE:
        status = STATUS_SUCCESS;
        break;
    case IRP_MJ_DEVICE_CONTROL:
        if (irpSp->Parameters.DeviceIoControl.IoControlCode == IOCTL_SYSVAD_GET_LOOPBACK_DATA)
        {
            ULONG outLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
            PBYTE outBuf = (PBYTE)Irp->AssociatedIrp.SystemBuffer;
            info = LoopbackBuffer_Read(outBuf, outLen);
            status = STATUS_SUCCESS;
        }
        break;
    default:
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS LoopbackControl_CreateDevice(_In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING devName;
    UNICODE_STRING symLink;
    NTSTATUS status;
    RtlInitUnicodeString(&devName, L"\\Device\\SysVADLoopback");
    RtlInitUnicodeString(&symLink, L"\\DosDevices\\SysVADLoopback");

    status = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN, 0, FALSE, &g_LoopbackDevice);
    if (!NT_SUCCESS(status))
        return status;

    g_LoopbackDevice->Flags |= DO_DIRECT_IO;
    for (UINT32 i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i)
        DriverObject->MajorFunction[i] = LoopbackDispatch;

    status = IoCreateSymbolicLink(&symLink, &devName);
    if (!NT_SUCCESS(status))
    {
        IoDeleteDevice(g_LoopbackDevice);
        g_LoopbackDevice = NULL;
    }
    return status;
}

void LoopbackControl_DeleteDevice()
{
    UNICODE_STRING symLink;
    RtlInitUnicodeString(&symLink, L"\\DosDevices\\SysVADLoopback");
    IoDeleteSymbolicLink(&symLink);
    if (g_LoopbackDevice)
    {
        IoDeleteDevice(g_LoopbackDevice);
        g_LoopbackDevice = NULL;
    }
}
