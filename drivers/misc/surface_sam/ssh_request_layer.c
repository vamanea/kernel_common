// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/error-injection.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <linux/surface_aggregator_module.h>

#include "ssh_packet_layer.h"
#include "ssh_request_layer.h"

#include "ssam_trace.h"


#define SSH_RTL_REQUEST_TIMEOUT			ms_to_ktime(3000)
#define SSH_RTL_REQUEST_TIMEOUT_RESOLUTION	ms_to_ktime(max(2000 / HZ, 50))

#define SSH_RTL_MAX_PENDING		3


#ifdef CONFIG_SURFACE_SAM_SSH_ERROR_INJECTION

/**
 * ssh_rtl_should_drop_response - error injection hook to drop request responses
 *
 * Useful to cause request transmission timeouts in the driver by dropping the
 * response to a request.
 */
static noinline bool ssh_rtl_should_drop_response(void)
{
	return false;
}
ALLOW_ERROR_INJECTION(ssh_rtl_should_drop_response, TRUE);

#else

static inline bool ssh_rtl_should_drop_response(void)
{
	return false;
}

#endif


static inline u16 ssh_request_get_rqid(struct ssh_request *rqst)
{
	return get_unaligned_le16(rqst->packet.data.ptr
				  + SSH_MSGOFFSET_COMMAND(rqid));
}

static inline u32 ssh_request_get_rqid_safe(struct ssh_request *rqst)
{
	if (!rqst->packet.data.ptr)
		return -1;

	return ssh_request_get_rqid(rqst);
}


static void ssh_rtl_queue_remove(struct ssh_request *rqst)
{
	struct ssh_rtl *rtl = ssh_request_rtl(rqst);
	bool remove;

	spin_lock(&rtl->queue.lock);

	remove = test_and_clear_bit(SSH_REQUEST_SF_QUEUED_BIT, &rqst->state);
	if (remove)
		list_del(&rqst->node);

	spin_unlock(&rtl->queue.lock);

	if (remove)
		ssh_request_put(rqst);
}

static bool ssh_rtl_queue_empty(struct ssh_rtl *rtl)
{
	bool empty;

	spin_lock(&rtl->queue.lock);
	empty = list_empty(&rtl->queue.head);
	spin_unlock(&rtl->queue.lock);

	return empty;
}


static void ssh_rtl_pending_remove(struct ssh_request *rqst)
{
	struct ssh_rtl *rtl = ssh_request_rtl(rqst);
	bool remove;

	spin_lock(&rtl->pending.lock);

	remove = test_and_clear_bit(SSH_REQUEST_SF_PENDING_BIT, &rqst->state);
	if (remove) {
		atomic_dec(&rtl->pending.count);
		list_del(&rqst->node);
	}

	spin_unlock(&rtl->pending.lock);

	if (remove)
		ssh_request_put(rqst);
}

static int ssh_rtl_tx_pending_push(struct ssh_request *rqst)
{
	struct ssh_rtl *rtl = ssh_request_rtl(rqst);

	spin_lock(&rtl->pending.lock);

	if (test_bit(SSH_REQUEST_SF_LOCKED_BIT, &rqst->state)) {
		spin_unlock(&rtl->pending.lock);
		return -EINVAL;
	}

	if (test_and_set_bit(SSH_REQUEST_SF_PENDING_BIT, &rqst->state)) {
		spin_unlock(&rtl->pending.lock);
		return -EALREADY;
	}

	atomic_inc(&rtl->pending.count);
	list_add_tail(&ssh_request_get(rqst)->node, &rtl->pending.head);

	spin_unlock(&rtl->pending.lock);
	return 0;
}


static void ssh_rtl_complete_with_status(struct ssh_request *rqst, int status)
{
	struct ssh_rtl *rtl = ssh_request_rtl(rqst);

	trace_ssam_request_complete(rqst, status);

	// rtl/ptl may not be set if we're cancelling before submitting
	rtl_dbg_cond(rtl, "rtl: completing request (rqid: 0x%04x, status: %d)\n",
		     ssh_request_get_rqid_safe(rqst), status);

	if (status && status != -ECANCELED)
		rtl_dbg_cond(rtl, "rtl: request error: %d\n", status);

	rqst->ops->complete(rqst, NULL, NULL, status);
}

static void ssh_rtl_complete_with_rsp(struct ssh_request *rqst,
				      const struct ssh_command *cmd,
				      const struct ssam_span *data)
{
	struct ssh_rtl *rtl = ssh_request_rtl(rqst);

	trace_ssam_request_complete(rqst, 0);

	rtl_dbg(rtl, "rtl: completing request with response (rqid: 0x%04x)\n",
		ssh_request_get_rqid(rqst));

	rqst->ops->complete(rqst, cmd, data, 0);
}


