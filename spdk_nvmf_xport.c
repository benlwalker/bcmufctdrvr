/*
 *   BSD LICENSE
 *
 *   Copyright (c) 2018 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/env.h"
#include "spdk/assert.h"
#include "spdk/nvmf.h"
#include "spdk/endian.h"
#include "spdk/nvmf_spec.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/event.h"
#include "spdk/likely.h"
#include "spdk/trace.h"
#include "spdk_internal/log.h"
#include "spdk_nvmf_xport.h"
#include "fc_subsystem.h"
#include "fc.h"
#include "ocs.h"
#include "spdk/barrier.h"

/*
 * Broadcom FC SLI-4 definitions
 */

#define BCM_MAJOR_CODE_STANDARD 0
#define BCM_MAJOR_CODE_SENTINEL 1

#define BCM_HWQP(hq) ((struct bcm_nvmf_hw_queues *)((hq)->queues))

/* Note: this define should be in sync with FCNVME_MAX_LS_BUFFER_SIZE in fc_nvme_spec.h */
#define BCM_RQ_BUFFER_SIZE  2048

#define BCM_MAX_IOVECS (SPDK_NVMF_MAX_SGL_ENTRIES + 2) /* 2 for skips */

#define BCM_CQE_CODE_OFFSET	14

#define PTR_TO_ADDR32_HI(x)  (uint32_t)((uint64_t)(x & 0xFFFFFFFF00000000LL) >> 32);
#define PTR_TO_ADDR32_LO(x)  (uint32_t)((uint64_t)x & 0x0FFFFFFFFLL);

/* FC CQ Event Codes */
#define BCM_CQE_CODE_WORK_REQUEST_COMPLETION    0x01
#define BCM_CQE_CODE_RELEASE_WQE                0x02
#define BCM_CQE_CODE_RQ_ASYNC                   0x04
#define BCM_CQE_CODE_XRI_ABORTED                0x05
#define BCM_CQE_CODE_RQ_COALESCING              0x06
#define BCM_CQE_CODE_RQ_CONSUMPTION             0x07
#define BCM_CQE_CODE_MEASUREMENT_REPORTING      0x08
#define BCM_CQE_CODE_RQ_ASYNC_V1                0x09
#define BCM_CQE_CODE_OPTIMIZED_WRITE_CMD        0x0B
#define BCM_CQE_CODE_OPTIMIZED_WRITE_DATA       0x0C
#define BCM_CQE_CODE_NVME_ERSP_COMPLETION       0x0D
#define BCM_CQE_CODE_RQ_MARKER                  0x1D

/* FC Completion Status Codes */
#define BCM_FC_WCQE_STATUS_SUCCESS              0x00
#define BCM_FC_WCQE_STATUS_FCP_RSP_FAILURE      0x01
#define BCM_FC_WCQE_STATUS_REMOTE_STOP          0x02
#define BCM_FC_WCQE_STATUS_LOCAL_REJECT         0x03
#define BCM_FC_WCQE_STATUS_NPORT_RJT            0x04
#define BCM_FC_WCQE_STATUS_FABRIC_RJT           0x05
#define BCM_FC_WCQE_STATUS_NPORT_BSY            0x06
#define BCM_FC_WCQE_STATUS_FABRIC_BSY           0x07
#define BCM_FC_WCQE_STATUS_LS_RJT               0x09
#define BCM_FC_WCQE_STATUS_RX_BUFF_OVERRUN      0x0a
#define BCM_FC_WCQE_STATUS_CMD_REJECT           0x0b
#define BCM_FC_WCQE_STATUS_FCP_TGT_LENCHECK     0x0c
#define BCM_FC_WCQE_STATUS_RQ_BUF_LEN_EXCEEDED  0x11
#define BCM_FC_WCQE_STATUS_RQ_INSUFF_BUF_NEEDED 0x12
#define BCM_FC_WCQE_STATUS_RQ_INSUFF_FRM_DISC   0x13
#define BCM_FC_WCQE_STATUS_RQ_DMA_FAILURE       0x14
#define BCM_FC_WCQE_STATUS_FCP_RSP_TRUNCATE     0x15
#define BCM_FC_WCQE_STATUS_DI_ERROR             0x16
#define BCM_FC_WCQE_STATUS_BA_RJT               0x17
#define BCM_FC_WCQE_STATUS_RQ_INSUFF_XRI_NEEDED 0x18
#define BCM_FC_WCQE_STATUS_RQ_INSUFF_XRI_DISC   0x19
#define BCM_FC_WCQE_STATUS_RX_ERROR_DETECT      0x1a
#define BCM_FC_WCQE_STATUS_RX_ABORT_REQUEST     0x1b

/* Driver generated status codes; better not overlap with chip's status codes! */
#define BCM_FC_WCQE_STATUS_TARGET_WQE_TIMEOUT  	0xff
#define BCM_FC_WCQE_STATUS_SHUTDOWN            	0xfe
#define BCM_FC_WCQE_STATUS_DISPATCH_ERROR      	0xfd

#define SLI4_FC_COALESCE_RQ_SUCCESS		0x10
#define SLI4_FC_COALESCE_RQ_INSUFF_XRI_NEEDED	0x18


/* DI_ERROR Extended Status */
#define BCM_FC_DI_ERROR_GE     (1 << 0) /* Guard Error */
#define BCM_FC_DI_ERROR_AE     (1 << 1) /* Application Tag Error */
#define BCM_FC_DI_ERROR_RE     (1 << 2) /* Reference Tag Error */
#define BCM_FC_DI_ERROR_TDPV   (1 << 3) /* Total Data Placed Valid */
#define BCM_FC_DI_ERROR_UDB    (1 << 4) /* Uninitialized DIF Block */
#define BCM_FC_DI_ERROR_EDIR   (1 << 5) /* Error direction */

/* Local Reject Reason Codes */
#define BCM_FC_LOCAL_REJECT_MISSING_CONTINUE       0x01
#define BCM_FC_LOCAL_REJECT_SEQUENCE_TIMEOUT       0x02
#define BCM_FC_LOCAL_REJECT_INTERNAL_ERROR         0x03
#define BCM_FC_LOCAL_REJECT_INVALID_RPI            0x04
#define BCM_FC_LOCAL_REJECT_NO_XRI                 0x05
#define BCM_FC_LOCAL_REJECT_ILLEGAL_COMMAND        0x06
#define BCM_FC_LOCAL_REJECT_XCHG_DROPPED           0x07
#define BCM_FC_LOCAL_REJECT_ILLEGAL_FIELD          0x08
#define BCM_FC_LOCAL_REJECT_NO_ABORT_MATCH         0x0c
#define BCM_FC_LOCAL_REJECT_TX_DMA_FAILED          0x0d
#define BCM_FC_LOCAL_REJECT_RX_DMA_FAILED          0x0e
#define BCM_FC_LOCAL_REJECT_ILLEGAL_FRAME          0x0f
#define BCM_FC_LOCAL_REJECT_NO_RESOURCES           0x11
#define BCM_FC_LOCAL_REJECT_FCP_CONF_FAILURE       0x12
#define BCM_FC_LOCAL_REJECT_ILLEGAL_LENGTH         0x13
#define BCM_FC_LOCAL_REJECT_UNSUPPORTED_FEATURE    0x14
#define BCM_FC_LOCAL_REJECT_ABORT_IN_PROGRESS      0x15
#define BCM_FC_LOCAL_REJECT_ABORT_REQUESTED        0x16
#define BCM_FC_LOCAL_REJECT_RCV_BUFFER_TIMEOUT     0x17
#define BCM_FC_LOCAL_REJECT_LOOP_OPEN_FAILURE      0x18
#define BCM_FC_LOCAL_REJECT_LINK_DOWN              0x1a
#define BCM_FC_LOCAL_REJECT_CORRUPTED_DATA         0x1b
#define BCM_FC_LOCAL_REJECT_CORRUPTED_RPI          0x1c
#define BCM_FC_LOCAL_REJECT_OUT_OF_ORDER_DATA      0x1d
#define BCM_FC_LOCAL_REJECT_OUT_OF_ORDER_ACK       0x1e
#define BCM_FC_LOCAL_REJECT_DUP_FRAME              0x1f
#define BCM_FC_LOCAL_REJECT_LINK_CONTROL_FRAME     0x20
#define BCM_FC_LOCAL_REJECT_BAD_HOST_ADDRESS       0x21
#define BCM_FC_LOCAL_REJECT_MISSING_HDR_BUFFER     0x23
#define BCM_FC_LOCAL_REJECT_MSEQ_CHAIN_CORRUPTED   0x24
#define BCM_FC_LOCAL_REJECT_ABORTMULT_REQUESTED    0x25
#define BCM_FC_LOCAL_REJECT_BUFFER_SHORTAGE        0x28
#define BCM_FC_LOCAL_REJECT_RCV_XRIBUF_WAITING     0x29
#define BCM_FC_LOCAL_REJECT_INVALID_VPI            0x2e
#define BCM_FC_LOCAL_REJECT_MISSING_XRIBUF         0x30
#define BCM_FC_LOCAL_REJECT_INVALID_RELOFFSET      0x40
#define BCM_FC_LOCAL_REJECT_MISSING_RELOFFSET      0x41
#define BCM_FC_LOCAL_REJECT_INSUFF_BUFFERSPACE     0x42
#define BCM_FC_LOCAL_REJECT_MISSING_SI             0x43
#define BCM_FC_LOCAL_REJECT_MISSING_ES             0x44
#define BCM_FC_LOCAL_REJECT_INCOMPLETE_XFER        0x45
#define BCM_FC_LOCAL_REJECT_SLER_FAILURE           0x46
#define BCM_FC_LOCAL_REJECT_SLER_CMD_RCV_FAILURE   0x47
#define BCM_FC_LOCAL_REJECT_SLER_REC_RJT_ERR       0x48
#define BCM_FC_LOCAL_REJECT_SLER_REC_SRR_RETRY_ERR 0x49
#define BCM_FC_LOCAL_REJECT_SLER_SRR_RJT_ERR       0x4a
#define BCM_FC_LOCAL_REJECT_SLER_RRQ_RJT_ERR       0x4c
#define BCM_FC_LOCAL_REJECT_SLER_RRQ_RETRY_ERR     0x4d
#define BCM_FC_LOCAL_REJECT_SLER_ABTS_ERR          0x4e

#define BCM_FC_ASYNC_RQ_SUCCESS			   0x10
#define BCM_FC_ASYNC_RQ_BUF_LEN_EXCEEDED	   0x11
#define BCM_FC_ASYNC_RQ_INSUFF_BUF_NEEDED	   0x12
#define BCM_FC_ASYNC_RQ_INSUFF_BUF_FRM_DISC	   0x13
#define BCM_FC_ASYNC_RQ_DMA_FAILURE		   0x14

/**
 * Work Queue Entry (WQE) types.
 */
#define BCM_WQE_ABORT			0x0f
#define BCM_WQE_ELS_REQUEST64		0x8a
#define BCM_WQE_FCP_IBIDIR64		0xac
#define BCM_WQE_FCP_IREAD64		0x9a
#define BCM_WQE_FCP_IWRITE64		0x98
#define BCM_WQE_FCP_ICMND64		0x9c
#define BCM_WQE_FCP_TRECEIVE64		0xa1
#define BCM_WQE_FCP_CONT_TRECEIVE64	0xe5
#define BCM_WQE_FCP_TRSP64		0xa3
#define BCM_WQE_FCP_TSEND64		0x9f
#define BCM_WQE_GEN_REQUEST64		0xc2
#define BCM_WQE_SEND_FRAME		0xe1
#define BCM_WQE_XMIT_BCAST64		0X84
#define BCM_WQE_XMIT_BLS_RSP		0x97
#define BCM_WQE_ELS_RSP64		0x95
#define BCM_WQE_XMIT_SEQUENCE64		0x82
#define BCM_WQE_REQUEUE_XRI		0x93
#define BCM_WQE_MARKER			0xe6

/**
 * WQE command types.
 */
#define BCM_CMD_FCP_IREAD64_WQE		0x00
#define BCM_CMD_FCP_ICMND64_WQE		0x00
#define BCM_CMD_FCP_IWRITE64_WQE	0x01
#define BCM_CMD_FCP_TRECEIVE64_WQE	0x02
#define BCM_CMD_FCP_TRSP64_WQE		0x03
#define BCM_CMD_FCP_TSEND64_WQE		0x07
#define BCM_CMD_GEN_REQUEST64_WQE	0x08
#define BCM_CMD_XMIT_BCAST64_WQE	0x08
#define BCM_CMD_XMIT_BLS_RSP64_WQE	0x08
#define BCM_CMD_ABORT_WQE		0x08
#define BCM_CMD_XMIT_SEQUENCE64_WQE	0x08
#define BCM_CMD_REQUEUE_XRI_WQE		0x0A
#define BCM_CMD_SEND_FRAME_WQE		0x0a
#define BCM_CMD_MARKER_WQE		0x0a

#define BCM_WQE_SIZE			0x05
#define BCM_WQE_EXT_SIZE		0x06

#define BCM_WQE_BYTES			(16 * sizeof(uint32_t))
#define BCM_WQE_EXT_BYTES		(32 * sizeof(uint32_t))

#define BCM_ELS_REQUEST64_CONTEXT_RPI	0x0
#define BCM_ELS_REQUEST64_CONTEXT_VPI	0x1
#define BCM_ELS_REQUEST64_CONTEXT_VFI	0x2
#define BCM_ELS_REQUEST64_CONTEXT_FCFI	0x3

#define BCM_ELS_REQUEST64_CLASS_2	0x1
#define BCM_ELS_REQUEST64_CLASS_3	0x2

#define BCM_ELS_REQUEST64_DIR_WRITE	0x0
#define BCM_ELS_REQUEST64_DIR_READ	0x1

#define BCM_ELS_REQUEST64_OTHER		0x0
#define BCM_ELS_REQUEST64_LOGO		0x1
#define BCM_ELS_REQUEST64_FDISC		0x2
#define BCM_ELS_REQUEST64_FLOGIN	0x3
#define BCM_ELS_REQUEST64_PLOGI		0x4

#define BCM_ELS_REQUEST64_CMD_GEN		0x08
#define BCM_ELS_REQUEST64_CMD_NON_FABRIC	0x0c
#define BCM_ELS_REQUEST64_CMD_FABRIC		0x0d

#define BCM_IO_CONTINUATION		BIT(0)	/** The XRI associated with this IO is already active */
#define BCM_IO_AUTO_GOOD_RESPONSE	BIT(1)	/** Automatically generate a good RSP frame */
#define BCM_IO_NO_ABORT			BIT(2)
#define BCM_IO_DNRX			BIT(3)	/** Set the DNRX bit because no auto xref rdy buffer is posted */

/* Mask for ccp (CS_CTL) */
#define BCM_MASK_CCP	0xfe /* Upper 7 bits of CS_CTL is priority */

#define BCM_BDE_TYPE_BDE_64		0x00	/** Generic 64-bit data */
#define BCM_BDE_TYPE_BDE_IMM		0x01	/** Immediate data */
#define BCM_BDE_TYPE_BLP		0x40	/** Buffer List Pointer */

#define BCM_SGE_TYPE_DATA		0x00
#define BCM_SGE_TYPE_SKIP		0x0c

#define BCM_ABORT_CRITERIA_XRI_TAG      0x01

/* Support for sending ABTS in case of sequence errors */
#define BCM_SUPPORT_ABTS_FOR_SEQ_ERRORS		true

#define BCM_MARKER_CATAGORY_ALL_RQ		0x1
#define BCM_MARKER_CATAGORY_ALL_RQ_EXCEPT_ONE	0x2

/* FC CQE Types */
typedef enum {
	BCM_FC_QENTRY_ASYNC,
	BCM_FC_QENTRY_MQ,
	BCM_FC_QENTRY_RQ,
	BCM_FC_QENTRY_WQ,
	BCM_FC_QENTRY_WQ_RELEASE,
	BCM_FC_QENTRY_OPT_WRITE_CMD,
	BCM_FC_QENTRY_OPT_WRITE_DATA,
	BCM_FC_QENTRY_XABT,
	BCM_FC_QENTRY_NVME_ERSP,
	BCM_FC_QENTRY_MAX,         /* must be last */
} bcm_qentry_type_e;

typedef enum  {
	BCM_FC_QUEUE_TYPE_EQ,
	BCM_FC_QUEUE_TYPE_CQ_WQ,
	BCM_FC_QUEUE_TYPE_CQ_RQ,
	BCM_FC_QUEUE_TYPE_WQ,
	BCM_FC_QUEUE_TYPE_RQ_HDR,
	BCM_FC_QUEUE_TYPE_RQ_DATA,
} bcm_fc_queue_type_e;

/* SGE structure */
typedef struct bcm_sge {
	uint32_t	buffer_address_high;
	uint32_t	buffer_address_low;
	uint32_t	data_offset: 27,
			sge_type: 4,
			last: 1;
	uint32_t	buffer_length;
} bcm_sge_t;

