// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if IS_WIN32 != 1
#error "The jlinkdll driver is only supported on Windows hosts"
#endif

#include <helper/command.h>
#include <helper/log.h>
#include <helper/binarybuffer.h>
#include <jtag/adapter.h>
#include <jtag/interface.h>
#include <jtag/commands.h>
#include <transport/transport.h>

#include <errno.h>
#include <inttypes.h>
#include <windows.h>

#define JLINKARM_TIF_JTAG	0
#define JLINKARM_TIF_CJTAG	7

#define JLINKDLL_MAX_TRANSFER_BITS	4096

typedef void (*jlinkdll_log_handler_t)(const char *msg);
typedef const char *(*jlinkdll_openex_t)(jlinkdll_log_handler_t log,
		jlinkdll_log_handler_t error);
typedef void (*jlinkdll_close_t)(void);
typedef int (*jlinkdll_exec_command_t)(const char *cmd, char *out,
		int out_size);
typedef int (*jlinkdll_emu_select_by_usbsn_t)(uint32_t serial);
typedef uint32_t (*jlinkdll_get_dll_version_t)(void);
typedef int (*jlinkdll_get_sn_t)(void);
typedef int (*jlinkdll_tif_get_available_t)(void);
typedef int (*jlinkdll_tif_select_t)(int tif);
typedef void (*jlinkdll_set_speed_t)(uint32_t speed_khz);
typedef int (*jlinkdll_connect_t)(void);
typedef int (*jlinkdll_jtag_enable_if_t)(void);
typedef int (*jlinkdll_jtag_disable_if_t)(void);
typedef int (*jlinkdll_jtag_store_raw_t)(const uint8_t *tdi,
		const uint8_t *tms, uint32_t num_bits);
typedef int (*jlinkdll_jtag_store_get_raw_t)(const uint8_t *tdi,
		uint8_t *tdo, const uint8_t *tms, uint32_t num_bits);
typedef int (*jlinkdll_jtag_store_bits_t)(uint32_t tms, uint32_t tdi,
		uint32_t num_bits);
typedef int (*jlinkdll_jtag_store_inst_t)(const uint8_t *data, int num_bits);
typedef int (*jlinkdll_jtag_store_data_t)(const uint8_t *data, int num_bits);
typedef int (*jlinkdll_jtag_store_get_data_t)(const uint8_t *data,
		uint8_t *out, int num_bits);
typedef uint32_t (*jlinkdll_jtag_get_device_id_t)(int index);
typedef uint32_t (*jlinkdll_jtag_get_u32_t)(int bitpos);
typedef int (*jlinkdll_jtag_sync_bits_t)(void);
typedef int (*jlinkdll_read_mem_t)(uint32_t addr, uint32_t num_bytes,
		void *data);
typedef int (*jlinkdll_write_mem_t)(uint32_t addr, uint32_t num_bytes,
		const void *data);
typedef void (*jlinkdll_set_pin_t)(void);

struct jlinkdll_api {
	jlinkdll_openex_t openex;
	jlinkdll_close_t close;
	jlinkdll_exec_command_t exec_command;
	jlinkdll_emu_select_by_usbsn_t emu_select_by_usbsn;
	jlinkdll_get_dll_version_t get_dll_version;
	jlinkdll_get_sn_t get_sn;
	jlinkdll_tif_get_available_t tif_get_available;
	jlinkdll_tif_select_t tif_select;
	jlinkdll_set_speed_t set_speed;
	jlinkdll_connect_t connect;
	jlinkdll_jtag_enable_if_t jtag_enable_if;
	jlinkdll_jtag_disable_if_t jtag_disable_if;
	jlinkdll_jtag_store_raw_t jtag_store_raw;
	jlinkdll_jtag_store_get_raw_t jtag_store_get_raw;
	jlinkdll_jtag_store_bits_t jtag_store_bits;
	jlinkdll_jtag_store_inst_t jtag_store_inst;
	jlinkdll_jtag_store_data_t jtag_store_data;
	jlinkdll_jtag_store_get_data_t jtag_store_get_data;
	jlinkdll_jtag_get_device_id_t jtag_get_device_id;
	jlinkdll_jtag_get_u32_t jtag_get_u32;
	jlinkdll_jtag_sync_bits_t jtag_sync_bits;
	jlinkdll_read_mem_t read_mem;
	jlinkdll_write_mem_t write_mem;
	jlinkdll_set_pin_t set_reset;
	jlinkdll_set_pin_t clear_reset;
	jlinkdll_set_pin_t set_trst;
	jlinkdll_set_pin_t clear_trst;
};

struct jlinkdll_exec {
	char *cmd;
	struct jlinkdll_exec *next;
};

static HMODULE jlinkdll_library;
static struct jlinkdll_api jlinkdll;
static char *jlinkdll_path;
static char *jlinkdll_device;
static bool jlinkdll_connect_on_init;
static bool jlinkdll_opened;
static bool jlinkdll_jtag_if_enabled;
static struct jlinkdll_exec *jlinkdll_execs;
static struct jlinkdll_exec **jlinkdll_execs_tail = &jlinkdll_execs;

static const char * const jlinkdll_transports[] = { "jtag", "cjtag", NULL };

static int jlinkdll_selected_tif(void)
{
	struct transport *transport = get_current_transport();

	if (transport && !strcmp(transport->name, "cjtag"))
		return JLINKARM_TIF_CJTAG;

	return JLINKARM_TIF_JTAG;
}

static const char *jlinkdll_tif_name(int tif)
{
	return tif == JLINKARM_TIF_CJTAG ? "cJTAG" : "JTAG";
}

static void jlinkdll_log_handler(const char *msg)
{
	if (msg && *msg)
		LOG_DEBUG("J-Link DLL: %s", msg);
}

