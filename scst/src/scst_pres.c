/*
 *  scst_pres.c
 *
 *  Copyright (C) 2009 - 2010 Alexey Obitotskiy <alexeyo1@open-e.com>
 *  Copyright (C) 2009 - 2010 Open-E, Inc.
 *  Copyright (C) 2009 - 2018 Vladislav Bolkhovitin <vst@vlnb.net>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation, version 2
 *  of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/unistd.h>
#include <linux/string.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/ctype.h>
#include <asm/byteorder.h>
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <linux/uaccess.h>
#include <linux/namei.h>
#ifndef INSIDE_KERNEL_TREE
#include <linux/version.h>
#endif
#include <linux/vmalloc.h>
#include <asm/unaligned.h>

#ifdef INSIDE_KERNEL_TREE
#include <scst/scst.h>
#include <scst/scst_const.h>
#else
#include "scst.h"
#include "scst_const.h"
#endif
#include "scst_priv.h"
#include "scst_pres.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0) &&		\
	(!defined(RHEL_RELEASE_CODE) ||				\
	 RHEL_RELEASE_CODE -0 < RHEL_RELEASE_VERSION(9, 1))
#include <stdarg.h>
#else
#include <linux/stdarg.h>
#endif

#define SCST_PR_ROOT_ENTRY	"pr"
#define SCST_PR_FILE_SIGN	0xBBEEEEAAEEBBDD77LLU
#define SCST_PR_FILE_VERSION	1LLU

#define FILE_BUFFER_SIZE	512

#ifndef isblank
#define isblank(c)		((c) == ' ' || (c) == '\t')
#endif

#if defined(CONFIG_LOCKDEP)
#define scst_assert_pr_mutex_held(dev)					\
	do {								\
		if (dev->dev_list_entry.next &&				\
		    !list_empty(&dev->dev_list_entry))			\
			lockdep_assert_held(&dev->dev_pr_mutex);	\
	} while (0)
#else
static inline void scst_assert_pr_mutex_held(struct scst_device *dev)
{
}
#endif

uint32_t scst_tid_size(const uint8_t *tid)
{
	sBUG_ON(tid == NULL);

	if ((tid[0] & 0x0f) == SCSI_TRANSPORTID_PROTOCOLID_ISCSI)
		return get_unaligned_be16(&tid[2]) + 4;
	else
		return TID_COMMON_SIZE;
}

/* Secures transport ID by setting 0 in the last byte of ASCII transport IDs */
static bool tid_secure(uint8_t *tid, unsigned int buf_size)
{
	if ((tid[0] & 0x0f) == SCSI_TRANSPORTID_PROTOCOLID_ISCSI) {
		const uint32_t size = scst_tid_size(tid);

		if (size > buf_size)
			return false;
		tid[size - 1] = '\0';
	}

	return true;
}

/* Returns false if tid's are not equal, true otherwise */
bool tid_equal(const uint8_t *tid_a, const uint8_t *tid_b)
{
	int len;

	if (tid_a == NULL || tid_b == NULL)
		return false;

	if ((tid_a[0] & 0x0f) != (tid_b[0] & 0x0f)) {
		TRACE_DBG("%s", "Different protocol IDs");
		return false;
	}

	if ((tid_a[0] & 0x0f) == SCSI_TRANSPORTID_PROTOCOLID_ISCSI) {
		const uint8_t tid_a_fmt = tid_a[0] & 0xc0;
		const uint8_t tid_b_fmt = tid_b[0] & 0xc0;
		uint32_t tid_a_len, tid_a_max = scst_tid_size(tid_a) - 4;
		uint32_t tid_b_len, tid_b_max = scst_tid_size(tid_b) - 4;
		int i;

		tid_a += 4;
		tid_b += 4;

		if (tid_a_fmt == 0x00)
			tid_a_len = strnlen(tid_a, tid_a_max);
		else if (tid_a_fmt == 0x40) {
			if (tid_a_fmt != tid_b_fmt) {
				uint8_t *p = strnchr(tid_a, tid_a_max, ',');

				if (p == NULL)
					goto out_error;
				tid_a_len = p - tid_a;

				sBUG_ON(tid_a_len > tid_a_max);
			} else
				tid_a_len = strnlen(tid_a, tid_a_max);
		} else
			goto out_error;

		if (tid_b_fmt == 0x00)
			tid_b_len = strnlen(tid_b, tid_b_max);
		else if (tid_b_fmt == 0x40) {
			if (tid_a_fmt != tid_b_fmt) {
				uint8_t *p = strnchr(tid_b, tid_b_max, ',');

				if (p == NULL)
					goto out_error;
				tid_b_len = p - tid_b;

				sBUG_ON(tid_b_len > tid_b_max);
			} else
				tid_b_len = strnlen(tid_b, tid_b_max);
		} else
			goto out_error;

		if (tid_a_len != tid_b_len)
			return false;

		len = tid_a_len;

		/* ISCSI names are case insensitive */
		for (i = 0; i < len; i++)
			if (tolower(tid_a[i]) != tolower(tid_b[i]))
				return false;
		return true;
	}

	len = TID_COMMON_SIZE;

	return memcmp(tid_a, tid_b, len) == 0;

out_error:
	PRINT_ERROR("%s", "Invalid initiator port transport id");
	return false;
}

/* Must be called under dev_pr_mutex */
void scst_pr_set_holder(struct scst_device *dev,
	struct scst_dev_registrant *holder, uint8_t scope, uint8_t type)
{
	scst_assert_pr_mutex_held(dev);

	dev->pr_is_set = 1;
	dev->pr_scope = scope;
	dev->pr_type = type;
	if (dev->pr_type != TYPE_EXCLUSIVE_ACCESS_ALL_REG &&
	    dev->pr_type != TYPE_WRITE_EXCLUSIVE_ALL_REG)
		dev->pr_holder = holder;
}

/* Must be called under dev_pr_mutex */
static bool scst_pr_is_holder(struct scst_device *dev,
	struct scst_dev_registrant *reg)
{
	bool res = false;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	if (!dev->pr_is_set)
		goto out;

	if (dev->pr_type == TYPE_EXCLUSIVE_ACCESS_ALL_REG ||
	    dev->pr_type == TYPE_WRITE_EXCLUSIVE_ALL_REG) {
		res = (reg != NULL);
	} else
		res = (dev->pr_holder == reg);

out:
	TRACE_EXIT_RES(res);
	return res;
}

#if defined(CONFIG_SCST_DEBUG) || defined(CONFIG_SCST_TRACING)

/* Must be called under dev_pr_mutex */
void scst_pr_dump_prs(struct scst_device *dev, bool force)
{
	scst_assert_pr_mutex_held(dev);

	if (!force) {
#if defined(CONFIG_SCST_DEBUG)
		if ((trace_flag & TRACE_PRES) == 0)
#endif
			goto out;
	}

	PRINT_INFO("Persistent reservations for device %s:", dev->virt_name);

	if (list_empty(&dev->dev_registrants_list))
		PRINT_INFO("%s", "  No registrants");
	else {
		struct scst_dev_registrant *reg;
		int i = 0;

		list_for_each_entry(reg, &dev->dev_registrants_list,
					dev_registrants_list_entry) {
			PRINT_INFO("  [%d] registrant %s/%d, key %016llx "
				"(reg %p, tgt_dev %p)", i++,
				debug_transport_id_to_initiator_name(
					reg->transport_id),
				reg->rel_tgt_id, be64_to_cpu(reg->key), reg,
				reg->tgt_dev);
		}
	}

	if (dev->pr_is_set) {
		struct scst_dev_registrant *holder = dev->pr_holder;

		if (holder != NULL)
			PRINT_INFO("Reservation holder is %s/%d (key %016llx, "
				"scope %x, type %x, reg %p, tgt_dev %p)",
				debug_transport_id_to_initiator_name(
							holder->transport_id),
				holder->rel_tgt_id, be64_to_cpu(holder->key),
				dev->pr_scope, dev->pr_type, holder,
				holder->tgt_dev);
		else
			PRINT_INFO("All registrants are reservation holders "
				"(scope %x, type %x)", dev->pr_scope,
				dev->pr_type);
	} else
		PRINT_INFO("%s", "Not reserved");

out:
	return;
}

#endif /* defined(CONFIG_SCST_DEBUG) || defined(CONFIG_SCST_TRACING) */

/* dev_pr_mutex must be locked */
static void scst_pr_find_registrants_list_all(struct scst_device *dev,
	struct scst_dev_registrant *exclude_reg, struct list_head *list)
{
	struct scst_dev_registrant *reg;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	TRACE_PR("Finding all registered records for device '%s' "
		"with exclude reg key %016llx",
		dev->virt_name, be64_to_cpu(exclude_reg->key));

	list_for_each_entry(reg, &dev->dev_registrants_list,
				dev_registrants_list_entry) {
		if (reg == exclude_reg)
			continue;
		TRACE_PR("Adding registrant %s/%d (%p) to find list (key %016llx)",
			debug_transport_id_to_initiator_name(reg->transport_id),
			reg->rel_tgt_id, reg, be64_to_cpu(reg->key));
		list_add_tail(&reg->aux_list_entry, list);
	}

	TRACE_EXIT();
	return;
}

/* dev_pr_mutex must be locked */
static void scst_pr_find_registrants_list_key(struct scst_device *dev,
	__be64 key, struct list_head *list)
{
	struct scst_dev_registrant *reg;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	TRACE_PR("Finding registrants for device '%s' with key %016llx",
		dev->virt_name, be64_to_cpu(key));

	list_for_each_entry(reg, &dev->dev_registrants_list,
				dev_registrants_list_entry) {
		if (reg->key == key) {
			TRACE_PR("Adding registrant %s/%d (%p) to the find "
				"list (key %016llx)",
				debug_transport_id_to_initiator_name(
					reg->transport_id),
				reg->rel_tgt_id, reg->tgt_dev,
				be64_to_cpu(key));
			list_add_tail(&reg->aux_list_entry, list);
		}
	}

	TRACE_EXIT();
	return;
}

/* dev_pr_mutex must be locked */
struct scst_dev_registrant *scst_pr_find_reg(
	struct scst_device *dev, const uint8_t *transport_id,
	const uint16_t rel_tgt_id)
{
	struct scst_dev_registrant *reg, *res = NULL;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	list_for_each_entry(reg, &dev->dev_registrants_list,
				dev_registrants_list_entry) {
		if ((reg->rel_tgt_id == rel_tgt_id) &&
		    tid_equal(reg->transport_id, transport_id)) {
			res = reg;
			break;
		}
	}

	TRACE_EXIT_HRES(res);
	return res;
}

/* Must be called under dev_pr_mutex */
static void scst_pr_clear_reservation(struct scst_device *dev)
{
	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	WARN_ON(!dev->pr_is_set);

	dev->pr_is_set = 0;
	dev->pr_scope = SCOPE_LU;
	dev->pr_type = TYPE_UNSPECIFIED;

	dev->pr_holder = NULL;

	TRACE_EXIT();
	return;
}

