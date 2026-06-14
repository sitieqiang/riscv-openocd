/***************************************************************************
 *   Copyright (C) 2010 by Antonio Borneo <borneo.antonio@gmail.com>       *
 *   Modified by Yanwen Wang <wangyanwen@nucleisys.com> based on fespi.c   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include "imp.h"
#include "spi.h"
#include <jtag/jtag.h>
#include <helper/time_support.h>
#include <target/algorithm.h>
#include "target/riscv/riscv.h"
#include <helper/configuration.h>

#define ERASE_CMD			(1)
#define WRITE_CMD			(2)
#define READ_CMD			(3)
#define PROBE_CMD			(4)
#define CUSTOM_DEFAULT_TRANSFER_CHUNK	(256 * 1024)

enum custom_loader_type {
	CUSTOM_LOADER_FILE,
	CUSTOM_LOADER_GATESEA_QSPI,
};

struct flash_bank_msg {
	bool probed;
	const struct flash_device *dev;
	target_addr_t ctrl_base;
	char *loader_path;
	enum custom_loader_type loader_type;
	uint8_t cs;
	uint8_t *buffer;
	uint32_t param_0;
	uint32_t param_1;
	bool simulation;
	uint32_t sectorsize;
	uint32_t chunksize;
};

static const uint8_t gatesea_qspi_riscv32_bin[] = {
#include "../../../contrib/loaders/flash/gatesea_qspi/riscv32_gatesea_qspi.inc"
};

static const uint8_t gatesea_qspi_riscv64_bin[] = {
#include "../../../contrib/loaders/flash/gatesea_qspi/riscv64_gatesea_qspi.inc"
};

static const struct flash_device *custom_find_flash_device(uint32_t id)
{
	for (const struct flash_device *p = flash_devices; p->name; p++) {
		if (p->device_id == id)
			return p;
	}
	return NULL;
}

static int custom_run_algorithm(struct flash_bank *bank)
{
	struct flash_bank_msg *bank_msg = bank->driver_priv;
	struct target *target = bank->target;
	int retval = ERROR_OK;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	int xlen = riscv_xlen(target);
	struct working_area *algorithm_wa = NULL;
	struct working_area *data_wa = NULL;
	uint8_t *bin_alloc = NULL;
	const uint8_t *bin = NULL;
	size_t bin_size = 0;

	if (bank_msg->loader_type == CUSTOM_LOADER_GATESEA_QSPI) {
		if (xlen == 32) {
			bin = gatesea_qspi_riscv32_bin;
			bin_size = sizeof(gatesea_qspi_riscv32_bin);
		} else {
			bin = gatesea_qspi_riscv64_bin;
			bin_size = sizeof(gatesea_qspi_riscv64_bin);
		}
		LOG_INFO("Using built-in gatesea_qspi custom flashloader");
	} else {
		FILE *fd = fopen((char *)bank_msg->loader_path, "rb");
		if (fd == NULL) {
			LOG_INFO("Try to find custom flashloader %s in openocd configuration search dirs.",
					(char *)bank_msg->loader_path);
			char *full_path = find_file((char *)bank_msg->loader_path);
			if (full_path) {
				fd = fopen(full_path, "rb");
				LOG_INFO("Using custom flashloader %s found in openocd configuration search dirs.",
						full_path);
				free(full_path);
			} else {
				LOG_ERROR("Unable to find flashloader %s in openocd configuration search dirs.",
						(char *)bank_msg->loader_path);
				retval = ERROR_FAIL;
				goto err;
			}
		} else {
			LOG_INFO("Using custom flashloader %s", bank_msg->loader_path);
		}

		if (fseek(fd, 0, SEEK_END) != 0) {
			LOG_ERROR("seek loader error");
			fclose(fd);
			retval = ERROR_FAIL;
			goto err;
		}
		long file_size = ftell(fd);
		if (file_size < 0) {
			LOG_ERROR("tell loader size error");
			fclose(fd);
			retval = ERROR_FAIL;
			goto err;
		}
		bin_size = (size_t)file_size;
		rewind(fd);
		bin_alloc = malloc(bin_size);
		if (!bin_alloc) {
			LOG_ERROR("not enough memory");
			fclose(fd);
			retval = ERROR_FAIL;
			goto err;
		}
		if (fread(bin_alloc, 1, bin_size, fd) != bin_size) {
			LOG_ERROR("read loader error");
			fclose(fd);
			retval = ERROR_FAIL;
			goto err;
		}
		fclose(fd);
		bin = bin_alloc;
	}

	if (target->working_area_size < bin_size) {
		LOG_ERROR("working_area_size less than loader_bin_size");
		retval = ERROR_FAIL;
		goto err;
	}

	unsigned data_wa_size = 0;
	if (target_alloc_working_area(target, bin_size, &algorithm_wa) == ERROR_OK) {
		LOG_OUTPUT("Flash loader upload started: %zu bytes to " TARGET_ADDR_FMT "\n",
				bin_size, algorithm_wa->address);
		retval = target_write_buffer(target, algorithm_wa->address, bin_size, bin);
		if (retval != ERROR_OK) {
			LOG_ERROR("Failed to write code to " TARGET_ADDR_FMT ": %d",
					algorithm_wa->address, retval);
			target_free_working_area(target, algorithm_wa);
			algorithm_wa = NULL;
		} else {
			LOG_OUTPUT("Flash loader upload completed\n");
			if ((bank_msg->cs == WRITE_CMD) || (bank_msg->cs == READ_CMD)) {
				data_wa_size = MIN(target->working_area_size - algorithm_wa->size, bank_msg->param_0);
				data_wa_size = MIN(data_wa_size, bank_msg->chunksize);
				if (data_wa_size == 0) {
					LOG_ERROR("no working area available for custom flash data buffer");
					retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
					goto err;
				}
				LOG_OUTPUT("Flash data buffer allocation started: %" PRIu32 " bytes\n",
						data_wa_size);
				while (1) {
					if (target_alloc_working_area_try(target, data_wa_size, &data_wa) == ERROR_OK)
						break;
					data_wa_size = data_wa_size * 3 / 4;
					if (data_wa_size == 0) {
						LOG_ERROR("no working area available for custom flash data buffer");
						retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
						goto err;
					}
				}
				LOG_OUTPUT("Flash data buffer allocated: %" PRIu32 " bytes at " TARGET_ADDR_FMT "\n",
						data_wa_size, data_wa->address);
			}
		}
	} else {
		LOG_WARNING("Couldn't allocate %zd-byte working area.", bin_size);
		algorithm_wa = NULL;
		retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	if (algorithm_wa) {
		uint32_t count = 0;
		uint32_t offset = 0;
		uint32_t first_addr = 0;
		uint32_t end_addr = 0;
		uint32_t cur_count = 0;
		int algorithm_result = 0;
		struct reg_param reg_params[5];
		init_reg_param(&reg_params[0], "a0", xlen, PARAM_IN_OUT);
		init_reg_param(&reg_params[1], "a1", xlen, PARAM_OUT);
		init_reg_param(&reg_params[2], "a2", xlen, PARAM_OUT);
		init_reg_param(&reg_params[3], "a3", xlen, PARAM_OUT);
		init_reg_param(&reg_params[4], "a4", xlen, PARAM_OUT);
		switch (bank_msg->cs)
		{
		case ERASE_CMD:
			first_addr = bank_msg->param_0;
			end_addr = bank_msg->param_1;
			LOG_OUTPUT("Flash erase started: offset 0x%08" PRIx32 " .. 0x%08" PRIx32
					" (%" PRIu32 " bytes)\n",
					first_addr, end_addr, end_addr - first_addr);
			buf_set_u64(reg_params[0].value, 0, xlen, bank_msg->cs);
			buf_set_u64(reg_params[1].value, 0, xlen, bank_msg->ctrl_base);
			buf_set_u64(reg_params[2].value, 0, xlen, first_addr);
			buf_set_u64(reg_params[3].value, 0, xlen, end_addr);
			buf_set_u64(reg_params[4].value, 0, xlen, 0);
			if (bank_msg->simulation) {
				retval = target_run_algorithm(target, 0, NULL,
						ARRAY_SIZE(reg_params), reg_params,
						algorithm_wa->address, 0, 0x7FFFFFFF, NULL);
			} else {
				retval = target_run_algorithm(target, 0, NULL,
						ARRAY_SIZE(reg_params), reg_params,
						algorithm_wa->address, 0, (end_addr - first_addr) * 2, NULL);
			}
			if (retval != ERROR_OK) {
				LOG_ERROR("Failed to execute algorithm at " TARGET_ADDR_FMT ": %d",
						algorithm_wa->address, retval);
				goto err;
			}
			algorithm_result = buf_get_u64(reg_params[0].value, 0, xlen);
			if (algorithm_result != 0) {
				LOG_ERROR("Algorithm returned error %d", algorithm_result);
				LOG_ERROR("erase command error");
				retval = ERROR_FAIL;
				goto err;
			}
			LOG_OUTPUT("Flash erase completed: offset 0x%08" PRIx32 " .. 0x%08" PRIx32 "\n",
					first_addr, end_addr);
			break;
		case WRITE_CMD:
			count = bank_msg->param_0;
			offset = bank_msg->param_1;
			cur_count = 0;
			uint32_t total_count = count;
			uint32_t written_count = 0;
			uint32_t last_progress = UINT32_MAX;
			if (total_count > 0) {
				last_progress = 0;
				LOG_OUTPUT("Flash write started: %" PRIu32 " bytes, chunk %" PRIu32 " bytes\n",
						total_count, data_wa_size);
				LOG_OUTPUT("Flash write progress: 0%% (0/%" PRIu32 " bytes)\n", total_count);
			}
			while (count > 0) {
				cur_count = MIN(count, data_wa_size);
				buf_set_u64(reg_params[0].value, 0, xlen, bank_msg->cs);
				buf_set_u64(reg_params[1].value, 0, xlen, bank_msg->ctrl_base);
				buf_set_u64(reg_params[2].value, 0, xlen, data_wa->address);
				buf_set_u64(reg_params[3].value, 0, xlen, offset);
				buf_set_u64(reg_params[4].value, 0, xlen, cur_count);
				retval = target_write_buffer(target, data_wa->address, cur_count, bank_msg->buffer);
				if (retval != ERROR_OK) {
					LOG_DEBUG("Failed to write %d bytes to " TARGET_ADDR_FMT ": %d",
							cur_count, data_wa->address, retval);
					goto err;
				}
				if (bank_msg->simulation) {
					retval = target_run_algorithm(target, 0, NULL,
						ARRAY_SIZE(reg_params), reg_params,
						algorithm_wa->address, 0, 0x7FFFFFFF, NULL);
				} else {
					retval = target_run_algorithm(target, 0, NULL,
							ARRAY_SIZE(reg_params), reg_params,
							algorithm_wa->address, 0, cur_count * 2, NULL);
				}
				if (retval != ERROR_OK) {
					LOG_ERROR("Failed to execute algorithm at " TARGET_ADDR_FMT ": %d",
							algorithm_wa->address, retval);
					goto err;
				}
				algorithm_result = buf_get_u64(reg_params[0].value, 0, xlen);
				if (algorithm_result != 0) {
					LOG_ERROR("Algorithm returned error %d", algorithm_result);
					LOG_ERROR("write command error");
					retval = ERROR_FAIL;
					goto err;
				}
				bank_msg->buffer += cur_count;
				offset += cur_count;
				count -= cur_count;
				written_count += cur_count;
				if (total_count > 0) {
					uint32_t progress = (uint32_t)((uint64_t)written_count * 100 / total_count);
					if (progress == 100 || progress >= last_progress + 5) {
						LOG_OUTPUT("Flash write progress: %" PRIu32 "%% (%" PRIu32 "/%" PRIu32 " bytes)\n",
								progress, written_count, total_count);
						last_progress = progress;
					}
				}
			}
			break;
		case READ_CMD:
			count = bank_msg->param_0;
			offset = bank_msg->param_1;
			cur_count = 0;
			uint32_t read_total_count = count;
			uint32_t read_done_count = 0;
			uint32_t read_last_progress = UINT32_MAX;
			if (read_total_count > 0) {
				read_last_progress = 0;
				LOG_OUTPUT("Flash read started: %" PRIu32 " bytes, chunk %" PRIu32 " bytes\n",
						read_total_count, data_wa_size);
				LOG_OUTPUT("Flash read progress: 0%% (0/%" PRIu32 " bytes)\n", read_total_count);
			}
			while (count > 0) {
				cur_count = MIN(count, data_wa_size);
				buf_set_u64(reg_params[0].value, 0, xlen, bank_msg->cs);
				buf_set_u64(reg_params[1].value, 0, xlen, bank_msg->ctrl_base);
				buf_set_u64(reg_params[2].value, 0, xlen, data_wa->address);
				buf_set_u64(reg_params[3].value, 0, xlen, offset);
				buf_set_u64(reg_params[4].value, 0, xlen, cur_count);
				if (bank_msg->simulation) {
					retval = target_run_algorithm(target, 0, NULL,
						ARRAY_SIZE(reg_params), reg_params,
						algorithm_wa->address, 0, 0x7FFFFFFF, NULL);
				} else {
					retval = target_run_algorithm(target, 0, NULL,
							ARRAY_SIZE(reg_params), reg_params,
							algorithm_wa->address, 0, cur_count * 2, NULL);
				}
				if (retval != ERROR_OK) {
					LOG_ERROR("Failed to execute algorithm at " TARGET_ADDR_FMT ": %d",
							algorithm_wa->address, retval);
					goto err;
				}
				algorithm_result = buf_get_u64(reg_params[0].value, 0, xlen);
				if (algorithm_result != 0) {
					LOG_ERROR("Algorithm returned error %d", algorithm_result);
					LOG_ERROR("read command error");
					retval = ERROR_FAIL;
					goto err;
				}
				retval = target_read_buffer(target, data_wa->address, cur_count, bank_msg->buffer);
				if (retval != ERROR_OK) {
					LOG_DEBUG("Failed to read %d bytes from " TARGET_ADDR_FMT ": %d",
							cur_count, data_wa->address, retval);
					goto err;
				}
				bank_msg->buffer += cur_count;
				offset += cur_count;
				count -= cur_count;
				read_done_count += cur_count;
				if (read_total_count > 0) {
					uint32_t progress = (uint32_t)((uint64_t)read_done_count * 100 / read_total_count);
					if (progress == 100 || progress >= read_last_progress + 5) {
						LOG_OUTPUT("Flash read progress: %" PRIu32 "%% (%" PRIu32 "/%" PRIu32 " bytes)\n",
								progress, read_done_count, read_total_count);
						read_last_progress = progress;
					}
				}
			}
			break;
		case PROBE_CMD:
			buf_set_u64(reg_params[0].value, 0, xlen, bank_msg->cs);
			buf_set_u64(reg_params[1].value, 0, xlen, bank_msg->ctrl_base);
			buf_set_u64(reg_params[2].value, 0, xlen, 0);
			buf_set_u64(reg_params[3].value, 0, xlen, 0);
			buf_set_u64(reg_params[4].value, 0, xlen, 0);
			if (bank_msg->simulation) {
				retval = target_run_algorithm(target, 0, NULL,
					ARRAY_SIZE(reg_params), reg_params,
					algorithm_wa->address, 0, 0x7FFFFFFF, NULL);
			} else {
				retval = target_run_algorithm(target, 0, NULL,
						ARRAY_SIZE(reg_params), reg_params,
						algorithm_wa->address, 0, 10000, NULL);
			}
			if (retval != ERROR_OK) {
				LOG_ERROR("Failed to execute algorithm at " TARGET_ADDR_FMT ": %d",
						algorithm_wa->address, retval);
				goto err;
			}
			algorithm_result = buf_get_u64(reg_params[0].value, 0, xlen);
			retval = algorithm_result;
			break;
		default:
			break;
		}
		target_free_working_area(target, data_wa);
		target_free_working_area(target, algorithm_wa);
	}

err:
	free(bin_alloc);
	if (algorithm_wa) {
		target_free_working_area(target, data_wa);
		target_free_working_area(target, algorithm_wa);
	}
	return retval;
}

FLASH_BANK_COMMAND_HANDLER(custom_flash_bank_command)
{
	struct flash_bank_msg *bank_msg;

	LOG_DEBUG("%s", __func__);

	if (CMD_ARGC < 8) {
		LOG_ERROR("Parameter error:");
		LOG_ERROR("flash bank $FLASHNAME custom 0x20000000 0 0 0 $TARGETNAME 0x10014000 ~/work/riscv.bin [simulation] [sectorsize=]");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	bank_msg = malloc(sizeof(struct flash_bank_msg));
	if (bank_msg == NULL) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}

	bank->driver_priv = bank_msg;
	bank_msg->probed = false;
	bank_msg->ctrl_base = 0;
	bank_msg->loader_path = NULL;
	bank_msg->loader_type = CUSTOM_LOADER_FILE;
	bank_msg->cs = 0;
	bank_msg->buffer = NULL;
	bank_msg->param_0 = 0;
	bank_msg->param_1 = 0;

	COMMAND_PARSE_ADDRESS(CMD_ARGV[6], bank_msg->ctrl_base);
	LOG_DEBUG("ASSUMING CUSTOM device at ctrl_base = " TARGET_ADDR_FMT,
			bank_msg->ctrl_base);
	bank_msg->loader_path = malloc(strlen(CMD_ARGV[7]) + 1);
	if (!bank_msg->loader_path) {
		free(bank_msg);
		return ERROR_FAIL;
	}
	strcpy((char*)bank_msg->loader_path, CMD_ARGV[7]);
	for (char *p = bank_msg->loader_path; *p; p++) {
		if (*p == '\\')
			*p = '/';
	}
	if (strcmp(bank_msg->loader_path, "gatesea_qspi") == 0 ||
			strcmp(bank_msg->loader_path, "builtin:gatesea_qspi") == 0) {
		bank_msg->loader_type = CUSTOM_LOADER_GATESEA_QSPI;
	}
	bank_msg->simulation = false;
	bank_msg->sectorsize = 0;
	bank_msg->chunksize = CUSTOM_DEFAULT_TRANSFER_CHUNK;
	for (unsigned int i = 8; i < CMD_ARGC; i++) {
		if(strcmp(CMD_ARGV[i], "simulation") == 0) {
			bank_msg->simulation = true;
			LOG_DEBUG("Custom Simulation Mode");
		}
		if(strncmp(CMD_ARGV[i], "sectorsize=", strlen("sectorsize=")) == 0) {
			COMMAND_PARSE_NUMBER(u32, CMD_ARGV[i]+strlen("sectorsize="), bank_msg->sectorsize);
			LOG_DEBUG("Custom flash sectorsize is %x", bank_msg->sectorsize);
		}
		if(strncmp(CMD_ARGV[i], "chunksize=", strlen("chunksize=")) == 0) {
			COMMAND_PARSE_NUMBER(u32, CMD_ARGV[i]+strlen("chunksize="), bank_msg->chunksize);
			LOG_DEBUG("Custom flash transfer chunk size is %x", bank_msg->chunksize);
		}
	}
	if (bank_msg->chunksize == 0) {
		LOG_ERROR("custom flash chunksize must be greater than zero");
		free(bank_msg->loader_path);
		free(bank_msg);
		return ERROR_COMMAND_ARGUMENT_INVALID;
	}

	return ERROR_OK;
}

static int custom_erase(struct flash_bank *bank, unsigned int first,
		unsigned int last)
{
	struct flash_bank_msg *bank_msg = bank->driver_priv;

	bank_msg->cs = ERASE_CMD;
	bank_msg->buffer = NULL;
	bank_msg->param_0 = bank->sectors[first].offset;
	bank_msg->param_1 = bank->sectors[last].offset + bank->sectors[last].size;

	return custom_run_algorithm(bank);
}

static int custom_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	struct flash_bank_msg *bank_msg = bank->driver_priv;

	bank_msg->cs = WRITE_CMD;
	bank_msg->buffer = (uint8_t*)buffer;
	bank_msg->param_0 = count;
	bank_msg->param_1 = offset;

	return custom_run_algorithm(bank);
}

static int custom_read(struct flash_bank *bank, uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	struct flash_bank_msg *bank_msg = bank->driver_priv;

	bank_msg->cs = READ_CMD;
	bank_msg->buffer = buffer;
	bank_msg->param_0 = count;
	bank_msg->param_1 = offset;

	return custom_run_algorithm(bank);
}

static int custom_verify(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	uint8_t *readback;
	unsigned int diffs = 0;

	if (count == 0)
		return ERROR_OK;

	readback = malloc(count);
	if (!readback)
		return ERROR_FAIL;

	LOG_OUTPUT("Flash verify started: %" PRIu32 " bytes\n", count);

	int retval = custom_read(bank, readback, offset, count);
	if (retval != ERROR_OK) {
		free(readback);
		return retval;
	}

	for (uint32_t i = 0; i < count; i++) {
		if (readback[i] == buffer[i])
			continue;

		if (diffs < 128) {
			LOG_ERROR("diff %u address " TARGET_ADDR_FMT
					". Was 0x%02" PRIx8 " instead of 0x%02" PRIx8,
					diffs,
					bank->base + offset + i,
					readback[i],
					buffer[i]);
		} else if (diffs == 128) {
			LOG_ERROR("More than 128 errors, the rest are not printed.");
		}
		diffs++;
	}

	free(readback);

	if (diffs > 0) {
		LOG_ERROR("Flash verify failed: %u differences found", diffs);
		return ERROR_FAIL;
	}

	LOG_OUTPUT("Flash verify completed: %" PRIu32 " bytes\n", count);
	return ERROR_OK;
}

static int custom_probe(struct flash_bank *bank)
{
	struct flash_bank_msg *bank_msg = bank->driver_priv;

	uint32_t id;
	struct flash_sector *sectors;

	if (bank_msg->probed)
		free(bank->sectors);
	bank_msg->probed = false;

	bank_msg->cs = PROBE_CMD;
	bank_msg->buffer = NULL;
	bank_msg->param_0 = 0;
	bank_msg->param_1 = 0;

	id = custom_run_algorithm(bank);
	if ((int)id < 0) {
		LOG_ERROR("custom flash probe failed");
		return ERROR_FAIL;
	}

	bank_msg->dev = custom_find_flash_device(id);
	if (bank_msg->dev) {
		LOG_INFO("Found custom flash device '%s' (ID 0x%08" PRIx32 ")",
				bank_msg->dev->name, bank_msg->dev->device_id);
		if (bank->size == 0)
			bank->size = bank_msg->dev->size_in_bytes;
	} else {
		LOG_WARNING("Unknown custom flash device (ID 0x%08" PRIx32
				"), using configured geometry", id);
		bank_msg->dev = custom_find_flash_device(0x12345678);
		if (bank->size == 0 && bank_msg->dev)
			bank->size = bank_msg->dev->size_in_bytes;
	}

	if (bank->size == 0) {
		LOG_ERROR("custom flash size is unknown; specify flash bank size");
		return ERROR_FAIL;
	}

	/* if no sectors, treat whole bank as single sector */
	if (0 == bank_msg->sectorsize) {
		if (bank_msg->dev && bank_msg->dev->sectorsize)
			bank_msg->sectorsize = bank_msg->dev->sectorsize;
		else
			bank_msg->sectorsize = bank->size;
	}
	if (bank_msg->sectorsize == 0) {
		LOG_ERROR("custom flash sector size is unknown");
		return ERROR_FAIL;
	}
	/* create and fill sectors array */
	bank->num_sectors = (bank->size + bank_msg->sectorsize - 1) / bank_msg->sectorsize;
	sectors = malloc(sizeof(struct flash_sector) * bank->num_sectors);
	if (sectors == NULL) {
		LOG_ERROR("not enough memory");
		return ERROR_FAIL;
	}
	for (unsigned int sector = 0; sector < bank->num_sectors; sector++) {
		sectors[sector].offset = sector * bank_msg->sectorsize;
		uint32_t remaining = bank->size - sectors[sector].offset;
		sectors[sector].size = MIN(bank_msg->sectorsize, remaining);
		sectors[sector].is_erased = -1;
		sectors[sector].is_protected = 0;
	}
	bank->sectors = sectors;
	bank_msg->probed = true;
	return ERROR_OK;
}

