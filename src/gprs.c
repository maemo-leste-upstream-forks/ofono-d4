/*
 * oFono - Open Source Telephony
 * Copyright (C) 2008-2011  Intel Corporation
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>

#include <ell/ell.h>
#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "common.h"
#include "provisiondb.h"
#include "storage.h"
#include "simutil.h"
#include "util.h"

#define GPRS_FLAG_ATTACHING 0x1
#define GPRS_FLAG_RECHECK 0x2
#define GPRS_FLAG_ATTACHED_UPDATE 0x4

#define SETTINGS_STORE "gprs"
#define SETTINGS_GROUP "Settings"
#define MAX_CONTEXT_NAME_LENGTH 127
#define MAX_MESSAGE_PROXY_LENGTH 255
#define MAX_MESSAGE_CENTER_LENGTH 255
#define MAX_CONTEXTS 256
#define SUSPEND_TIMEOUT 8

struct ofono_gprs {
	GSList *contexts;
	ofono_bool_t attached;
	ofono_bool_t driver_attached;
	ofono_bool_t roaming_allowed;
	ofono_bool_t powered;
	ofono_bool_t suspended;
	int status;
	int flags;
	int bearer;
	guint suspend_timeout;
	struct l_uintset *used_pids;
	unsigned int last_context_id;
	struct l_uintset *used_cids;
	int netreg_status;
	struct ofono_netreg *netreg;
	unsigned int netreg_watch;
	unsigned int status_watch;
	GKeyFile *settings;
	char *imsi;
	DBusMessage *pending;
	GSList *context_drivers;
	const struct ofono_gprs_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	unsigned int spn_watch;
};

struct ipv4_settings {
	ofono_bool_t static_ip;
	char *ip;
	char *netmask;
	char *gateway;
	char **dns;
	char *proxy;
};

struct ipv6_settings {
	char *ip;
	unsigned char prefix_len;
	char *gateway;
	char **dns;
};

struct context_settings {
	struct ipv4_settings *ipv4;
	struct ipv6_settings *ipv6;
};

struct ofono_gprs_context {
	struct ofono_gprs *gprs;
	enum ofono_gprs_context_type type;
	ofono_bool_t inuse;
	const struct ofono_gprs_context_driver *driver;
	void *driver_data;
	char *interface;
	struct context_settings *settings;
	struct ofono_atom *atom;
};

struct pri_context {
	ofono_bool_t active;
	enum ofono_gprs_context_type type;
	char name[MAX_CONTEXT_NAME_LENGTH + 1];
	char message_proxy[MAX_MESSAGE_PROXY_LENGTH + 1];
	char message_center[MAX_MESSAGE_CENTER_LENGTH + 1];
	unsigned int id;
	char *path;
	char *key;
	char *proxy_host;
	uint16_t proxy_port;
	DBusMessage *pending;
	struct ofono_gprs_primary_context context;
	struct ofono_gprs_context *context_driver;
	struct ofono_gprs *gprs;
};

static void gprs_attached_update(struct ofono_gprs *gprs);
static void gprs_netreg_update(struct ofono_gprs *gprs);
static void gprs_deactivate_next(struct ofono_gprs *gprs);

const char *packet_bearer_to_string(int bearer)
{
	switch (bearer) {
	case PACKET_BEARER_NONE:
		return "none";
	case PACKET_BEARER_GPRS:
		return "gprs";
	case PACKET_BEARER_EGPRS:
		return "edge";
	case PACKET_BEARER_UMTS:
		return "umts";
	case PACKET_BEARER_HSUPA:
		return "hsupa";
	case PACKET_BEARER_HSDPA:
		return "hsdpa";
	case PACKET_BEARER_HSUPA_HSDPA:
		return "hspa";
	case PACKET_BEARER_EPS:
		return "lte";
	}
	return "";
}

static const char *gprs_context_default_name(enum ofono_gprs_context_type type)
{
	switch (type) {
	case OFONO_GPRS_CONTEXT_TYPE_ANY:
		return NULL;
	case OFONO_GPRS_CONTEXT_TYPE_INTERNET:
		return "Internet";
	case OFONO_GPRS_CONTEXT_TYPE_MMS:
		return "MMS";
	case OFONO_GPRS_CONTEXT_TYPE_WAP:
		return "WAP";
	case OFONO_GPRS_CONTEXT_TYPE_IMS:
		return "IMS";
	case OFONO_GPRS_CONTEXT_TYPE_SUPL:
		return "SUPL";
	case OFONO_GPRS_CONTEXT_TYPE_IA:
		return "Initial Attach";
	}

	return NULL;
}

static const char *gprs_context_type_to_string(
					enum ofono_gprs_context_type type)
{
	switch (type) {
	case OFONO_GPRS_CONTEXT_TYPE_ANY:
		return NULL;
	case OFONO_GPRS_CONTEXT_TYPE_INTERNET:
		return "internet";
	case OFONO_GPRS_CONTEXT_TYPE_MMS:
		return "mms";
	case OFONO_GPRS_CONTEXT_TYPE_WAP:
		return "wap";
	case OFONO_GPRS_CONTEXT_TYPE_IMS:
		return "ims";
	case OFONO_GPRS_CONTEXT_TYPE_SUPL:
		return "supl";
	case OFONO_GPRS_CONTEXT_TYPE_IA:
		return "ia";
	}

	return NULL;
}

static gboolean gprs_context_string_to_type(const char *str,
					enum ofono_gprs_context_type *out)
{
	if (g_str_equal(str, "internet")) {
		*out = OFONO_GPRS_CONTEXT_TYPE_INTERNET;
		return TRUE;
	} else if (g_str_equal(str, "wap")) {
		*out = OFONO_GPRS_CONTEXT_TYPE_WAP;
		return TRUE;
	} else if (g_str_equal(str, "mms")) {
		*out = OFONO_GPRS_CONTEXT_TYPE_MMS;
		return TRUE;
	} else if (g_str_equal(str, "ims")) {
		*out = OFONO_GPRS_CONTEXT_TYPE_IMS;
		return TRUE;
	} else if (g_str_equal(str, "supl")) {
		*out = OFONO_GPRS_CONTEXT_TYPE_SUPL;
		return TRUE;
	}

	return FALSE;
}

static struct ofono_gprs_context *find_avail_gprs_context(
							struct pri_context *ctx)
{
	GSList *l;

	for (l = ctx->gprs->context_drivers; l; l = l->next) {
		struct ofono_gprs_context *gc = l->data;

		if (gc->inuse == TRUE)
			continue;

		if (gc->driver == NULL)
			continue;

		if (gc->driver->activate_primary == NULL ||
				gc->driver->deactivate_primary == NULL)
			continue;

		if (gc->type != OFONO_GPRS_CONTEXT_TYPE_ANY &&
				gc->type != ctx->type)
			continue;

		return gc;
	}

	return NULL;
}

static gboolean assign_context(struct pri_context *ctx, unsigned int use_cid)
{
	struct l_uintset *used_cids = ctx->gprs->used_cids;
	struct ofono_gprs_context *gc;

	if (used_cids == NULL)
		return FALSE;

	if (!use_cid)
		use_cid = l_uintset_find_unused_min(used_cids);

	if (use_cid > l_uintset_get_max(used_cids))
		return FALSE;

	gc = find_avail_gprs_context(ctx);
	if (gc == NULL)
		return FALSE;

	l_uintset_put(used_cids, use_cid);
	ctx->context.cid = use_cid;

	ctx->context_driver = gc;
	ctx->context_driver->inuse = TRUE;

	if (ctx->context.proto == OFONO_GPRS_PROTO_IPV4V6 ||
			ctx->context.proto == OFONO_GPRS_PROTO_IP)
		gc->settings->ipv4 = g_new0(struct ipv4_settings, 1);

	if (ctx->context.proto == OFONO_GPRS_PROTO_IPV4V6 ||
			ctx->context.proto == OFONO_GPRS_PROTO_IPV6)
		gc->settings->ipv6 = g_new0(struct ipv6_settings, 1);

	return TRUE;
}

static void release_context(struct pri_context *ctx)
{
	if (ctx == NULL || ctx->gprs == NULL || ctx->context_driver == NULL)
		return;

	l_uintset_take(ctx->gprs->used_cids, ctx->context.cid);
	ctx->context.cid = 0;
	ctx->context_driver->inuse = FALSE;
	ctx->context_driver = NULL;
	ctx->active = FALSE;
}

static struct pri_context *gprs_context_by_path(struct ofono_gprs *gprs,
						const char *ctx_path)
{
	GSList *l;

	for (l = gprs->contexts; l; l = l->next) {
		struct pri_context *ctx = l->data;

		if (g_str_equal(ctx_path, ctx->path))
			return ctx;
	}

	return NULL;
}

static void context_settings_free(struct context_settings *settings)
{
	if (settings->ipv4) {
		l_free(settings->ipv4->ip);
		l_free(settings->ipv4->netmask);
		l_free(settings->ipv4->gateway);
		l_strv_free(settings->ipv4->dns);
		l_free(settings->ipv4->proxy);

		g_free(settings->ipv4);
		settings->ipv4 = NULL;
	}

	if (settings->ipv6) {
		l_free(settings->ipv6->ip);
		l_free(settings->ipv6->gateway);
		l_strv_free(settings->ipv6->dns);

		g_free(settings->ipv6);
		settings->ipv6 = NULL;
	}
}

static void context_settings_append_ipv4(struct context_settings *settings,
						const char *interface,
						DBusMessageIter *iter)
{
	DBusMessageIter variant;
	DBusMessageIter array;
	char typesig[5];
	char arraysig[6];
	const char *method;

	arraysig[0] = DBUS_TYPE_ARRAY;
	arraysig[1] = typesig[0] = DBUS_DICT_ENTRY_BEGIN_CHAR;
	arraysig[2] = typesig[1] = DBUS_TYPE_STRING;
	arraysig[3] = typesig[2] = DBUS_TYPE_VARIANT;
	arraysig[4] = typesig[3] = DBUS_DICT_ENTRY_END_CHAR;
	arraysig[5] = typesig[4] = '\0';

	dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
						arraysig, &variant);

	dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY,
						typesig, &array);
	if (settings == NULL || settings->ipv4 == NULL)
		goto done;

	ofono_dbus_dict_append(&array, "Interface",
				DBUS_TYPE_STRING, &interface);

	/* If we have a Proxy, no other settings are relevant */
	if (settings->ipv4->proxy) {
		ofono_dbus_dict_append(&array, "Proxy", DBUS_TYPE_STRING,
					&settings->ipv4->proxy);
		goto done;
	}

	if (settings->ipv4->static_ip == TRUE)
		method = "static";
	else
		method = "dhcp";

	ofono_dbus_dict_append(&array, "Method", DBUS_TYPE_STRING, &method);

	if (settings->ipv4->ip)
		ofono_dbus_dict_append(&array, "Address", DBUS_TYPE_STRING,
					&settings->ipv4->ip);

	if (settings->ipv4->netmask)
		ofono_dbus_dict_append(&array, "Netmask", DBUS_TYPE_STRING,
					&settings->ipv4->netmask);

	if (settings->ipv4->gateway)
		ofono_dbus_dict_append(&array, "Gateway", DBUS_TYPE_STRING,
					&settings->ipv4->gateway);

	if (settings->ipv4->dns)
		ofono_dbus_dict_append_array(&array, "DomainNameServers",
						DBUS_TYPE_STRING,
						&settings->ipv4->dns);