static bool ssh_rtl_tx_can_process(struct ssh_request *rqst)
{
	struct ssh_rtl *rtl = ssh_request_rtl(rqst);

	if (test_bit(SSH_REQUEST_TY_FLUSH_BIT, &rqst->state))
		return !atomic_read(&rtl->pending.count);

	return atomic_read(&rtl->pending.count) < SSH_RTL_MAX_PENDING;
}

static struct ssh_request *ssh_rtl_tx_next(struct ssh_rtl *rtl)
{
	struct ssh_request *rqst = ERR_PTR(-ENOENT);
	struct ssh_request *p, *n;

	spin_lock(&rtl->queue.lock);

	// find first non-locked request and remove it
	list_for_each_entry_safe(p, n, &rtl->queue.head, node) {
		if (unlikely(test_bit(SSH_REQUEST_SF_LOCKED_BIT, &p->state)))
			continue;

		if (!ssh_rtl_tx_can_process(p)) {
			rqst = ERR_PTR(-EBUSY);
			break;
		}

		// remove from queue and mark as transmitting
		set_bit(SSH_REQUEST_SF_TRANSMITTING_BIT, &p->state);
		// ensure state never gets zero
		smp_mb__before_atomic();
		clear_bit(SSH_REQUEST_SF_QUEUED_BIT, &p->state);

		list_del(&p->node);

		rqst = p;
		break;
	}

	spin_unlock(&rtl->queue.lock);
	return rqst;
}

static int ssh_rtl_tx_try_process_one(struct ssh_rtl *rtl)
{
	struct ssh_request *rqst;
	int status;

	// get and prepare next request for transmit
	rqst = ssh_rtl_tx_next(rtl);
	if (IS_ERR(rqst))
		return PTR_ERR(rqst);

	// add to/mark as pending
	status = ssh_rtl_tx_pending_push(rqst);
	if (status) {
		ssh_request_put(rqst);
		return -EAGAIN;
	}

	// submit packet
	status = ssh_ptl_submit(&rtl->ptl, &rqst->packet);
	if (status == -ESHUTDOWN) {
		/*
		 * Packet has been refused due to the packet layer shutting
		 * down. Complete it here.
		 */
		set_bit(SSH_REQUEST_SF_LOCKED_BIT, &rqst->state);
		/*
		 * Note: A barrier is not required here, as there are only two
		 * references in the system at this point: The one that we have,
		 * and the other one that belongs to the pending set. Due to the
		 * request being marked as "transmitting", our process is the
		 * only one allowed to remove the pending node and change the
		 * state. Normally, the task would fall to the packet callback,
		 * but as this is a path where submission failed, this callback
		 * will never be executed.
		 */

		ssh_rtl_pending_remove(rqst);
		ssh_rtl_complete_with_status(rqst, -ESHUTDOWN);

		ssh_request_put(rqst);
		return -ESHUTDOWN;

	} else if (status) {
		/*
		 * If submitting the packet failed and the packet layer isn't
		 * shutting down, the packet has either been submmitted/queued
		 * before (-EALREADY, which cannot happen as we have guaranteed
		 * that requests cannot be re-submitted), or the packet was
		 * marked as locked (-EINVAL). To mark the packet locked at this
		 * stage, the request, and thus the packets itself, had to have
		 * been canceled. Simply drop the reference. Cancellation itself
		 * will remove it from the set of pending requests.
		 */

		WARN_ON(status != -EINVAL);

		ssh_request_put(rqst);
		return -EAGAIN;
	}

	ssh_request_put(rqst);
	return 0;
}

static bool ssh_rtl_tx_schedule(struct ssh_rtl *rtl)
{
	if (atomic_read(&rtl->pending.count) >= SSH_RTL_MAX_PENDING)
		return false;

	if (ssh_rtl_queue_empty(rtl))
		return false;

	return schedule_work(&rtl->tx.work);
}

static void ssh_rtl_tx_work_fn(struct work_struct *work)
{
	struct ssh_rtl *rtl = to_ssh_rtl(work, tx.work);
	int i, status;

	/*
	 * Try to be nice and not block the workqueue: Run a maximum of 10
	 * tries, then re-submit if necessary. This should not be neccesary,
	 * for normal execution, but guarantee it anyway.
	 */
	for (i = 0; i < 10; i++) {
		status = ssh_rtl_tx_try_process_one(rtl);
		if (status == -ENOENT || status == -EBUSY)
			return;		// no more requests to process

		if (status == -ESHUTDOWN) {
			/*
			 * Packet system shutting down. No new packets can be
			 * transmitted. Return silently, the party initiating
			 * the shutdown should handle the rest.
			 */
			return;
		}

		WARN_ON(status != 0 && status != -EAGAIN);
	}

	// out of tries, reschedule
	ssh_rtl_tx_schedule(rtl);
}


