/* libusb-win32, Generic Windows USB Library
 * Copyright (c) 2002-2005 Stephan Meyer <ste_meyer@web.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "stub_driver.h"
#include "stub_dbg.h"
#include "stub_dev.h"
#include "stub_res.h"
#include "usbd_helper.h"

#include "stub_cspkt.h"

#include <usbdlib.h>

typedef void (*cb_urb_done_t)(usbip_stub_dev_t *devstub, NTSTATUS status, PURB purb, stub_res_t *sres);

typedef struct {
	PDEVICE_OBJECT	devobj;
	PURB	purb;
	IO_STATUS_BLOCK	io_status;
	cb_urb_done_t	cb_urb_done;
	stub_res_t	*sres;
} safe_completion_t;

static NTSTATUS
do_safe_completion(PDEVICE_OBJECT devobj, PIRP irp, PVOID ctx)
{
	safe_completion_t	*safe_completion = (safe_completion_t *)ctx;
	usbip_stub_dev_t	*devstub;

	UNREFERENCED_PARAMETER(devobj);

	DBGI(DBG_GENERAL, "do_safe_completion: status = %s\n", dbg_usbd_status(safe_completion->purb->UrbHeader.Status));

	devstub = (usbip_stub_dev_t *)safe_completion->devobj->DeviceExtension;
	del_pending_stub_res(devstub, safe_completion->sres);

	safe_completion->cb_urb_done(devstub, irp->IoStatus.Status, safe_completion->purb, safe_completion->sres);

	ExFreePoolWithTag(safe_completion, USBIP_STUB_POOL_TAG);
	IoFreeIrp(irp);

	return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
call_usbd_nb(usbip_stub_dev_t *devstub, PURB purb, cb_urb_done_t cb_urb_done, stub_res_t *sres)
{
	IRP *irp;
	IO_STACK_LOCATION	*irpstack;
	safe_completion_t	*safe_completion;
	NTSTATUS status;

	DBGI(DBG_GENERAL, "call_usbd_nb: enter\n");

	//safe_completion = (safe_completion_t *)ExAllocatePoolWithTag(NonPagedPool, sizeof(safe_completion_t), USBIP_STUB_POOL_TAG);
	safe_completion = (safe_completion_t*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(safe_completion_t), USBIP_STUB_POOL_TAG);
	
	if (safe_completion == NULL) {
		DBGE(DBG_GENERAL, "call_usbd_nb: out of memory: cannot allocate safe_completion\n");
		return STATUS_NO_MEMORY;
	}
	safe_completion->devobj = devstub->self;
	safe_completion->purb = purb;
	safe_completion->cb_urb_done = cb_urb_done;
	safe_completion->sres = sres;

	irp = IoAllocateIrp(devstub->self->StackSize + 1, FALSE);
	if (irp == NULL) {
		DBGE(DBG_GENERAL, "call_usbd_nb: IoAllocateIrp: out of memory\n");
		ExFreePoolWithTag(safe_completion, USBIP_STUB_POOL_TAG);
		return STATUS_NO_MEMORY;
	}

	irpstack = IoGetNextIrpStackLocation(irp);
	irpstack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
	irpstack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
	irpstack->Parameters.Others.Argument1 = purb;
	irpstack->Parameters.Others.Argument2 = NULL;
	irpstack->DeviceObject = devstub->self;

	IoSetCompletionRoutine(irp, do_safe_completion, safe_completion, TRUE, TRUE, TRUE);

	add_pending_stub_res(devstub, sres, irp);
	DBGI(DBG_GENERAL, "call_usbd_nb: call_usbd_nb: %s\n", dbg_stub_res(sres, devstub));
	status = IoCallDriver(devstub->next_stack_dev, irp);
	DBGI(DBG_GENERAL, "call_usbd_nb: status = %s\n", dbg_ntstatus(status));

	/* Completion routine will treat remaining works depending on success or failure.
	 * Just return success code so a caller doesn't have to take any action such as releasing sres.  
	 */
	status = STATUS_SUCCESS;
	return status;
}

