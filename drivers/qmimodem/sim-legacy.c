/*
 * oFono - Open Source Telephony
 * Copyright (C) 2011-2012  Intel Corporation
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sim.h>

#include <glib.h>

#include "qmi.h"
#include "dms.h"
#include "util.h"

#include "simutil.h"

struct sim_data {
	struct qmi_service *dms;
	int retries[OFONO_SIM_PASSWORD_INVALID];
};

static void qmi_read_file_info(struct ofono_sim *sim, int fileid,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_file_info_cb_t cb, void *user_data)
{
	unsigned char access[3] = { 0x0f, 0xff, 0xff };

	DBG("file id 0x%04x", fileid);

	switch (fileid) {
	case SIM_EF_ICCID_FILEID:
		CALLBACK_WITH_SUCCESS(cb, 10, 0, 0, access, 1, user_data);
		break;
	default:
		CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL, 0, user_data);
		break;
	}
}

static void get_iccid_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_read_cb_t cb = cbd->cb;
	unsigned char iccid[10];
	int iccid_len, len;
	char *str;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
		return;
	}

	str = qmi_result_get_string(result, QMI_DMS_RESULT_ICCID);
	if (!str) {
		CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
		return;
	}

	len = strlen(str);
	if (len > 20) {
		l_free(str);
		CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
		return;
	}

	sim_encode_bcd_number(str, iccid);
	iccid_len = len / 2;
	l_free(str);

	CALLBACK_WITH_SUCCESS(cb, iccid, iccid_len, cbd->data);
}

static void qmi_read_file_transparent(struct ofono_sim *sim,
					int fileid, int start, int length,
					const unsigned char *path,
					unsigned int path_len,
					ofono_sim_read_cb_t cb, void *user_data)
{
	struct sim_data *data = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("file id 0x%04x", fileid);

	switch (fileid) {
	case SIM_EF_ICCID_FILEID:
		if (qmi_service_send(data->dms, QMI_DMS_GET_ICCID, NULL,
						get_iccid_cb, cbd, l_free) > 0)
			return;
		break;
	}

	CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);

	l_free(cbd);
}

static void get_imsi_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_imsi_cb_t cb = cbd->cb;
	char *str;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	str = qmi_result_get_string(result, QMI_DMS_RESULT_IMSI);
	if (!str) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, str, cbd->data);

	l_free(str);
}

static void qmi_read_imsi(struct ofono_sim *sim,
				ofono_sim_imsi_cb_t cb, void *user_data)
{
	struct sim_data *data = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("");

	if (qmi_service_send(data->dms, QMI_DMS_GET_IMSI, NULL,
					get_imsi_cb, cbd, l_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);

	l_free(cbd);
}

static void get_pin_status_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_passwd_cb_t cb = cbd->cb;
	struct sim_data *data = cbd->user;
	const struct qmi_dms_pin_status *pin;
	uint16_t len;
	int pin_type;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	pin = qmi_result_get(result, QMI_DMS_RESULT_PIN1_STATUS, &len);
	if (!pin) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	DBG("PIN 1 status %d", pin->status);

	switch (pin->status) {
	case QMI_DMS_PIN_ENABLED_UNVERIFIED:
		pin_type = OFONO_SIM_PASSWORD_SIM_PIN;
		break;
	case QMI_DMS_PIN_ENABLED_VERIFIED:
	case QMI_DMS_PIN_DISABLED:
		pin_type = OFONO_SIM_PASSWORD_NONE;
		break;
	default:
		pin_type = OFONO_SIM_PASSWORD_INVALID;
		break;
	}

	data->retries[OFONO_SIM_PASSWORD_SIM_PIN] = pin->verify_retries;
	data->retries[OFONO_SIM_PASSWORD_SIM_PUK] = pin->unblock_retries;

	pin = qmi_result_get(result, QMI_DMS_RESULT_PIN2_STATUS, &len);
	if (!pin)
		goto done;

	DBG("PIN 2 status %d", pin->status);

	data->retries[OFONO_SIM_PASSWORD_SIM_PIN2] = pin->verify_retries;
	data->retries[OFONO_SIM_PASSWORD_SIM_PUK2] = pin->unblock_retries;

done:
	CALLBACK_WITH_SUCCESS(cb, pin_type, cbd->data);
}

static void qmi_query_passwd_state(struct ofono_sim *sim,
				ofono_sim_passwd_cb_t cb, void *user_data)
{
	struct sim_data *data = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("");

	cbd->user = data;

	if (qmi_service_send(data->dms, QMI_DMS_GET_PIN_STATUS, NULL,
					get_pin_status_cb, cbd, l_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);

	l_free(cbd);
}

static void qmi_query_pin_retries(struct ofono_sim *sim,
				ofono_sim_pin_retries_cb_t cb, void *user_data)
{
	struct sim_data *data = ofono_sim_get_data(sim);

	DBG("");

	CALLBACK_WITH_SUCCESS(cb, data->retries, user_data);
}

static void process_uim_state(struct ofono_sim *sim, uint8_t state)
{
	DBG("UIM state %d", state);

	switch (state) {
	case QMI_DMS_UIM_STATE_INIT_COMPLETE:
		ofono_sim_inserted_notify(sim, true);
		ofono_sim_initialized_notify(sim);
		break;
	case QMI_DMS_UIM_STATE_INIT_FAILED:
	case QMI_DMS_UIM_STATE_NOT_PRESENT:
	case QMI_DMS_UIM_STATE_INVALID:
		ofono_sim_inserted_notify(sim, false);
		break;
	}
}

static void event_notify(struct qmi_result *result, void *user_data)
{
	struct ofono_sim *sim = user_data;
	uint8_t state;

	DBG("");

	if (qmi_result_get_uint8(result, QMI_DMS_NOTIFY_UIM_STATE, &state))
		process_uim_state(sim, state);
}

static void get_uim_state(struct qmi_result *result, void *user_data)
{
	struct ofono_sim *sim = user_data;
	uint8_t state;

	DBG("");

	if (qmi_result_set_error(result, NULL))
		goto done;

	if (qmi_result_get_uint8(result, QMI_DMS_RESULT_UIM_STATE, &state))
		process_uim_state(sim, state);

done:
	ofono_sim_register(sim);
}

static void set_event_cb(struct qmi_result *result, void *user_data)
{
	struct ofono_sim *sim = user_data;
	struct sim_data *data = ofono_sim_get_data(sim);

	DBG("");

	if (qmi_result_set_error(result, NULL))
		goto done;

	if (qmi_service_send(data->dms, QMI_DMS_GET_UIM_STATE, NULL,
					get_uim_state, sim, NULL) > 0)
		return;

done:
	ofono_sim_register(sim);
}

static int qmi_sim_probe(struct ofono_sim *sim,
				unsigned int vendor, void *user_data)
{
	struct qmi_service *dms = user_data;
	struct qmi_param *param;
	struct sim_data *data;
	int i;

	DBG("");

	param = qmi_param_new();
	qmi_param_append_uint8(param, QMI_DMS_PARAM_REPORT_PIN_STATUS, 0x01);
	qmi_param_append_uint8(param, QMI_DMS_PARAM_REPORT_OPER_MODE, 0x01);
	qmi_param_append_uint8(param, QMI_DMS_PARAM_REPORT_UIM_STATE, 0x01);

	if (!qmi_service_send(dms, QMI_DMS_SET_EVENT, param,
					set_event_cb, sim, NULL)) {
		qmi_param_free(param);
		qmi_service_free(dms);
		return -EIO;
	}

	data = l_new(struct sim_data, 1);
	data->dms = dms;

	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++)
		data->retries[i] = -1;

	qmi_service_register(dms, QMI_DMS_EVENT, event_notify, sim, NULL);

	ofono_sim_set_data(sim, data);

	return 0;
}

static void qmi_sim_remove(struct ofono_sim *sim)
{
	struct sim_data *data = ofono_sim_get_data(sim);

	DBG("");

	ofono_sim_set_data(sim, NULL);

	qmi_service_free(data->dms);
	l_free(data);
}

static const struct ofono_sim_driver driver = {
	.probe			= qmi_sim_probe,
	.remove			= qmi_sim_remove,
	.read_file_info		= qmi_read_file_info,
	.read_file_transparent	= qmi_read_file_transparent,
	.read_imsi		= qmi_read_imsi,
	.query_passwd_state	= qmi_query_passwd_state,
	.query_pin_retries	= qmi_query_pin_retries,
};

OFONO_ATOM_DRIVER_BUILTIN(sim, qmimodem_legacy, &driver)
