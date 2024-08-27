#include "vhci_driver.h"
#include "vhci_plugin.tmh"

#include "strutil.h"

#include "usbip_proto.h"
#include "usbip_vhci_api.h"
#include "devconf.h"

extern VOID
setup_ep_callbacks(PUDECX_USB_DEVICE_STATE_CHANGE_CALLBACKS pcallbacks);

extern NTSTATUS
add_ep(pctx_vusb_t vusb, PUDECXUSBENDPOINT_INIT *pepinit, PUSB_ENDPOINT_DESCRIPTOR dscr_ep);

static void
setup_with_dsc_dev(pctx_vusb_t vusb, PUSB_DEVICE_DESCRIPTOR dsc_dev)
{
	if (dsc_dev) {
		vusb->id_vendor = dsc_dev->idVendor;
		vusb->id_product = dsc_dev->idProduct;
		vusb->dev_speed = get_usb_speed(dsc_dev->bcdUSB);
		vusb->iSerial = dsc_dev->iSerialNumber;
	}
	else {
		vusb->id_vendor = 0;
		vusb->id_product = 0;
		vusb->dev_speed = 0;
		vusb->iSerial = 0;
	}
}

static BOOLEAN
setup_with_dsc_conf(pctx_vusb_t vusb, PUSB_CONFIGURATION_DESCRIPTOR dsc_conf)
{
	vusb->dsc_conf = ExAllocatePoolWithTag(PagedPool, dsc_conf->wTotalLength, VHCI_POOLTAG);
	if (vusb->dsc_conf == NULL) {
		TRE(PLUGIN, "failed to allocate configuration descriptor");
		return FALSE;
	}

	RtlCopyMemory(vusb->dsc_conf, dsc_conf, dsc_conf->wTotalLength);
	if (dsc_conf->bNumInterfaces > 0) {
		int	i;

		vusb->intf_altsettings = (PSHORT)ExAllocatePoolWithTag(PagedPool, dsc_conf->bNumInterfaces * sizeof(SHORT), VHCI_POOLTAG);
		if (vusb->intf_altsettings == NULL) {
			TRE(PLUGIN, "failed to allocate alternative settings for interfaces");
			return FALSE;
		}

		for (i = 0; i < dsc_conf->bNumInterfaces; i++)
			vusb->intf_altsettings[i] = -1;
	}
	vusb->default_conf_value = dsc_conf->bConfigurationValue;

	return TRUE;
}

static BOOLEAN
setup_vusb(UDECXUSBDEVICE ude_usbdev, pvhci_pluginfo_t pluginfo)
{
	pctx_vusb_t	vusb = TO_VUSB(ude_usbdev);
	WDF_OBJECT_ATTRIBUTES       attrs, attrs_hmem;
	NTSTATUS	status;

	WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
	attrs.ParentObject = ude_usbdev;

	vusb->dsc_conf = NULL;
	vusb->intf_altsettings = NULL;
	vusb->wserial = NULL;

	status = WdfSpinLockCreate(&attrs, &vusb->spin_lock);
	if (NT_ERROR(status)) {
		TRE(PLUGIN, "failed to create wait lock: %!STATUS!", status);
		return FALSE;
	}

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs_hmem, urb_req_t);
	attrs_hmem.ParentObject = ude_usbdev;

	status = WdfLookasideListCreate(&attrs, sizeof(urb_req_t), NonPagedPool, &attrs_hmem, 0, &vusb->lookaside_urbr);
	if (NT_ERROR(status)) {
		TRE(PLUGIN, "failed to create urbr memory: %!STATUS!", status);
		return FALSE;
	}

	setup_with_dsc_dev(vusb, (PUSB_DEVICE_DESCRIPTOR)pluginfo->dscr_dev);

	if (!setup_with_dsc_conf(vusb, (PUSB_CONFIGURATION_DESCRIPTOR)pluginfo->dscr_conf)) {
		TRE(PLUGIN, "failed to setup usb with configuration descritor");
		return FALSE;
	}

	vusb->devid = pluginfo->devid;
	vusb->port = pluginfo->port;

	vusb->ude_usbdev = ude_usbdev;
	vusb->pending_req_read = NULL;
	vusb->urbr_sent_partial = NULL;
	vusb->len_sent_partial = 0;
	vusb->seq_num = 0;
	vusb->invalid = FALSE;
	vusb->refcnt = 0;

	if (vusb->iSerial > 0 && pluginfo->wserial[0] != L'\0')
		vusb->wserial = libdrv_strdupW(pluginfo->wserial);
	else
		vusb->wserial = NULL;

	InitializeListHead(&vusb->head_urbr);
	InitializeListHead(&vusb->head_urbr_pending);
	InitializeListHead(&vusb->head_urbr_sent);

	return TRUE;
}

