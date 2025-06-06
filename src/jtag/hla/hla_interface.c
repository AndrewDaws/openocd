// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   Copyright (C) 2011 by Mathias Kuester                                 *
 *   Mathias Kuester <kesmtp@freenet.de>                                   *
 *                                                                         *
 *   Copyright (C) 2012 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* project specific includes */
#include <jtag/interface.h>
#include <transport/transport.h>
#include <helper/time_support.h>

#include <jtag/hla/hla_layout.h>
#include <jtag/hla/hla_transport.h>
#include <jtag/hla/hla_interface.h>

#include <target/target.h>

static struct hl_interface hl_if = {
	.param = {
		.device_desc = NULL,
		.vid = { 0 },
		.pid = { 0 },
		.transport = HL_TRANSPORT_UNKNOWN,
		.connect_under_reset = false,
		.use_stlink_tcp = false,
		.stlink_tcp_port = 7184,
	},
	.layout = NULL,
	.handle = NULL,
};

int hl_interface_open(enum hl_transports tr)
{
	LOG_DEBUG("hl_interface_open");

	enum reset_types jtag_reset_config = jtag_get_reset_config();

	if (jtag_reset_config & RESET_CNCT_UNDER_SRST) {
		if (jtag_reset_config & RESET_SRST_NO_GATING)
			hl_if.param.connect_under_reset = true;
		else
			LOG_WARNING("\'srst_nogate\' reset_config option is required");
	}

	/* set transport mode */
	hl_if.param.transport = tr;

	int result = hl_if.layout->open(&hl_if);
	if (result != ERROR_OK)
		return result;

	return hl_interface_init_reset();
}

int hl_interface_init_target(struct target *t)
{
	int res;

	LOG_DEBUG("hl_interface_init_target");

	/* this is the interface for the current target and we
	 * can setup the private pointer in the tap structure
	 * if the interface match the tap idcode
	 */
	res = hl_if.layout->api->idcode(hl_if.handle, &t->tap->idcode);

	if (res != ERROR_OK)
		return res;

	unsigned int ii, limit = t->tap->expected_ids_cnt;
	int found = 0;

	for (ii = 0; ii < limit; ii++) {
		uint32_t expected = t->tap->expected_ids[ii];

		/* treat "-expected-id 0" as a "don't-warn" wildcard */
		if (!expected || !t->tap->idcode ||
		    (t->tap->idcode == expected)) {
			found = 1;
			break;
		}
	}

	if (found == 0) {
		LOG_WARNING("UNEXPECTED idcode: 0x%08" PRIx32, t->tap->idcode);
		for (ii = 0; ii < limit; ii++)
			LOG_ERROR("expected %u of %u: 0x%08" PRIx32, ii + 1, limit,
				t->tap->expected_ids[ii]);

		return ERROR_FAIL;
	}

	t->tap->priv = &hl_if;
	t->tap->has_idcode = true;

	return ERROR_OK;
}

static int hl_interface_init(void)
{
	LOG_DEBUG("hl_interface_init");

	/* here we can initialize the layout */
	return hl_layout_init(&hl_if);
}

static int hl_interface_quit(void)
{
	LOG_DEBUG("hl_interface_quit");

	if (hl_if.layout->api->close)
		hl_if.layout->api->close(hl_if.handle);

	jtag_command_queue_reset();

	free((void *)hl_if.param.device_desc);

	return ERROR_OK;
}

static int hl_interface_reset(int req_trst, int req_srst)
{
	return hl_if.layout->api->assert_srst(hl_if.handle, req_srst ? 0 : 1);
}

int hl_interface_init_reset(void)
{
	/* in case the adapter has not already handled asserting srst
	 * we will attempt it again */
	if (hl_if.param.connect_under_reset) {
		adapter_assert_reset();
	} else {
		adapter_deassert_reset();
	}

	return ERROR_OK;
}

