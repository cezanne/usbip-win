#ifndef WINDOWS_DEF_H
	#define WINDOWS_DEF_H 1
	//#include <windows.h>
#endif
#include <ntddk.h>

#include <ntstrsafe.h>

#include "strutil.h"

ULONG	libdrv_pooltag = 'dbil';

size_t
libdrv_strlenW(LPCWSTR cwstr)
{
	size_t	len;
	NTSTATUS	status;

	if (cwstr == NULL)
		return 0;
	status = RtlStringCchLengthW(cwstr, NTSTRSAFE_MAX_CCH, &len);
	if (NT_ERROR(status))
		return 0;
	return len;
}

LPWSTR
libdrv_strdupW(LPCWSTR cwstr)
{
	PWCHAR	wstr_duped;
	size_t	len;
	NTSTATUS	status;

	if (cwstr == NULL)
		return NULL;
	status = RtlStringCchLengthW(cwstr, NTSTRSAFE_MAX_CCH, &len);
	if (NT_ERROR(status))
		return NULL;
	//wstr_duped = ExAllocatePoolWithTag(PagedPool, (len + 1) * sizeof(WCHAR), libdrv_pooltag);
	wstr_duped = ExAllocatePool2(POOL_FLAG_PAGED, (len + 1) * sizeof(WCHAR), libdrv_pooltag);

	if (wstr_duped == NULL)
		return NULL;

	RtlStringCchPrintfW(wstr_duped, len + 1, cwstr);
	return wstr_duped;
}

int
libdrv_snprintf(char *buf, int size, const char *fmt, ...)
{
	va_list	arglist;
	size_t	len;
	NTSTATUS	status;

	va_start(arglist, fmt);
	status = RtlStringCchVPrintfA(buf, size, fmt, arglist);
	va_end(arglist);

	if (NT_ERROR(status))
		return 0;
	status = RtlStringCchLengthA(buf, size, &len);
	if (NT_ERROR(status))
		return 0;
	return (int)len;
}

int
libdrv_snprintfW(PWCHAR buf, int size, LPCWSTR fmt, ...)
{
	va_list	arglist;
	size_t	len;
	NTSTATUS	status;

	va_start(arglist, fmt);
	status = RtlStringCchVPrintfW(buf, size, fmt, arglist);
	va_end(arglist);

	if (NT_ERROR(status))
		return 0;
	status = RtlStringCchLengthW(buf, size, &len);
	if (NT_ERROR(status))
		return 0;
	return (int)len;
}

#define BUFMAX_ASPRINTF	128

int
libdrv_asprintfW(PWCHAR *pbuf, LPCWSTR fmt, ...)
{
	WCHAR	buf[BUFMAX_ASPRINTF];
	va_list	arglist;
	size_t	len;
	NTSTATUS	status;

	va_start(arglist, fmt);
	status = RtlStringCchVPrintfW(buf, BUFMAX_ASPRINTF, fmt, arglist);
	va_end(arglist);

	if (NT_ERROR(status))
		return 0;
	status = RtlStringCchLengthW(buf, BUFMAX_ASPRINTF, &len);
	if (NT_ERROR(status))
		return 0;
	*pbuf = libdrv_strdupW(buf);
	if (*pbuf == NULL)
		return 0;
	return (int)len;
}

VOID
libdrv_free(PVOID data)
{
	if (data)
		ExFreePoolWithTag(data, libdrv_pooltag);
}