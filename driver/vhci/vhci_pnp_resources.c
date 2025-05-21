#include "vhci.h"

#include "vhci_dev.h"
#include "vhci_irp.h"

static PAGEABLE PIO_RESOURCE_REQUIREMENTS_LIST
get_query_empty_resource_requirements(void)
{
	PIO_RESOURCE_REQUIREMENTS_LIST	reqs;

	//reqs = ExAllocatePoolWithTag(PagedPool, sizeof(IO_RESOURCE_REQUIREMENTS_LIST), USBIP_VHCI_POOL_TAG);
	reqs = ExAllocatePool2(POOL_FLAG_PAGED, sizeof(IO_RESOURCE_REQUIREMENTS_LIST), USBIP_VHCI_POOL_TAG);
	
	if (reqs == NULL) {
		return NULL;
	}
	reqs->ListSize = sizeof(IO_RESOURCE_REQUIREMENTS_LIST);
	reqs->BusNumber = 10;
	reqs->SlotNumber = 1;
	reqs->AlternativeLists = 0;
	reqs->List[0].Version = 1;
	reqs->List[0].Revision = 1;
	reqs->List[0].Count = 0;
	return reqs;
}

static PAGEABLE PCM_RESOURCE_LIST
get_query_empty_resources(void)
{
	PCM_RESOURCE_LIST	rscs;

	//rscs = ExAllocatePoolWithTag(PagedPool, sizeof(CM_RESOURCE_LIST), USBIP_VHCI_POOL_TAG);
	rscs = ExAllocatePool2(POOL_FLAG_PAGED, sizeof(CM_RESOURCE_LIST), USBIP_VHCI_POOL_TAG);

	
	if (rscs == NULL) {
		return NULL;
	}
	rscs->Count = 0;
	return rscs;
}

PAGEABLE NTSTATUS
pnp_query_resource_requirements(pvdev_t vdev, PIRP irp)
{
	if (!IS_FDO(vdev->type)) {
		irp->IoStatus.Information = (ULONG_PTR)get_query_empty_resource_requirements();
		return irp_done(irp, STATUS_SUCCESS);
	}
	else {
		return irp_pass_down(vdev->devobj_lower, irp);
	}
}

PAGEABLE NTSTATUS
pnp_query_resources(pvdev_t vdev, PIRP irp)
{
	if (!IS_FDO(vdev->type)) {
		irp->IoStatus.Information = (ULONG_PTR)get_query_empty_resources();
		return irp_done(irp, STATUS_SUCCESS);
	}
	else {
		return irp_pass_down(vdev->devobj_lower, irp);
	}
}

PAGEABLE NTSTATUS
pnp_filter_resource_requirements(pvdev_t vdev, PIRP irp)
{
	if (IS_FDO(vdev->type)) {
		irp->IoStatus.Information = (ULONG_PTR)get_query_empty_resource_requirements();
		return irp_done(irp, STATUS_SUCCESS);
	}
	else {
		return irp_done(irp, STATUS_NOT_SUPPORTED);
	}
}