done:
	dbus_message_iter_close_container(&variant, &array);

	dbus_message_iter_close_container(iter, &variant);
}

static void context_settings_append_ipv4_dict(struct context_settings *settings,
						const char *interface,
						DBusMessageIter *dict)
{
	DBusMessageIter entry;
	const char *key = "Settings";

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY,
						NULL, &entry);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);

	context_settings_append_ipv4(settings, interface, &entry);

	dbus_message_iter_close_container(dict, &entry);
}

static void context_settings_append_ipv6(struct context_settings *settings,
						const char *interface,
						DBusMessageIter *iter)
{
	DBusMessageIter variant;
	DBusMessageIter array;
	char typesig[5];
	char arraysig[6];

	arraysig[0] = DBUS_TYPE_ARRAY;
	arraysig[1] = typesig[0] = DBUS_DICT_ENTRY_BEGIN_CHAR;
	arraysig[2] = typesig[1] = DBUS_TYPE_STRING;
	arraysig[3] = typesig[2] = DBUS_TYPE_VARIANT;
	arraysig[4] = typesig[3] = DBUS_DICT_ENTRY_END_CHAR;
	arraysig[5] = typesig[4] = '\0';

	dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
						arraysig, &variant);

	dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY,
						typesig, &array);
	if (settings == NULL || settings->ipv6 == NULL)
		goto done;

	ofono_dbus_dict_append(&array, "Interface",
				DBUS_TYPE_STRING, &interface);

	if (settings->ipv6->ip)
		ofono_dbus_dict_append(&array, "Address", DBUS_TYPE_STRING,
					&settings->ipv6->ip);

	if (settings->ipv6->prefix_len)
		ofono_dbus_dict_append(&array, "PrefixLength", DBUS_TYPE_BYTE,
					&settings->ipv6->prefix_len);

	if (settings->ipv6->gateway)
		ofono_dbus_dict_append(&array, "Gateway", DBUS_TYPE_STRING,
					&settings->ipv6->gateway);

	if (settings->ipv6->dns)
		ofono_dbus_dict_append_array(&array, "DomainNameServers",
						DBUS_TYPE_STRING,
						&settings->ipv6->dns);

done:
	dbus_message_iter_close_container(&variant, &array);

	dbus_message_iter_close_container(iter, &variant);
}

static void context_settings_append_ipv6_dict(struct context_settings *settings,
						const char *interface,
						DBusMessageIter *dict)
{
	DBusMessageIter entry;
	const char *key = "IPv6.Settings";

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY,
						NULL, &entry);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);

	context_settings_append_ipv6(settings, interface, &entry);

	dbus_message_iter_close_container(dict, &entry);
}

static void signal_settings(struct pri_context *ctx, const char *prop,
		void (*append)(struct context_settings *,
					const char *, DBusMessageIter *))

{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = ctx->path;
	DBusMessage *signal;
	DBusMessageIter iter;
	struct context_settings *settings;
	const char *interface;

	signal = dbus_message_new_signal(path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"PropertyChanged");

	if (signal == NULL)
		return;

	dbus_message_iter_init_append(signal, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &prop);

	if (ctx->context_driver) {
		settings = ctx->context_driver->settings;
		interface = ctx->context_driver->interface;
	} else {
		settings = NULL;
		interface = NULL;
	}

	append(settings, interface, &iter);
	g_dbus_send_message(conn, signal);
}

static void pri_context_signal_settings(struct pri_context *ctx,
					gboolean ipv4, gboolean ipv6)
{
	if (ipv4)
		signal_settings(ctx, "Settings",
				context_settings_append_ipv4);

	if (ipv6)
		signal_settings(ctx, "IPv6.Settings",
				context_settings_append_ipv6);
}

static void pri_parse_proxy(struct pri_context *ctx, const char *proxy)
{
	char *scheme, *host, *port, *path;

	scheme = l_strdup(proxy);
	if (scheme == NULL)
		return;

	host = strstr(scheme, "://");
	if (host != NULL) {
		*host = '\0';
		host += 3;

		if (strcasecmp(scheme, "https") == 0)
			ctx->proxy_port = 443;
		else if (strcasecmp(scheme, "http") == 0)
			ctx->proxy_port = 80;
		else {
			l_free(scheme);
			return;
		}
	} else {
		host = scheme;
		ctx->proxy_port = 80;
	}

	path = strchr(host, '/');
	if (path != NULL)
		*(path++) = '\0';

	port = strrchr(host, ':');
	if (port != NULL) {
		char *end;
		int tmp = strtol(port + 1, &end, 10);

		if (*end == '\0') {
			*port = '\0';
			ctx->proxy_port = tmp;
		}
	}

	l_free(ctx->proxy_host);
	ctx->proxy_host = l_strdup(host);

	l_free(scheme);
}

static void pri_ifupdown(const char *interface, ofono_bool_t active)
{
	struct ifreq ifr;
	int sk;

	if (interface == NULL)
		return;

	sk = socket(PF_INET, SOCK_DGRAM, 0);
	if (sk < 0)
		return;

	memset(&ifr, 0, sizeof(ifr));
	l_strlcpy(ifr.ifr_name, interface, IFNAMSIZ);

	if (ioctl(sk, SIOCGIFFLAGS, &ifr) < 0)
		goto done;

	if (active == TRUE) {
		if (ifr.ifr_flags & IFF_UP)
			goto done;
		ifr.ifr_flags |= IFF_UP;
	} else {
		if (!(ifr.ifr_flags & IFF_UP))
			goto done;
		ifr.ifr_flags &= ~IFF_UP;
	}

	if (ioctl(sk, SIOCSIFFLAGS, &ifr) < 0)
		ofono_error("Failed to change interface flags");

done:
	close(sk);
}

static void pri_set_ipv4_addr(const char *interface, const char *address)
{
	struct ifreq ifr;
	struct sockaddr_in addr;
	int sk;

	if (interface == NULL)
		return;

	sk = socket(PF_INET, SOCK_DGRAM, 0);
	if (sk < 0)
		return;

	memset(&ifr, 0, sizeof(ifr));
	l_strlcpy(ifr.ifr_name, interface, IFNAMSIZ);

	if (ioctl(sk, SIOCGIFFLAGS, &ifr) < 0)
		goto done;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = address ? inet_addr(address) : INADDR_ANY;
	memcpy(&ifr.ifr_addr, &addr, sizeof(ifr.ifr_addr));

	if (ioctl(sk, SIOCSIFADDR, &ifr) < 0) {
		ofono_error("Failed to set interface address");
		goto done;
	}

	if (address == NULL)
		goto done;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr("255.255.255.255");
	memcpy(&ifr.ifr_netmask, &addr, sizeof(ifr.ifr_netmask));

	if (ioctl(sk, SIOCSIFNETMASK, &ifr) < 0)
		ofono_error("Failed to set interface netmask");

done:
	close(sk);
}

static void pri_setproxy(const char *interface, const char *proxy)
{
	struct rtentry rt;
	struct sockaddr_in addr;
	int sk;

	if (interface == NULL)
		return;

	sk = socket(PF_INET, SOCK_DGRAM, 0);
	if (sk < 0)
		return;

	memset(&rt, 0, sizeof(rt));
	rt.rt_flags = RTF_UP | RTF_HOST;
	rt.rt_dev = (char *) interface;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(proxy);
	memcpy(&rt.rt_dst, &addr, sizeof(rt.rt_dst));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	memcpy(&rt.rt_gateway, &addr, sizeof(rt.rt_gateway));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	memcpy(&rt.rt_genmask, &addr, sizeof(rt.rt_genmask));

	if (ioctl(sk, SIOCADDRT, &rt) < 0)
		ofono_error("Failed to add proxy host route");

	close(sk);
}

static void pri_reset_context_settings(struct pri_context *ctx)
{
	struct context_settings *settings;
	const char *interface;
	gboolean signal_ipv4;
	gboolean signal_ipv6;

	if (ctx->context_driver == NULL)
		return;

	interface = ctx->context_driver->interface;
	settings = ctx->context_driver->settings;

	signal_ipv4 = settings->ipv4 != NULL;
	signal_ipv6 = settings->ipv6 != NULL;

	context_settings_free(settings);

	pri_context_signal_settings(ctx, signal_ipv4, signal_ipv6);

	if (ctx->type == OFONO_GPRS_CONTEXT_TYPE_MMS) {
		pri_set_ipv4_addr(interface, NULL);

		l_free(ctx->proxy_host);
		ctx->proxy_host = NULL;
		ctx->proxy_port = 0;
	}

	pri_ifupdown(interface, FALSE);
}

static void pri_update_mms_context_settings(struct pri_context *ctx)
{
	struct ofono_gprs_context *gc = ctx->context_driver;
	struct context_settings *settings = gc->settings;

	settings->ipv4->proxy = l_strdup(ctx->message_proxy);
	pri_parse_proxy(ctx, ctx->message_proxy);

	DBG("proxy %s port %u", ctx->proxy_host, ctx->proxy_port);

	pri_set_ipv4_addr(gc->interface, settings->ipv4->ip);

	if (ctx->proxy_host)
		pri_setproxy(gc->interface, ctx->proxy_host);
}

static void append_context_properties(struct pri_context *ctx,
					DBusMessageIter *dict)
{
	const char *type = gprs_context_type_to_string(ctx->type);
	const char *proto = gprs_proto_to_string(ctx->context.proto);
	const char *name = ctx->name;
	dbus_bool_t value;
	const char *strvalue;
	struct context_settings *settings;
	const char *interface;

	ofono_dbus_dict_append(dict, "Name", DBUS_TYPE_STRING, &name);

	value = ctx->active;
	ofono_dbus_dict_append(dict, "Active", DBUS_TYPE_BOOLEAN, &value);

	ofono_dbus_dict_append(dict, "Type", DBUS_TYPE_STRING, &type);

	ofono_dbus_dict_append(dict, "Protocol", DBUS_TYPE_STRING, &proto);

	strvalue = ctx->context.apn;
	ofono_dbus_dict_append(dict, "AccessPointName", DBUS_TYPE_STRING,
				&strvalue);

	strvalue = ctx->context.username;
	ofono_dbus_dict_append(dict, "Username", DBUS_TYPE_STRING,
				&strvalue);

	strvalue = ctx->context.password;
	ofono_dbus_dict_append(dict, "Password", DBUS_TYPE_STRING,
				&strvalue);

	strvalue = gprs_auth_method_to_string(ctx->context.auth_method);
	ofono_dbus_dict_append(dict, "AuthenticationMethod", DBUS_TYPE_STRING,
				&strvalue);

	if (ctx->type == OFONO_GPRS_CONTEXT_TYPE_MMS) {
		strvalue = ctx->message_proxy;
		ofono_dbus_dict_append(dict, "MessageProxy",
					DBUS_TYPE_STRING, &strvalue);

		strvalue = ctx->message_center;
		ofono_dbus_dict_append(dict, "MessageCenter",
					DBUS_TYPE_STRING, &strvalue);
	}

	if (ctx->context_driver) {
		settings = ctx->context_driver->settings;
		interface = ctx->context_driver->interface;
	} else {
		settings = NULL;
		interface = NULL;
	}

	context_settings_append_ipv4_dict(settings, interface, dict);
	context_settings_append_ipv6_dict(settings, interface, dict);
}