static NTSTATUS
vusb_d0_entry(_In_ WDFDEVICE hdev, _In_ UDECXUSBDEVICE ude_usbdev)
{
	UNREFERENCED_PARAMETER(hdev);
	UNREFERENCED_PARAMETER(ude_usbdev);

	TRD(VUSB, "Enter");

	return STATUS_NOT_SUPPORTED;
}

static NTSTATUS
vusb_d0_exit(_In_ WDFDEVICE hdev, _In_ UDECXUSBDEVICE ude_usbdev, UDECX_USB_DEVICE_WAKE_SETTING setting)
{
	UNREFERENCED_PARAMETER(hdev);
	UNREFERENCED_PARAMETER(ude_usbdev);
	UNREFERENCED_PARAMETER(setting);

	TRD(VUSB, "Enter");

	return STATUS_NOT_SUPPORTED;
}

static NTSTATUS
vusb_set_function_suspend_and_wake(_In_ WDFDEVICE UdecxWdfDevice, _In_ UDECXUSBDEVICE UdecxUsbDevice,
	_In_ ULONG Interface, _In_ UDECX_USB_DEVICE_FUNCTION_POWER FunctionPower)
{
	UNREFERENCED_PARAMETER(UdecxWdfDevice);
	UNREFERENCED_PARAMETER(UdecxUsbDevice);
	UNREFERENCED_PARAMETER(Interface);
	UNREFERENCED_PARAMETER(FunctionPower);

	TRD(VUSB, "Enter");

	return STATUS_NOT_SUPPORTED;
}

static PUDECXUSBDEVICE_INIT
build_vusb_pdinit(pctx_vhci_t vhci, UDECX_ENDPOINT_TYPE eptype, UDECX_USB_DEVICE_SPEED speed)
{
	PUDECXUSBDEVICE_INIT	pdinit;
	UDECX_USB_DEVICE_STATE_CHANGE_CALLBACKS	callbacks;

	pdinit = UdecxUsbDeviceInitAllocate(vhci->hdev);

	UDECX_USB_DEVICE_CALLBACKS_INIT(&callbacks);

	setup_ep_callbacks(&callbacks);
	callbacks.EvtUsbDeviceLinkPowerEntry = vusb_d0_entry;
	callbacks.EvtUsbDeviceLinkPowerExit = vusb_d0_exit;
	callbacks.EvtUsbDeviceSetFunctionSuspendAndWake = vusb_set_function_suspend_and_wake;

	UdecxUsbDeviceInitSetStateChangeCallbacks(pdinit, &callbacks);
	UdecxUsbDeviceInitSetSpeed(pdinit, speed);

	UdecxUsbDeviceInitSetEndpointsType(pdinit, eptype);

	return pdinit;
}

static void
setup_descriptors(PUDECXUSBDEVICE_INIT pdinit, pvhci_pluginfo_t pluginfo)
{
	NTSTATUS	status;
	USHORT		conf_dscr_fullsize;

	status = UdecxUsbDeviceInitAddDescriptor(pdinit, pluginfo->dscr_dev, 18);
	if (NT_ERROR(status)) {
		TRW(PLUGIN, "failed to add a device descriptor to device init");
	}
	conf_dscr_fullsize = *((PUSHORT)pluginfo->dscr_conf + 1);
	status = UdecxUsbDeviceInitAddDescriptor(pdinit, pluginfo->dscr_conf, conf_dscr_fullsize);
	if (NT_ERROR(status)) {
		TRW(PLUGIN, "failed to add a configuration descriptor to device init");
	}
}

