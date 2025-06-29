// SPDX-License-Identifier: GPL-2.0
/*
 * System Control and Management Interface (SCMI) Message Protocol driver
 *
 * SCMI Message Protocol is used between the System Control Processor(SCP)
 * and the Application Processors(AP). The Message Handling Unit(MHU)
 * provides a mechanism for inter-processor communication between SCP's
 * Cortex M3 and AP.
 *
 * SCP offers control and management of the core/cluster power states,
 * various power domain DVFS including the core/cluster, certain system
 * clocks configuration, thermal sensors and many others.
 *
 * Copyright (C) 2018-2021 ARM Ltd.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <common.h>
#include <linux/bitmap.h>
#include <driver.h>
#include <linux/export.h>
#include <linux/notifier.h>
#include <linux/io.h>
#include <io-64-nonatomic-hi-lo.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <of_address.h>
#include <of_device.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/mutex.h>
#include <linux/processor.h>

#include "common.h"

static DEFINE_IDR(scmi_protocols);
static DEFINE_SPINLOCK(protocol_lock);

/* List of all SCMI devices active in system */
static LIST_HEAD(scmi_list);
/* Protection for the entire list */
static DEFINE_MUTEX(scmi_list_mutex);
/* Track the unique id for the transfers for debug & profiling purpose */
static atomic_t transfer_last_id;

/**
 * struct scmi_xfers_info - Structure to manage transfer information
 *
 * @xfer_alloc_table: Bitmap table for allocated messages.
 *	Index of this bitmap table is also used for message
 *	sequence identifier.
 * @xfer_lock: Protection for message allocation
 * @max_msg: Maximum number of messages that can be pending
 * @free_xfers: A free list for available to use xfers. It is initialized with
 *		a number of xfers equal to the maximum allowed in-flight
 *		messages.
 */
struct scmi_xfers_info {
	unsigned long *xfer_alloc_table;
	spinlock_t xfer_lock;
	int max_msg;
	struct hlist_head free_xfers;
};

/**
 * struct scmi_protocol_instance  - Describe an initialized protocol instance.
 * @handle: Reference to the SCMI handle associated to this protocol instance.
 * @proto: A reference to the protocol descriptor.
 * @users: A refcount to track effective users of this protocol.
 * @priv: Reference for optional protocol private data.
 * @ph: An embedded protocol handle that will be passed down to protocol
 *	initialization code to identify this instance.
 *
 * Each protocol is initialized independently once for each SCMI platform in
 * which is defined by DT and implemented by the SCMI server fw.
 */
struct scmi_protocol_instance {
	const struct scmi_handle	*handle;
	const struct scmi_protocol	*proto;
	refcount_t			users;
	void				*priv;
	struct scmi_protocol_handle	ph;
};

#define ph_to_pi(h)	container_of(h, struct scmi_protocol_instance, ph)

/**
 * struct scmi_info - Structure representing a SCMI instance
 *
 * @id: A sequence number starting from zero identifying this instance
 * @dev: Device pointer
 * @desc: SoC description for this instance
 * @version: SCMI revision information containing protocol version,
 *	implementation version and (sub-)vendor identification.
 * @handle: Instance of SCMI handle to send to clients
 * @tx_minfo: Universal Transmit Message management info
 * @rx_minfo: Universal Receive Message management info
 * @tx_idr: IDR object to map protocol id to Tx channel info pointer
 * @rx_idr: IDR object to map protocol id to Rx channel info pointer
 * @protocols: IDR for protocols' instance descriptors initialized for
 *	       this SCMI instance: populated on protocol's first attempted
 *	       usage.
 * @protocols_mtx: A mutex to protect protocols instances initialization.
 * @protocols_imp: List of protocols implemented, currently maximum of
 *		   scmi_revision_info.num_protocols elements allocated by the
 *		   base protocol
 * @active_protocols: IDR storing device_nodes for protocols actually defined
 *		      in the DT and confirmed as implemented by fw.
 * @atomic_threshold: Optional system wide DT-configured threshold, expressed
 *		      in microseconds, for atomic operations.
 *		      Only SCMI synchronous commands reported by the platform
 *		      to have an execution latency lesser-equal to the threshold
 *		      should be considered for atomic mode operation: such
 *		      decision is finally left up to the SCMI drivers.
 * @node: List head
 * @users: Number of users of this instance
 * @dev_req_nb: A notifier to listen for device request/unrequest on the scmi
 *		bus
 * @devreq_mtx: A mutex to serialize device creation for this SCMI instance
 */
struct scmi_info {
	struct device *dev;
	const struct scmi_desc *desc;
	struct scmi_revision_info version;
	struct scmi_handle handle;
	struct scmi_xfers_info tx_minfo;
	struct scmi_xfers_info rx_minfo;
	struct idr tx_idr;
	struct idr rx_idr;
	struct idr protocols;
	/* Ensure mutual exclusive access to protocols instance array */
	struct mutex protocols_mtx;
	u8 *protocols_imp;
	struct idr active_protocols;
	unsigned int atomic_threshold;
	struct list_head node;
	struct notifier_block dev_req_nb;
	/* Serialize device creation process for this instance */
	struct mutex devreq_mtx;
};

#define handle_to_scmi_info(h)	container_of(h, struct scmi_info, handle)
#define req_nb_to_scmi_info(nb)	container_of(nb, struct scmi_info, dev_req_nb)

static const struct scmi_protocol *scmi_protocol_get(int protocol_id)
{
	const struct scmi_protocol *proto;

	proto = idr_find(&scmi_protocols, protocol_id);
	if (!proto) {
		pr_warn("SCMI Protocol 0x%x not found!\n", protocol_id);
		return NULL;
	}

	pr_debug("Found SCMI Protocol 0x%x\n", protocol_id);

	return proto;
}

int scmi_protocol_register(const struct scmi_protocol *proto)
{
	int ret;

	if (!proto) {
		pr_err("invalid protocol\n");
		return -EINVAL;
	}

	if (!proto->instance_init) {
		pr_err("missing init for protocol 0x%x\n", proto->id);
		return -EINVAL;
	}

	spin_lock(&protocol_lock);
	ret = idr_alloc(&scmi_protocols, (void *)proto, proto->id, proto->id + 1,
			GFP_KERNEL);
	spin_unlock(&protocol_lock);
	if (ret != proto->id) {
		pr_err("unable to allocate SCMI idr slot for 0x%x - err %d\n",
		       proto->id, ret);
		return ret;
	}

	pr_debug("Registered SCMI Protocol 0x%x\n", proto->id);

	return 0;
}
EXPORT_SYMBOL_GPL(scmi_protocol_register);

/**
 * scmi_create_protocol_devices  - Create devices for all pending requests for
 * this SCMI instance.
 *
 * @np: The device node describing the protocol
 * @info: The SCMI instance descriptor
 * @prot_id: The protocol ID
 * @name: The optional name of the device to be created: if not provided this
 *	  call will lead to the creation of all the devices currently requested
 *	  for the specified protocol.
 */
static void scmi_create_protocol_devices(struct device_node *np,
					 struct scmi_info *info,
					 int prot_id, const char *name)
{
	struct scmi_device *sdev;

	mutex_lock(&info->devreq_mtx);
	sdev = scmi_device_create(np, info->dev, prot_id, name);
	if (name && !sdev)
		dev_err(info->dev,
			"failed to create device for protocol 0x%X (%s)\n",
			prot_id, name);
	mutex_unlock(&info->devreq_mtx);
}