static NTSTATUS
call_usbd(usbip_stub_dev_t *devstub, PURB purb)
{
	KEVENT	event;
	IRP *irp;
	IO_STACK_LOCATION	*irpstack;
	IO_STATUS_BLOCK		io_status;
	NTSTATUS status;

	DBGI(DBG_GENERAL, "call_usbd: enter\n");

	KeInitializeEvent(&event, NotificationEvent, FALSE);
	irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB, devstub->next_stack_dev, NULL, 0, NULL, 0, TRUE, &event, &io_status);
	if (irp == NULL) {
		DBGE(DBG_GENERAL, "IoBuildDeviceIoControlRequest failed\n");
		return STATUS_NO_MEMORY;
	}

	irpstack = IoGetNextIrpStackLocation(irp);
	irpstack->Parameters.Others.Argument1 = purb;
	irpstack->Parameters.Others.Argument2 = NULL;

	status = IoCallDriver(devstub->next_stack_dev, irp);
	if (status == STATUS_PENDING) {
		KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
		status = io_status.Status;
	}

	DBGI(DBG_GENERAL, "call_usbd: status = %s, usbd_status:%s\n", dbg_ntstatus(status), dbg_usbd_status(purb->UrbHeader.Status));
	return status;
}

BOOLEAN
get_usb_status(usbip_stub_dev_t *devstub, USHORT op, USHORT idx, PVOID buf, PUCHAR plen)
{
	URB		Urb;
	NTSTATUS	status;

	UsbBuildGetStatusRequest(&Urb, op, idx, buf, NULL, NULL);
	status = call_usbd(devstub, &Urb);
	if (NT_SUCCESS(status)) {
		*plen = (UCHAR)Urb.UrbControlGetStatusRequest.TransferBufferLength;
		return TRUE;
	}
	return FALSE;
}

BOOLEAN
get_usb_desc(usbip_stub_dev_t *devstub, UCHAR descType, UCHAR idx, USHORT idLang, PVOID buff, ULONG *pbufflen)
{
	URB		Urb;
	NTSTATUS	status;

	UsbBuildGetDescriptorRequest(&Urb, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST), descType, idx, idLang, buff, NULL, *pbufflen, NULL);
	status = call_usbd(devstub, &Urb);
	if (NT_SUCCESS(status)) {
		*pbufflen = Urb.UrbControlDescriptorRequest.TransferBufferLength;
		return TRUE;
	}
	return FALSE;
}

BOOLEAN
get_usb_device_desc(usbip_stub_dev_t* devstub, PUSB_DEVICE_DESCRIPTOR pdesc)
{
	ULONG	len = sizeof(USB_DEVICE_DESCRIPTOR);
	return get_usb_desc(devstub, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, pdesc, &len);
}

static INT
find_usb_dsc_conf(usbip_stub_dev_t *devstub, UCHAR bVal, PUSB_CONFIGURATION_DESCRIPTOR dsc_conf)
{
	USB_DEVICE_DESCRIPTOR	DevDesc;
	UCHAR		i;

	if (!get_usb_device_desc(devstub, &DevDesc)) {
		return -1;
	}

	for (i = 0; i < DevDesc.bNumConfigurations; i++) {
		ULONG	len = sizeof(USB_CONFIGURATION_DESCRIPTOR);
		if (get_usb_desc(devstub, USB_CONFIGURATION_DESCRIPTOR_TYPE, i, 0, dsc_conf, &len)) {
			if (dsc_conf->bConfigurationValue == bVal)
				return i;
		}
	}
	return -1;
}

