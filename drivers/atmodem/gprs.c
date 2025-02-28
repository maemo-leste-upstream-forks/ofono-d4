/*
 * oFono - Open Source Telephony
 * Copyright (C) 2008-2011  Intel Corporation
 * Copyright (C) 2010  ST-Ericsson AB
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <ell/ell.h>
#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs.h>

#include <drivers/atmodem/atutil.h>

#include "gatchat.h"
#include "gatresult.h"

#include "common.h"
#include "vendor.h"

#define MAX_CONTEXTS 255

static const char *cgreg_prefix[] = { "+CGREG:", NULL };
static const char *cgerep_prefix[] = { "+CGEREP:", NULL };
static const char *cgdcont_prefix[] = { "+CGDCONT:", NULL };
static const char *cgact_prefix[] = { "+CGACT:", NULL };
static const char *none_prefix[] = { NULL };

struct gprs_data {
	GAtChat *chat;
	unsigned int vendor;
	int last_auto_context_id;
	gboolean telit_try_reattach;
	int attached;
};

struct list_contexts_data
{
	struct ofono_gprs *gprs;
	void *cb;
	void *data;
	struct l_uintset *active_cids;
	int ref_count;
};

static struct list_contexts_data * list_contexts_data_new(
				struct ofono_gprs *gprs, void *cb, void *data)
{
	struct list_contexts_data *ret;

	ret = g_new0(struct list_contexts_data, 1);
	ret->ref_count = 1;
	ret->gprs = gprs;
	ret->cb = cb;
	ret->data = data;

	return ret;
}

static struct list_contexts_data * list_contexts_data_ref(
						struct list_contexts_data *ld)
{
	ld->ref_count++;
	return ld;
}

static void list_contexts_data_unref(gpointer user_data)
{
	struct list_contexts_data *ld = user_data;

	if (--ld->ref_count)
		return;

	l_uintset_free(ld->active_cids);
	g_free(ld);
}

static void at_cgatt_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_gprs_set_attached(struct ofono_gprs *gprs, int attached,
					ofono_gprs_cb_t cb, void *data)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	snprintf(buf, sizeof(buf), "AT+CGATT=%i", attached ? 1 : 0);

	if (g_at_chat_send(gd->chat, buf, none_prefix,
				at_cgatt_cb, cbd, g_free) > 0) {
		gd->attached = attached;
		return;
	}

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_cgreg_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_status_cb_t cb = cbd->cb;
	struct ofono_error error;
	int status;
	struct gprs_data *gd = cbd->user;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	if (at_util_parse_reg(result, "+CGREG:", NULL, &status,
				NULL, NULL, NULL, gd->vendor) == FALSE) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	cb(&error, status, cbd->data);
}

static void at_gprs_registration_status(struct ofono_gprs *gprs,
					ofono_gprs_status_cb_t cb,
					void *data)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct cb_data *cbd = cb_data_new(cb, data);

	cbd->user = gd;

	switch (gd->vendor) {
	case OFONO_VENDOR_GOBI:
		/*
		 * Send *CNTI=0 to find out the current tech, it will be
		 * intercepted in gobi_cnti_notify in network registration
		 */
		g_at_chat_send(gd->chat, "AT*CNTI=0", none_prefix,
				NULL, NULL, NULL);
		break;
	case OFONO_VENDOR_NOVATEL:
		/*
		 * Send $CNTI=0 to find out the current tech, it will be
		 * intercepted in nw_cnti_notify in network registration
		 */
		g_at_chat_send(gd->chat, "AT$CNTI=0", none_prefix,
				NULL, NULL, NULL);
		break;
	}

	if (g_at_chat_send(gd->chat, "AT+CGREG?", cgreg_prefix,
				at_cgreg_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void at_cgdcont_parse(struct ofono_gprs *gprs, GAtResult *result,
				struct l_uintset *cids)
{
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CGDCONT:")) {
		int read_cid;
		const char *apn = NULL;

		if (!g_at_result_iter_next_number(&iter, &read_cid))
			break;

		if (!l_uintset_contains(cids, read_cid))
			continue;

		/* ignore protocol */
		g_at_result_iter_skip_next(&iter);

		g_at_result_iter_next_string(&iter, &apn);

		if (apn)
			ofono_gprs_cid_activated(gprs, read_cid, apn);
		else
			ofono_warn("cid %d: Activated but no apn present",
					read_cid);
	}
}

static void at_cgdcont_read_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	int activated_cid = gd->last_auto_context_id;
	struct l_uintset *cids;

	DBG("ok %d", ok);

	if (!ok) {
		ofono_warn("Can't read CGDCONT contexts.");
		return;
	}

	if (activated_cid == -1) {
		DBG("Context got deactivated while calling CGDCONT");
		return;
	}

	cids = l_uintset_new_from_range(0, activated_cid);

	l_uintset_put(cids, activated_cid);

	at_cgdcont_parse(gprs, result, cids);

	l_uintset_free(cids);
}

