#include "usb_util.h"

#include "usbip_proto.h"

USHORT
get_usb_speed(USHORT bcdUSB)
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
	case 0x0300:
		return USB_SPEED_SUPER;
	case 0x0310:
		return USB_SPEED_SUPER_PLUS;
	case 0x0320:
		return USB_SPEED_SUPER_PLUS;
	default:
		return USB_SPEED_LOW;
	}
}