#define BCM_SGE_SIZE sizeof(struct bcm_sge)

/* SLI BDE structure */
typedef struct bcm_bde {
	uint32_t	buffer_length: 24,
			bde_type: 8;
	union {
		struct {
			uint32_t buffer_address_low;
			uint32_t buffer_address_high;
		} data;
		struct {
			uint32_t offset;
			uint32_t rsvd2;
		} imm;
		struct {
			uint32_t sgl_segment_address_low;
			uint32_t sgl_segment_address_high;
		} blp;
	} u;
} bcm_bde_t;

/* FS-5 FC frame NVME header */
typedef struct fc_frame_hdr {
	uint32_t  r_ctl: 8,
		  d_id: 24;
	uint32_t  cs_ctl: 8,
		  s_id: 24;
	uint32_t  type: 8,
		  f_ctl: 24;
	uint32_t  seq_id: 8,
		  df_ctl: 8,
		  seq_cnt: 16;
	uint32_t  ox_id: 16,
		  rx_id: 16;
	uint32_t   parameter;

} fc_frame_hdr_t;

typedef struct fc_frame_hdr_le {
	uint32_t	d_id: 24,
			r_ctl: 8;
	uint32_t	s_id: 24,
			cs_ctl: 8;
	uint32_t	f_ctl: 24,
			type: 8;
	uint32_t	seq_cnt: 16,
			df_ctl: 8,
			seq_id: 8;
	uint32_t	rx_id: 16,
			ox_id: 16;
	uint32_t	parameter;

} fc_frame_hdr_le_t;

/* Doorbell register structure definitions */
typedef struct eqdoorbell {
	uint32_t eq_id            : 9;
	uint32_t ci               : 1;
	uint32_t qt               : 1;
	uint32_t eq_id_ext        : 5;
	uint32_t num_popped       : 13;
	uint32_t arm              : 1;
	uint32_t rsvd             : 1;
	uint32_t solicit_enable   : 1;
} eqdoorbell_t;

typedef struct cqdoorbell {
	uint32_t cq_id            : 10;
	uint32_t qt               : 1;
	uint32_t cq_id_ext        : 5;
	uint32_t num_popped       : 13;
	uint32_t arm              : 1;
	uint32_t rsvd             : 1;
	uint32_t solicit_enable   : 1;
} cqdoorbell_t;

typedef struct wqdoorbell {
	uint32_t wq_id          : 16;
	uint32_t wq_index       : 8;
	uint32_t num_posted     : 8;
} wqdoorbell_t;

typedef struct rqdoorbell {
	uint32_t rq_id          : 16;
	uint32_t num_posted     : 14;
	uint32_t rsvd           : 2;
} rqdoorbell_t;

typedef union doorbell_u {
	eqdoorbell_t eqdoorbell;
	cqdoorbell_t cqdoorbell;
	wqdoorbell_t wqdoorbell;
	rqdoorbell_t rqdoorbell;
	uint32_t     doorbell;
} doorbell_t;

/* EQE bit definition */
typedef struct eqe {
	uint32_t  valid        : 1;
	uint32_t  major_code   : 3;
	uint32_t  minor_code   : 12;
	uint32_t  resource_id  : 16;
} eqe_t;

/* CQE bit definitions */
typedef struct cqe {
	union {
		struct {
			uint32_t  word0;
			uint32_t  word1;
			uint32_t  word2;
			uint32_t  word3;
		} words;

		// Generic
		struct {
			uint8_t   hw_status;
			uint8_t   status;
			uint16_t  request_tag;
			union {
				uint32_t wqe_specific;
				uint32_t total_data_placed;
			} word1;

			uint32_t ext_status;
			uint32_t rsvd0          : 16;
			uint32_t event_code     : 8;
			uint32_t pri            : 3;
			uint32_t pv             : 1;
			uint32_t xb             : 1;
			uint32_t rha            : 1;
			uint32_t rsvd1          : 1;
			uint32_t valid          : 1;
		} generic;

		struct {
			uint32_t hw_status      : 8;
			uint32_t status         : 8;
			uint32_t request_tag    : 16;
			uint32_t wqe_specific_1;
			uint32_t wqe_specific_2;
			uint32_t rsvd0          : 15;
			uint32_t qx             : 1;
			uint32_t code           : 8;
			uint32_t pri            : 3;
			uint32_t pv             : 1;
			uint32_t xb             : 1;
			uint32_t rsvd1          : 2;
			uint32_t valid          : 1;
		} wcqe;

		// WQE Release CQE
		struct {
			uint32_t rsvd0;
			uint32_t rsvd1;
			uint32_t wqe_index      : 16;
			uint32_t wqe_id         : 16;
			uint32_t rsvd2          : 16;
			uint32_t event_code     : 8;
			uint32_t rsvd3          : 7;
			uint32_t valid          : 1;
		} release;

		struct {
			uint32_t rsvd0           : 8;
			uint32_t status          : 8;
			uint32_t rq_element_index: 12;
			uint32_t rsvd1           : 4;
			uint32_t rsvd2;
			uint32_t fcfi            : 6;
			uint32_t rq_id           : 10;
			uint32_t payload_data_placement_length: 16;
			uint32_t sof_byte        : 8;
			uint32_t eof_byte        : 8;
			uint32_t code            : 8;
			uint32_t header_data_placement_length: 6;
			uint32_t rsvd3           : 1;
			uint32_t valid           : 1;
		} async_rcqe;

		struct  {
			uint32_t rsvd0           : 8;
			uint32_t status          : 8;
			uint32_t rq_element_index: 15;
			uint32_t rsvd1           : 1;
			uint32_t fcfi            : 6;
			uint32_t rsvd2           : 26;
			uint32_t rq_id           : 16;
			uint32_t payload_data_placement_length: 16;
			uint32_t sof_byte        : 8;
			uint32_t eof_byte        : 8;
			uint32_t code            : 8;
			uint32_t header_data_placement_length: 6;
			uint32_t rsvd3           : 1;
			uint32_t valid           : 1;
		} async_rcqe_v1;

		struct {
			uint32_t rsvd0           : 8;
			uint32_t status          : 8;
			uint32_t rq_element_index: 15;
			uint32_t iv              : 1;
			uint32_t tag_lower;
			uint32_t tag_higher;
			uint32_t rq_id           : 16;
			uint32_t code            : 8;
			uint32_t rsvd1           : 7;
			uint32_t valid           : 1;
		} async_marker;

		struct {
			uint32_t rsvd0           : 8;
			uint32_t status          : 8;
			uint32_t rq_element_index: 15;
			uint32_t iv              : 1;
			uint32_t fcfi            : 6;
			uint32_t rsvd1           : 8;
			uint32_t oox             : 1;
			uint32_t agxr            : 1;
			uint32_t xri             : 16;
			uint32_t rq_id			 : 16;
			uint32_t payload_data_placement_length: 16;
			uint32_t rpi             : 16;
			uint32_t code            : 8;
			uint32_t header_data_placement_length: 6;
			uint32_t rsvd2           : 1;
			uint32_t valid           : 1;
		} optimized_write_cmd_cqe;

		struct  {
			uint32_t hw_status       : 8;
			uint32_t status          : 8;
			uint32_t xri             : 16;
			uint32_t total_data_placed;
			uint32_t extended_status;
			uint32_t rsvd0           : 16;
			uint32_t code            : 8;
			uint32_t pri             : 3;
			uint32_t pv              : 1;
			uint32_t xb              : 1;
			uint32_t rha             : 1;
			uint32_t rsvd1           : 1;
			uint32_t valid           : 1;
		} optimized_write_data_cqe;

		struct  {
			uint32_t rsvd0            : 8;
			uint32_t status           : 8;
			uint32_t rq_element_index : 12;
			uint32_t rsvd1            : 4;
			uint32_t rsvd2;
			uint32_t rq_id            : 16;
			uint32_t sequence_reporting_placement_length: 16;
			uint32_t rsvd3            : 16;
			uint32_t code             : 8;
			uint32_t rsvd4            : 7;
			uint32_t valid            : 1;
		} coalescing_rcqe;

		struct {
			uint32_t rsvd0           : 8;
			uint32_t status          : 8;
			uint32_t rsvd1           : 16;
			uint32_t extended_status;
			uint32_t xri             : 16;
			uint32_t remote_xid      : 16;
			uint32_t rsvd2           : 16;
			uint32_t code            : 8;
			uint32_t xr              : 1;
			uint32_t rsvd3           : 3;
			uint32_t eo              : 1;
			uint32_t br              : 1;
			uint32_t ia              : 1;
			uint32_t valid           : 1;
		} xri_aborted_cqe;

		struct {
			uint32_t rsvd0           : 32;
			uint32_t rsvd1           : 32;
			uint32_t wqe_index       : 16;
			uint32_t wq_id           : 16;
			uint32_t rsvd2           : 16;
			uint32_t code            : 8;
			uint32_t rsvd3           : 7;
			uint32_t valid           : 1;
		} wqec;

		// NVME ERSP CQE
		struct {
			uint16_t nvme_cqe_1;
			uint16_t request_tag;
			uint32_t nvme_cqe_0;
			uint32_t rsn;
			uint32_t sghd           : 16;
			uint32_t code           : 8;
			uint32_t pri            : 3;
			uint32_t pv             : 1;
			uint32_t xb             : 1;
			uint32_t rha            : 1;
			uint32_t rsvd           : 1;
			uint32_t valid          : 1;
		} ersp;

	} u;

} cqe_t;

/* CQE types */
typedef struct bcm_fc_async_rcqe {
	uint32_t rsvd0 : 8,
		 status: 8,
		 rq_element_index: 12,
		 rsvd1: 4;
	uint32_t rsvd2;
	uint32_t fcfi: 6,
		 rq_id: 10,
		 payload_data_placement_length: 16;
	uint32_t sof_byte: 8,
		 eof_byte: 8,
		 code: 8,
		 header_data_placement_length: 6,
		 rsvd3: 1,
		 vld: 1;

} bcm_fc_async_rcqe_t;

typedef struct bcm_fc_coalescing_rcqe {
	uint32_t rsvd0: 8,
		 status: 8,
		 rq_element_index: 12,
		 rsvd1: 4;
	uint32_t rsvd2;
	uint32_t rq_id: 16,
		 sequence_reporting_placement_length: 16;
	uint32_t rsvd3: 16,
		 code: 8,
		 rsvd4: 7,
		 vld: 1;
} bcm_fc_coalescing_rcqe_t;

typedef struct bcm_fc_async_rcqe_v1 {

	uint32_t rsvd0: 8,
		 status: 8,
		 rq_element_index: 15,
		 rsvd1: 1;
	uint32_t fcfi: 6,
		 rsvd2: 26;
	uint32_t rq_id: 16,
		 payload_data_placement_length: 16;
	uint32_t sof_byte: 8,
		 eof_byte: 8,
		 code: 8,
		 header_data_placement_length: 6,
		 rsvd3: 1,
		 vld: 1;

} bcm_fc_async_rcqe_v1_t;


typedef struct bcm_fc_async_rcqe_marker {
	uint32_t rsvd0: 8,
		 status: 8,
		 rq_element_index: 15,
		 rsvd1: 1;
	uint32_t tag_lower;
	uint32_t tag_higher;
	uint32_t rq_id: 16,
		 code: 8,
		 : 6,
		 : 1,
		 vld: 1;
} bcm_fc_async_rcqe_marker_t;

typedef struct bcm_fc_optimized_write_cmd_cqe {
	uint32_t rsvd0: 8,
		 status: 8,
		 rq_element_index: 15,
		 iv: 1;
	uint32_t fcfi: 6,
		 rsvd1: 8,
		 oox: 1,
		 agxr: 1,
		 xri: 16;
	uint32_t rq_id: 16,
		 payload_data_placement_length: 16;
	uint32_t rpi: 16,
		 code: 8,
		 header_data_placement_length: 6,
		 rsvd2: 1,
		 vld: 1;
} bcm_fc_optimized_write_cmd_cqe_t;

typedef struct bcm_fc_optimized_write_data_cqe {
	uint32_t hw_status: 8,
		 status: 8,
		 xri: 16;
	uint32_t total_data_placed;
	uint32_t extended_status;
	uint32_t rsvd0: 16,
		 code: 8,
		 pri: 3,
		 pv: 1,
		 xb: 1,
		 rha: 1,
		 rsvd1: 1,
		 vld: 1;
} bcm_fc_optimized_write_data_cqe_t;


/* WQE commands */
typedef struct bcm_fcp_treceive64_wqe {
	bcm_bde_t	bde;
	uint32_t	payload_offset_length;
	uint32_t	relative_offset;
	/**
	 * DWord 5 can either be the task retry identifier (HLM=0) or
	 * the remote N_Port ID (HLM=1), or if implementing the Skyhawk
	 * T10-PI workaround, the secondary xri tag
	 */
	union {
		uint32_t	sec_xri_tag: 16,
				: 16;
		uint32_t	dword;
	} dword5;
	uint32_t	xri_tag: 16,
			context_tag: 16;
	uint32_t dif: 2,
		 ct: 2,
		 bs: 3,
		 : 1,
		 command: 8,
		 class: 3,
			 ar: 1,
			 pu: 2,
			 conf: 1,
			 lnk: 1,
			 timer: 8;
	uint32_t	abort_tag;
	uint32_t	request_tag: 16,
			remote_xid: 16;

	uint32_t	ebde_cnt: 4,
			nvme: 1,
			appid: 1,
			oas: 1,
			len_loc: 2,
			qosd: 1,
			wchn: 1,
			xbl: 1,
			hlm: 1,
			iod: 1,
			dbde: 1,
			wqes: 1,
			pri: 3,
			pv: 1,
			eat: 1,
			xc: 1,
			sr: 1,
			ccpe: 1,
			ccp: 8;
	uint32_t	cmd_type: 4,
			irsp: 1,
			pbde: 1,
			: 1,
			wqec: 1,
			irsplen: 4,
			: 4,
			cq_id: 16;

	uint32_t	fcp_data_receive_length;
	bcm_bde_t	first_data_bde; /* reserved if performance hints disabled */
} bcm_fcp_treceive64_wqe_t;

typedef struct bcm_fcp_trsp64_wqe {
	bcm_bde_t	bde;
	uint32_t	fcp_response_length;
	uint32_t	rsvd4;
	/**
	 * DWord 5 can either be the task retry identifier (HLM=0) or
	 * the remote N_Port ID (HLM=1)
	 */
	uint32_t	dword5;
	uint32_t	xri_tag: 16,
			rpi: 16;
	uint32_t	: 2,
			ct: 2,
			dnrx: 1,
			: 3,
			command: 8,
			class: 3,
				ag: 1,
				pu: 2,
				conf: 1,
				lnk: 1,
				timer: 8;
	uint32_t	abort_tag;
	uint32_t	request_tag: 16,
			remote_xid: 16;
	uint32_t	ebde_cnt: 4,
			nvme: 1,
			appid: 1,
			oas: 1,
			len_loc: 2,
			qosd: 1,
			wchn: 1,
			xbl: 1,
			hlm: 1,
			iod: 1,
			dbde: 1,
			wqes: 1,
			pri: 3,
			pv: 1,
			eat: 1,
			xc: 1,
			sr: 1,
			ccpe: 1,
			ccp: 8;
	uint32_t	cmd_type: 4,
			irsp: 1,
			: 2,
			wqec: 1,
			irsplen: 4,
			: 4,
			cq_id: 16;
	uint32_t	rsvd12;
	uint32_t	rsvd13;
	uint32_t	rsvd14;
	uint32_t	rsvd15;
	uint32_t	inline_rsp;
	uint32_t	rsvdN[15];
} bcm_fcp_trsp64_wqe_t;

typedef struct bcm_fcp_tsend64_wqe {
	bcm_bde_t	bde;
	uint32_t	payload_offset_length;
	uint32_t	relative_offset;
	/**
	 * DWord 5 can either be the task retry identifier (HLM=0) or
	 * the remote N_Port ID (HLM=1)
	 */
	uint32_t	dword5;
	uint32_t	xri_tag: 16,
			rpi: 16;
	uint32_t	dif: 2,
			ct: 2,
			bs: 3,
			: 1,
			command: 8,
			class: 3,
				ar: 1,
				pu: 2,
				conf: 1,
				lnk: 1,
				timer: 8;
	uint32_t	abort_tag;
	uint32_t	request_tag: 16,
			remote_xid: 16;
	uint32_t	ebde_cnt: 4,
			nvme: 1,
			appid: 1,
			oas: 1,
			len_loc: 2,
			qosd: 1,
			wchn: 1,
			xbl: 1,
			hlm: 1,
			iod: 1,
			dbde: 1,
			wqes: 1,
			pri: 3,
			pv: 1,
			eat: 1,
			xc: 1,
			sr: 1,
			ccpe: 1,
			ccp: 8;
	uint32_t	cmd_type: 4,
			irsp: 1,
			: 1,
			sriu: 1,
			wqec: 1,
			irsplen: 4,
			: 4,
			cq_id: 16;
	uint32_t	fcp_data_transmit_length;
	bcm_bde_t	first_data_bde;	/* reserved if performance hints disabled */
} bcm_fcp_tsend64_wqe_t;