/* Must be called under dev_pr_mutex */
void scst_pr_clear_holder(struct scst_device *dev)
{
	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	WARN_ON(!dev->pr_is_set);

	if (dev->pr_type == TYPE_WRITE_EXCLUSIVE_ALL_REG ||
	    dev->pr_type == TYPE_EXCLUSIVE_ACCESS_ALL_REG) {
		if (list_empty(&dev->dev_registrants_list))
			scst_pr_clear_reservation(dev);
	} else
		scst_pr_clear_reservation(dev);

	dev->pr_holder = NULL;

	TRACE_EXIT();
	return;
}

/* Must be called under dev_pr_mutex */
struct scst_dev_registrant *scst_pr_add_registrant(
	struct scst_device *dev, const uint8_t *transport_id,
	const uint16_t rel_tgt_id, __be64 key,
	bool dev_lock_locked)
{
	struct scst_dev_registrant *reg;
	struct scst_tgt_dev *t;
	gfp_t gfp_flags = dev_lock_locked ? GFP_ATOMIC : GFP_KERNEL;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	sBUG_ON(dev == NULL);
	sBUG_ON(transport_id == NULL);

	TRACE_PR("Registering %s/%d (dev %s)",
		debug_transport_id_to_initiator_name(transport_id),
		rel_tgt_id, dev->virt_name);

	reg = scst_pr_find_reg(dev, transport_id, rel_tgt_id);
	if (reg != NULL) {
		/*
		 * It might happen when a target driver would make >1 session
		 * from the same initiator to the same target.
		 */
		PRINT_ERROR("Registrant %p/%d (dev %s) already exists!", reg,
			rel_tgt_id, dev->virt_name);
		PRINT_BUFFER("TransportID", transport_id, 24);
		WARN_ON(1);
		reg = NULL;
		goto out;
	}

	reg = kzalloc(sizeof(*reg), gfp_flags);
	if (reg == NULL) {
		PRINT_ERROR("%s", "Unable to allocate registration record");
		goto out;
	}

	dev->cl_ops->pr_init_reg(dev, reg);

	reg->transport_id = kmemdup(transport_id, scst_tid_size(transport_id),
				    gfp_flags);
	if (reg->transport_id == NULL) {
		PRINT_ERROR("%s", "Unable to allocate initiator port "
			"transport id");
		goto out_free;
	}

	reg->rel_tgt_id = rel_tgt_id;
	reg->key = key;

	/*
	 * We can't use scst_mutex here, because of the circular
	 * locking dependency with dev_pr_mutex.
	 */
#if !defined(__CHECKER__)
	if (!dev_lock_locked)
		spin_lock_bh(&dev->dev_lock);
#endif
	list_for_each_entry(t, &dev->dev_tgt_dev_list, dev_tgt_dev_list_entry) {
		if (tid_equal(t->sess->transport_id, transport_id) &&
		    (t->sess->tgt->rel_tgt_id == rel_tgt_id) &&
		    (t->registrant == NULL)) {
			/*
			 * We must assign here, because t can die
			 * immediately after we release dev_lock.
			 */
			TRACE_PR("Found tgt_dev %p", t);
			reg->tgt_dev = t;
			t->registrant = reg;
			break;
		}
	}
#if !defined(__CHECKER__)
	if (!dev_lock_locked)
		spin_unlock_bh(&dev->dev_lock);
#endif

	list_add_tail(&reg->dev_registrants_list_entry,
		&dev->dev_registrants_list);

	TRACE_PR("Reg %p registered (dev %s, tgt_dev %p)", reg,
		dev->virt_name, reg->tgt_dev);

out:
	TRACE_EXIT_HRES((unsigned long)reg);
	return reg;

out_free:
	kfree(reg);
	reg = NULL;
	goto out;
}

/* Must be called under dev_pr_mutex */
void scst_pr_remove_registrant(struct scst_device *dev,
	struct scst_dev_registrant *reg)
{
	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	TRACE_PR("Removing registrant %s/%d (reg %p, tgt_dev %p, key %016llx, "
		"dev %s)", debug_transport_id_to_initiator_name(reg->transport_id),
		reg->rel_tgt_id, reg, reg->tgt_dev, be64_to_cpu(reg->key),
		dev->virt_name);

	list_del(&reg->dev_registrants_list_entry);

	dev->cl_ops->pr_rm_reg(dev, reg);

	if (scst_pr_is_holder(dev, reg))
		scst_pr_clear_holder(dev);

	if (reg->tgt_dev)
		reg->tgt_dev->registrant = NULL;

	kfree(reg->transport_id);
	kfree(reg);

	TRACE_EXIT();
	return;
}

static void scst_pr_remove_registrants(struct scst_device *dev)
{
	struct scst_dev_registrant *reg, *tmp_reg;

	list_for_each_entry_safe(reg, tmp_reg, &dev->dev_registrants_list,
				 dev_registrants_list_entry) {
		scst_pr_remove_registrant(dev, reg);
	}
}

/* Must be called under dev_pr_mutex */
static void scst_pr_send_ua_reg(struct scst_device *dev,
	struct scst_dev_registrant *reg,
	int key, int asc, int ascq)
{
	static uint8_t ua[SCST_STANDARD_SENSE_LEN];

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	scst_set_sense(ua, sizeof(ua), dev->d_sense, key, asc, ascq);

	TRACE_PR("Queueing UA [%x %x %x]: registrant %s/%d (%p), tgt_dev %p, "
		"key %016llx", ua[2], ua[12], ua[13],
		debug_transport_id_to_initiator_name(reg->transport_id),
		reg->rel_tgt_id, reg, reg->tgt_dev, be64_to_cpu(reg->key));

	if (reg->tgt_dev)
		scst_check_set_UA(reg->tgt_dev, ua, sizeof(ua), 0);

	TRACE_EXIT();
	return;
}

/* Must be called under dev_pr_mutex */
static void scst_pr_send_ua_all(struct scst_device *dev,
	struct scst_dev_registrant *exclude_reg,
	int key, int asc, int ascq)
{
	struct scst_dev_registrant *reg;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	list_for_each_entry(reg, &dev->dev_registrants_list,
				dev_registrants_list_entry) {
		if (reg != exclude_reg)
			scst_pr_send_ua_reg(dev, reg, key, asc, ascq);
	}

	TRACE_EXIT();
	return;
}

/* Must be called under dev_pr_mutex */
static void scst_pr_abort_reg(struct scst_device *dev,
	struct scst_cmd *pr_cmd, struct scst_dev_registrant *reg)
{
	struct scst_session *sess;
	__be64 packed_lun;
	int rc;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	if (reg->tgt_dev == NULL) {
		TRACE_PR("Registrant %s/%d (%p, key 0x%016llx) has no session",
			debug_transport_id_to_initiator_name(reg->transport_id),
			reg->rel_tgt_id, reg, be64_to_cpu(reg->key));
		goto out;
	}

	sess = reg->tgt_dev->sess;

	TRACE_PR("Aborting %d commands for %s/%d (reg %p, key 0x%016llx, "
		"tgt_dev %p, sess %p)",
		atomic_read(&reg->tgt_dev->tgt_dev_cmd_count),
		debug_transport_id_to_initiator_name(reg->transport_id),
		reg->rel_tgt_id, reg, be64_to_cpu(reg->key), reg->tgt_dev,
		sess);

	packed_lun = scst_pack_lun(reg->tgt_dev->lun, sess->acg->addr_method);

	rc = scst_rx_mgmt_fn_lun(sess, SCST_PR_ABORT_ALL,
		&packed_lun, sizeof(packed_lun), SCST_NON_ATOMIC,
		pr_cmd);
	if (rc != 0) {
		/*
		 * There's nothing more we can do here... Hopefully, it would
		 * never happen.
		 */
		PRINT_ERROR("SCST_PR_ABORT_ALL failed %d (sess %p)",
			rc, sess);
		goto out;
	}

	if ((reg->tgt_dev != pr_cmd->tgt_dev) && !dev->tas) {
		uint8_t sense_buffer[SCST_STANDARD_SENSE_LEN];
		int sl;

		sl = scst_set_sense(sense_buffer, sizeof(sense_buffer),
			dev->d_sense,
			SCST_LOAD_SENSE(scst_sense_cleared_by_another_ini_UA));
		/*
		 * Potentially, setting UA here, when the aborted commands are
		 * still running, can lead to a situation that one of them could
		 * take it, then that would be detected and the UA requeued.
		 * But, meanwhile, one or more subsequent, i.e. not aborted,
		 * commands can "leak" executed normally. So, as result, the
		 * UA would be delivered one or more commands "later". However,
		 * that should be OK, because, if multiple commands are being
		 * executed in parallel, you can't control exact order of UA
		 * delivery anyway.
		 */
		scst_check_set_UA(reg->tgt_dev, sense_buffer, sl, 0);
	}

out:
	TRACE_EXIT();
	return;
}


