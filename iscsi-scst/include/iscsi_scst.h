/*
 *  Copyright (C) 2007 - 2018 Vladislav Bolkhovitin
 *  Copyright (C) 2007 - 2018 Western Digital Corporation
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

#ifndef _ISCSI_SCST_U_H
#define _ISCSI_SCST_U_H

#ifdef __KERNEL__
#include <linux/uaccess.h>  /* mm_segment_t */
#include <linux/version.h>

/* <asm/uaccess.h> */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
/*
 * With kernel 5.9 or older, certain kernel functions, e.g. setsockopt(),
 * sock_recvmsg() only accept a pointer to a user space buffer. With kernel
 * v5.10 these functions accept a sockptr_t, a data structure that includes
 * information about the address space of a pointer. See also commit
 * 47058bb54b57 ("x86: remove address space overrides using set_fs()"). See
 * also https://lwn.net/Articles/832121/. The definitions below make it easy
 * to write kernel code that is compatible with all kernel versions.
 */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
/*
 * Backport mm_segment_t to save compatibility with older kernel versions.
 *
 * mm_segment_t was dropped in commit
 * 967747bbc084 ("uaccess: remove CONFIG_SET_FS") # v5.18
 */
typedef struct {
	/* empty dummy */
} mm_segment_t;
#endif

#define KERNEL_DS ((mm_segment_t) { })
static inline mm_segment_t get_fs(void) { return ((mm_segment_t) { }); }
static inline void set_fs(mm_segment_t seg) { }
#endif
#endif

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <sys/uio.h>
#define aligned_u64 uint64_t __attribute__((aligned(8)))
#endif

#include "iscsi_scst_ver.h"
#include "iscsi_scst_itf_ver.h"

/* The maximum length of 223 bytes in the RFC. */
#define ISCSI_NAME_LEN		256

#define ISCSI_PORTAL_LEN	64

/* Full name is iSCSI name + connected portal */
#define ISCSI_FULL_NAME_LEN	(ISCSI_NAME_LEN + ISCSI_PORTAL_LEN)

#define ISCSI_LISTEN_PORT	3260

#define SCSI_ID_LEN		24

#define ISCSI_MAX_ATTR_NAME_LEN		50
#define ISCSI_MAX_ATTR_VALUE_LEN	512

enum {
	key_initial_r2t,
	key_immediate_data,
	key_max_connections,
	key_max_recv_data_length,
	key_max_xmit_data_length,
	key_max_burst_length,
	key_first_burst_length,
	key_default_wait_time,
	key_default_retain_time,
	key_max_outstanding_r2t,
	key_data_pdu_inorder,
	key_data_sequence_inorder,
	key_error_recovery_level,
	key_header_digest,
	key_data_digest,
	key_ofmarker,
	key_ifmarker,
	key_ofmarkint,
	key_ifmarkint,
	key_rdma_extensions,
	key_target_recv_data_length,
	key_initiator_recv_data_length,
	key_max_ahs_length,
	key_tagged_buffer_for_solicited_data_only,
	key_iser_hello_required,
	key_max_outstanding_unexpected_pdus,
	session_key_last,
};

enum {
	key_queued_cmnds,
	key_rsp_timeout,
	key_nop_in_interval,
	key_nop_in_timeout,
	key_max_sessions,
	target_key_last,
};

enum {
	key_session,
	key_target,
};

struct iscsi_kern_target_info {
	u32 tid;
	u32 cookie;
	char name[ISCSI_NAME_LEN];
	u32 attrs_num;
	aligned_u64 attrs_ptr;
};

struct iscsi_kern_session_info {
	u32 tid;
	aligned_u64 sid;
	char initiator_name[ISCSI_NAME_LEN];
	char full_initiator_name[ISCSI_FULL_NAME_LEN];
	u32 exp_cmd_sn;
	s32 session_params[session_key_last];
	s32 target_params[target_key_last];
};

#define DIGEST_ALL		(DIGEST_NONE | DIGEST_CRC32C)
#define DIGEST_NONE		(1 << 0)
#define DIGEST_CRC32C           (1 << 1)