typedef struct bcm_xmit_sequence64_wqe_s {
	bcm_bde_t	bde;
	uint32_t	remote_n_port_id: 24,
			: 8;
	uint32_t	relative_offset;
	uint32_t        : 2,
			si: 1,
			ft: 1,
			: 2,
			xo: 1,
			ls: 1,
			df_ctl: 8,
			type: 8,
			r_ctl: 8;
	uint32_t        xri_tag: 16,
			context_tag: 16;
	uint32_t        dif: 2,
			ct: 2,
			bs: 3,
			: 1,
			command: 8,
			class: 3,
				: 1,
				  pu: 2,
				  : 2,
				    timer: 8;
	uint32_t        abort_tag;
	uint32_t        request_tag: 16,
			remote_xid: 16;
	uint32_t        ebde_cnt: 4,
			: 3,
			len_loc: 2,
			qosd: 1,
			: 1,
			xbl: 1,
			hlm: 1,
			iod: 1,
			dbde: 1,
			wqes: 1,
			pri: 3,
			pv: 1,
			eat: 1,
			xc: 1,
			sr: 1,
			ccpe: 1,
			ccp: 8;
	uint32_t        cmd_type: 4,
			: 3,
			wqec: 1,
			: 8,
			cq_id: 16;
	uint32_t        sequence_payload_len;
	uint32_t        rsvd13;
	uint32_t        rsvd14;
	uint32_t        rsvd15;
	uint32_t	rsvd16[16];
} bcm_xmit_sequence64_wqe_t;

typedef struct bcm_generic_wqe_s {
	uint32_t	rsvd[6];
	uint32_t        xri_tag: 16,
			context_tag: 16;
	uint32_t	rsvd1;
	uint32_t	abort_tag;
	uint32_t        request_tag: 16,
			: 16;
	uint32_t        ebde_cnt: 4,
			: 3,
			len_loc: 2,
			qosd: 1,
			: 1,
			xbl: 1,
			hlm: 1,
			iod: 1,
			dbde: 1,
			wqes: 1,
			pri: 3,
			pv: 1,
			eat: 1,
			xc: 1,
			sr: 1,
			ccpe: 1,
			ccp: 8;
	uint32_t        cmd_type: 4,
			: 3,
			wqec: 1,
			: 8,
			cq_id: 16;
	uint32_t	rsvd2[20];
} bcm_generic_wqe_t;


typedef struct bcm_abort_wqe_s {
	uint32_t	rsvd0;
	uint32_t	rsvd1;
	uint32_t	ext_t_tag;
	uint32_t	ia: 1,
			ir: 1,
			: 6,
			criteria: 8,
			: 16;
	uint32_t	ext_t_mask;
	uint32_t	t_mask;
	uint32_t	xri_tag: 16,
			context_tag: 16;
	uint32_t	: 2,
			ct: 2,
			: 4,
			command: 8,
			class: 3,
				: 1,
				  pu: 2,
				  : 2,
				    timer: 8;
	uint32_t	t_tag;
	uint32_t	request_tag: 16,
			: 16;
	uint32_t	ebde_cnt: 4,
			: 3,
			len_loc: 2,
			qosd: 1,
			: 1,
			xbl: 1,
			: 1,
			iod: 1,
			dbde: 1,
			wqes: 1,
			pri: 3,
			pv: 1,
			eat: 1,
			xc: 1,
			: 1,
			ccpe: 1,
			ccp: 8;
	uint32_t	cmd_type: 4,
			: 3,
			wqec: 1,
			: 8,
			cq_id: 16;
} bcm_abort_wqe_t;

typedef struct bcm_xmit_bls_rsp_wqe_s {
	uint32_t	payload_word0;
	uint32_t	rx_id: 16,
			ox_id: 16;
	uint32_t	high_seq_cnt: 16,
			low_seq_cnt: 16;
	uint32_t	rsvd3;
	uint32_t	local_n_port_id: 24,
			: 8;
	uint32_t	remote_id: 24,
			: 6,
			ar: 1,
			xo: 1;
	uint32_t	xri_tag: 16,
			context_tag: 16;
	uint32_t	: 2,
			ct: 2,
			: 4,
			command: 8,
			class: 3,
				: 1,
				  pu: 2,
				  : 2,
				    timer: 8;
	uint32_t	abort_tag;
	uint32_t	request_tag: 16,
			: 16;
	uint32_t	ebde_cnt: 4,
			: 3,
			len_loc: 2,
			qosd: 1,
			: 1,
			xbl: 1,
			hlm: 1,
			iod: 1,
			dbde: 1,
			wqes: 1,
			pri: 3,
			pv: 1,
			eat: 1,
			xc: 1,
			: 1,
			ccpe: 1,
			ccp: 8;
	uint32_t	cmd_type: 4,
			: 3,
			wqec: 1,
			: 8,
			cq_id: 16;
	uint32_t	temporary_rpi: 16,
			: 16;
	uint32_t	rsvd13;
	uint32_t	rsvd14;
	uint32_t	rsvd15;
} bcm_xmit_bls_rsp_wqe_t;

typedef struct bcm_send_frame_wqe_s {
	bcm_bde_t	bde;
	uint32_t	frame_length;
	uint32_t	fc_header_0_1[2];
	uint32_t	xri_tag: 16,
			context_tag: 16;
	uint32_t	: 2,
			ct: 2,
			: 4,
			command: 8,
			class: 3,
				: 1,
				  pu: 2,
				  : 2,
				    timer: 8;
	uint32_t        abort_tag;
	uint32_t	request_tag: 16,
			eof: 8,
			sof: 8;
	uint32_t	ebde_cnt: 4,
			: 3,
			lenloc: 2,
			qosd: 1,
			wchn: 1,
			xbl: 1,
			hlm: 1,
			iod: 1,
			dbde: 1,
			wqes: 1,
			pri: 3,
			pv: 1,
			eat: 1,
			xc: 1,
			: 1,
			ccpe: 1,
			ccp: 8;
	uint32_t	cmd_type: 4,
			: 3,
			wqec: 1,
			: 8,
			cq_id: 16;
	uint32_t	fc_header_2_5[4];
	uint32_t        inline_rsp;
	uint32_t        rsvdN[15];
} bcm_send_frame_wqe_t;

typedef struct bcm_gen_request64_wqe_s {
	bcm_bde_t	bde;
	uint32_t	request_payload_length;
	uint32_t	relative_offset;
	uint32_t	: 8,
			df_ctl: 8,
			type: 8,
			r_ctl: 8;
	uint32_t	xri_tag: 16,
			context_tag: 16;
	uint32_t	: 2,
			ct: 2,
			: 4,
			command: 8,
			class: 3,
				: 1,
				  pu: 2,
				  : 2,
				    timer: 8;
	uint32_t	abort_tag;
	uint32_t	request_tag: 16,
			: 16;
	uint32_t	ebde_cnt: 4,
			: 3,
			len_loc: 2,
			qosd: 1,
			: 1,
			xbl: 1,
			hlm: 1,
			iod: 1,
			dbde: 1,
			wqes: 1,
			pri: 3,
			pv: 1,
			eat: 1,
			xc: 1,
			: 1,
			ccpe: 1,
			ccp: 8;
	uint32_t	cmd_type: 4,
			: 3,
			wqec: 1,
			: 8,
			cq_id: 16;
	uint32_t	remote_n_port_id: 24,
			: 8;
	uint32_t	rsvd13;
	uint32_t	rsvd14;
	uint32_t	max_response_payload_length;
} bcm_gen_request64_wqe_t;

typedef struct bcm_marker_wqe_s {
	uint32_t	rsvd0[3];
	uint32_t	marker_catagery: 2,
			: 30;
	uint32_t	tag_lower;
	uint32_t	tag_higher;
	uint32_t	rsvd1;
	uint32_t        : 8,
			command: 8,
			: 16;
	uint32_t	rsvd2;
	uint32_t	: 16,
			rq_id: 16;
	uint32_t	ebde_cnt: 4,
			: 3,
			len_loc: 2,
			qosd: 1,
			rsvd3: 22;
	uint32_t	cmd_type: 4,
			: 3,
			wqec: 1,
			: 8,
			cq_id: 16;
	uint32_t	rsvd4[4];
} bcm_marker_wqe_t;

/*
 * FC RQ buffer NVMe Command
 */
struct __attribute__((__packed__)) spdk_nvmf_fc_rq_buf_nvme_cmd {
	struct spdk_nvmf_fc_cmnd_iu cmd_iu;
	struct spdk_nvmf_fc_xfer_rdy_iu xfer_rdy;
	struct bcm_sge sge[BCM_MAX_IOVECS];
	uint8_t rsvd[BCM_RQ_BUFFER_SIZE - (sizeof(struct spdk_nvmf_fc_cmnd_iu)
					   + sizeof(struct spdk_nvmf_fc_xfer_rdy_iu)
					   + (sizeof(struct bcm_sge) * BCM_MAX_IOVECS))];
};

SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_rq_buf_nvme_cmd) ==
		   BCM_RQ_BUFFER_SIZE, "RQ Buffer overflow");


/*
 * Send single request - single response buffers
 */
struct nvmf_fc_srsr_bufs {
	struct spdk_nvmf_fc_srsr_bufs srsr_bufs;
	uint64_t rqst_phys;
	uint64_t rsp_phys;
	void *sgl_virt;
	uint64_t sgl_phys;
	uint32_t sgl_len;
	char mz_name[24]; /* name of memzone buffer allocated */
};

/*
 * End of SLI-4 definitions
 */

/* global list of XRI lists for each port */
static TAILQ_HEAD(, fc_xri_list) g_nvmf_fc_xri_list =
	TAILQ_HEAD_INITIALIZER(g_nvmf_fc_xri_list);

struct fc_xri_list*
spdk_nvmf_fc_create_xri_list(uint32_t xri_base, uint32_t xri_count)
{
	struct fc_xri_list *xri_list = calloc(1, sizeof(struct fc_xri_list));
	uint32_t poweroftwo = 1, i;
	struct spdk_nvmf_fc_xchg *ring_xri_ptr = NULL;
	void *ring_xri_vptr[1];
	int rc = 0;

	if (!xri_list) {
		return NULL;
	}

	xri_list->xri_base = xri_base;
	xri_list->xri_count = xri_count;

	while (poweroftwo <= xri_count) {
                poweroftwo *= 2;
	}

	xri_list->xri_list = ring_xri_ptr = calloc(xri_count, sizeof(struct spdk_nvmf_fc_xchg));
	if (!xri_list->xri_list) {
		free(xri_list);
		return NULL;
	}

	xri_list->xri_ring = spdk_ring_create(SPDK_RING_TYPE_MP_MC, poweroftwo,
                                  		      SPDK_ENV_SOCKET_ID_ANY);
	if (!xri_list->xri_ring) {
		free(xri_list->xri_list);
		free(xri_list);
		return NULL;
	}

        for (i = 0; i < xri_count; i++) {
                ring_xri_ptr->xchg_id = xri_base + i;
                ring_xri_vptr[0] = ring_xri_ptr;
                rc = spdk_ring_enqueue(xri_list->xri_ring, ring_xri_vptr, 1, NULL);
                if (rc == 0) {
			free(xri_list->xri_list);
			free(xri_list);
			return NULL;
                }
                ring_xri_ptr++;
        }

	/* put xri list in global xri list (for cleanup) */
	TAILQ_INSERT_TAIL(&g_nvmf_fc_xri_list, xri_list, link);

	return xri_list;
}

static void 
nvmf_fc_xri_list_cleanup(void)
{
	struct fc_xri_list *xri_list, *tmp;

	TAILQ_FOREACH_SAFE(xri_list, &g_nvmf_fc_xri_list, link, tmp) {
		TAILQ_REMOVE(&g_nvmf_fc_xri_list, xri_list, link);
		while (1) {
			void *xri[10000];
			if (spdk_ring_dequeue(xri_list->xri_ring, xri, 10000) < 10000) {
				break;
			}
		}
                spdk_ring_free(xri_list->xri_ring);
		free(xri_list);
	}
}

static inline void
nvmf_fc_queue_tail_inc(bcm_sli_queue_t *q)
{
	q->tail = (q->tail + 1) % q->max_entries;
}

static inline void
nvmf_fc_queue_head_inc(bcm_sli_queue_t *q)
{
	q->head = (q->head + 1) % q->max_entries;
}

static inline void *
nvmf_fc_queue_head_node(bcm_sli_queue_t *q)
{
	return q->address + q->head * q->size;
}

static inline void *
nvmf_fc_queue_tail_node(bcm_sli_queue_t *q)
{
	return q->address + q->tail * q->size;
}

static inline bool
nvmf_fc_queue_full(bcm_sli_queue_t *q)
{
	return (q->used >= q->max_entries);
}

static uint32_t
nvmf_fc_rqpair_get_buffer_id(struct spdk_nvmf_fc_hwqp *hwqp, uint16_t rqindex)
{
	return BCM_HWQP(hwqp)->rq_hdr.rq_map[rqindex];
}

static struct spdk_nvmf_fc_frame_hdr *
nvmf_fc_rqpair_get_frame_header(struct spdk_nvmf_fc_hwqp *hwqp, uint16_t rqindex)
{
	uint32_t buf_index = BCM_HWQP(hwqp)->rq_hdr.rq_map[rqindex];
	return BCM_HWQP(hwqp)->rq_hdr.buffer[buf_index].virt;
}

static struct spdk_nvmf_fc_buffer_desc *
nvmf_fc_rqpair_get_frame_buffer(struct spdk_nvmf_fc_hwqp *hwqp, uint16_t rqindex)
{
	uint32_t buf_index = BCM_HWQP(hwqp)->rq_hdr.rq_map[rqindex]; // Use header map.
	return BCM_HWQP(hwqp)->rq_payload.buffer + buf_index;
}

static int
nvmf_fc_create_reqtag_pool(struct spdk_nvmf_fc_hwqp *hwqp)
{
	int i;
	struct fc_wrkq *wq = &BCM_HWQP(hwqp)->wq;
	fc_reqtag_t *obj;
	static int unique_number = 0;

	unique_number++;

	/* Create reqtag ring of size MAX_REQTAG_POOL_SIZE + 1 and make sure its power of 2. */
	wq->reqtag_ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, (MAX_REQTAG_POOL_SIZE + 1),
					   SPDK_ENV_SOCKET_ID_ANY);
	if (!wq->reqtag_ring) {
		SPDK_ERRLOG("create fc reqtag ring failed\n");
		return -1;
	}

	/* Create ring objects */
	wq->reqtag_objs = calloc(MAX_REQTAG_POOL_SIZE, sizeof(fc_reqtag_t));
	if (!wq->reqtag_objs) {
		SPDK_ERRLOG("create fc reqtag ring objects failed\n");
		goto error;
	}

	/* Initialise index value in ring objects and queue the objects to ring */
	for (i = 0; i < MAX_REQTAG_POOL_SIZE; i ++) {
		obj = wq->reqtag_objs + i;

		obj->index = i;
		if (spdk_ring_enqueue(wq->reqtag_ring, (void **)&obj, 1, NULL) != 1) {
			SPDK_ERRLOG("fc reqtag ring enqueue objects failed %d\n", i);
			goto error;
		}
		wq->p_reqtags[i] = NULL;
	}

	/* Init the wqec counter */
	wq->wqec_count = 0;

	return 0;
error:
	if (wq->reqtag_objs) {
		free(wq->reqtag_objs);
	}

	if (wq->reqtag_ring) {
		spdk_ring_free(wq->reqtag_ring);
	}
	return -1;
}

static fc_reqtag_t *
nvmf_fc_get_reqtag(struct spdk_nvmf_fc_hwqp *hwqp)
{
	struct fc_wrkq *wq = &BCM_HWQP(hwqp)->wq;
	fc_reqtag_t *tag;

	if (!spdk_ring_dequeue(wq->reqtag_ring, (void **)&tag, 1)) {
		return NULL;
	}

	/* Save the pointer for lookup */
	wq->p_reqtags[tag->index] = tag;
	return tag;
}

static fc_reqtag_t *
nvmf_fc_lookup_reqtag(struct spdk_nvmf_fc_hwqp *hwqp, uint16_t index)
{
	assert(index < MAX_REQTAG_POOL_SIZE);
	if (index < MAX_REQTAG_POOL_SIZE) {
		return BCM_HWQP(hwqp)->wq.p_reqtags[index];
	} else {
		SPDK_ERRLOG("Invalid index\n");
		return NULL;
	}
}

static int
nvmf_fc_release_reqtag(struct spdk_nvmf_fc_hwqp *hwqp, fc_reqtag_t *tag)
{
	struct fc_wrkq *wq = &BCM_HWQP(hwqp)->wq;
	int rc;

	rc = spdk_ring_enqueue(wq->reqtag_ring, (void**)&tag, 1, NULL);

	if (rc == 1) {
		wq->p_reqtags[tag->index] = NULL;
		tag->cb = NULL;
		tag->cb_args = NULL;
	}
	// Driver & library interpretation of return code is different
	rc ^= 1;

	return rc;
}