PUSB_CONFIGURATION_DESCRIPTOR
get_usb_dsc_conf(usbip_stub_dev_t *devstub, UCHAR bVal)
{
	USB_CONFIGURATION_DESCRIPTOR	ConfDesc;
	PUSB_CONFIGURATION_DESCRIPTOR	dsc_conf;
	ULONG	len;
	INT   iConfiguration;
	
	iConfiguration = find_usb_dsc_conf(devstub, bVal, &ConfDesc);
	if (iConfiguration == -1)
		return NULL;

	//dsc_conf = ExAllocatePoolWithTag(NonPagedPool, ConfDesc.wTotalLength, USBIP_STUB_POOL_TAG);
	dsc_conf = ExAllocatePool2(POOL_FLAG_NON_PAGED, ConfDesc.wTotalLength, USBIP_STUB_POOL_TAG);

	
	if (dsc_conf == NULL)
		return NULL;

	len = ConfDesc.wTotalLength;
	if (!get_usb_desc(devstub, USB_CONFIGURATION_DESCRIPTOR_TYPE, (UCHAR)iConfiguration, 0, dsc_conf, &len)) {
		ExFreePoolWithTag(dsc_conf, USBIP_STUB_POOL_TAG);
		return NULL;
	}
	return dsc_conf;
}

static PUSBD_INTERFACE_LIST_ENTRY
build_default_intf_list(PUSB_CONFIGURATION_DESCRIPTOR dsc_conf)
{
	PUSBD_INTERFACE_LIST_ENTRY	pintf_list;
	int	size;
	unsigned	i;

	size = sizeof(USBD_INTERFACE_LIST_ENTRY) * (dsc_conf->bNumInterfaces + 1);
	//pintf_list = ExAllocatePoolWithTag(NonPagedPool, size, USBIP_STUB_POOL_TAG);
	pintf_list = ExAllocatePool2(POOL_FLAG_NON_PAGED, size, USBIP_STUB_POOL_TAG);
	
	if (pintf_list == NULL)
		return NULL;

	RtlZeroMemory(pintf_list, size);

	for (i = 0; i < dsc_conf->bNumInterfaces; i++) {
		PUSB_INTERFACE_DESCRIPTOR	dsc_intf;
		dsc_intf = dsc_find_intf(dsc_conf, (UCHAR)i, 0);
		if (dsc_intf == NULL)
			break;
		pintf_list[i].InterfaceDescriptor = dsc_intf;
	}
	return pintf_list;
}

BOOLEAN
select_usb_conf(usbip_stub_dev_t *devstub, USHORT bVal)
{
	PUSB_CONFIGURATION_DESCRIPTOR	dsc_conf;
	PURB		purb;
	PUSBD_INTERFACE_LIST_ENTRY	pintf_list;
	NTSTATUS	status;
	struct _URB_SELECT_CONFIGURATION	*purb_selc;
	
	dsc_conf = get_usb_dsc_conf(devstub, (UCHAR)bVal);
	if (dsc_conf == NULL) {
		DBGE(DBG_GENERAL, "select_usb_conf: non-existent configuration descriptor: index: %hu\n", bVal);
		return FALSE;
	}

	pintf_list = build_default_intf_list(dsc_conf);
	if (pintf_list == NULL) {
		DBGE(DBG_GENERAL, "select_usb_conf: out of memory: pintf_list\n");
		ExFreePoolWithTag(dsc_conf, USBIP_STUB_POOL_TAG);
		return FALSE;
	}

	status = USBD_SelectConfigUrbAllocateAndBuild(devstub->hUSBD, dsc_conf, pintf_list, &purb);
	if (NT_ERROR(status)) {
		DBGE(DBG_GENERAL, "select_usb_conf: failed to selectConfigUrb: %s\n", dbg_ntstatus(status));
		ExFreePoolWithTag(pintf_list, USBIP_STUB_POOL_TAG);
		ExFreePoolWithTag(dsc_conf, USBIP_STUB_POOL_TAG);
		return FALSE;
	}

	status = call_usbd(devstub, purb);
	if (NT_ERROR(status)) {
		DBGI(DBG_GENERAL, "select_usb_conf: failed to select configuration: %s\n", dbg_devstub(devstub));
		USBD_UrbFree(devstub->hUSBD, purb);
		ExFreePoolWithTag(pintf_list, USBIP_STUB_POOL_TAG);
		ExFreePoolWithTag(dsc_conf, USBIP_STUB_POOL_TAG);
		return FALSE;
	}

	purb_selc = &purb->UrbSelectConfiguration;

	if (devstub->devconf) {
		free_devconf(devstub->devconf);
	}
	devstub->devconf = create_devconf(purb_selc->ConfigurationDescriptor, purb_selc->ConfigurationHandle, pintf_list);
	USBD_UrbFree(devstub->hUSBD, purb);
	ExFreePoolWithTag(pintf_list, USBIP_STUB_POOL_TAG);
	ExFreePoolWithTag(dsc_conf, USBIP_STUB_POOL_TAG);
	return TRUE;
}