static void at_cgdcont_act_read_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct list_contexts_data *ld = user_data;
	ofono_gprs_cb_t cb = ld->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok)
		ofono_warn("Can't read CGDCONT context.");
	else
		at_cgdcont_parse(ld->gprs, result, ld->active_cids);

	cb(&error, ld->data);
}

static void at_cgact_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct list_contexts_data *ld = user_data;
	struct gprs_data *gd = ofono_gprs_get_data(ld->gprs);
	ofono_gprs_cb_t cb = ld->cb;
	struct ofono_error error;
	GAtResultIter iter;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		ofono_warn("Can't read CGACT contexts.");

		cb(&error, ld->data);

		return;
	}

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CGACT:")) {
		int read_cid = -1;
		int read_status = -1;

		if (!g_at_result_iter_next_number(&iter, &read_cid))
			break;

		if (!g_at_result_iter_next_number(&iter, &read_status))
			break;

		if (read_status != 1)
			continue;

		/* Flag this as auto context as it was obviously active */
		if (gd->last_auto_context_id == -1)
			gd->last_auto_context_id = read_cid;

		if (!ld->active_cids)
			ld->active_cids = l_uintset_new(MAX_CONTEXTS);

		l_uintset_put(ld->active_cids, read_cid);
	}

	if (ld->active_cids != NULL) {
		if (g_at_chat_send(gd->chat, "AT+CGDCONT?", cgdcont_prefix,
					at_cgdcont_act_read_cb, ld,
					list_contexts_data_unref)) {
			list_contexts_data_ref(ld);
			return;
		}

		CALLBACK_WITH_FAILURE(cb, ld->data);
	} else {
		/* No active contexts found */
		cb(&error, ld->data);
	}
}

static void at_gprs_list_active_contexts(struct ofono_gprs *gprs,
						ofono_gprs_cb_t cb, void *data)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct list_contexts_data *ld = list_contexts_data_new(gprs, cb, data);

	if (g_at_chat_send(gd->chat, "AT+CGACT?", cgact_prefix,
				at_cgact_cb, ld, list_contexts_data_unref))
		return;

	list_contexts_data_unref(ld);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void cgreg_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	int status;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);

	if (at_util_parse_reg_unsolicited(result, "+CGREG:", &status,
				NULL, NULL, NULL, gd->vendor) == FALSE)
		return;

	/*
	 * Telit AT modem firmware (tested with UE910-EUR) generates
	 * +CGREG: 0\r\n\r\n+CGEV: NW DETACH
	 * after a context is de-activated and ppp connection closed.
	 * Then, after a random amount of time (observed from a few seconds
	 * to a few hours), an unsolicited +CGREG: 1 arrives.
	 * Attempt to fix the problem, by sending AT+CGATT=1 once.
	 * This does not re-activate the context, but if a network connection
	 * is still correct, will generate an immediate +CGREG: 1.
	 */
	if (gd->vendor == OFONO_VENDOR_TELIT) {
		if (gd->attached && !status && !gd->telit_try_reattach) {
			DBG("Trying to re-attach gprs network");
			gd->telit_try_reattach = TRUE;
			g_at_chat_send(gd->chat, "AT+CGATT=1", none_prefix,
					NULL, NULL, NULL);
			return;
		}

		gd->telit_try_reattach = FALSE;
	}

	ofono_gprs_status_notify(gprs, status);
}