static void scmi_destroy_protocol_devices(struct scmi_info *info,
					  int prot_id, const char *name)
{
	mutex_lock(&info->devreq_mtx);
	scmi_device_destroy(info->dev, prot_id, name);
	mutex_unlock(&info->devreq_mtx);
}

/**
 * scmi_xfer_token_set  - Reserve and set new token for the xfer at hand
 *
 * @minfo: Pointer to Tx/Rx Message management info based on channel type
 * @xfer: The xfer to act upon
 *
 * Pick the next unused monotonically increasing token and set it into
 * xfer->hdr.seq: picking a monotonically increasing value avoids immediate
 * reuse of freshly completed or timed-out xfers, thus mitigating the risk
 * of incorrect association of a late and expired xfer with a live in-flight
 * transaction, both happening to re-use the same token identifier.
 *
 * Since platform is NOT required to answer our request in-order we should
 * account for a few rare but possible scenarios:
 *
 *  - exactly 'next_token' may be NOT available so pick xfer_id >= next_token
 *    using find_next_zero_bit() starting from candidate next_token bit
 *
 *  - all tokens ahead upto (MSG_TOKEN_ID_MASK - 1) are used in-flight but we
 *    are plenty of free tokens at start, so try a second pass using
 *    find_next_zero_bit() and starting from 0.
 *
 *  X = used in-flight
 *
 * Normal
 * ------
 *
 *		|- xfer_id picked
 *   -----------+----------------------------------------------------------
 *   | | |X|X|X| | | | | | ... ... ... ... ... ... ... ... ... ... ...|X|X|
 *   ----------------------------------------------------------------------
 *		^
 *		|- next_token
 *
 * Out-of-order pending at start
 * -----------------------------
 *
 *	  |- xfer_id picked, last_token fixed
 *   -----+----------------------------------------------------------------
 *   |X|X| | | | |X|X| ... ... ... ... ... ... ... ... ... ... ... ...|X| |
 *   ----------------------------------------------------------------------
 *    ^
 *    |- next_token
 *
 *
 * Out-of-order pending at end
 * ---------------------------
 *
 *	  |- xfer_id picked, last_token fixed
 *   -----+----------------------------------------------------------------
 *   |X|X| | | | |X|X| ... ... ... ... ... ... ... ... ... ... |X|X|X||X|X|
 *   ----------------------------------------------------------------------
 *								^
 *								|- next_token
 *
 * Context: Assumes to be called with @xfer_lock already acquired.
 *
 * Return: 0 on Success or error
 */
static int scmi_xfer_token_set(struct scmi_xfers_info *minfo,
			       struct scmi_xfer *xfer)
{
	unsigned long xfer_id, next_token;

	/*
	 * Pick a candidate monotonic token in range [0, MSG_TOKEN_MAX - 1]
	 * using the pre-allocated transfer_id as a base.
	 * Note that the global transfer_id is shared across all message types
	 * so there could be holes in the allocated set of monotonic sequence
	 * numbers, but that is going to limit the effectiveness of the
	 * mitigation only in very rare limit conditions.
	 */
	next_token = (xfer->transfer_id & (MSG_TOKEN_MAX - 1));

	/* Pick the next available xfer_id >= next_token */
	xfer_id = find_next_zero_bit(minfo->xfer_alloc_table,
				     MSG_TOKEN_MAX, next_token);
	if (xfer_id == MSG_TOKEN_MAX) {
		/*
		 * After heavily out-of-order responses, there are no free
		 * tokens ahead, but only at start of xfer_alloc_table so
		 * try again from the beginning.
		 */
		xfer_id = find_next_zero_bit(minfo->xfer_alloc_table,
					     MSG_TOKEN_MAX, 0);
		/*
		 * Something is wrong if we got here since there can be a
		 * maximum number of (MSG_TOKEN_MAX - 1) in-flight messages
		 * but we have not found any free token [0, MSG_TOKEN_MAX - 1].
		 */
		if (WARN_ON_ONCE(xfer_id == MSG_TOKEN_MAX))
			return -ENOMEM;
	}

	/* Update +/- last_token accordingly if we skipped some hole */
	if (xfer_id != next_token)
		atomic_add((int)(xfer_id - next_token), &transfer_last_id);

	xfer->hdr.seq = (u16)xfer_id;

	return 0;
}

/**
 * scmi_xfer_token_clear  - Release the token
 *
 * @minfo: Pointer to Tx/Rx Message management info based on channel type
 * @xfer: The xfer to act upon
 */
static inline void scmi_xfer_token_clear(struct scmi_xfers_info *minfo,
					 struct scmi_xfer *xfer)
{
	clear_bit(xfer->hdr.seq, minfo->xfer_alloc_table);
}

/**
 * scmi_xfer_inflight_register_unlocked  - Register the xfer as in-flight
 *
 * @xfer: The xfer to register
 * @minfo: Pointer to Tx/Rx Message management info based on channel type
 *
 * Note that this helper assumes that the xfer to be registered as in-flight
 * had been built using an xfer sequence number which still corresponds to a
 * free slot in the xfer_alloc_table.
 *
 * Context: Assumes to be called with @xfer_lock already acquired.
 */
static inline void
scmi_xfer_inflight_register_unlocked(struct scmi_xfer *xfer,
				     struct scmi_xfers_info *minfo)
{
	/* Set in-flight */
	set_bit(xfer->hdr.seq, minfo->xfer_alloc_table);
	xfer->pending = true;
}

/**
 * scmi_xfer_pending_set  - Pick a proper sequence number and mark the xfer
 * as pending in-flight
 *
 * @xfer: The xfer to act upon
 * @minfo: Pointer to Tx/Rx Message management info based on channel type
 *
 * Return: 0 on Success or error otherwise
 */
static inline int scmi_xfer_pending_set(struct scmi_xfer *xfer,
					struct scmi_xfers_info *minfo)
{
	int ret;
	unsigned long flags;

	spin_lock_irqsave(&minfo->xfer_lock, flags);
	/* Set a new monotonic token as the xfer sequence number */
	ret = scmi_xfer_token_set(minfo, xfer);
	if (!ret)
		scmi_xfer_inflight_register_unlocked(xfer, minfo);
	spin_unlock_irqrestore(&minfo->xfer_lock, flags);

	return ret;
}

/**
 * scmi_xfer_get() - Allocate one message
 *
 * @handle: Pointer to SCMI entity handle
 * @minfo: Pointer to Tx/Rx Message management info based on channel type
 *
 * Helper function which is used by various message functions that are
 * exposed to clients of this driver for allocating a message traffic event.
 *
 * Picks an xfer from the free list @free_xfers (if any available) and perform
 * a basic initialization.
 *
 * Note that, at this point, still no sequence number is assigned to the
 * allocated xfer, nor it is registered as a pending transaction.
 *
 * The successfully initialized xfer is refcounted.
 *
 * Context: Holds @xfer_lock while manipulating @free_xfers.
 *
 * Return: An initialized xfer if all went fine, else pointer error.
 */
static struct scmi_xfer *scmi_xfer_get(const struct scmi_handle *handle,
				       struct scmi_xfers_info *minfo)
{
	unsigned long flags;
	struct scmi_xfer *xfer;