BOOLEAN
select_usb_intf(usbip_stub_dev_t *devstub, UCHAR intf_num, USHORT alt_setting)
{
	PURB	purb;
	struct _URB_SELECT_INTERFACE	*purb_seli;
	USHORT	info_intf_size;
	ULONG	len_urb;
	NTSTATUS	status;

	if (devstub->devconf == NULL) {
		DBGW(DBG_GENERAL, "select_usb_intf: empty devconf: num: %hhu, alt:%hu\n", intf_num, alt_setting);
		return FALSE;
	}

	PUSB_INTERFACE_DESCRIPTOR	dsc_intf = dsc_find_intf(devstub->devconf->dsc_conf, intf_num, alt_setting);
	if (dsc_intf == NULL) {
		DBGW(DBG_GENERAL, "select_usb_intf: empty dsc_intf: num: %hhu, alt:%hu\n", intf_num, alt_setting);
		return FALSE;
	}

	info_intf_size = get_info_intf_size(devstub->devconf, intf_num, alt_setting);
	if (info_intf_size == 0) {
		DBGW(DBG_GENERAL, "select_usb_intf: non-existent interface: num: %hhu, alt:%hu\n", intf_num, alt_setting);
		return FALSE;
	}

	len_urb = sizeof(struct _URB_SELECT_INTERFACE) - sizeof(USBD_INTERFACE_INFORMATION) + info_intf_size;
	//purb = (PURB)ExAllocatePoolWithTag(NonPagedPool, len_urb, USBIP_STUB_POOL_TAG);
	purb = (PURB)ExAllocatePool2(POOL_FLAG_NON_PAGED, len_urb, USBIP_STUB_POOL_TAG);

	
	if (purb == NULL) {
		DBGE(DBG_GENERAL, "select_usb_intf: out of memory\n");
		return FALSE;
	}
	UsbBuildSelectInterfaceRequest(purb, (USHORT)len_urb, devstub->devconf->hConf, intf_num, (UCHAR)alt_setting);
	purb_seli = &purb->UrbSelectInterface;
	memset(&purb_seli->Interface.Pipes, 0, sizeof(USBD_PIPE_INFORMATION)*dsc_intf->bNumEndpoints);

	purb_seli->Interface.Class = dsc_intf->bInterfaceClass;
	purb_seli->Interface.SubClass = dsc_intf->bInterfaceSubClass;
	purb_seli->Interface.Protocol = dsc_intf->bInterfaceProtocol;
	purb_seli->Interface.NumberOfPipes = dsc_intf->bNumEndpoints;

	status = call_usbd(devstub, purb);
	ExFreePoolWithTag(purb, USBIP_STUB_POOL_TAG);
	if (NT_SUCCESS(status)) {
		update_devconf(devstub->devconf, &purb_seli->Interface);
		return TRUE;
	}
	return FALSE;
}

BOOLEAN
reset_pipe(usbip_stub_dev_t *devstub, USBD_PIPE_HANDLE hPipe)
{
	URB	urb;
	NTSTATUS	status;

	urb.UrbHeader.Function = URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL;
	urb.UrbHeader.Length = sizeof(struct _URB_PIPE_REQUEST);
	urb.UrbPipeRequest.PipeHandle = hPipe;

	status = call_usbd(devstub, &urb);
	if (NT_SUCCESS(status))
		return TRUE;
	return FALSE;
}

