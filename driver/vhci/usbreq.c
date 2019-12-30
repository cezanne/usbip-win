#include "vhci.h"

#include "usbip_proto.h"
#include "usbip_vhci_api.h"
#include "usbreq.h"

extern NTSTATUS
store_urbr(PIRP irp, struct urb_req *urbr);

#ifdef DBG

const char *
dbg_urbr(struct urb_req *urbr)
{
	static char	buf[128];


	if (urbr == NULL)
		return "[null]";
	dbg_snprintf(buf, 128, "[seq:%u]", urbr->seq_num);
	return buf;
}

#endif

void
build_setup_packet(usb_cspkt_t *csp, unsigned char direct_in, unsigned char type, unsigned char recip, unsigned char request)
{
	csp->bmRequestType.B = 0;
	csp->bmRequestType.Type = type;
	if (direct_in)
		csp->bmRequestType.Dir = BMREQUEST_DEVICE_TO_HOST;
	csp->bmRequestType.Recipient = recip;
	csp->bRequest = request;
}

struct urb_req *
find_sent_urbr(pusbip_vpdo_dev_t vpdo, struct usbip_header *hdr)
{
	KIRQL		oldirql;
	PLIST_ENTRY	le;

	KeAcquireSpinLock(&vpdo->lock_urbr, &oldirql);
	for (le = vpdo->head_urbr_sent.Flink; le != &vpdo->head_urbr_sent; le = le->Flink) {
		struct urb_req	*urbr;
		urbr = CONTAINING_RECORD(le, struct urb_req, list_state);
		if (urbr->seq_num == hdr->base.seqnum) {
			RemoveEntryListInit(&urbr->list_all);
			RemoveEntryListInit(&urbr->list_state);
			KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);
			return urbr;
		}
	}
	KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);

	return NULL;
}

struct urb_req *
find_pending_urbr(pusbip_vpdo_dev_t vpdo)
{
	struct urb_req	*urbr;

	if (IsListEmpty(&vpdo->head_urbr_pending))
		return NULL;

	urbr = CONTAINING_RECORD(vpdo->head_urbr_pending.Flink, struct urb_req, list_state);
	urbr->seq_num = ++(vpdo->seq_num);
	RemoveEntryListInit(&urbr->list_state);
	return urbr;
}

static struct urb_req *
find_urbr_with_irp(pusbip_vpdo_dev_t vpdo, PIRP irp)
{
	PLIST_ENTRY	le;

	for (le = vpdo->head_urbr.Flink; le != &vpdo->head_urbr; le = le->Flink) {
		struct urb_req	*urbr;

		urbr = CONTAINING_RECORD(le, struct urb_req, list_all);
		if (urbr->irp == irp)
			return urbr;
	}

	return NULL;
}

static void
submit_urbr_unlink(pusbip_vpdo_dev_t vpdo, unsigned long seq_num_unlink)
{
	struct urb_req	*urbr_unlink;

	urbr_unlink = create_urbr(vpdo, NULL, seq_num_unlink);
	if (urbr_unlink != NULL) {
		NTSTATUS	status = submit_urbr(vpdo, urbr_unlink);
		if (NT_ERROR(status)) {
			DBGI(DBG_GENERAL, "failed to submit unlink urb: %s\n", dbg_urbr(urbr_unlink));
			free_urbr(urbr_unlink);
		}
	}
}

void
remove_cancelled_urbr(pusbip_vpdo_dev_t vpdo, PIRP irp)
{
	struct urb_req	*urbr;
	KIRQL	oldirql = irp->CancelIrql;

	DBGI(DBG_GENERAL, "irp will be cancelled, KeAcquireSpinLockAtDpcLevel\n");

	KeAcquireSpinLockAtDpcLevel(&vpdo->lock_urbr);

	DBGI(DBG_GENERAL, "irp will be cancelled, KeAcquireSpinLockAtDpcLevel passed\n");

	urbr = find_urbr_with_irp(vpdo, irp);
	DBGI(DBG_GENERAL, "irp will be cancelled, find_urbr_with_irp passed\n");
	if (urbr != NULL) {
		RemoveEntryListInit(&urbr->list_state);
		RemoveEntryListInit(&urbr->list_all);
		if (vpdo->urbr_sent_partial == urbr) {
			vpdo->urbr_sent_partial = NULL;
			vpdo->len_sent_partial = 0;
		}
	}
	else {
		DBGW(DBG_URB, "no matching urbr\n");
	}

	KeReleaseSpinLockForDpc(&vpdo->lock_urbr, oldirql); // KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);

	DBGI(DBG_GENERAL, "irp will be cancelled, KeReleaseSpinLock passed\n");

	if (urbr != NULL) {
		submit_urbr_unlink(vpdo, urbr->seq_num);
		DBGI(DBG_GENERAL, "irp will be cancelled, submit_urbr_unlink passed\n");

		DBGI(DBG_GENERAL, "cancelled urb destroyed: %s\n", dbg_urbr(urbr));
		free_urbr(urbr);
	}
}