int ssh_rtl_submit(struct ssh_rtl *rtl, struct ssh_request *rqst)
{
	trace_ssam_request_submit(rqst);

	/*
	 * Ensure that requests expecting a response are sequenced. If this
	 * invariant ever changes, see the comment in ssh_rtl_complete on what
	 * is required to be changed in the code.
	 */
	if (test_bit(SSH_REQUEST_TY_HAS_RESPONSE_BIT, &rqst->state))
		if (!test_bit(SSH_PACKET_TY_SEQUENCED_BIT, &rqst->packet.state))
			return -EINVAL;

	// try to set ptl and check if this request has already been submitted
	if (cmpxchg(&rqst->packet.ptl, NULL, &rtl->ptl) != NULL)
		return -EALREADY;

	spin_lock(&rtl->queue.lock);

	if (test_bit(SSH_RTL_SF_SHUTDOWN_BIT, &rtl->state)) {
		spin_unlock(&rtl->queue.lock);
		return -ESHUTDOWN;
	}

	if (test_bit(SSH_REQUEST_SF_LOCKED_BIT, &rqst->state)) {
		spin_unlock(&rtl->queue.lock);
		return -EINVAL;
	}

	set_bit(SSH_REQUEST_SF_QUEUED_BIT, &rqst->state);
	list_add_tail(&ssh_request_get(rqst)->node, &rtl->queue.head);

	spin_unlock(&rtl->queue.lock);

	ssh_rtl_tx_schedule(rtl);
	return 0;
}

static void ssh_rtl_timeout_reaper_mod(struct ssh_rtl *rtl, ktime_t now,
				       ktime_t expires)
{
	unsigned long delta = msecs_to_jiffies(ktime_ms_delta(expires, now));
	ktime_t aexp = ktime_add(expires, SSH_RTL_REQUEST_TIMEOUT_RESOLUTION);
	ktime_t old;

	// re-adjust / schedule reaper if it is above resolution delta
	old = READ_ONCE(rtl->rtx_timeout.expires);
	while (ktime_before(aexp, old))
		old = cmpxchg64(&rtl->rtx_timeout.expires, old, expires);

	// if we updated the reaper expiration, modify work timeout
	if (old == expires)
		mod_delayed_work(system_wq, &rtl->rtx_timeout.reaper, delta);
}

static void ssh_rtl_timeout_start(struct ssh_request *rqst)
{
	struct ssh_rtl *rtl = ssh_request_rtl(rqst);
	ktime_t timestamp = ktime_get_coarse_boottime();
	ktime_t timeout = rtl->rtx_timeout.timeout;

	if (test_bit(SSH_REQUEST_SF_LOCKED_BIT, &rqst->state))
		return;

	WRITE_ONCE(rqst->timestamp, timestamp);
	/*
	 * Ensure timestamp is set before starting the reaper. Paired with
	 * implicit barrier following check on ssh_request_get_expiration in
	 * ssh_rtl_timeout_reap.
	 */
	smp_mb__after_atomic();

	ssh_rtl_timeout_reaper_mod(rtl, timestamp, timestamp + timeout);
}