/* Called under scst_mutex */
static int scst_pr_do_load_device_file(struct scst_device *dev,
	const char *file_name)
{
	int res = 0, rc;
	struct file *file = NULL;
	char *buf = NULL;
	loff_t file_size, pos, data_size;
	uint64_t sign, version;
	uint8_t pr_is_set, aptpl;
	__be64 key;
	uint16_t rel_tgt_id;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	scst_pr_remove_registrants(dev);

	TRACE_PR("Loading persistent file '%s'", file_name);

	file_size = scst_file_or_bdev_size(file_name);
	if (file_size < 0) {
		res = file_size;
		goto out;
	}

	file = filp_open(file_name, O_RDONLY, 0);
	if (IS_ERR(file)) {
		res = PTR_ERR(file);
		TRACE_PR("Unable to open file '%s' - error %d", file_name, res);
		goto out;
	}

	/* Let's limit the file size by some reasonable number */
	if ((file_size == 0) || (file_size >= 15*1024*1024)) {
		PRINT_ERROR("Invalid PR file size %d", (int)file_size);
		res = -EINVAL;
		goto out_close;
	}

	buf = vmalloc(file_size);
	if (buf == NULL) {
		res = -ENOMEM;
		PRINT_ERROR("%s", "Unable to allocate buffer");
		goto out_close;
	}

	pos = 0;
	rc = kernel_read(file, buf, file_size, &pos);
	if (rc != file_size) {
		PRINT_ERROR("Unable to read file '%s' - error %d", file_name,
			rc);
		res = rc;
		goto out_close;
	}

	data_size = 0;
	data_size += sizeof(sign);
	data_size += sizeof(version);
	data_size += sizeof(aptpl);
	data_size += sizeof(pr_is_set);
	data_size += sizeof(dev->pr_type);
	data_size += sizeof(dev->pr_scope);

	if (file_size < data_size) {
		res = -EINVAL;
		PRINT_ERROR("Invalid file '%s' - size too small", file_name);
		goto out_close;
	}

	pos = 0;

	sign = get_unaligned((uint64_t *)&buf[pos]);
	if (sign != SCST_PR_FILE_SIGN) {
		res = -EINVAL;
		PRINT_ERROR("Invalid persistent file signature %016llx "
			"(expected %016llx)", sign, SCST_PR_FILE_SIGN);
		goto out_close;
	}
	pos += sizeof(sign);

	version = get_unaligned((uint64_t *)&buf[pos]);
	if (version != SCST_PR_FILE_VERSION) {
		res = -EINVAL;
		PRINT_ERROR("Invalid persistent file version %016llx "
			"(expected %016llx)", version, SCST_PR_FILE_VERSION);
		goto out_close;
	}
	pos += sizeof(version);

	while (data_size + 1 < file_size) {
		uint8_t *tid;

		data_size++;
		tid = &buf[data_size];
		data_size += scst_tid_size(tid);
		data_size += sizeof(key);
		data_size += sizeof(rel_tgt_id);

		if (data_size > file_size) {
			res = -EINVAL;
			PRINT_ERROR("Invalid file '%s' - size mismatch have "
				"%lld expected %lld", file_name, file_size,
				data_size);
			goto out_close;
		}
	}

	aptpl = buf[pos];
	dev->pr_aptpl = aptpl ? 1 : 0;
	pos += sizeof(aptpl);

	pr_is_set = buf[pos];
	dev->pr_is_set = pr_is_set ? 1 : 0;
	pos += sizeof(pr_is_set);

	dev->pr_type = buf[pos];
	pos += sizeof(dev->pr_type);

	dev->pr_scope = buf[pos];
	pos += sizeof(dev->pr_scope);

	while (pos + 1 < file_size) {
		uint8_t is_holder;
		uint8_t *tid;
		struct scst_dev_registrant *reg = NULL;

		is_holder = buf[pos++];

		tid = &buf[pos];
		pos += scst_tid_size(tid);

		key = get_unaligned((__be64 *)&buf[pos]);
		pos += sizeof(key);

		rel_tgt_id = get_unaligned((uint16_t *)&buf[pos]);
		pos += sizeof(rel_tgt_id);

		reg = scst_pr_add_registrant(dev, tid, rel_tgt_id, key, false);
		if (reg == NULL) {
			res = -ENOMEM;
			goto out_close;
		}

		if (is_holder)
			dev->pr_holder = reg;
	}

out_close:
	filp_close(file, NULL);

out:
	if (buf != NULL)
		vfree(buf);

	TRACE_EXIT_RES(res);
	return res;
}

static int scst_pr_load_device_file(struct scst_device *dev)
{
	int res, rc;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	if (dev->pr_file_name == NULL || dev->pr_file_name1 == NULL) {
		PRINT_ERROR("Invalid file paths for '%s'", dev->virt_name);
		res = -EINVAL;
		goto out;
	}

	res = scst_pr_do_load_device_file(dev, dev->pr_file_name);
	if (res == 0)
		goto out_dump;
	else if (res == -ENOMEM)
		goto out;

	rc = res;

	res = scst_pr_do_load_device_file(dev, dev->pr_file_name1);
	if (res != 0) {
		if (res == -ENOENT)
			res = rc;
		goto out;
	}

out_dump:
	scst_pr_dump_prs(dev, false);

out:
	TRACE_EXIT_RES(res);
	return res;
}

static void scst_pr_remove_device_files(struct scst_device *dev)
{
	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	if (dev->pr_file_name)
		scst_remove_file(dev->pr_file_name);
	if (dev->pr_file_name1)
		scst_remove_file(dev->pr_file_name1);

	TRACE_EXIT();
	return;
}

/* Must be called under dev_pr_mutex */
void scst_pr_sync_device_file(struct scst_device *dev)
{
	int res = 0;
	struct file *file;
	loff_t pos = 0;
	uint64_t sign;
	uint64_t version;
	uint8_t pr_is_set, aptpl;
	struct scst_dev_registrant *reg;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	if ((dev->pr_aptpl == 0) || list_empty(&dev->dev_registrants_list)) {
		scst_pr_remove_device_files(dev);
		goto out;
	}

	scst_copy_file(dev->pr_file_name, dev->pr_file_name1);

	file = filp_open(dev->pr_file_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(file)) {
		res = PTR_ERR(file);
		PRINT_ERROR("Unable to (re)create PR file '%s' - error %d",
			dev->pr_file_name, res);
		goto out;
	}

	TRACE_PR("Updating pr file '%s'", dev->pr_file_name);

	/*
	 * signature
	 */
	sign = 0;
	pos = 0;
	res = kernel_write(file, &sign, sizeof(sign), &pos);
	if (res != sizeof(sign))
		goto write_error;

	/*
	 * version
	 */
	version = SCST_PR_FILE_VERSION;
	res = kernel_write(file, &version, sizeof(version), &pos);
	if (res != sizeof(version))
		goto write_error;

	/*
	 * APTPL
	 */
	aptpl = dev->pr_aptpl;
	res = kernel_write(file, &aptpl, sizeof(aptpl), &pos);
	if (res != sizeof(aptpl))
		goto write_error;

	/*
	 * reservation
	 */
	pr_is_set = dev->pr_is_set;
	res = kernel_write(file, &pr_is_set, sizeof(pr_is_set), &pos);
	if (res != sizeof(pr_is_set))
		goto write_error;

	res = kernel_write(file, &dev->pr_type, sizeof(dev->pr_type), &pos);
	if (res != sizeof(dev->pr_type))
		goto write_error;

	res = kernel_write(file, &dev->pr_scope, sizeof(dev->pr_scope), &pos);
	if (res != sizeof(dev->pr_scope))
		goto write_error;

	/*
	 * registration records
	 */
	list_for_each_entry(reg, &dev->dev_registrants_list,
			    dev_registrants_list_entry) {
		uint8_t is_holder = 0;
		uint32_t size;

		is_holder = (dev->pr_holder == reg);

		res = kernel_write(file, &is_holder, sizeof(is_holder), &pos);
		if (res != sizeof(is_holder))
			goto write_error;

		size = scst_tid_size(reg->transport_id);
		res = kernel_write(file, reg->transport_id, size, &pos);
		if (res != size)
			goto write_error;

		res = kernel_write(file, &reg->key, sizeof(reg->key), &pos);
		if (res != sizeof(reg->key))
			goto write_error;

		res = kernel_write(file, &reg->rel_tgt_id,
				   sizeof(reg->rel_tgt_id), &pos);
		if (res != sizeof(reg->rel_tgt_id))
			goto write_error;
	}

	res = vfs_fsync(file, 1);
	if (res != 0) {
		PRINT_ERROR("fsync() of the PR file failed: %d", res);
		goto write_error_close;
	}

	sign = SCST_PR_FILE_SIGN;
	pos = 0;
	res = kernel_write(file, &sign, sizeof(sign), &pos);
	if (res != sizeof(sign))
		goto write_error;

	res = vfs_fsync(file, 1);
	if (res != 0) {
		PRINT_ERROR("fsync() of the PR file failed: %d", res);
		goto write_error_close;
	}

	res = 0;

	filp_close(file, NULL);

out:
	if (res != 0) {
		PRINT_CRIT_ERROR("Unable to save persistent information "
				 "(device %s)", dev->virt_name);
		 /*
		  * It's safer to not return any error to the initiator and expect
		  * operator's intervention to be able to save the PR's state next
		  * time, than to screw up all the interactions with this initiator.
		  */
	}

	TRACE_EXIT();
	return;

write_error:
	PRINT_ERROR("Error writing to '%s' - error %d", dev->pr_file_name, res);

write_error_close:
	filp_close(file, NULL);

	{
		struct path path;
		int rc;

		rc = kern_path(dev->pr_file_name, 0, &path);
		if (!rc)
			scst_vfs_unlink_and_put(&path);
		else
			TRACE_PR("Unable to lookup '%s' - error %d",
				dev->pr_file_name, rc);
	}
	goto out;
}


/**
 * scst_pr_set_file_name - set name of file in which to save PR information
 * @dev:  SCST device.
 * @prev: If not NULL, the current path will be stored in *@prev. It is the
 *        responsibility of the caller to invoke kfree(*@prev) at an
 *        appropriate time.
 * @fmt:  Full path of the file in which to save PR info.
 *
 * This function must be called either while @dev is not on the device list
 * or with scst_mutex held.
 */
int scst_pr_set_file_name(struct scst_device *dev, char **prev,
			  const char *fmt, ...)
{
	va_list args;
	char *pr_file_name = NULL, *bkp = NULL;
	int file_mode, res = -EINVAL;

	scst_assert_pr_mutex_held(dev);

	sBUG_ON(!fmt);

	res = -ENOMEM;
	va_start(args, fmt);
	pr_file_name = kvasprintf(GFP_KERNEL, fmt, args);
	va_end(args);
	if (!pr_file_name) {
		PRINT_ERROR("Unable to kvasprintf() new PR file name");
		goto out;
	}

	res = -EINVAL;
	if (pr_file_name[0] != '/') {
		PRINT_ERROR("PR file name %s must be absolute!", pr_file_name);
		goto out;
	}

	file_mode = scst_get_file_mode(pr_file_name);
	if (file_mode >= 0 && !S_ISREG(file_mode) && !S_ISBLK(file_mode)) {
		PRINT_ERROR("PR file name %s must be file or block device!",
			    pr_file_name);
		goto out;
	}

	res = -ENOENT;
	if (!scst_parent_dir_exists(pr_file_name)) {
		PRINT_ERROR("PR file name %s parent directory doesn't exist",
			    pr_file_name);
		goto out;
	}

	res = -ENOMEM;
	bkp = kasprintf(GFP_KERNEL, "%s.1", pr_file_name);
	if (!bkp) {
		PRINT_ERROR("Unable to kasprintf() backup PR file name");
		goto out;
	}
	if (prev) {
		*prev = dev->pr_file_name;
		dev->pr_file_name = pr_file_name;
		pr_file_name = NULL;
	} else
		swap(dev->pr_file_name, pr_file_name);
	swap(dev->pr_file_name1, bkp);
	res = 0;

out:
	kfree(pr_file_name);
	kfree(bkp);
	return res;
}

/* Initialize the PR members in *dev. */
int scst_pr_init(struct scst_device *dev)
{
	mutex_init(&dev->dev_pr_mutex);
	dev->cl_ops = &scst_no_dlm_cl_ops;
	dev->pr_generation = 0;
	dev->pr_is_set = 0;
	dev->pr_holder = NULL;
	dev->pr_scope = SCOPE_LU;
	dev->pr_type = TYPE_UNSPECIFIED;
	INIT_LIST_HEAD(&dev->dev_registrants_list);

	return 0;
}

