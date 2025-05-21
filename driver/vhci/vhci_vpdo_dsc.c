#include "vhci.h"

#include "vhci_dev.h"
#include "usbreq.h"
#include "usbip_proto.h"
#include "vhci_irp.h"

static NTSTATUS
req_fetch_dsc(pvpdo_dev_t vpdo, PIRP irp)
{
	struct urb_req	*urbr;
	NTSTATUS	status;

	urbr = create_urbr(vpdo, irp, 0);
	if (urbr == NULL)
		status = STATUS_INSUFFICIENT_RESOURCES;
	else {
		status = submit_urbr(vpdo, urbr);
		if (NT_SUCCESS(status))
			return STATUS_PENDING;
		else {
			DBGI(DBG_GENERAL, "failed to submit unlink urb: %s\n", dbg_urbr(urbr));
			free_urbr(urbr);
			status = STATUS_UNSUCCESSFUL;
		}
	}
	return irp_done(irp, status);
}

PAGEABLE NTSTATUS
vpdo_get_dsc_from_nodeconn(pvpdo_dev_t vpdo, PIRP irp, PUSB_DESCRIPTOR_REQUEST dsc_req, PULONG psize)
{
	usb_cspkt_t	*csp = (usb_cspkt_t *)&dsc_req->SetupPacket;
	PVOID		dsc_data = NULL;
	ULONG		dsc_len = 0;
	NTSTATUS	status = STATUS_INVALID_PARAMETER;

	switch (csp->wValue.HiByte) {
	case USB_DEVICE_DESCRIPTOR_TYPE:
		dsc_data = vpdo->dsc_dev;
		if (dsc_data != NULL)
			dsc_len = sizeof(USB_DEVICE_DESCRIPTOR);
		break;
	case USB_CONFIGURATION_DESCRIPTOR_TYPE:
		dsc_data = vpdo->dsc_conf;
		if (dsc_data != NULL)
			dsc_len = vpdo->dsc_conf->wTotalLength;
		break;
	case USB_STRING_DESCRIPTOR_TYPE:
		status = req_fetch_dsc(vpdo, irp);
		break;
	default:
		DBGE(DBG_GENERAL, "unhandled descriptor type: %s\n", dbg_usb_descriptor_type(csp->wValue.HiByte));
		break;
	}

	if (dsc_data != NULL) {
		ULONG	outlen = sizeof(USB_DESCRIPTOR_REQUEST) + dsc_len;
		ULONG	ncopy = outlen;

		if (*psize < sizeof(USB_DESCRIPTOR_REQUEST)) {
			*psize = outlen;
			return STATUS_BUFFER_TOO_SMALL;
		}
		if (*psize < outlen) {
			ncopy = *psize - sizeof(USB_DESCRIPTOR_REQUEST);
		}
		status = STATUS_SUCCESS;
		if (ncopy > 0)
			RtlCopyMemory(dsc_req->Data, dsc_data, ncopy);
		if (ncopy == outlen)
			*psize = outlen;
	}

	return status;
}

/*
 * need to cache a descriptor?
 * Currently, device descriptor & full configuration descriptor are cached in vpdo.
 */
static BOOLEAN
need_caching_dsc(pvpdo_dev_t vpdo, struct _URB_CONTROL_DESCRIPTOR_REQUEST* urb_cdr, PUSB_COMMON_DESCRIPTOR dsc)
{
	switch (urb_cdr->DescriptorType) {
	case USB_DEVICE_DESCRIPTOR_TYPE:
		if (vpdo->dsc_dev != NULL)
			return FALSE;
		break;
	case USB_CONFIGURATION_DESCRIPTOR_TYPE:
		if (vpdo->dsc_conf == NULL) {
			PUSB_CONFIGURATION_DESCRIPTOR	dsc_conf = (PUSB_CONFIGURATION_DESCRIPTOR)dsc;
			if (dsc_conf->wTotalLength != urb_cdr->TransferBufferLength) {
				DBGI(DBG_WRITE, "ignore non-full configuration descriptor\n");
				return FALSE;
			}
		}
		else
			return FALSE;
		break;
	case USB_STRING_DESCRIPTOR_TYPE:
		/* string descrptor will be fetched on demand */
		return FALSE;
	default:
		return FALSE;
	}
	return TRUE;
}

void
try_to_cache_descriptor(pvpdo_dev_t vpdo, struct _URB_CONTROL_DESCRIPTOR_REQUEST* urb_cdr, PUSB_COMMON_DESCRIPTOR dsc)
{
	PUSB_COMMON_DESCRIPTOR	dsc_new;

	if (!need_caching_dsc(vpdo, urb_cdr, dsc))
		return;

	//dsc_new = ExAllocatePoolWithTag(PagedPool, urb_cdr->TransferBufferLength, USBIP_VHCI_POOL_TAG);
	dsc_new = ExAllocatePool2(POOL_FLAG_PAGED, urb_cdr->TransferBufferLength, USBIP_VHCI_POOL_TAG);
	
	if (dsc_new == NULL) {
		DBGE(DBG_WRITE, "out of memory\n");
		return;
	}
	RtlCopyMemory(dsc_new, dsc, urb_cdr->TransferBufferLength);

	switch (urb_cdr->DescriptorType) {
	case USB_DEVICE_DESCRIPTOR_TYPE:
		vpdo->dsc_dev = (PUSB_DEVICE_DESCRIPTOR)dsc_new;
		break;
	case USB_CONFIGURATION_DESCRIPTOR_TYPE:
		vpdo->dsc_conf = (PUSB_CONFIGURATION_DESCRIPTOR)dsc_new;
		break;
	default:
		ExFreePoolWithTag(dsc_new, USBIP_VHCI_POOL_TAG);
		break;
	}
}
