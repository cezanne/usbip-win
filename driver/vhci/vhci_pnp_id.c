#include "vhci.h"
#include "vhci_pnp.h"
#include "usbip_vhci_api.h"
#include "vhci_irp.h"

#define DEVID_VHCI	HWID_VHCI
/* Device with zero class/subclass/protocol */
#define IS_ZERO_CLASS(vpdo)	((vpdo)->usbclass == 0x00 && (vpdo)->subclass == 0x00 && (vpdo)->protocol == 0x00 && (vpdo)->inum > 1)
/* Device with IAD(Interface Association Descriptor) */
#define IS_IAD_DEVICE(vpdo)	((vpdo)->usbclass == 0xef && (vpdo)->subclass == 0x02 && (vpdo)->protocol == 0x01)
#define DEVID_VHUB	HWID_VHUB

/*
 * The first hardware ID in the list should be the device ID, and
 * the remaining IDs should be listed in order of decreasing suitability.
 */
#define HWIDS_VHCI	DEVID_VHCI L"\0"

#define HWIDS_VHUB \
	DEVID_VHUB L"\0" \
	VHUB_PREFIX L"&VID_" VHUB_VID L"&PID_" VHUB_PID L"\0"

// vdev_type_t is an index
static const LPCWSTR	vdev_devids[] = {
	NULL, DEVID_VHCI, NULL, DEVID_VHUB, NULL,
	L"USB\\VID_%04hx&PID_%04hx" // 21 chars after formatting
};

static const size_t	vdev_devid_size[] = {
	0, sizeof(DEVID_VHCI),
	0, sizeof(DEVID_VHUB),
	0, (21 + 1) * sizeof(WCHAR)
};

/* HW & compatible id use multi string.
 * For single print function, use a semicolon as a string separator,
 * which will be replaced with '\0'. */
static const LPCWSTR	vdev_hwids[] = {
	NULL, HWIDS_VHCI,
	NULL, HWIDS_VHUB,
	NULL, L"USB\\VID_%04hx&PID_%04hx&REV_%04hx;"	// 31 chars after formatting
	      L"USB\\VID_%04hx&PID_%04hx;"		// 22 chars after formatting
};

static const size_t	vdev_hwids_size[] = {
	0, sizeof(HWIDS_VHCI),
	0, sizeof(HWIDS_VHUB),
	0, (31 + 22 + 1) * sizeof(WCHAR)
};

/*
 * For all USB devices, the USB bus driver reports a device ID with the following format:
 * USB\VID_xxxx&PID_yyyy
 */