static void
cancel_urbr(PDEVICE_OBJECT devobj, PIRP irp)
{
	pusbip_vpdo_dev_t	vpdo;

	vpdo = (pusbip_vpdo_dev_t)devobj->DeviceExtension;
	DBGI(DBG_GENERAL, "irp will be cancelled: %p\n", irp);

	remove_cancelled_urbr(vpdo, irp);
//	DBGI(DBG_GENERAL, "irp will be cancelled, cancell routine proceeed: %p\n", irp);

	irp->IoStatus.Status = STATUS_CANCELLED;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	IoReleaseCancelSpinLock(irp->CancelIrql);
}

struct urb_req *
create_urbr(pusbip_vpdo_dev_t vpdo, PIRP irp, unsigned long seq_num_unlink)
{
	struct urb_req	*urbr;

	urbr = ExAllocateFromNPagedLookasideList(&g_lookaside);
	if (urbr == NULL) {
		DBGE(DBG_URB, "create_urbr: out of memory\n");
		return NULL;
	}
	RtlZeroMemory(urbr, sizeof(*urbr));
	urbr->vpdo = vpdo;
	urbr->irp = irp;
	urbr->seq_num_unlink = seq_num_unlink;
	InitializeListHead(&urbr->list_all);
	InitializeListHead(&urbr->list_state);
	return urbr;
}

void
free_urbr(struct urb_req *urbr)
{
	ASSERT(IsListEmpty(&urbr->list_all));
	ASSERT(IsListEmpty(&urbr->list_state));
	ExFreeToNPagedLookasideList(&g_lookaside, urbr);
}

NTSTATUS
submit_urbr(pusbip_vpdo_dev_t vpdo, struct urb_req *urbr)
{
	KIRQL	oldirql;
	PIRP	read_irp;
	NTSTATUS	status = STATUS_PENDING;

	KeAcquireSpinLock(&vpdo->lock_urbr, &oldirql);

	if (vpdo->urbr_sent_partial || vpdo->pending_read_irp == NULL) {
		if (urbr->irp != NULL) {
			IoSetCancelRoutine(urbr->irp, cancel_urbr);
			IoMarkIrpPending(urbr->irp);
		}
		InsertTailList(&vpdo->head_urbr_pending, &urbr->list_state);
		InsertTailList(&vpdo->head_urbr, &urbr->list_all);
		KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);

		DBGI(DBG_URB, "submit_urbr: urb pending\n");
		return STATUS_PENDING;
	}

	read_irp = vpdo->pending_read_irp;
	vpdo->urbr_sent_partial = urbr;

	urbr->seq_num = ++(vpdo->seq_num);

	KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);

	status = store_urbr(read_irp, urbr);

	KeAcquireSpinLock(&vpdo->lock_urbr, &oldirql);

	if (status == STATUS_SUCCESS) {
		if (urbr->irp != NULL) {
			IoSetCancelRoutine(urbr->irp, cancel_urbr);
			IoMarkIrpPending(urbr->irp);
		}
		if (vpdo->len_sent_partial == 0) {
			vpdo->urbr_sent_partial = NULL;
			InsertTailList(&vpdo->head_urbr_sent, &urbr->list_state);
		}

		InsertTailList(&vpdo->head_urbr, &urbr->list_all);
		vpdo->pending_read_irp = NULL;
		KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);

		read_irp->IoStatus.Status = STATUS_SUCCESS;
		IoCompleteRequest(read_irp, IO_NO_INCREMENT);
		status = STATUS_PENDING;
	}
	else {
		vpdo->urbr_sent_partial = NULL;
		KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);
		DBGI(DBG_URB, "store_urbr failed\n");
		status = STATUS_INVALID_PARAMETER;
	}
	DBGI(DBG_URB, "submit_urbr: urb requested: status:%s\n", dbg_ntstatus(status));
	return status;
}