static void jlinkdll_error_handler(const char *msg)
{
	if (msg && *msg)
		LOG_ERROR("J-Link DLL: %s", msg);
}

static void *jlinkdll_get_proc(const char *name, bool required)
{
	FARPROC proc = GetProcAddress(jlinkdll_library, name);

	if (!proc && required)
		LOG_ERROR("J-Link DLL is missing required symbol %s", name);

	return (void *)proc;
}

static int jlinkdll_load_api(void)
{
#if defined(_WIN64)
	const char *default_dll = "JLink_x64.dll";
#else
	const char *default_dll = "JLinkARM.dll";
#endif
	const char *path = jlinkdll_path ? jlinkdll_path : default_dll;

	jlinkdll_library = LoadLibraryA(path);
	if (!jlinkdll_library) {
		LOG_ERROR("Failed to load J-Link DLL '%s' (Windows error %lu)",
			path, (unsigned long)GetLastError());
		return ERROR_JTAG_DEVICE_ERROR;
	}

#define LOAD_REQUIRED(member, type, symbol) \
	do { \
		jlinkdll.member = (type)jlinkdll_get_proc((symbol), true); \
		if (!jlinkdll.member) \
			return ERROR_JTAG_DEVICE_ERROR; \
	} while (0)

#define LOAD_OPTIONAL(member, type, symbol) \
	do { \
		jlinkdll.member = (type)jlinkdll_get_proc((symbol), false); \
	} while (0)

	LOAD_REQUIRED(openex, jlinkdll_openex_t, "JLINKARM_OpenEx");
	LOAD_REQUIRED(close, jlinkdll_close_t, "JLINKARM_Close");
	LOAD_REQUIRED(tif_select, jlinkdll_tif_select_t, "JLINKARM_TIF_Select");
	LOAD_REQUIRED(set_speed, jlinkdll_set_speed_t, "JLINKARM_SetSpeed");
	LOAD_REQUIRED(jtag_enable_if, jlinkdll_jtag_enable_if_t,
			"JLINKARM_JTAG_EnableIF");
	LOAD_REQUIRED(jtag_disable_if, jlinkdll_jtag_disable_if_t,
			"JLINKARM_JTAG_DisableIF");
	LOAD_REQUIRED(jtag_store_raw, jlinkdll_jtag_store_raw_t,
			"JLINKARM_JTAG_StoreRaw");
	LOAD_REQUIRED(jtag_store_get_raw, jlinkdll_jtag_store_get_raw_t,
			"JLINKARM_JTAG_StoreGetRaw");
	LOAD_REQUIRED(jtag_sync_bits, jlinkdll_jtag_sync_bits_t,
			"JLINKARM_JTAG_SyncBits");

	LOAD_OPTIONAL(jtag_store_bits, jlinkdll_jtag_store_bits_t,
			"JLINK_JTAG_StoreBits");
	LOAD_OPTIONAL(jtag_store_inst, jlinkdll_jtag_store_inst_t,
			"JLINKARM_JTAG_StoreInst");
	LOAD_OPTIONAL(jtag_store_data, jlinkdll_jtag_store_data_t,
			"JLINKARM_JTAG_StoreData");
	LOAD_OPTIONAL(jtag_store_get_data, jlinkdll_jtag_store_get_data_t,
			"JLINKARM_JTAG_StoreGetData");
	LOAD_OPTIONAL(jtag_get_device_id, jlinkdll_jtag_get_device_id_t,
			"JLINKARM_JTAG_GetDeviceId");
	LOAD_OPTIONAL(jtag_get_u32, jlinkdll_jtag_get_u32_t,
			"JLINKARM_JTAG_GetU32");
	LOAD_OPTIONAL(read_mem, jlinkdll_read_mem_t, "JLINKARM_ReadMem");
	LOAD_OPTIONAL(write_mem, jlinkdll_write_mem_t, "JLINKARM_WriteMem");

	LOAD_OPTIONAL(exec_command, jlinkdll_exec_command_t,
			"JLINKARM_ExecCommand");
	LOAD_OPTIONAL(emu_select_by_usbsn, jlinkdll_emu_select_by_usbsn_t,
			"JLINKARM_EMU_SelectByUSBSN");
	LOAD_OPTIONAL(get_dll_version, jlinkdll_get_dll_version_t,
			"JLINKARM_GetDLLVersion");
	LOAD_OPTIONAL(get_sn, jlinkdll_get_sn_t, "JLINKARM_GetSN");
	LOAD_OPTIONAL(tif_get_available, jlinkdll_tif_get_available_t,
			"JLINKARM_TIF_GetAvailable");
	LOAD_OPTIONAL(connect, jlinkdll_connect_t, "JLINKARM_Connect");
	LOAD_OPTIONAL(set_reset, jlinkdll_set_pin_t, "JLINKARM_SetRESET");
	LOAD_OPTIONAL(clear_reset, jlinkdll_set_pin_t, "JLINKARM_ClrRESET");
	LOAD_OPTIONAL(set_trst, jlinkdll_set_pin_t, "JLINKARM_SetTRST");
	LOAD_OPTIONAL(clear_trst, jlinkdll_set_pin_t, "JLINKARM_ClrTRST");

#undef LOAD_OPTIONAL
#undef LOAD_REQUIRED

	if (jlinkdll.get_dll_version)
		LOG_INFO("J-Link DLL version: %" PRIu32,
			jlinkdll.get_dll_version());

	return ERROR_OK;
}

static void jlinkdll_unload_api(void)
{
	memset(&jlinkdll, 0, sizeof(jlinkdll));

	if (jlinkdll_library) {
		FreeLibrary(jlinkdll_library);
		jlinkdll_library = NULL;
	}
}

static int jlinkdll_parse_serial(const char *serial, uint32_t *out)
{
	char *end;
	unsigned long value;

	errno = 0;
	value = strtoul(serial, &end, 10);
	if (errno || !end || *end || value > UINT32_MAX)
		return ERROR_FAIL;

	*out = (uint32_t)value;
	return ERROR_OK;
}

