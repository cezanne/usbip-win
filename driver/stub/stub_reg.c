#include "stub_driver.h"

#include "stub_dbg.h"

static char *
reg_get_property(PDEVICE_OBJECT pdo, int property)
{
	UNICODE_STRING	prop_uni;
	ANSI_STRING	prop_ansi;
	char	*prop;
	PWCHAR	buf;
	ULONG	len;
	NTSTATUS	status;

	if (pdo == NULL)
		return NULL;

	status = IoGetDeviceProperty(pdo, property, 0, NULL, &len);
	if (status != STATUS_BUFFER_TOO_SMALL) {
		DBGE(DBG_GENERAL, "reg_get_property: IoGetDeviceProperty failed to get size: status: %x\n", status);
		return NULL;
	}

	//buf = ExAllocatePoolWithTag(PagedPool, len + sizeof(WCHAR), USBIP_STUB_POOL_TAG);
	buf = ExAllocatePool2(POOL_FLAG_PAGED, len + sizeof(WCHAR), USBIP_STUB_POOL_TAG);
	
	if (buf == NULL) {
		DBGE(DBG_GENERAL, "reg_get_property: out of memory\n");
		return NULL;
	}

	status = IoGetDeviceProperty(pdo, property, len, buf, &len);
	if (NT_ERROR(status)) {
		DBGE(DBG_GENERAL, "reg_get_property: IoGetDeviceProperty failed: status: %x\n", status);
		ExFreePool(buf);
		return NULL;
	}

	buf[len / sizeof(WCHAR)] = L'\0';
	RtlInitUnicodeString(&prop_uni, buf);

	status = RtlUnicodeStringToAnsiString(&prop_ansi, &prop_uni, TRUE);
	ExFreePool(buf);

	if (NT_ERROR(status)) {
		DBGE(DBG_GENERAL, "reg_get_property: failed to convert unicode string: status: %x\n", status);
		return NULL;
	}

	//prop = ExAllocatePoolWithTag(PagedPool, prop_ansi.Length + 1, USBIP_STUB_POOL_TAG);
	prop = ExAllocatePool2(POOL_FLAG_PAGED, prop_ansi.Length + 1, USBIP_STUB_POOL_TAG);

	
	if (prop == NULL) {
		DBGE(DBG_GENERAL, "reg_get_property: out of memory\n");
		RtlFreeAnsiString(&prop_ansi);
		return NULL;
	}

	RtlCopyMemory(prop, prop_ansi.Buffer, prop_ansi.Length);
	prop[prop_ansi.Length] = '\0';
	RtlFreeAnsiString(&prop_ansi);

	return prop;
}