	spin_lock_irqsave(&minfo->xfer_lock, flags);
	if (hlist_empty(&minfo->free_xfers)) {
		spin_unlock_irqrestore(&minfo->xfer_lock, flags);
		return ERR_PTR(-ENOMEM);
	}

	/* grab an xfer from the free_list */
	xfer = hlist_entry(minfo->free_xfers.first, struct scmi_xfer, node);
	hlist_del_init(&xfer->node);

	/*
	 * Allocate transfer_id early so that can be used also as base for
	 * monotonic sequence number generation if needed.
	 */
	xfer->transfer_id = atomic_inc_return(&transfer_last_id);

	refcount_set(&xfer->users, 1);
	atomic_set(&xfer->busy, SCMI_XFER_FREE);
	spin_unlock_irqrestore(&minfo->xfer_lock, flags);

	return xfer;
}

/**
 * __scmi_xfer_put() - Release a message
 *
 * @minfo: Pointer to Tx/Rx Message management info based on channel type
 * @xfer: message that was reserved by scmi_xfer_get
 *
 * After refcount check, possibly release an xfer, clearing the token slot,
 * removing xfer from @pending_xfers and putting it back into free_xfers.
 *
 * This holds a spinlock to maintain integrity of internal data structures.
 */
static void
__scmi_xfer_put(struct scmi_xfers_info *minfo, struct scmi_xfer *xfer)
{
	unsigned long flags;

	spin_lock_irqsave(&minfo->xfer_lock, flags);
	if (refcount_dec_and_test(&xfer->users)) {
		if (xfer->pending) {
			scmi_xfer_token_clear(minfo, xfer);
			xfer->pending = false;
		}
		hlist_add_head(&xfer->node, &minfo->free_xfers);
	}
	spin_unlock_irqrestore(&minfo->xfer_lock, flags);
}

/**
 * xfer_put() - Release a transmit message
 *
 * @ph: Pointer to SCMI protocol handle
 * @xfer: message that was reserved by xfer_get_init
 */
static void xfer_put(const struct scmi_protocol_handle *ph,
		     struct scmi_xfer *xfer)
{
	const struct scmi_protocol_instance *pi = ph_to_pi(ph);
	struct scmi_info *info = handle_to_scmi_info(pi->handle);

	__scmi_xfer_put(&info->tx_minfo, xfer);
}

static int scmi_wait_for_reply(struct device *dev, const struct scmi_desc *desc,
			       struct scmi_chan_info *cinfo,
			       struct scmi_xfer *xfer, unsigned int timeout_ms)
{
	int ret = 0;
	unsigned long flags;

	/*
	 * Do not fetch_response if an out-of-order delayed
	 * response is being processed.
	 */
	spin_lock_irqsave(&xfer->lock, flags);
	if (xfer->state == SCMI_XFER_SENT_OK) {
		desc->ops->fetch_response(cinfo, xfer);
		xfer->state = SCMI_XFER_RESP_OK;
	}
	spin_unlock_irqrestore(&xfer->lock, flags);

	return ret;
}

/**
 * scmi_wait_for_message_response  - An helper to group all the possible ways of
 * waiting for a synchronous message response.
 *
 * @cinfo: SCMI channel info
 * @xfer: Reference to the transfer being waited for.
 *
 * Return: 0 on Success, error otherwise.
 */
static int scmi_wait_for_message_response(struct scmi_chan_info *cinfo,
					  struct scmi_xfer *xfer)
{
	struct scmi_info *info = handle_to_scmi_info(cinfo->handle);
	struct device *dev = info->dev;

	return scmi_wait_for_reply(dev, info->desc, cinfo, xfer,
				   info->desc->max_rx_timeout_ms);
}

/**
 * do_xfer() - Do one transfer
 *
 * @ph: Pointer to SCMI protocol handle
 * @xfer: Transfer to initiate and wait for response
 *
 * Return: -ETIMEDOUT in case of no response, if transmit error,
 *	return corresponding error, else if all goes well,
 *	return 0.
 */
static int do_xfer(const struct scmi_protocol_handle *ph,
		   struct scmi_xfer *xfer)
{
	int ret;
	const struct scmi_protocol_instance *pi = ph_to_pi(ph);
	struct scmi_info *info = handle_to_scmi_info(pi->handle);
	struct device *dev = info->dev;
	struct scmi_chan_info *cinfo;

	/* Check for polling request on custom command xfers at first */
	if (!is_transport_polling_capable(info->desc)) {
		dev_warn_once(dev,
			      "Polling mode is not supported by transport.\n");
		return -EINVAL;
	}

	cinfo = idr_find(&info->tx_idr, pi->proto->id);
	if (unlikely(!cinfo))
		return -EINVAL;

	/*
	 * Initialise protocol id now from protocol handle to avoid it being
	 * overridden by mistake (or malice) by the protocol code mangling with
	 * the scmi_xfer structure prior to this.
	 */
	xfer->hdr.protocol_id = pi->proto->id;

	/* Clear any stale status */
	xfer->hdr.status = SCMI_SUCCESS;
	xfer->state = SCMI_XFER_SENT_OK;

	ret = info->desc->ops->send_message(cinfo, xfer);
	if (ret < 0) {
		dev_dbg(dev, "Failed to send message %d\n", ret);
		return ret;
	}

	ret = scmi_wait_for_message_response(cinfo, xfer);
	if (!ret && xfer->hdr.status)
		ret = scmi_to_linux_errno(xfer->hdr.status);

	if (info->desc->ops->mark_txdone)
		info->desc->ops->mark_txdone(cinfo, ret, xfer);

	return ret;
}

static void reset_rx_to_maxsz(const struct scmi_protocol_handle *ph,
			      struct scmi_xfer *xfer)
{
	const struct scmi_protocol_instance *pi = ph_to_pi(ph);
	struct scmi_info *info = handle_to_scmi_info(pi->handle);

	xfer->rx.len = info->desc->max_msg_size;
}

/**
 * xfer_get_init() - Allocate and initialise one message for transmit
 *
 * @ph: Pointer to SCMI protocol handle
 * @msg_id: Message identifier
 * @tx_size: transmit message size
 * @rx_size: receive message size
 * @p: pointer to the allocated and initialised message
 *
 * This function allocates the message using @scmi_xfer_get and
 * initialise the header.
 *
 * Return: 0 if all went fine with @p pointing to message, else
 *	corresponding error.
 */
static int xfer_get_init(const struct scmi_protocol_handle *ph,
			 u8 msg_id, size_t tx_size, size_t rx_size,
			 struct scmi_xfer **p)
{
	int ret;
	struct scmi_xfer *xfer;
	const struct scmi_protocol_instance *pi = ph_to_pi(ph);
	struct scmi_info *info = handle_to_scmi_info(pi->handle);
	struct scmi_xfers_info *minfo = &info->tx_minfo;
	struct device *dev = info->dev;

	/* Ensure we have sane transfer sizes */
	if (rx_size > info->desc->max_msg_size ||
	    tx_size > info->desc->max_msg_size)
		return -ERANGE;

	xfer = scmi_xfer_get(pi->handle, minfo);
	if (IS_ERR(xfer)) {
		ret = PTR_ERR(xfer);
		dev_err(dev, "failed to get free message slot(%d)\n", ret);
		return ret;
	}

