#include "vhci_driver.h"

#include "vhci_urbr_store_control.tmh"

#include "usbip_proto.h"
#include "vhci_urbr.h"

#include "strutil.h"

NTSTATUS
store_urbr_control_transfer_partial(WDFREQUEST req_read, purb_req_t urbr)
{
	struct _URB_CONTROL_TRANSFER	*urb_ctltrans = &urbr->u.urb.urb->UrbControlTransfer;
	PVOID	dst;
	char	*buf;

	dst = get_data_from_req_read(req_read, urb_ctltrans->TransferBufferLength);
	if (dst == NULL)
		return STATUS_BUFFER_TOO_SMALL;

	/*
	 * reading from TransferBuffer or TransferBufferMDL,
	 * whichever of them is not null
	 */
	buf = get_buf(urb_ctltrans->TransferBuffer, urb_ctltrans->TransferBufferMDL);
	if (buf == NULL)
		return STATUS_INSUFFICIENT_RESOURCES;
	RtlCopyMemory(dst, buf, urb_ctltrans->TransferBufferLength);
	WdfRequestSetInformation(req_read, urb_ctltrans->TransferBufferLength);
	urbr->ep->vusb->len_sent_partial = 0;

	return STATUS_SUCCESS;
}

NTSTATUS
store_urbr_control_transfer_ex_partial(WDFREQUEST req_read, purb_req_t urbr)
{
	struct _URB_CONTROL_TRANSFER_EX	*urb_ctltrans_ex = &urbr->u.urb.urb->UrbControlTransferEx;
	PVOID	dst;
	char	*buf;

	dst = get_data_from_req_read(req_read, urb_ctltrans_ex->TransferBufferLength);
	if (dst == NULL)
		return STATUS_BUFFER_TOO_SMALL;

	/*
	 * reading from TransferBuffer or TransferBufferMDL,
	 * whichever of them is not null
	 */
	buf = get_buf(urb_ctltrans_ex->TransferBuffer, urb_ctltrans_ex->TransferBufferMDL);
	if (buf == NULL)
		return STATUS_INSUFFICIENT_RESOURCES;
	RtlCopyMemory(dst, buf, urb_ctltrans_ex->TransferBufferLength);
	WdfRequestSetInformation(req_read, urb_ctltrans_ex->TransferBufferLength);
	urbr->ep->vusb->len_sent_partial = 0;

	return STATUS_SUCCESS;
}

NTSTATUS
store_urbr_control_transfer(WDFREQUEST req_read, purb_req_t urbr)
{
	struct _URB_CONTROL_TRANSFER	*urb_ctltrans = &urbr->u.urb.urb->UrbControlTransfer;
	struct usbip_header	*hdr;
	int	in = IS_TRANSFER_FLAGS_IN(urb_ctltrans->TransferFlags);
	ULONG	nread = 0;
	NTSTATUS	status = STATUS_SUCCESS;

	hdr = get_hdr_from_req_read(req_read);
	if (hdr == NULL)
		return STATUS_BUFFER_TOO_SMALL;

	set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->ep->vusb->devid, in, urbr->ep,
		urb_ctltrans->TransferFlags | USBD_SHORT_TRANSFER_OK, urb_ctltrans->TransferBufferLength);
	RtlCopyMemory(hdr->u.cmd_submit.setup, urb_ctltrans->SetupPacket, 8);

	nread = sizeof(struct usbip_header);
	if (!in && urb_ctltrans->TransferBufferLength > 0) {
		if (get_read_payload_length(req_read) >= urb_ctltrans->TransferBufferLength) {
			PVOID	buf = get_buf(urb_ctltrans->TransferBuffer, urb_ctltrans->TransferBufferMDL);
			if (buf == NULL) {
				status = STATUS_INSUFFICIENT_RESOURCES;
				goto out;
			}
			nread += urb_ctltrans->TransferBufferLength;
			RtlCopyMemory(hdr + 1, buf, urb_ctltrans->TransferBufferLength);
		}
		else {
			urbr->ep->vusb->len_sent_partial = sizeof(struct usbip_header);
		}
	}
out:
	WdfRequestSetInformation(req_read, nread);
	return status;
}