int
set_feature(usbip_stub_dev_t *devstub, USHORT func, USHORT feature, USHORT index)
{
	URB	urb;
	NTSTATUS	status;

	urb.UrbHeader.Function = func;
	urb.UrbHeader.Length = sizeof(struct _URB_CONTROL_FEATURE_REQUEST);
	urb.UrbControlFeatureRequest.FeatureSelector = feature;
	urb.UrbControlFeatureRequest.Index = index;
	/* should be NULL. If not, usbd returns STATUS_INVALID_PARAMETER */
	urb.UrbControlFeatureRequest.UrbLink = NULL;
	status = call_usbd(devstub, &urb);
	if (NT_SUCCESS(status))
		return 0;
	/*
	 * TODO: Only applied to this routine beause it's unclear that the status is
	 * unsuccessful when a device is stalled.
	 */
	if (status == STATUS_UNSUCCESSFUL && urb.UrbHeader.Status == USBD_STATUS_STALL_PID)
		return to_usbip_status(urb.UrbHeader.Status);
	return -1;
}

int
submit_class_vendor_req(usbip_stub_dev_t *devstub, BOOLEAN is_in, USHORT cmd, UCHAR reservedBits, UCHAR request, USHORT value, USHORT index, PVOID data, PULONG plen)
{
	URB		Urb;
	ULONG		flags = 0;
	NTSTATUS	status;

	if (is_in)
		flags |= USBD_TRANSFER_DIRECTION_IN;
	UsbBuildVendorRequest(&Urb, cmd, sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST), flags, reservedBits, request, value, index, data, NULL, *plen, NULL);
	status = call_usbd(devstub, &Urb);
	if (NT_SUCCESS(status)) {
		*plen = Urb.UrbControlVendorClassRequest.TransferBufferLength;
		return 0;
	}
	/*
	 * TODO: apply STALL error like as set_feature.
	 * Should be checked that this error might be better handled in call_usbd().
	 */
	if (status == STATUS_UNSUCCESSFUL && Urb.UrbHeader.Status == USBD_STATUS_STALL_PID)
		return to_usbip_status(Urb.UrbHeader.Status);
	return -1;
}

static void
done_bulk_intr_transfer(usbip_stub_dev_t *devstub, NTSTATUS status, PURB purb, stub_res_t *sres)
{
	DBGI(DBG_GENERAL, "done_bulk_intr_transfer: sres:%s,status:%s,usbd_status:%s\n",
		dbg_stub_res(sres, devstub), dbg_ntstatus(status), dbg_usbd_status(purb->UrbHeader.Status));

	if (status == STATUS_CANCELLED) {
		/* cancelled. just drop it */
		free_stub_res(sres);
	}
	else {
		if (NT_SUCCESS(status)) {
			if (sres->data != NULL)
				sres->data_len = purb->UrbBulkOrInterruptTransfer.TransferBufferLength;
			sres->header.u.ret_submit.actual_length = purb->UrbBulkOrInterruptTransfer.TransferBufferLength;
		}
		else {
			sres->data_len = 0;
			sres->header.u.ret_submit.actual_length = 0;
			sres->header.u.ret_submit.status = to_usbip_status(purb->UrbHeader.Status);
		}
		reply_stub_req(devstub, sres);
	}
	ExFreePoolWithTag(purb, USBIP_STUB_POOL_TAG);
}

