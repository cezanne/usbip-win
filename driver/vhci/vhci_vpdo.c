#include "vhci.h"

#include "vhci_dev.h"
#include "usbreq.h"
#include "usbip_proto.h"

PAGEABLE NTSTATUS
vpdo_select_config(pvpdo_dev_t vpdo, struct _URB_SELECT_CONFIGURATION *urb_selc)
{
	PUSB_CONFIGURATION_DESCRIPTOR	dsc_conf = urb_selc->ConfigurationDescriptor;
	PUSB_CONFIGURATION_DESCRIPTOR	dsc_conf_new = NULL;
	NTSTATUS	status;

	if (dsc_conf == NULL) {
		DBGI(DBG_VPDO, "going to unconfigured state\n");
		if (vpdo->dsc_conf != NULL) {
			ExFreePoolWithTag(vpdo->dsc_conf, USBIP_VHCI_POOL_TAG);
			vpdo->dsc_conf = NULL;
		}
		return STATUS_SUCCESS;
	}

	if (vpdo->dsc_conf == NULL || vpdo->dsc_conf->wTotalLength != dsc_conf->wTotalLength) {
		//dsc_conf_new = ExAllocatePoolWithTag(NonPagedPool, dsc_conf->wTotalLength, USBIP_VHCI_POOL_TAG);
		dsc_conf_new = ExAllocatePool2(POOL_FLAG_NON_PAGED, dsc_conf->wTotalLength, USBIP_VHCI_POOL_TAG);
		
		if (dsc_conf_new == NULL) {
			DBGE(DBG_WRITE, "failed to allocate configuration descriptor: out of memory\n");
			return STATUS_UNSUCCESSFUL;
		}
	}
	else {
		dsc_conf_new = NULL;
	}
	if (dsc_conf_new != NULL && vpdo->dsc_conf != NULL) {
		ExFreePoolWithTag(vpdo->dsc_conf, USBIP_VHCI_POOL_TAG);
		vpdo->dsc_conf = dsc_conf_new;
	}
	RtlCopyMemory(vpdo->dsc_conf, dsc_conf, dsc_conf->wTotalLength);

	status = setup_config(vpdo->dsc_conf, &urb_selc->Interface, (PUCHAR)urb_selc + urb_selc->Hdr.Length, vpdo->speed);
	if (NT_SUCCESS(status)) {
		/* assign meaningless value, handle value is not used */
		urb_selc->ConfigurationHandle = (USBD_CONFIGURATION_HANDLE)0x12345678;
	}

	return status;
}

PAGEABLE NTSTATUS
vpdo_select_interface(pvpdo_dev_t vpdo, PUSBD_INTERFACE_INFORMATION info_intf)
{
	NTSTATUS	status;

	if (vpdo->dsc_conf == NULL) {
		DBGW(DBG_WRITE, "failed to select interface: empty configuration descriptor\n");
		return STATUS_INVALID_DEVICE_REQUEST;
	}
	status = setup_intf(info_intf, vpdo->dsc_conf, vpdo->speed);
	if (NT_SUCCESS(status)) {
		vpdo->current_intf_num = info_intf->InterfaceNumber;
		vpdo->current_intf_alt = info_intf->AlternateSetting;
	}
	return status;
}

static PAGEABLE void
copy_pipe_info(USB_PIPE_INFO *pinfos, PUSB_CONFIGURATION_DESCRIPTOR dsc_conf, PUSB_INTERFACE_DESCRIPTOR dsc_intf)
{
	PVOID	start;
	int	i;

	for (i = 0, start = dsc_intf; i < dsc_intf->bNumEndpoints; i++) {
		PUSB_ENDPOINT_DESCRIPTOR	dsc_ep;

		dsc_ep = dsc_next_ep(dsc_conf, start);
		RtlCopyMemory(&pinfos[i].EndpointDescriptor, dsc_ep, sizeof(USB_ENDPOINT_DESCRIPTOR));
		pinfos[i].ScheduleOffset = 0;///TODO
		start = dsc_ep;
	}
}