static BOOLEAN
is_serial_setup_pkt(UCHAR iSerial, PUCHAR setup)
{
	if (setup[0] != 0x80 || setup[1] != 0x06 || setup[3] != 0x03 || setup[2] != iSerial)
		return FALSE;
	return TRUE;
}

static NTSTATUS
fetch_done_urbr_control_transfer_ex(pctx_vusb_t vusb, struct _URB_CONTROL_TRANSFER_EX *urb_ctltrans_ex)
{
	PWCHAR	dsc_serial;
	size_t	len;
	NTSTATUS	status;

	len = libdrv_strlenW(vusb->wserial) * sizeof(WCHAR) + 2;
	//dsc_serial = ExAllocatePoolWithTag(PagedPool, len, VHCI_POOLTAG);
	dsc_serial = ExAllocatePool2(POOL_FLAG_PAGED, len, VHCI_POOLTAG);
	
	*(PUCHAR)dsc_serial = (UCHAR)len;
	((PUCHAR)dsc_serial)[1] = 0x03;
	RtlCopyMemory((PUCHAR)dsc_serial + 2, vusb->wserial, len - 2);

	if (urb_ctltrans_ex->TransferBufferLength < len)
		len = urb_ctltrans_ex->TransferBufferLength;
	status = copy_to_transfer_buffer(urb_ctltrans_ex->TransferBuffer, urb_ctltrans_ex->TransferBufferMDL,
		urb_ctltrans_ex->TransferBufferLength, dsc_serial, (int)len);
	ExFreePoolWithTag(dsc_serial, VHCI_POOLTAG);

	if (status == STATUS_SUCCESS) {
		urb_ctltrans_ex->TransferBufferLength = (ULONG)len;
		/* this status code lets urbr be completed without fetching */
		return STATUS_FLT_IO_COMPLETE;
	}
	return status;
}

NTSTATUS
store_urbr_control_transfer_ex(WDFREQUEST req_read, purb_req_t urbr)
{
	pctx_vusb_t	vusb = urbr->ep->vusb;
	struct _URB_CONTROL_TRANSFER_EX	*urb_ctltrans_ex = &urbr->u.urb.urb->UrbControlTransferEx;
	struct usbip_header	*hdr;
	int	in = IS_TRANSFER_FLAGS_IN(urb_ctltrans_ex->TransferFlags);
	ULONG	nread = 0;
	NTSTATUS	status = STATUS_SUCCESS;

	/*
	 * overwrite USB serial if applicable
	 * UDE vhub seems to request a serial string via URB_FUNCTION_CONTROL_TRANSFER_EX.
	 */
	if (vusb->iSerial > 0 && vusb->wserial && is_serial_setup_pkt(vusb->iSerial, urb_ctltrans_ex->SetupPacket)) {
		TRD(READ, "overwrite serial string: %S", vusb->wserial);
		return fetch_done_urbr_control_transfer_ex(vusb, urb_ctltrans_ex);
	}

	hdr = get_hdr_from_req_read(req_read);
	if (hdr == NULL)
		return STATUS_BUFFER_TOO_SMALL;

	set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->ep->vusb->devid, in, urbr->ep,
		urb_ctltrans_ex->TransferFlags, urb_ctltrans_ex->TransferBufferLength);
	RtlCopyMemory(hdr->u.cmd_submit.setup, urb_ctltrans_ex->SetupPacket, 8);

	nread = sizeof(struct usbip_header);
	if (!in && urb_ctltrans_ex->TransferBufferLength > 0) {
		if (get_read_payload_length(req_read) >= urb_ctltrans_ex->TransferBufferLength) {
			PVOID	buf = get_buf(urb_ctltrans_ex->TransferBuffer, urb_ctltrans_ex->TransferBufferMDL);
			if (buf == NULL) {
				status = STATUS_INSUFFICIENT_RESOURCES;
				goto out;
			}
			nread += urb_ctltrans_ex->TransferBufferLength;
			RtlCopyMemory(hdr + 1, buf, urb_ctltrans_ex->TransferBufferLength);
		}
		else {
			urbr->ep->vusb->len_sent_partial = sizeof(struct usbip_header);
		}
	}
out:
	WdfRequestSetInformation(req_read, nread);
	return status;
}