static void ssh_rtl_complete(struct ssh_rtl *rtl,
			     const struct ssh_command *command,
			     const struct ssam_span *command_data)
{
	struct ssh_request *r = NULL;
	struct ssh_request *p, *n;
	u16 rqid = get_unaligned_le16(&command->rqid);

	trace_ssam_rx_response_received(command, command_data->len);

	/*
	 * Get request from pending based on request ID and mark it as response
	 * received and locked.
	 */
	spin_lock(&rtl->pending.lock);
	list_for_each_entry_safe(p, n, &rtl->pending.head, node) {
		// we generally expect requests to be processed in order
		if (unlikely(ssh_request_get_rqid(p) != rqid))
			continue;

		// simulate response timeout
		if (ssh_rtl_should_drop_response()) {
			spin_unlock(&rtl->pending.lock);

			trace_ssam_ei_rx_drop_response(p);
			rtl_info(rtl, "request error injection: dropping response for request %p\n",
				 &p->packet);
			return;
		}

		/*
		 * Mark as "response received" and "locked" as we're going to
		 * complete it.
		 */
		set_bit(SSH_REQUEST_SF_LOCKED_BIT, &p->state);
		set_bit(SSH_REQUEST_SF_RSPRCVD_BIT, &p->state);
		// ensure state never gets zero
		smp_mb__before_atomic();
		clear_bit(SSH_REQUEST_SF_PENDING_BIT, &p->state);

		atomic_dec(&rtl->pending.count);
		list_del(&p->node);

		r = p;
		break;
	}
	spin_unlock(&rtl->pending.lock);

	if (!r) {
		rtl_warn(rtl, "rtl: dropping unexpected command message (rqid = 0x%04x)\n",
			 rqid);
		return;
	}

	// if the request hasn't been completed yet, we will do this now
	if (test_and_set_bit(SSH_REQUEST_SF_COMPLETED_BIT, &r->state)) {
		ssh_request_put(r);
		ssh_rtl_tx_schedule(rtl);
		return;
	}

	/*
	 * Make sure the request has been transmitted. In case of a sequenced
	 * request, we are guaranteed that the completion callback will run on
	 * the receiver thread directly when the ACK for the packet has been
	 * received. Similarly, this function is guaranteed to run on the
	 * receiver thread. Thus we are guaranteed that if the packet has been
	 * successfully transmitted and received an ACK, the transmitted flag
	 * has been set and is visible here.
	 *
	 * We are currently not handling unsequenced packets here, as those
	 * should never expect a response as ensured in ssh_rtl_submit. If this
	 * ever changes, one would have to test for
	 *
	 *	(r->state & (transmitting | transmitted))
	 *
	 * on unsequenced packets to determine if they could have been
	 * transmitted. There are no synchronization guarantees as in the
	 * sequenced case, since, in this case, the callback function will not
	 * run on the same thread. Thus an exact determination is impossible.
	 */
	if (!test_bit(SSH_REQUEST_SF_TRANSMITTED_BIT, &r->state)) {
		rtl_err(rtl, "rtl: received response before ACK for request (rqid = 0x%04x)\n",
			rqid);

		/*
		 * NB: Timeout has already been canceled, request already been
		 * removed from pending and marked as locked and completed. As
		 * we receive a "false" response, the packet might still be
		 * queued though.
		 */
		ssh_rtl_queue_remove(r);

		ssh_rtl_complete_with_status(r, -EREMOTEIO);
		ssh_request_put(r);

		ssh_rtl_tx_schedule(rtl);
		return;
	}

	/*
	 * NB: Timeout has already been canceled, request already been
	 * removed from pending and marked as locked and completed. The request
	 * can also not be queued any more, as it has been marked as
	 * transmitting and later transmitted. Thus no need to remove it from
	 * anywhere.
	 */

	ssh_rtl_complete_with_rsp(r, command, command_data);
	ssh_request_put(r);

	ssh_rtl_tx_schedule(rtl);
}


static bool ssh_rtl_cancel_nonpending(struct ssh_request *r)
{
	struct ssh_rtl *rtl;
	unsigned long state, fixed;
	bool remove;

	/*
	 * Handle unsubmitted request: Try to mark the packet as locked,
	 * expecting the state to be zero (i.e. unsubmitted). Note that, if
	 * setting the state worked, we might still be adding the packet to the
	 * queue in a currently executing submit call. In that case, however,
	 * ptl reference must have been set previously, as locked is checked
	 * after setting ptl. Thus only if we successfully lock this request and
	 * ptl is NULL, we have successfully removed the request.
	 * Otherwise we need to try and grab it from the queue.
	 *
	 * Note that if the CMPXCHG fails, we are guaranteed that ptl has
	 * been set and is non-NULL, as states can only be nonzero after this
	 * has been set. Also note that we need to fetch the static (type) flags
	 * to ensure that they don't cause the cmpxchg to fail.
	 */
	fixed = READ_ONCE(r->state) & SSH_REQUEST_FLAGS_TY_MASK;
	state = cmpxchg(&r->state, fixed, SSH_REQUEST_SF_LOCKED_BIT);
	if (!state && !READ_ONCE(r->packet.ptl)) {
		if (test_and_set_bit(SSH_REQUEST_SF_COMPLETED_BIT, &r->state))
			return true;

		ssh_rtl_complete_with_status(r, -ECANCELED);
		return true;
	}

	rtl = ssh_request_rtl(r);
	spin_lock(&rtl->queue.lock);

	/*
	 * Note: 1) Requests cannot be re-submitted. 2) If a request is queued,
	 * it cannot be "transmitting"/"pending" yet. Thus, if we successfully
	 * remove the the request here, we have removed all its occurences in
	 * the system.
	 */

	remove = test_and_clear_bit(SSH_REQUEST_SF_QUEUED_BIT, &r->state);
	if (!remove) {
		spin_unlock(&rtl->queue.lock);
		return false;
	}

	set_bit(SSH_REQUEST_SF_LOCKED_BIT, &r->state);
	list_del(&r->node);

	spin_unlock(&rtl->queue.lock);

	ssh_request_put(r);	// drop reference obtained from queue

	if (test_and_set_bit(SSH_REQUEST_SF_COMPLETED_BIT, &r->state))
		return true;

	ssh_rtl_complete_with_status(r, -ECANCELED);
	return true;
}