static void cgev_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	GAtResultIter iter;
	const char *event;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CGEV:"))
		return;

	if (!g_at_result_iter_next_unquoted_string(&iter, &event))
		return;

	if (g_str_equal(event, "NW DETACH") ||
			g_str_equal(event, "ME DETACH")) {
		if (gd->vendor == OFONO_VENDOR_TELIT &&
				gd->telit_try_reattach)
			return;

		gd->attached = FALSE;
		ofono_gprs_detached_notify(gprs);
		return;
	} else if (g_str_has_prefix(event, "ME PDN ACT")) {
		sscanf(event, "%*s %*s %*s %u", &gd->last_auto_context_id);

		g_at_chat_send(gd->chat, "AT+CGDCONT?", cgdcont_prefix,
				at_cgdcont_read_cb, gprs, NULL);
	} else if (g_str_has_prefix(event, "ME PDN DEACT")) {
		int context_id;
		sscanf(event, "%*s %*s %*s %u", &context_id);
		/* Indicate that this cid is not activated anymore */
		if (gd->last_auto_context_id == context_id)
			gd->last_auto_context_id = -1;
	}
}

static void xdatastat_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	GAtResultIter iter;
	int stat;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+XDATASTAT:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &stat))

	DBG("stat %d", stat);

	switch (stat) {
	case 0:
		ofono_gprs_suspend_notify(gprs, GPRS_SUSPENDED_UNKNOWN_CAUSE);
		break;
	case 1:
		ofono_gprs_resume_notify(gprs);
		break;
	}
}

static void huawei_mode_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	GAtResultIter iter;
	int mode, submode;
	gint bearer;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^MODE:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &mode))
		return;

	if (!g_at_result_iter_next_number(&iter, &submode))
		return;

	switch (submode) {
	case 1:
	case 2:
		bearer = 1;	/* GPRS */
		break;
	case 3:
		bearer = 2;	/* EDGE */
		break;
	case 4:
		bearer = 3;	/* UMTS */
		break;
	case 5:
		bearer = 5;	/* HSDPA */
		break;
	case 6:
		bearer = 4;	/* HSUPA */
		break;
	case 7:
	case 9:
		bearer = 6;	/* HSUPA + HSDPA */
		break;
	default:
		bearer = 0;
		break;
	}

	ofono_gprs_bearer_notify(gprs, bearer);
}

static void huawei_hcsq_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	GAtResultIter iter;
	const char *mode;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^HCSQ:"))
		return;

	if (!g_at_result_iter_next_string(&iter, &mode))
		return;

	if (!strcmp("LTE", mode))
		ofono_gprs_bearer_notify(gprs, 7); /* LTE */

	/* in other modes, notification ^MODE is used */
}

static void telit_mode_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	GAtResultIter iter;
	gint nt, bearer;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "#PSNT:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &nt))
		return;

	switch (nt) {
	case 0:
		bearer = 1;    /* GPRS */
		break;
	case 1:
		bearer = 2;    /* EDGE */
		break;
	case 2:
		bearer = 3;    /* UMTS */
		break;
	case 3:
		bearer = 5;    /* HSDPA */
		break;
	case 4:
		bearer = 7;    /* LTE */
		break;
	default:
		bearer = 0;
		break;
	}

	ofono_gprs_bearer_notify(gprs, bearer);
}

static void simcom_mode_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	GAtResultIter iter;
	gint stat, bearer;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CNSMOD:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &stat))
		return;

	switch (stat) {
	case 0:
		bearer = PACKET_BEARER_NONE;
		break;
	case 1:
	case 2:
		bearer = PACKET_BEARER_GPRS;
		break;
	case 3:
		bearer = PACKET_BEARER_EGPRS;
		break;
	case 4:
		bearer = PACKET_BEARER_UMTS;
		break;
	case 5:
		bearer = PACKET_BEARER_HSDPA;
		break;
	case 6:
		bearer = PACKET_BEARER_HSUPA;
		break;
	case 7:
		bearer = PACKET_BEARER_HSUPA_HSDPA;
		break;
	case 8:
		bearer = PACKET_BEARER_EPS;
		break;
	default:
		bearer = PACKET_BEARER_NONE;
		break;
	}

	ofono_gprs_bearer_notify(gprs, bearer);
}

static void ublox_ureg_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	GAtResultIter iter;
	gint state, bearer;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+UREG:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &state))
		return;

	switch (state) {
	case 4:
		bearer = 5;
		break;
	case 5:
		bearer = 4;
		break;
	case 8:
		bearer = 1;
		break;
	case 9:
		bearer = 2;
		break;
	default:
		bearer = state;
	}

	ofono_gprs_bearer_notify(gprs, bearer);
}