static void
nvmf_fc_bcm_notify_queue(bcm_sli_queue_t *q, bool arm_queue, uint16_t num_entries)
{
	doorbell_t *reg, entry;

	reg = (doorbell_t *)q->doorbell_reg;
	entry.doorbell = 0;

	switch (q->type) {
	case BCM_FC_QUEUE_TYPE_EQ:
		entry.eqdoorbell.qt = 1;
		entry.eqdoorbell.ci = 1;
		entry.eqdoorbell.num_popped = num_entries;
		entry.eqdoorbell.eq_id = (q->qid & 0x1ff);
		entry.eqdoorbell.eq_id_ext = ((q->qid >> 9) & 0x1f);
		entry.eqdoorbell.arm = arm_queue;
		break;
	case BCM_FC_QUEUE_TYPE_CQ_WQ:
	case BCM_FC_QUEUE_TYPE_CQ_RQ:
		entry.cqdoorbell.num_popped = num_entries;
		entry.cqdoorbell.cq_id = (q->qid & 0x3ff);
		entry.cqdoorbell.cq_id_ext = ((q->qid >> 10) & 0x1f);
		entry.cqdoorbell.solicit_enable = 0;
		entry.cqdoorbell.arm = arm_queue;
		break;
	case BCM_FC_QUEUE_TYPE_WQ:
		entry.wqdoorbell.wq_id = (q->qid & 0xffff);
		entry.wqdoorbell.wq_index = (q->head & 0x00ff);
		entry.wqdoorbell.num_posted = num_entries;
		break;
	case BCM_FC_QUEUE_TYPE_RQ_HDR:
	case BCM_FC_QUEUE_TYPE_RQ_DATA:
		entry.rqdoorbell.rq_id = q->qid;
		entry.rqdoorbell.num_posted = num_entries;
		break;
	}

	spdk_wmb();
	reg->doorbell = entry.doorbell;
}

static uint8_t
nvmf_fc_queue_entry_is_valid(bcm_sli_queue_t *q, uint8_t *qe, uint8_t clear)
{
	uint8_t valid = 0;

	switch (q->type) {
	case BCM_FC_QUEUE_TYPE_EQ:
		valid = ((eqe_t *)qe)->valid;
		if (valid & clear) {
			((eqe_t *)qe)->valid = 0;
		}
		break;
	case BCM_FC_QUEUE_TYPE_CQ_WQ:
	case BCM_FC_QUEUE_TYPE_CQ_RQ:
		/*
		 * For both WCQE and RCQE, the valid bit
		 * is bit 31 of dword 3 (0 based)
		 */
		valid = (qe[15] & 0x80) != 0;
		if (valid & clear) {
			qe[15] &= ~0x80;
		}
		break;
	default:
		SPDK_ERRLOG("%s doesn't handle type=%#x\n", __func__, q->type);
	}

	return valid;
}

static int
nvmf_fc_read_queue_entry(bcm_sli_queue_t *q, uint8_t *entry)
{
	uint8_t	*qe;

	switch (q->type) {
	case BCM_FC_QUEUE_TYPE_EQ:
	case BCM_FC_QUEUE_TYPE_CQ_WQ:
	case BCM_FC_QUEUE_TYPE_CQ_RQ:
		break;
	default:
		SPDK_ERRLOG("%s read not handled for queue type=%#x\n",
			    __func__, q->type);
		return -1;
	}

	/* Get the tail entry */
	qe = nvmf_fc_queue_tail_node(q);

	/* Check if entry is valid */
	if (!nvmf_fc_queue_entry_is_valid(q, qe, true)) {
		return -1;
	}

	/* Make a copy if user requests */
	if (entry) {
		memcpy(entry, qe, q->size);
	}

	nvmf_fc_queue_tail_inc(q);

	return 0;
}

static int
nvmf_fc_write_queue_entry(bcm_sli_queue_t *q, uint8_t *entry)
{
	uint8_t	*qe;

	if (!entry) {
		return -1;
	}

	switch (q->type) {
	case BCM_FC_QUEUE_TYPE_WQ:
	case BCM_FC_QUEUE_TYPE_RQ_HDR:
	case BCM_FC_QUEUE_TYPE_RQ_DATA:
		break;
	default:
		SPDK_ERRLOG("%s write not handled for queue type=%#x\n",
			    __func__, q->type);
		// For other queues write is not valid.
		return -1;
	}

	/* We need to check if there is space available */
	if (nvmf_fc_queue_full(q)) {
		SPDK_ERRLOG("%s queue full for type = %#x\n", __func__, q->type);
		return -1;
	}

	/* Copy entry */
	qe = nvmf_fc_queue_head_node(q);
	memcpy(qe, entry, q->size);

	/* Update queue */
	nvmf_fc_queue_head_inc(q);

	return 0;
}

static int
nvmf_fc_post_wqe(struct spdk_nvmf_fc_hwqp *hwqp, uint8_t *entry, bool notify,
		 bcm_fc_wqe_cb cb, void *cb_args)
{
	int rc = -1;
	bcm_generic_wqe_t *wqe = (bcm_generic_wqe_t *)entry;
	struct fc_wrkq *wq = &BCM_HWQP(hwqp)->wq;
	fc_reqtag_t *reqtag = NULL;

	if (!entry || !cb) {
		goto error;
	}

	/* Make sure queue is online */
	if (hwqp->state != SPDK_FC_HWQP_ONLINE) {
		/*
		SPDK_ERRLOG("%s queue is not online. WQE not posted. hwqp_id = %d type = %#x\n",
			    __func__, hwqp->hwqp_id, wq->q.type);
		 */
		goto error;
	}

	/* Make sure queue is not full */
	if (nvmf_fc_queue_full(&wq->q)) {
		SPDK_ERRLOG("%s queue full. type = %#x\n", __func__, wq->q.type);
		goto error;
	}

	/* Alloc a reqtag */
	reqtag = nvmf_fc_get_reqtag(hwqp);
	if (!reqtag) {
		SPDK_ERRLOG("%s No reqtag available\n", __func__);
		goto error;
	}
	reqtag->cb = cb;
	reqtag->cb_args = cb_args;

	/* Update request tag in the WQE entry */
	wqe->request_tag = reqtag->index;
	wq->wqec_count ++;

	if (wq->wqec_count == MAX_WQ_WQEC_CNT) {
		wqe->wqec = 1;
	}

	rc = nvmf_fc_write_queue_entry(&wq->q, entry);
	if (rc) {
		SPDK_ERRLOG("%s: WQE write failed. \n", __func__);
		hwqp->counters.wqe_write_err++;
		goto error;
	}

	wq->q.used++;
	if (wqe->wqec) {
		/* Reset wqec count. */
		wq->wqec_count = 0;
	}

	if (notify) {
		nvmf_fc_bcm_notify_queue(&wq->q, false, 1);
	}
	return 0;
error:
	if (reqtag) {
		nvmf_fc_release_reqtag(hwqp, reqtag);
	}
	return rc;
}

static int
nvmf_fc_parse_eq_entry(struct eqe *qe, uint16_t *cq_id)
{
	int rc = 0;

	assert(qe);
	assert(cq_id);

	if (!qe || !cq_id) {
		SPDK_ERRLOG("%s: bad parameters eq=%p, cq_id=%p\n", __func__,
			    qe, cq_id);
		return -1;
	}

	switch (qe->major_code) {
	case BCM_MAJOR_CODE_STANDARD:
		*cq_id = qe->resource_id;
		rc = 0;
		break;
	case BCM_MAJOR_CODE_SENTINEL:
		SPDK_NOTICELOG("%s: sentinel EQE\n", __func__);
		rc = 1;
		break;
	default:
		SPDK_NOTICELOG("%s: Unsupported EQE: major %x minor %x\n",
			       __func__, qe->major_code, qe->minor_code);
		rc = -1;
	}
	return rc;
}

static uint32_t
nvmf_fc_parse_cqe_ext_status(uint8_t *cqe)
{
	cqe_t *cqe_entry = (void *)cqe;
	uint32_t mask;

	switch (cqe_entry->u.wcqe.status) {
	case BCM_FC_WCQE_STATUS_FCP_RSP_FAILURE:
		mask = UINT32_MAX;
		break;
	case BCM_FC_WCQE_STATUS_LOCAL_REJECT:
	case BCM_FC_WCQE_STATUS_CMD_REJECT:
		mask = 0xff;
		break;
	case BCM_FC_WCQE_STATUS_NPORT_RJT:
	case BCM_FC_WCQE_STATUS_FABRIC_RJT:
	case BCM_FC_WCQE_STATUS_NPORT_BSY:
	case BCM_FC_WCQE_STATUS_FABRIC_BSY:
	case BCM_FC_WCQE_STATUS_LS_RJT:
		mask = UINT32_MAX;
		break;
	case BCM_FC_WCQE_STATUS_DI_ERROR:
		mask = UINT32_MAX;
		break;
	default:
		mask = 0;
	}

	return cqe_entry->u.wcqe.wqe_specific_2 & mask;
}



static int
nvmf_fc_parse_cq_entry(struct fc_eventq *cq, uint8_t *cqe, bcm_qentry_type_e *etype, uint16_t *r_id)
{
	int     rc = -1;
	cqe_t *cqe_entry = (cqe_t *)cqe;
	uint32_t ext_status = 0;

	if (!cq || !cqe || !etype || !r_id) {
		SPDK_ERRLOG("%s: bad parameters cq=%p cqe=%p etype=%p q_id=%p\n",
			    __func__, cq, cqe, etype, r_id);
		return -1;
	}

	switch (cqe_entry->u.generic.event_code) {
	case BCM_CQE_CODE_WORK_REQUEST_COMPLETION: {
		*etype = BCM_FC_QENTRY_WQ;
		*r_id = cqe_entry->u.wcqe.request_tag;
		rc = cqe_entry->u.wcqe.status;
		if (rc) {
			ext_status = nvmf_fc_parse_cqe_ext_status(cqe);
			if ((rc == BCM_FC_WCQE_STATUS_LOCAL_REJECT) &&
			    ((ext_status == BCM_FC_LOCAL_REJECT_NO_XRI) ||
			     (ext_status == BCM_FC_LOCAL_REJECT_ABORT_REQUESTED))) {
				SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LLD,
					      "WCQE: status=%#x hw_status=%#x tag=%#x w1=%#x w2=%#x\n",
					      cqe_entry->u.wcqe.status,
					      cqe_entry->u.wcqe.hw_status,
					      cqe_entry->u.wcqe.request_tag,
					      cqe_entry->u.wcqe.wqe_specific_1,
					      cqe_entry->u.wcqe.wqe_specific_2);
			} else {
				SPDK_NOTICELOG("WCQE: status=%#x hw_status=%#x tag=%#x w1=%#x w2=%#x xb=%d\n",
					       cqe_entry->u.wcqe.status,
					       cqe_entry->u.wcqe.hw_status,
					       cqe_entry->u.wcqe.request_tag,
					       cqe_entry->u.wcqe.wqe_specific_1,
					       cqe_entry->u.wcqe.wqe_specific_2,
					       cqe_entry->u.wcqe.xb);
				SPDK_NOTICELOG("  %08X %08X %08X %08X\n",
					       ((uint32_t *)cqe)[0], ((uint32_t *)cqe)[1],
					       ((uint32_t *)cqe)[2], ((uint32_t *)cqe)[3]);
			}
		}
		break;
	}
	case BCM_CQE_CODE_RQ_ASYNC: {
		*etype = BCM_FC_QENTRY_RQ;
		*r_id = cqe_entry->u.async_rcqe.rq_id;
		rc = cqe_entry->u.async_rcqe.status;
		break;
	}
	case BCM_CQE_CODE_RQ_ASYNC_V1: {
		*etype = BCM_FC_QENTRY_RQ;
		*r_id = cqe_entry->u.async_rcqe_v1.rq_id;
		rc = cqe_entry->u.async_rcqe_v1.status;
		break;
	}
	case BCM_CQE_CODE_RQ_MARKER: {
		*etype = BCM_FC_QENTRY_RQ;
		*r_id = cqe_entry->u.async_marker.rq_id;
		rc = cqe_entry->u.async_marker.status;
		break;
	}
	case BCM_CQE_CODE_XRI_ABORTED: {
		*etype = BCM_FC_QENTRY_XABT;
		*r_id = cqe_entry->u.xri_aborted_cqe.xri;
		rc = 0;
		break;
	}
	case BCM_CQE_CODE_RELEASE_WQE: {
		*etype = BCM_FC_QENTRY_WQ_RELEASE;
		*r_id = cqe_entry->u.wqec.wq_id;
		rc = 0;
		break;
	}
	default:
		SPDK_ERRLOG("%s: CQE completion code %d not handled\n", __func__,
			    cqe_entry->u.generic.event_code);
		*etype = BCM_FC_QENTRY_MAX;
		*r_id = UINT16_MAX;

	}
	return rc;
}

static int
nvmf_fc_rqe_rqid_and_index(uint8_t *cqe, uint16_t *rq_id, uint32_t *index)
{
	bcm_fc_async_rcqe_t	*rcqe = (void *)cqe;
	bcm_fc_async_rcqe_v1_t	*rcqe_v1 = (void *)cqe;
	bcm_fc_async_rcqe_marker_t *marker = (void *)cqe;
	int	rc = -1;
	uint8_t	code = 0;

	*rq_id = 0;
	*index = UINT32_MAX;

	code = cqe[BCM_CQE_CODE_OFFSET];

	if (code == BCM_CQE_CODE_RQ_ASYNC) {
		*rq_id = rcqe->rq_id;
		if (BCM_FC_ASYNC_RQ_SUCCESS == rcqe->status) {
			*index = rcqe->rq_element_index;
			rc = 0;
		} else {
			*index = rcqe->rq_element_index;
			rc = rcqe->status;
			SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LLD,
				      "%s: status=%02x rq_id=%d, index=%x pdpl=%x sof=%02x eof=%02x hdpl=%x\n",
				      __func__, rcqe->status,
				      rcqe->rq_id,
				      rcqe->rq_element_index, rcqe->payload_data_placement_length, rcqe->sof_byte,
				      rcqe->eof_byte, rcqe->header_data_placement_length);
		}
	} else if (code == BCM_CQE_CODE_RQ_ASYNC_V1) {
		*rq_id = rcqe_v1->rq_id;
		if (BCM_FC_ASYNC_RQ_SUCCESS == rcqe_v1->status) {
			*index = rcqe_v1->rq_element_index;
			rc = 0;
		} else {
			*index = rcqe_v1->rq_element_index;
			rc = rcqe_v1->status;
			SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LLD,
				      "%s: status=%02x rq_id=%d, index=%x pdpl=%x sof=%02x eof=%02x hdpl=%x\n",
				      __func__, rcqe_v1->status,
				      rcqe_v1->rq_id, rcqe_v1->rq_element_index,
				      rcqe_v1->payload_data_placement_length, rcqe_v1->sof_byte,
				      rcqe_v1->eof_byte, rcqe_v1->header_data_placement_length);
		}

	} else if (code == BCM_CQE_CODE_RQ_MARKER) {
		*rq_id = marker->rq_id;
		*index = marker->rq_element_index;
		if (BCM_FC_ASYNC_RQ_SUCCESS == marker->status) {
			rc = 0;
			SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LLD,
				      "%s: marker cqe status=%02x rq_id=%d, index=%x\n", __func__,
				      marker->status, marker->rq_id, marker->rq_element_index);
		} else {
			rc = marker->status;
			SPDK_ERRLOG("%s: marker cqe status=%02x rq_id=%d, index=%x\n", __func__,
				    marker->status, marker->rq_id, marker->rq_element_index);
		}

	} else {
		*index = UINT32_MAX;

		rc = rcqe->status;

		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LLD,
			      "%s: status=%02x rq_id=%d, index=%x pdpl=%x sof=%02x eof=%02x hdpl=%x\n", __func__,
			      rcqe->status, rcqe->rq_id, rcqe->rq_element_index, rcqe->payload_data_placement_length,
			      rcqe->sof_byte, rcqe->eof_byte, rcqe->header_data_placement_length);
	}

	return rc;
}

static int
nvmf_fc_rqpair_buffer_post(struct spdk_nvmf_fc_hwqp *hwqp, uint16_t idx, bool notify)
{
	int rc;
	struct fc_rcvq *hdr = &BCM_HWQP(hwqp)->rq_hdr;
	struct fc_rcvq *payload = &BCM_HWQP(hwqp)->rq_payload;
	uint32_t phys_hdr[2];

	/* Post payload buffer */
	phys_hdr[0] =  PTR_TO_ADDR32_HI(payload->buffer[idx].phys);
	phys_hdr[1] =  PTR_TO_ADDR32_LO(payload->buffer[idx].phys);
	rc = nvmf_fc_write_queue_entry(&payload->q, (uint8_t *)phys_hdr);
	if (!rc) {
		/* Post header buffer */
		phys_hdr[0] =  PTR_TO_ADDR32_HI(hdr->buffer[idx].phys);
		phys_hdr[1] =  PTR_TO_ADDR32_LO(hdr->buffer[idx].phys);
		rc = nvmf_fc_write_queue_entry(&hdr->q, (uint8_t *)phys_hdr);
		if (!rc) {

			BCM_HWQP(hwqp)->rq_hdr.q.used++;
			BCM_HWQP(hwqp)->rq_payload.q.used++;

			if (notify) {
				nvmf_fc_bcm_notify_queue(&hdr->q, false, 1);
			}
		}
	}
	return rc;
}