static bool ssh_rtl_cancel_pending(struct ssh_request *r)
{
	// if the packet is already locked, it's going to be removed shortly
	if (test_and_set_bit(SSH_REQUEST_SF_LOCKED_BIT, &r->state))
		return true;

	/*
	 * Now that we have locked the packet, we have guaranteed that it can't
	 * be added to the system any more. If rtl is zero, the locked
	 * check in ssh_rtl_submit has not been run and any submission,
	 * currently in progress or called later, won't add the packet. Thus we
	 * can directly complete it.
	 */
	if (!ssh_request_rtl(r)) {
		if (test_and_set_bit(SSH_REQUEST_SF_COMPLETED_BIT, &r->state))
			return true;

		ssh_rtl_complete_with_status(r, -ECANCELED);
		return true;
	}

	/*
	 * Try to cancel the packet. If the packet has not been completed yet,
	 * this will subsequently (and synchronously) call the completion
	 * callback of the packet, which will complete the request.
	 */
	ssh_ptl_cancel(&r->packet);

	/*
	 * If the packet has been completed with success, i.e. has not been
	 * canceled by the above call, the request may not have been completed
	 * yet (may be waiting for a response). Check if we need to do this
	 * here.
	 */
	if (test_and_set_bit(SSH_REQUEST_SF_COMPLETED_BIT, &r->state))
		return true;

	ssh_rtl_queue_remove(r);
	ssh_rtl_pending_remove(r);
	ssh_rtl_complete_with_status(r, -ECANCELED);

	return true;
}

bool ssh_rtl_cancel(struct ssh_request *rqst, bool pending)
{
	struct ssh_rtl *rtl;
	bool canceled;

	if (test_and_set_bit(SSH_REQUEST_SF_CANCELED_BIT, &rqst->state))
		return true;

	trace_ssam_request_cancel(rqst);

	if (pending)
		canceled = ssh_rtl_cancel_pending(rqst);
	else
		canceled = ssh_rtl_cancel_nonpending(rqst);

	// note: rtl may be NULL if request has not been submitted yet
	rtl = ssh_request_rtl(rqst);
	if (canceled && rtl)
		ssh_rtl_tx_schedule(rtl);

	return canceled;
}


static void ssh_rtl_packet_callback(struct ssh_packet *p, int status)
{
	struct ssh_request *r = to_ssh_request(p, packet);

	if (unlikely(status)) {
		set_bit(SSH_REQUEST_SF_LOCKED_BIT, &r->state);

		if (test_and_set_bit(SSH_REQUEST_SF_COMPLETED_BIT, &r->state))
			return;

		/*
		 * The packet may get cancelled even though it has not been
		 * submitted yet. The request may still be queued. Check the
		 * queue and remove it if necessary. As the timeout would have
		 * been started in this function on success, there's no need to
		 * cancel it here.
		 */
		ssh_rtl_queue_remove(r);
		ssh_rtl_pending_remove(r);
		ssh_rtl_complete_with_status(r, status);

		ssh_rtl_tx_schedule(ssh_request_rtl(r));
		return;
	}

	// update state: mark as transmitted and clear transmitting
	set_bit(SSH_REQUEST_SF_TRANSMITTED_BIT, &r->state);
	// ensure state never gets zero
	smp_mb__before_atomic();
	clear_bit(SSH_REQUEST_SF_TRANSMITTING_BIT, &r->state);

	// if we expect a response, we just need to start the timeout
	if (test_bit(SSH_REQUEST_TY_HAS_RESPONSE_BIT, &r->state)) {
		ssh_rtl_timeout_start(r);
		return;
	}

	/*
	 * If we don't expect a response, lock, remove, and complete the
	 * request. Note that, at this point, the request is guaranteed to have
	 * left the queue and no timeout has been started. Thus we only need to
	 * remove it from pending. If the request has already been completed (it
	 * may have been canceled) return.
	 */

	set_bit(SSH_REQUEST_SF_LOCKED_BIT, &r->state);
	if (test_and_set_bit(SSH_REQUEST_SF_COMPLETED_BIT, &r->state))
		return;

	ssh_rtl_pending_remove(r);
	ssh_rtl_complete_with_status(r, 0);

	ssh_rtl_tx_schedule(ssh_request_rtl(r));
}


static ktime_t ssh_request_get_expiration(struct ssh_request *r, ktime_t timeo)
{
	ktime_t timestamp = READ_ONCE(r->timestamp);

	if (timestamp != KTIME_MAX)
		return ktime_add(timestamp, timeo);
	else
		return KTIME_MAX;
}