static VOID
vusb_cleanup(_In_ WDFOBJECT ude_usbdev)
{
	pctx_vusb_t vusb;

	TRD(VUSB, "Enter");

	vusb = TO_VUSB(ude_usbdev);
	if (vusb->dsc_conf != NULL)
		ExFreePoolWithTag(vusb->dsc_conf, VHCI_POOLTAG);
	if (vusb->intf_altsettings != NULL)
		ExFreePoolWithTag(vusb->intf_altsettings, VHCI_POOLTAG);
	libdrv_free(vusb->wserial);
}

static void
create_endpoints(UDECXUSBDEVICE ude_usbdev, pvhci_pluginfo_t pluginfo)
{
	pctx_vusb_t vusb;
	PUDECXUSBENDPOINT_INIT	epinit;
	PUSB_CONFIGURATION_DESCRIPTOR	dsc_conf = (PUSB_CONFIGURATION_DESCRIPTOR)pluginfo->dscr_conf;
	PUSB_ENDPOINT_DESCRIPTOR	dsc_ep;
	PVOID	start;

	vusb = TO_VUSB(ude_usbdev);
	vusb->ude_usbdev = ude_usbdev;
	epinit = UdecxUsbSimpleEndpointInitAllocate(ude_usbdev);

	TRD(VUSB, "Enter: epinit=0x%p", epinit);
	add_ep(vusb, &epinit, NULL);

	start = dsc_conf;
	while ((dsc_ep = dsc_next_ep(dsc_conf, start)) != NULL) {
		epinit = UdecxUsbSimpleEndpointInitAllocate(ude_usbdev);
		TRD(VUSB, "While: epinit=0x%p, dsc_ep->bEndpointAddress=0x%x",
			epinit, dsc_ep->bEndpointAddress);
		add_ep(vusb, &epinit, dsc_ep);
		start = dsc_ep;
	}
	TRD(VUSB, "Leave");
}

static UDECX_ENDPOINT_TYPE
get_eptype(pvhci_pluginfo_t pluginfo)
{
	PUSB_DEVICE_DESCRIPTOR	dsc_dev = (PUSB_DEVICE_DESCRIPTOR)pluginfo->dscr_dev;
	PUSB_CONFIGURATION_DESCRIPTOR	dsc_conf = (PUSB_CONFIGURATION_DESCRIPTOR)pluginfo->dscr_conf;

	if (dsc_dev->bNumConfigurations > 1 || dsc_conf->bNumInterfaces > 1)
		return UdecxEndpointTypeDynamic;
	if (dsc_conf_get_n_intfs(dsc_conf) > 1)
		return UdecxEndpointTypeDynamic;
	return UdecxEndpointTypeSimple;
}

static UDECX_USB_DEVICE_SPEED
get_device_speed(pvhci_pluginfo_t pluginfo)
{
	unsigned short	bcdUSB = *(unsigned short *)(pluginfo->dscr_dev + 2);

	switch (bcdUSB) {
	case 0x0100:
		return UdecxUsbLowSpeed;
	case 0x0110:
		return UdecxUsbFullSpeed;
	case 0x0200:
		return UdecxUsbHighSpeed;
	case 0x0210:
		return UdecxUsbHighSpeed;
	case 0x0300:
		return UdecxUsbSuperSpeed;
	case 0x0310:
		return UdecxUsbSuperSpeed;
	case 0x0320:
		return UdecxUsbSuperSpeed;
	default:
		TRE(PLUGIN, "unknown bcdUSB:%x", (ULONG)bcdUSB);
		return UdecxUsbLowSpeed;
	}
}

static char
get_free_port(pctx_vhci_t vhci, BOOLEAN is_usb30)
{
	ULONG	port_start = is_usb30 ? MAX_HUB_20PORTS: 0;
	ULONG	i;

	for (i = port_start; i != vhci->n_max_ports; i++) {
		pctx_vusb_t	vusb = vhci->vusbs[i];
		if (vusb == NULL)
			return (CHAR)i;
	}
	/* Never happen */
	return (CHAR)-1;
}