static int jlinkdll_exec_command(const char *cmd)
{
	char out[512] = { 0 };
	int ret;

	if (!jlinkdll.exec_command) {
		LOG_ERROR("J-Link DLL does not export JLINKARM_ExecCommand");
		return ERROR_JTAG_NOT_IMPLEMENTED;
	}

	LOG_INFO("J-Link DLL command: %s", cmd);
	ret = jlinkdll.exec_command(cmd, out, sizeof(out));
	if (ret < 0) {
		LOG_ERROR("J-Link DLL command '%s' failed with %d%s%s", cmd, ret,
			out[0] ? ": " : "", out[0] ? out : "");
		return ERROR_FAIL;
	}

	if (out[0])
		LOG_INFO("J-Link DLL command response: %s", out);

	LOG_INFO("J-Link DLL command returned %d", ret);
	return ERROR_OK;
}

static int jlinkdll_append_exec(const char *cmd)
{
	struct jlinkdll_exec *item = calloc(1, sizeof(*item));

	if (!item)
		return ERROR_FAIL;

	item->cmd = strdup(cmd);
	if (!item->cmd) {
		free(item);
		return ERROR_FAIL;
	}

	*jlinkdll_execs_tail = item;
	jlinkdll_execs_tail = &item->next;
	return ERROR_OK;
}

static int jlinkdll_run_config_commands(void)
{
	struct jlinkdll_exec *item;

	for (item = jlinkdll_execs; item; item = item->next) {
		int ret = jlinkdll_exec_command(item->cmd);
		if (ret != ERROR_OK)
			return ret;
	}

	return ERROR_OK;
}