static DBusMessage *pri_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct pri_context *ctx = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);
	append_context_properties(ctx, &dict);
	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void pri_activate_callback(const struct ofono_error *error, void *data)
{
	struct pri_context *ctx = data;
	struct ofono_gprs_context *gc = ctx->context_driver;
	DBusConnection *conn = ofono_dbus_get_connection();
	dbus_bool_t value;

	DBG("%p", ctx);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Activating context failed with error: %s",
				telephony_error_to_str(error));
		__ofono_dbus_pending_reply(&ctx->pending,
					__ofono_error_failed(ctx->pending));
		context_settings_free(ctx->context_driver->settings);
		release_context(ctx);
		return;
	}

	ctx->active = TRUE;
	__ofono_dbus_pending_reply(&ctx->pending,
				dbus_message_new_method_return(ctx->pending));

	if (gc->interface != NULL) {
		pri_ifupdown(gc->interface, TRUE);

		if (ctx->type == OFONO_GPRS_CONTEXT_TYPE_MMS &&
				gc->settings->ipv4)
			pri_update_mms_context_settings(ctx);

		pri_context_signal_settings(ctx, gc->settings->ipv4 != NULL,
						gc->settings->ipv6 != NULL);
	}

	value = ctx->active;
	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Active", DBUS_TYPE_BOOLEAN, &value);
}

static void pri_deactivate_callback(const struct ofono_error *error, void *data)
{
	struct pri_context *ctx = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	dbus_bool_t value;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Deactivating context failed with error: %s",
				telephony_error_to_str(error));
		__ofono_dbus_pending_reply(&ctx->pending,
					__ofono_error_failed(ctx->pending));
		return;
	}

	__ofono_dbus_pending_reply(&ctx->pending,
				dbus_message_new_method_return(ctx->pending));

	pri_reset_context_settings(ctx);
	release_context(ctx);

	value = ctx->active;
	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Active", DBUS_TYPE_BOOLEAN, &value);

	/*
	 * If "Attached" property was about to be signalled as TRUE but there
	 * were still active contexts, try again to signal "Attached" property
	 * to registered applications after active contexts have been released.
	 */
	if (ctx->gprs->flags & GPRS_FLAG_ATTACHED_UPDATE) {
		ctx->gprs->flags &= ~GPRS_FLAG_ATTACHED_UPDATE;
		gprs_attached_update(ctx->gprs);
	}
}

static void gprs_set_attached_property(struct ofono_gprs *gprs,
					ofono_bool_t attached)
{
	const char *path;
	DBusConnection *conn = ofono_dbus_get_connection();
	dbus_bool_t value = attached;

	if (gprs->attached == attached)
		return;

	gprs->attached = attached;

	path = __ofono_atom_get_path(gprs->atom);
	ofono_dbus_signal_property_changed(conn, path,
				OFONO_CONNECTION_MANAGER_INTERFACE,
				"Attached", DBUS_TYPE_BOOLEAN, &value);
}

static void pri_read_settings_callback(const struct ofono_error *error,
					void *data)
{
	struct pri_context *pri_ctx = data;
	struct ofono_gprs_context *gc = pri_ctx->context_driver;
	DBusConnection *conn = ofono_dbus_get_connection();
	dbus_bool_t value;

	DBG("%p", pri_ctx);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Reading context settings failed with error: %s",
				telephony_error_to_str(error));
		context_settings_free(pri_ctx->context_driver->settings);
		release_context(pri_ctx);
		return;
	}

	pri_ctx->active = TRUE;

	if (gc->interface != NULL) {
		pri_ifupdown(gc->interface, TRUE);

		pri_context_signal_settings(pri_ctx, gc->settings->ipv4 != NULL,
						gc->settings->ipv6 != NULL);
	}

	value = pri_ctx->active;

	gprs_set_attached_property(pri_ctx->gprs, TRUE);

	ofono_dbus_signal_property_changed(conn, pri_ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Active", DBUS_TYPE_BOOLEAN, &value);
}

static DBusMessage *pri_set_apn(struct pri_context *ctx, DBusConnection *conn,
				DBusMessage *msg, const char *apn)
{
	GKeyFile *settings = ctx->gprs->settings;

	if (g_str_equal(apn, ctx->context.apn))
		return dbus_message_new_method_return(msg);

	if (is_valid_apn(apn) == FALSE)
		return __ofono_error_invalid_format(msg);

	strcpy(ctx->context.apn, apn);

	if (settings) {
		g_key_file_set_string(settings, ctx->key,
					"AccessPointName", apn);
		storage_sync(ctx->gprs->imsi, SETTINGS_STORE, settings);
	}

	if (msg)
		g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"AccessPointName",
					DBUS_TYPE_STRING, &apn);

	return NULL;
}

static DBusMessage *pri_set_username(struct pri_context *ctx,
					DBusConnection *conn, DBusMessage *msg,
					const char *username)
{
	GKeyFile *settings = ctx->gprs->settings;

	if (strlen(username) > OFONO_GPRS_MAX_USERNAME_LENGTH)
		return __ofono_error_invalid_format(msg);

	if (g_str_equal(username, ctx->context.username))
		return dbus_message_new_method_return(msg);

	strcpy(ctx->context.username, username);

	if (settings) {
		g_key_file_set_string(settings, ctx->key,
					"Username", username);
		storage_sync(ctx->gprs->imsi, SETTINGS_STORE, settings);
	}

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Username",
					DBUS_TYPE_STRING, &username);

	return NULL;
}

static DBusMessage *pri_set_password(struct pri_context *ctx,
					DBusConnection *conn, DBusMessage *msg,
					const char *password)
{
	GKeyFile *settings = ctx->gprs->settings;

	if (strlen(password) > OFONO_GPRS_MAX_PASSWORD_LENGTH)
		return __ofono_error_invalid_format(msg);

	if (g_str_equal(password, ctx->context.password))
		return dbus_message_new_method_return(msg);

	strcpy(ctx->context.password, password);

	if (settings) {
		g_key_file_set_string(settings, ctx->key,
					"Password", password);
		storage_sync(ctx->gprs->imsi, SETTINGS_STORE, settings);
	}

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Password",
					DBUS_TYPE_STRING, &password);

	return NULL;
}

static DBusMessage *pri_set_type(struct pri_context *ctx, DBusConnection *conn,
					DBusMessage *msg, const char *type)
{
	GKeyFile *settings = ctx->gprs->settings;
	enum ofono_gprs_context_type context_type;

	if (gprs_context_string_to_type(type, &context_type) == FALSE)
		return __ofono_error_invalid_format(msg);

	if (ctx->type == context_type)
		return dbus_message_new_method_return(msg);

	ctx->type = context_type;

	if (settings) {
		g_key_file_set_string(settings, ctx->key, "Type", type);
		storage_sync(ctx->gprs->imsi, SETTINGS_STORE, settings);
	}

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Type", DBUS_TYPE_STRING, &type);

	return NULL;
}

static DBusMessage *pri_set_proto(struct pri_context *ctx,
					DBusConnection *conn,
					DBusMessage *msg, const char *str)
{
	GKeyFile *settings = ctx->gprs->settings;
	enum ofono_gprs_proto proto;

	if (!gprs_proto_from_string(str, &proto))
		return __ofono_error_invalid_format(msg);

	if (ctx->context.proto == proto)
		return dbus_message_new_method_return(msg);

	ctx->context.proto = proto;

	if (settings) {
		g_key_file_set_string(settings, ctx->key, "Protocol", str);
		storage_sync(ctx->gprs->imsi, SETTINGS_STORE, settings);
	}

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Protocol", DBUS_TYPE_STRING, &str);

	return NULL;
}

static DBusMessage *pri_set_name(struct pri_context *ctx, DBusConnection *conn,
					DBusMessage *msg, const char *name)
{
	GKeyFile *settings = ctx->gprs->settings;

	if (strlen(name) > MAX_CONTEXT_NAME_LENGTH)
		return __ofono_error_invalid_format(msg);

	if (g_str_equal(ctx->name, name))
		return dbus_message_new_method_return(msg);

	strcpy(ctx->name, name);

	if (settings) {
		g_key_file_set_string(settings, ctx->key, "Name", ctx->name);
		storage_sync(ctx->gprs->imsi, SETTINGS_STORE, settings);
	}

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Name", DBUS_TYPE_STRING, &name);

	return NULL;
}

static DBusMessage *pri_set_message_proxy(struct pri_context *ctx,
					DBusConnection *conn,
					DBusMessage *msg, const char *proxy)
{
	GKeyFile *settings = ctx->gprs->settings;

	if (strlen(proxy) > MAX_MESSAGE_PROXY_LENGTH)
		return __ofono_error_invalid_format(msg);

	if (g_str_equal(ctx->message_proxy, proxy))
		return dbus_message_new_method_return(msg);

	strcpy(ctx->message_proxy, proxy);

	if (settings) {
		g_key_file_set_string(settings, ctx->key, "MessageProxy",
							ctx->message_proxy);
		storage_sync(ctx->gprs->imsi, SETTINGS_STORE, settings);
	}

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
				OFONO_CONNECTION_CONTEXT_INTERFACE,
				"MessageProxy", DBUS_TYPE_STRING, &proxy);

	return NULL;
}

static DBusMessage *pri_set_message_center(struct pri_context *ctx,
					DBusConnection *conn,
					DBusMessage *msg, const char *center)
{
	GKeyFile *settings = ctx->gprs->settings;

	if (strlen(center) > MAX_MESSAGE_CENTER_LENGTH)
		return __ofono_error_invalid_format(msg);

	if (g_str_equal(ctx->message_center, center))
		return dbus_message_new_method_return(msg);

	strcpy(ctx->message_center, center);

	if (settings) {
		g_key_file_set_string(settings, ctx->key, "MessageCenter",
							ctx->message_center);
		storage_sync(ctx->gprs->imsi, SETTINGS_STORE, settings);
	}

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
				OFONO_CONNECTION_CONTEXT_INTERFACE,
				"MessageCenter", DBUS_TYPE_STRING, &center);

	return NULL;
}

static DBusMessage *pri_set_auth_method(struct pri_context *ctx,
					DBusConnection *conn,
					DBusMessage *msg, const char *str)
{
	GKeyFile *settings = ctx->gprs->settings;
	enum ofono_gprs_auth_method auth;

	if (!gprs_auth_method_from_string(str, &auth))
		return __ofono_error_invalid_format(msg);

	if (ctx->context.auth_method == auth)
		return dbus_message_new_method_return(msg);

	ctx->context.auth_method = auth;

	if (settings) {
		g_key_file_set_string(settings, ctx->key,
					"AuthenticationMethod", str);
		storage_sync(ctx->gprs->imsi, SETTINGS_STORE, settings);
	}

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"AuthenticationMethod",
					DBUS_TYPE_STRING, &str);

	return NULL;
}