PAGEABLE NTSTATUS
vpdo_get_nodeconn_info(pvpdo_dev_t vpdo, PUSB_NODE_CONNECTION_INFORMATION conninfo, PULONG poutlen)
{
	PUSB_INTERFACE_DESCRIPTOR	dsc_intf = NULL;
	ULONG	outlen;
	NTSTATUS	status = STATUS_INVALID_PARAMETER;

	conninfo->DeviceAddress = (USHORT)conninfo->ConnectionIndex;
	conninfo->NumberOfOpenPipes = 0;
	conninfo->DeviceIsHub = FALSE;

	if (vpdo == NULL) {
		conninfo->ConnectionStatus = NoDeviceConnected;
		conninfo->LowSpeed = FALSE;
		outlen = sizeof(USB_NODE_CONNECTION_INFORMATION);
		status = STATUS_SUCCESS;
	}
	else {
		if (vpdo->dsc_dev == NULL)
			return STATUS_INVALID_PARAMETER;

		conninfo->ConnectionStatus = DeviceConnected;

		RtlCopyMemory(&conninfo->DeviceDescriptor, vpdo->dsc_dev, sizeof(USB_DEVICE_DESCRIPTOR));

		if (vpdo->dsc_conf != NULL)
			conninfo->CurrentConfigurationValue = vpdo->dsc_conf->bConfigurationValue;
		conninfo->LowSpeed = (vpdo->speed == USB_SPEED_LOW || vpdo->speed == USB_SPEED_FULL) ? TRUE : FALSE;

		dsc_intf = dsc_find_intf(vpdo->dsc_conf, vpdo->current_intf_num, vpdo->current_intf_alt);
		if (dsc_intf != NULL)
			conninfo->NumberOfOpenPipes = dsc_intf->bNumEndpoints;

		outlen = sizeof(USB_NODE_CONNECTION_INFORMATION) + sizeof(USB_PIPE_INFO) * conninfo->NumberOfOpenPipes;
		if (*poutlen < outlen) {
			status = STATUS_BUFFER_TOO_SMALL;
		}
		else {
			if (conninfo->NumberOfOpenPipes > 0)
				copy_pipe_info(conninfo->PipeList, vpdo->dsc_conf, dsc_intf);
			status = STATUS_SUCCESS;
		}
	}
	*poutlen = outlen;

	return status;
}

PAGEABLE NTSTATUS
vpdo_get_nodeconn_info_ex(pvpdo_dev_t vpdo, PUSB_NODE_CONNECTION_INFORMATION_EX conninfo, PULONG poutlen)
{
	PUSB_INTERFACE_DESCRIPTOR	dsc_intf = NULL;
	ULONG	outlen;
	NTSTATUS	status = STATUS_INVALID_PARAMETER;

	conninfo->DeviceAddress = (USHORT)conninfo->ConnectionIndex;
	conninfo->NumberOfOpenPipes = 0;
	conninfo->DeviceIsHub = FALSE;

	if (vpdo == NULL) {
		conninfo->ConnectionStatus = NoDeviceConnected;
		conninfo->Speed = UsbFullSpeed;
		outlen = sizeof(USB_NODE_CONNECTION_INFORMATION);
		status = STATUS_SUCCESS;
	}
	else {
		if (vpdo->dsc_dev == NULL)
			return STATUS_INVALID_PARAMETER;

		conninfo->ConnectionStatus = DeviceConnected;

		RtlCopyMemory(&conninfo->DeviceDescriptor, vpdo->dsc_dev, sizeof(USB_DEVICE_DESCRIPTOR));

		if (vpdo->dsc_conf != NULL)
			conninfo->CurrentConfigurationValue = vpdo->dsc_conf->bConfigurationValue;
		conninfo->Speed = vpdo->speed;

		dsc_intf = dsc_find_intf(vpdo->dsc_conf, vpdo->current_intf_num, vpdo->current_intf_alt);
		if (dsc_intf != NULL)
			conninfo->NumberOfOpenPipes = dsc_intf->bNumEndpoints;

		outlen = sizeof(USB_NODE_CONNECTION_INFORMATION) + sizeof(USB_PIPE_INFO) * conninfo->NumberOfOpenPipes;
		if (*poutlen < outlen) {
			status = STATUS_BUFFER_TOO_SMALL;
		}
		else {
			if (conninfo->NumberOfOpenPipes > 0)
				copy_pipe_info(conninfo->PipeList, vpdo->dsc_conf, dsc_intf);
			status = STATUS_SUCCESS;
		}
	}
	*poutlen = outlen;

	return status;
}

PAGEABLE NTSTATUS
vpdo_get_nodeconn_info_ex_v2(pvpdo_dev_t vpdo, PUSB_NODE_CONNECTION_INFORMATION_EX_V2 conninfo, PULONG poutlen)
{
	UNREFERENCED_PARAMETER(vpdo);

	conninfo->SupportedUsbProtocols.ul = 0;
	conninfo->SupportedUsbProtocols.Usb110 = TRUE;
	conninfo->SupportedUsbProtocols.Usb200 = TRUE;
	conninfo->Flags.ul = 0;
	conninfo->Flags.DeviceIsOperatingAtSuperSpeedOrHigher = FALSE;
	conninfo->Flags.DeviceIsSuperSpeedCapableOrHigher = FALSE;
	conninfo->Flags.DeviceIsOperatingAtSuperSpeedPlusOrHigher = FALSE;
	conninfo->Flags.DeviceIsSuperSpeedPlusCapableOrHigher = FALSE;

	*poutlen = sizeof(USB_NODE_CONNECTION_INFORMATION_EX_V2);

	return STATUS_SUCCESS;
}