	/* Pick a sequence number and register this xfer as in-flight */
	ret = scmi_xfer_pending_set(xfer, minfo);
	if (ret) {
		dev_err(pi->handle->dev,
			"Failed to get monotonic token %d\n", ret);
		__scmi_xfer_put(minfo, xfer);
		return ret;
	}

	xfer->tx.len = tx_size;
	xfer->rx.len = rx_size ? : info->desc->max_msg_size;
	xfer->hdr.type = MSG_TYPE_COMMAND;
	xfer->hdr.id = msg_id;

	*p = xfer;

	return 0;
}

/**
 * version_get() - command to get the revision of the SCMI entity
 *
 * @ph: Pointer to SCMI protocol handle
 * @version: Holds returned version of protocol.
 *
 * Updates the SCMI information in the internal data structure.
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int version_get(const struct scmi_protocol_handle *ph, u32 *version)
{
	int ret;
	__le32 *rev_info;
	struct scmi_xfer *t;

	ret = xfer_get_init(ph, PROTOCOL_VERSION, 0, sizeof(*version), &t);
	if (ret)
		return ret;

	ret = do_xfer(ph, t);
	if (!ret) {
		rev_info = t->rx.buf;
		*version = le32_to_cpu(*rev_info);
	}

	xfer_put(ph, t);
	return ret;
}

/**
 * scmi_set_protocol_priv  - Set protocol specific data at init time
 *
 * @ph: A reference to the protocol handle.
 * @priv: The private data to set.
 *
 * Return: 0 on Success
 */
static int scmi_set_protocol_priv(const struct scmi_protocol_handle *ph,
				  void *priv)
{
	struct scmi_protocol_instance *pi = ph_to_pi(ph);

	pi->priv = priv;

	return 0;
}

/**
 * scmi_get_protocol_priv  - Set protocol specific data at init time
 *
 * @ph: A reference to the protocol handle.
 *
 * Return: Protocol private data if any was set.
 */
static void *scmi_get_protocol_priv(const struct scmi_protocol_handle *ph)
{
	const struct scmi_protocol_instance *pi = ph_to_pi(ph);

	return pi->priv;
}

static const struct scmi_xfer_ops xfer_ops = {
	.version_get = version_get,
	.xfer_get_init = xfer_get_init,
	.reset_rx_to_maxsz = reset_rx_to_maxsz,
	.do_xfer = do_xfer,
	.xfer_put = xfer_put,
};

struct scmi_msg_resp_domain_name_get {
	__le32 flags;
	u8 name[SCMI_MAX_STR_SIZE];
};

/**
 * scmi_common_extended_name_get  - Common helper to get extended resources name
 * @ph: A protocol handle reference.
 * @cmd_id: The specific command ID to use.
 * @res_id: The specific resource ID to use.
 * @name: A pointer to the preallocated area where the retrieved name will be
 *	  stored as a NULL terminated string.
 * @len: The len in bytes of the @name char array.
 *
 * Return: 0 on Succcess
 */
static int scmi_common_extended_name_get(const struct scmi_protocol_handle *ph,
					 u8 cmd_id, u32 res_id, char *name,
					 size_t len)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_resp_domain_name_get *resp;

	ret = ph->xops->xfer_get_init(ph, cmd_id, sizeof(res_id),
				      sizeof(*resp), &t);
	if (ret)
		goto out;

	put_unaligned_le32(res_id, t->tx.buf);
	resp = t->rx.buf;

	ret = ph->xops->do_xfer(ph, t);
	if (!ret)
		strscpy(name, resp->name, len);

	ph->xops->xfer_put(ph, t);
out:
	if (ret)
		dev_warn(ph->dev,
			 "Failed to get extended name - id:%u (ret:%d). Using %s\n",
			 res_id, ret, name);
	return ret;
}

/**
 * struct scmi_iterator  - Iterator descriptor
 * @msg: A reference to the message TX buffer; filled by @prepare_message with
 *	 a proper custom command payload for each multi-part command request.
 * @resp: A reference to the response RX buffer; used by @update_state and
 *	  @process_response to parse the multi-part replies.
 * @t: A reference to the underlying xfer initialized and used transparently by
 *     the iterator internal routines.
 * @ph: A reference to the associated protocol handle to be used.
 * @ops: A reference to the custom provided iterator operations.
 * @state: The current iterator state; used and updated in turn by the iterators
 *	   internal routines and by the caller-provided @scmi_iterator_ops.
 * @priv: A reference to optional private data as provided by the caller and
 *	  passed back to the @@scmi_iterator_ops.
 */
struct scmi_iterator {
	void *msg;
	void *resp;
	struct scmi_xfer *t;
	const struct scmi_protocol_handle *ph;
	struct scmi_iterator_ops *ops;
	struct scmi_iterator_state state;
	void *priv;
};

static void *scmi_iterator_init(const struct scmi_protocol_handle *ph,
				struct scmi_iterator_ops *ops,
				unsigned int max_resources, u8 msg_id,
				size_t tx_size, void *priv)
{
	int ret;
	struct scmi_iterator *i;

	i = devm_kzalloc(ph->dev, sizeof(*i), GFP_KERNEL);
	if (!i)
		return ERR_PTR(-ENOMEM);

	i->ph = ph;
	i->ops = ops;
	i->priv = priv;

	ret = ph->xops->xfer_get_init(ph, msg_id, tx_size, 0, &i->t);
	if (ret) {
		devm_kfree(ph->dev, i);
		return ERR_PTR(ret);
	}

	i->state.max_resources = max_resources;
	i->msg = i->t->tx.buf;
	i->resp = i->t->rx.buf;

	return i;
}

static int scmi_iterator_run(void *iter)
{
	int ret = -EINVAL;
	struct scmi_iterator_ops *iops;
	const struct scmi_protocol_handle *ph;
	struct scmi_iterator_state *st;
	struct scmi_iterator *i = iter;

	if (!i || !i->ops || !i->ph)
		return ret;

	iops = i->ops;
	ph = i->ph;
	st = &i->state;

	do {
		iops->prepare_message(i->msg, st->desc_index, i->priv);
		ret = ph->xops->do_xfer(ph, i->t);
		if (ret)
			break;

		st->rx_len = i->t->rx.len;
		ret = iops->update_state(st, i->resp, i->priv);
		if (ret)
			break;

		if (st->num_returned > st->max_resources - st->desc_index) {
			dev_err(ph->dev,
				"No. of resources can't exceed %d\n",
				st->max_resources);
			ret = -EINVAL;
			break;
		}

		for (st->loop_idx = 0; st->loop_idx < st->num_returned;
		     st->loop_idx++) {
			ret = iops->process_response(ph, i->resp, st, i->priv);
			if (ret)
				goto out;
		}

		st->desc_index += st->num_returned;
		ph->xops->reset_rx_to_maxsz(ph, i->t);
		/*
		 * check for both returned and remaining to avoid infinite
		 * loop due to buggy firmware
		 */
	} while (st->num_returned && st->num_remaining);

out:
	/* Finalize and destroy iterator */
	ph->xops->xfer_put(ph, i->t);
	devm_kfree(ph->dev, i);

	return ret;
}

struct scmi_msg_get_fc_info {
	__le32 domain;
	__le32 message_id;
};