static void cpsb_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	GAtResultIter iter;
	gint bearer;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CPSB:"))
		return;

	if (!g_at_result_iter_next_number(&iter, NULL))
		return;

	if (!g_at_result_iter_next_number(&iter, &bearer))
		return;

	ofono_gprs_bearer_notify(gprs, bearer);
}

static void gprs_initialized(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);

	g_at_chat_register(gd->chat, "+CGEV:", cgev_notify, FALSE, gprs, NULL);
	g_at_chat_register(gd->chat, "+CGREG:", cgreg_notify,
						FALSE, gprs, NULL);

	switch (gd->vendor) {
	case OFONO_VENDOR_HUAWEI:
		g_at_chat_register(gd->chat, "^MODE:", huawei_mode_notify,
						FALSE, gprs, NULL);
		g_at_chat_register(gd->chat, "^HCSQ:", huawei_hcsq_notify,
						FALSE, gprs, NULL);
		break;
	case OFONO_VENDOR_UBLOX:
		g_at_chat_register(gd->chat, "+UREG:", ublox_ureg_notify,
						FALSE, gprs, NULL);
		g_at_chat_send(gd->chat, "AT+UREG=1", none_prefix,
						NULL, NULL, NULL);
		break;
	case OFONO_VENDOR_TELIT:
		g_at_chat_register(gd->chat, "#PSNT:", telit_mode_notify,
						FALSE, gprs, NULL);
		g_at_chat_send(gd->chat, "AT#PSNT=1", none_prefix,
						NULL, NULL, NULL);
		break;
	case OFONO_VENDOR_SIMCOM_A76XX:
		g_at_chat_register(gd->chat, "+CNSMOD:", simcom_mode_notify,
						FALSE, gprs, NULL);
		g_at_chat_send(gd->chat, "AT+CNSMOD=1", none_prefix,
						NULL, NULL, NULL);
		break;
	case OFONO_VENDOR_QUECTEL_EC2X:
	case OFONO_VENDOR_QUECTEL_EG91X:
	case OFONO_VENDOR_QUECTEL_SERIAL:
		break;
	default:
		g_at_chat_register(gd->chat, "+CPSB:", cpsb_notify,
						FALSE, gprs, NULL);
		g_at_chat_send(gd->chat, "AT+CPSB=1", none_prefix,
						NULL, NULL, NULL);
		break;
	}

	switch (gd->vendor) {
	case OFONO_VENDOR_IFX:
		/* Register for GPRS suspend notifications */
		g_at_chat_register(gd->chat, "+XDATASTAT:", xdatastat_notify,
						FALSE, gprs, NULL);
		g_at_chat_send(gd->chat, "AT+XDATASTAT=1", none_prefix,
						NULL, NULL, NULL);
		break;
	}

	ofono_gprs_register(gprs);
}

static void at_cgerep_test_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	GAtResultIter iter;
	int min, max, arg1 = 0, arg2 = 0;
	gboolean two_arguments = true;
	char buf[20];

	if (!ok) {
		ofono_error("Error querying AT+CGEREP=? Failing...");
		ofono_gprs_remove(gprs);
		return;
	}

	g_at_result_iter_init(&iter, result);

	g_at_result_iter_next(&iter, "+CGEREP:");

	if (!g_at_result_iter_open_list(&iter)) {
		ofono_error("Malformed reply from AT+CGEREP=? Failing...");
		ofono_gprs_remove(gprs);
		return;
	}

	while (g_at_result_iter_next_range(&iter, &min, &max)) {
		if ((min <= 1) && (max >= 1))
			arg1 = 1;

		if ((min <= 2) && (max >= 2))
			arg1 = 2;
	}

	if (!g_at_result_iter_close_list(&iter))
		goto out;

	if (!g_at_result_iter_open_list(&iter)) {
		two_arguments = false;
		goto out;
	}

	while (g_at_result_iter_next_range(&iter, &min, &max)) {
		if ((min <= 1) && (max >= 1))
			arg2 = 1;
	}

	g_at_result_iter_close_list(&iter);

out:
	if (two_arguments)
		sprintf(buf, "AT+CGEREP=%u,%u", arg1, arg2);
	else
		sprintf(buf, "AT+CGEREP=%u", arg1);

	g_at_chat_send(gd->chat, buf, none_prefix, gprs_initialized, gprs,
		NULL);
}