static DBusMessage *pri_set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct pri_context *ctx = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;
	dbus_bool_t value;
	const char *str;

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);

	if (g_str_equal(property, "Active")) {
		struct ofono_gprs_context *gc;

		if (ctx->gprs->pending)
			return __ofono_error_busy(msg);

		if (ctx->pending)
			return __ofono_error_busy(msg);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (ctx->active == (ofono_bool_t) value)
			return dbus_message_new_method_return(msg);

		if (value && !ctx->gprs->attached)
			return __ofono_error_not_attached(msg);

		if (ctx->gprs->flags & GPRS_FLAG_ATTACHING)
			return __ofono_error_attach_in_progress(msg);

		if (value && assign_context(ctx, 0) == FALSE)
			return __ofono_error_not_implemented(msg);

		gc = ctx->context_driver;

		ctx->pending = dbus_message_ref(msg);

		if (value)
			gc->driver->activate_primary(gc, &ctx->context,
						pri_activate_callback, ctx);
		else
			gc->driver->deactivate_primary(gc, ctx->context.cid,
						pri_deactivate_callback, ctx);

		return NULL;
	}

	/* All other properties are read-only when context is active */
	if (ctx->active == TRUE)
		return __ofono_error_in_use(msg);

	if (!strcmp(property, "AccessPointName")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		return pri_set_apn(ctx, conn, msg, str);
	} else if (!strcmp(property, "Type")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		return pri_set_type(ctx, conn, msg, str);
	} else if (!strcmp(property, "Protocol")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		return pri_set_proto(ctx, conn, msg, str);
	} else if (!strcmp(property, "Username")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		return pri_set_username(ctx, conn, msg, str);
	} else if (!strcmp(property, "Password")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		return pri_set_password(ctx, conn, msg, str);
	} else if (!strcmp(property, "Name")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		return pri_set_name(ctx, conn, msg, str);
	} else if (!strcmp(property, "AuthenticationMethod")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		return pri_set_auth_method(ctx, conn, msg, str);
	}

	if (ctx->type != OFONO_GPRS_CONTEXT_TYPE_MMS)
		return __ofono_error_invalid_args(msg);

	if (!strcmp(property, "MessageProxy")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		return pri_set_message_proxy(ctx, conn, msg, str);
	} else if (!strcmp(property, "MessageCenter")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		return pri_set_message_center(ctx, conn, msg, str);
	}

	return __ofono_error_invalid_args(msg);
}

static const GDBusMethodTable context_methods[] = {
	{ GDBUS_METHOD("GetProperties",
			NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
			pri_get_properties) },
	{ GDBUS_ASYNC_METHOD("SetProperty",
			GDBUS_ARGS({ "property", "s" }, { "value", "v" }),
			NULL, pri_set_property) },
	{ }
};

static const GDBusSignalTable context_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};

static struct pri_context *pri_context_create(struct ofono_gprs *gprs,
					const char *name,
					enum ofono_gprs_context_type type)
{
	struct pri_context *context = g_try_new0(struct pri_context, 1);

	if (context == NULL)
		return NULL;

	if (name == NULL) {
		name = gprs_context_default_name(type);
		if (name == NULL) {
			g_free(context);
			return NULL;
		}
	}

	context->gprs = gprs;
	strcpy(context->name, name);
	context->type = type;

	return context;
}

static void pri_context_destroy(gpointer userdata)
{
	struct pri_context *ctx = userdata;

	l_free(ctx->proxy_host);
	l_free(ctx->path);
	g_free(ctx);
}

static gboolean context_dbus_register(struct pri_context *ctx)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	char path[256];
	const char *basepath;

	basepath = __ofono_atom_get_path(ctx->gprs->atom);

	snprintf(path, sizeof(path), "%s/context%u", basepath, ctx->id);

	if (!g_dbus_register_interface(conn, path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					context_methods, context_signals,
					NULL, ctx, pri_context_destroy)) {
		ofono_error("Could not register PrimaryContext %s", path);
		l_uintset_take(ctx->gprs->used_pids, ctx->id);
		pri_context_destroy(ctx);

		return FALSE;
	}

	ctx->path = l_strdup(path);
	ctx->key = ctx->path + strlen(basepath) + 1;

	return TRUE;
}

static gboolean context_dbus_unregister(struct pri_context *ctx)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	char path[256];

	if (ctx->active == TRUE) {
		const char *interface =
			ctx->context_driver->interface;

		if (ctx->type == OFONO_GPRS_CONTEXT_TYPE_MMS)
			pri_set_ipv4_addr(interface, NULL);

		pri_ifupdown(interface, FALSE);
	}

	strcpy(path, ctx->path);
	l_uintset_take(ctx->gprs->used_pids, ctx->id);

	return g_dbus_unregister_interface(conn, path,
					OFONO_CONNECTION_CONTEXT_INTERFACE);
}

static void update_suspended_property(struct ofono_gprs *gprs,
				ofono_bool_t suspended)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(gprs->atom);
	dbus_bool_t value = suspended;

	if (gprs->suspend_timeout) {
		g_source_remove(gprs->suspend_timeout);
		gprs->suspend_timeout = 0;
	}

	if (gprs->suspended == suspended)
		return;

	DBG("%s GPRS service %s", __ofono_atom_get_path(gprs->atom),
		suspended ? "suspended" : "resumed");

	gprs->suspended = suspended;

	if (gprs->attached)
		ofono_dbus_signal_property_changed(conn, path,
					OFONO_CONNECTION_MANAGER_INTERFACE,
					"Suspended", DBUS_TYPE_BOOLEAN, &value);
}

static gboolean suspend_timeout(gpointer data)
{
	struct ofono_gprs *gprs = data;

	gprs->suspend_timeout = 0;
	update_suspended_property(gprs, TRUE);
	return FALSE;
}

void ofono_gprs_suspend_notify(struct ofono_gprs *gprs, int cause)
{
	switch (cause) {
	case GPRS_SUSPENDED_DETACHED:
	case GPRS_SUSPENDED_CALL:
	case GPRS_SUSPENDED_NO_COVERAGE:
		update_suspended_property(gprs, TRUE);
		break;

	case GPRS_SUSPENDED_SIGNALLING:
	case GPRS_SUSPENDED_UNKNOWN_CAUSE:
		if (gprs->suspend_timeout)
			g_source_remove(gprs->suspend_timeout);
		gprs->suspend_timeout = g_timeout_add_seconds(SUSPEND_TIMEOUT,
							suspend_timeout,
							gprs);
		break;
	}
}

void ofono_gprs_resume_notify(struct ofono_gprs *gprs)
{
	update_suspended_property(gprs, FALSE);
}

static gboolean have_active_contexts(struct ofono_gprs *gprs)
{
	GSList *l;
	struct pri_context *ctx;

	for (l = gprs->contexts; l; l = l->next) {
		ctx = l->data;

		if (ctx->active == TRUE)
			return TRUE;
	}

	return FALSE;
}

static gboolean have_detachable_active_contexts(struct ofono_gprs *gprs)
{
	GSList *l;

	for (l = gprs->contexts; l; l = l->next) {
		struct pri_context *ctx;
		struct ofono_gprs_context *gc;

		ctx = l->data;
		gc = ctx->context_driver;

		if (!gc || !gc->driver->detach_shutdown)
			continue;

		if (ctx->active == TRUE)
			return TRUE;
	}

	return FALSE;
}

static bool have_read_settings(struct ofono_gprs *gprs)
{
	GSList *l;

	for (l = gprs->context_drivers; l; l = l->next) {
		struct ofono_gprs_context *gc = l->data;

		if (gc->driver && gc->driver->read_settings)
			return true;
	}

	return false;
}

static void pri_context_signal_active(struct pri_context *ctx)
{
	DBusConnection *conn;
	dbus_bool_t value;

	value = ctx->active;
	conn = ofono_dbus_get_connection();

	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Active", DBUS_TYPE_BOOLEAN, &value);
}

static void detach_active_contexts(struct ofono_gprs *gprs)
{
	GSList *l;
	struct pri_context *ctx;

	for (l = gprs->contexts; l; l = l->next) {
		struct ofono_gprs_context *gc;

		ctx = l->data;

		if (ctx->active == FALSE)
			continue;

		/* This context is already being messed with */
		if (ctx->pending)
			continue;

		gc = ctx->context_driver;

		if (gc->driver->detach_shutdown != NULL)
			gc->driver->detach_shutdown(gc, ctx->context.cid);

		/* Make sure the context is properly cleared */
		pri_reset_context_settings(ctx);
		release_context(ctx);
		pri_context_signal_active(ctx);
	}
}

static gboolean on_lte(struct ofono_gprs *gprs)
{
	if (ofono_netreg_get_technology(gprs->netreg) ==
			ACCESS_TECHNOLOGY_EUTRAN && have_read_settings(gprs))
		return TRUE;

	return FALSE;
}

static void gprs_attached_update(struct ofono_gprs *gprs)
{
	ofono_bool_t attached;
	int status = gprs->status;

	if (on_lte(gprs))
		/*
		 * For LTE we attached status reflects successful context
		 * activation.
		 * Since we in gprs_netreg_update not even try to attach
		 * to GPRS if we are running on LTE, we can on some modems
		 * expect the gprs status to be unknown. That must not
		 * result in detaching...
		 */
		attached = have_active_contexts(gprs);
	else
		attached = gprs->driver_attached && L_IN_SET(status,
			NETWORK_REGISTRATION_STATUS_REGISTERED,
			NETWORK_REGISTRATION_STATUS_REGISTERED_SMS_EUTRAN,
			NETWORK_REGISTRATION_STATUS_ROAMING,
			NETWORK_REGISTRATION_STATUS_ROAMING_SMS_EUTRAN);

	if (attached == gprs->attached)
		return;

	/*
	 * If an active context is found, a PPP session might be still active
	 * at driver level. "Attached" = TRUE property can't be signalled to
	 * the applications registered on GPRS properties.
	 * Active contexts have to be release at driver level.
	 *
	 * Skip that for LTE since the condition to be attached on LTE
	 * is that a context gets activated
	 */
	if (have_detachable_active_contexts(gprs) && !on_lte(gprs)) {
		detach_active_contexts(gprs);

		if (attached == TRUE) {
			/*
			 * Some times the context activates after a detach event
			 * and right before an attach. We close it to avoid
			 * unexpected open contexts.
			 */
			gprs->flags |= GPRS_FLAG_ATTACHED_UPDATE;
			return;
		}
	}

	if (attached == FALSE)
		gprs->bearer = -1;

	gprs_set_attached_property(gprs, attached);
}

static void registration_status_cb(const struct ofono_error *error,
					int status, void *data)
{
	struct ofono_gprs *gprs = data;

	DBG("%s error %d status %d", __ofono_atom_get_path(gprs->atom),
		error->type, status);

	gprs->flags &= ~GPRS_FLAG_ATTACHING;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		ofono_gprs_status_notify(gprs, status);
	else
		gprs_attached_update(gprs);

	if (gprs->flags & GPRS_FLAG_RECHECK) {
		gprs->flags &= ~GPRS_FLAG_RECHECK;
		gprs_netreg_update(gprs);
	}
}

static void gprs_attach_callback(const struct ofono_error *error, void *data)
{
	struct ofono_gprs *gprs = data;

	DBG("%s error = %d", __ofono_atom_get_path(gprs->atom), error->type);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		gprs->driver_attached = !gprs->driver_attached;

	if (gprs->driver->attached_status == NULL) {
		struct ofono_error status_error;

		status_error.type = OFONO_ERROR_TYPE_FAILURE;
		status_error.error = 0;

		registration_status_cb(&status_error, -1, gprs);
		return;
	}

	gprs->driver->attached_status(gprs, registration_status_cb, gprs);
}

static void gprs_netreg_removed(struct ofono_gprs *gprs)
{
	gprs->netreg = NULL;

	gprs->flags &= ~(GPRS_FLAG_RECHECK | GPRS_FLAG_ATTACHING);
	gprs->status_watch = 0;
	gprs->netreg_status = NETWORK_REGISTRATION_STATUS_NOT_REGISTERED;
	gprs->driver_attached = FALSE;

	gprs_attached_update(gprs);
}