struct scmi_msg_resp_desc_fc {
	__le32 attr;
#define SUPPORTS_DOORBELL(x)		((x) & BIT(0))
#define DOORBELL_REG_WIDTH(x)		FIELD_GET(GENMASK(2, 1), (x))
	__le32 rate_limit;
	__le32 chan_addr_low;
	__le32 chan_addr_high;
	__le32 chan_size;
	__le32 db_addr_low;
	__le32 db_addr_high;
	__le32 db_set_lmask;
	__le32 db_set_hmask;
	__le32 db_preserve_lmask;
	__le32 db_preserve_hmask;
};

static void
scmi_common_fastchannel_init(const struct scmi_protocol_handle *ph,
			     u8 describe_id, u32 message_id, u32 valid_size,
			     u32 domain, void __iomem **p_addr,
			     struct scmi_fc_db_info **p_db)
{
	int ret;
	u32 flags;
	u64 phys_addr;
	u8 size;
	void __iomem *addr;
	struct scmi_xfer *t;
	struct scmi_fc_db_info *db = NULL;
	struct scmi_msg_get_fc_info *info;
	struct scmi_msg_resp_desc_fc *resp;
	const struct scmi_protocol_instance *pi = ph_to_pi(ph);

	if (!p_addr) {
		ret = -EINVAL;
		goto err_out;
	}

	ret = ph->xops->xfer_get_init(ph, describe_id,
				      sizeof(*info), sizeof(*resp), &t);
	if (ret)
		goto err_out;

	info = t->tx.buf;
	info->domain = cpu_to_le32(domain);
	info->message_id = cpu_to_le32(message_id);

	/*
	 * Bail out on error leaving fc_info addresses zeroed; this includes
	 * the case in which the requested domain/message_id does NOT support
	 * fastchannels at all.
	 */
	ret = ph->xops->do_xfer(ph, t);
	if (ret)
		goto err_xfer;

	resp = t->rx.buf;
	flags = le32_to_cpu(resp->attr);
	size = le32_to_cpu(resp->chan_size);
	if (size != valid_size) {
		ret = -EINVAL;
		goto err_xfer;
	}

	phys_addr = le32_to_cpu(resp->chan_addr_low);
	phys_addr |= (u64)le32_to_cpu(resp->chan_addr_high) << 32;
	addr = devm_ioremap(ph->dev, phys_addr, size);
	if (!addr) {
		ret = -EADDRNOTAVAIL;
		goto err_xfer;
	}

	*p_addr = addr;

	if (p_db && SUPPORTS_DOORBELL(flags)) {
		db = devm_kzalloc(ph->dev, sizeof(*db), GFP_KERNEL);
		if (!db) {
			ret = -ENOMEM;
			goto err_db;
		}

		size = 1 << DOORBELL_REG_WIDTH(flags);
		phys_addr = le32_to_cpu(resp->db_addr_low);
		phys_addr |= (u64)le32_to_cpu(resp->db_addr_high) << 32;
		addr = devm_ioremap(ph->dev, phys_addr, size);
		if (!addr) {
			ret = -EADDRNOTAVAIL;
			goto err_db_mem;
		}

		db->addr = addr;
		db->width = size;
		db->set = le32_to_cpu(resp->db_set_lmask);
		db->set |= (u64)le32_to_cpu(resp->db_set_hmask) << 32;
		db->mask = le32_to_cpu(resp->db_preserve_lmask);
		db->mask |= (u64)le32_to_cpu(resp->db_preserve_hmask) << 32;

		*p_db = db;
	}

	ph->xops->xfer_put(ph, t);

	dev_dbg(ph->dev,
		"Using valid FC for protocol %X [MSG_ID:%u / RES_ID:%u]\n",
		pi->proto->id, message_id, domain);

	return;

err_db_mem:
	devm_kfree(ph->dev, db);

err_db:
	*p_addr = NULL;

err_xfer:
	ph->xops->xfer_put(ph, t);

err_out:
	dev_warn(ph->dev,
		 "Failed to get FC for protocol %X [MSG_ID:%u / RES_ID:%u] - ret:%d. Using regular messaging.\n",
		 pi->proto->id, message_id, domain, ret);
}

#define SCMI_PROTO_FC_RING_DB(w)			\
do {							\
	u##w val = 0;					\
							\
	if (db->mask)					\
		val = ioread##w(db->addr) & db->mask;	\
	iowrite##w((u##w)db->set | val, db->addr);	\
} while (0)

static void scmi_common_fastchannel_db_ring(struct scmi_fc_db_info *db)
{
	if (!db || !db->addr)
		return;

	if (db->width == 1)
		SCMI_PROTO_FC_RING_DB(8);
	else if (db->width == 2)
		SCMI_PROTO_FC_RING_DB(16);
	else if (db->width == 4)
		SCMI_PROTO_FC_RING_DB(32);
	else /* db->width == 8 */
#ifdef CONFIG_64BIT
		SCMI_PROTO_FC_RING_DB(64);
#else
	{
		u64 val = 0;

		if (db->mask)
			val = ioread64_hi_lo(db->addr) & db->mask;
		iowrite64_hi_lo(db->set | val, db->addr);
	}
#endif
}

static const struct scmi_proto_helpers_ops helpers_ops = {
	.extended_name_get = scmi_common_extended_name_get,
	.iter_response_init = scmi_iterator_init,
	.iter_response_run = scmi_iterator_run,
	.fastchannel_init = scmi_common_fastchannel_init,
	.fastchannel_db_ring = scmi_common_fastchannel_db_ring,
};

/**
 * scmi_revision_area_get  - Retrieve version memory area.
 *
 * @ph: A reference to the protocol handle.
 *
 * A helper to grab the version memory area reference during SCMI Base protocol
 * initialization.
 *
 * Return: A reference to the version memory area associated to the SCMI
 *	   instance underlying this protocol handle.
 */
struct scmi_revision_info *
scmi_revision_area_get(const struct scmi_protocol_handle *ph)
{
	const struct scmi_protocol_instance *pi = ph_to_pi(ph);

	return pi->handle->version;
}

/**
 * scmi_alloc_init_protocol_instance  - Allocate and initialize a protocol
 * instance descriptor.
 * @info: The reference to the related SCMI instance.
 * @proto: The protocol descriptor.
 *
 * Allocate a new protocol instance descriptor, using the provided @proto
 * description, against the specified SCMI instance @info, and initialize it.
 *
 * Context: Assumes to be called with @protocols_mtx already acquired.
 * Return: A reference to a freshly allocated and initialized protocol instance
 *	   or ERR_PTR on failure.
 */
static struct scmi_protocol_instance *
scmi_alloc_init_protocol_instance(struct scmi_info *info,
				  const struct scmi_protocol *proto)
{
	int ret = -ENOMEM;
	struct scmi_protocol_instance *pi;
	const struct scmi_handle *handle = &info->handle;

	pi = devm_kzalloc(handle->dev, sizeof(*pi), GFP_KERNEL);
	if (!pi)
		goto clean;

	pi->proto = proto;
	pi->handle = handle;
	pi->ph.dev = handle->dev;
	pi->ph.xops = &xfer_ops;
	pi->ph.hops = &helpers_ops;
	pi->ph.set_priv = scmi_set_protocol_priv;
	pi->ph.get_priv = scmi_get_protocol_priv;
	refcount_set(&pi->users, 1);
	/* proto->init is assured NON NULL by scmi_protocol_register */
	ret = pi->proto->instance_init(&pi->ph);
	if (ret)
		goto clean;

	ret = idr_alloc(&info->protocols, pi, proto->id, proto->id + 1, GFP_KERNEL);
	if (ret != proto->id)
		goto clean;

	dev_dbg(handle->dev, "Initialized protocol: 0x%X\n", pi->proto->id);

	return pi;

clean:
	return ERR_PTR(ret);
}