static void at_cgreg_test_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	gint range[2];
	GAtResultIter iter;
	int cgreg1 = 0;
	int cgreg2 = 0;
	const char *cmd;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

retry:
	if (!g_at_result_iter_next(&iter, "+CGREG:"))
		goto error;

	if (!g_at_result_iter_open_list(&iter))
		goto retry;

	while (g_at_result_iter_next_range(&iter, &range[0], &range[1])) {
		if (1 >= range[0] && 1 <= range[1])
			cgreg1 = 1;
		if (2 >= range[0] && 2 <= range[1])
			cgreg2 = 1;
	}

	g_at_result_iter_close_list(&iter);

	if (cgreg2)
		cmd = "AT+CGREG=2";
	else if (cgreg1)
		cmd = "AT+CGREG=1";
	else
		goto error;

	g_at_chat_send(gd->chat, cmd, none_prefix, NULL, NULL, NULL);

	if (gd->vendor != OFONO_VENDOR_SIMCOM_A76XX)
		g_at_chat_send(gd->chat, "AT+CGAUTO=0", none_prefix,
					NULL, NULL, NULL);

	switch (gd->vendor) {
	case OFONO_VENDOR_MBM:
		/* Ericsson MBM and ST-E modems don't support AT+CGEREP=2,1 */
		g_at_chat_send(gd->chat, "AT+CGEREP=1,0", none_prefix,
			gprs_initialized, gprs, NULL);
		break;
	case OFONO_VENDOR_NOKIA:
		/* Nokia data cards don't support AT+CGEREP=1,0 either */
		g_at_chat_send(gd->chat, "AT+CGEREP=1", none_prefix,
			gprs_initialized, gprs, NULL);
		break;
	default:
		g_at_chat_send(gd->chat, "AT+CGEREP=?", cgerep_prefix,
			at_cgerep_test_cb, gprs, NULL);
		break;
	}

	return;

error:
	ofono_info("GPRS not supported on this device");
	ofono_gprs_remove(gprs);
}

static void at_cgdcont_test_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	GAtResultIter iter;
	int min, max;
	const char *pdp_type;
	gboolean found = FALSE;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	while (!found && g_at_result_iter_next(&iter, "+CGDCONT:")) {
		gboolean in_list = FALSE;

		if (!g_at_result_iter_open_list(&iter))
			continue;

		if (g_at_result_iter_next_range(&iter, &min, &max) == FALSE)
			continue;

		if (!g_at_result_iter_skip_next(&iter))
			continue;

		if (g_at_result_iter_open_list(&iter))
			in_list = TRUE;

		if (!g_at_result_iter_next_string(&iter, &pdp_type))
			continue;

		if (in_list && !g_at_result_iter_close_list(&iter))
			continue;

		/* We look for IP PDPs */
		if (g_str_equal(pdp_type, "IP"))
			found = TRUE;
	}

	if (found == FALSE)
		goto error;

	ofono_gprs_set_cid_range(gprs, min, max);

	g_at_chat_send(gd->chat, "AT+CGREG=?", cgreg_prefix,
			at_cgreg_test_cb, gprs, NULL);

	return;

error:
	ofono_info("GPRS not supported on this device");
	ofono_gprs_remove(gprs);
}

static int at_gprs_probe(struct ofono_gprs *gprs,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct gprs_data *gd;

	gd = g_try_new0(struct gprs_data, 1);
	if (gd == NULL)
		return -ENOMEM;

	gd->chat = g_at_chat_clone(chat);
	gd->vendor = vendor;
	gd->last_auto_context_id = -1;

	ofono_gprs_set_data(gprs, gd);

	g_at_chat_send(gd->chat, "AT+CGDCONT=?", cgdcont_prefix,
			at_cgdcont_test_cb, gprs, NULL);

	return 0;
}

static void at_gprs_remove(struct ofono_gprs *gprs)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);

	ofono_gprs_set_data(gprs, NULL);

	g_at_chat_unref(gd->chat);
	g_free(gd);
}

static const struct ofono_gprs_driver driver = {
	.probe			= at_gprs_probe,
	.remove			= at_gprs_remove,
	.set_attached		= at_gprs_set_attached,
	.attached_status	= at_gprs_registration_status,
	.list_active_contexts	= at_gprs_list_active_contexts,
};

OFONO_ATOM_DRIVER_BUILTIN(gprs, atmodem, &driver)