static void ssh_rtl_timeout_reap(struct work_struct *work)
{
	struct ssh_rtl *rtl = to_ssh_rtl(work, rtx_timeout.reaper.work);
	struct ssh_request *r, *n;
	LIST_HEAD(claimed);
	ktime_t now = ktime_get_coarse_boottime();
	ktime_t timeout = rtl->rtx_timeout.timeout;
	ktime_t next = KTIME_MAX;

	trace_ssam_rtl_timeout_reap("pending", atomic_read(&rtl->pending.count));

	/*
	 * Mark reaper as "not pending". This is done before checking any
	 * requests to avoid lost-update type problems.
	 */
	WRITE_ONCE(rtl->rtx_timeout.expires, KTIME_MAX);
	/*
	 * Ensure that the reaper is marked as deactivated before we continue
	 * checking requests to prevent lost-update problems when a request is
	 * added to the pending set and ssh_rtl_timeout_reaper_mod is called
	 * during execution of the part below.
	 */
	smp_mb__after_atomic();

	spin_lock(&rtl->pending.lock);
	list_for_each_entry_safe(r, n, &rtl->pending.head, node) {
		ktime_t expires = ssh_request_get_expiration(r, timeout);

		/*
		 * Check if the timeout hasn't expired yet. Find out next
		 * expiration date to be handled after this run.
		 */
		if (ktime_after(expires, now)) {
			next = ktime_before(expires, next) ? expires : next;
			continue;
		}

		// avoid further transitions if locked
		if (test_and_set_bit(SSH_REQUEST_SF_LOCKED_BIT, &r->state))
			continue;

		/*
		 * We have now marked the packet as locked. Thus it cannot be
		 * added to the pending or queued lists again after we've
		 * removed it here. We can therefore re-use the node of this
		 * packet temporarily.
		 */

		clear_bit(SSH_REQUEST_SF_PENDING_BIT, &r->state);

		atomic_dec(&rtl->pending.count);
		list_del(&r->node);

		list_add_tail(&r->node, &claimed);
	}
	spin_unlock(&rtl->pending.lock);

	// cancel and complete the request
	list_for_each_entry_safe(r, n, &claimed, node) {
		trace_ssam_request_timeout(r);

		/*
		 * At this point we've removed the packet from pending. This
		 * means that we've obtained the last (only) reference of the
		 * system to it. Thus we can just complete it.
		 */
		if (!test_and_set_bit(SSH_REQUEST_SF_COMPLETED_BIT, &r->state))
			ssh_rtl_complete_with_status(r, -ETIMEDOUT);

		// drop the reference we've obtained by removing it from pending
		list_del(&r->node);
		ssh_request_put(r);
	}

	// ensure that reaper doesn't run again immediately
	next = max(next, ktime_add(now, SSH_RTL_REQUEST_TIMEOUT_RESOLUTION));
	if (next != KTIME_MAX)
		ssh_rtl_timeout_reaper_mod(rtl, now, next);

	ssh_rtl_tx_schedule(rtl);
}


static void ssh_rtl_rx_event(struct ssh_rtl *rtl, const struct ssh_command *cmd,
			     const struct ssam_span *data)
{
	trace_ssam_rx_event_received(cmd, data->len);

	rtl_dbg(rtl, "rtl: handling event (rqid: 0x%04x)\n",
		get_unaligned_le16(&cmd->rqid));

	rtl->ops.handle_event(rtl, cmd, data);
}

static void ssh_rtl_rx_command(struct ssh_ptl *p, const struct ssam_span *data)
{
	struct ssh_rtl *rtl = to_ssh_rtl(p, ptl);
	struct device *dev = &p->serdev->dev;
	struct ssh_command *command;
	struct ssam_span command_data;

	if (sshp_parse_command(dev, data, &command, &command_data))
		return;

	if (ssh_rqid_is_event(get_unaligned_le16(&command->rqid)))
		ssh_rtl_rx_event(rtl, command, &command_data);
	else
		ssh_rtl_complete(rtl, command, &command_data);
}

static void ssh_rtl_rx_data(struct ssh_ptl *p, const struct ssam_span *data)
{
	switch (data->ptr[0]) {
	case SSH_PLD_TYPE_CMD:
		ssh_rtl_rx_command(p, data);
		break;

	default:
		ptl_err(p, "rtl: rx: unknown frame payload type (type: 0x%02x)\n",
			data->ptr[0]);
		break;
	}
}


bool ssh_rtl_tx_flush(struct ssh_rtl *rtl)
{
	return flush_work(&rtl->tx.work);
}

int ssh_rtl_rx_start(struct ssh_rtl *rtl)
{
	return ssh_ptl_rx_start(&rtl->ptl);
}