static void
nvmf_fc_rqpair_buffer_release(struct spdk_nvmf_fc_hwqp *hwqp, uint16_t buff_idx)
{
	/* Decrement used */
	BCM_HWQP(hwqp)->rq_hdr.q.used--;
	BCM_HWQP(hwqp)->rq_payload.q.used--;

	/* Increment tail */
	nvmf_fc_queue_tail_inc(&BCM_HWQP(hwqp)->rq_hdr.q);
	nvmf_fc_queue_tail_inc(&BCM_HWQP(hwqp)->rq_payload.q);

	/* Repost the freebuffer to head of queue. */
	BCM_HWQP(hwqp)->rq_hdr.rq_map[
		BCM_HWQP(hwqp)->rq_hdr.q.head] = buff_idx;
	nvmf_fc_rqpair_buffer_post(hwqp, buff_idx, true);
}

static int
nvmf_fc_lld_init(void) 
{
	return spdk_fc_subsystem_init();
}

static void 
nvmf_fc_lld_fini(void) 
{
	spdk_fc_subsystem_fini();
	nvmf_fc_xri_list_cleanup();
}

static void
nvmf_fc_lld_start(void)
{
	ocs_spdk_start_pollers();
}

static int
nvmf_fc_init_q(struct spdk_nvmf_fc_hwqp *hwqp)
{
	return nvmf_fc_create_reqtag_pool(hwqp);
}

static void
nvmf_fc_reinit_q(void *queues_prev, void *queues_curr)
{
	struct fc_wrkq *wq_prev, *wq_curr;
	int count = 0;
	int i;

	wq_prev = &((struct bcm_nvmf_hw_queues *)queues_prev)->wq;
	wq_curr = &((struct bcm_nvmf_hw_queues *)queues_curr)->wq;

 	/* Copy over reqtag pool information from previous wq queues. */
	wq_curr->reqtag_ring = wq_prev->reqtag_ring;
	wq_curr->reqtag_objs = wq_prev->reqtag_objs;

	wq_curr->wqec_count = 0;
	for (i = 0; i < MAX_REQTAG_POOL_SIZE; i++) {
		if (wq_prev->p_reqtags[i] != NULL) {
			/*
			 * This condition is possible if we killed
			 * commands that were in transfer state.
			 */
			wq_prev->p_reqtags[i]->cb = NULL;
			wq_prev->p_reqtags[i]->cb_args = NULL;

			(void)spdk_ring_enqueue(wq_curr->reqtag_ring, (void**) &wq_prev->p_reqtags[i], 1, NULL);
			wq_prev->p_reqtags[i] = NULL;
			count = count + 1;
		}
		wq_curr->p_reqtags[i] = NULL;
	}

	if (count) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LLD, "Found %d outstanding reqtags that were released\n",
			      count);
	}
}

static int
nvmf_fc_init_rqpair_buffers(struct spdk_nvmf_fc_hwqp *hwqp)
{
	int rc = 0;
	uint16_t i;
	struct fc_rcvq *hdr = &BCM_HWQP(hwqp)->rq_hdr;
	struct fc_rcvq *payload = &BCM_HWQP(hwqp)->rq_payload;

	/* Init queue variables */
	BCM_HWQP(hwqp)->eq.q.posted_limit = 16;
	BCM_HWQP(hwqp)->cq_wq.q.posted_limit = 16;
	BCM_HWQP(hwqp)->cq_rq.q.posted_limit = 16;

	BCM_HWQP(hwqp)->eq.q.processed_limit = 64;
	BCM_HWQP(hwqp)->cq_wq.q.processed_limit = 64;
	BCM_HWQP(hwqp)->cq_rq.q.processed_limit = 64;

	BCM_HWQP(hwqp)->eq.auto_arm_flag = false;

	BCM_HWQP(hwqp)->cq_wq.auto_arm_flag = true;
	BCM_HWQP(hwqp)->cq_rq.auto_arm_flag = true;

	BCM_HWQP(hwqp)->eq.q.type = BCM_FC_QUEUE_TYPE_EQ;
	BCM_HWQP(hwqp)->cq_wq.q.type = BCM_FC_QUEUE_TYPE_CQ_WQ;
	BCM_HWQP(hwqp)->wq.q.type = BCM_FC_QUEUE_TYPE_WQ;
	BCM_HWQP(hwqp)->cq_rq.q.type = BCM_FC_QUEUE_TYPE_CQ_RQ;
	BCM_HWQP(hwqp)->rq_hdr.q.type = BCM_FC_QUEUE_TYPE_RQ_HDR;
	BCM_HWQP(hwqp)->rq_payload.q.type = BCM_FC_QUEUE_TYPE_RQ_DATA;

	if (hdr->q.max_entries != payload->q.max_entries) {
		assert(0);
	}
	if (hdr->q.max_entries > MAX_RQ_ENTRIES) {
		assert(0);
	}

	for (i = 0; i < hdr->q.max_entries; i++) {
		rc = nvmf_fc_rqpair_buffer_post(hwqp, i, false);
		if (rc) {
			break;
		}
		hdr->rq_map[i] = i;
	}

	/* Make sure CQs are in armed state */
	nvmf_fc_bcm_notify_queue(&BCM_HWQP(hwqp)->cq_wq.q, true, 0);
	nvmf_fc_bcm_notify_queue(&BCM_HWQP(hwqp)->cq_rq.q, true, 0);

	if (!rc) {
		/* Ring doorbell for one less */
		nvmf_fc_bcm_notify_queue(&hdr->q, false, (hdr->q.max_entries - 1));
	}

	return rc;
}

static int
nvmf_fc_set_q_online_state(struct spdk_nvmf_fc_hwqp *hwqp, bool online)
{
	BCM_HWQP(hwqp)->free_rq_slots = BCM_HWQP(hwqp)->rq_payload.num_buffers;
	hwqp->num_conns = 0;
	return 0;
}

static void
nvmf_fc_abort_cmpl_cb(void *ctx, uint8_t *cqe, int32_t status, void *arg)
{
	struct spdk_nvmf_fc_hwqp *hwqp = ctx;
	struct spdk_nvmf_fc_caller_ctx *carg = arg;

	SPDK_NOTICELOG("IO Aborted(XRI:0x%x, Status=%d)\n",
		       ((struct spdk_nvmf_fc_xchg *)(carg->ctx))->xchg_id, status);

	if (carg->cb) {
		carg->cb(hwqp, status, carg->cb_args);
	}

	free(carg);
}

static int
nvmf_fc_issue_abort(struct spdk_nvmf_fc_hwqp *hwqp,
		    struct spdk_nvmf_fc_xchg *xri,
		    spdk_nvmf_fc_caller_cb cb, void *cb_args)
{
	uint8_t wqe[128] = { 0 };
	bcm_abort_wqe_t *abort = (bcm_abort_wqe_t *)wqe;
	struct spdk_nvmf_fc_caller_ctx *ctx = NULL;
	int rc = -1;

	ctx = calloc(1, sizeof(struct spdk_nvmf_fc_caller_ctx));
	if (!ctx) {
		goto done;
	}
	ctx->ctx = xri;
	ctx->cb = cb;
	ctx->cb_args = cb_args;

	abort->criteria = BCM_ABORT_CRITERIA_XRI_TAG;
	abort->ia = xri->send_abts ? 0 : 1;
	abort->ir = 1; /* Supress ABTS retries. */
	abort->command = BCM_WQE_ABORT;
	abort->qosd = true;
	abort->cq_id = UINT16_MAX;
	abort->cmd_type = BCM_CMD_ABORT_WQE;
	abort->t_tag = xri->xchg_id;

	if (xri->send_abts) {
		/* Increment abts sent count */
		hwqp->counters.num_abts_sent++;
	}

	rc = nvmf_fc_post_wqe(hwqp, (uint8_t *)abort, true, nvmf_fc_abort_cmpl_cb, ctx);
done:
	if (rc && ctx) {
		free(ctx);
	}

	if (!rc) {
		xri->active = false;
		xri->send_abts = false;
		SPDK_NOTICELOG("Abort WQE posted for XRI = %d\n", xri->xchg_id);
	}
	return rc;
}

static struct spdk_nvmf_fc_xchg *
nvmf_fc_get_xri(struct spdk_nvmf_fc_hwqp *hwqp)
{
	struct spdk_nvmf_fc_xchg *xri[1];

	/* Get the physical port from the hwqp and dequeue an XRI from the
	 * corresponding ring. Send this back.
	 */
	if (1 != spdk_ring_dequeue(BCM_HWQP(hwqp)->xri_list->xri_ring, (void **)xri, 1)) {
		return NULL;
	}

	xri[0]->active	= false;
	xri[0]->aborted = false;
	xri[0]->send_abts = false;

	return xri[0];
}

static int
nvmf_fc_put_xri(struct spdk_nvmf_fc_hwqp *hwqp, struct spdk_nvmf_fc_xchg *xri)
{
	void *xxri[1];
	xxri[0] = xri;
	return spdk_ring_enqueue(BCM_HWQP(hwqp)->xri_list->xri_ring, xxri, 1, NULL);
}

static inline void
nvmf_fc_nvmf_add_xri_pending(struct spdk_nvmf_fc_hwqp *hwqp,
			     struct spdk_nvmf_fc_xchg *xri)
{
	struct spdk_nvmf_fc_xchg *tmp;

	/* Check if its already exists. */
	TAILQ_FOREACH(tmp, &(BCM_HWQP(hwqp)->pending_xri_list), link) {
		if (tmp == xri) {
			return;
		}
	}

	/* Add */
	TAILQ_INSERT_TAIL(&(BCM_HWQP(hwqp)->pending_xri_list), xri, link);
}


static void
nvmf_fc_cleanup_xri(struct spdk_nvmf_fc_hwqp *hwqp,
		    struct spdk_nvmf_fc_xchg *xri, bool hw_xri_busy, bool abts)
{
	if (hw_xri_busy && !spdk_nvmf_fc_is_port_dead(hwqp)) {
		if (xri->active) {
			/* Driver state of xri is also active, so send abort */
			nvmf_fc_issue_abort(hwqp, xri, NULL, NULL);
		}
		/* Wait for XRI_ABORTED_CQE */
		nvmf_fc_nvmf_add_xri_pending(hwqp, xri);
	} else {
		xri->active = false;
		nvmf_fc_put_xri(hwqp, xri);
	}
}

static void
nvmf_fc_nvmf_del_xri_pending(struct spdk_nvmf_fc_hwqp *hwqp, uint32_t xri)
{
	struct spdk_nvmf_fc_xchg *tmp;

	TAILQ_FOREACH(tmp, &(BCM_HWQP(hwqp)->pending_xri_list), link) {
		if (tmp->xchg_id == xri) {
			nvmf_fc_put_xri(hwqp, tmp);
			TAILQ_REMOVE(&(BCM_HWQP(hwqp)->pending_xri_list), tmp, link);
			return;
		}
	}
}

static bool
nvmf_fc_abts_required(uint8_t *cqe_entry)
{
	cqe_t *cqe = (cqe_t *)cqe_entry;
	uint16_t status = cqe->u.wcqe.status;
	uint32_t ext_status = nvmf_fc_parse_cqe_ext_status(cqe_entry);
	bool send_abts = false;

	if (BCM_SUPPORT_ABTS_FOR_SEQ_ERRORS && status &&
	    !(status == BCM_FC_WCQE_STATUS_LOCAL_REJECT &&
	      ((ext_status == BCM_FC_LOCAL_REJECT_NO_XRI) ||
	       (ext_status == BCM_FC_LOCAL_REJECT_INVALID_RPI) ||
	       (ext_status == BCM_FC_LOCAL_REJECT_ABORT_REQUESTED)))) {
		send_abts = true;
	}
	return send_abts;
}

static void
nvmf_fc_bls_cmpl_cb(void *ctx, uint8_t *cqe, int32_t status, void *arg)
{
	struct spdk_nvmf_fc_hwqp *hwqp = ctx;
	cqe_t *cqe_entry 	= (cqe_t *)cqe;
	struct spdk_nvmf_fc_caller_ctx *carg 	= arg;
	struct spdk_nvmf_fc_xchg *xri = carg->ctx;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LLD, "BLS WQE Compl(%d) \n", status);

	nvmf_fc_cleanup_xri(hwqp, xri, cqe_entry->u.generic.xb, false);

	if (carg->cb) {
		carg->cb(hwqp, status, carg->cb_args);
	}

	free(carg);
}

static void
nvmf_fc_srsr_cmpl_cb(void *ctx, uint8_t *cqe, int32_t status, void *arg)
{
	struct spdk_nvmf_fc_hwqp *hwqp = ctx;
	cqe_t *cqe_entry 	= (cqe_t *)cqe;
	struct spdk_nvmf_fc_caller_ctx *carg 	= arg;
	struct spdk_nvmf_fc_xchg *xri = carg->ctx;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LLD, "SRSR WQE Compl(%d) \n", status);

	nvmf_fc_cleanup_xri(hwqp, xri, cqe_entry->u.generic.xb,
				     nvmf_fc_abts_required(cqe));

	if (carg->cb) {
		carg->cb(hwqp, status, carg->cb_args);
	}

	free(carg);
}

static void
nvmf_fc_ls_rsp_cmpl_cb(void *ctx, uint8_t *cqe, int32_t status, void *arg)
{
	struct spdk_nvmf_fc_ls_rqst *ls_rqst = arg;
	struct spdk_nvmf_fc_hwqp *hwqp = ctx;
	cqe_t *cqe_entry = (cqe_t *)cqe;

	nvmf_fc_cleanup_xri(hwqp, ls_rqst->xchg, cqe_entry->u.generic.xb,
				     nvmf_fc_abts_required(cqe));

	/* Release RQ buffer */
	nvmf_fc_rqpair_buffer_release(hwqp, ls_rqst->rqstbuf.buf_index);

	if (status) {
		SPDK_ERRLOG("LS WQE Compl(%d) error\n", status);
	}
}


static void
nvmf_fc_def_cmpl_cb(void *ctx, uint8_t *cqe, int32_t status, void *arg)
{
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LLD, "DEF WQE Compl(%d) \n", status);
}

static void
nvmf_fc_io_cmpl_cb(void *ctx, uint8_t *cqe, int32_t status, void *arg)
{
	struct spdk_nvmf_fc_request *fc_req = arg;
	cqe_t *cqe_entry = (cqe_t *)cqe;

	/* Assert if its not a valid completion. */
	assert(fc_req->magic != 0xDEADBEEF);

	if (status || fc_req->is_aborted) {
		goto io_done;
	}

	/* Write Tranfer done */
	if (fc_req->state == SPDK_NVMF_FC_REQ_WRITE_XFER) {
		fc_req->transfered_len = cqe_entry->u.generic.word1.total_data_placed;

		spdk_nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_WRITE_BDEV);

		spdk_nvmf_request_exec(&fc_req->req);

		return;
	}
	/* Read Tranfer done */
	else if (fc_req->state == SPDK_NVMF_FC_REQ_READ_XFER) {

		fc_req->transfered_len = cqe_entry->u.generic.word1.total_data_placed;

		spdk_nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_READ_RSP);
		if (spdk_nvmf_fc_handle_rsp(fc_req)) {
			goto io_done;
		}
		return;
	}

	/* IO completed successfully */
	spdk_nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_SUCCESS);

io_done:
	if (fc_req->xchg) {
		nvmf_fc_cleanup_xri(fc_req->hwqp, fc_req->xchg,
					 cqe_entry->u.generic.xb,
					 nvmf_fc_abts_required(cqe));
		fc_req->xchg = NULL;
	}

	if (fc_req->is_aborted) {
		spdk_nvmf_fc_request_abort_complete(fc_req);
	} else {
		spdk_nvmf_fc_request_free(fc_req);
	}
}

static void
nvmf_fc_process_wqe_completion(struct spdk_nvmf_fc_hwqp *hwqp, uint16_t tag, int status,
			       uint8_t *cqe)
{
	fc_reqtag_t *reqtag;

	reqtag = nvmf_fc_lookup_reqtag(hwqp, tag);
	if (!reqtag) {
		SPDK_ERRLOG("Could not find reqtag(%d) for WQE Compl HWQP = %d\n",
			    tag, hwqp->hwqp_id);
		return;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LLD, "WQE Compl(%d)\n", status);

	/* Call the callback */
	if (reqtag->cb) {
		reqtag->cb(hwqp, cqe, status, reqtag->cb_args);
	} else {
		SPDK_ERRLOG("reqtag(%d) cb NULL for WQE Compl\n", tag);
	}

	/* Release reqtag */
	if (nvmf_fc_release_reqtag(hwqp, reqtag)) {
		SPDK_ERRLOG("%s: reqtag(%d) release failed\n", __func__, tag);
	}
}