/**
 * scmi_get_protocol_instance  - Protocol initialization helper.
 * @handle: A reference to the SCMI platform instance.
 * @protocol_id: The protocol being requested.
 *
 * In case the required protocol has never been requested before for this
 * instance, allocate and initialize all the needed structures.
 *
 * Return: A reference to an initialized protocol instance or error on failure:
 *	   in particular returns -EPROBE_DEFER when the desired protocol could
 *	   NOT be found.
 */
static struct scmi_protocol_instance * __must_check
scmi_get_protocol_instance(const struct scmi_handle *handle, u8 protocol_id)
{
	struct scmi_protocol_instance *pi;
	struct scmi_info *info = handle_to_scmi_info(handle);

	mutex_lock(&info->protocols_mtx);
	pi = idr_find(&info->protocols, protocol_id);

	if (pi) {
		refcount_inc(&pi->users);
	} else {
		const struct scmi_protocol *proto;

		/* Fails if protocol not registered on bus */
		proto = scmi_protocol_get(protocol_id);
		if (proto)
			pi = scmi_alloc_init_protocol_instance(info, proto);
		else
			pi = ERR_PTR(-EPROBE_DEFER);
	}
	mutex_unlock(&info->protocols_mtx);

	return pi;
}

/**
 * scmi_protocol_acquire  - Protocol acquire
 * @handle: A reference to the SCMI platform instance.
 * @protocol_id: The protocol being requested.
 *
 * Register a new user for the requested protocol on the specified SCMI
 * platform instance, possibly triggering its initialization on first user.
 *
 * Return: 0 if protocol was acquired successfully.
 */
int scmi_protocol_acquire(const struct scmi_handle *handle, u8 protocol_id)
{
	return PTR_ERR_OR_ZERO(scmi_get_protocol_instance(handle, protocol_id));
}

/**
 * scmi_protocol_release  - Protocol de-initialization helper.
 * @handle: A reference to the SCMI platform instance.
 * @protocol_id: The protocol being requested.
 *
 * Remove one user for the specified protocol and triggers de-initialization
 * and resources de-allocation once the last user has gone.
 */
void scmi_protocol_release(const struct scmi_handle *handle, u8 protocol_id)
{
	struct scmi_info *info = handle_to_scmi_info(handle);
	struct scmi_protocol_instance *pi;

	mutex_lock(&info->protocols_mtx);
	pi = idr_find(&info->protocols, protocol_id);
	if (WARN_ON(!pi))
		goto out;

	if (refcount_dec_and_test(&pi->users)) {
		if (pi->proto->instance_deinit)
			pi->proto->instance_deinit(&pi->ph);

		idr_remove(&info->protocols, protocol_id);

		dev_dbg(handle->dev, "De-Initialized protocol: 0x%X\n",
			protocol_id);
	}

out:
	mutex_unlock(&info->protocols_mtx);
}

void scmi_setup_protocol_implemented(const struct scmi_protocol_handle *ph,
				     u8 *prot_imp)
{
	const struct scmi_protocol_instance *pi = ph_to_pi(ph);
	struct scmi_info *info = handle_to_scmi_info(pi->handle);

	info->protocols_imp = prot_imp;
}

static bool
scmi_is_protocol_implemented(const struct scmi_handle *handle, u8 prot_id)
{
	int i;
	struct scmi_info *info = handle_to_scmi_info(handle);
	struct scmi_revision_info *rev = handle->version;

	if (!info->protocols_imp)
		return false;

	for (i = 0; i < rev->num_protocols; i++)
		if (info->protocols_imp[i] == prot_id)
			return true;
	return false;
}

static const void __must_check *
scmi_dev_protocol_get(struct scmi_device *sdev, u8 protocol_id,
		       struct scmi_protocol_handle **ph)
{
	struct scmi_protocol_instance *pi;

	if (!ph)
		return ERR_PTR(-EINVAL);

	pi = scmi_get_protocol_instance(sdev->handle, protocol_id);
	if (IS_ERR(pi))
		return ERR_CAST(pi);

	*ph = &pi->ph;

	return pi->proto->ops;
}

static int __must_check scmi_dev_protocol_acquire(struct scmi_device *sdev,
						   u8 protocol_id)
{
	return PTR_ERR_OR_ZERO(scmi_get_protocol_instance(sdev->handle, protocol_id));
}

static void scmi_dev_protocol_put(struct scmi_device *sdev, u8 protocol_id)
{
	scmi_protocol_release(sdev->handle, protocol_id);
}

/**
 * scmi_is_transport_atomic  - Method to check if underlying transport for an
 * SCMI instance is configured as atomic.
 *
 * @handle: A reference to the SCMI platform instance.
 * @atomic_threshold: An optional return value for the system wide currently
 *		      configured threshold for atomic operations.
 *
 * Return: True if transport is configured as atomic
 */
static bool scmi_is_transport_atomic(const struct scmi_handle *handle,
				     unsigned int *atomic_threshold)
{
	bool ret;
	struct scmi_info *info = handle_to_scmi_info(handle);

	ret = is_transport_polling_capable(info->desc);
	if (ret && atomic_threshold)
		*atomic_threshold = info->atomic_threshold;

	return ret;
}

static int __scmi_xfer_info_init(struct scmi_info *sinfo,
				 struct scmi_xfers_info *info)
{
	int i;
	struct scmi_xfer *xfer;
	struct device *dev = sinfo->dev;
	const struct scmi_desc *desc = sinfo->desc;

	/* Pre-allocated messages, no more than what hdr.seq can support */
	if (WARN_ON(!info->max_msg || info->max_msg > MSG_TOKEN_MAX)) {
		dev_err(dev,
			"Invalid maximum messages %d, not in range [1 - %lu]\n",
			info->max_msg, MSG_TOKEN_MAX);
		return -EINVAL;
	}

	/* Allocate a bitmask sized to hold MSG_TOKEN_MAX tokens */
	info->xfer_alloc_table = devm_bitmap_zalloc(dev, MSG_TOKEN_MAX,
						    GFP_KERNEL);
	if (!info->xfer_alloc_table)
		return -ENOMEM;

	/*
	 * Preallocate a number of xfers equal to max inflight messages,
	 * pre-initialize the buffer pointer to pre-allocated buffers and
	 * attach all of them to the free list
	 */
	INIT_HLIST_HEAD(&info->free_xfers);
	for (i = 0; i < info->max_msg; i++) {
		xfer = devm_kzalloc(dev, sizeof(*xfer), GFP_KERNEL);
		if (!xfer)
			return -ENOMEM;

		xfer->rx.buf = devm_kcalloc(dev, sizeof(u8), desc->max_msg_size,
					    GFP_KERNEL);
		if (!xfer->rx.buf)
			return -ENOMEM;

		xfer->tx.buf = xfer->rx.buf;
		spin_lock_init(&xfer->lock);

		/* Add initialized xfer to the free list */
		hlist_add_head(&xfer->node, &info->free_xfers);
	}