int ssh_rtl_tx_start(struct ssh_rtl *rtl)
{
	int status;
	bool sched;

	status = ssh_ptl_tx_start(&rtl->ptl);
	if (status)
		return status;

	/*
	 * If the packet layer has been shut down and restarted without shutting
	 * down the request layer, there may still be requests queued and not
	 * handled.
	 */
	spin_lock(&rtl->queue.lock);
	sched = !list_empty(&rtl->queue.head);
	spin_unlock(&rtl->queue.lock);

	if (sched)
		ssh_rtl_tx_schedule(rtl);

	return 0;
}

int ssh_rtl_init(struct ssh_rtl *rtl, struct serdev_device *serdev,
		 const struct ssh_rtl_ops *ops)
{
	struct ssh_ptl_ops ptl_ops;
	int status;

	ptl_ops.data_received = ssh_rtl_rx_data;

	status = ssh_ptl_init(&rtl->ptl, serdev, &ptl_ops);
	if (status)
		return status;

	spin_lock_init(&rtl->queue.lock);
	INIT_LIST_HEAD(&rtl->queue.head);

	spin_lock_init(&rtl->pending.lock);
	INIT_LIST_HEAD(&rtl->pending.head);
	atomic_set_release(&rtl->pending.count, 0);

	INIT_WORK(&rtl->tx.work, ssh_rtl_tx_work_fn);

	rtl->rtx_timeout.timeout = SSH_RTL_REQUEST_TIMEOUT;
	rtl->rtx_timeout.expires = KTIME_MAX;
	INIT_DELAYED_WORK(&rtl->rtx_timeout.reaper, ssh_rtl_timeout_reap);

	rtl->ops = *ops;

	return 0;
}

void ssh_rtl_destroy(struct ssh_rtl *rtl)
{
	ssh_ptl_destroy(&rtl->ptl);
}


static void ssh_rtl_packet_release(struct ssh_packet *p)
{
	struct ssh_request *rqst;

	rqst = to_ssh_request(p, packet);
	rqst->ops->release(rqst);
}

static const struct ssh_packet_ops ssh_rtl_packet_ops = {
	.complete = ssh_rtl_packet_callback,
	.release = ssh_rtl_packet_release,
};

void ssh_request_init(struct ssh_request *rqst, enum ssam_request_flags flags,
		      const struct ssh_request_ops *ops)
{
	struct ssh_packet_args packet_args;

	packet_args.type = BIT(SSH_PACKET_TY_BLOCKING_BIT);
	if (!(flags & SSAM_REQUEST_UNSEQUENCED))
		packet_args.type |= BIT(SSH_PACKET_TY_SEQUENCED_BIT);

	packet_args.priority = SSH_PACKET_PRIORITY(DATA, 0);
	packet_args.ops = &ssh_rtl_packet_ops;

	ssh_packet_init(&rqst->packet, &packet_args);
	INIT_LIST_HEAD(&rqst->node);

	rqst->state = 0;
	if (flags & SSAM_REQUEST_HAS_RESPONSE)
		rqst->state |= BIT(SSH_REQUEST_TY_HAS_RESPONSE_BIT);

	rqst->timestamp = KTIME_MAX;
	rqst->ops = ops;
}


struct ssh_flush_request {
	struct ssh_request base;
	struct completion completion;
	int status;
};

static void ssh_rtl_flush_request_complete(struct ssh_request *r,
					   const struct ssh_command *cmd,
					   const struct ssam_span *data,
					   int status)
{
	struct ssh_flush_request *rqst;

	rqst = container_of(r, struct ssh_flush_request, base);
	rqst->status = status;
}

static void ssh_rtl_flush_request_release(struct ssh_request *r)
{
	struct ssh_flush_request *rqst;

	rqst = container_of(r, struct ssh_flush_request, base);
	complete_all(&rqst->completion);
}

static const struct ssh_request_ops ssh_rtl_flush_request_ops = {
	.complete = ssh_rtl_flush_request_complete,
	.release = ssh_rtl_flush_request_release,
};

/**
 * ssh_rtl_flush - flush the request transmission layer
 * @rtl:     request transmission layer
 * @timeout: timeout for the flush operation in jiffies
 *
 * Queue a special flush request and wait for its completion. This request
 * will be completed after all other currently queued and pending requests
 * have been completed. Instead of a normal data packet, this request submits
 * a special flush packet, meaning that upon completion, also the underlying
 * packet transmission layer has been flushed.
 *
 * Flushing the request layer gurarantees that all previously submitted
 * requests have been fully completed before this call returns. Additinally,
 * flushing blocks execution of all later submitted requests until the flush
 * has been completed.
 *
 * If the caller ensures that no new requests are submitted after a call to
 * this function, the request transmission layer is guaranteed to have no
 * remaining requests when this call returns. The same guarantee does not hold
 * for the packet layer, on which control packets may still be queued after
 * this call. See the documentation of ssh_ptl_flush for more details on
 * packet layer flushing.
 *
 * Return: Zero on success, -ETIMEDOUT if the flush timed out and has been
 * canceled as a result of the timeout, or -ESHUTDOWN if the packet and/or
 * request transmission layer has been shut down before this call. May also
 * return -EINTR if the underlying packet transmission has been interrupted.
 */
