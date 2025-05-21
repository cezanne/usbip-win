#include "vhci.h"

#include <wdmguid.h>

#include "vhci_pnp.h"
#include "vhci_irp.h"

extern NTSTATUS
pnp_query_id(pvdev_t vdev, PIRP irp, PIO_STACK_LOCATION irpstack);

extern NTSTATUS
pnp_query_device_text(pvdev_t vdev, PIRP irp, PIO_STACK_LOCATION irpstack);

extern NTSTATUS
pnp_query_interface(pvdev_t vdev, PIRP irp, PIO_STACK_LOCATION irpstack);

extern NTSTATUS
pnp_query_dev_relations(pvdev_t vdev, PIRP irp, PIO_STACK_LOCATION irpstack);

extern NTSTATUS
pnp_query_capabilities(pvdev_t vdev, PIRP irp, PIO_STACK_LOCATION irpstack);

extern NTSTATUS
pnp_start_device(pvdev_t vdev, PIRP irp);

extern NTSTATUS
pnp_remove_device(pvdev_t vdev, PIRP irp);

extern BOOLEAN
process_pnp_vpdo(pvpdo_dev_t vpdo, PIRP irp, PIO_STACK_LOCATION irpstack);

extern NTSTATUS
pnp_query_resource_requirements(pvdev_t vdev, PIRP irp);

extern NTSTATUS
pnp_query_resources(pvdev_t vdev, PIRP irp);

extern NTSTATUS
pnp_filter_resource_requirements(pvdev_t vdev, PIRP irp);

#define IRP_PASS_DOWN_OR_SUCCESS(vdev, irp)			\
	do {							\
		if (IS_FDO((vdev)->type)) {			\
			irp->IoStatus.Status = STATUS_SUCCESS;	\
			return irp_pass_down((vdev)->devobj_lower, irp);	\
		}						\
		else						\
			return irp_success(irp);		\
	} while (0)

static PAGEABLE NTSTATUS
pnp_query_stop_device(pvdev_t vdev, PIRP irp)
{
	SET_NEW_PNP_STATE(vdev, StopPending);
	IRP_PASS_DOWN_OR_SUCCESS(vdev, irp);
}

static PAGEABLE NTSTATUS
pnp_cancel_stop_device(pvdev_t vdev, PIRP irp)
{
	if (vdev->DevicePnPState == StopPending) {
		// We did receive a query-stop, so restore.
		RESTORE_PREVIOUS_PNP_STATE(vdev);
		ASSERT(vdev->DevicePnPState == Started);
	}
	IRP_PASS_DOWN_OR_SUCCESS(vdev, irp);
}

static PAGEABLE NTSTATUS
pnp_stop_device(pvdev_t vdev, PIRP irp)
{
	SET_NEW_PNP_STATE(vdev, Stopped);
	IRP_PASS_DOWN_OR_SUCCESS(vdev, irp);
}

static PAGEABLE NTSTATUS
pnp_query_remove_device(pvdev_t vdev, PIRP irp)
{
	switch (vdev->type) {
	case VDEV_VPDO:
		/* vpdo cannot be removed */
		vhub_mark_unplugged_vpdo(VHUB_FROM_VPDO((pvpdo_dev_t)vdev), (pvpdo_dev_t)vdev);
		break;
	default:
		break;
	}
	SET_NEW_PNP_STATE(vdev, RemovePending);
	IRP_PASS_DOWN_OR_SUCCESS(vdev, irp);
}

static PAGEABLE NTSTATUS
pnp_cancel_remove_device(pvdev_t vdev, PIRP irp)
{
	if (vdev->DevicePnPState == RemovePending) {
		RESTORE_PREVIOUS_PNP_STATE(vdev);
	}
	IRP_PASS_DOWN_OR_SUCCESS(vdev, irp);
}

static PAGEABLE NTSTATUS
pnp_surprise_removal(pvdev_t vdev, PIRP irp)
{
	SET_NEW_PNP_STATE(vdev, SurpriseRemovePending);
	IRP_PASS_DOWN_OR_SUCCESS(vdev, irp);
}