/* Free the resources allocated by scst_pr_init().  */
void scst_pr_cleanup(struct scst_device *dev)
{
	dev->cl_ops->pr_cleanup(dev);
}

/* Caller must hold scst_mutex and activity must be suspended. */
int scst_pr_set_cluster_mode(struct scst_device *dev, bool cluster_mode,
			     const char *cl_dev_id)
{
	int res = 0;

#if defined(CONFIG_DLM) || defined(CONFIG_DLM_MODULE) && \
	!defined(CONFIG_SCST_NO_DLM)
	bool cluster_mode_enabled = false;

	cluster_mode_enabled = dev->cl_ops == &scst_dlm_cl_ops;

	if (cluster_mode_enabled == cluster_mode)
		goto out;

	PRINT_INFO("%s: changing cluster_mode from %d into %d", dev->virt_name,
		   cluster_mode_enabled, cluster_mode);
	dev->cl_ops->pr_cleanup(dev);
	dev->cl_ops = cluster_mode ? &scst_dlm_cl_ops : &scst_no_dlm_cl_ops;
	res = dev->cl_ops->pr_init(dev, cl_dev_id);
	if (res) {
		PRINT_ERROR("%s: changing cluster_mode into %d failed: %d",
			    dev->virt_name, cluster_mode, res);
		dev->cl_ops = &scst_no_dlm_cl_ops;
	}

out:
#else
	res = cluster_mode ? -EOPNOTSUPP : 0;
#endif

	return res;
}
EXPORT_SYMBOL(scst_pr_set_cluster_mode);

/* Must be called under dev_pr_mutex or before dev is on the device list. */
int scst_pr_init_dev(struct scst_device *dev)
{
	int res = 0;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	sBUG_ON(!dev->pr_file_name || !dev->pr_file_name1);

	res = scst_pr_load_device_file(dev);
	if (res == -ENOENT)
		res = 0;

	TRACE_EXIT_RES(res);
	return res;
}

/* Called under scst_mutex */
void scst_pr_clear_dev(struct scst_device *dev)
{
	TRACE_ENTRY();

	scst_pr_remove_registrants(dev);

	kfree(dev->pr_file_name);
	kfree(dev->pr_file_name1);

	TRACE_EXIT();
	return;
}

/* Called under scst_mutex */
int scst_pr_init_tgt_dev(struct scst_tgt_dev *tgt_dev)
{
	int res = 0;
	struct scst_dev_registrant *reg;
	struct scst_device *dev = tgt_dev->dev;
	const uint8_t *transport_id = tgt_dev->sess->transport_id;
	const uint16_t rel_tgt_id = tgt_dev->sess->tgt->rel_tgt_id;

	TRACE_ENTRY();

	if (tgt_dev->sess->transport_id == NULL)
		goto out;

	scst_pr_write_lock(dev);

	reg = scst_pr_find_reg(dev, transport_id, rel_tgt_id);
	if ((reg != NULL) && (reg->tgt_dev == NULL)) {
		TRACE_PR("Assigning reg %s/%d (%p) to tgt_dev %p (dev %s)",
			debug_transport_id_to_initiator_name(transport_id),
			rel_tgt_id, reg, tgt_dev, dev->virt_name);
		tgt_dev->registrant = reg;
		reg->tgt_dev = tgt_dev;
	}

	scst_pr_write_unlock(dev);

out:
	TRACE_EXIT_RES(res);
	return res;
}

/* Called under scst_mutex */
void scst_pr_clear_tgt_dev(struct scst_tgt_dev *tgt_dev)
{
	struct scst_device *dev = tgt_dev->dev;
	struct scst_dev_registrant *reg;
	struct scst_tgt_dev *t;

	TRACE_ENTRY();

	scst_pr_write_lock(dev);

	reg = tgt_dev->registrant;
	if (reg) {
		tgt_dev->registrant = NULL;
		reg->tgt_dev = NULL;

		/* Just in case, actually. It should never happen. */
		list_for_each_entry(t, &dev->dev_tgt_dev_list,
					dev_tgt_dev_list_entry) {
			if (t == tgt_dev)
				continue;
			if ((t->sess->tgt->rel_tgt_id == reg->rel_tgt_id) &&
			    tid_equal(t->sess->transport_id, reg->transport_id)) {
				TRACE_PR("Reassigning reg %s/%d (%p) to tgt_dev "
					"%p (being cleared tgt_dev %p)",
					debug_transport_id_to_initiator_name(
						reg->transport_id),
					reg->rel_tgt_id, reg, t, tgt_dev);
				t->registrant = reg;
				reg->tgt_dev = t;
				break;
			}
		}
	}

	scst_pr_write_unlock(dev);

	TRACE_EXIT();
	return;
}

/* Called with dev_pr_mutex locked. Might also be called under scst_mutex2. */
static int scst_pr_register_with_spec_i_pt(struct scst_cmd *cmd,
	const uint16_t rel_tgt_id, uint8_t *buffer, int buffer_size,
	struct list_head *rollback_list)
{
	int res = 0;
	int offset;
	unsigned int ext_size;
	__be64 action_key;
	struct scst_device *dev = cmd->dev;
	struct scst_dev_registrant *reg;

	scst_assert_pr_mutex_held(cmd->dev);

	action_key = get_unaligned((__be64 *)&buffer[8]);

	ext_size = get_unaligned_be32(&buffer[24]);
	if ((ext_size + 28) > buffer_size) {
		TRACE_PR("Invalid buffer size %d (max %d)", buffer_size,
			ext_size + 28);
		scst_set_cmd_error(cmd,
			SCST_LOAD_SENSE(scst_sense_parameter_list_length_invalid));
		res = -EINVAL;
		goto out;
	}

	offset = 0;
	while (offset < ext_size) {
		uint8_t *const transport_id = &buffer[28 + offset];
		const uint32_t tid_size = scst_tid_size(transport_id);

		if (offset + tid_size > ext_size) {
			TRACE_PR("Invalid transport_id size %d (max %d)",
				tid_size, ext_size - offset);
			scst_set_invalid_field_in_parm_list(cmd, 24, 0);
			res = -EINVAL;
			goto out;
		}
		WARN_ON_ONCE(!tid_secure(transport_id, tid_size));
		offset += tid_size;
	}

	offset = 0;
	while (offset < ext_size) {
		uint8_t *transport_id = &buffer[28 + offset];
		struct scst_tgt_dev *t;

		TRACE_PR("rel_tgt_id %d, transport_id %s", rel_tgt_id,
			debug_transport_id_to_initiator_name(transport_id));

		if ((transport_id[0] & 0x0f) == SCSI_TRANSPORTID_PROTOCOLID_ISCSI &&
		    (transport_id[0] & 0xc0) == 0) {
			TRACE_PR("Wildcard iSCSI TransportID %s",
				&transport_id[4]);
			/*
			 * We can't use scst_mutex here because the caller
			 * already holds dev_pr_mutex.
			 */
			spin_lock_bh(&dev->dev_lock);
			list_for_each_entry(t, &dev->dev_tgt_dev_list,
						dev_tgt_dev_list_entry) {
				/*
				 * We must go over all matching tgt_devs and
				 * register them on the requested rel_tgt_id
				 */
				if (!tid_equal(t->sess->transport_id,
						transport_id))
					continue;

				reg = scst_pr_find_reg(dev,
					t->sess->transport_id, rel_tgt_id);
				if (reg == NULL) {
					reg = scst_pr_add_registrant(dev,
						t->sess->transport_id,
						rel_tgt_id, action_key, true);
					if (reg == NULL) {
						spin_unlock_bh(&dev->dev_lock);
						scst_set_busy(cmd);
						res = -ENOMEM;
						goto out;
					}
				} else if (reg->key != action_key) {
					TRACE_PR("Changing key of reg %p "
						"(tgt_dev %p)", reg, t);
					reg->rollback_key = reg->key;
					reg->key = action_key;
				} else
					continue;

				list_add_tail(&reg->aux_list_entry,
					rollback_list);
			}
			spin_unlock_bh(&dev->dev_lock);
		} else {
			reg = scst_pr_find_reg(dev, transport_id, rel_tgt_id);
			if (reg != NULL) {
				if (reg->key == action_key)
					goto next;
				TRACE_PR("Changing key of reg %p (tgt_dev %p)",
					reg, reg->tgt_dev);
				reg->rollback_key = reg->key;
				reg->key = action_key;
			} else {
				reg = scst_pr_add_registrant(dev, transport_id,
						rel_tgt_id, action_key, false);
				if (reg == NULL) {
					scst_set_busy(cmd);
					res = -ENOMEM;
					goto out;
				}
			}

			list_add_tail(&reg->aux_list_entry,
				rollback_list);
		}
next:
		offset += scst_tid_size(transport_id);
	}
out:
	return res;
}

/* Called with dev_pr_mutex locked, no IRQ */
static void scst_pr_unregister(struct scst_device *dev,
	struct scst_dev_registrant *reg)
{
	bool is_holder;
	uint8_t pr_type;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	TRACE_PR("Unregistering key %0llx", reg->key);

	is_holder = scst_pr_is_holder(dev, reg);
	pr_type = dev->pr_type;

	scst_pr_remove_registrant(dev, reg);

	if (is_holder && !dev->pr_is_set) {
		/* A registration just released */
		switch (pr_type) {
		case TYPE_WRITE_EXCLUSIVE_REGONLY:
		case TYPE_EXCLUSIVE_ACCESS_REGONLY:
			scst_pr_send_ua_all(dev, NULL,
				SCST_LOAD_SENSE(scst_sense_reservation_released));
			break;
		}
	}

	TRACE_EXIT();
	return;
}

/* Called with dev_pr_mutex locked, no IRQ */
static void scst_pr_unregister_all_tg_pt(struct scst_device *dev,
	const uint8_t *transport_id)
{
	struct scst_tgt_template *tgtt;
	uint8_t proto_id = transport_id[0] & 0x0f;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	/*
	 * We can't use scst_mutex here since the caller already holds
	 * dev_pr_mutex.
	 */
	mutex_lock(&scst_mutex2);

	list_for_each_entry(tgtt, &scst_template_list, scst_template_list_entry) {
		struct scst_tgt *tgt;

		if (tgtt->get_initiator_port_transport_id == NULL)
			continue;

		list_for_each_entry(tgt, &tgtt->tgt_list, tgt_list_entry) {
			struct scst_dev_registrant *reg;

			if (tgtt->get_initiator_port_transport_id(tgt, NULL, NULL) != proto_id)
				continue;

			reg = scst_pr_find_reg(dev, transport_id,
					tgt->rel_tgt_id);
			if (reg == NULL)
				continue;

			scst_pr_unregister(dev, reg);
		}
	}

	mutex_unlock(&scst_mutex2);

	TRACE_EXIT();
	return;
}