BOOLEAN
reg_get_properties(usbip_stub_dev_t *devstub)
{
	UNREFERENCED_PARAMETER(devstub);
#if 0/////TODO
	HANDLE	hkey;
	PVOID keyObject = NULL;
	NTSTATUS status;
	UNICODE_STRING surprise_removal_ok_name;
	UNICODE_STRING initial_config_value_name;
	UNICODE_STRING device_interface_guids;
	UNICODE_STRING device_interface_guid_value;
	KEY_VALUE_FULL_INFORMATION *info;
	ULONG pool_length;
	ULONG length;
	ULONG val;
	LPWSTR valW;

	if (!devstub->pdo == NULL)
		return FALSE;

	/* default settings */
	devstub->surprise_removal_ok = FALSE;
	devstub->is_filter = TRUE;
	devstub->initial_config_value = SET_CONFIG_ACTIVE_CONFIG;

	status = IoOpenDeviceRegistryKey(devstub->pdo, PLUGPLAY_REGKEY_DEVICE, STANDARD_RIGHTS_ALL, &hkey);
	if (NT_FAILED(status)) {
		DBGW("reg_get_properties: IoOpenDeviceRegistryKey failed: status: %x\n", status);
		return FALSE;
	}

	RtlInitUnicodeString(&surprise_removal_ok_name, LIBUSB_REG_SURPRISE_REMOVAL_OK);
	RtlInitUnicodeString(&initial_config_value_name, LIBUSB_REG_INITIAL_CONFIG_VALUE);
	RtlInitUnicodeString(&device_interface_guids, LIBUSB_REG_DEVICE_INTERFACE_GUIDS);

	pool_length = sizeof(KEY_VALUE_FULL_INFORMATION) + 512;

	info = ExAllocatePool(NonPagedPool, pool_length);
	if (info == NULL) {
		ZwClose(hkey);
		USBERR("ExAllocatePool failed allocating %d bytes\n", pool_length);
		return FALSE;
	}

	// get surprise_removal_ok
	// get is_filter
	length = pool_length;
	memset(info, 0, length);

	status = ZwQueryValueKey(key, &surprise_removal_ok_name, KeyValueFullInformation, info, length, &length);

	if (NT_SUCCESS(status) && (info->Type == REG_DWORD)) {
		val = *((ULONG *)(((char *)info) + info->DataOffset));

		dev->surprise_removal_ok = val ? TRUE : FALSE;
		dev->is_filter = FALSE;
	}

	if (!dev->is_filter) {
		// get device interface guid
		length = pool_length;
		memset(info, 0, length);

		status = ZwQueryValueKey(key, &device_interface_guids, 
					 KeyValueFullInformation, info, length, &length);

		if (NT_SUCCESS(status) && (info->Type == REG_MULTI_SZ)) {
			valW = ((LPWSTR)(((char *)info) + info->DataOffset));
			RtlInitUnicodeString(&device_interface_guid_value,valW);
			status = RtlGUIDFromString(&device_interface_guid_value, &dev->device_interface_guid);
			if (NT_SUCCESS(status)) {
				if (IsEqualGUID(&dev->device_interface_guid, &LibusbKDeviceGuid)) {
					USBWRN0("libusbK default device DeviceInterfaceGUID found. skippng..\n");
					RtlInitUnicodeString(&device_interface_guid_value,Libusb0DeviceGuidW);
				} 
				else if (IsEqualGUID(&dev->device_interface_guid, &Libusb0FilterGuid)) {
					USBWRN0("libusb0 filter DeviceInterfaceGUID found. skippng..\n");
					RtlInitUnicodeString(&device_interface_guid_value,Libusb0DeviceGuidW);
				}
				else if (IsEqualGUID(&dev->device_interface_guid, &Libusb0DeviceGuid)) {
					USBWRN0("libusb0 device DeviceInterfaceGUID found. skippng..\n");
					RtlInitUnicodeString(&device_interface_guid_value,Libusb0DeviceGuidW);
				}
				else {
					USBMSG0("found user specified device interface guid.\n");
					dev->device_interface_in_use = TRUE;
				}
			}
			else {
				USBERR0("invalid user specified device interface guid.");
			}
		}
		if (!dev->device_interface_in_use) {
			RtlInitUnicodeString(&device_interface_guid_value,Libusb0DeviceGuidW);
		}
	}
	else {
		RtlInitUnicodeString(&device_interface_guid_value,Libusb0FilterGuidW);
	}

	if (!dev->device_interface_in_use) {
		status = RtlGUIDFromString(&device_interface_guid_value, &dev->device_interface_guid);
		if (NT_SUCCESS(status)) {
			USBMSG0("using default device interface guid.\n");
			dev->device_interface_in_use = TRUE;
		}
		else {
			USBERR0("failed using default device interface guid.\n");
		}
	}

	// get initial_config_value
	length = pool_length;
	memset(info, 0, length);

	status = ZwQueryValueKey(key, &initial_config_value_name,
				 KeyValueFullInformation, info, length, &length);

	if (NT_SUCCESS(status) && (info->Type == REG_DWORD)) {
		val = *((ULONG *)(((char *)info) + info->DataOffset));
		dev->initial_config_value = (int)val;
	}

	status = ObReferenceObjectByHandle(key, KEY_READ, NULL, KernelMode, &keyObject, NULL);
	if (NT_SUCCESS(status)) {
		length = pool_length;
		memset(info, 0, length);
		status = ObQueryNameString(keyObject, (POBJECT_NAME_INFORMATION)info, length, &length);
		if (NT_SUCCESS(status)) {
			PWSTR nameW =((POBJECT_NAME_INFORMATION)info)->Name.Buffer;
			PSTR  nameA = dev->objname_plugplay_registry_key;

			val=0;
			while (nameW[val] && val < (length/2)) {
				*nameA=(char)nameW[val];
				nameA++;
				val++;
			}
			*nameA='\0';

			USBDBG("reg-key-name=%s\n",dev->objname_plugplay_registry_key);
		}
		else {
			USBERR("ObQueryNameString failed. status=%Xh\n",status);
		}

		ObDereferenceObject(keyObject);
	}
	else {
		USBERR("ObReferenceObjectByHandle failed. status=%Xh\n",status);
	}

	ZwClose(key);
	ExFreePool(info);
#endif
	return TRUE;
}

char *
reg_get_id_hw(PDEVICE_OBJECT pdo)
{
	return reg_get_property(pdo, DevicePropertyHardwareID);
}

char *
reg_get_id_compat(PDEVICE_OBJECT pdo)
{
	return reg_get_property(pdo, DevicePropertyCompatibleIDs);
}