NTSTATUS
submit_bulk_intr_transfer(usbip_stub_dev_t *devstub, USBD_PIPE_HANDLE hPipe, unsigned long seqnum, PVOID data, ULONG datalen, BOOLEAN is_in)
{
	PURB		purb;
	ULONG		flags = USBD_SHORT_TRANSFER_OK;
	stub_res_t	*sres;

	//purb = ExAllocatePoolWithTag(NonPagedPool, sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER), USBIP_STUB_POOL_TAG);
	purb = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER), USBIP_STUB_POOL_TAG);

	
	if (purb == NULL) {
		DBGE(DBG_GENERAL, "submit_bulk_intr_transfer: out of memory: urb\n");
		return STATUS_NO_MEMORY;
	}
	if (is_in)
		flags |= USBD_TRANSFER_DIRECTION_IN;
	UsbBuildInterruptOrBulkTransferRequest(purb, sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER), hPipe, data, NULL, datalen, flags, NULL);
	/* actual data length will be set by when urb is completed */
	sres = create_stub_res(USBIP_RET_SUBMIT, seqnum, 0, is_in ? data: NULL, is_in ? datalen: 0, 0, FALSE);
	if (sres == NULL) {
		ExFreePoolWithTag(purb, USBIP_STUB_POOL_TAG);
		return STATUS_UNSUCCESSFUL;
	}
	return call_usbd_nb(devstub, purb, done_bulk_intr_transfer, sres);
}

static void
compact_usbd_iso_data(ULONG n_pkts, char *src, const USBD_ISO_PACKET_DESCRIPTOR* usbd_iso_descs)
{
	const USBD_ISO_PACKET_DESCRIPTOR	*usbd_iso_desc;
	char	*dst = src;
	ULONG	i;

	for (usbd_iso_desc = usbd_iso_descs, i = 0; i < n_pkts; usbd_iso_desc++, i++) {
		if (dst != src + usbd_iso_desc->Offset)
			RtlCopyMemory(dst, src + usbd_iso_desc->Offset, usbd_iso_desc->Length);
		dst += usbd_iso_desc->Length;
	}
}

static void
done_iso_transfer(usbip_stub_dev_t *devstub, NTSTATUS status, PURB purb, stub_res_t *sres)
{
	DBGI(DBG_GENERAL, "done_iso_transfer: sres:%s,status:%s,usbd_status:%s\n",
		dbg_stub_res(sres, devstub), dbg_ntstatus(status), dbg_usbd_status(purb->UrbHeader.Status));

	if (status == STATUS_CANCELLED) {
		/* cancelled. just drop it */
		free_stub_res(sres);
	}
	else {
		if (NT_SUCCESS(status)) {
			struct _URB_ISOCH_TRANSFER	*purb_iso = &purb->UrbIsochronousTransfer;
			struct usbip_iso_packet_descriptor	*iso_descs;
			int	actual_len, n_pkts, iso_descs_len;

			n_pkts = sres->header.u.ret_submit.number_of_packets;
			iso_descs_len = sizeof(struct usbip_iso_packet_descriptor) * n_pkts;

			if (sres->data != NULL) { /* direction IN case */
				/* if iso packets are not filled fully, packet data compaction and moving iso_descs are required. */
				actual_len = get_usbd_iso_descs_len(purb_iso->NumberOfPackets, purb_iso->IsoPacket);
				NT_ASSERT(actual_len <= sres->data_len);
				if (actual_len < sres->data_len) {
					/* usbip expects a length field in iso descriptor to be intact.
					 * Copying old isochronous descriptors maintain only length field.
					 * Other fields will be overwritten by to_iso_descs() routine.
					 */
					compact_usbd_iso_data(n_pkts, sres->data, purb_iso->IsoPacket);
					RtlCopyMemory((char *)sres->data + actual_len, (char *)sres->data + sres->data_len, iso_descs_len);
				}
			}
			else {
				//sres->data = ExAllocatePoolWithTag(NonPagedPool, (SIZE_T)sres->data_len, USBIP_STUB_POOL_TAG);
				sres->data = ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)sres->data_len, USBIP_STUB_POOL_TAG);

				
				if (sres->data == NULL) {
					DBGE(DBG_GENERAL, "done_iso_transfer: out of memory\n");
					sres->data_len = 0;
				}
				else {
					/* Copy old iso descriptors. */
					RtlCopyMemory(sres->data, (char *)purb_iso->TransferBuffer + sres->data_len, iso_descs_len);
				}
				sres->data_len = iso_descs_len;
				actual_len = 0;
				ExFreePoolWithTag(purb_iso->TransferBuffer, USBIP_STUB_POOL_TAG);
			}

			iso_descs = (struct usbip_iso_packet_descriptor *)((char *)sres->data + actual_len);
			to_iso_descs(n_pkts, iso_descs, purb_iso->IsoPacket, TRUE);
			sres->data_len = actual_len + iso_descs_len;
			sres->header.u.ret_submit.actual_length = actual_len;
			sres->header.u.ret_submit.start_frame = purb_iso->StartFrame;
			sres->header.u.ret_submit.error_count = purb_iso->ErrorCount;
		}
		else {
			sres->header.u.ret_submit.status = to_usbip_status(purb->UrbHeader.Status);
		}
		reply_stub_req(devstub, sres);
	}
	USBD_UrbFree(devstub->hUSBD, purb);
}

