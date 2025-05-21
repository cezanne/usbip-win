#include "vhci.h"

#include <usbdi.h>

#include "globals.h"
#include "usbreq.h"
#include "vhci_pnp.h"

//
// Global Debug Level
//

GLOBALS Globals;

NPAGED_LOOKASIDE_LIST g_lookaside;

PAGEABLE __drv_dispatchType(IRP_MJ_READ)
DRIVER_DISPATCH vhci_read;

PAGEABLE __drv_dispatchType(IRP_MJ_WRITE)
DRIVER_DISPATCH vhci_write;

PAGEABLE __drv_dispatchType(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH vhci_ioctl;

PAGEABLE __drv_dispatchType(IRP_MJ_INTERNAL_DEVICE_CONTROL)
DRIVER_DISPATCH vhci_internal_ioctl;

PAGEABLE __drv_dispatchType(IRP_MJ_PNP)
DRIVER_DISPATCH vhci_pnp;

__drv_dispatchType(IRP_MJ_POWER)
DRIVER_DISPATCH vhci_power;

PAGEABLE __drv_dispatchType(IRP_MJ_SYSTEM_CONTROL)
DRIVER_DISPATCH vhci_system_control;

PAGEABLE DRIVER_ADD_DEVICE vhci_add_device;

static PAGEABLE VOID
vhci_driverUnload(__in PDRIVER_OBJECT drvobj)
{
	UNREFERENCED_PARAMETER(drvobj);

	PAGED_CODE();

	DBGI(DBG_GENERAL, "Unload\n");

	ExDeleteNPagedLookasideList(&g_lookaside);

	//
	// All the device objects should be gone.
	//

	ASSERT(NULL == drvobj->DeviceObject);

	//
	// Here we free all the resources allocated in the DriverEntry
	//

	if (Globals.RegistryPath.Buffer)
		ExFreePool(Globals.RegistryPath.Buffer);
}

static PAGEABLE NTSTATUS
vhci_create(__in PDEVICE_OBJECT devobj, __in PIRP Irp)
{
	pvdev_t	vdev = DEVOBJ_TO_VDEV(devobj);

	PAGED_CODE();

	DBGI(DBG_GENERAL, "vhci_create(%s): Enter\n", dbg_vdev_type(vdev->type));

	// Check to see whether the bus is removed
	if (vdev->DevicePnPState == Deleted) {
		DBGW(DBG_GENERAL, "vhci_create(%s): no such device\n", dbg_vdev_type(vdev->type));

		Irp->IoStatus.Status = STATUS_NO_SUCH_DEVICE;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		return STATUS_NO_SUCH_DEVICE;
	}

	Irp->IoStatus.Information = 0;
	Irp->IoStatus.Status = STATUS_SUCCESS;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	DBGI(DBG_GENERAL, "vhci_create(%s): Leave\n", dbg_vdev_type(vdev->type));

	return STATUS_SUCCESS;
}

static PAGEABLE void
cleanup_vpdo(pvhci_dev_t vhci, PIRP irp)
{
	PIO_STACK_LOCATION  irpstack;
	pvpdo_dev_t	vpdo;

	irpstack = IoGetCurrentIrpStackLocation(irp);
	vpdo = irpstack->FileObject->FsContext;
	if (vpdo) {
		vpdo->fo = NULL;
		irpstack->FileObject->FsContext = NULL;
		if (vpdo->plugged)
			vhci_unplug_port(vhci, (CHAR)vpdo->port);
	}
}

static PAGEABLE NTSTATUS
vhci_cleanup(__in PDEVICE_OBJECT devobj, __in PIRP irp)
{
	pvdev_t	vdev = DEVOBJ_TO_VDEV(devobj);

	PAGED_CODE();

	DBGI(DBG_GENERAL, "vhci_cleanup(%s): Enter\n", dbg_vdev_type(vdev->type));

	// Check to see whether the bus is removed
	if (vdev->DevicePnPState == Deleted) {
		DBGW(DBG_GENERAL, "vhci_cleanup(%s): no such device\n", dbg_vdev_type(vdev->type));
		irp->IoStatus.Status = STATUS_NO_SUCH_DEVICE;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return STATUS_NO_SUCH_DEVICE;
	}
	if (IS_DEVOBJ_VHCI(devobj)) {
		cleanup_vpdo(DEVOBJ_TO_VHCI(devobj), irp);
	}

	irp->IoStatus.Information = 0;
	irp->IoStatus.Status = STATUS_SUCCESS;
	IoCompleteRequest(irp, IO_NO_INCREMENT);

	DBGI(DBG_GENERAL, "vhci_cleanup(%s): Leave\n", dbg_vdev_type(vdev->type));

	return STATUS_SUCCESS;
}

static PAGEABLE NTSTATUS
vhci_close(__in PDEVICE_OBJECT devobj, __in PIRP Irp)
{
	pvdev_t	vdev = DEVOBJ_TO_VDEV(devobj);
	NTSTATUS	status;

	PAGED_CODE();

	// Check to see whether the bus is removed
	if (vdev->DevicePnPState == Deleted) {
		Irp->IoStatus.Status = status = STATUS_NO_SUCH_DEVICE;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		return status;
	}
	Irp->IoStatus.Information = 0;
	Irp->IoStatus.Status = STATUS_SUCCESS;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return STATUS_SUCCESS;
}

PAGEABLE NTSTATUS
DriverEntry(__in PDRIVER_OBJECT drvobj, __in PUNICODE_STRING RegistryPath)
{
	DBGI(DBG_GENERAL, "DriverEntry: Enter\n");

	ExInitializeNPagedLookasideList(&g_lookaside, NULL,NULL, 0, sizeof(struct urb_req), 'USBV', 0);

	// Save the RegistryPath for WMI.
	Globals.RegistryPath.MaximumLength = RegistryPath->Length + sizeof(UNICODE_NULL);
	Globals.RegistryPath.Length = RegistryPath->Length;
	//Globals.RegistryPath.Buffer = ExAllocatePoolWithTag(PagedPool, Globals.RegistryPath.MaximumLength, USBIP_VHCI_POOL_TAG);
	Globals.RegistryPath.Buffer = ExAllocatePool2(POOL_FLAG_PAGED, Globals.RegistryPath.MaximumLength, USBIP_VHCI_POOL_TAG);


	if (!Globals.RegistryPath.Buffer) {
		ExDeleteNPagedLookasideList(&g_lookaside);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	DBGI(DBG_GENERAL, "RegistryPath %p\r\n", RegistryPath);

	RtlCopyUnicodeString(&Globals.RegistryPath, RegistryPath);

	// Set entry points into the driver
	drvobj->MajorFunction[IRP_MJ_CREATE] = vhci_create;
	drvobj->MajorFunction[IRP_MJ_CLEANUP] = vhci_cleanup;
	drvobj->MajorFunction[IRP_MJ_CLOSE] = vhci_close;
	drvobj->MajorFunction[IRP_MJ_READ] = vhci_read;
	drvobj->MajorFunction[IRP_MJ_WRITE] = vhci_write;
	drvobj->MajorFunction[IRP_MJ_PNP] = vhci_pnp;
	drvobj->MajorFunction[IRP_MJ_POWER] = vhci_power;
	drvobj->MajorFunction[IRP_MJ_DEVICE_CONTROL] = vhci_ioctl;
	drvobj->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = vhci_internal_ioctl;
	drvobj->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = vhci_system_control;
	drvobj->DriverUnload = vhci_driverUnload;
	drvobj->DriverExtension->AddDevice = vhci_add_device;

	DBGI(DBG_GENERAL, "DriverEntry: Leave\n");

	return STATUS_SUCCESS;
}