static void gprs_netreg_update(struct ofono_gprs *gprs)
{
	ofono_bool_t attach;

	/*
	 * This function can get called by other reasons than netreg
	 * updating its status. So check if we have a valid netreg status yet.
	 * The only reason for not having a valid status is basically during
	 * startup while the netreg atom is fetching the status.
	 */
	if (gprs->netreg_status < 0)
		return;

	attach = (gprs->netreg_status == NETWORK_REGISTRATION_STATUS_REGISTERED) ||
		(gprs->netreg_status == NETWORK_REGISTRATION_STATUS_REGISTERED_SMS_EUTRAN);

	attach = attach || (gprs->roaming_allowed &&
		((gprs->netreg_status == NETWORK_REGISTRATION_STATUS_ROAMING) ||
		(gprs->netreg_status == NETWORK_REGISTRATION_STATUS_ROAMING_SMS_EUTRAN)));

	attach = attach && gprs->powered;

	DBG("attach: %u, driver_attached: %u", attach, gprs->driver_attached);

	if (on_lte(gprs)) {
		/*
		 * For LTE we set attached status only on successful
		 * context activation.
		 *
		 * The context could potentially be registered before the
		 * netreg update is received.
		 */
		gprs_attached_update(gprs);
		return;
	}

	if (gprs->driver_attached == attach)
		return;

	if (gprs->flags & GPRS_FLAG_ATTACHING) {
		gprs->flags |= GPRS_FLAG_RECHECK;
		return;
	}

	gprs->flags |= GPRS_FLAG_ATTACHING;

	gprs->driver_attached = attach;
	gprs->driver->set_attached(gprs, attach, gprs_attach_callback, gprs);
}

static void netreg_status_changed(int status, int lac, int ci, int tech,
					const char *mcc, const char *mnc,
					void *data)
{
	struct ofono_gprs *gprs = data;

	DBG("%d (%s)", status, registration_status_to_string(status));

	gprs->netreg_status = status;

	gprs_netreg_update(gprs);
}

static DBusMessage *gprs_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	dbus_bool_t value;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	value = gprs->attached;
	ofono_dbus_dict_append(&dict, "Attached", DBUS_TYPE_BOOLEAN, &value);

	if (gprs->bearer != -1) {
		const char *bearer = packet_bearer_to_string(gprs->bearer);

		ofono_dbus_dict_append(&dict, "Bearer",
					DBUS_TYPE_STRING, &bearer);
	}

	value = gprs->roaming_allowed;
	ofono_dbus_dict_append(&dict, "RoamingAllowed",
				DBUS_TYPE_BOOLEAN, &value);

	value = gprs->powered;
	ofono_dbus_dict_append(&dict, "Powered", DBUS_TYPE_BOOLEAN, &value);

	if (gprs->attached) {
		value = gprs->suspended;
		ofono_dbus_dict_append(&dict, "Suspended",
				DBUS_TYPE_BOOLEAN, &value);
	}

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *gprs_set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;
	dbus_bool_t value;
	const char *path;

	if (gprs->pending)
		return __ofono_error_busy(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);

	if (!strcmp(property, "RoamingAllowed")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (gprs->roaming_allowed == (ofono_bool_t) value)
			return dbus_message_new_method_return(msg);

		gprs->roaming_allowed = value;

		if (gprs->settings) {
			g_key_file_set_boolean(gprs->settings, SETTINGS_GROUP,
						"RoamingAllowed",
						gprs->roaming_allowed);
			storage_sync(gprs->imsi, SETTINGS_STORE,
					gprs->settings);
		}

		gprs_netreg_update(gprs);
	} else if (!strcmp(property, "Powered")) {
		if (gprs->driver->set_attached == NULL)
			return __ofono_error_not_implemented(msg);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (gprs->powered == (ofono_bool_t) value)
			return dbus_message_new_method_return(msg);

		gprs->powered = value;

		if (gprs->settings) {
			g_key_file_set_integer(gprs->settings, SETTINGS_GROUP,
						"Powered", gprs->powered);
			storage_sync(gprs->imsi, SETTINGS_STORE,
					gprs->settings);
		}

		gprs_netreg_update(gprs);
	} else {
		return __ofono_error_invalid_args(msg);
	}

	path = __ofono_atom_get_path(gprs->atom);
	ofono_dbus_signal_property_changed(conn, path,
					OFONO_CONNECTION_MANAGER_INTERFACE,
					property, DBUS_TYPE_BOOLEAN, &value);

	return dbus_message_new_method_return(msg);
}

static void write_context_settings(struct ofono_gprs *gprs,
					struct pri_context *context)
{
	const char *auth_method;

	g_key_file_set_string(gprs->settings, context->key,
				"Name", context->name);
	g_key_file_set_string(gprs->settings, context->key,
				"AccessPointName", context->context.apn);
	g_key_file_set_string(gprs->settings, context->key,
				"Username", context->context.username);
	g_key_file_set_string(gprs->settings, context->key,
				"Password", context->context.password);

	auth_method = gprs_auth_method_to_string(context->context.auth_method);
	g_key_file_set_string(gprs->settings, context->key,
				"AuthenticationMethod", auth_method);

	g_key_file_set_string(gprs->settings, context->key, "Type",
				gprs_context_type_to_string(context->type));
	g_key_file_set_string(gprs->settings, context->key, "Protocol",
				gprs_proto_to_string(context->context.proto));

	if (context->type == OFONO_GPRS_CONTEXT_TYPE_MMS) {
		g_key_file_set_string(gprs->settings, context->key,
					"MessageProxy",
					context->message_proxy);
		g_key_file_set_string(gprs->settings, context->key,
					"MessageCenter",
					context->message_center);
	}
}

static struct pri_context *find_usable_context(struct ofono_gprs *gprs,
					const char *apn)
{
	GSList *l;
	struct pri_context *pri_ctx;

	/* Look for matching APN: */
	for (l = gprs->contexts; l; l = l->next) {
		pri_ctx = l->data;

		/* Looking only at prefix for the LTE case when a user APN is
		 * web.provider.com but it apepars as
		 * web.provider.com.mncX.mccY.gprs .
		 */
		if (l_str_has_prefix(apn, pri_ctx->context.apn))
			return pri_ctx;
	}

	/* Look for a provision failed pri context: */
	for (l = gprs->contexts; l; l = l->next) {
		pri_ctx = l->data;

		if (pri_ctx->context.apn[0] == '\0')
			return pri_ctx;
	}

	return NULL;
}

static struct pri_context *add_context(struct ofono_gprs *gprs,
					const char *name,
					enum ofono_gprs_context_type type)
{
	unsigned int id;
	struct pri_context *context;

	if (gprs->last_context_id)
		id = l_uintset_find_unused(gprs->used_pids,
							gprs->last_context_id);
	else
		id = l_uintset_find_unused_min(gprs->used_pids);

	if (id > l_uintset_get_max(gprs->used_pids))
		return NULL;

	context = pri_context_create(gprs, name, type);
	if (context == NULL) {
		ofono_error("Unable to allocate context struct");
		return NULL;
	}

	l_uintset_put(gprs->used_pids, id);
	context->id = id;

	DBG("Registering new context");

	if (!context_dbus_register(context)) {
		ofono_error("Unable to register primary context");
		return NULL;
	}

	gprs->last_context_id = id;

	if (gprs->settings) {
		write_context_settings(gprs, context);
		storage_sync(gprs->imsi, SETTINGS_STORE, gprs->settings);
	}

	gprs->contexts = g_slist_append(gprs->contexts, context);

	return context;
}

void ofono_gprs_cid_activated(struct ofono_gprs  *gprs, unsigned int cid,
				const char *apn)
{
	struct pri_context *pri_ctx;
	struct ofono_gprs_context *gc;

	DBG("cid %u", cid);

	if (!__ofono_atom_get_registered(gprs->atom)) {
		DBG("cid %u activated before atom registered", cid);
		return;
	}

	if (l_uintset_contains(gprs->used_cids, cid)) {
		DBG("cid %u already activated", cid);
		return;
	}

	if (strlen(apn) > OFONO_GPRS_MAX_APN_LENGTH
				|| is_valid_apn(apn) == FALSE) {
		ofono_error("Context activated with an invalid APN");
		return;
	}

	pri_ctx = find_usable_context(gprs, apn);

	if (!pri_ctx) {
		pri_ctx = add_context(gprs, apn,
					OFONO_GPRS_CONTEXT_TYPE_INTERNET);
		if (!pri_ctx) {
			ofono_error("Can't find/create automatic context %d "
					"with APN %s.", cid, apn);
			return;
		}
	}

	if (assign_context(pri_ctx, cid) == FALSE) {
		ofono_warn("Can't assign context to driver for APN.");
		return;
	}

	gc = pri_ctx->context_driver;

	if (gc->driver->read_settings == NULL) {
		ofono_warn("Context activated for driver that doesn't support "
				"automatic context activation.");
		release_context(pri_ctx);
		return;
	}

	/*
	 * We weren't able to find a context with a matching APN and allocated
	 * a brand new one instead.  Set the APN accordingly
	 */
	if (strlen(pri_ctx->context.apn) == 0) {
		DBusConnection *conn = ofono_dbus_get_connection();

		strcpy(pri_ctx->context.apn, apn);

		ofono_dbus_signal_property_changed(conn, pri_ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"AccessPointName",
					DBUS_TYPE_STRING, &apn);
	}

	gc->driver->read_settings(gc, cid, pri_read_settings_callback, pri_ctx);
}

static void send_context_added_signal(struct ofono_gprs *gprs,
					struct pri_context *context,
					DBusConnection *conn)
{
	const char *path;
	DBusMessage *signal;
	DBusMessageIter iter;
	DBusMessageIter dict;

	path = __ofono_atom_get_path(gprs->atom);
	signal = dbus_message_new_signal(path,
					OFONO_CONNECTION_MANAGER_INTERFACE,
					"ContextAdded");
	if (!signal)
		return;

	dbus_message_iter_init_append(signal, &iter);

	path = context->path;
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);
	append_context_properties(context, &dict);
	dbus_message_iter_close_container(&iter, &dict);

	g_dbus_send_message(conn, signal);
}

static DBusMessage *gprs_add_context(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;
	struct pri_context *context;
	const char *typestr;
	const char *name;
	const char *path;
	enum ofono_gprs_context_type type;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &typestr,
					DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	if (gprs_context_string_to_type(typestr, &type) == FALSE)
		return __ofono_error_invalid_format(msg);

	name = gprs_context_default_name(type);
	if (name == NULL)
		name = typestr;

	context = add_context(gprs, name, type);
	if (context == NULL)
		return __ofono_error_failed(msg);

	path = context->path;

	g_dbus_send_reply(conn, msg, DBUS_TYPE_OBJECT_PATH, &path,
					DBUS_TYPE_INVALID);

	send_context_added_signal(gprs, context, conn);

	return NULL;
}