int ssh_rtl_flush(struct ssh_rtl *rtl, unsigned long timeout)
{
	const unsigned int init_flags = SSAM_REQUEST_UNSEQUENCED;
	struct ssh_flush_request rqst;
	int status;

	ssh_request_init(&rqst.base, init_flags, &ssh_rtl_flush_request_ops);
	rqst.base.packet.state |= BIT(SSH_PACKET_TY_FLUSH_BIT);
	rqst.base.packet.priority = SSH_PACKET_PRIORITY(FLUSH, 0);
	rqst.base.state |= BIT(SSH_REQUEST_TY_FLUSH_BIT);

	init_completion(&rqst.completion);

	status = ssh_rtl_submit(rtl, &rqst.base);
	if (status)
		return status;

	ssh_request_put(&rqst.base);

	if (wait_for_completion_timeout(&rqst.completion, timeout))
		return 0;

	ssh_rtl_cancel(&rqst.base, true);
	wait_for_completion(&rqst.completion);

	WARN_ON(rqst.status != 0 && rqst.status != -ECANCELED
		&& rqst.status != -ESHUTDOWN && rqst.status != -EINTR);

	return rqst.status == -ECANCELED ? -ETIMEDOUT : status;
}


void ssh_rtl_shutdown(struct ssh_rtl *rtl)
{
	struct ssh_request *r, *n;
	LIST_HEAD(claimed);
	int pending;

	set_bit(SSH_RTL_SF_SHUTDOWN_BIT, &rtl->state);
	/*
	 * Ensure that the layer gets marked as shut-down before actually
	 * stopping it. In combination with the check in ssh_rtl_sunmit, this
	 * guarantees that no new requests can be added and all already queued
	 * requests are properly cancelled.
	 */
	smp_mb__after_atomic();

	// remove requests from queue
	spin_lock(&rtl->queue.lock);
	list_for_each_entry_safe(r, n, &rtl->queue.head, node) {
		set_bit(SSH_REQUEST_SF_LOCKED_BIT, &r->state);
		// ensure state never gets zero
		smp_mb__before_atomic();
		clear_bit(SSH_REQUEST_SF_QUEUED_BIT, &r->state);

		list_del(&r->node);
		list_add_tail(&r->node, &claimed);
	}
	spin_unlock(&rtl->queue.lock);

	/*
	 * We have now guaranteed that the queue is empty and no more new
	 * requests can be submitted (i.e. it will stay empty). This means that
	 * calling ssh_rtl_tx_schedule will not schedule tx.work any more. So we
	 * can simply call cancel_work_sync on tx.work here and when that
	 * returns, we've locked it down. This also means that after this call,
	 * we don't submit any more packets to the underlying packet layer, so
	 * we can also shut that down.
	 */

	cancel_work_sync(&rtl->tx.work);
	ssh_ptl_shutdown(&rtl->ptl);
	cancel_delayed_work_sync(&rtl->rtx_timeout.reaper);

	/*
	 * Shutting down the packet layer should also have caneled all requests.
	 * Thus the pending set should be empty. Attempt to handle this
	 * gracefully anyways, even though this should be dead code.
	 */

	pending = atomic_read(&rtl->pending.count);
	if (WARN_ON(pending)) {
		spin_lock(&rtl->pending.lock);
		list_for_each_entry_safe(r, n, &rtl->pending.head, node) {
			set_bit(SSH_REQUEST_SF_LOCKED_BIT, &r->state);
			// ensure state never gets zero
			smp_mb__before_atomic();
			clear_bit(SSH_REQUEST_SF_PENDING_BIT, &r->state);

			list_del(&r->node);
			list_add_tail(&r->node, &claimed);
		}
		spin_unlock(&rtl->pending.lock);
	}

	// finally cancel and complete requests
	list_for_each_entry_safe(r, n, &claimed, node) {
		// test_and_set because we still might compete with cancellation
		if (!test_and_set_bit(SSH_REQUEST_SF_COMPLETED_BIT, &r->state))
			ssh_rtl_complete_with_status(r, -ESHUTDOWN);

		// drop the reference we've obtained by removing it from list
		list_del(&r->node);
		ssh_request_put(r);
	}
}