static int jlinkdll_select_serial(void)
{
	const char *serial = adapter_get_required_serial();
	uint32_t serial_number;
	int ret;

	if (!serial)
		return ERROR_OK;

	if (!jlinkdll.emu_select_by_usbsn) {
		LOG_ERROR("adapter serial was set, but J-Link DLL does not export "
			"JLINKARM_EMU_SelectByUSBSN");
		return ERROR_JTAG_NOT_IMPLEMENTED;
	}

	ret = jlinkdll_parse_serial(serial, &serial_number);
	if (ret != ERROR_OK) {
		LOG_ERROR("Invalid J-Link serial number: %s", serial);
		return ret;
	}

	ret = jlinkdll.emu_select_by_usbsn(serial_number);
	if (ret < 0) {
		LOG_ERROR("JLINKARM_EMU_SelectByUSBSN(%" PRIu32 ") failed: %d",
			serial_number, ret);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	LOG_INFO("Selected J-Link serial %" PRIu32, serial_number);
	return ERROR_OK;
}

static int jlinkdll_flush(void)
{
	int ret = jlinkdll.jtag_sync_bits();

	if (ret < 0) {
		LOG_ERROR("JLINKARM_JTAG_SyncBits() failed: %d", ret);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	return ERROR_OK;
}

static bool jlinkdll_use_cjtag_highlevel(void)
{
	return jlinkdll_selected_tif() == JLINKARM_TIF_CJTAG &&
		jlinkdll.jtag_store_inst &&
		jlinkdll.jtag_store_data &&
		jlinkdll.jtag_store_get_data &&
		jlinkdll.jtag_get_device_id &&
		jlinkdll.jtag_get_u32;
}

static uint32_t jlinkdll_buffer_get_bits(const uint8_t *buffer,
		unsigned int num_bits)
{
	uint32_t value = 0;

	for (unsigned int i = 0; i < num_bits; i++) {
		if (buf_get_u32(buffer, i, 1))
			value |= (uint32_t)1 << i;
	}

	return value;
}

static bool jlinkdll_buffer_is_ones(const uint8_t *buffer,
		unsigned int num_bits)
{
	for (unsigned int i = 0; i < num_bits; i++) {
		if (!buf_get_u32(buffer, i, 1))
			return false;
	}

	return true;
}

static bool jlinkdll_synthesize_scan(struct scan_command *scan,
		const uint8_t *out_buffer, uint8_t *in_buffer,
		unsigned int scan_size)
{
	size_t bytes = DIV_ROUND_UP(scan_size, 8);

	if (!in_buffer || !jlinkdll_buffer_is_ones(out_buffer, scan_size))
		return false;

	/*
	 * The SEGGER cJTAG DLL path operates on the DLL-configured TAP, not on
	 * arbitrary blind chain scans. Synthesize OpenOCD's generic chain
	 * validation scans from the chain that JLINKARM_Connect() already found.
	 */
	if (!scan->ir_scan && scan->end_state == TAP_DRPAUSE &&
			scan_size >= 64 && jlinkdll.jtag_get_device_id) {
		uint32_t idcode = jlinkdll.jtag_get_device_id(0);

		if (!idcode)
			return false;

		memset(in_buffer, 0xff, bytes);
		buf_set_u32(in_buffer, 0, 32, idcode);
		LOG_DEBUG("Synthesized JTAG IDCODE scan from J-Link DLL: 0x%08" PRIx32,
			idcode);
		return true;
	}

	if (scan->ir_scan) {
		unsigned int total_ir_length = 0;
		unsigned int bit_offset = 0;

		for (struct jtag_tap *tap = jtag_tap_next_enabled(NULL); tap;
				tap = jtag_tap_next_enabled(tap)) {
			if (!tap->ir_length)
				return false;
			total_ir_length += tap->ir_length;
		}

		if (scan_size != total_ir_length + 2)
			return false;

		memset(in_buffer, 0xff, bytes);
		for (struct jtag_tap *tap = jtag_tap_next_enabled(NULL); tap;
				tap = jtag_tap_next_enabled(tap)) {
			buf_set_u32(in_buffer, bit_offset, tap->ir_length,
				tap->ir_capture_value);
			bit_offset += tap->ir_length;
		}
		buf_set_u32(in_buffer, bit_offset, 2, 0x3);
		LOG_DEBUG("Synthesized JTAG IR capture validation scan");
		return true;
	}

	return false;
}

static int jlinkdll_clock_data(const uint8_t *tdi, unsigned int tdi_offset,
		const uint8_t *tms, unsigned int tms_offset, uint8_t *tdo,
		unsigned int tdo_offset, unsigned int num_bits)
{
	unsigned int done = 0;
	bool use_store_bits = !tdo && jlinkdll_selected_tif() == JLINKARM_TIF_CJTAG &&
		jlinkdll.jtag_store_bits;

	while (done < num_bits) {
		unsigned int chunk = num_bits - done;
		size_t bytes;
		uint8_t *tdi_chunk;
		uint8_t *tms_chunk;
		uint8_t *tdo_chunk = NULL;
		int ret;

		if (use_store_bits && chunk > 32)
			chunk = 32;
		else if (chunk > JLINKDLL_MAX_TRANSFER_BITS)
			chunk = JLINKDLL_MAX_TRANSFER_BITS;

		bytes = DIV_ROUND_UP(chunk, 8);
		tdi_chunk = calloc(1, bytes);
		tms_chunk = calloc(1, bytes);
		if (!tdi_chunk || !tms_chunk) {
			free(tdi_chunk);
			free(tms_chunk);
			return ERROR_FAIL;
		}

		if (tdi)
			bit_copy(tdi_chunk, 0, tdi, tdi_offset + done, chunk);
		if (tms)
			bit_copy(tms_chunk, 0, tms, tms_offset + done, chunk);

		if (tdo) {
			tdo_chunk = calloc(1, bytes);
			if (!tdo_chunk) {
				free(tdi_chunk);
				free(tms_chunk);
				return ERROR_FAIL;
			}

			ret = jlinkdll.jtag_store_get_raw(tdi_chunk, tdo_chunk,
				tms_chunk, chunk);
			if (ret >= 0)
				ret = jlinkdll_flush();
			if (ret == ERROR_OK || ret >= 0)
				bit_copy(tdo, tdo_offset + done, tdo_chunk, 0, chunk);
		} else if (use_store_bits) {
			uint32_t tdi_bits = jlinkdll_buffer_get_bits(tdi_chunk, chunk);
			uint32_t tms_bits = jlinkdll_buffer_get_bits(tms_chunk, chunk);

			ret = jlinkdll.jtag_store_bits(tms_bits, tdi_bits, chunk);
		} else {
			ret = jlinkdll.jtag_store_raw(tdi_chunk, tms_chunk, chunk);
		}

		free(tdi_chunk);
		free(tms_chunk);
		free(tdo_chunk);

		if (ret < 0) {
			LOG_ERROR("J-Link DLL raw JTAG transfer failed: %d", ret);
			return ERROR_JTAG_DEVICE_ERROR;
		}

		done += chunk;
	}

	return ERROR_OK;
}

static int jlinkdll_end_state(tap_state_t state)
{
	if (!tap_is_state_stable(state)) {
		LOG_ERROR("BUG: %i is not a valid end state", state);
		return ERROR_JTAG_STATE_INVALID;
	}

	tap_set_end_state(state);
	return ERROR_OK;
}

static int jlinkdll_state_move(void)
{
	uint8_t tms_scan = tap_get_tms_path(tap_get_state(), tap_get_end_state());
	uint8_t tms_bits = tap_get_tms_path_len(tap_get_state(),
			tap_get_end_state());
	int ret;

	if (jlinkdll_use_cjtag_highlevel() && tap_get_end_state() == TAP_RESET) {
		tap_set_state(TAP_RESET);
		return ERROR_OK;
	}

	ret = jlinkdll_clock_data(NULL, 0, &tms_scan, 0, NULL, 0, tms_bits);
	if (ret != ERROR_OK)
		return ret;

	tap_set_state(tap_get_end_state());
	return ERROR_OK;
}

static int jlinkdll_path_move(unsigned int num_states, tap_state_t *path)
{
	uint8_t tms = 0xff;

	for (unsigned int i = 0; i < num_states; i++) {
		int ret;

		if (path[i] == tap_state_transition(tap_get_state(), false))
			ret = jlinkdll_clock_data(NULL, 0, NULL, 0, NULL, 0, 1);
		else if (path[i] == tap_state_transition(tap_get_state(), true))
			ret = jlinkdll_clock_data(NULL, 0, &tms, 0, NULL, 0, 1);
		else {
			LOG_ERROR("BUG: %s -> %s is not a valid TAP transition",
				tap_state_name(tap_get_state()),
				tap_state_name(path[i]));
			return ERROR_JTAG_TRANSITION_INVALID;
		}

		if (ret != ERROR_OK)
			return ret;

		tap_set_state(path[i]);
	}

	tap_set_end_state(tap_get_state());
	return ERROR_OK;
}

static int jlinkdll_stableclocks(unsigned int num_cycles)
{
	uint8_t tms = tap_get_state() == TAP_RESET ? 0xff : 0x00;

	if (jlinkdll_use_cjtag_highlevel() && tap_get_state() == TAP_RESET)
		return ERROR_OK;

	while (num_cycles) {
		unsigned int chunk = num_cycles;
		int ret;

		if (chunk > 8)
			chunk = 8;

		ret = jlinkdll_clock_data(NULL, 0, &tms, 0, NULL, 0, chunk);
		if (ret != ERROR_OK)
			return ret;

		num_cycles -= chunk;
	}

	return ERROR_OK;
}

static int jlinkdll_runtest(unsigned int num_cycles)
{
	tap_state_t saved_end_state = tap_get_end_state();
	int ret;

	if (tap_get_state() != TAP_IDLE) {
		ret = jlinkdll_end_state(TAP_IDLE);
		if (ret != ERROR_OK)
			return ret;

		ret = jlinkdll_state_move();
		if (ret != ERROR_OK)
			return ret;
	}

	ret = jlinkdll_stableclocks(num_cycles);
	if (ret != ERROR_OK)
		return ret;

	ret = jlinkdll_end_state(saved_end_state);
	if (ret != ERROR_OK)
		return ret;

	if (tap_get_state() != tap_get_end_state())
		return jlinkdll_state_move();

	return ERROR_OK;
}

static int jlinkdll_execute_scan_highlevel(struct jtag_command *cmd)
{
	struct scan_command *scan = cmd->cmd.scan;
	struct scan_field *field;
	unsigned int scan_size = 0;
	unsigned int bit_offset = 0;
	bool has_input = false;
	uint8_t *out_buffer = NULL;
	uint8_t *in_buffer = NULL;
	size_t bytes;
	int ret;
	int bitpos;

	while (scan->num_fields > 0 &&
			scan->fields[scan->num_fields - 1].num_bits == 0) {
		scan->num_fields--;
		LOG_DEBUG("discarding trailing empty field");
	}

	if (!scan->num_fields) {
		LOG_DEBUG("empty scan, doing nothing");
		return ERROR_OK;
	}

	ret = jlinkdll_end_state(scan->end_state);
	if (ret != ERROR_OK)
		return ret;

	field = scan->fields;
	for (unsigned int i = 0; i < scan->num_fields; i++, field++) {
		scan_size += field->num_bits;
		has_input |= !!field->in_value;
	}

	if (scan->ir_scan && scan_size > 32 && has_input) {
		LOG_ERROR("J-Link DLL cJTAG IR scans with input are limited to 32 bits");
		return ERROR_JTAG_DEVICE_ERROR;
	}

	bytes = DIV_ROUND_UP(scan_size, 8);
	out_buffer = calloc(1, bytes);
	if (!out_buffer)
		return ERROR_FAIL;

	if (has_input) {
		in_buffer = calloc(1, bytes);
		if (!in_buffer) {
			free(out_buffer);
			return ERROR_FAIL;
		}
	}

	field = scan->fields;
	for (unsigned int i = 0; i < scan->num_fields; i++, field++) {
		if (field->out_value)
			bit_copy(out_buffer, bit_offset, field->out_value, 0,
				field->num_bits);
		bit_offset += field->num_bits;
	}

	if (jlinkdll_synthesize_scan(scan, out_buffer, in_buffer, scan_size)) {
		ret = ERROR_OK;
		goto copy_input;
	}

	if (scan->ir_scan) {
		bitpos = jlinkdll.jtag_store_inst(out_buffer, scan_size);
		if (bitpos < 0) {
			LOG_ERROR("JLINKARM_JTAG_StoreInst() failed: %d", bitpos);
			ret = ERROR_JTAG_DEVICE_ERROR;
			goto done;
		}

		if (has_input) {
			uint32_t ir_capture = jlinkdll.jtag_get_u32(bitpos);

			buf_set_u32(in_buffer, 0, scan_size, ir_capture);
		}

		ret = jlinkdll_flush();
	} else if (has_input) {
		bitpos = jlinkdll.jtag_store_get_data(out_buffer, in_buffer,
				scan_size);
		if (bitpos < 0) {
			LOG_ERROR("JLINKARM_JTAG_StoreGetData() failed: %d", bitpos);
			ret = ERROR_JTAG_DEVICE_ERROR;
			goto done;
		}

		ret = jlinkdll_flush();
	} else {
		bitpos = jlinkdll.jtag_store_data(out_buffer, scan_size);
		if (bitpos < 0) {
			LOG_ERROR("JLINKARM_JTAG_StoreData() failed: %d", bitpos);
			ret = ERROR_JTAG_DEVICE_ERROR;
			goto done;
		}

		ret = jlinkdll_flush();
	}

	if (ret != ERROR_OK)
		goto done;

copy_input:
	if (has_input) {
		bit_offset = 0;
		field = scan->fields;
		for (unsigned int i = 0; i < scan->num_fields; i++, field++) {
			if (field->in_value)
				bit_copy(field->in_value, 0, in_buffer, bit_offset,
					field->num_bits);
			bit_offset += field->num_bits;
		}
	}

	tap_set_state(tap_get_end_state());

	LOG_DEBUG_IO("%s scan through J-Link DLL cJTAG API, %u bits, end in %s",
		scan->ir_scan ? "IR" : "DR", scan_size,
		tap_state_name(tap_get_end_state()));

done:
	free(out_buffer);
	free(in_buffer);
	return ret;
}

static int jlinkdll_execute_scan(struct jtag_command *cmd)
{
	struct scan_command *scan = cmd->cmd.scan;
	struct scan_field *field;
	unsigned int scan_size = 0;
	int ret;

	if (jlinkdll_use_cjtag_highlevel())
		return jlinkdll_execute_scan_highlevel(cmd);

	LOG_DEBUG_IO("%s type:%d", scan->ir_scan ? "IRSCAN" : "DRSCAN",
		jtag_scan_type(scan));

	while (scan->num_fields > 0 &&
			scan->fields[scan->num_fields - 1].num_bits == 0) {
		scan->num_fields--;
		LOG_DEBUG("discarding trailing empty field");
	}

	if (!scan->num_fields) {
		LOG_DEBUG("empty scan, doing nothing");
		return ERROR_OK;
	}

	if (scan->ir_scan) {
		if (tap_get_state() != TAP_IRSHIFT) {
			ret = jlinkdll_end_state(TAP_IRSHIFT);
			if (ret != ERROR_OK)
				return ret;
			ret = jlinkdll_state_move();
			if (ret != ERROR_OK)
				return ret;
		}
	} else if (tap_get_state() != TAP_DRSHIFT) {
		ret = jlinkdll_end_state(TAP_DRSHIFT);
		if (ret != ERROR_OK)
			return ret;
		ret = jlinkdll_state_move();
		if (ret != ERROR_OK)
			return ret;
	}

	ret = jlinkdll_end_state(scan->end_state);
	if (ret != ERROR_OK)
		return ret;

	field = scan->fields;
	for (unsigned int i = 0; i < scan->num_fields; i++, field++) {
		scan_size += field->num_bits;
		LOG_DEBUG_IO("%s%s field %u/%u %u bits",
			field->in_value ? "in" : "",
			field->out_value ? "out" : "",
			i, scan->num_fields, field->num_bits);

		if (i == scan->num_fields - 1 &&
				tap_get_state() != tap_get_end_state()) {
			uint8_t last_bit = 0;
			uint8_t tms_bits = 0x01;

			ret = jlinkdll_clock_data(field->out_value, 0, NULL, 0,
				field->in_value, 0, field->num_bits - 1);
			if (ret != ERROR_OK)
				return ret;

			if (field->out_value)
				bit_copy(&last_bit, 0, field->out_value,
					field->num_bits - 1, 1);

			ret = jlinkdll_clock_data(&last_bit, 0, &tms_bits, 0,
				field->in_value, field->num_bits - 1, 1);
			if (ret != ERROR_OK)
				return ret;
			tap_set_state(tap_state_transition(tap_get_state(), true));

			ret = jlinkdll_clock_data(NULL, 0, &tms_bits, 1, NULL, 0, 1);
			if (ret != ERROR_OK)
				return ret;
			tap_set_state(tap_state_transition(tap_get_state(), false));
		} else {
			ret = jlinkdll_clock_data(field->out_value, 0, NULL, 0,
				field->in_value, 0, field->num_bits);
			if (ret != ERROR_OK)
				return ret;
		}
	}

	if (tap_get_state() != tap_get_end_state()) {
		ret = jlinkdll_end_state(tap_get_end_state());
		if (ret != ERROR_OK)
			return ret;
		ret = jlinkdll_state_move();
		if (ret != ERROR_OK)
			return ret;
	}

	LOG_DEBUG_IO("%s scan, %u bits, end in %s",
		scan->ir_scan ? "IR" : "DR", scan_size,
		tap_state_name(tap_get_end_state()));

	return ERROR_OK;
}

static int jlinkdll_execute_tms(struct jtag_command *cmd)
{
	unsigned int num_bits = cmd->cmd.tms->num_bits;
	const uint8_t *bits = cmd->cmd.tms->bits;
	tap_state_t state = tap_get_state();
	int ret;

	LOG_DEBUG_IO("TMS: %u bits", num_bits);

	ret = jlinkdll_clock_data(NULL, 0, bits, 0, NULL, 0, num_bits);
	if (ret != ERROR_OK)
		return ret;

	for (unsigned int i = 0; i < num_bits; i++)
		state = tap_state_transition(state, buf_get_u32(bits + i / 8,
				i % 8, 1));

	tap_set_state(state);
	if (tap_is_state_stable(state))
		tap_set_end_state(state);

	return ERROR_OK;
}

static int jlinkdll_execute_command(struct jtag_command *cmd)
{
	int ret;

	switch (cmd->type) {
	case JTAG_STABLECLOCKS:
		LOG_DEBUG_IO("stableclocks %u cycles",
			cmd->cmd.stableclocks->num_cycles);
		return jlinkdll_stableclocks(cmd->cmd.stableclocks->num_cycles);
	case JTAG_RUNTEST:
		LOG_DEBUG_IO("runtest %u cycles, end in %s",
			cmd->cmd.runtest->num_cycles,
			tap_state_name(cmd->cmd.runtest->end_state));
		ret = jlinkdll_end_state(cmd->cmd.runtest->end_state);
		if (ret != ERROR_OK)
			return ret;
		return jlinkdll_runtest(cmd->cmd.runtest->num_cycles);
	case JTAG_TLR_RESET:
		LOG_DEBUG_IO("statemove end in %s",
			tap_state_name(cmd->cmd.statemove->end_state));
		ret = jlinkdll_end_state(cmd->cmd.statemove->end_state);
		if (ret != ERROR_OK)
			return ret;
		return jlinkdll_state_move();
	case JTAG_PATHMOVE:
		LOG_DEBUG_IO("pathmove: %u states, end in %s",
			cmd->cmd.pathmove->num_states,
			tap_state_name(cmd->cmd.pathmove->path[
				cmd->cmd.pathmove->num_states - 1]));
		return jlinkdll_path_move(cmd->cmd.pathmove->num_states,
			cmd->cmd.pathmove->path);
	case JTAG_SCAN:
		return jlinkdll_execute_scan(cmd);
	case JTAG_RESET:
		LOG_DEBUG_IO("reset trst: %i srst: %i", cmd->cmd.reset->trst,
			cmd->cmd.reset->srst);
		return ERROR_OK;
	case JTAG_SLEEP:
		LOG_DEBUG_IO("sleep %" PRIu32, cmd->cmd.sleep->us);
		ret = jlinkdll_flush();
		if (ret != ERROR_OK)
			return ret;
		jtag_sleep(cmd->cmd.sleep->us);
		return ERROR_OK;
	case JTAG_TMS:
		return jlinkdll_execute_tms(cmd);
	default:
		LOG_ERROR("BUG: Unknown JTAG command type encountered");
		return ERROR_JTAG_QUEUE_FAILED;
	}
}

static int jlinkdll_execute_queue(struct jtag_command *cmd_queue)
{
	struct jtag_command *cmd = cmd_queue;

	while (cmd) {
		int ret = jlinkdll_execute_command(cmd);
		if (ret != ERROR_OK)
			return ret;
		cmd = cmd->next;
	}

	return jlinkdll_flush();
}

static int jlinkdll_speed(int speed)
{
	if (jlinkdll_opened)
		jlinkdll.set_speed(speed);

	return ERROR_OK;
}

static int jlinkdll_khz(int khz, int *jtag_speed)
{
	*jtag_speed = khz;
	return ERROR_OK;
}

static int jlinkdll_speed_div(int speed, int *khz)
{
	*khz = speed;
	return ERROR_OK;
}

static int jlinkdll_reset(int trst, int srst)
{
	LOG_DEBUG("TRST: %i, SRST: %i", trst, srst);

	if (srst == 1) {
		if (!jlinkdll.clear_reset)
			return ERROR_JTAG_NOT_IMPLEMENTED;
		jlinkdll.clear_reset();
	} else if (srst == 0 && jlinkdll.set_reset) {
		jlinkdll.set_reset();
	}

	if (trst == 1) {
		if (!jlinkdll.clear_trst)
			return ERROR_JTAG_NOT_IMPLEMENTED;
		jlinkdll.clear_trst();
	} else if (trst == 0 && jlinkdll.set_trst) {
		jlinkdll.set_trst();
	}

	return jlinkdll_flush();
}

static int jlinkdll_read_memory(target_addr_t address, uint32_t size,
		uint8_t *buffer)
{
	int ret;

	if (!jlinkdll_opened || !jlinkdll.read_mem)
		return ERROR_JTAG_NOT_IMPLEMENTED;

	if (address > UINT32_MAX || size > UINT32_MAX - (uint32_t)address + 1)
		return ERROR_COMMAND_ARGUMENT_INVALID;

	ret = jlinkdll_flush();
	if (ret != ERROR_OK)
		return ret;

	LOG_DEBUG("J-Link DLL fast memory read: %" PRIu32 " bytes at 0x%08" PRIx32,
			size, (uint32_t)address);
	ret = jlinkdll.read_mem((uint32_t)address, size, buffer);
	if (ret < 0) {
		LOG_DEBUG("JLINKARM_ReadMem(0x%08" PRIx32 ", %" PRIu32 ") failed: %d",
				(uint32_t)address, size, ret);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	return ERROR_OK;
}

static int jlinkdll_write_memory(target_addr_t address, uint32_t size,
		const uint8_t *buffer)
{
	int ret;

	if (!jlinkdll_opened || !jlinkdll.write_mem)
		return ERROR_JTAG_NOT_IMPLEMENTED;

	if (address > UINT32_MAX || size > UINT32_MAX - (uint32_t)address + 1)
		return ERROR_COMMAND_ARGUMENT_INVALID;

	ret = jlinkdll_flush();
	if (ret != ERROR_OK)
		return ret;

	LOG_DEBUG("J-Link DLL fast memory write: %" PRIu32 " bytes at 0x%08" PRIx32,
			size, (uint32_t)address);
	ret = jlinkdll.write_mem((uint32_t)address, size, buffer);
	if (ret < 0) {
		LOG_DEBUG("JLINKARM_WriteMem(0x%08" PRIx32 ", %" PRIu32 ") failed: %d",
				(uint32_t)address, size, ret);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	return ERROR_OK;
}

static int jlinkdll_init(void)
{
	const char *open_error;
	int tif = jlinkdll_selected_tif();
	int ret;

	ret = jlinkdll_load_api();
	if (ret != ERROR_OK)
		return ret;

	if (tif == JLINKARM_TIF_CJTAG &&
			(!jlinkdll.jtag_store_bits || !jlinkdll_use_cjtag_highlevel())) {
		LOG_ERROR("J-Link DLL cJTAG support requires StoreBits, StoreInst, "
			"StoreData, StoreGetData, GetDeviceId, and GetU32 exports");
		ret = ERROR_JTAG_DEVICE_ERROR;
		goto fail;
	}

	ret = jlinkdll_select_serial();
	if (ret != ERROR_OK)
		goto fail;

	open_error = jlinkdll.openex(jlinkdll_log_handler, jlinkdll_error_handler);
	if (open_error) {
		LOG_ERROR("JLINKARM_OpenEx() failed: %s", open_error);
		ret = ERROR_JTAG_DEVICE_ERROR;
		goto fail;
	}
	jlinkdll_opened = true;

	if (jlinkdll.get_sn) {
		int sn = jlinkdll.get_sn();
		if (sn >= 0)
			LOG_INFO("J-Link serial: %d", sn);
	}

	if (jlinkdll_device) {
		char *cmd = alloc_printf("device = %s", jlinkdll_device);
		if (!cmd) {
			ret = ERROR_FAIL;
			goto fail;
		}
		ret = jlinkdll_exec_command(cmd);
		free(cmd);
		if (ret != ERROR_OK)
			goto fail;
	}

	ret = jlinkdll_run_config_commands();
	if (ret != ERROR_OK)
		goto fail;

	ret = jlinkdll.tif_select(tif);
	if (ret < 0) {
		LOG_ERROR("JLINKARM_TIF_Select(%s) failed: %d",
			jlinkdll_tif_name(tif), ret);
		ret = ERROR_JTAG_DEVICE_ERROR;
		goto fail;
	}
	LOG_INFO("J-Link target interface: %s", jlinkdll_tif_name(tif));

	jlinkdll.set_speed(adapter_get_speed_khz());

	ret = jlinkdll.jtag_enable_if();
	if (ret < 0) {
		if (tif != JLINKARM_TIF_CJTAG) {
			LOG_ERROR("JLINKARM_JTAG_EnableIF() failed: %d", ret);
			ret = ERROR_JTAG_DEVICE_ERROR;
			goto fail;
		}

		LOG_DEBUG("JLINKARM_JTAG_EnableIF() failed for cJTAG: %d; "
			"continuing after TIF_Select(cJTAG)", ret);
	} else {
		jlinkdll_jtag_if_enabled = true;
	}

	if (jlinkdll_connect_on_init) {
		if (!jlinkdll.connect) {
			LOG_ERROR("jlinkdll connect was requested, but J-Link DLL does "
				"not export JLINKARM_Connect");
			ret = ERROR_JTAG_NOT_IMPLEMENTED;
			goto fail;
		}

		LOG_INFO("Connecting to target through J-Link DLL");
		ret = jlinkdll.connect();
		LOG_INFO("JLINKARM_Connect() returned %d", ret);
		if (ret != 0) {
			LOG_ERROR("JLINKARM_Connect() failed: %d", ret);
			ret = ERROR_JTAG_DEVICE_ERROR;
			goto fail;
		}
	}

	tap_set_state(TAP_RESET);
	tap_set_end_state(TAP_RESET);
	return ERROR_OK;

fail:
	if (jlinkdll_opened) {
		jlinkdll.close();
		jlinkdll_opened = false;
	}
	jlinkdll_unload_api();
	return ret;
}

static int jlinkdll_quit(void)
{
	int ret = ERROR_OK;

	if (jlinkdll_opened && jlinkdll_jtag_if_enabled) {
		int dll_ret = jlinkdll.jtag_disable_if();
		if (dll_ret < 0) {
			LOG_ERROR("JLINKARM_JTAG_DisableIF() failed: %d", dll_ret);
			ret = ERROR_JTAG_DEVICE_ERROR;
		}
		jlinkdll_jtag_if_enabled = false;
	}

	if (jlinkdll_opened) {
		jlinkdll.close();
		jlinkdll_opened = false;
	}

	jlinkdll_unload_api();
	return ret;
}

COMMAND_HANDLER(jlinkdll_handle_path_command)
{
	if (CMD_ARGC > 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (CMD_ARGC == 1) {
		free(jlinkdll_path);
		jlinkdll_path = strdup(CMD_ARGV[0]);
		if (!jlinkdll_path)
			return ERROR_FAIL;
	}

	command_print(CMD, "J-Link DLL path: %s",
		jlinkdll_path ? jlinkdll_path : "<system default>");
	return ERROR_OK;
}

COMMAND_HANDLER(jlinkdll_handle_device_command)
{
	if (CMD_ARGC > 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (CMD_ARGC == 1) {
		free(jlinkdll_device);
		jlinkdll_device = strdup(CMD_ARGV[0]);
		if (!jlinkdll_device)
			return ERROR_FAIL;
	}

	command_print(CMD, "J-Link device: %s",
		jlinkdll_device ? jlinkdll_device : "<none>");
	return ERROR_OK;
}

COMMAND_HANDLER(jlinkdll_handle_connect_command)
{
	if (CMD_ARGC > 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (CMD_ARGC == 1)
		COMMAND_PARSE_ON_OFF(CMD_ARGV[0], jlinkdll_connect_on_init);

	command_print(CMD, "J-Link DLL connect on init: %s",
		jlinkdll_connect_on_init ? "on" : "off");
	return ERROR_OK;
}

COMMAND_HANDLER(jlinkdll_handle_exec_command)
{
	char *cmd_string;

	if (CMD_ARGC < 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	cmd_string = strdup(CMD_ARGV[0]);
	if (!cmd_string)
		return ERROR_FAIL;

	for (unsigned int i = 1; i < CMD_ARGC; i++) {
		char *joined = alloc_printf("%s %s", cmd_string, CMD_ARGV[i]);
		free(cmd_string);
		if (!joined)
			return ERROR_FAIL;
		cmd_string = joined;
	}

	if (jlinkdll_opened) {
		int ret = jlinkdll_exec_command(cmd_string);
		free(cmd_string);
		return ret;
	}

	int ret = jlinkdll_append_exec(cmd_string);
	free(cmd_string);
	return ret;
}

static const struct command_registration jlinkdll_subcommand_handlers[] = {
	{
		.name = "path",
		.handler = &jlinkdll_handle_path_command,
		.mode = COMMAND_CONFIG,
		.help = "display or set the path to JLink_x64.dll/JLinkARM.dll",
		.usage = "[dll_path]",
	},
	{
		.name = "device",
		.handler = &jlinkdll_handle_device_command,
		.mode = COMMAND_CONFIG,
		.help = "display or set a J-Link device name passed to the DLL",
		.usage = "[device_name]",
	},
	{
		.name = "connect",
		.handler = &jlinkdll_handle_connect_command,
		.mode = COMMAND_CONFIG,
		.help = "enable or disable JLINKARM_Connect() during init",
		.usage = "[on|off]",
	},
	{
		.name = "exec",
		.handler = &jlinkdll_handle_exec_command,
		.mode = COMMAND_ANY,
		.help = "queue or execute a raw J-Link DLL command string",
		.usage = "command_string",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration jlinkdll_command_handlers[] = {
	{
		.name = "jlinkdll",
		.mode = COMMAND_ANY,
		.help = "J-Link DLL command group",
		.usage = "",
		.chain = jlinkdll_subcommand_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

static struct jtag_interface jlinkdll_interface = {
	.execute_queue = &jlinkdll_execute_queue,
};

struct adapter_driver jlinkdll_adapter_driver = {
	.name = "jlinkdll",
	.transports = jlinkdll_transports,
	.commands = jlinkdll_command_handlers,
	.init = &jlinkdll_init,
	.quit = &jlinkdll_quit,
	.reset = &jlinkdll_reset,
	.speed = &jlinkdll_speed,
	.khz = &jlinkdll_khz,
	.speed_div = &jlinkdll_speed_div,
	.read_memory = &jlinkdll_read_memory,
	.write_memory = &jlinkdll_write_memory,
	.jtag_ops = &jlinkdll_interface,
};
