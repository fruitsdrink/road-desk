/* Road Desk Mirror — GM1 control device.
 * Creates \\.\RoadDeskMirror for userspace probe/IOCTL.
 * XPDM display attachment + dirty/fb = GM2 (see ../display, ../miniport).
 */

#include <ntddk.h>
#include "rdm_ioctl.h"

static PDEVICE_OBJECT g_DeviceObject = NULL;
static UNICODE_STRING g_SymLink;

static NTSTATUS RdmCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  UNREFERENCED_PARAMETER(DeviceObject);
  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return STATUS_SUCCESS;
}

static NTSTATUS RdmDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PIO_STACK_LOCATION sp;
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
  ULONG_PTR info = 0;
  PVOID buf;
  ULONG outLen;
  ULONG code;

  UNREFERENCED_PARAMETER(DeviceObject);

  sp = IoGetCurrentIrpStackLocation(Irp);
  buf = Irp->AssociatedIrp.SystemBuffer;
  outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
  code = sp->Parameters.DeviceIoControl.IoControlCode;

  switch (code) {
    case IOCTL_RDM_GET_STATUS:
      if (outLen < sizeof(RDM_STATUS) || buf == NULL) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      {
        RDM_STATUS* st = (RDM_STATUS*)buf;
        RtlZeroMemory(st, sizeof(*st));
        st->AbiVersion = ROAD_DESK_MIRROR_ABI_VERSION;
        st->Flags = RDM_STATUS_LOADED; /* GM1: control device up; no capture yet */
        st->DriverBuild = RDM_DRIVER_BUILD;
        info = sizeof(RDM_STATUS);
        status = STATUS_SUCCESS;
      }
      break;

    case IOCTL_RDM_GET_INFO:
      if (outLen < sizeof(RDM_INFO) || buf == NULL) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      {
        RDM_INFO* inf = (RDM_INFO*)buf;
        RtlZeroMemory(inf, sizeof(*inf));
        /* GM2 fills real desktop metrics after XPDM attach */
        info = sizeof(RDM_INFO);
        status = STATUS_SUCCESS;
      }
      break;

    case IOCTL_RDM_GET_DIRTY:
    case IOCTL_RDM_MAP_FB:
      status = STATUS_NOT_SUPPORTED;
      break;

    default:
      status = STATUS_INVALID_DEVICE_REQUEST;
      break;
  }

  Irp->IoStatus.Status = status;
  Irp->IoStatus.Information = info;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return status;
}

static VOID RdmUnload(PDRIVER_OBJECT DriverObject) {
  UNREFERENCED_PARAMETER(DriverObject);
  if (g_SymLink.Buffer != NULL) {
    IoDeleteSymbolicLink(&g_SymLink);
    g_SymLink.Buffer = NULL;
  }
  if (g_DeviceObject != NULL) {
    IoDeleteDevice(g_DeviceObject);
    g_DeviceObject = NULL;
  }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
  UNICODE_STRING devName;
  NTSTATUS status;

  UNREFERENCED_PARAMETER(RegistryPath);

  RtlInitUnicodeString(&devName, RDM_NT_DEVICE_NAME);
  RtlInitUnicodeString(&g_SymLink, RDM_DOS_DEVICE_NAME);

  status = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN, 0, FALSE,
                          &g_DeviceObject);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  status = IoCreateSymbolicLink(&g_SymLink, &devName);
  if (!NT_SUCCESS(status)) {
    IoDeleteDevice(g_DeviceObject);
    g_DeviceObject = NULL;
    return status;
  }

  DriverObject->MajorFunction[IRP_MJ_CREATE] = RdmCreateClose;
  DriverObject->MajorFunction[IRP_MJ_CLOSE] = RdmCreateClose;
  DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = RdmDeviceControl;
  DriverObject->DriverUnload = RdmUnload;

  g_DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
  return STATUS_SUCCESS;
}