/* Called with dev_pr_mutex locked. Might also be called under scst_mutex2. */
static int scst_pr_register_on_tgt_id(struct scst_cmd *cmd,
	const uint16_t rel_tgt_id, uint8_t *buffer, int buffer_size,
	bool spec_i_pt, struct list_head *rollback_list)
{
	int res;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(cmd->dev);

	TRACE_PR("rel_tgt_id %d, spec_i_pt %d", rel_tgt_id, spec_i_pt);

	if (spec_i_pt) {
		res = scst_pr_register_with_spec_i_pt(cmd, rel_tgt_id, buffer,
					buffer_size, rollback_list);
		if (res != 0)
			goto out;
	}

	/* tgt_dev can be among TIDs for scst_pr_register_with_spec_i_pt() */

	if (scst_pr_find_reg(cmd->dev, cmd->sess->transport_id, rel_tgt_id) == NULL) {
		__be64 action_key;
		struct scst_dev_registrant *reg;

		action_key = get_unaligned((__be64 *)&buffer[8]);

		reg = scst_pr_add_registrant(cmd->dev, cmd->sess->transport_id,
			rel_tgt_id, action_key, false);
		if (reg == NULL) {
			res = -ENOMEM;
			scst_set_busy(cmd);
			goto out;
		}

		list_add_tail(&reg->aux_list_entry, rollback_list);
	}

	res = 0;

out:
	TRACE_EXIT_RES(res);
	return res;
}

/* Add registrants for remote ports. Called with dev_pr_mutex locked, no IRQ. */
static int scst_register_remote_ports(struct scst_cmd *cmd, uint8_t *buffer,
	int buffer_size, bool spec_i_pt, struct list_head *rollback_list)
{
	struct scst_dev_group *dg;
	struct scst_target_group *tg;
	struct scst_tg_tgt *tgtgt;
	int res = 0;

	scst_alua_lock();

	dg = scst_lookup_dg_by_dev(cmd->dev);
	if (!dg)
		goto out_unlock;

	list_for_each_entry(tg, &dg->tg_list, entry) {
		list_for_each_entry(tgtgt, &tg->tgt_list, entry) {
			/* Skip local target ports */
			if (tgtgt->tgt)
				continue;
			/* To do: check the initiator port transport ID. */
			if (tgtgt->rel_tgt_id == 0)
				continue;
			res = scst_pr_register_on_tgt_id(cmd, tgtgt->rel_tgt_id,
							 buffer, buffer_size,
							 spec_i_pt,
							 rollback_list);
			if (res != 0)
				goto out_unlock;
		}
	}
out_unlock:
	scst_alua_unlock();

	return res;
}

/* Register all target ports. Called with dev_pr_mutex locked, no IRQ. */
static int scst_pr_register_all_tg_pt(struct scst_cmd *cmd, uint8_t *buffer,
	int buffer_size, bool spec_i_pt, struct list_head *rollback_list)
{
	int res = 0;
	struct scst_tgt_template *tgtt;
	uint8_t proto_id = cmd->sess->transport_id[0] & 0x0f;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(cmd->dev);

	/*
	 * We can't use scst_mutex here because the caller already holds
	 * dev_pr_mutex.
	 */
	mutex_lock(&scst_mutex2);

	list_for_each_entry(tgtt, &scst_template_list, scst_template_list_entry) {
		struct scst_tgt *tgt;

		if (tgtt->get_initiator_port_transport_id == NULL)
			continue;

		TRACE_PR("tgtt %s, spec_i_pt %d", tgtt->name, spec_i_pt);

		list_for_each_entry(tgt, &tgtt->tgt_list, tgt_list_entry) {
			if (tgtt->get_initiator_port_transport_id(tgt, NULL, NULL) != proto_id)
				continue;
			if (tgt->rel_tgt_id == 0)
				continue;
			if (tgt->tgt_forward_dst) {
				TRACE_PR("ALL_TG_PT: skipping forwarding "
					"target %s", tgt->tgt_name);
				continue;
			}
			TRACE_PR("tgt %s, rel_tgt_id %d", tgt->tgt_name,
				tgt->rel_tgt_id);
			res = scst_pr_register_on_tgt_id(cmd, tgt->rel_tgt_id,
				buffer, buffer_size, spec_i_pt, rollback_list);
			if (res != 0)
				goto out_unlock;
		}
	}

	res = scst_register_remote_ports(cmd, buffer, buffer_size, spec_i_pt,
					 rollback_list);

out_unlock:
	mutex_unlock(&scst_mutex2);

	TRACE_EXIT_RES(res);
	return res;
}

/* Called with dev_pr_mutex locked, no IRQ */
static int __scst_pr_register(struct scst_cmd *cmd, uint8_t *buffer,
	int buffer_size, bool spec_i_pt, bool all_tg_pt)
{
	int res;
	struct scst_dev_registrant *reg, *treg;
	LIST_HEAD(rollback_list);

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(cmd->dev);

	if (all_tg_pt) {
		res = scst_pr_register_all_tg_pt(cmd, buffer, buffer_size,
				spec_i_pt, &rollback_list);
		if (res != 0)
			goto out_rollback;
	} else {
		res = scst_pr_register_on_tgt_id(cmd,
			cmd->sess->tgt->rel_tgt_id, buffer, buffer_size,
			spec_i_pt, &rollback_list);
		if (res != 0)
			goto out_rollback;
	}

	list_for_each_entry(reg, &rollback_list, aux_list_entry) {
		reg->rollback_key = 0;
	}

out:
	TRACE_EXIT_RES(res);
	return res;

out_rollback:
	list_for_each_entry_safe(reg, treg, &rollback_list, aux_list_entry) {
		list_del(&reg->aux_list_entry);
		if (reg->rollback_key == 0)
			scst_pr_remove_registrant(cmd->dev, reg);
		else {
			reg->key = reg->rollback_key;
			reg->rollback_key = 0;
		}
	}
	goto out;
}

/* Called with dev_pr_mutex locked, no IRQ */
void scst_pr_register(struct scst_cmd *cmd, uint8_t *buffer, int buffer_size)
{
	int aptpl, spec_i_pt, all_tg_pt;
	__be64 key, action_key;
	struct scst_device *dev = cmd->dev;
	struct scst_tgt_dev *tgt_dev = cmd->tgt_dev;
	struct scst_session *sess = cmd->sess;
	struct scst_dev_registrant *reg;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(cmd->dev);

	aptpl = buffer[20] & 0x01;
	spec_i_pt = (buffer[20] >> 3) & 0x01;
	all_tg_pt = (buffer[20] >> 2) & 0x01;
	key = get_unaligned((__be64 *)&buffer[0]);
	action_key = get_unaligned((__be64 *)&buffer[8]);

	if (spec_i_pt == 0 && buffer_size != 24) {
		TRACE_PR("Invalid buffer size %d", buffer_size);
		scst_set_cmd_error(cmd,
			SCST_LOAD_SENSE(scst_sense_parameter_list_length_invalid));
		goto out;
	}


	reg = tgt_dev->registrant;

	TRACE_PR("Register: initiator %s/%d (%p), key %0llx, action_key %0llx "
		"(tgt_dev %p)",
		debug_transport_id_to_initiator_name(sess->transport_id),
		sess->tgt->rel_tgt_id, reg, be64_to_cpu(key),
		be64_to_cpu(action_key), tgt_dev);

	if (reg == NULL) {
		TRACE_PR("tgt_dev %p is not registered yet - registering",
			tgt_dev);
		if (key) {
			TRACE_PR("%s", "Key must be zero on new registration");
			scst_set_cmd_error_status(cmd, SAM_STAT_RESERVATION_CONFLICT);
			goto out;
		}
		if (action_key) {
			int rc = __scst_pr_register(cmd, buffer, buffer_size,
					spec_i_pt, all_tg_pt);
			if (rc != 0)
				goto out;
		} else
			TRACE_PR("%s", "Doing nothing - action_key is zero");
	} else {
		if (reg->key != key) {
			TRACE_PR("tgt_dev %p already registered - reservation "
				"key %0llx mismatch", tgt_dev,
				be64_to_cpu(reg->key));
			scst_set_cmd_error_status(cmd,
				SAM_STAT_RESERVATION_CONFLICT);
			goto out;
		}
		if (spec_i_pt) {
			TRACE_PR("%s", "spec_i_pt must be zero in this case");
			scst_set_invalid_field_in_parm_list(cmd, 20,
				SCST_INVAL_FIELD_BIT_OFFS_VALID | 3);
			goto out;
		}
		if (action_key == 0) {
			if (all_tg_pt)
				scst_pr_unregister_all_tg_pt(dev,
					sess->transport_id);
			else
				scst_pr_unregister(dev, reg);
		} else
			reg->key = action_key;
	}

	dev->pr_generation++;

	dev->pr_aptpl = aptpl;

	scst_pr_dump_prs(dev, false);

out:
	TRACE_EXIT();
	return;
}

/* Called with dev_pr_mutex locked, no IRQ */
void scst_pr_register_and_ignore(struct scst_cmd *cmd, uint8_t *buffer,
	int buffer_size)
{
	int aptpl, all_tg_pt;
	__be64 action_key;
	struct scst_dev_registrant *reg = NULL;
	struct scst_device *dev = cmd->dev;
	struct scst_tgt_dev *tgt_dev = cmd->tgt_dev;
	struct scst_session *sess = cmd->sess;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(cmd->dev);

	aptpl = buffer[20] & 0x01;
	all_tg_pt = (buffer[20] >> 2) & 0x01;
	action_key = get_unaligned((__be64 *)&buffer[8]);

	if (buffer_size != 24) {
		TRACE_PR("Invalid buffer size %d", buffer_size);
		scst_set_cmd_error(cmd,
			SCST_LOAD_SENSE(scst_sense_parameter_list_length_invalid));
		goto out;
	}


	reg = tgt_dev->registrant;

	TRACE_PR("Register and ignore: initiator %s/%d (%p), action_key "
		"%016llx (tgt_dev %p)",
		debug_transport_id_to_initiator_name(sess->transport_id),
		sess->tgt->rel_tgt_id, reg, be64_to_cpu(action_key),
		tgt_dev);

	if (reg == NULL) {
		TRACE_PR("Tgt_dev %p is not registered yet - trying to "
			"register", tgt_dev);
		if (action_key) {
			int rc = __scst_pr_register(cmd, buffer, buffer_size,
					false, all_tg_pt);
			if (rc != 0)
				goto out;
		} else
			TRACE_PR("%s", "Doing nothing, action_key is zero");
	} else {
		if (action_key == 0) {
			if (all_tg_pt)
				scst_pr_unregister_all_tg_pt(dev,
					sess->transport_id);
			else
				scst_pr_unregister(dev, reg);
		} else
			reg->key = action_key;
	}

	dev->pr_generation++;

	dev->pr_aptpl = aptpl;

	scst_pr_dump_prs(dev, false);

out:
	TRACE_EXIT();
	return;
}