static void
nvmf_fc_process_wqe_release(struct spdk_nvmf_fc_hwqp *hwqp, uint16_t wqid)
{
	BCM_HWQP(hwqp)->wq.q.used -= MAX_WQ_WQEC_CNT;
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LLD, "WQE RELEASE\n");
}

static uint32_t
nvmf_fc_fill_sgl(struct spdk_nvmf_fc_request *fc_req)
{
	uint32_t i;
	uint32_t offset = 0;
	uint64_t iov_phys;
	bcm_sge_t *sge = NULL;
	struct spdk_nvmf_fc_rq_buf_nvme_cmd *req_buf = NULL;
	struct spdk_nvmf_fc_hwqp *hwqp = fc_req->hwqp;

	assert((fc_req->req.iovcnt) <= BCM_MAX_IOVECS);
	if ((fc_req->req.iovcnt) > BCM_MAX_IOVECS) {
		SPDK_ERRLOG("Error: (fc_req->req.iovcnt) > BCM_MAX_IOVECS\n");
		return 0;
	}
	assert(fc_req->req.iovcnt != 0);
	if (fc_req->req.iovcnt == 0) {
		SPDK_ERRLOG("Error: fc_req->req.iovcnt == 0\n");
		return 0;
	}

	/* Use RQ buffer for SGL */
	req_buf = BCM_HWQP(hwqp)->rq_payload.buffer[fc_req->buf_index].virt;
	sge = &req_buf->sge[0];

	memset(sge, 0, (sizeof(bcm_sge_t) * BCM_MAX_IOVECS));

	if (fc_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) { /* Write */
		uint64_t xfer_rdy_phys;
		struct spdk_nvmf_fc_xfer_rdy_iu *xfer_rdy_iu;

		/* 1st SGE is tranfer ready buffer */
		xfer_rdy_iu = BCM_HWQP(hwqp)->rq_payload.buffer[fc_req->buf_index].virt +
			      offsetof(struct spdk_nvmf_fc_rq_buf_nvme_cmd, xfer_rdy);
		xfer_rdy_iu->relative_offset = 0;
		to_be32(&xfer_rdy_iu->burst_len, fc_req->req.length);

		xfer_rdy_phys = BCM_HWQP(hwqp)->rq_payload.buffer[fc_req->buf_index].phys +
				offsetof(struct spdk_nvmf_fc_rq_buf_nvme_cmd, xfer_rdy);

		sge->sge_type = BCM_SGE_TYPE_DATA;
		sge->buffer_address_low  = PTR_TO_ADDR32_LO(xfer_rdy_phys);
		sge->buffer_address_high = PTR_TO_ADDR32_HI(xfer_rdy_phys);
		sge->buffer_length = sizeof(struct spdk_nvmf_fc_xfer_rdy_iu);
		sge++;

	} else if (fc_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) { /* read */
		/* 1st SGE is skip. */
		sge->sge_type = BCM_SGE_TYPE_SKIP;
		sge++;
	} else {
		return 0;
	}

	/* 2nd SGE is skip. */
	sge->sge_type = BCM_SGE_TYPE_SKIP;
	sge++;

	for (i = 0; i < fc_req->req.iovcnt; i++) {
		size_t mapped_size = fc_req->req.iov[i].iov_len;

		iov_phys = spdk_vtophys(fc_req->req.iov[i].iov_base, &mapped_size);
		/* ocs_assert(mapped_size == fc_req->req.iov[i].iov_len, 0);
		 */
		sge->sge_type = BCM_SGE_TYPE_DATA;
		sge->buffer_address_low  = PTR_TO_ADDR32_LO(iov_phys);
		sge->buffer_address_high = PTR_TO_ADDR32_HI(iov_phys);
		sge->buffer_length = fc_req->req.iov[i].iov_len;
		sge->data_offset = offset;
		offset += fc_req->req.iov[i].iov_len;

		if (i == (fc_req->req.iovcnt - 1)) {
			/* last */
			sge->last = true;
		} else {
			sge++;
		}
	}
	return offset;
}

static int
nvmf_fc_recv_data(struct spdk_nvmf_fc_request *fc_req)
{
	int rc = 0;
	uint8_t wqe[128] = { 0 };
	bcm_fcp_treceive64_wqe_t *trecv = (bcm_fcp_treceive64_wqe_t *)wqe;
	struct spdk_nvmf_fc_hwqp *hwqp = fc_req->hwqp;

	if (!fc_req->req.iovcnt) {
		return -1;
	}

	trecv->xbl = true;
	if (fc_req->req.iovcnt == 1) {
		/* Data is a single physical address, use a BDE */
		uint64_t bde_phys;
		size_t phys_map_size;

		phys_map_size = (size_t)fc_req->req.length;
		bde_phys = spdk_vtophys(fc_req->req.iov[0].iov_base, &phys_map_size);
		/* ocs_assert(phys_map_size == (size_t)fc_req->req.length, 0);
		 */
		trecv->dbde = true;
		trecv->bde.bde_type = BCM_BDE_TYPE_BDE_64;
		trecv->bde.buffer_length = fc_req->req.length;
		trecv->bde.u.data.buffer_address_low = PTR_TO_ADDR32_LO(bde_phys);
		trecv->bde.u.data.buffer_address_high = PTR_TO_ADDR32_HI(bde_phys);
	} else {
		uint64_t sgl_phys;

		if (!nvmf_fc_fill_sgl(fc_req)) {
			return -1;
		}

		sgl_phys = BCM_HWQP(hwqp)->rq_payload.buffer[fc_req->buf_index].phys +
			   offsetof(struct spdk_nvmf_fc_rq_buf_nvme_cmd, sge);

		trecv->bde.bde_type = BCM_BDE_TYPE_BLP;
		trecv->bde.buffer_length = fc_req->req.length;
		trecv->bde.u.blp.sgl_segment_address_low = PTR_TO_ADDR32_LO(sgl_phys);
		trecv->bde.u.blp.sgl_segment_address_high = PTR_TO_ADDR32_HI(sgl_phys);
	}

	trecv->relative_offset = 0;
	trecv->xri_tag = fc_req->xchg->xchg_id;
	trecv->context_tag = fc_req->rpi;
	trecv->pu = true;
	trecv->ar = false;

	trecv->command = BCM_WQE_FCP_TRECEIVE64;
	trecv->class = BCM_ELS_REQUEST64_CLASS_3;
	trecv->ct = BCM_ELS_REQUEST64_CONTEXT_RPI;

	trecv->remote_xid = fc_req->oxid;
	trecv->nvme 	= 1;
	trecv->iod 	= 1;
	trecv->len_loc 	= 0x2;
	trecv->timer 	= 30;

	trecv->cmd_type = BCM_CMD_FCP_TRECEIVE64_WQE;
	trecv->cq_id = 0xFFFF;
	trecv->fcp_data_receive_length = fc_req->req.length;

	rc = nvmf_fc_post_wqe(hwqp, (uint8_t *)trecv, true, nvmf_fc_io_cmpl_cb, fc_req);
	if (!rc) {
		fc_req->xchg->active = true;
	}

	return rc;
}

static void
nvmf_fc_process_marker_cqe(struct spdk_nvmf_fc_hwqp *hwqp, uint8_t *cqe)
{
	bcm_fc_async_rcqe_marker_t *marker = (void *)cqe;
	struct spdk_nvmf_fc_poller_api_queue_sync_done_args *poller_args;

	poller_args = calloc(1, sizeof(struct spdk_nvmf_fc_poller_api_queue_sync_done_args)); 
	if (poller_args) {
		poller_args->hwqp = hwqp;
		poller_args->tag = (uint64_t)marker->tag_higher << 32 | marker->tag_lower;
		poller_args->cb_info.cb_thread = spdk_get_thread();
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LLD, "Process Marker compl for tag = %lx\n", 
			      poller_args->tag);
		spdk_nvmf_fc_poller_api_func(hwqp, SPDK_NVMF_FC_POLLER_API_QUEUE_SYNC_DONE,
					     poller_args);
	}
}

static int
nvmf_fc_process_rqpair(struct spdk_nvmf_fc_hwqp *hwqp, fc_eventq_t *cq, uint8_t *cqe)
{
	int rc = 0, rq_index = 0;
	uint16_t rq_id = 0;
	int32_t rq_status;
	uint32_t buff_idx = 0;
	struct spdk_nvmf_fc_frame_hdr *frame = NULL;
	struct spdk_nvmf_fc_buffer_desc *payload_buffer = NULL;
	bcm_fc_async_rcqe_t *rcqe = (bcm_fc_async_rcqe_t *)cqe;
	uint8_t code = cqe[BCM_CQE_CODE_OFFSET];

	assert(hwqp);
	if (hwqp == NULL) {
		SPDK_ERRLOG("Error: hwqp is NULL\n");
		return -1;
	}
	assert(cq);
	if (cq == NULL) {
		SPDK_ERRLOG("Error: cq is NULL\n");
		return -1;
	}
	assert(cqe);
	if (cqe == NULL) {
		SPDK_ERRLOG("Error: cqe is NULL\n");
		return -1;
	}

	rq_status = nvmf_fc_rqe_rqid_and_index(cqe, &rq_id, &rq_index);
	if (0 != rq_status) {
		switch (rq_status) {
		case BCM_FC_ASYNC_RQ_BUF_LEN_EXCEEDED:
		case BCM_FC_ASYNC_RQ_DMA_FAILURE:
			if (rq_index < 0 || rq_index >= BCM_HWQP(hwqp)->rq_hdr.q.max_entries) {
				SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LLD,
					      "%s: status=%#x: rq_id lookup failed for id=%#x\n",
					      __func__, rq_status, rq_id);
				hwqp->counters.rq_buf_len_err++;
				break;
			}

			buff_idx = nvmf_fc_rqpair_get_buffer_id(hwqp, rq_index);
			goto buffer_release;

		case BCM_FC_ASYNC_RQ_INSUFF_BUF_NEEDED:
		case BCM_FC_ASYNC_RQ_INSUFF_BUF_FRM_DISC:
			SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LLD,
				      "%s: Warning: RCQE status=%#x, \n",
				      __func__, rq_status);
			hwqp->counters.rq_status_err++;
		default:
			break;
		}

		/* Buffer not consumed. No need to return */
		return -1;
	}

	/* Make sure rq_index is in range */
	if (rq_index >= BCM_HWQP(hwqp)->rq_hdr.q.max_entries) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LLD,
			      "%s: Error: rq index out of range for RQ%d\n",
			      __func__, rq_id);
		hwqp->counters.rq_index_err++;
		return -1;
	}

	/* Process NVME frame */
	buff_idx = nvmf_fc_rqpair_get_buffer_id(hwqp, rq_index);
	frame = nvmf_fc_rqpair_get_frame_header(hwqp, rq_index);
	payload_buffer = nvmf_fc_rqpair_get_frame_buffer(hwqp, rq_index);

	if (code == BCM_CQE_CODE_RQ_MARKER) {
		/* Process marker completion */
		nvmf_fc_process_marker_cqe(hwqp, cqe);
	} else {
		rc = spdk_nvmf_fc_hwqp_process_frame(hwqp, buff_idx, frame, payload_buffer,
						    rcqe->payload_data_placement_length);
		if (!rc) {
			return 0;
		}
	}

buffer_release:
	/* Return buffer to chip */
	nvmf_fc_rqpair_buffer_release(hwqp, buff_idx);
	return rc;
}

static int
nvmf_fc_process_cq_entry(struct spdk_nvmf_fc_hwqp *hwqp, struct fc_eventq *cq)
{
	int rc = 0, budget = cq->q.processed_limit;
	uint8_t	cqe[sizeof(cqe_t)];
	uint16_t rid = UINT16_MAX;
	uint32_t n_processed = 0;
	bcm_qentry_type_e ctype;     /* completion type */

	assert(hwqp);
	assert(cq);

	while (!nvmf_fc_read_queue_entry(&cq->q, &cqe[0])) {
		n_processed++;
		budget --;

		rc = nvmf_fc_parse_cq_entry(cq, cqe, &ctype, &rid);
		/*
		 * The sign of status is significant. If status is:
		 * == 0 : call completed correctly and the CQE indicated success
		 *  > 0 : call completed correctly and the CQE indicated an error
		 *  < 0 : call failed and no information is available about the CQE
		 */
		if (rc < 0) {
			if ((rc == -2) && budget) {
				/* Entry was consumed */
				continue;
			}
			break;
		}

		switch ((int)ctype) {
		case BCM_FC_QENTRY_WQ:
			nvmf_fc_process_wqe_completion(hwqp, rid, rc, cqe);
			break;
		case BCM_FC_QENTRY_WQ_RELEASE:
			nvmf_fc_process_wqe_release(hwqp, rid);
			break;
		case BCM_FC_QENTRY_RQ:
			nvmf_fc_process_rqpair(hwqp, cq, cqe);
			break;
		case BCM_FC_QENTRY_XABT:
			nvmf_fc_nvmf_del_xri_pending(hwqp, rid);
			break;
		default:
			SPDK_WARNLOG("%s: unhandled ctype=%#x rid=%#x\n",
				     __func__, ctype, rid);
			hwqp->counters.invalid_cq_type++;
			break;
		}

		if (n_processed >= (cq->q.posted_limit)) {
			nvmf_fc_bcm_notify_queue(&cq->q, false, n_processed);
			n_processed = 0;
		}

		if (!budget || (hwqp->state == SPDK_FC_HWQP_OFFLINE)) {
			break;
		}
	}

	nvmf_fc_bcm_notify_queue(&cq->q, cq->auto_arm_flag, n_processed);

	return rc;
}

static uint32_t
nvmf_fc_process_queue(struct spdk_nvmf_fc_hwqp *hwqp)
{
	int rc = 0, budget = 0;
	uint32_t n_processed = 0;
	uint32_t n_processed_total = 0;
	uint8_t eqe[sizeof(eqe_t)] = { 0 };
	uint16_t cq_id;
	struct fc_eventq *eq;
	bool pending_req_processed = false;

	assert(hwqp);
	if (hwqp == NULL) {
		SPDK_ERRLOG("Error: hwqp is NULL\n");
		return 0;
	}

	eq = &BCM_HWQP(hwqp)->eq;

	budget = eq->q.processed_limit;

	while (!nvmf_fc_read_queue_entry(&eq->q, &eqe[0])) {
		n_processed++;
		budget --;

		rc = nvmf_fc_parse_eq_entry((struct eqe *)eqe, &cq_id);
		if (spdk_likely(rc))  {
			if (rc > 0) {
				/* EQ is full.  Process all CQs */
				nvmf_fc_process_cq_entry(hwqp, &BCM_HWQP(hwqp)->cq_wq);
				nvmf_fc_process_cq_entry(hwqp, &BCM_HWQP(hwqp)->cq_rq);
				pending_req_processed = true;
			} else {
				break;
			}
		} else {
			if (cq_id == BCM_HWQP(hwqp)->cq_wq.q.qid) {
				nvmf_fc_process_cq_entry(hwqp, &BCM_HWQP(hwqp)->cq_wq);
				/*
				 * There might be some buffers/xri freed.
				 * First give chance for pending frames
				 */
				spdk_nvmf_fc_hwqp_process_pending_ls_rqsts(hwqp);
				spdk_nvmf_fc_hwqp_process_pending_reqs(hwqp);
			} else if (cq_id == BCM_HWQP(hwqp)->cq_rq.q.qid) {
				nvmf_fc_process_cq_entry(hwqp, &BCM_HWQP(hwqp)->cq_rq);
			} else {
				SPDK_ERRLOG("%s bad CQ_ID %#06x\n", __func__, cq_id);
				hwqp->counters.invalid_cq_id++;
			}
		}

		if (n_processed >= (eq->q.posted_limit)) {
			nvmf_fc_bcm_notify_queue(&eq->q, false, n_processed);
			n_processed_total += n_processed;
			n_processed = 0;
		}

		if (!budget || (hwqp->state == SPDK_FC_HWQP_OFFLINE)) {
			break;
		}
	}

	if (!pending_req_processed) {
		spdk_nvmf_fc_hwqp_process_pending_reqs(hwqp);
	}

	if (n_processed) {
		nvmf_fc_bcm_notify_queue(&eq->q, eq->auto_arm_flag, n_processed);
	}

	return (n_processed + n_processed_total);
}

