/* libusb-win32, Generic Windows USB Library
* Copyright (c) 2010 Travis Robinson <libusbdotnet@gmail.com>
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
#include "stub_irp.h"
#include "usbip_stub_api.h"
#include "usbip_proto.h"

#include "stub_usbd.h"
#include "stub_devconf.h"

static UCHAR
get_speed_from_bcdUSB(USHORT bcdUSB)
{
	switch (bcdUSB) {
	case 0x0100:
		return USB_SPEED_LOW;
	case 0x0110:
		return USB_SPEED_FULL;
	case 0x0200:
		return USB_SPEED_HIGH;
	case 0x0210:
		return USB_SPEED_HIGH;
	case 0x0250:
		return USB_SPEED_WIRELESS;
	case 0x0300:
		return USB_SPEED_SUPER;
	case 0x0310:
		return USB_SPEED_SUPER_PLUS;
	case 0x0320:
		return USB_SPEED_SUPER_PLUS;
	default:
		return USB_SPEED_UNKNOWN;
	}
}

static NTSTATUS
process_get_devinfo(usbip_stub_dev_t *devstub, IRP *irp)
{
	PIO_STACK_LOCATION	irpStack;
	ULONG	outlen;
	NTSTATUS	status = STATUS_SUCCESS;

	irpStack = IoGetCurrentIrpStackLocation(irp);

	outlen = irpStack->Parameters.DeviceIoControl.OutputBufferLength;
	irp->IoStatus.Information = 0;
	if (outlen < sizeof(ioctl_usbip_stub_devinfo_t))
		status = STATUS_INVALID_PARAMETER;
	else {
		USB_DEVICE_DESCRIPTOR	desc;

		if (get_usb_device_desc(devstub, &desc)) {
			ioctl_usbip_stub_devinfo_t	*devinfo;

			devinfo = (ioctl_usbip_stub_devinfo_t *)irp->AssociatedIrp.SystemBuffer;
			devinfo->vendor = desc.idVendor;
			devinfo->product = desc.idProduct;
			devinfo->speed = get_speed_from_bcdUSB(desc.bcdUSB);
			devinfo->class = desc.bDeviceClass;
			devinfo->subclass = desc.bDeviceSubClass;
			devinfo->protocol = desc.bDeviceProtocol;
			irp->IoStatus.Information = sizeof(ioctl_usbip_stub_devinfo_t);
		}
		else {
			status = STATUS_UNSUCCESSFUL;
		}
	}

	irp->IoStatus.Status = status;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return status;
}

static NTSTATUS
process_export(usbip_stub_dev_t *devstub, IRP *irp)
{
	UNREFERENCED_PARAMETER(devstub);

	DBGI(DBG_IOCTL, "exporting: %s\n", dbg_devstub(devstub));

	irp->IoStatus.Status = STATUS_SUCCESS;
	IoCompleteRequest(irp, IO_NO_INCREMENT);

	DBGI(DBG_IOCTL, "exported: %s\n", dbg_devstub(devstub));

	return STATUS_SUCCESS;
}

NTSTATUS
stub_dispatch_ioctl(usbip_stub_dev_t *devstub, IRP *irp)
{
	PIO_STACK_LOCATION	irpStack;
	ULONG			ioctl_code;

	irpStack = IoGetCurrentIrpStackLocation(irp);
	ioctl_code = irpStack->Parameters.DeviceIoControl.IoControlCode;

	DBGI(DBG_IOCTL, "dispatch_ioctl: code: %s\n", dbg_stub_ioctl_code(ioctl_code));

	switch (ioctl_code) {
	case IOCTL_USBIP_STUB_GET_DEVINFO:
		return process_get_devinfo(devstub, irp);
	case IOCTL_USBIP_STUB_EXPORT:
		return process_export(devstub, irp);
	default:
		return pass_irp_down(devstub, irp, NULL, NULL);
	}
}