static int hl_interface_khz(int khz, int *jtag_speed)
{
	if (!hl_if.layout->api->speed)
		return ERROR_OK;

	*jtag_speed = hl_if.layout->api->speed(hl_if.handle, khz, true);
	return ERROR_OK;
}

static int hl_interface_speed_div(int speed, int *khz)
{
	*khz = speed;
	return ERROR_OK;
}

static int hl_interface_speed(int speed)
{
	if (!hl_if.layout->api->speed)
		return ERROR_OK;

	if (!hl_if.handle)
		return ERROR_OK;

	hl_if.layout->api->speed(hl_if.handle, speed, false);

	return ERROR_OK;
}

int hl_interface_override_target(const char **targetname)
{
	if (hl_if.layout->api->override_target) {
		if (hl_if.layout->api->override_target(*targetname)) {
			*targetname = "hla_target";
			return ERROR_OK;
		} else
			return ERROR_FAIL;
	}
	return ERROR_FAIL;
}

static int hl_interface_config_trace(bool enabled, enum tpiu_pin_protocol pin_protocol,
		uint32_t port_size, unsigned int *trace_freq,
		unsigned int traceclkin_freq, uint16_t *prescaler)
{
	if (hl_if.layout->api->config_trace)
		return hl_if.layout->api->config_trace(hl_if.handle, enabled,
			pin_protocol, port_size, trace_freq, traceclkin_freq, prescaler);
	else if (enabled) {
		LOG_ERROR("The selected interface does not support tracing");
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int hl_interface_poll_trace(uint8_t *buf, size_t *size)
{
	if (hl_if.layout->api->poll_trace)
		return hl_if.layout->api->poll_trace(hl_if.handle, buf, size);

	return ERROR_FAIL;
}

COMMAND_HANDLER(hl_interface_handle_device_desc_command)
{
	LOG_DEBUG("hl_interface_handle_device_desc_command");

	if (CMD_ARGC == 1) {
		hl_if.param.device_desc = strdup(CMD_ARGV[0]);
	} else {
		LOG_ERROR("expected exactly one argument to hl_device_desc <description>");
	}

	return ERROR_OK;
}

COMMAND_HANDLER(hl_interface_handle_layout_command)
{
	LOG_DEBUG("hl_interface_handle_layout_command");

	if (CMD_ARGC != 1) {
		LOG_ERROR("Need exactly one argument to stlink_layout");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	if (hl_if.layout) {
		LOG_ERROR("already specified hl_layout %s",
				hl_if.layout->name);
		return (strcmp(hl_if.layout->name, CMD_ARGV[0]) != 0)
		    ? ERROR_FAIL : ERROR_OK;
	}

	for (const struct hl_layout *l = hl_layout_get_list(); l->name;
	     l++) {
		if (strcmp(l->name, CMD_ARGV[0]) == 0) {
			hl_if.layout = l;
			return ERROR_OK;
		}
	}

	LOG_ERROR("No adapter layout '%s' found", CMD_ARGV[0]);
	return ERROR_FAIL;
}

COMMAND_HANDLER(hl_interface_handle_vid_pid_command)
{
	if (CMD_ARGC > HLA_MAX_USB_IDS * 2) {
		LOG_WARNING("ignoring extra IDs in hla_vid_pid "
			"(maximum is %d pairs)", HLA_MAX_USB_IDS);
		CMD_ARGC = HLA_MAX_USB_IDS * 2;
	}
	if (CMD_ARGC < 2 || (CMD_ARGC & 1)) {
		LOG_WARNING("incomplete hla_vid_pid configuration directive");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	unsigned int i;
	for (i = 0; i < CMD_ARGC; i += 2) {
		COMMAND_PARSE_NUMBER(u16, CMD_ARGV[i], hl_if.param.vid[i / 2]);
		COMMAND_PARSE_NUMBER(u16, CMD_ARGV[i + 1], hl_if.param.pid[i / 2]);
	}

	/*
	 * Explicitly terminate, in case there are multiple instances of
	 * hla_vid_pid.
	 */
	hl_if.param.vid[i / 2] = hl_if.param.pid[i / 2] = 0;

	return ERROR_OK;
}

COMMAND_HANDLER(hl_interface_handle_stlink_backend_command)
{
	/* default values */
	bool use_stlink_tcp = false;
	uint16_t stlink_tcp_port = 7184;

	if (CMD_ARGC == 0 || CMD_ARGC > 2)
		return ERROR_COMMAND_SYNTAX_ERROR;
	else if (strcmp(CMD_ARGV[0], "usb") == 0) {
		if (CMD_ARGC > 1)
			return ERROR_COMMAND_SYNTAX_ERROR;
		/* else use_stlink_tcp = false (already the case ) */
	} else if (strcmp(CMD_ARGV[0], "tcp") == 0) {
		use_stlink_tcp = true;
		if (CMD_ARGC == 2)
			COMMAND_PARSE_NUMBER(u16, CMD_ARGV[1], stlink_tcp_port);
	} else
		return ERROR_COMMAND_SYNTAX_ERROR;

	hl_if.param.use_stlink_tcp = use_stlink_tcp;
	hl_if.param.stlink_tcp_port = stlink_tcp_port;

	return ERROR_OK;
}

COMMAND_HANDLER(interface_handle_hla_command)
{
	if (CMD_ARGC != 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (!hl_if.layout->api->custom_command) {
		LOG_ERROR("The selected adapter doesn't support custom commands");
		return ERROR_FAIL;
	}

	hl_if.layout->api->custom_command(hl_if.handle, CMD_ARGV[0]);

	return ERROR_OK;
}

static const struct command_registration hl_interface_subcommand_handlers[] = {
	{
	 .name = "device_desc",
	 .handler = &hl_interface_handle_device_desc_command,
	 .mode = COMMAND_CONFIG,
	 .help = "set the device description of the adapter",
	 .usage = "description_string",
	 },
	{
	 .name = "layout",
	 .handler = &hl_interface_handle_layout_command,
	 .mode = COMMAND_CONFIG,
	 .help = "set the layout of the adapter",
	 .usage = "layout_name",
	 },
	{
	 .name = "vid_pid",
	 .handler = &hl_interface_handle_vid_pid_command,
	 .mode = COMMAND_CONFIG,
	 .help = "the vendor and product ID of the adapter",
	 .usage = "(vid pid)*",
	 },
	{
	 .name = "stlink_backend",
	 .handler = &hl_interface_handle_stlink_backend_command,
	 .mode = COMMAND_CONFIG,
	 .help = "select which ST-Link backend to use",
	 .usage = "usb | tcp [port]",
	},
	{
	 .name = "command",
	 .handler = &interface_handle_hla_command,
	 .mode = COMMAND_EXEC,
	 .help = "execute a custom adapter-specific command",
	 .usage = "<command>",
	 },
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration hl_interface_command_handlers[] = {
	{
		.name = "hla",
		.mode = COMMAND_ANY,
		.help = "perform hla management",
		.chain = hl_interface_subcommand_handlers,
		.usage = "",
	},
	COMMAND_REGISTRATION_DONE
};

struct adapter_driver hl_adapter_driver = {
	.name = "hla",
	.transport_ids = TRANSPORT_HLA_SWD | TRANSPORT_HLA_JTAG,
	.transport_preferred_id = TRANSPORT_HLA_SWD,
	.commands = hl_interface_command_handlers,

	.init = hl_interface_init,
	.quit = hl_interface_quit,
	.reset = hl_interface_reset,
	.speed = &hl_interface_speed,
	.khz = &hl_interface_khz,
	.speed_div = &hl_interface_speed_div,
	.config_trace = &hl_interface_config_trace,
	.poll_trace = &hl_interface_poll_trace,

	/* no ops for HLA, targets hla_target and stm8 intercept them all */
};