static int
nvmf_fc_xmt_ls_rsp(struct spdk_nvmf_fc_nport *tgtport,
			    struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	uint8_t wqe[128] = { 0 };
	bcm_xmit_sequence64_wqe_t *xmit = (bcm_xmit_sequence64_wqe_t *)wqe;
	struct spdk_nvmf_fc_hwqp *hwqp = NULL;
	int rc = -1;

	xmit->xbl = true;
	xmit->bde.bde_type = BCM_BDE_TYPE_BDE_64;
	xmit->bde.buffer_length = ls_rqst->rsp_len;
	xmit->bde.u.data.buffer_address_low  = PTR_TO_ADDR32_LO(ls_rqst->rspbuf.phys);
	xmit->bde.u.data.buffer_address_high = PTR_TO_ADDR32_HI(ls_rqst->rspbuf.phys);
	xmit->sequence_payload_len = ls_rqst->rsp_len;
	xmit->relative_offset = 0;

	xmit->si = 0;
	xmit->ft = 0;
	xmit->xo = 0;
	xmit->ls = 1;
	xmit->dbde = 1;

	xmit->dif 	= 0;
	xmit->pu 	= 0;
	xmit->abort_tag = 0;
	xmit->bs 	= 0;

	xmit->df_ctl	= 0;
	xmit->type 	= FCNVME_TYPE_NVMF_DATA;
	xmit->r_ctl 	= FCNVME_R_CTL_LS_RESPONSE;

	xmit->cmd_type 	= BCM_CMD_XMIT_SEQUENCE64_WQE;
	xmit->command 	= BCM_WQE_XMIT_SEQUENCE64;
	xmit->class 	= BCM_ELS_REQUEST64_CLASS_3;
	xmit->ct 	= BCM_ELS_REQUEST64_CONTEXT_RPI;
	xmit->iod 	= BCM_ELS_REQUEST64_DIR_WRITE;

	xmit->xri_tag	 = ls_rqst->xchg->xchg_id;
	xmit->remote_xid  = ls_rqst->oxid;
	xmit->context_tag = ls_rqst->rpi;

	xmit->len_loc 	= 2;
	xmit->cq_id 	= 0xFFFF;

	hwqp = (struct spdk_nvmf_fc_hwqp *)ls_rqst->private_data;

	rc = nvmf_fc_post_wqe(hwqp, (uint8_t *)xmit, true, nvmf_fc_ls_rsp_cmpl_cb, ls_rqst);
	if (!rc) {
		ls_rqst->xchg->active = true;
	}

	return rc;
}

static int
nvmf_fc_send_data(struct spdk_nvmf_fc_request *fc_req)
{
	int rc = 0;
	uint8_t wqe[128] = { 0 };
	uint32_t xfer_len = 0;
	bcm_fcp_tsend64_wqe_t *tsend = (bcm_fcp_tsend64_wqe_t *)wqe;
	struct spdk_nvmf_fc_hwqp *hwqp = fc_req->hwqp;
	struct spdk_nvmf_qpair *qpair = fc_req->req.qpair;
	struct spdk_nvmf_fc_conn *fc_conn = spdk_nvmf_fc_get_conn(qpair);

	if (!fc_req->req.iovcnt) {
		return -1;
	}

	tsend->xbl = true;
	if (fc_req->req.iovcnt == 1) {
		/* Data is a single physical address, use a BDE */
		uint64_t bde_phys;
		size_t phys_map_size;

		phys_map_size = (size_t)fc_req->req.length;
		bde_phys = spdk_vtophys(fc_req->req.iov[0].iov_base, &phys_map_size);
		/* ocs_assert(phys_map_size == (size_t)fc_req->req.length, 0);
		 */
		tsend->dbde = true;
		tsend->bde.bde_type = BCM_BDE_TYPE_BDE_64;
		tsend->bde.buffer_length = fc_req->req.length;
		tsend->bde.u.data.buffer_address_low = PTR_TO_ADDR32_LO(bde_phys);
		tsend->bde.u.data.buffer_address_high = PTR_TO_ADDR32_HI(bde_phys);

		xfer_len = fc_req->req.iov[0].iov_len;
	} else {
		uint64_t sgl_phys;

		xfer_len = nvmf_fc_fill_sgl(fc_req);
		if (!xfer_len) {
			return -1;
		}

		sgl_phys = BCM_HWQP(hwqp)->rq_payload.buffer[fc_req->buf_index].phys +
			   offsetof(struct spdk_nvmf_fc_rq_buf_nvme_cmd, sge);

		tsend->bde.bde_type = BCM_BDE_TYPE_BLP;
		tsend->bde.buffer_length = fc_req->req.length;
		tsend->bde.u.blp.sgl_segment_address_low = PTR_TO_ADDR32_LO(sgl_phys);
		tsend->bde.u.blp.sgl_segment_address_high = PTR_TO_ADDR32_HI(sgl_phys);
	}

	tsend->relative_offset = 0;
	tsend->xri_tag = fc_req->xchg->xchg_id;
	tsend->rpi = fc_req->rpi;
	tsend->pu = true;

	if (!spdk_nvmf_fc_send_ersp_required(fc_req, (fc_conn->rsp_count + 1),
					xfer_len)) {
		fc_conn->rsp_count++;
		spdk_nvmf_fc_advance_conn_sqhead(qpair);
		tsend->ar = true;
		spdk_nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_READ_RSP);
	}

	tsend->command = BCM_WQE_FCP_TSEND64;
	tsend->class = BCM_ELS_REQUEST64_CLASS_3;
	tsend->ct = BCM_ELS_REQUEST64_CONTEXT_RPI;
	tsend->remote_xid = fc_req->oxid;
	tsend->nvme = 1;
	tsend->len_loc = 0x2;

	tsend->cmd_type = BCM_CMD_FCP_TSEND64_WQE;
	tsend->cq_id = 0xFFFF;
	tsend->fcp_data_transmit_length = fc_req->req.length;

	rc = nvmf_fc_post_wqe(hwqp, (uint8_t *)tsend, true, nvmf_fc_io_cmpl_cb, fc_req);
	if (!rc) {
		fc_req->xchg->active = true;
	}

	return rc;
}

static void
nvmf_fc_sendframe_cmpl_cb(void *ctx, uint8_t *cqe, int32_t status, void *arg)
{
	if (status) {
		SPDK_ERRLOG("SendFrame WQE Compl(%d) error\n", status);
	}
}

static int
nvmf_fc_send_frame(struct spdk_nvmf_fc_hwqp *hwqp,
		   uint32_t s_id, uint32_t d_id, uint16_t ox_id,
		   uint8_t htype, uint8_t r_ctl, uint32_t f_ctl,
		   uint8_t *payload, uint32_t plen)

{
	uint8_t wqe[128] = { 0 };
	uint32_t *p_hdr;
	int rc = -1;
	bcm_send_frame_wqe_t *sf = (bcm_send_frame_wqe_t *)wqe;
	fc_frame_hdr_le_t hdr;

	/*
	 * Make sure we dont have payload greater than 64 bytes which
	 * is the space availble to inline in WQE.
	*/
	if (plen > 64) {
		return -1;
	}

	/* Build header */
	memset(&hdr, 0, sizeof(fc_frame_hdr_le_t));

	hdr.d_id	 = s_id;
	hdr.s_id	 = d_id;
	hdr.r_ctl	 = r_ctl;
	hdr.cs_ctl	 = 0;
	hdr.f_ctl	 = f_ctl;
	hdr.type	 = htype;
	hdr.seq_cnt	 = 0;
	hdr.df_ctl	 = 0;
	hdr.rx_id	 = 0xffff;
	hdr.ox_id	 = ox_id;
	hdr.parameter	 = 0;

	/* Assign a SEQID. */
	hdr.seq_id = BCM_HWQP(hwqp)->send_frame_seqid++;
	p_hdr = (uint32_t *)&hdr;

	/* Fill header in WQE */
	sf->fc_header_0_1[0] = p_hdr[0];
	sf->fc_header_0_1[1] = p_hdr[1];
	sf->fc_header_2_5[0] = p_hdr[2];
	sf->fc_header_2_5[1] = p_hdr[3];
	sf->fc_header_2_5[2] = p_hdr[4];
	sf->fc_header_2_5[3] = p_hdr[5];

	/* If payload present, copy inline in wqe. */
	if (plen) {
		sf->frame_length = plen;
		sf->dbde = true;
		sf->bde.bde_type = BCM_BDE_TYPE_BDE_IMM;
		sf->bde.buffer_length = plen;
		sf->bde.u.imm.offset = 64;
		memcpy(&sf->inline_rsp, payload, plen);
	}
	
	sf->xri_tag	 = BCM_HWQP(hwqp)->send_frame_xri;
	sf->command	 = BCM_WQE_SEND_FRAME;
	sf->sof		 = 0x2e; /* SOFI3 */
	sf->eof		 = 0x42; /* EOFT */
	sf->wqes	 = 1;
	sf->iod		 = BCM_ELS_REQUEST64_DIR_WRITE;
	sf->qosd	 = 0;    /* Include in QOS */
	sf->lenloc	 = 1;
	sf->xc		 = 1;
	sf->xbl		 = 1;
	sf->cmd_type	 = BCM_CMD_SEND_FRAME_WQE;
	sf->cq_id	 = 0xffff;

	rc = nvmf_fc_post_wqe(hwqp, (uint8_t *)sf, true, nvmf_fc_sendframe_cmpl_cb, NULL);

	return rc;
}

static int
nvmf_fc_sendframe_rsp(struct spdk_nvmf_fc_request *fc_req,
			 uint8_t *ersp_buf, uint32_t ersp_len)
{
	int rc = 0;
	uint8_t good_rsp[FCNVME_GOOD_RSP_LEN] = { 0 };
	uint8_t *rsp = NULL, rsp_len, rctl;

	if (!ersp_buf) {
		rsp 	= good_rsp;
		rsp_len = FCNVME_GOOD_RSP_LEN;
		rctl	= FCNVME_R_CTL_STATUS;
	} else {
		rsp	= ersp_buf;
		rsp_len	= ersp_len;
		rctl	= FCNVME_R_CTL_ERSP_STATUS;
	}

	rc = nvmf_fc_send_frame(fc_req->hwqp, fc_req->s_id, fc_req->d_id, fc_req->oxid,
				FCNVME_TYPE_FC_EXCHANGE, rctl, FCNVME_F_CTL_RSP, rsp, rsp_len);
	if (rc) {
		SPDK_ERRLOG("%s: SendFrame failed. rc = %d\n", __func__, rc);
	} else {
		cqe_t cqe;

		memset(&cqe, 0, sizeof(cqe_t));
		cqe.u.wcqe.status = BCM_FC_WCQE_STATUS_SUCCESS;

		/*
  		 * In case of success, dont care for sendframe wqe completion.
		 * treat as the FC_REQ completed successfully.
		*/
		nvmf_fc_io_cmpl_cb(fc_req->hwqp, (uint8_t *)&cqe,
				   BCM_FC_WCQE_STATUS_SUCCESS, fc_req);
	}

	return rc;
}

static int
nvmf_fc_xmt_rsp(struct spdk_nvmf_fc_request *fc_req, uint8_t *ersp_buf, uint32_t ersp_len)
{
	int rc = 0;
	uint8_t wqe[128] = { 0 };
	bcm_fcp_trsp64_wqe_t *trsp = (bcm_fcp_trsp64_wqe_t *)wqe;
	struct spdk_nvmf_fc_hwqp *hwqp = fc_req->hwqp;

	if (spdk_nvmf_fc_use_send_frame(&fc_req->req)) {
		return nvmf_fc_sendframe_rsp(fc_req, ersp_buf, ersp_len);
	}

	if (!ersp_buf) {
		/* Auto-Gen all zeroes in IU 12-byte payload */
		trsp->ag = true;
	} else {
		trsp->wqes = 1;
		trsp->irsp = 1;
		trsp->fcp_response_length = ersp_len;
		trsp->irsplen = (ersp_len >> 2) - 1;
		memcpy(&trsp->inline_rsp, ersp_buf, ersp_len);
	}

	if (fc_req->xchg->active) {
		trsp->xc = true;
	}

	trsp->command = BCM_WQE_FCP_TRSP64;
	trsp->class = BCM_ELS_REQUEST64_CLASS_3;
	trsp->xri_tag = fc_req->xchg->xchg_id;
	trsp->remote_xid  = fc_req->oxid;
	trsp->rpi = fc_req->rpi;
	trsp->len_loc = 0x1;
	trsp->cq_id = 0xFFFF;
	trsp->cmd_type = BCM_CMD_FCP_TRSP64_WQE;
	trsp->nvme = 1;

	rc = nvmf_fc_post_wqe(hwqp, (uint8_t *)trsp, true, nvmf_fc_io_cmpl_cb, fc_req);
	if (!rc) {
		fc_req->xchg->active = true;
	}

	return rc;
}

static int
nvmf_fc_xmt_bls_rsp(struct spdk_nvmf_fc_hwqp *hwqp,
			     uint16_t ox_id, uint16_t rx_id,
			     uint16_t rpi, bool rjt, uint8_t rjt_exp,
			     spdk_nvmf_fc_caller_cb cb, void *cb_args)
{
	uint8_t wqe[128] = { 0 };
	bcm_xmit_bls_rsp_wqe_t *bls = (bcm_xmit_bls_rsp_wqe_t *)wqe;
	int rc = -1;
	struct spdk_nvmf_fc_caller_ctx *ctx = NULL;
	struct spdk_nvmf_fc_xchg *xri = NULL;

	xri = nvmf_fc_get_xri(hwqp);
	if (!xri) {
		goto done;
	}

	ctx = calloc(1, sizeof(struct spdk_nvmf_fc_caller_ctx));
	if (!ctx) {
		goto done;
	}
	ctx->ctx = xri;
	ctx->cb = cb;
	ctx->cb_args = cb_args;

	if (rjt) {
		bls->payload_word0 = ((uint32_t)FCNVME_BLS_REJECT_UNABLE_TO_PERFORM << 16) |
				     ((uint32_t)rjt_exp << 8);
		bls->ar = true;
	} else {
		bls->high_seq_cnt = UINT16_MAX;
	}

	bls->ox_id       = ox_id;
	bls->rx_id       = rx_id;
	bls->ct          = BCM_ELS_REQUEST64_CONTEXT_RPI;
	bls->context_tag = rpi;
	bls->xri_tag     = xri->xchg_id;
	bls->class       = BCM_ELS_REQUEST64_CLASS_3;
	bls->command     = BCM_WQE_XMIT_BLS_RSP;
	bls->qosd        = true;
	bls->cq_id       = UINT16_MAX;
	bls->cmd_type    = BCM_CMD_XMIT_BLS_RSP64_WQE;

	rc = nvmf_fc_post_wqe(hwqp, (uint8_t *)bls, true, nvmf_fc_bls_cmpl_cb, ctx);
done:
	if (rc && ctx) {
		free(ctx);
	}

	if (rc && xri) {
		nvmf_fc_put_xri(hwqp, xri);
	}

	if (!rc) {
		xri->active = true;
	}

	return rc;
}

static struct spdk_nvmf_fc_srsr_bufs*
nvmf_fc_alloc_srsr_bufs(size_t rqst_len, size_t rsp_len)
{
	static uint32_t mz_ind = 0;
	struct nvmf_fc_srsr_bufs *lld_srsr_bufs;
	struct spdk_nvmf_fc_srsr_bufs *ret_bufs; 

	/* allocate the larger internal structure used to manage srsr buffers */
	lld_srsr_bufs = calloc(1, sizeof(struct nvmf_fc_srsr_bufs)); 

	if (!lld_srsr_bufs) {
		SPDK_ERRLOG("No memory to alloc send disconnect buffer struct\n");
		return NULL;
	} 

	ret_bufs = &lld_srsr_bufs->srsr_bufs;
	ret_bufs->rqst_len = rqst_len;
	ret_bufs->rsp_len = rsp_len;
	lld_srsr_bufs->sgl_len = 2 * sizeof(bcm_sge_t);

	snprintf(lld_srsr_bufs->mz_name, sizeof(lld_srsr_bufs->mz_name), "assoc_dcbuf_%x", mz_ind++);
	ret_bufs->rqst = spdk_memzone_reserve(lld_srsr_bufs->mz_name, rqst_len + rsp_len +
					      lld_srsr_bufs->sgl_len, SPDK_ENV_SOCKET_ID_ANY, 0);

	if (ret_bufs->rqst == NULL) {
		SPDK_ERRLOG("No memory to alloc send disconnect buffer struct\n");
		free(lld_srsr_bufs);
		return NULL;
	}

	lld_srsr_bufs->rqst_phys = spdk_vtophys(lld_srsr_bufs->srsr_bufs.rqst, NULL);
	lld_srsr_bufs->srsr_bufs.rsp = lld_srsr_bufs->srsr_bufs.rqst + ret_bufs->rqst_len;
	lld_srsr_bufs->rsp_phys = lld_srsr_bufs->rqst_phys + ret_bufs->rqst_len;
	lld_srsr_bufs->sgl_virt = lld_srsr_bufs->srsr_bufs.rsp + ret_bufs->rsp_len;
	lld_srsr_bufs->sgl_phys = lld_srsr_bufs->rsp_phys + ret_bufs->rsp_len;

	return ret_bufs;
}