static pctx_vusb_t
vusb_plugin(pctx_vhci_t vhci, pvhci_pluginfo_t pluginfo)
{
	pctx_vusb_t	vusb;
	PUDECXUSBDEVICE_INIT	pdinit;
	UDECX_ENDPOINT_TYPE	eptype;
	UDECX_USB_DEVICE_SPEED	speed;
	UDECX_USB_DEVICE_PLUG_IN_OPTIONS	opts;
	UDECXUSBDEVICE	ude_usbdev;
	WDF_OBJECT_ATTRIBUTES       attrs;
	NTSTATUS	status;

	eptype = get_eptype(pluginfo);
	speed = get_device_speed(pluginfo);
	pdinit = build_vusb_pdinit(vhci, eptype, speed);
	setup_descriptors(pdinit, pluginfo);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, ctx_vusb_t);
	attrs.EvtCleanupCallback = vusb_cleanup;

	status = UdecxUsbDeviceCreate(&pdinit, &attrs, &ude_usbdev);
	if (NT_ERROR(status)) {
		TRE(PLUGIN, "failed to create usb device: %!STATUS!", status);
		UdecxUsbDeviceInitFree(pdinit);
		return NULL;
	}

	vusb = TO_VUSB(ude_usbdev);
	vusb->vhci = vhci;

	vusb->ep_default = NULL;
	vusb->is_simple_ep_alloc = (eptype == UdecxEndpointTypeSimple) ? TRUE : FALSE;

	UDECX_USB_DEVICE_PLUG_IN_OPTIONS_INIT(&opts);
	if (speed == UdecxUsbSuperSpeed)
		opts.Usb30PortNumber = pluginfo->port + 1;
	else
		opts.Usb20PortNumber = pluginfo->port + 1;

	if (!setup_vusb(ude_usbdev, pluginfo)) {
		WdfObjectDelete(ude_usbdev);
		return NULL;
	}

	if (vusb->is_simple_ep_alloc)
		create_endpoints(ude_usbdev, pluginfo);

	status = UdecxUsbDevicePlugIn(ude_usbdev, &opts);
	if (NT_ERROR(status)) {
		TRE(PLUGIN, "failed to plugin a new device %!STATUS!", status);
		WdfObjectDelete(ude_usbdev);
		return NULL;
	}

	return vusb;
}

#define IS_USB30_PLUGINFO(pluginfo)	((get_device_speed(pluginfo) == UdecxUsbSuperSpeed))

NTSTATUS
plugin_vusb(pctx_vhci_t vhci, WDFREQUEST req, pvhci_pluginfo_t pluginfo)
{
	pctx_vusb_t	vusb;
	NTSTATUS	status = STATUS_UNSUCCESSFUL;

	WdfSpinLockAcquire(vhci->spin_lock);

	if (vhci->n_used_ports == vhci->n_max_ports) {
		WdfSpinLockRelease(vhci->spin_lock);
		return STATUS_END_OF_FILE;
	}

	pluginfo->port = get_free_port(vhci, IS_USB30_PLUGINFO(pluginfo));
	/* assign a temporary non-null value indicating on-going vusb allocation */
	vhci->vusbs[pluginfo->port] = VUSB_CREATING;
	WdfSpinLockRelease(vhci->spin_lock);

	vusb = vusb_plugin(vhci, pluginfo);

	WdfSpinLockAcquire(vhci->spin_lock);
	if (vusb != NULL) {
		pctx_safe_vusb_t	svusb = TO_SAFE_VUSB_FROM_REQ(req);

		svusb->port = pluginfo->port;
		status = STATUS_SUCCESS;
	}
	vhci->vusbs[pluginfo->port] = vusb;
	vhci->n_used_ports++;
	WdfSpinLockRelease(vhci->spin_lock);

	if ((vusb != NULL) && (vusb->is_simple_ep_alloc)) {
		/* UDE framework ignores SELECT CONF & INTF for a simple type */
		submit_req_select(vusb->ep_default, NULL, TRUE, vusb->default_conf_value, 0, 0);
		submit_req_select(vusb->ep_default, NULL, FALSE, 0, 0, 0);
	}
	return status;
}
