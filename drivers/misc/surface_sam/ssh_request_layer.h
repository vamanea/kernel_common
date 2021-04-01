/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _SSAM_SSH_REQUEST_LAYER_H
#define _SSAM_SSH_REQUEST_LAYER_H

#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <linux/surface_aggregator_module.h>

#include "ssh_packet_layer.h"


/**
 * enum ssh_rtl_state_flags - State-flags for &struct ssh_rtl.
 *
 * @SSH_RTL_SF_SHUTDOWN_BIT:
 *	Indicates that the request transmission layer has been shut down or is
 *	being shut down and should not accept any new requests.
 */
enum ssh_rtl_state_flags {
	SSH_RTL_SF_SHUTDOWN_BIT,
};

/**
 * struct ssh_rtl_ops - Callback operations for request transmission layer.
 * @handle_event: Function called when a SSH event has been received. The
 *                specified function takes the request layer, received command
 *                struct, and corresponding payload as arguments. If the event
 *                has no payload, the payload span is empty (not %NULL).
 */
struct ssh_rtl_ops {
	void (*handle_event)(struct ssh_rtl *rtl, const struct ssh_command *cmd,
			     const struct ssam_span *data);
};

/**
 * struct ssh_rtl - SSH request transmission layer.
 * @ptl:           Underlying packet transmission layer.
 * @state:         State(-flags) of the transmission layer.
 * @queue:         Request submission queue.
 * @queue.lock:    Lock for modifying the request submission queue.
 * @queue.head:    List-head of the request submission queue.
 * @pending:       Set/list of pending requests.
 * @pending.lock:  Lock for modifying the request set.
 * @pending.head:  List-head of the pending set/list.
 * @pending.count: Number of currently pending requests.
 * @tx:            Transmitter subsystem.
 * @tx.work:       Transmitter work item.
 * @rtx_timeout:   Retransmission timeout subsystem.
 * @rtx_timeout.timeout: Timout inverval for retransmission.
 * @rtx_timeout.expires: Time specifying when the reaper work is next scheduled.
 * @rtx_timeout.reaper:  Work performing timeout checks and subsequent actions.
 * @ops:           Request layer operations.
 */
struct ssh_rtl {
	struct ssh_ptl ptl;
	unsigned long state;

	struct {
		spinlock_t lock;
		struct list_head head;
	} queue;

	struct {
		spinlock_t lock;
		struct list_head head;
		atomic_t count;
	} pending;

	struct {
		struct work_struct work;
	} tx;

	struct {
		ktime_t timeout;
		ktime_t expires;
		struct delayed_work reaper;
	} rtx_timeout;

	struct ssh_rtl_ops ops;
};

#define rtl_dbg(r, fmt, ...)  ptl_dbg(&(r)->ptl, fmt, ##__VA_ARGS__)
#define rtl_info(p, fmt, ...) ptl_info(&(p)->ptl, fmt, ##__VA_ARGS__)
#define rtl_warn(r, fmt, ...) ptl_warn(&(r)->ptl, fmt, ##__VA_ARGS__)
#define rtl_err(r, fmt, ...)  ptl_err(&(r)->ptl, fmt, ##__VA_ARGS__)
#define rtl_dbg_cond(r, fmt, ...) __ssam_prcond(rtl_dbg, r, fmt, ##__VA_ARGS__)

#define to_ssh_rtl(ptr, member) \
	container_of(ptr, struct ssh_rtl, member)

/**
 * ssh_rtl_get_device() - Get device associated with request transmission layer.
 * @rtl: The request transmission layer.
 *
 * Return: Returns the device on which the given request transmission layer
 * builds upon.
 */
static inline struct device *ssh_rtl_get_device(struct ssh_rtl *rtl)
{
	return ssh_ptl_get_device(&rtl->ptl);
}

/**
 * ssh_request_rtl() - Get request transmission layer associated with request.
 * @rqst: The request to get the request transmission layer reference for.
 *
 * Return: Returns the &struct ssh_rtl associated with the given SSH request.
 */
static inline struct ssh_rtl *ssh_request_rtl(struct ssh_request *rqst)
{
	struct ssh_ptl *ptl;

	ptl = READ_ONCE(rqst->packet.ptl);
	return likely(ptl) ? to_ssh_rtl(ptl, ptl) : NULL;
}

int ssh_rtl_submit(struct ssh_rtl *rtl, struct ssh_request *rqst);
bool ssh_rtl_cancel(struct ssh_request *rqst, bool pending);

int ssh_rtl_init(struct ssh_rtl *rtl, struct serdev_device *serdev,
		 const struct ssh_rtl_ops *ops);

int ssh_rtl_start(struct ssh_rtl *rtl);
int ssh_rtl_flush(struct ssh_rtl *rtl, unsigned long timeout);
void ssh_rtl_shutdown(struct ssh_rtl *rtl);
void ssh_rtl_destroy(struct ssh_rtl *rtl);

void ssh_request_init(struct ssh_request *rqst, enum ssam_request_flags flags,
		      const struct ssh_request_ops *ops);

#endif /* _SSAM_SSH_REQUEST_LAYER_H */