static void
nvmf_fc_free_srsr_bufs(struct spdk_nvmf_fc_srsr_bufs *srsr_bufs)
{
	if (srsr_bufs) {
		struct nvmf_fc_srsr_bufs *b; 
		b = SPDK_CONTAINEROF(srsr_bufs, struct nvmf_fc_srsr_bufs, srsr_bufs);
		if (b) {
			spdk_memzone_free(b->mz_name);
		}
		free(srsr_bufs);
	}
}

static int
nvmf_fc_xmt_srsr_req(struct spdk_nvmf_fc_hwqp *hwqp,
		     struct spdk_nvmf_fc_srsr_bufs *xmt_srsr_bufs,
		     spdk_nvmf_fc_caller_cb cb, void *cb_args)
{
	struct nvmf_fc_srsr_bufs *srsr_bufs;
	uint8_t wqe[128] = { 0 };
	int rc = -1;
	bcm_gen_request64_wqe_t *gen = (bcm_gen_request64_wqe_t *)wqe;
	struct spdk_nvmf_fc_caller_ctx *ctx = NULL;
	struct spdk_nvmf_fc_xchg *xri = NULL;
	bcm_sge_t *sge = NULL;

	srsr_bufs = SPDK_CONTAINEROF(xmt_srsr_bufs, struct nvmf_fc_srsr_bufs, srsr_bufs);
	if (!srsr_bufs) {
		goto done;
	}

	/* Make sure caller allocated space for two sges. */
	if (srsr_bufs->sgl_len != (2 * sizeof(bcm_sge_t))) {
		goto done;
	}
	sge = srsr_bufs->sgl_virt;
	memset(sge, 0, (sizeof(bcm_sge_t) * 2));

	xri = nvmf_fc_get_xri(hwqp);
	if (!xri) {
		/* Might be we should reserve some XRI for this */
		goto done;
	}

	ctx = calloc(1, sizeof(struct spdk_nvmf_fc_caller_ctx));
	if (!ctx) {
		goto done;
	}
	ctx->ctx = xri;
	ctx->cb = cb;
	ctx->cb_args = cb_args;

	/* Fill SGL */
	sge->buffer_address_high = PTR_TO_ADDR32_HI(srsr_bufs->rqst_phys);
	sge->buffer_address_low  = PTR_TO_ADDR32_LO(srsr_bufs->rqst_phys);
	sge->sge_type = BCM_SGE_TYPE_DATA;
	sge->buffer_length = srsr_bufs->srsr_bufs.rqst_len;
	sge ++;

	sge->buffer_address_high = PTR_TO_ADDR32_HI(srsr_bufs->rsp_phys);
	sge->buffer_address_low  = PTR_TO_ADDR32_LO(srsr_bufs->rsp_phys);
	sge->sge_type = BCM_SGE_TYPE_DATA;
	sge->buffer_length = srsr_bufs->srsr_bufs.rsp_len;
	sge->last = true;

	/* Fill WQE contents */
	gen->xbl = true;
	gen->bde.bde_type = BCM_BDE_TYPE_BLP;
	gen->bde.buffer_length = 2 * sizeof(bcm_sge_t);
	gen->bde.u.data.buffer_address_low  = PTR_TO_ADDR32_LO(srsr_bufs->sgl_phys);
	gen->bde.u.data.buffer_address_high = PTR_TO_ADDR32_HI(srsr_bufs->sgl_phys);

	gen->request_payload_length = srsr_bufs->srsr_bufs.rqst_len;
	gen->max_response_payload_length = srsr_bufs->srsr_bufs.rsp_len;
	gen->df_ctl	 = 0;
	gen->type	 = FCNVME_TYPE_NVMF_DATA;
	gen->r_ctl	 = FCNVME_R_CTL_LS_REQUEST;
	gen->xri_tag	 = xri->xchg_id;
	gen->ct		 = BCM_ELS_REQUEST64_CONTEXT_RPI;
	gen->context_tag = srsr_bufs->srsr_bufs.rpi;
	gen->class	 = BCM_ELS_REQUEST64_CLASS_3;
	gen->command	 = BCM_WQE_GEN_REQUEST64;
	gen->timer	 = 30;
	gen->iod	 = BCM_ELS_REQUEST64_DIR_READ;
	gen->qosd	 = true;
	gen->cmd_type	 = BCM_CMD_GEN_REQUEST64_WQE;
	gen->cq_id	 = 0xffff;

	rc = nvmf_fc_post_wqe(hwqp, (uint8_t *)gen, true, nvmf_fc_srsr_cmpl_cb,
			      ctx);
done:
	if (rc && ctx) {
		free(ctx);
	}

	if (rc && xri) {
		nvmf_fc_put_xri(hwqp, xri);
	}

	if (!rc) {
		xri->active = true;
	}

	return rc;
}

static bool
nvmf_fc_q_sync_available(void) {
	return true;
}

static int
nvmf_fc_issue_q_sync(struct spdk_nvmf_fc_hwqp *hwqp, uint64_t u_id, uint16_t skip_rq)
{
	uint8_t wqe[128] = { 0 };
	bcm_marker_wqe_t *marker = (bcm_marker_wqe_t *)wqe;

	if (skip_rq != UINT16_MAX) {
		marker->marker_catagery = BCM_MARKER_CATAGORY_ALL_RQ_EXCEPT_ONE;
		marker->rq_id = skip_rq;
	} else {
		marker->marker_catagery = BCM_MARKER_CATAGORY_ALL_RQ;
	}

	marker->tag_lower	= PTR_TO_ADDR32_LO(u_id);
	marker->tag_higher	= PTR_TO_ADDR32_HI(u_id);
	marker->command		= BCM_WQE_MARKER;
	marker->cmd_type	= BCM_CMD_MARKER_WQE;
	marker->qosd		= 1;
	marker->cq_id		= UINT16_MAX;

	return nvmf_fc_post_wqe(hwqp, (uint8_t *)marker, true, nvmf_fc_def_cmpl_cb, NULL);
}

static inline uint64_t
nvmf_fc_gen_conn_id(uint32_t qnum, struct spdk_nvmf_fc_hwqp *hwqp)
{
	struct bcm_nvmf_hw_queues *hwq = BCM_HWQP(hwqp);

	hwq->cid_cnt++;
	return ((hwqp->fc_port->num_io_queues * hwq->cid_cnt) + qnum);
}

static bool
nvmf_fc_assign_conn_to_hwqp(struct spdk_nvmf_fc_hwqp *hwqp,
			    uint64_t *conn_id, uint32_t sq_size)
{
 	struct bcm_nvmf_hw_queues *hwq = (struct bcm_nvmf_hw_queues *)hwqp->queues;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS, "Assign connection to HWQP\n");

	if (hwq->free_rq_slots < sq_size) {
		return false; /* no queue has space of this connection */
	}

	hwq->free_rq_slots -= sq_size;
	hwqp->num_conns++;

	/* create connection ID */
	*conn_id = nvmf_fc_gen_conn_id(hwqp->hwqp_id, hwqp);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS,
		      "QP assign to %d (free %d), conn_id 0x%lx\n",
		      hwqp->hwqp_id, hwq->free_rq_slots, *conn_id);

	return true;
}

static struct spdk_nvmf_fc_hwqp *
nvmf_fc_get_hwqp_from_conn_id(struct spdk_nvmf_fc_hwqp *queues, 
			      uint32_t num_queues, uint64_t conn_id)
{
	return &queues[conn_id % num_queues];
}

static void
nvmf_fc_release_conn(struct spdk_nvmf_fc_hwqp *hwqp, uint64_t conn_id,
		     uint32_t sq_size)
{ 
	hwqp->num_conns--;
	BCM_HWQP(hwqp)->free_rq_slots += sq_size;
}

/*
 * Dump queue entry
 */
static void
nvmf_fc_dump_buffer(struct spdk_nvmf_fc_queue_dump_info *dump_info,
		    const char *name, void *buffer, uint32_t size)
{
	uint32_t *dword;
	uint32_t  i;
	uint32_t  count;

	/*
	 * Print a max of 8 dwords per buffer line.
	 */
#define NVMF_TGT_FC_NEWLINE_MOD 8

	/*
	 * Make sure the print data  size is non-zero.
	 */
	count = size / sizeof(uint32_t);
	if (count == 0) {
		return;
	}

	spdk_nvmf_fc_dump_buf_print(dump_info, "%s type=buffer:", (char *)name);
	dword = buffer;

	for (i = 0; i < count; i++) {
		spdk_nvmf_fc_dump_buf_print(dump_info, "%08x", *dword++);
		if ((i % NVMF_TGT_FC_NEWLINE_MOD) == (NVMF_TGT_FC_NEWLINE_MOD - 1)) {
			spdk_nvmf_fc_dump_buf_print(dump_info, "\n");
		}
	}
}

/*
 * Dump queue entries.
 */
static void
nvmf_fc_dump_queue_entries(struct spdk_nvmf_fc_queue_dump_info *dump_info,
			   bcm_sli_queue_t *q)
{
#define NVMF_TGT_FC_QDUMP_RADIUS 1
	char     name[64];
	int32_t  index = 0;
	uint8_t *entry = NULL;
	uint32_t i;

	index = q->tail;

	index -= NVMF_TGT_FC_QDUMP_RADIUS;
	if (index < 0) {
		index += q->max_entries;
	}

	/*
	 * Print the NVMF_TGT_FC_QDUMP_RADIUS number of entries before and
	 * the tail index.
	 */
	for (i = 0; i < 2 * NVMF_TGT_FC_QDUMP_RADIUS + 1; i++) {
		bzero(name, sizeof(name));
		(void)snprintf(name, sizeof(name), "\nentry:%d ", index);
		entry = q->address;
		entry += index * q->size;
		nvmf_fc_dump_buffer(dump_info, name, entry, q->size);
		index++;
		if (index >= q->max_entries) {
			index = 0;
		}
	}
	spdk_nvmf_fc_dump_buf_print(dump_info, "\n");
}

/*
 * Dump the contents of Event Q.
 */
static void
nvmf_fc_dump_sli_queue(struct spdk_nvmf_fc_queue_dump_info *dump_info, char *name,
		       bcm_sli_queue_t *q)
{
	spdk_nvmf_fc_dump_buf_print(dump_info,
				   "\nname:%s, head:%" PRIu16 ", tail:%" PRIu16 ", used:%" PRIu16 ", "
				   "posted_limit:%" PRIu32 ", processed_limit:%" PRIu32 ", "
				   "type:%" PRIu16 ", qid:%" PRIu16 ", size:%" PRIu16 ", "
				   "max_entries:%" PRIu16 ", address:%p",
				   name, q->head, q->tail, q->used, q->posted_limit, q->processed_limit,
				   q->type, q->qid, q->size, q->max_entries, q->address);

	nvmf_fc_dump_queue_entries(dump_info, q);
}

/*
 * Dump the contents of Event Q.
 */
static void
nvmf_fc_dump_eventq(struct spdk_nvmf_fc_queue_dump_info *dump_info,
		    char *name, fc_eventq_t *eq)
{
	nvmf_fc_dump_sli_queue(dump_info, name, &eq->q);
}

/*
 * Dump the contents of Work Q.
 */
static void
nvmf_fc_dump_wrkq(struct spdk_nvmf_fc_queue_dump_info *dump_info,
		  char *name, fc_wrkq_t *wq)
{
	nvmf_fc_dump_sli_queue(dump_info, name, &wq->q);
}

/*
 * Dump the contents of recv Q.
 */
static void
nvmf_fc_dump_rcvq(struct spdk_nvmf_fc_queue_dump_info *dump_info, char *name, fc_rcvq_t *rq)
{
	nvmf_fc_dump_sli_queue(dump_info, name, &rq->q);
}

/*
 * Dump the contents of fc_hwqp.
 */
static void
nvmf_fc_dump_hwqp(struct spdk_nvmf_fc_queue_dump_info *dump_info,
		  struct bcm_nvmf_hw_queues *hw_queue)
{
	/*
	 * Dump the EQ.
	 */
	nvmf_fc_dump_eventq(dump_info, "eq", &hw_queue->eq);

	/*
	 * Dump the CQ-WQ.
	 */
	nvmf_fc_dump_eventq(dump_info, "cq_wq", &hw_queue->cq_wq);

	/*
	 * Dump the CQ-RQ.
	 */
	nvmf_fc_dump_eventq(dump_info, "cq_rq", &hw_queue->cq_rq);

	/*
	 * Dump the WQ.
	 */
	nvmf_fc_dump_wrkq(dump_info, "wq", &hw_queue->wq);

	/*
	 * Dump the RQ-HDR.
	 */
	nvmf_fc_dump_rcvq(dump_info, "rq_hdr", &hw_queue->rq_hdr);

	/*
	 * Dump the RQ-PAYLOAD.
	 */
	nvmf_fc_dump_rcvq(dump_info, "rq_payload", &hw_queue->rq_payload);
}

/*
 * Dump the hwqps.
 */
static void
nvmf_fc_dump_all_queues(struct spdk_nvmf_fc_hwqp *ls_queue,
			struct spdk_nvmf_fc_hwqp *io_queues,
			uint32_t num_io_queues,
			struct spdk_nvmf_fc_queue_dump_info *dump_info)
{
	uint32_t i = 0;

	/*
	 * Dump the LS queue.
	 */
	spdk_nvmf_fc_dump_buf_print(dump_info, "\nHW Queue type: LS, HW Queue ID:%d", ls_queue->hwqp_id);
	nvmf_fc_dump_hwqp(dump_info, (struct bcm_nvmf_hw_queues *)ls_queue->queues);

	/*
	 * Dump the IO queues.
	 */
	for (i = 0; i < num_io_queues; i++) {
		spdk_nvmf_fc_dump_buf_print(dump_info, "\nHW Queue type: IO, HW Queue ID:%d",
					   io_queues[i].hwqp_id);
		nvmf_fc_dump_hwqp(dump_info, (struct bcm_nvmf_hw_queues *)io_queues[i].queues);
	}
}

static void
nvmf_fc_get_xri_info(struct spdk_nvmf_fc_hwqp *hwqp, struct spdk_nvmf_fc_xchg_info *info)
{
	struct bcm_nvmf_hw_queues *hwq = BCM_HWQP(hwqp);

	info->xchg_base = hwq->xri_list->xri_base;
	info->xchg_total_count = hwq->xri_list->xri_count;
	info->xchg_avail_count = spdk_ring_count(hwq->xri_list->xri_ring);
}

static int
nvmf_fc_put_xchg(struct spdk_nvmf_fc_hwqp *hwqp, struct spdk_nvmf_fc_xchg *xri)
{
	/* If exchange is busy, first cleanup the xri with hardware. */
	if (xri->active) {
		nvmf_fc_cleanup_xri(hwqp, xri, true, xri->send_abts);
	} else if (xri->aborted && xri->send_abts) {
		/*TODO: XRI not activated but need to send ABTS */
	} else {
		nvmf_fc_put_xri(hwqp, xri);
	}
	return 0;
}

static struct spdk_thread*
nvmf_fc_get_rsvd_thread(void)
{
	return ocs_get_rsvd_thread();
}

struct spdk_nvmf_fc_ll_drvr_ops g_ocs_spdk_nvmf_fc_lld_ops = {
	.lld_init = nvmf_fc_lld_init,
	.lld_fini = nvmf_fc_lld_fini,
	.lld_start = nvmf_fc_lld_start,
	.init_q = nvmf_fc_init_q,
	.reinit_q = nvmf_fc_reinit_q,
	.init_q_buffers = nvmf_fc_init_rqpair_buffers,
	.set_q_online_state = nvmf_fc_set_q_online_state,
	.get_xchg = nvmf_fc_get_xri,
	.put_xchg = nvmf_fc_put_xchg,
	.poll_queue = nvmf_fc_process_queue,
	.recv_data = nvmf_fc_recv_data,
	.send_data = nvmf_fc_send_data,
	.q_buffer_release = nvmf_fc_rqpair_buffer_release,
	.xmt_rsp = nvmf_fc_xmt_rsp,
	.xmt_ls_rsp = nvmf_fc_xmt_ls_rsp,
	.issue_abort = nvmf_fc_issue_abort,
	.xmt_bls_rsp = nvmf_fc_xmt_bls_rsp,
	.alloc_srsr_bufs = nvmf_fc_alloc_srsr_bufs,
	.free_srsr_bufs = nvmf_fc_free_srsr_bufs,
	.xmt_srsr_req = nvmf_fc_xmt_srsr_req,
	.q_sync_available = nvmf_fc_q_sync_available,
	.issue_q_sync = nvmf_fc_issue_q_sync,
	.assign_conn_to_hwqp = nvmf_fc_assign_conn_to_hwqp,
	.get_hwqp_from_conn_id = nvmf_fc_get_hwqp_from_conn_id,
	.release_conn = nvmf_fc_release_conn,
	.dump_all_queues = nvmf_fc_dump_all_queues,
	.get_xchg_info = nvmf_fc_get_xri_info,
	.get_rsvd_thread = nvmf_fc_get_rsvd_thread,
};

SPDK_LOG_REGISTER_COMPONENT("nvmf_fc_lld", SPDK_LOG_NVMF_FC_LLD)