	spin_lock_init(&info->xfer_lock);

	return 0;
}

static int scmi_channels_max_msg_configure(struct scmi_info *sinfo)
{
	const struct scmi_desc *desc = sinfo->desc;

	if (!desc->ops->get_max_msg) {
		sinfo->tx_minfo.max_msg = desc->max_msg;
		sinfo->rx_minfo.max_msg = desc->max_msg;
	} else {
		struct scmi_chan_info *base_cinfo;

		base_cinfo = idr_find(&sinfo->tx_idr, SCMI_PROTOCOL_BASE);
		if (!base_cinfo)
			return -EINVAL;
		sinfo->tx_minfo.max_msg = desc->ops->get_max_msg(base_cinfo);

		/* RX channel is optional so can be skipped */
		base_cinfo = idr_find(&sinfo->rx_idr, SCMI_PROTOCOL_BASE);
		if (base_cinfo)
			sinfo->rx_minfo.max_msg =
				desc->ops->get_max_msg(base_cinfo);
	}

	return 0;
}

static int scmi_xfer_info_init(struct scmi_info *sinfo)
{
	int ret;

	ret = scmi_channels_max_msg_configure(sinfo);
	if (ret)
		return ret;

	ret = __scmi_xfer_info_init(sinfo, &sinfo->tx_minfo);
	if (!ret && !idr_is_empty(&sinfo->rx_idr))
		ret = __scmi_xfer_info_init(sinfo, &sinfo->rx_minfo);

	return ret;
}

/**
 * scmi_handle_get() - Get the SCMI handle for a device
 *
 * @dev: pointer to device for which we want SCMI handle
 *
 * NOTE: The function does not track individual clients of the framework
 * and is expected to be maintained by caller of SCMI protocol library.
 * scmi_handle_put must be balanced with successful scmi_handle_get
 *
 * Return: pointer to handle if successful, NULL on error
 */
struct scmi_handle *scmi_handle_get(struct device *dev)
{
	struct list_head *p;
	struct scmi_info *info;
	struct scmi_handle *handle = NULL;

	mutex_lock(&scmi_list_mutex);
	list_for_each(p, &scmi_list) {
		info = list_entry(p, struct scmi_info, node);
		if (dev->parent == info->dev) {
			handle = &info->handle;
			break;
		}
	}
	mutex_unlock(&scmi_list_mutex);

	return handle;
}

static int scmi_chan_setup(struct scmi_info *info, struct device_node *of_node,
			   int prot_id, bool tx)
{
	int ret, idx;
	char name[32];
	struct scmi_chan_info *cinfo;
	struct idr *idr;
	struct scmi_device *tdev = NULL;

	/* Transmit channel is first entry i.e. index 0 */
	idx = tx ? 0 : 1;
	idr = tx ? &info->tx_idr : &info->rx_idr;

	if (!info->desc->ops->chan_available(of_node, idx)) {
		cinfo = idr_find(idr, SCMI_PROTOCOL_BASE);
		if (unlikely(!cinfo)) /* Possible only if platform has no Rx */
			return -EINVAL;
		goto idr_alloc;
	}

	cinfo = devm_kzalloc(info->dev, sizeof(*cinfo), GFP_KERNEL);
	if (!cinfo)
		return -ENOMEM;

	cinfo->rx_timeout_ms = info->desc->max_rx_timeout_ms;

	/* Create a unique name for this transport device */
	snprintf(name, 32, "__scmi_transport_device_%s_%02X",
		 idx ? "rx" : "tx", prot_id);
	/* Create a uniquely named, dedicated transport device for this chan */
	tdev = scmi_device_create(of_node, info->dev, prot_id, name);
	if (!tdev) {
		dev_err(info->dev,
			"failed to create transport device (%s)\n", name);
		devm_kfree(info->dev, cinfo);
		return -EINVAL;
	}
	of_node_get(of_node);

	cinfo->id = prot_id;
	cinfo->dev = &tdev->dev;
	ret = info->desc->ops->chan_setup(cinfo, info->dev, tx);
	if (ret) {
		of_node_put(of_node);
		scmi_device_destroy(info->dev, prot_id, name);
		devm_kfree(info->dev, cinfo);
		return ret;
	}

	if (tx && is_polling_required(cinfo, info->desc)) {
		if (is_transport_polling_capable(info->desc))
			dev_dbg(&tdev->dev,
				"Enabled polling mode TX channel - prot_id:%d\n",
				prot_id);
		else
			dev_warn(&tdev->dev,
				 "Polling mode NOT supported by transport.\n");
	}

idr_alloc:
	ret = idr_alloc(idr, cinfo, prot_id, prot_id + 1, GFP_KERNEL);
	if (ret != prot_id) {
		dev_err(info->dev,
			"unable to allocate SCMI idr slot err %d\n", ret);
		/* Destroy channel and device only if created by this call. */
		if (tdev) {
			of_node_put(of_node);
			scmi_device_destroy(info->dev, prot_id, name);
			devm_kfree(info->dev, cinfo);
		}
		return ret;
	}

	cinfo->handle = &info->handle;
	return 0;
}

static inline int
scmi_txrx_setup(struct scmi_info *info, struct device_node *of_node,
		int prot_id)
{
	int ret = scmi_chan_setup(info, of_node, prot_id, true);

	if (!ret) {
		/* Rx is optional, report only memory errors */
		ret = scmi_chan_setup(info, of_node, prot_id, false);
		if (ret && ret != -ENOMEM)
			ret = 0;
	}

	return ret;
}

/**
 * scmi_channels_setup  - Helper to initialize all required channels
 *
 * @info: The SCMI instance descriptor.
 *
 * Initialize all the channels found described in the DT against the underlying
 * configured transport using custom defined dedicated devices instead of
 * borrowing devices from the SCMI drivers; this way channels are initialized
 * upfront during core SCMI stack probing and are no more coupled with SCMI
 * devices used by SCMI drivers.
 *
 * Note that, even though a pair of TX/RX channels is associated to each
 * protocol defined in the DT, a distinct freshly initialized channel is
 * created only if the DT node for the protocol at hand describes a dedicated
 * channel: in all the other cases the common BASE protocol channel is reused.
 *
 * Return: 0 on Success
 */
static int scmi_channels_setup(struct scmi_info *info)
{
	int ret;
	struct device_node *child, *top_np = info->dev->of_node;

	/* Initialize a common generic channel at first */
	ret = scmi_txrx_setup(info, top_np, SCMI_PROTOCOL_BASE);
	if (ret)
		return ret;

	for_each_available_child_of_node(top_np, child) {
		u32 prot_id;

		if (of_property_read_u32(child, "reg", &prot_id))
			continue;

		if (!FIELD_FIT(MSG_PROTOCOL_ID_MASK, prot_id))
			dev_err(info->dev,
				"Out of range protocol %d\n", prot_id);

		ret = scmi_txrx_setup(info, child, prot_id);
		if (ret) {
			of_node_put(child);
			return ret;
		}
	}

	return 0;
}

static int scmi_chan_destroy(int id, void *p, void *idr)
{
	struct scmi_chan_info *cinfo = p;

	if (cinfo->dev) {
		struct scmi_info *info = handle_to_scmi_info(cinfo->handle);
		struct scmi_device *sdev = to_scmi_dev(cinfo->dev);

		of_node_put(cinfo->dev->of_node);
		scmi_device_destroy(info->dev, id, sdev->name);
		cinfo->dev = NULL;
	}

	idr_remove(idr, id);

	return 0;
}