static NTSTATUS
setup_device_id(pvdev_t vdev, PIRP irp)
{
	PWCHAR	id_dev;
	LPCWSTR	id_fmt;
	size_t	id_size;

	id_fmt = vdev_devids[vdev->type];
	if (id_fmt == NULL) {
		DBGI(DBG_PNP, "%s: query device id: NOT SUPPORTED\n", dbg_vdev_type(vdev->type));
		return STATUS_NOT_SUPPORTED;
	}

	id_size = vdev_devid_size[vdev->type];
	//id_dev = ExAllocatePoolWithTag(PagedPool, id_size, USBIP_VHCI_POOL_TAG);
	id_dev = ExAllocatePool2(POOL_FLAG_PAGED, id_size, USBIP_VHCI_POOL_TAG);
	
	if (id_dev == NULL) {
		DBGE(DBG_PNP, "%s: query device id: out of memory\n", dbg_vdev_type(vdev->type));
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	if (vdev->type == VDEV_VPDO) {
		pvpdo_dev_t	vpdo = (pvpdo_dev_t)vdev;
		RtlStringCbPrintfW(id_dev, id_size, id_fmt, vpdo->vendor, vpdo->product);
	}
	else
		RtlCopyMemory(id_dev, id_fmt, id_size);

	irp->IoStatus.Information = (ULONG_PTR)id_dev;

	DBGI(DBG_PNP, "%s: device id: %S\n", dbg_vdev_type(vdev->type), id_dev);

	return STATUS_SUCCESS;
}

static NTSTATUS
setup_hw_ids(pvdev_t vdev, PIRP irp)
{
	PWCHAR	ids_hw;
	LPCWSTR	ids_fmt;
	size_t	ids_size;

	ids_fmt = vdev_hwids[vdev->type];
	if (ids_fmt == NULL) {
		DBGI(DBG_PNP, "%s: query hw ids: NOT SUPPORTED%s\n", dbg_vdev_type(vdev->type));
		return STATUS_NOT_SUPPORTED;
	}

	ids_size = vdev_hwids_size[vdev->type];
	//ids_hw = ExAllocatePoolWithTag(PagedPool, ids_size, USBIP_VHCI_POOL_TAG);
	ids_hw = ExAllocatePool2(POOL_FLAG_PAGED, ids_size, USBIP_VHCI_POOL_TAG);

	
	if (ids_hw == NULL) {
		DBGE(DBG_PNP, "%s: query hw ids: out of memory\n", dbg_vdev_type(vdev->type));
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	if (vdev->type == VDEV_VPDO) {
		pvpdo_dev_t vpdo = (pvpdo_dev_t)vdev;
		RtlStringCbPrintfW(ids_hw, ids_size, ids_fmt,
				   vpdo->vendor, vpdo->product, vpdo->revision, vpdo->vendor, vpdo->product);
		ids_hw[31 + 22 - 1] = L'\0';
	}
	else {
		RtlCopyMemory(ids_hw, ids_fmt, ids_size);
	}

	DBGI(DBG_PNP, "%s: hw id: %S\n", dbg_vdev_type(vdev->type), ids_hw);

	if (vdev->type == VDEV_VPDO) {
		/* Convert into multi string by replacing a semicolon */
		ids_hw[31 - 1] = L'\0';
	}
	irp->IoStatus.Information = (ULONG_PTR)ids_hw;

	return STATUS_SUCCESS;
}

/*
 * Some old(?) applications may use instance id as USB serial.
 */
static NTSTATUS
setup_inst_id_or_serial(pvdev_t vdev, PIRP irp, BOOLEAN serial)
{
	pvpdo_dev_t	vpdo;
	PWCHAR	id_inst;

	if (vdev->type != VDEV_VPDO) {
		DBGI(DBG_PNP, "%s: query instance id: NOT SUPPORTED\n", dbg_vdev_type(vdev->type));
		return STATUS_NOT_SUPPORTED;
	}

	vpdo = (pvpdo_dev_t)vdev;

	//id_inst = ExAllocatePoolWithTag(PagedPool, (MAX_VHCI_SERIAL_ID + 1) * sizeof(wchar_t), USBIP_VHCI_POOL_TAG);
	id_inst = ExAllocatePool2(POOL_FLAG_PAGED, (MAX_VHCI_SERIAL_ID + 1) * sizeof(wchar_t), USBIP_VHCI_POOL_TAG);

	
	if (id_inst == NULL) {
		DBGE(DBG_PNP, "vpdo: query instance id or serial: out of memory\n");
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	if (vpdo->winstid != NULL)
		RtlStringCchCopyW(id_inst, MAX_VHCI_SERIAL_ID + 1, vpdo->winstid);
	else {
		if (serial)
			id_inst[0] = '\0';
		else
			RtlStringCchPrintfW(id_inst, MAX_VHCI_SERIAL_ID + 1, L"%04hx", vpdo->port);
	}

	irp->IoStatus.Information = (ULONG_PTR)id_inst;

	DBGI(DBG_PNP, "vpdo: %s: %S\n", serial ? "serial": "instance id", id_inst);

	return STATUS_SUCCESS;
}

/*
 * See https://docs.microsoft.com/en-us/windows-hardware/drivers/usbcon/enumeration-of-the-composite-parent-device
 */
static BOOLEAN
need_composite(vpdo_dev_t *vpdo)
{
	if ((IS_ZERO_CLASS(vpdo) || IS_IAD_DEVICE(vpdo)) && vpdo->inum > 1 && vpdo->num_configurations == 1) {
		return TRUE;
	}

	return FALSE;
}

static NTSTATUS
setup_compat_ids(pvdev_t vdev, PIRP irp)
{
	pvpdo_dev_t	vpdo;
	PWCHAR	ids_compat;
	LPCWSTR	ids_fmt =
		L"USB\\Class_%02hhx&SubClass_%02hhx&Prot_%02hhx;" // 33 chars after formatting
		L"USB\\Class_%02hhx&SubClass_%02hhx;" // 25 chars after formatting
		L"USB\\Class_%02hhx;"	// 13 chars after formatting
		L"USB\\COMPOSITE;";	// 14 chars
	size_t	ids_size = (33 + 25 + 13 + 14 + 1) * sizeof(WCHAR);

	if (vdev->type != VDEV_VPDO) {
		DBGI(DBG_PNP, "%s: query compatible id: NOT SUPPORTED\n", dbg_vdev_type(vdev->type));
		return STATUS_NOT_SUPPORTED;
	}

	vpdo = (pvpdo_dev_t)vdev;

	//ids_compat = ExAllocatePoolWithTag(PagedPool, ids_size, USBIP_VHCI_POOL_TAG);
	ids_compat = ExAllocatePool2(POOL_FLAG_PAGED, ids_size, USBIP_VHCI_POOL_TAG);

	
	if (ids_compat == NULL) {
		DBGE(DBG_PNP, "vpdo: query compatible id: out of memory\n");
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	RtlStringCbPrintfW(ids_compat, ids_size, ids_fmt,
			   vpdo->usbclass, vpdo->subclass, vpdo->protocol,
			   vpdo->usbclass, vpdo->subclass,
			   vpdo->usbclass);

	/* Convert last semicolon */
	ids_compat[33 + 25 + 13 + 14 - 1] = L'\0';

	if (!need_composite(vpdo)) {
		/* USB\COMPOSITE is dropped */
		ids_compat[33 + 25 + 13 - 1] = L'\0';
	}

	DBGI(DBG_PNP, "vpdo: compatible ids: %S\n", ids_compat);

	ids_compat[33 - 1] = L'\0';
	ids_compat[33 + 25 - 1] = L'\0';
	if (ids_compat[33 + 25 + 13 - 1] == L'\0') {
		/* no composite */
		ids_compat[33 + 25 + 13] = L'\0';
	}
	else {
		ids_compat[33 + 25 + 13 - 1] = L'\0';
		ids_compat[33 + 25 + 13 + 14 - 1] = L'\0';
	}

	irp->IoStatus.Information = (ULONG_PTR)ids_compat;

	return STATUS_SUCCESS;
}

PAGEABLE NTSTATUS
pnp_query_id(pvdev_t vdev, PIRP irp, PIO_STACK_LOCATION irpstack)
{
	NTSTATUS	status = STATUS_NOT_SUPPORTED;

	DBGI(DBG_PNP, "%s: query id: %s\n", dbg_vdev_type(vdev->type), dbg_bus_query_id_type(irpstack->Parameters.QueryId.IdType));

	PAGED_CODE();

	switch (irpstack->Parameters.QueryId.IdType) {
	case BusQueryDeviceID:
		status = setup_device_id(vdev, irp);
		break;
	case BusQueryInstanceID:
		status = setup_inst_id_or_serial(vdev, irp, FALSE);
		break;
	case BusQueryHardwareIDs:
		status = setup_hw_ids(vdev, irp);
		break;
	case BusQueryCompatibleIDs:
		status = setup_compat_ids(vdev, irp);
		break;
	case BusQueryDeviceSerialNumber:
		status = setup_inst_id_or_serial(vdev, irp, FALSE);
		break;
	case BusQueryContainerID:
		break;
	default:
		DBGI(DBG_PNP, "%s: unhandled query id: %s\n", dbg_vdev_type(vdev->type), dbg_bus_query_id_type(irpstack->Parameters.QueryId.IdType));
		break;
	}

	return irp_done(irp, status);
}