/* Called with dev_pr_mutex locked, no IRQ */
void scst_pr_register_and_move(struct scst_cmd *cmd, uint8_t *buffer,
	int buffer_size)
{
	int aptpl;
	int unreg;
	unsigned int tid_buffer_size;
	__be64 key, action_key;
	struct scst_device *dev = cmd->dev;
	struct scst_tgt_dev *tgt_dev = cmd->tgt_dev;
	struct scst_session *sess = cmd->sess;
	struct scst_dev_registrant *reg, *reg_move;
	const uint8_t *transport_id = NULL;
	uint8_t *transport_id_move = NULL;
	uint16_t rel_tgt_id_move;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(cmd->dev);

	aptpl = buffer[17] & 0x01;
	key = get_unaligned((__be64 *)&buffer[0]);
	action_key = get_unaligned((__be64 *)&buffer[8]);
	unreg = (buffer[17] >> 1) & 0x01;
	tid_buffer_size = get_unaligned_be32(&buffer[20]);


	if ((tid_buffer_size + 24) > buffer_size) {
		TRACE_PR("Invalid buffer size %d (%d)",
			buffer_size, tid_buffer_size + 24);
		scst_set_cmd_error(cmd,
			SCST_LOAD_SENSE(scst_sense_invalid_field_in_parm_list));
		goto out;
	}

	if (tid_buffer_size < 24) {
		TRACE_PR("%s", "Transport id buffer too small");
		scst_set_invalid_field_in_parm_list(cmd, 20, 0);
		goto out;
	}

	reg = tgt_dev->registrant;
	/* We already checked reg is not NULL */
	if (reg->key != key) {
		TRACE_PR("Registrant's %s/%d (%p) key %016llx mismatch with "
			"%016llx (tgt_dev %p)",
			debug_transport_id_to_initiator_name(reg->transport_id),
			reg->rel_tgt_id, reg, be64_to_cpu(reg->key),
			be64_to_cpu(key), tgt_dev);
		scst_set_cmd_error_status(cmd, SAM_STAT_RESERVATION_CONFLICT);
		goto out;
	}

	if (!dev->pr_is_set) {
		TRACE_PR("%s", "There must be a PR");
		scst_set_cmd_error(cmd,
			SCST_LOAD_SENSE(scst_sense_invalid_field_in_cdb));
		goto out;
	}

	/*
	 * This check also required by table "PERSISTENT RESERVE OUT service
	 * actions that are allowed in the presence of various reservations".
	 */
	if (!scst_pr_is_holder(dev, reg)) {
		TRACE_PR("Registrant %s/%d (%p) is not a holder (tgt_dev %p)",
			debug_transport_id_to_initiator_name(reg->transport_id),
			reg->rel_tgt_id, reg, tgt_dev);
		scst_set_cmd_error_status(cmd, SAM_STAT_RESERVATION_CONFLICT);
		goto out;
	}

	if (action_key == 0) {
		TRACE_PR("%s", "Action key must be non-zero");
		scst_set_invalid_field_in_cdb(cmd, 8, 0);
		goto out;
	}

	transport_id = sess->transport_id;
	transport_id_move = (uint8_t *)&buffer[24];
	rel_tgt_id_move = get_unaligned_be16(&buffer[18]);

	if ((scst_tid_size(transport_id_move) + 24) > buffer_size) {
		TRACE_PR("Invalid buffer size %d (%d)",
			buffer_size, scst_tid_size(transport_id_move) + 24);
		scst_set_cmd_error(cmd,
			SCST_LOAD_SENSE(scst_sense_invalid_field_in_parm_list));
		goto out;
	}

	if (!tid_secure(transport_id_move, buffer_size - 24)) {
		TRACE_PR("Transport ID length %d exceeds buffer size %d",
			 scst_tid_size(transport_id_move), buffer_size - 24);
		scst_set_cmd_error(cmd,
			SCST_LOAD_SENSE(scst_sense_invalid_field_in_parm_list));
		goto out;
	}

	if (dev->pr_type == TYPE_WRITE_EXCLUSIVE_ALL_REG ||
	    dev->pr_type == TYPE_EXCLUSIVE_ACCESS_ALL_REG) {
		TRACE_PR("Unable to finish operation due to wrong reservation "
			"type %02x", dev->pr_type);
		scst_set_cmd_error_status(cmd, SAM_STAT_RESERVATION_CONFLICT);
		goto out;
	}

	if (tid_equal(transport_id, transport_id_move)) {
		TRACE_PR("%s", "Equal transport id's");
		scst_set_invalid_field_in_parm_list(cmd, 24, 0);
		goto out;
	}

	reg_move = scst_pr_find_reg(dev, transport_id_move, rel_tgt_id_move);
	if (reg_move == NULL) {
		reg_move = scst_pr_add_registrant(dev, transport_id_move,
			rel_tgt_id_move, action_key, false);
		if (reg_move == NULL) {
			scst_set_busy(cmd);
			goto out;
		}
	} else if (reg_move->key != action_key) {
		TRACE_PR("Changing key for reg %p", reg);
		reg_move->key = action_key;
	}

	TRACE_PR("Register and move: from initiator %s/%d (%p, tgt_dev %p) to "
		"initiator %s/%d (%p, tgt_dev %p), key %016llx (unreg %d)",
		debug_transport_id_to_initiator_name(reg->transport_id),
		reg->rel_tgt_id, reg, reg->tgt_dev,
		debug_transport_id_to_initiator_name(transport_id_move),
		rel_tgt_id_move, reg_move, reg_move->tgt_dev,
		be64_to_cpu(action_key), unreg);

	/* Move the holder */
	scst_pr_set_holder(dev, reg_move, dev->pr_scope, dev->pr_type);

	if (unreg)
		scst_pr_remove_registrant(dev, reg);

	dev->pr_generation++;

	dev->pr_aptpl = aptpl;

	scst_pr_dump_prs(dev, false);

out:
	TRACE_EXIT();
	return;
}

/* Called with dev_pr_mutex locked, no IRQ */
void scst_pr_reserve(struct scst_cmd *cmd, uint8_t *buffer, int buffer_size)
{
	uint8_t scope, type;
	__be64 key;
	struct scst_device *dev = cmd->dev;
	struct scst_tgt_dev *tgt_dev = cmd->tgt_dev;
	struct scst_dev_registrant *reg;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	key = get_unaligned((__be64 *)&buffer[0]);
	scope = cmd->cdb[2] >> 4;
	type = cmd->cdb[2] & 0x0f;

	if (buffer_size != 24) {
		TRACE_PR("Invalid buffer size %d", buffer_size);
		scst_set_cmd_error(cmd,
			SCST_LOAD_SENSE(scst_sense_parameter_list_length_invalid));
		goto out;
	}

	if (!scst_pr_type_valid(type)) {
		TRACE_PR("Invalid reservation type %d", type);
		scst_set_invalid_field_in_cdb(cmd, 2,
			SCST_INVAL_FIELD_BIT_OFFS_VALID | 0);
		goto out;
	}

	if (scope != SCOPE_LU) {
		TRACE_PR("Invalid reservation scope %d", scope);
		scst_set_invalid_field_in_cdb(cmd, 2,
			SCST_INVAL_FIELD_BIT_OFFS_VALID | 4);
		goto out;
	}

	reg = tgt_dev->registrant;

	TRACE_PR("Reserve: initiator %s/%d (%p), key %016llx, scope %d, "
		"type %d (tgt_dev %p)",
		debug_transport_id_to_initiator_name(cmd->sess->transport_id),
		cmd->sess->tgt->rel_tgt_id, reg, be64_to_cpu(key), scope,
		type, tgt_dev);

	/* We already checked reg is not NULL */
	if (reg->key != key) {
		TRACE_PR("Registrant's %p key %016llx mismatch with %016llx",
			reg, be64_to_cpu(reg->key), be64_to_cpu(key));
		scst_set_cmd_error_status(cmd, SAM_STAT_RESERVATION_CONFLICT);
		goto out;
	}

	if (!dev->pr_is_set)
		scst_pr_set_holder(dev, reg, scope, type);
	else {
		if (!scst_pr_is_holder(dev, reg)) {
			/*
			 * This check also required by table "PERSISTENT
			 * RESERVE OUT service actions that are allowed in the
			 * presence of various reservations".
			 */
			TRACE_PR("Only holder can override - reg %p is not a "
				"holder", reg);
			scst_set_cmd_error_status(cmd,
				SAM_STAT_RESERVATION_CONFLICT);
			goto out;
		} else {
			if (dev->pr_scope != scope || dev->pr_type != type) {
				TRACE_PR("Error overriding scope or type for "
					"reg %p", reg);
				scst_set_cmd_error_status(cmd,
					SAM_STAT_RESERVATION_CONFLICT);
				goto out;
			} else
				TRACE_PR("Do nothing: reservation of reg %p "
					"is the same", reg);
		}
	}

	scst_pr_dump_prs(dev, false);

out:
	TRACE_EXIT();
	return;
}

/* Called with dev_pr_mutex locked, no IRQ */
void scst_pr_release(struct scst_cmd *cmd, uint8_t *buffer, int buffer_size)
{
	int scope, type;
	__be64 key;
	struct scst_device *dev = cmd->dev;
	struct scst_tgt_dev *tgt_dev = cmd->tgt_dev;
	struct scst_dev_registrant *reg;
	uint8_t cur_pr_type;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	key = get_unaligned((__be64 *)&buffer[0]);
	scope = cmd->cdb[2] >> 4;
	type = cmd->cdb[2] & 0x0f;

	if (buffer_size != 24) {
		TRACE_PR("Invalid buffer size %d", buffer_size);
		scst_set_cmd_error(cmd,
			SCST_LOAD_SENSE(scst_sense_parameter_list_length_invalid));
		goto out;
	}

	if (!dev->pr_is_set) {
		TRACE_PR("%s", "There is no PR - do nothing");
		goto out;
	}

	reg = tgt_dev->registrant;

	TRACE_PR("Release: initiator %s/%d (%p), key %016llx, scope %d, type "
		"%d (tgt_dev %p)", debug_transport_id_to_initiator_name(
					cmd->sess->transport_id),
		cmd->sess->tgt->rel_tgt_id, reg, be64_to_cpu(key), scope,
		type, tgt_dev);

	/* We already checked reg is not NULL */
	if (reg->key != key) {
		TRACE_PR("Registrant's %p key %016llx mismatch with %016llx",
			reg, be64_to_cpu(reg->key), be64_to_cpu(key));
		scst_set_cmd_error_status(cmd, SAM_STAT_RESERVATION_CONFLICT);
		goto out;
	}

	if (!scst_pr_is_holder(dev, reg)) {
		TRACE_PR("Registrant %p is not a holder - do nothing", reg);
		goto out;
	}

	if (dev->pr_scope != scope || dev->pr_type != type) {
		TRACE_PR("%s", "Released scope or type do not match with "
			"holder");
		scst_set_cmd_error(cmd,
			SCST_LOAD_SENSE(scst_sense_invalid_release));
		goto out;
	}

	cur_pr_type = dev->pr_type; /* it will be cleared */

	scst_pr_clear_reservation(dev);

	switch (cur_pr_type) {
	case TYPE_WRITE_EXCLUSIVE_REGONLY:
	case TYPE_EXCLUSIVE_ACCESS_REGONLY:
	case TYPE_WRITE_EXCLUSIVE_ALL_REG:
	case TYPE_EXCLUSIVE_ACCESS_ALL_REG:
		scst_pr_send_ua_all(dev, reg,
			SCST_LOAD_SENSE(scst_sense_reservation_released));
	}

	scst_pr_dump_prs(dev, false);

out:
	TRACE_EXIT();
	return;
}