static void scmi_cleanup_channels(struct scmi_info *info, struct idr *idr)
{
	/* At first free all channels at the transport layer ... */
	idr_for_each(idr, info->desc->ops->chan_free, idr);

	/* ...then destroy all underlying devices */
	idr_for_each(idr, scmi_chan_destroy, idr);

	idr_destroy(idr);
}

static void scmi_cleanup_txrx_channels(struct scmi_info *info)
{
	scmi_cleanup_channels(info, &info->tx_idr);

	scmi_cleanup_channels(info, &info->rx_idr);
}

static int scmi_device_request_notifier(struct notifier_block *nb,
					unsigned long action, void *data)
{
	struct device_node *np;
	struct scmi_device_id *id_table = data;
	struct scmi_info *info = req_nb_to_scmi_info(nb);

	np = idr_find(&info->active_protocols, id_table->protocol_id);
	if (!np)
		return NOTIFY_DONE;

	dev_dbg(info->dev, "%sRequested device (%s) for protocol 0x%x\n",
		action == SCMI_BUS_NOTIFY_DEVICE_REQUEST ? "" : "UN-",
		id_table->name, id_table->protocol_id);

	switch (action) {
	case SCMI_BUS_NOTIFY_DEVICE_REQUEST:
		scmi_create_protocol_devices(np, info, id_table->protocol_id,
					     id_table->name);
		break;
	case SCMI_BUS_NOTIFY_DEVICE_UNREQUEST:
		scmi_destroy_protocol_devices(info, id_table->protocol_id,
					      id_table->name);
		break;
	default:
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}

static int scmi_probe(struct device *dev)
{
	int ret;
	struct scmi_handle *handle;
	const struct scmi_desc *desc;
	struct scmi_info *info;
	struct device_node *child, *np = dev->of_node;

	desc = of_device_get_match_data(dev);
	if (!desc)
		return -EINVAL;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = dev;
	info->desc = desc;
	info->dev_req_nb.notifier_call = scmi_device_request_notifier;
	INIT_LIST_HEAD(&info->node);
	idr_init(&info->protocols);
	mutex_init(&info->protocols_mtx);
	idr_init(&info->active_protocols);
	mutex_init(&info->devreq_mtx);

	dev->priv = info;
	idr_init(&info->tx_idr);
	idr_init(&info->rx_idr);

	handle = &info->handle;
	handle->dev = info->dev;
	handle->version = &info->version;
	handle->dev_protocol_acquire = scmi_dev_protocol_acquire;
	handle->dev_protocol_get = scmi_dev_protocol_get;
	handle->dev_protocol_put = scmi_dev_protocol_put;

	/* System wide atomic threshold for atomic ops .. if any */
	if (!of_property_read_u32(np, "atomic-threshold-us",
				  &info->atomic_threshold))
		dev_info(dev,
			 "SCMI System wide atomic threshold set to %d us\n",
			 info->atomic_threshold);
	handle->is_transport_atomic = scmi_is_transport_atomic;

	if (desc->ops->link_supplier) {
		ret = desc->ops->link_supplier(dev);
		if (ret)
			goto clear_ida;
	}

	/* Setup all channels described in the DT at first */
	ret = scmi_channels_setup(info);
	if (ret)
		goto clear_ida;

	ret = blocking_notifier_chain_register(&scmi_requested_devices_nh,
					       &info->dev_req_nb);
	if (ret)
		goto clear_txrx_setup;

	ret = scmi_xfer_info_init(info);
	if (ret)
		goto clear_dev_req_notifier;

	if (!is_transport_polling_capable(info->desc))
		dev_err(dev,
			"Transport is not polling capable. Atomic mode not supported.\n");

	/*
	 * Trigger SCMI Base protocol initialization.
	 * It's mandatory and won't be ever released/deinit.
	 */
	ret = scmi_protocol_acquire(handle, SCMI_PROTOCOL_BASE);
	if (ret) {
		dev_err(dev, "unable to communicate with SCMI\n");
		goto notification_exit;
	}

	mutex_lock(&scmi_list_mutex);
	list_add_tail(&info->node, &scmi_list);
	mutex_unlock(&scmi_list_mutex);

	for_each_available_child_of_node(np, child) {
		u32 prot_id;

		if (of_property_read_u32(child, "reg", &prot_id))
			continue;

		if (!FIELD_FIT(MSG_PROTOCOL_ID_MASK, prot_id))
			dev_err(dev, "Out of range protocol %d\n", prot_id);

		if (!scmi_is_protocol_implemented(handle, prot_id)) {
			dev_err(dev, "SCMI protocol %d not implemented\n",
				prot_id);
			continue;
		}

		/*
		 * Save this valid DT protocol descriptor amongst
		 * @active_protocols for this SCMI instance/
		 */
		ret = idr_alloc(&info->active_protocols, child,
				prot_id, prot_id + 1, GFP_KERNEL);
		if (ret != prot_id) {
			dev_err(dev, "SCMI protocol %d already activated. Skip\n",
				prot_id);
			continue;
		}

		of_node_get(child);
		scmi_create_protocol_devices(child, info, prot_id, NULL);
	}

	return 0;

notification_exit:
clear_dev_req_notifier:
	blocking_notifier_chain_unregister(&scmi_requested_devices_nh,
					   &info->dev_req_nb);
clear_txrx_setup:
	scmi_cleanup_txrx_channels(info);
clear_ida:
	return ret;
}

/* Each compatible listed below must have descriptor associated with it */
static const struct of_device_id scmi_of_match[] = {
#ifdef CONFIG_ARM_SCMI_TRANSPORT_OPTEE
	{ .compatible = "linaro,scmi-optee", .data = &scmi_optee_desc },
#endif
#ifdef CONFIG_ARM_SCMI_TRANSPORT_SMC
	{ .compatible = "arm,scmi-smc", .data = &scmi_smc_desc},
	{ .compatible = "arm,scmi-smc-param", .data = &scmi_smc_desc},
#endif
	{ /* Sentinel */ },
};

MODULE_DEVICE_TABLE(of, scmi_of_match);

static struct driver arm_scmi_driver = {
	.name = "arm-scmi",
	.of_match_table = scmi_of_match,
	.probe = scmi_probe,
};
core_platform_driver(arm_scmi_driver);

static int __init scmi_bus_driver_init(void)
{
	int ret;

	ret = scmi_bus_init();
	if (ret)
		return ret;

	/* Bail out if no SCMI transport was configured */
	if (WARN_ON(!IS_ENABLED(CONFIG_ARM_SCMI_HAVE_TRANSPORT)))
		return -EINVAL;

	scmi_base_register();

	scmi_clock_register();
	scmi_power_register();
	scmi_reset_register();
	scmi_sensors_register();
	scmi_voltage_register();

	return 0;
}
pure_initcall(scmi_bus_driver_init);

MODULE_ALIAS("platform:arm-scmi");
MODULE_AUTHOR("Sudeep Holla <sudeep.holla@arm.com>");
MODULE_DESCRIPTION("ARM SCMI protocol driver");
MODULE_LICENSE("GPL v2");