static void gprs_deactivate_for_remove(const struct ofono_error *error,
						void *data)
{
	struct pri_context *ctx = data;
	struct ofono_gprs *gprs = ctx->gprs;
	DBusConnection *conn = ofono_dbus_get_connection();
	char *path;
	const char *atompath;
	dbus_bool_t value;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Removing context failed with error: %s",
				telephony_error_to_str(error));

		__ofono_dbus_pending_reply(&gprs->pending,
					__ofono_error_failed(gprs->pending));
		return;
	}

	pri_reset_context_settings(ctx);
	release_context(ctx);

	value = FALSE;
	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Active", DBUS_TYPE_BOOLEAN, &value);

	if (gprs->settings) {
		g_key_file_remove_group(gprs->settings, ctx->key, NULL);
		storage_sync(gprs->imsi, SETTINGS_STORE, gprs->settings);
	}

	/* Make a backup copy of path for signal emission below */
	path = l_strdup(ctx->path);

	context_dbus_unregister(ctx);
	gprs->contexts = g_slist_remove(gprs->contexts, ctx);

	__ofono_dbus_pending_reply(&gprs->pending,
				dbus_message_new_method_return(gprs->pending));

	atompath = __ofono_atom_get_path(gprs->atom);
	g_dbus_emit_signal(conn, atompath, OFONO_CONNECTION_MANAGER_INTERFACE,
				"ContextRemoved", DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID);
	l_free(path);
}

static DBusMessage *gprs_remove_context(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;
	struct pri_context *ctx;
	const char *path;
	const char *atompath;

	if (gprs->pending)
		return __ofono_error_busy(msg);

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
					DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	if (path[0] == '\0')
		return __ofono_error_invalid_format(msg);

	ctx = gprs_context_by_path(gprs, path);
	if (ctx == NULL)
		return __ofono_error_not_found(msg);

	/* This context is already being messed with */
	if (ctx->pending)
		return __ofono_error_busy(msg);

	if (ctx->active) {
		struct ofono_gprs_context *gc = ctx->context_driver;

		gprs->pending = dbus_message_ref(msg);
		gc->driver->deactivate_primary(gc, ctx->context.cid,
					gprs_deactivate_for_remove, ctx);
		return NULL;
	}

	if (gprs->settings) {
		g_key_file_remove_group(gprs->settings, ctx->key, NULL);
		storage_sync(gprs->imsi, SETTINGS_STORE, gprs->settings);
	}

	DBG("Unregistering context: %s", ctx->path);
	context_dbus_unregister(ctx);
	gprs->contexts = g_slist_remove(gprs->contexts, ctx);

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	atompath = __ofono_atom_get_path(gprs->atom);
	g_dbus_emit_signal(conn, atompath, OFONO_CONNECTION_MANAGER_INTERFACE,
				"ContextRemoved", DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID);

	return NULL;
}

static void gprs_deactivate_for_all(const struct ofono_error *error,
					void *data)
{
	struct pri_context *ctx = data;
	struct ofono_gprs *gprs = ctx->gprs;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		__ofono_dbus_pending_reply(&gprs->pending,
					__ofono_error_failed(gprs->pending));
		return;
	}

	pri_reset_context_settings(ctx);
	release_context(ctx);
	pri_context_signal_active(ctx);

	gprs_deactivate_next(gprs);
}

static void gprs_deactivate_next(struct ofono_gprs *gprs)
{
	GSList *l;
	struct pri_context *ctx;
	struct ofono_gprs_context *gc;

	for (l = gprs->contexts; l; l = l->next) {
		ctx = l->data;

		if (ctx->active == FALSE)
			continue;

		gc = ctx->context_driver;
		gc->driver->deactivate_primary(gc, ctx->context.cid,
					gprs_deactivate_for_all, ctx);

		return;
	}

	__ofono_dbus_pending_reply(&gprs->pending,
				dbus_message_new_method_return(gprs->pending));
}

static DBusMessage *gprs_deactivate_all(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;
	GSList *l;
	struct pri_context *ctx;

	if (gprs->pending)
		return __ofono_error_busy(msg);

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	for (l = gprs->contexts; l; l = l->next) {
		ctx = l->data;

		if (ctx->pending)
			return __ofono_error_busy(msg);
	}

	gprs->pending = dbus_message_ref(msg);

	gprs_deactivate_next(gprs);

	return NULL;
}

static DBusMessage *gprs_get_contexts(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter array;
	DBusMessageIter entry, dict;
	const char *path;
	GSList *l;
	struct pri_context *ctx;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					DBUS_STRUCT_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_OBJECT_PATH_AS_STRING
					DBUS_TYPE_ARRAY_AS_STRING
					DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_STRING_AS_STRING
					DBUS_TYPE_VARIANT_AS_STRING
					DBUS_DICT_ENTRY_END_CHAR_AS_STRING
					DBUS_STRUCT_END_CHAR_AS_STRING,
					&array);

	for (l = gprs->contexts; l; l = l->next) {
		ctx = l->data;

		path = ctx->path;

		dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT,
							NULL, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_OBJECT_PATH,
						&path);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

		append_context_properties(ctx, &dict);
		dbus_message_iter_close_container(&entry, &dict);
		dbus_message_iter_close_container(&array, &entry);
	}

	dbus_message_iter_close_container(&iter, &array);

	return reply;
}

static void provision_context(const struct provision_db_entry *ap,
				struct ofono_gprs *gprs)
{
	unsigned int id;
	struct pri_context *context = NULL;
	enum ofono_gprs_context_type type;

	/* Sanity check */
	if (ap == NULL)
		return;

	if (ap->name && strlen(ap->name) > MAX_CONTEXT_NAME_LENGTH)
		return;

	if (is_valid_apn(ap->apn) == FALSE)
		return;

	if (ap->username &&
			strlen(ap->username) > OFONO_GPRS_MAX_USERNAME_LENGTH)
		return;

	if (ap->password &&
			strlen(ap->password) > OFONO_GPRS_MAX_PASSWORD_LENGTH)
		return;

	if (ap->message_proxy &&
			strlen(ap->message_proxy) > MAX_MESSAGE_PROXY_LENGTH)
		return;

	if (ap->message_center &&
			strlen(ap->message_center) > MAX_MESSAGE_CENTER_LENGTH)
		return;

	if (gprs->last_context_id)
		id = l_uintset_find_unused(gprs->used_pids,
							gprs->last_context_id);
	else
		id = l_uintset_find_unused_min(gprs->used_pids);

	if (id > l_uintset_get_max(gprs->used_pids))
		return;

	/*
	 * Right now oFono is not setup to handle contexts with multiple
	 * types due to the way the D-Bus API is structured.  This mainly
	 * affects Internet + MMS contexts.  For now, work around this
	 * by selecting the primary (lowest bit) context type
	 */
	type = 1 << (__builtin_ffs(ap->type) - 1);

	context = pri_context_create(gprs, ap->name, type);
	if (context == NULL)
		return;

	l_uintset_put(gprs->used_pids, id);
	context->id = id;

	if (ap->username != NULL)
		strcpy(context->context.username, ap->username);

	if (ap->password != NULL)
		strcpy(context->context.password, ap->password);

	context->context.auth_method = ap->auth_method;

	strcpy(context->context.apn, ap->apn);
	context->context.proto = ap->proto;

	if (type == OFONO_GPRS_CONTEXT_TYPE_MMS) {
		if (ap->message_proxy != NULL)
			strcpy(context->message_proxy, ap->message_proxy);

		if (ap->message_center != NULL)
			strcpy(context->message_center, ap->message_center);
	}

	if (context_dbus_register(context) == FALSE)
		return;

	gprs->last_context_id = id;

	if (gprs->settings) {
		write_context_settings(gprs, context);
		storage_sync(gprs->imsi, SETTINGS_STORE, gprs->settings);
	}

	gprs->contexts = g_slist_append(gprs->contexts, context);
}

static void provision_contexts(struct ofono_gprs *gprs, const char *mcc,
				const char *mnc, const char *spn)
{
	struct provision_db_entry *settings;
	size_t count;
	size_t i;

	if (!__ofono_provision_get_settings(mcc, mnc, spn, &settings, &count)) {
		ofono_warn("Provisioning failed");
		return;
	}

	for (i = 0; i < count; i++)
		provision_context(&settings[i], gprs);

	l_free(settings);
}

static void remove_non_active_context(struct ofono_gprs *gprs,
				struct pri_context *ctx, DBusConnection *conn)
{
	char *path;
	const char *atompath;

	if (gprs->settings) {
		g_key_file_remove_group(gprs->settings, ctx->key, NULL);
		storage_sync(gprs->imsi, SETTINGS_STORE, gprs->settings);
	}

	/* Make a backup copy of path for signal emission below */
	path = l_strdup(ctx->path);

	context_dbus_unregister(ctx);
	gprs->contexts = g_slist_remove(gprs->contexts, ctx);

	atompath = __ofono_atom_get_path(gprs->atom);
	g_dbus_emit_signal(conn, atompath, OFONO_CONNECTION_MANAGER_INTERFACE,
				"ContextRemoved", DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID);
	l_free(path);
}

static DBusMessage *gprs_reset_contexts(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;
	struct ofono_modem *modem = __ofono_atom_get_modem(gprs->atom);
	struct ofono_sim *sim = __ofono_atom_find(OFONO_ATOM_TYPE_SIM, modem);
	DBusMessage *reply;
	GSList *l;

	if (gprs->pending)
		return __ofono_error_busy(msg);

	/*
	 * We want __ofono_error_busy to take precedence over
	 * __ofono_error_not_allowed errors, so we check it first.
	 */

	for (l = gprs->contexts; l; l = l->next) {
		struct pri_context *ctx = l->data;

		if (ctx->pending)
			return __ofono_error_busy(msg);
	}

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	if (gprs->powered)
		return __ofono_error_not_allowed(msg);

	for (l = gprs->contexts; l; l = l->next) {
		struct pri_context *ctx = l->data;

		if (ctx->active)
			return __ofono_error_not_allowed(msg);
	}

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	/* Remove first the current contexts, re-provision after */

	while (gprs->contexts != NULL) {
		struct pri_context *ctx = gprs->contexts->data;
		remove_non_active_context(gprs, ctx, conn);
	}

	gprs->last_context_id = 0;

	provision_contexts(gprs, ofono_sim_get_mcc(sim),
				ofono_sim_get_mnc(sim), ofono_sim_get_spn(sim));

	if (gprs->contexts == NULL) /* Automatic provisioning failed */
		add_context(gprs, NULL, OFONO_GPRS_CONTEXT_TYPE_INTERNET);

	for (l = gprs->contexts; l; l = l->next) {
		struct pri_context *ctx = l->data;
		send_context_added_signal(gprs, ctx, conn);
	}

	return reply;
}

static const GDBusMethodTable manager_methods[] = {
	{ GDBUS_METHOD("GetProperties",
			NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
			gprs_get_properties) },
	{ GDBUS_METHOD("SetProperty",
			GDBUS_ARGS({ "property", "s" }, { "value", "v" }),
			NULL, gprs_set_property) },
	{ GDBUS_ASYNC_METHOD("AddContext",
			GDBUS_ARGS({ "type", "s" }),
			GDBUS_ARGS({ "path", "o" }),
			gprs_add_context) },
	{ GDBUS_ASYNC_METHOD("RemoveContext",
			GDBUS_ARGS({ "path", "o" }), NULL,
			gprs_remove_context) },
	{ GDBUS_ASYNC_METHOD("DeactivateAll", NULL, NULL,
			gprs_deactivate_all) },
	{ GDBUS_METHOD("GetContexts", NULL,
			GDBUS_ARGS({ "contexts_with_properties", "a(oa{sv})" }),
			gprs_get_contexts) },
	{ GDBUS_ASYNC_METHOD("ResetContexts", NULL, NULL,
			gprs_reset_contexts) },
	{ }
};