NTSTATUS
submit_iso_transfer(usbip_stub_dev_t *devstub, USBD_PIPE_HANDLE hPipe, unsigned long seqnum,
	ULONG usbd_flags, ULONG n_pkts, ULONG start_frame, struct usbip_iso_packet_descriptor *iso_descs, PVOID data, ULONG datalen)
{
	PURB	purb;
	struct _URB_ISOCH_TRANSFER	*purb_iso;
	stub_res_t	*sres;
	BOOLEAN		is_in;
	NTSTATUS	status;

	status = USBD_IsochUrbAllocate(devstub->hUSBD, n_pkts, &purb);
	if (NT_ERROR(status)) {
		DBGE(DBG_GENERAL, "submit_iso_transfer: out of memory: urb\n");
		return status;
	}

	purb_iso = &purb->UrbIsochronousTransfer;
	purb_iso->Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
	purb_iso->Hdr.Length = (USHORT)GET_ISO_URB_SIZE(n_pkts - 1);
	purb_iso->PipeHandle = hPipe;
	purb_iso->TransferFlags = usbd_flags;
	purb_iso->TransferBuffer = data;
	purb_iso->TransferBufferLength = datalen;
	purb_iso->NumberOfPackets = n_pkts;
	purb_iso->StartFrame = start_frame;
	to_usbd_iso_descs(n_pkts, purb_iso->IsoPacket, iso_descs, FALSE);

	is_in = usbd_flags & USBD_TRANSFER_DIRECTION_IN ? TRUE: FALSE;
	sres = create_stub_res(USBIP_RET_SUBMIT, seqnum, 0, is_in ? data: NULL, datalen, n_pkts, FALSE);
	if (sres == NULL) {
		USBD_UrbFree(devstub->hUSBD, purb);
		return STATUS_UNSUCCESSFUL;
	}
	return call_usbd_nb(devstub, purb, done_iso_transfer, sres);
}

BOOLEAN
submit_control_transfer(usbip_stub_dev_t *devstub, usb_cspkt_t *csp, PVOID data, PULONG pdata_len)
{
	struct _URB_CONTROL_TRANSFER	UrbControl;
	ULONG		flags = USBD_DEFAULT_PIPE_TRANSFER;
	NTSTATUS	status;

	if (CSPKT_DIRECTION(csp))
		flags |= USBD_TRANSFER_DIRECTION_IN;
	RtlZeroMemory(&UrbControl, sizeof(struct _URB_CONTROL_TRANSFER));
	UrbControl.Hdr.Function = URB_FUNCTION_CONTROL_TRANSFER;
	UrbControl.Hdr.Length = sizeof(struct _URB_CONTROL_TRANSFER);
	RtlCopyMemory(UrbControl.SetupPacket, csp, 8);
	UrbControl.TransferFlags = flags;
	UrbControl.TransferBuffer = data;
	UrbControl.TransferBufferLength = *pdata_len;

	status = call_usbd(devstub, (PURB)&UrbControl);
	if (NT_SUCCESS(status)) {
		*pdata_len = UrbControl.TransferBufferLength;
		return TRUE;
	}
	return FALSE;
}