static int custom_info(struct flash_bank *bank, struct command_invocation *command)
{
	struct flash_bank_msg *bank_msg = bank->driver_priv;

	if (!(bank_msg->probed)) {
		return ERROR_OK;
	}

	return ERROR_OK;
}

static int custom_auto_probe(struct flash_bank *bank)
{
	struct flash_bank_msg *bank_msg = bank->driver_priv;
	if (bank_msg->probed)
		return ERROR_OK;
	return custom_probe(bank);
}

static int custom_protect(struct flash_bank *bank, int set,
		unsigned int first, unsigned int last)
{
	for (unsigned int sector = first; sector <= last; sector++)
		bank->sectors[sector].is_protected = set;
	return ERROR_OK;
}

static int custom_protect_check(struct flash_bank *bank)
{
	/* Nothing to do. Protection is only handled in SW. */
	return ERROR_OK;
}

const struct flash_driver custom_flash = {
	.name = "custom",
	.flash_bank_command = custom_flash_bank_command,
	.erase = custom_erase,
	.protect = custom_protect,
	.write = custom_write,
	.read = custom_read,
	.verify = custom_verify,
	.probe = custom_probe,
	.auto_probe = custom_auto_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = custom_protect_check,
	.info = custom_info,
	.free_driver_priv = default_flash_free_driver_priv
};