struct iscsi_kern_conn_info {
	u32 tid;
	aligned_u64 sid;

	u32 cid;
	u32 stat_sn;
	u32 exp_stat_sn;
	int fd;
};

struct iscsi_kern_attr {
	u32 mode;
	char name[ISCSI_MAX_ATTR_NAME_LEN];
};

struct iscsi_kern_mgmt_cmd_res_info {
	u32 tid;
	u32 cookie;
	u32 req_cmd;
	u32 result;
	char value[ISCSI_MAX_ATTR_VALUE_LEN];
};

struct iscsi_kern_params_info {
	u32 tid;
	aligned_u64 sid;

	u32 params_type;
	u32 partial;

	s32 session_params[session_key_last];
	s32 target_params[target_key_last];
};

enum iscsi_kern_event_code {
	E_ADD_TARGET,
	E_DEL_TARGET,
	E_MGMT_CMD,
	E_ENABLE_TARGET,
	E_DISABLE_TARGET,
	E_GET_ATTR_VALUE,
	E_SET_ATTR_VALUE,
	E_CONN_CLOSE,
};

struct iscsi_kern_event {
	u32 tid;
	aligned_u64 sid;
	u32 cid;
	u32 code;
	u32 cookie;
	u32 param1_size;
	u32 param2_size;
};

struct iscsi_kern_register_info {
	union {
		aligned_u64 version;
		struct {
			int max_data_seg_len;
			int max_queued_cmds;
		};
	};
};

struct iscsi_kern_attr_info {
	u32 tid;
	u32 cookie;
	struct iscsi_kern_attr attr;
};

struct iscsi_kern_initiator_info {
	u32 tid;
	char full_initiator_name[ISCSI_FULL_NAME_LEN];
};

#define	DEFAULT_NR_QUEUED_CMNDS	32
#define	MIN_NR_QUEUED_CMNDS	1
#define	MAX_NR_QUEUED_CMNDS	2048

#define DEFAULT_RSP_TIMEOUT	90
#define MIN_RSP_TIMEOUT		2
#define MAX_RSP_TIMEOUT		65535

#define DEFAULT_NOP_IN_INTERVAL 30
#define MIN_NOP_IN_INTERVAL	0
#define MAX_NOP_IN_INTERVAL	65535

#define DEFAULT_NOP_IN_TIMEOUT  30
#define MIN_NOP_IN_TIMEOUT	2
#define MAX_NOP_IN_TIMEOUT	65535

#define NETLINK_ISCSI_SCST	25

#define REGISTER_USERD		_IOWR('s', 0, struct iscsi_kern_register_info)
#define ADD_TARGET		_IOW('s', 1, struct iscsi_kern_target_info)
#define DEL_TARGET		_IOW('s', 2, struct iscsi_kern_target_info)
#define ADD_SESSION		_IOW('s', 3, struct iscsi_kern_session_info)
#define DEL_SESSION		_IOW('s', 4, struct iscsi_kern_session_info)
#define ADD_CONN		_IOW('s', 5, struct iscsi_kern_conn_info)
#define DEL_CONN		_IOW('s', 6, struct iscsi_kern_conn_info)
#define ISCSI_PARAM_SET		_IOW('s', 7, struct iscsi_kern_params_info)
#define ISCSI_PARAM_GET		_IOWR('s', 8, struct iscsi_kern_params_info)

#define ISCSI_ATTR_ADD		_IOW('s', 9, struct iscsi_kern_attr_info)
#define ISCSI_ATTR_DEL		_IOW('s', 10, struct iscsi_kern_attr_info)
#define MGMT_CMD_CALLBACK	_IOW('s', 11, struct iscsi_kern_mgmt_cmd_res_info)

#define ISCSI_INITIATOR_ALLOWED	_IOW('s', 12, struct iscsi_kern_initiator_info)

static inline int iscsi_is_key_internal(int key)
{
	switch (key) {
	case key_max_xmit_data_length:
		return 1;
	default:
		return 0;
	}
}

#endif