/* Called with dev_pr_mutex locked, no IRQ */
void scst_pr_clear(struct scst_cmd *cmd, uint8_t *buffer, int buffer_size)
{
	__be64 key;
	struct scst_device *dev = cmd->dev;
	struct scst_tgt_dev *tgt_dev = cmd->tgt_dev;
	struct scst_dev_registrant *reg, *r, *t;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	key = get_unaligned((__be64 *)&buffer[0]);

	if (buffer_size != 24) {
		TRACE_PR("Invalid buffer size %d", buffer_size);
		scst_set_cmd_error(cmd,
			SCST_LOAD_SENSE(scst_sense_parameter_list_length_invalid));
		goto out;
	}

	reg = tgt_dev->registrant;

	TRACE_PR("Clear: initiator %s/%d (%p), key %016llx (tgt_dev %p)",
		debug_transport_id_to_initiator_name(cmd->sess->transport_id),
		cmd->sess->tgt->rel_tgt_id, reg, be64_to_cpu(key), tgt_dev);

	/* We already checked reg is not NULL */
	if (reg->key != key) {
		TRACE_PR("Registrant's %p key %016llx mismatch with %016llx",
			reg, be64_to_cpu(reg->key), be64_to_cpu(key));
		scst_set_cmd_error_status(cmd, SAM_STAT_RESERVATION_CONFLICT);
		goto out;
	}

	scst_pr_send_ua_all(dev, reg,
		SCST_LOAD_SENSE(scst_sense_reservation_preempted));

	list_for_each_entry_safe(r, t, &dev->dev_registrants_list,
					dev_registrants_list_entry) {
		scst_pr_remove_registrant(dev, r);
	}

	dev->pr_generation++;

	scst_pr_dump_prs(dev, false);

out:
	TRACE_EXIT();
	return;
}

static void scst_pr_do_preempt(struct scst_cmd *cmd, uint8_t *buffer,
	int buffer_size, bool abort)
{
	__be64 key, action_key;
	int scope, type;
	struct scst_device *dev = cmd->dev;
	struct scst_tgt_dev *tgt_dev = cmd->tgt_dev;
	struct scst_dev_registrant *reg, *r, *rt;
	int existing_pr_type = dev->pr_type;
	int existing_pr_scope = dev->pr_scope;
	LIST_HEAD(preempt_list);

	TRACE_ENTRY();

	if (buffer_size != 24) {
		TRACE_PR("Invalid buffer size %d", buffer_size);
		scst_set_cmd_error(cmd,
			SCST_LOAD_SENSE(scst_sense_parameter_list_length_invalid));
		goto out;
	}

	key = get_unaligned((__be64 *)&buffer[0]);
	action_key = get_unaligned((__be64 *)&buffer[8]);
	scope = cmd->cdb[2] >> 4;
	type = cmd->cdb[2] & 0x0f;

	if (!scst_pr_type_valid(type)) {
		TRACE_PR("Invalid reservation type %d", type);
		scst_set_invalid_field_in_cdb(cmd, 1,
			SCST_INVAL_FIELD_BIT_OFFS_VALID | 0);
		goto out;
	}

	reg = tgt_dev->registrant;

	TRACE_PR("Preempt%s: initiator %s/%d (%p), key %016llx, action_key "
		"%016llx, scope %x type %x (tgt_dev %p)",
		abort ? " and abort" : "",
		debug_transport_id_to_initiator_name(cmd->sess->transport_id),
		cmd->sess->tgt->rel_tgt_id, reg, be64_to_cpu(key),
		be64_to_cpu(action_key), scope, type, tgt_dev);

	/* We already checked reg is not NULL */
	if (reg->key != key) {
		TRACE_PR("Registrant's %p key %016llx mismatch with %016llx",
			reg, be64_to_cpu(reg->key), be64_to_cpu(key));
		scst_set_cmd_error_status(cmd, SAM_STAT_RESERVATION_CONFLICT);
		goto out;
	}

	if (!dev->pr_is_set) {
		scst_pr_find_registrants_list_key(dev, action_key,
			&preempt_list);
		if (list_empty(&preempt_list))
			goto out_error;
		list_for_each_entry_safe(r, rt, &preempt_list, aux_list_entry) {
			if (abort)
				scst_pr_abort_reg(dev, cmd, r);
			if (r != reg) {
				scst_pr_send_ua_reg(dev, r, SCST_LOAD_SENSE(
					scst_sense_registrations_preempted));
				scst_pr_remove_registrant(dev, r);
			}
		}
		goto done;
	}

	if (dev->pr_type == TYPE_WRITE_EXCLUSIVE_ALL_REG ||
	    dev->pr_type == TYPE_EXCLUSIVE_ACCESS_ALL_REG) {
		if (action_key == 0) {
			scst_pr_find_registrants_list_all(dev, reg,
				&preempt_list);
			list_for_each_entry_safe(r, rt, &preempt_list,
					aux_list_entry) {
				sBUG_ON(r == reg);
				if (abort)
					scst_pr_abort_reg(dev, cmd, r);
				scst_pr_send_ua_reg(dev, r,
					SCST_LOAD_SENSE(
						scst_sense_registrations_preempted));
				scst_pr_remove_registrant(dev, r);
			}
			scst_pr_set_holder(dev, reg, scope, type);
		} else {
			scst_pr_find_registrants_list_key(dev, action_key,
				&preempt_list);
			if (list_empty(&preempt_list))
				goto out_error;
			list_for_each_entry_safe(r, rt, &preempt_list,
					aux_list_entry) {
				if (abort)
					scst_pr_abort_reg(dev, cmd, r);
				if (r != reg) {
					scst_pr_send_ua_reg(dev, r,
						SCST_LOAD_SENSE(
							scst_sense_registrations_preempted));
					scst_pr_remove_registrant(dev, r);
				}
			}
		}
		goto done;
	}

	if (dev->pr_holder->key != action_key) {
		if (action_key == 0) {
			scst_set_invalid_field_in_parm_list(cmd, 8, 0);
			goto out;
		} else {
			scst_pr_find_registrants_list_key(dev, action_key,
				&preempt_list);
			if (list_empty(&preempt_list))
				goto out_error;
			list_for_each_entry_safe(r, rt, &preempt_list,
					aux_list_entry) {
				if (abort)
					scst_pr_abort_reg(dev, cmd, r);
				if (r != reg)
					scst_pr_send_ua_reg(dev, r,
						SCST_LOAD_SENSE(
							scst_sense_registrations_preempted));
				scst_pr_remove_registrant(dev, r);
			}
			goto done;
		}
	}

	scst_pr_find_registrants_list_key(dev, action_key,
		&preempt_list);

	list_for_each_entry_safe(r, rt, &preempt_list, aux_list_entry) {
		if (abort)
			scst_pr_abort_reg(dev, cmd, r);
		if (r != reg) {
			scst_pr_send_ua_reg(dev, r, SCST_LOAD_SENSE(
				scst_sense_registrations_preempted));
			scst_pr_remove_registrant(dev, r);
		}
	}

	scst_pr_set_holder(dev, reg, scope, type);

	if (existing_pr_type != type || existing_pr_scope != scope) {
		list_for_each_entry(r, &dev->dev_registrants_list,
					dev_registrants_list_entry) {
			if (r != reg)
				scst_pr_send_ua_reg(dev, r, SCST_LOAD_SENSE(
					scst_sense_reservation_released));
		}
	}

done:
	dev->pr_generation++;

	scst_pr_dump_prs(dev, false);

out:
	TRACE_EXIT();
	return;

out_error:
	TRACE_PR("Invalid key %016llx", be64_to_cpu(action_key));
	scst_set_cmd_error_status(cmd, SAM_STAT_RESERVATION_CONFLICT);
	goto out;
}

/* Called with dev_pr_mutex locked, no IRQ */
void scst_pr_preempt(struct scst_cmd *cmd, uint8_t *buffer, int buffer_size)
{
	TRACE_ENTRY();

	scst_assert_pr_mutex_held(cmd->dev);

	scst_pr_do_preempt(cmd, buffer, buffer_size, false);

	TRACE_EXIT();
	return;
}

static void scst_cmd_done_pr_preempt(struct scst_cmd *cmd, int next_state,
	enum scst_exec_context pref_context)
{
	void (*saved_cmd_done)(struct scst_cmd *cmd, int next_state,
		enum scst_exec_context pref_context);

	TRACE_ENTRY();

	if (!atomic_dec_and_test(&cmd->pr_abort_counter->pr_abort_pending_cnt))
		goto out;

	saved_cmd_done = cmd->pr_abort_counter->saved_cmd_done;
	kfree(cmd->pr_abort_counter);
	cmd->pr_abort_counter = NULL;

	saved_cmd_done(cmd, next_state, pref_context);

out:
	TRACE_EXIT();
	return;
}

/*
 * Called with dev_pr_mutex locked, no IRQ. Expects session_list_lock
 * not locked
 */
void scst_pr_preempt_and_abort(struct scst_cmd *cmd, uint8_t *buffer,
	int buffer_size)
{
	TRACE_ENTRY();

	scst_assert_pr_mutex_held(cmd->dev);

	cmd->pr_abort_counter = kzalloc(sizeof(*cmd->pr_abort_counter),
		GFP_KERNEL);
	if (cmd->pr_abort_counter == NULL) {
		PRINT_ERROR("Unable to allocate PR abort counter (size %zd)",
			sizeof(*cmd->pr_abort_counter));
		scst_set_busy(cmd);
		goto out;
	}

	/* 1 to protect cmd from be done by the TM thread too early */
	atomic_set(&cmd->pr_abort_counter->pr_abort_pending_cnt, 1);
	atomic_set(&cmd->pr_abort_counter->pr_aborting_cnt, 1);
	init_completion(&cmd->pr_abort_counter->pr_aborting_cmpl);

	cmd->pr_abort_counter->saved_cmd_done = cmd->scst_cmd_done;
	cmd->scst_cmd_done = scst_cmd_done_pr_preempt;

	scst_pr_do_preempt(cmd, buffer, buffer_size, true);

	if (!atomic_dec_and_test(&cmd->pr_abort_counter->pr_aborting_cnt))
		wait_for_completion(&cmd->pr_abort_counter->pr_aborting_cmpl);

out:
	TRACE_EXIT();
	return;
}