static const GDBusSignalTable manager_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ GDBUS_SIGNAL("ContextAdded",
			GDBUS_ARGS({ "path", "o" }, { "properties", "a{sv}" })) },
	{ GDBUS_SIGNAL("ContextRemoved", GDBUS_ARGS({ "path", "o" })) },
	{ }
};

void ofono_gprs_detached_notify(struct ofono_gprs *gprs)
{
	DBG("%s", __ofono_atom_get_path(gprs->atom));

	/*
	 * In case we are attaching let that finish, it will update to the
	 * correct status. If we fiddle with driver_attach and the
	 * attach fails, the code will invert back the state to attached,
	 * which would leave us in an incorrect state.
	 */
	if (gprs->flags & GPRS_FLAG_ATTACHING)
		return;

	gprs->driver_attached = FALSE;
	gprs_attached_update(gprs);

	/*
	 * TODO: The network forced a detach, we should wait for some time
	 * and try to re-attach.  This might also be related to a suspend
	 * event while voicecall is active.
	 */
}

void ofono_gprs_status_notify(struct ofono_gprs *gprs, int status)
{
	DBG("%s status %s (%d)", __ofono_atom_get_path(gprs->atom),
			registration_status_to_string(status), status);

	gprs->status = status;

	/*
	 * If we're already taking action, e.g. attaching or detaching, then
	 * ignore this notification for now, we will take appropriate action
	 * after the set_attach operation has completed
	 */
	if (gprs->flags & GPRS_FLAG_ATTACHING)
		return;

	if (!L_IN_SET(status, NETWORK_REGISTRATION_STATUS_REGISTERED,
				NETWORK_REGISTRATION_STATUS_REGISTERED_SMS_EUTRAN,
				NETWORK_REGISTRATION_STATUS_ROAMING,
				NETWORK_REGISTRATION_STATUS_ROAMING_SMS_EUTRAN)) {
		ofono_gprs_detached_notify(gprs);
		return;
	}

	/* We registered without being powered */
	if (gprs->powered == FALSE)
		goto detach;

	if (gprs->roaming_allowed == FALSE &&
			status == NETWORK_REGISTRATION_STATUS_ROAMING)
		goto detach;

	gprs->driver_attached = TRUE;
	gprs_attached_update(gprs);

	return;

detach:
	gprs->flags |= GPRS_FLAG_ATTACHING;
	gprs->driver->set_attached(gprs, FALSE, gprs_attach_callback, gprs);
}

void ofono_gprs_set_cid_range(struct ofono_gprs *gprs,
				unsigned int min, unsigned int max)
{
	if (gprs == NULL)
		return;

	l_uintset_free(gprs->used_cids);
	gprs->used_cids = l_uintset_new_from_range(min, max);
}

static void gprs_context_unregister(struct ofono_atom *atom)
{
	struct ofono_gprs_context *gc = __ofono_atom_get_data(atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	GSList *l;
	struct pri_context *ctx;
	dbus_bool_t value;

	DBG("%p, %p", gc, gc->gprs);

	if (gc->gprs == NULL)
		goto done;

	for (l = gc->gprs->contexts; l; l = l->next) {
		ctx = l->data;

		if (ctx->context_driver != gc)
			continue;

		if (ctx->pending != NULL)
			__ofono_dbus_pending_reply(&ctx->pending,
					__ofono_error_failed(ctx->pending));

		if (ctx->active == FALSE)
			break;

		pri_reset_context_settings(ctx);
		release_context(ctx);

		value = FALSE;
		ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Active", DBUS_TYPE_BOOLEAN, &value);
	}

	gc->gprs->context_drivers = g_slist_remove(gc->gprs->context_drivers,
							gc);
	gc->gprs = NULL;

done:
	if (gc->settings) {
		context_settings_free(gc->settings);
		g_free(gc->settings);
		gc->settings = NULL;
	}
}

void ofono_gprs_add_context(struct ofono_gprs *gprs,
				struct ofono_gprs_context *gc)
{
	if (gc->driver == NULL)
		return;

	gc->gprs = gprs;
	gc->settings = g_new0(struct context_settings, 1);

	gprs->context_drivers = g_slist_append(gprs->context_drivers, gc);
	__ofono_atom_register(gc->atom, gprs_context_unregister);
}

void ofono_gprs_bearer_notify(struct ofono_gprs *gprs, int bearer)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;
	const char *value;

	if (gprs->bearer == bearer)
		return;

	gprs->bearer = bearer;
	path = __ofono_atom_get_path(gprs->atom);
	value = packet_bearer_to_string(bearer);
	ofono_dbus_signal_property_changed(conn, path,
					OFONO_CONNECTION_MANAGER_INTERFACE,
					"Bearer", DBUS_TYPE_STRING, &value);
}

void ofono_gprs_context_deactivated(struct ofono_gprs_context *gc,
					unsigned int cid)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_gprs *gprs = gc->gprs;
	GSList *l;
	struct pri_context *ctx;
	dbus_bool_t value;

	if (gprs == NULL)
		return;

	for (l = gprs->contexts; l; l = l->next) {
		ctx = l->data;

		if (ctx->context.cid != cid)
			continue;

		if (ctx->active == FALSE)
			break;

		pri_reset_context_settings(ctx);
		release_context(ctx);

		value = FALSE;
		ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Active", DBUS_TYPE_BOOLEAN, &value);
	}

	/*
	 * If "Attached" property was about to be signalled as TRUE but there
	 * were still active contexts, try again to signal "Attached" property
	 * to registered applications after active contexts have been released.
	 *
	 * "Attached" could also change to FALSE in case of LTE and getting
	 * deactivated
	 */
	if (on_lte(gprs) || gprs->flags & GPRS_FLAG_ATTACHED_UPDATE) {
		gprs->flags &= ~GPRS_FLAG_ATTACHED_UPDATE;
		gprs_attached_update(gprs);
	}
}

static void gprs_context_remove(struct ofono_atom *atom)
{
	struct ofono_gprs_context *gc = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (gc == NULL)
		return;

	if (gc->driver && gc->driver->remove)
		gc->driver->remove(gc);

	l_free(gc->interface);
	g_free(gc);
}

static void ofono_gprs_context_register(struct ofono_gprs_context *gc)
{
}

OFONO_DEFINE_ATOM_CREATE(gprs_context, OFONO_ATOM_TYPE_GPRS_CONTEXT, {
	atom->type = OFONO_GPRS_CONTEXT_TYPE_ANY;
})

void ofono_gprs_context_remove(struct ofono_gprs_context *gc)
{
	if (gc == NULL)
		return;

	__ofono_atom_free(gc->atom);
}

void ofono_gprs_context_set_data(struct ofono_gprs_context *gc, void *data)
{
	gc->driver_data = data;
}

void *ofono_gprs_context_get_data(struct ofono_gprs_context *gc)
{
	return gc->driver_data;
}

struct ofono_modem *ofono_gprs_context_get_modem(struct ofono_gprs_context *gc)
{
	return __ofono_atom_get_modem(gc->atom);
}

void ofono_gprs_context_set_type(struct ofono_gprs_context *gc,
					enum ofono_gprs_context_type type)
{
	DBG("type %d", type);

	gc->type = type;
}

enum ofono_gprs_context_type ofono_gprs_context_get_type(
						struct ofono_gprs_context *gc)
{
	return gc->type;
}

const char *ofono_gprs_context_get_interface(struct ofono_gprs_context *gc)
{
	return gc->interface;
}

void ofono_gprs_context_set_interface(struct ofono_gprs_context *gc,
					const char *interface)
{
	l_free(gc->interface);
	gc->interface = l_strdup(interface);
}

void ofono_gprs_context_set_ipv4_address(struct ofono_gprs_context *gc,
						const char *address,
						ofono_bool_t static_ip)
{
	struct context_settings *settings = gc->settings;

	if (settings->ipv4 == NULL)
		return;

	l_free(settings->ipv4->ip);
	settings->ipv4->ip = l_strdup(address);
	settings->ipv4->static_ip = static_ip;
}

void ofono_gprs_context_set_ipv4_netmask(struct ofono_gprs_context *gc,
						const char *netmask)
{
	struct context_settings *settings = gc->settings;

	if (settings->ipv4 == NULL)
		return;

	l_free(settings->ipv4->netmask);
	settings->ipv4->netmask = l_strdup(netmask);
}

void ofono_gprs_context_set_ipv4_prefix_length(struct ofono_gprs_context *gc,
						unsigned int length)
{
	struct context_settings *settings = gc->settings;
	struct in_addr ipv4;
	char buf[INET_ADDRSTRLEN];

	if (settings->ipv4 == NULL)
		return;

	l_free(settings->ipv4->netmask);

	memset(&ipv4, 0, sizeof(ipv4));

	if (length)
		ipv4.s_addr = htonl(~((1 << (32 - length)) - 1));

	inet_ntop(AF_INET, &ipv4, buf, sizeof(buf));
	settings->ipv4->netmask = l_strdup(buf);
}

void ofono_gprs_context_set_ipv4_gateway(struct ofono_gprs_context *gc,
						const char *gateway)
{
	struct context_settings *settings = gc->settings;

	if (settings->ipv4 == NULL)
		return;

	l_free(settings->ipv4->gateway);
	settings->ipv4->gateway = l_strdup(gateway);
}

void ofono_gprs_context_set_ipv4_dns_servers(struct ofono_gprs_context *gc,
						const char **dns)
{
	struct context_settings *settings = gc->settings;

	if (settings->ipv4 == NULL)
		return;

	l_strv_free(settings->ipv4->dns);
	settings->ipv4->dns = l_strv_copy((char **) dns);
}

void ofono_gprs_context_set_ipv6_address(struct ofono_gprs_context *gc,
						const char *address)
{
	struct context_settings *settings = gc->settings;

	if (settings->ipv6 == NULL)
		return;

	l_free(settings->ipv6->ip);
	settings->ipv6->ip = l_strdup(address);
}

void ofono_gprs_context_set_ipv6_prefix_length(struct ofono_gprs_context *gc,
						unsigned char length)
{
	struct context_settings *settings = gc->settings;

	if (settings->ipv6 == NULL)
		return;

	settings->ipv6->prefix_len = length;
}

void ofono_gprs_context_set_ipv6_gateway(struct ofono_gprs_context *gc,
						const char *gateway)
{
	struct context_settings *settings = gc->settings;

	if (settings->ipv6 == NULL)
		return;

	l_free(settings->ipv6->gateway);
	settings->ipv6->gateway = l_strdup(gateway);
}

void ofono_gprs_context_set_ipv6_dns_servers(struct ofono_gprs_context *gc,
						const char **dns)
{
	struct context_settings *settings = gc->settings;

	if (settings->ipv6 == NULL)
		return;

	l_strv_free(settings->ipv6->dns);
	settings->ipv6->dns = l_strv_copy((char **) dns);
}

static void free_contexts(struct ofono_gprs *gprs)
{
	GSList *l;

	if (gprs->settings) {
		storage_close(gprs->imsi, SETTINGS_STORE,
				gprs->settings, TRUE);

		l_free(gprs->imsi);
		gprs->imsi = NULL;
		gprs->settings = NULL;
	}

	for (l = gprs->contexts; l; l = l->next) {
		struct pri_context *context = l->data;

		context_dbus_unregister(context);
	}

	g_slist_free(gprs->contexts);
}