static PAGEABLE NTSTATUS
pnp_query_bus_information(PIRP irp)
{
	PPNP_BUS_INFORMATION busInfo;

	PAGED_CODE();

	//busInfo = ExAllocatePoolWithTag(PagedPool, sizeof(PNP_BUS_INFORMATION), USBIP_VHCI_POOL_TAG);
	busInfo = ExAllocatePool2(POOL_FLAG_PAGED, sizeof(PNP_BUS_INFORMATION), USBIP_VHCI_POOL_TAG);
	

	if (busInfo == NULL) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	busInfo->BusTypeGuid = GUID_BUS_TYPE_USB;

	// Some buses have a specific INTERFACE_TYPE value,
	// such as PCMCIABus, PCIBus, or PNPISABus.
	// For other buses, especially newer buses like USBIP, the bus
	// driver sets this member to PNPBus.
	busInfo->LegacyBusType = PNPBus;

	// This is an hypothetical bus
	busInfo->BusNumber = 10;
	irp->IoStatus.Information = (ULONG_PTR)busInfo;

	return irp_success(irp);
}

PAGEABLE NTSTATUS
vhci_pnp(__in PDEVICE_OBJECT devobj, __in PIRP irp)
{
	pvdev_t		vdev = DEVOBJ_TO_VDEV(devobj);
	PIO_STACK_LOCATION	irpstack;
	NTSTATUS	status;

	PAGED_CODE();

	irpstack = IoGetCurrentIrpStackLocation(irp);

	DBGI(DBG_GENERAL | DBG_PNP, "vhci_pnp(%s): Enter: minor:%s, irp: %p\n", dbg_vdev_type(DEVOBJ_VDEV_TYPE(devobj)), dbg_pnp_minor(irpstack->MinorFunction), irp);

	// If the device has been removed, the driver should
	// not pass the IRP down to the next lower driver.
	if (vdev->DevicePnPState == Deleted) {
		irp->IoStatus.Status = status = STATUS_NO_SUCH_DEVICE;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		goto END;
	}

	switch (irpstack->MinorFunction) {
	case IRP_MN_START_DEVICE:
		status = pnp_start_device(vdev, irp);
		break;
	case IRP_MN_QUERY_STOP_DEVICE:
		status = pnp_query_stop_device(vdev, irp);
		break;
	case IRP_MN_CANCEL_STOP_DEVICE:
		status = pnp_cancel_stop_device(vdev, irp);
		break;
	case IRP_MN_STOP_DEVICE:
		status = pnp_stop_device(vdev, irp);
		break;
	case IRP_MN_QUERY_REMOVE_DEVICE:
		status = pnp_query_remove_device(vdev, irp);
		break;
	case IRP_MN_CANCEL_REMOVE_DEVICE:
		status = pnp_cancel_remove_device(vdev, irp);
		break;
	case IRP_MN_REMOVE_DEVICE:
		status = pnp_remove_device(vdev, irp);
		break;
	case IRP_MN_SURPRISE_REMOVAL:
		status = pnp_surprise_removal(vdev, irp);
		break;
	case IRP_MN_QUERY_ID:
		status = pnp_query_id(vdev, irp, irpstack);
		break;
	case IRP_MN_QUERY_DEVICE_TEXT:
		status = pnp_query_device_text(vdev, irp, irpstack);
		break;
	case IRP_MN_QUERY_INTERFACE:
		status = pnp_query_interface(vdev, irp, irpstack);
		break;
	case IRP_MN_QUERY_DEVICE_RELATIONS:
		status = pnp_query_dev_relations(vdev, irp, irpstack);
		break;
	case IRP_MN_QUERY_CAPABILITIES:
		status = pnp_query_capabilities(vdev, irp, irpstack);
		break;
	case IRP_MN_QUERY_BUS_INFORMATION:
		status = pnp_query_bus_information(irp);
		break;
	case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
		status = pnp_query_resource_requirements(vdev, irp);
		break;
	case IRP_MN_QUERY_RESOURCES:
		status = pnp_query_resources(vdev, irp);
		break;
	case IRP_MN_FILTER_RESOURCE_REQUIREMENTS:
		status = pnp_filter_resource_requirements(vdev, irp);
		break;
	default:
		if (process_pnp_vpdo((pvpdo_dev_t)vdev, irp, irpstack))
			status = irp->IoStatus.Status;
		else
			status = irp_done(irp, irp->IoStatus.Status);
		break;
	}

END:
	DBGI(DBG_GENERAL | DBG_PNP, "vhci_pnp(%s): Leave: irp:%p, status:%s\n", dbg_vdev_type(DEVOBJ_VDEV_TYPE(devobj)), irp, dbg_ntstatus(status));

	return status;
}