/* Checks if this is a Compatible Reservation Handling (CRH) case */
bool scst_pr_crh_case(struct scst_cmd *cmd)
{
	bool allowed;
	struct scst_device *dev = cmd->dev;
	struct scst_tgt_dev *tgt_dev = cmd->tgt_dev;
	struct scst_dev_registrant *reg;
	uint8_t type;

	TRACE_ENTRY();

	TRACE_DBG("Test if there is a CRH case for command %s (%s) from "
		"%s", cmd->op_name, scst_get_opcode_name(cmd),
		cmd->sess->initiator_name);

	if (!dev->pr_is_set) {
		TRACE_PR("%s", "PR not set");
		allowed = false;
		goto out;
	}

	reg = tgt_dev->registrant;
	type = dev->pr_type;

	switch (type) {
	case TYPE_WRITE_EXCLUSIVE:
	case TYPE_EXCLUSIVE_ACCESS:
		WARN_ON(dev->pr_holder == NULL);
		if (reg == dev->pr_holder)
			allowed = true;
		else
			allowed = false;
		break;

	case TYPE_WRITE_EXCLUSIVE_REGONLY:
	case TYPE_EXCLUSIVE_ACCESS_REGONLY:
	case TYPE_WRITE_EXCLUSIVE_ALL_REG:
	case TYPE_EXCLUSIVE_ACCESS_ALL_REG:
		allowed = (reg != NULL);
		break;

	default:
		PRINT_ERROR("Invalid PR type %x", type);
		allowed = false;
		break;
	}

	if (!allowed)
		TRACE_PR("Command %s (%s) from %s rejected due to not CRH "
			"reservation", cmd->op_name, scst_get_opcode_name(cmd),
			cmd->sess->initiator_name);
	else
		TRACE_DBG("Command %s (%s) from %s is allowed to execute "
			"due to CRH", cmd->op_name, scst_get_opcode_name(cmd),
			cmd->sess->initiator_name);

out:
	TRACE_EXIT_RES(allowed);
	return allowed;

}

/* Check if command allowed in presence of reservation */
bool scst_pr_is_cmd_allowed(struct scst_cmd *cmd)
{
	bool allowed;
	struct scst_device *dev = cmd->dev;
	struct scst_tgt_dev *tgt_dev = cmd->tgt_dev;
	struct scst_dev_registrant *reg;
	uint8_t type;

	TRACE_ENTRY();

	scst_pr_read_lock(dev);

	TRACE_DBG("Testing if command %s (%s) from %s allowed to execute",
		cmd->op_name, scst_get_opcode_name(cmd), cmd->sess->initiator_name);

	/* Recheck, because it can change while we were waiting for the lock */
	if (unlikely(!dev->pr_is_set)) {
		allowed = true;
		goto out_unlock;
	}

	reg = tgt_dev->registrant;
	type = dev->pr_type;

	switch (type) {
	case TYPE_WRITE_EXCLUSIVE:
		if (reg && reg == dev->pr_holder)
			allowed = true;
		else
			allowed = (cmd->op_flags & SCST_WRITE_EXCL_ALLOWED) != 0;
		break;

	case TYPE_EXCLUSIVE_ACCESS:
		if (reg && reg == dev->pr_holder)
			allowed = true;
		else
			allowed = (cmd->op_flags & SCST_EXCL_ACCESS_ALLOWED) != 0;
		break;

	case TYPE_WRITE_EXCLUSIVE_REGONLY:
	case TYPE_WRITE_EXCLUSIVE_ALL_REG:
		if (reg)
			allowed = true;
		else
			allowed = (cmd->op_flags & SCST_WRITE_EXCL_ALLOWED) != 0;
		break;

	case TYPE_EXCLUSIVE_ACCESS_REGONLY:
	case TYPE_EXCLUSIVE_ACCESS_ALL_REG:
		if (reg)
			allowed = true;
		else
			allowed = (cmd->op_flags & SCST_EXCL_ACCESS_ALLOWED) != 0;
		break;

	default:
		PRINT_ERROR("Invalid PR type %x", type);
		allowed = false;
		break;
	}

	if (!allowed)
		TRACE_PR("Command %s (%s) from %s rejected due "
			"to PR", cmd->op_name, scst_get_opcode_name(cmd),
			cmd->sess->initiator_name);
	else
		TRACE_DBG("Command %s (%s) from %s is allowed to execute",
			cmd->op_name, scst_get_opcode_name(cmd),
			cmd->sess->initiator_name);

out_unlock:
	scst_pr_read_unlock(dev);

	TRACE_EXIT_RES(allowed);
	return allowed;
}

/* Called with dev_pr_mutex locked, no IRQ */
void scst_pr_read_keys(struct scst_cmd *cmd, uint8_t *buffer, int buffer_size)
{
	int offset = 0, size, size_max;
	struct scst_device *dev = cmd->dev;
	struct scst_dev_registrant *reg;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	if (buffer_size < 8) {
		TRACE_PR("buffer_size too small: %d. expected >= 8 "
			"(buffer %p)", buffer_size, buffer);
		goto skip;
	}

	TRACE_PR("Read Keys (dev %s): PRGen %d", dev->virt_name,
		dev->pr_generation);

	put_unaligned_be32(dev->pr_generation, &buffer[0]);

	offset = 8;
	size = 0;
	size_max = buffer_size - 8;

	list_for_each_entry(reg, &dev->dev_registrants_list,
				dev_registrants_list_entry) {
		if (size_max - size >= 8) {
			TRACE_PR("Read Keys (dev %s): key 0x%llx",
				dev->virt_name, reg->key);

			WARN_ON(reg->key == 0);

			put_unaligned(reg->key,
				(__be64 *)&buffer[offset]);

			offset += 8;
		}
		size += 8;
	}

	put_unaligned_be32(size, &buffer[4]);

skip:
	scst_set_resp_data_len(cmd, offset);

	TRACE_EXIT();
	return;
}

/* Called with dev_pr_mutex locked, no IRQ */
void scst_pr_read_reservation(struct scst_cmd *cmd, uint8_t *buffer,
	int buffer_size)
{
	struct scst_device *dev = cmd->dev;
	uint8_t b[24] = { };
	int size = 0;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	if (buffer_size < 8) {
		TRACE_PR("buffer_size too small: %d. expected >= 8 "
			"(buffer %p)", buffer_size, buffer);
		goto out;
	}

	put_unaligned_be32(dev->pr_generation, &b[0]);

	if (!dev->pr_is_set) {
		TRACE_PR("Read Reservation: no reservations for dev %s",
			dev->virt_name);
		b[4] =
		b[5] =
		b[6] =
		b[7] = 0;

		size = 8;
	} else {
		__be64 key = dev->pr_holder ? dev->pr_holder->key : 0;

		TRACE_PR("Read Reservation: dev %s, holder %p, key 0x%llx, "
			"scope %d, type %d", dev->virt_name, dev->pr_holder,
			be64_to_cpu(key), dev->pr_scope, dev->pr_type);

		b[4] =
		b[5] =
		b[6] = 0;
		b[7] = 0x10;

		put_unaligned(key, (__be64 *)&b[8]);
		b[21] = dev->pr_scope << 4 | dev->pr_type;

		size = 24;
	}

out:
	size = min(size, buffer_size);
	memcpy(buffer, b, size);
	memset(buffer + size, 0, buffer_size - size);
	scst_set_resp_data_len(cmd, size);

	TRACE_EXIT();
	return;
}

/* Called with dev_pr_mutex locked, no IRQ */
void scst_pr_report_caps(struct scst_cmd *cmd, uint8_t *buffer, int buffer_size)
{
	int offset = 0;
	unsigned int crh = 1;
	unsigned int atp_c = 1;
	unsigned int sip_c = 1;
	unsigned int ptpl_c = 1;
	struct scst_device *dev = cmd->dev;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(dev);

	if (buffer_size < 8) {
		TRACE_PR("buffer_size too small: %d. expected >= 8 "
			"(buffer %p)", buffer_size, buffer);
		goto skip;
	}

	TRACE_PR("Reporting capabilities (dev %s):  crh %x, sip_c %x, "
		"atp_c %x, ptpl_c %x, pr_aptpl %x", dev->virt_name,
		crh, sip_c, atp_c, ptpl_c, dev->pr_aptpl);

	buffer[0] = 0;
	buffer[1] = 8;

	buffer[2] = crh << 4 | sip_c << 3 | atp_c << 2 | ptpl_c;
	buffer[3] = (1 << 7) | (4 << 4) | (dev->pr_aptpl > 0 ? 1 : 0);

	/* All commands supported */
	buffer[4] = 0xEA;
	buffer[5] = 0x1;

	offset += 8;

skip:
	scst_set_resp_data_len(cmd, offset);

	TRACE_EXIT();
	return;
}

/* Called with dev_pr_mutex locked, no IRQ */
void scst_pr_read_full_status(struct scst_cmd *cmd, uint8_t *buffer,
	int buffer_size)
{
	int offset = 0, size, size_max;
	struct scst_device *dev = cmd->dev;
	struct scst_dev_registrant *reg;

	TRACE_ENTRY();

	scst_assert_pr_mutex_held(cmd->dev);

	if (buffer_size < 8)
		goto skip;

	put_unaligned_be32(dev->pr_generation, &buffer[0]);
	offset += 8;

	size = 0;
	size_max = buffer_size - 8;

	list_for_each_entry(reg, &dev->dev_registrants_list,
				dev_registrants_list_entry) {
		const uint32_t ts = scst_tid_size(reg->transport_id);
		const uint32_t rec_len = 24 + ts;

		if (size_max - size > rec_len) {
			memset(&buffer[offset], 0, rec_len);

			put_unaligned(reg->key, (__be64 *)(&buffer[offset]));

			if (dev->pr_is_set && scst_pr_is_holder(dev, reg)) {
				buffer[offset + 12] = 1;
				buffer[offset + 13] = (dev->pr_scope << 4) | dev->pr_type;
			}

			put_unaligned_be16(reg->rel_tgt_id,
					   &buffer[offset + 18]);
			put_unaligned_be32(ts, &buffer[offset + 20]);

			memcpy(&buffer[offset + 24], reg->transport_id, ts);

			offset += rec_len;
		}
		size += rec_len;
	}

	put_unaligned_be32(size, &buffer[4]);

skip:
	scst_set_resp_data_len(cmd, offset);

	TRACE_EXIT();
	return;
}
