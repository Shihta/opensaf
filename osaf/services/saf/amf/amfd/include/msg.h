/*      -*- OpenSAF  -*-
 *
 * (C) Copyright 2008 The OpenSAF Foundation
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. This file and program are licensed
 * under the GNU Lesser General Public License Version 2.1, February 1999.
 * The complete license can be accessed from the following location:
 * http://opensource.org/licenses/lgpl-license.php
 * See the Copying file included with the OpenSAF distribution for full
 * licensing terms.
 *
 * Author(s): Emerson Network Power
 *
 */

/*****************************************************************************
..............................................................................

..............................................................................

  DESCRIPTION:

  This module is the include file for Availability Director for message 
  processing module.
  
******************************************************************************
*/

/*
 * Module Inclusion Control...
 */
#ifndef AVD_MSG_H
#define AVD_MSG_H

#include <amf_d2nmsg.h>

typedef enum {
	AVD_D2D_CHANGE_ROLE_REQ = AVSV_DND_MSG_MAX,
	AVD_D2D_CHANGE_ROLE_RSP,
	AVD_D2D_MSG_MAX,
} AVD_D2D_MSG_TYPE;

typedef enum avd_rol_chg_cause_type {
        AVD_SWITCH_OVER,
        AVD_FAIL_OVER
} AVD_ROLE_CHG_CAUSE_T;

typedef AVSV_DND_MSG AVD_DND_MSG;
#define AVD_DND_MSG_NULL ((AVD_DND_MSG *)0)
#define AVD_D2D_MSG_NULL ((AVD_D2D_MSG *)0)

/* Message structure used by AVD for communication between
 * the active and standby AVD.
 */
typedef struct avd_d2d_msg {
	AVD_D2D_MSG_TYPE msg_type;
	union {
		struct {
			AVD_ROLE_CHG_CAUSE_T cause;
			SaAmfHAStateT role;
		} d2d_chg_role_req;
		struct {
			SaAmfHAStateT role;
			uint32_t status ; 
		} d2d_chg_role_rsp;
	} msg_info;
} AVD_D2D_MSG;

struct cl_cb_tag;
struct avd_avnd_tag;
class AVD_SU;
struct avd_su_si_rel_tag;
class AVD_COMP;
struct avd_comp_csi_rel_tag;
class AVD_CSI;

uint32_t avd_d2n_msg_dequeue(struct cl_cb_tag *cb);
uint32_t avd_d2n_msg_snd(struct cl_cb_tag *cb, struct avd_avnd_tag *nd_node, AVD_DND_MSG *snd_msg);
uint32_t avd_n2d_msg_rcv(AVD_DND_MSG *rcv_msg, NODE_ID node_id, uint16_t msg_fmt_ver);
uint32_t avd_mds_cpy(MDS_CALLBACK_COPY_INFO *cpy_info);
uint32_t avd_mds_enc(MDS_CALLBACK_ENC_INFO *enc_info);
uint32_t avd_mds_enc_flat(MDS_CALLBACK_ENC_FLAT_INFO *enc_info);
uint32_t avd_mds_dec(MDS_CALLBACK_DEC_INFO *dec_info);
uint32_t avd_mds_dec_flat(MDS_CALLBACK_DEC_FLAT_INFO *dec_info);
uint32_t avd_d2n_msg_bcast(struct cl_cb_tag *cb, AVD_DND_MSG *bcast_msg);

uint32_t avd_snd_node_ack_msg(struct cl_cb_tag *cb, struct avd_avnd_tag *avnd, uint32_t msg_id);
uint32_t avd_snd_node_data_verify_msg(struct cl_cb_tag *cb, struct avd_avnd_tag *avnd);
uint32_t avd_snd_node_info_on_fover_msg(struct cl_cb_tag *cb, struct avd_avnd_tag *avnd);
uint32_t avd_snd_node_update_msg(struct cl_cb_tag *cb, struct avd_avnd_tag *avnd);
uint32_t avd_snd_node_up_msg(struct cl_cb_tag *cb, struct avd_avnd_tag *avnd, uint32_t msg_id_ack);
uint32_t avd_snd_presence_msg(struct cl_cb_tag *cb, AVD_SU *su, bool term_state);
uint32_t avd_snd_oper_state_msg(struct cl_cb_tag *cb, struct avd_avnd_tag *avnd, uint32_t msg_id_ack);
uint32_t avd_snd_op_req_msg(struct cl_cb_tag *cb, struct avd_avnd_tag *avnd, AVSV_PARAM_INFO *param_info);
uint32_t avd_snd_su_msg(struct cl_cb_tag *cb, AVD_SU *su);
uint32_t avd_snd_susi_msg(struct cl_cb_tag *cb, AVD_SU *su, struct avd_su_si_rel_tag *susi,
				AVSV_SUSI_ACT actn, SaBoolT single_csi, struct avd_comp_csi_rel_tag*);
uint32_t avd_snd_set_leds_msg(struct cl_cb_tag *cb, struct avd_avnd_tag *avnd);

uint32_t avd_snd_pg_resp_msg(struct cl_cb_tag *, struct avd_avnd_tag *, AVD_CSI *,
				   AVSV_N2D_PG_TRACK_ACT_MSG_INFO *);
uint32_t avd_snd_pg_upd_msg(struct cl_cb_tag *, struct avd_avnd_tag *, struct avd_comp_csi_rel_tag *,
				  SaAmfProtectionGroupChangesT, SaNameT *);
uint32_t avd_snd_hb_msg(struct cl_cb_tag *);
uint32_t avd_snd_comp_validation_resp(struct cl_cb_tag *cb, struct avd_avnd_tag *avnd,
					    AVD_COMP *comp_ptr, AVD_DND_MSG *n2d_msg);
void avsv_d2d_msg_free(AVD_D2D_MSG *);
void avd_mds_d_enc(MDS_CALLBACK_ENC_INFO *);
void avd_mds_d_dec(MDS_CALLBACK_DEC_INFO *);
uint32_t avd_d2d_msg_snd(struct cl_cb_tag *, AVD_D2D_MSG *);
extern uint32_t avd_d2d_msg_rcv(AVD_D2D_MSG *rcv_msg);

#endif