static void gprs_unregister(struct ofono_atom *atom)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_gprs *gprs = __ofono_atom_get_data(atom);
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);

	DBG("%p", gprs);

	free_contexts(gprs);

	if (gprs->netreg_watch) {
		if (gprs->status_watch) {
			__ofono_netreg_remove_status_watch(gprs->netreg,
							gprs->status_watch);
			gprs->status_watch = 0;
		}

		__ofono_modem_remove_atom_watch(modem, gprs->netreg_watch);
		gprs->netreg_watch = 0;
		gprs->netreg = NULL;
	}

	if (gprs->spn_watch) {
		struct ofono_sim *sim = __ofono_atom_find(OFONO_ATOM_TYPE_SIM,
								modem);

		ofono_sim_remove_spn_watch(sim, &gprs->spn_watch);
	}

	ofono_modem_remove_interface(modem,
					OFONO_CONNECTION_MANAGER_INTERFACE);
	g_dbus_unregister_interface(conn, path,
					OFONO_CONNECTION_MANAGER_INTERFACE);
}

static void gprs_remove(struct ofono_atom *atom)
{
	struct ofono_gprs *gprs = __ofono_atom_get_data(atom);
	GSList *l;

	DBG("atom: %p", atom);

	if (gprs == NULL)
		return;

	if (gprs->suspend_timeout)
		g_source_remove(gprs->suspend_timeout);

	l_uintset_free(gprs->used_cids);
	gprs->used_cids = NULL;

	l_uintset_free(gprs->used_pids);
	gprs->used_pids = NULL;

	for (l = gprs->context_drivers; l; l = l->next) {
		struct ofono_gprs_context *gc = l->data;

		gc->gprs = NULL;
	}

	g_slist_free(gprs->context_drivers);

	if (gprs->driver && gprs->driver->remove)
		gprs->driver->remove(gprs);

	g_free(gprs);
}

OFONO_DEFINE_ATOM_CREATE(gprs, OFONO_ATOM_TYPE_GPRS, {
	atom->status = NETWORK_REGISTRATION_STATUS_UNKNOWN;
	atom->netreg_status = -1;
	atom->used_pids = l_uintset_new(MAX_CONTEXTS);
	atom->used_cids = l_uintset_new_from_range(1, MAX_CONTEXTS - 1);
})

static void netreg_watch(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond,
				void *data)
{
	struct ofono_gprs *gprs = data;
	int status;

	if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		gprs_netreg_removed(gprs);
		return;
	}

	gprs->netreg = __ofono_atom_get_data(atom);
	status = ofono_netreg_get_status(gprs->netreg);

	/*
	 * If the status is known, assign it, otherwise keep the init value
	 * to indicate that the netreg atom is not initialised with a known
	 * value
	 */
	if (status != NETWORK_REGISTRATION_STATUS_UNKNOWN)
		gprs->netreg_status = status;

	gprs->status_watch = __ofono_netreg_add_status_watch(gprs->netreg,
					netreg_status_changed, gprs, NULL);

	gprs_netreg_update(gprs);
}

static gboolean load_context(struct ofono_gprs *gprs, const char *group)
{
	char *name = NULL;
	char *typestr = NULL;
	char *protostr = NULL;
	char *username = NULL;
	char *password = NULL;
	char *apn = NULL;
	char *msgproxy = NULL;
	char *msgcenter = NULL;
	char *authstr = NULL;
	gboolean ret = FALSE;
	gboolean legacy = FALSE;
	struct pri_context *context;
	enum ofono_gprs_context_type type;
	enum ofono_gprs_proto proto;
	enum ofono_gprs_auth_method auth;
	unsigned int id;

	if (sscanf(group, "context%d", &id) != 1) {
		if (sscanf(group, "primarycontext%d", &id) != 1)
			goto error;

		legacy = TRUE;
	}

	if (id < 1 || id > MAX_CONTEXTS)
		goto error;

	name = g_key_file_get_string(gprs->settings, group, "Name", NULL);
	if (name == NULL)
		goto error;

	typestr = g_key_file_get_string(gprs->settings, group, "Type", NULL);
	if (typestr == NULL)
		goto error;

	if (gprs_context_string_to_type(typestr, &type) == FALSE)
		goto error;

	protostr = g_key_file_get_string(gprs->settings, group,
							"Protocol", NULL);
	if (protostr == NULL)
		protostr = g_strdup("ip");

	if (!gprs_proto_from_string(protostr, &proto))
		goto error;

	username = g_key_file_get_string(gprs->settings, group,
						"Username", NULL);
	if (username == NULL)
		goto error;

	if (strlen(username) > OFONO_GPRS_MAX_USERNAME_LENGTH)
		goto error;

	password = g_key_file_get_string(gprs->settings, group,
						"Password", NULL);
	if (password == NULL)
		goto error;

	authstr = g_key_file_get_string(gprs->settings, group,
						"AuthenticationMethod", NULL);
	if (authstr == NULL)
		authstr = g_strdup("chap");

	if (!gprs_auth_method_from_string(authstr, &auth))
		goto error;

	if (strlen(password) > OFONO_GPRS_MAX_PASSWORD_LENGTH)
		goto error;

	apn = g_key_file_get_string(gprs->settings, group,
					"AccessPointName", NULL);
	if (apn == NULL)
		goto error;

	if (type == OFONO_GPRS_CONTEXT_TYPE_MMS) {
		msgproxy = g_key_file_get_string(gprs->settings, group,
						"MessageProxy", NULL);

		msgcenter = g_key_file_get_string(gprs->settings, group,
						"MessageCenter", NULL);
	}

	/*
	 * Accept empty (just created) APNs, but don't allow other
	 * invalid ones
	 */
	if (apn[0] != '\0' && is_valid_apn(apn) == FALSE)
		goto error;

	context = pri_context_create(gprs, name, type);
	if (context == NULL)
		goto error;

	l_uintset_put(gprs->used_pids, id);
	context->id = id;
	strcpy(context->context.username, username);
	strcpy(context->context.password, password);
	strcpy(context->context.apn, apn);
	context->context.proto = proto;
	context->context.auth_method = auth;

	if (msgproxy != NULL)
		strcpy(context->message_proxy, msgproxy);

	if (msgcenter != NULL)
		strcpy(context->message_center, msgcenter);

	if (context_dbus_register(context) == FALSE)
		goto error;

	gprs->last_context_id = id;

	gprs->contexts = g_slist_append(gprs->contexts, context);
	ret = TRUE;

	if (legacy) {
		write_context_settings(gprs, context);
		g_key_file_remove_group(gprs->settings, group, NULL);
	}

error:
	g_free(name);
	g_free(typestr);
	g_free(protostr);
	g_free(username);
	g_free(password);
	g_free(apn);
	g_free(msgproxy);
	g_free(msgcenter);
	g_free(authstr);

	return ret;
}

static void gprs_load_settings(struct ofono_gprs *gprs, const char *imsi)
{
	GError *error;
	gboolean legacy = FALSE;
	char **groups;
	int i;

	gprs->settings = storage_open(imsi, SETTINGS_STORE);

	if (gprs->settings == NULL)
		return;

	gprs->imsi = l_strdup(imsi);

	error = NULL;
	gprs->powered = g_key_file_get_boolean(gprs->settings, SETTINGS_GROUP,
						"Powered", &error);

	/*
	 * If any error occurs, simply switch to defaults.
	 * Default to Powered = True
	 * and RoamingAllowed = False
	 */
	if (error) {
		g_error_free(error);
		gprs->powered = TRUE;
		g_key_file_set_boolean(gprs->settings, SETTINGS_GROUP,
					"Powered", gprs->powered);
	}

	error = NULL;
	gprs->roaming_allowed = g_key_file_get_boolean(gprs->settings,
							SETTINGS_GROUP,
							"RoamingAllowed",
							&error);

	if (error) {
		g_error_free(error);
		gprs->roaming_allowed = FALSE;
		g_key_file_set_boolean(gprs->settings, SETTINGS_GROUP,
					"RoamingAllowed",
					gprs->roaming_allowed);
	}

	groups = g_key_file_get_groups(gprs->settings, NULL);

	for (i = 0; groups[i]; i++) {
		if (g_str_equal(groups[i], SETTINGS_GROUP))
			continue;

		if (!l_str_has_prefix(groups[i], "context")) {
			if (!l_str_has_prefix(groups[i], "primarycontext"))
				goto remove;

			legacy = TRUE;
		}

		if (load_context(gprs, groups[i]) == TRUE)
			continue;

remove:
		g_key_file_remove_group(gprs->settings, groups[i], NULL);
	}

	g_strfreev(groups);

	if (legacy)
		storage_sync(imsi, SETTINGS_STORE, gprs->settings);
}

static void gprs_list_active_contexts_callback(const struct ofono_error *error,
						void *data)
{
	DBG("error = %d", error->type);
}

static void ofono_gprs_finish_register(struct ofono_gprs *gprs)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(gprs->atom);
	const char *path = __ofono_atom_get_path(gprs->atom);
	const struct ofono_gprs_driver *driver = gprs->driver;

	if (gprs->contexts == NULL) /* Automatic provisioning failed */
		add_context(gprs, NULL, OFONO_GPRS_CONTEXT_TYPE_INTERNET);

	if (!g_dbus_register_interface(conn, path,
					OFONO_CONNECTION_MANAGER_INTERFACE,
					manager_methods, manager_signals, NULL,
					gprs, NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_CONNECTION_MANAGER_INTERFACE);

		free_contexts(gprs);
		return;
	}

	ofono_modem_add_interface(modem,
				OFONO_CONNECTION_MANAGER_INTERFACE);

	gprs->netreg_watch = __ofono_modem_add_atom_watch(modem,
					OFONO_ATOM_TYPE_NETREG,
					netreg_watch, gprs, NULL);

	__ofono_atom_register(gprs->atom, gprs_unregister);

	/* Find any context activated during init */
	if (driver->list_active_contexts)
		driver->list_active_contexts(gprs,
					     gprs_list_active_contexts_callback,
					     gprs);
}

static void spn_read_cb(const char *spn, const char *dc, void *data)
{
	struct ofono_gprs *gprs	= data;
	struct ofono_modem *modem = __ofono_atom_get_modem(gprs->atom);
	struct ofono_sim *sim = __ofono_atom_find(OFONO_ATOM_TYPE_SIM, modem);

	provision_contexts(gprs, ofono_sim_get_mcc(sim),
					ofono_sim_get_mnc(sim), spn);

	ofono_sim_remove_spn_watch(sim, &gprs->spn_watch);

	ofono_gprs_finish_register(gprs);
}

struct ofono_modem *ofono_gprs_get_modem(struct ofono_gprs *gprs)
{
	return __ofono_atom_get_modem(gprs->atom);
}

void ofono_gprs_register(struct ofono_gprs *gprs)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(gprs->atom);
	struct ofono_sim *sim = __ofono_atom_find(OFONO_ATOM_TYPE_SIM, modem);

	if (sim == NULL)
		goto finish;

	gprs_load_settings(gprs, ofono_sim_get_imsi(sim));

	if (gprs->contexts)
		goto finish;

	ofono_sim_add_spn_watch(sim, &gprs->spn_watch, spn_read_cb, gprs, NULL);
	return;

finish:
	ofono_gprs_finish_register(gprs);
}

void ofono_gprs_remove(struct ofono_gprs *gprs)
{
	__ofono_atom_free(gprs->atom);
}

void ofono_gprs_set_data(struct ofono_gprs *gprs, void *data)
{
	gprs->driver_data = data;
}

void *ofono_gprs_get_data(struct ofono_gprs *gprs)
{
	return gprs->driver_data;
}
