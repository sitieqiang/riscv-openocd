// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "rtos.h"
#include "helper/log.h"
#include "helper/types.h"
#include "target/register.h"
#include "target/riscv/riscv.h"
#include <helper/command.h>

#define LITEOS_THREAD_NAME_STR_SIZE 32
#define LITEOS_CURRENT_EXECUTION_ID 1
#define LITEOS_DEFAULT_TCB_SIZE 128
#define LITEOS_MAX_TASKS 1024

#define LITEOS_TASK_STATUS_UNUSED  0x0001
#define LITEOS_TASK_STATUS_SUSPEND 0x0002
#define LITEOS_TASK_STATUS_READY   0x0004
#define LITEOS_TASK_STATUS_PEND    0x0008
#define LITEOS_TASK_STATUS_RUNNING 0x0010
#define LITEOS_TASK_STATUS_DELAY   0x0020
#define LITEOS_TASK_STATUS_TIMEOUT 0x0040
#define LITEOS_TASK_STATUS_EXIT    0x0100

enum liteos_symbol_values {
	LITEOS_VAL_G_LOS_TASK = 0,
	LITEOS_VAL_G_TASK_CB_ARRAY,
	LITEOS_VAL_G_TASK_MAX_NUM,
	LITEOS_VAL_G_TASK_SCHEDULED,
};

struct symbols {
	const char *name;
	bool optional;
};

static const struct symbols liteos_symbol_list[] = {
	{ "g_losTask", false },
	{ "g_taskCBArray", false },
	{ "g_taskMaxNum", false },
	{ "g_taskScheduled", true },
	{ NULL, false }
};

static unsigned int liteos_tcb_size = LITEOS_DEFAULT_TCB_SIZE;

/*
 * Offsets for the LiteOS-M LosTaskCB layout used by the Nuclei RV32 ILP32 port:
 *
 *   stackPointer 0x00
 *   taskStatus   0x04
 *   priority     0x06
 *   stackSize    0x28
 *   topOfStack   0x2c
 *   taskID       0x30
 *   taskName     0x44
 *
 * The default TCB size is 0x80 for the referenced LiteOS source without
 * LOSCFG_KERNEL_SIGNAL or LOSCFG_TASK_STRUCT_EXTENSION. Use liteos_tcb_size
 * when local configuration adds tail fields to LosTaskCB.
 */
#define LITEOS_TCB_STACK_POINTER_OFFSET 0x00
#define LITEOS_TCB_TASK_STATUS_OFFSET   0x04
#define LITEOS_TCB_PRIORITY_OFFSET      0x06
#define LITEOS_TCB_TASK_ID_OFFSET       0x30
#define LITEOS_TCB_TASK_NAME_OFFSET     0x44

static const struct stack_register_offset liteos_nuclei_rv32_stack_offsets[] = {
	{ GDB_REGNO_ZERO, -1, 32 },
	{ GDB_REGNO_RA, 0x04, 32 },
	{ GDB_REGNO_SP, -2, 32 },
	{ GDB_REGNO_GP, -1, 32 },
	{ GDB_REGNO_TP, -1, 32 },
	{ GDB_REGNO_T0, 0x08, 32 },
	{ GDB_REGNO_T1, 0x0c, 32 },
	{ GDB_REGNO_T2, 0x10, 32 },
	{ GDB_REGNO_FP, 0x14, 32 },
	{ GDB_REGNO_S1, 0x18, 32 },
	{ GDB_REGNO_A0, 0x1c, 32 },
	{ GDB_REGNO_A1, 0x20, 32 },
	{ GDB_REGNO_A2, 0x24, 32 },
	{ GDB_REGNO_A3, 0x28, 32 },
	{ GDB_REGNO_A4, 0x2c, 32 },
	{ GDB_REGNO_A5, 0x30, 32 },
	{ GDB_REGNO_A6, 0x34, 32 },
	{ GDB_REGNO_A7, 0x38, 32 },
	{ GDB_REGNO_S2, 0x3c, 32 },
	{ GDB_REGNO_S3, 0x40, 32 },
	{ GDB_REGNO_S4, 0x44, 32 },
	{ GDB_REGNO_S5, 0x48, 32 },
	{ GDB_REGNO_S6, 0x4c, 32 },
	{ GDB_REGNO_S7, 0x50, 32 },
	{ GDB_REGNO_S8, 0x54, 32 },
	{ GDB_REGNO_S9, 0x58, 32 },
	{ GDB_REGNO_S10, 0x5c, 32 },
	{ GDB_REGNO_S11, 0x60, 32 },
	{ GDB_REGNO_T3, 0x64, 32 },
	{ GDB_REGNO_T4, 0x68, 32 },
	{ GDB_REGNO_T5, 0x6c, 32 },
	{ GDB_REGNO_T6, 0x70, 32 },
	{ GDB_REGNO_PC, 0x00, 32 },
	{ GDB_REGNO_MSTATUS, 0x74, 32 },
};

static const struct rtos_register_stacking liteos_nuclei_rv32_stacking = {
	.stack_registers_size = 30 * 4,
	.stack_growth_direction = -1,
	.num_output_registers = 33,
	.calculate_process_stack = NULL,
	.register_offsets = liteos_nuclei_rv32_stack_offsets,
	.total_register_count = ARRAY_SIZE(liteos_nuclei_rv32_stack_offsets),
};

static const struct stack_register_offset liteos_nuclei_rv32e_stack_offsets[] = {
	{ GDB_REGNO_ZERO, -1, 32 },
	{ GDB_REGNO_RA, 0x04, 32 },
	{ GDB_REGNO_SP, -2, 32 },
	{ GDB_REGNO_GP, -1, 32 },
	{ GDB_REGNO_TP, -1, 32 },
	{ GDB_REGNO_T0, 0x08, 32 },
	{ GDB_REGNO_T1, 0x0c, 32 },
	{ GDB_REGNO_T2, 0x10, 32 },
	{ GDB_REGNO_FP, 0x14, 32 },
	{ GDB_REGNO_S1, 0x18, 32 },
	{ GDB_REGNO_A0, 0x1c, 32 },
	{ GDB_REGNO_A1, 0x20, 32 },
	{ GDB_REGNO_A2, 0x24, 32 },
	{ GDB_REGNO_A3, 0x28, 32 },
	{ GDB_REGNO_A4, 0x2c, 32 },
	{ GDB_REGNO_A5, 0x30, 32 },
	{ GDB_REGNO_PC, 0x00, 32 },
	{ GDB_REGNO_MSTATUS, 0x34, 32 },
};

static const struct rtos_register_stacking liteos_nuclei_rv32e_stacking = {
	.stack_registers_size = 14 * 4,
	.stack_growth_direction = -1,
	.num_output_registers = 17,
	.calculate_process_stack = NULL,
	.register_offsets = liteos_nuclei_rv32e_stack_offsets,
	.total_register_count = ARRAY_SIZE(liteos_nuclei_rv32e_stack_offsets),
};

COMMAND_HANDLER(handle_liteos_tcb_size)
{
	if (CMD_ARGC != 1) {
		LOG_ERROR("Command takes exactly 1 parameter");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	unsigned int size;
	COMMAND_PARSE_NUMBER(uint, CMD_ARGV[0], size);
	if (size == 0 || size > 4096 || (size & 0x3)) {
		LOG_ERROR("Invalid LiteOS TCB size. Use a non-zero 4-byte aligned value up to 4096.");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	liteos_tcb_size = size;
	return ERROR_OK;
}

static const struct command_registration liteos_commands[] = {
	{
		.name = "liteos_tcb_size",
		.handler = handle_liteos_tcb_size,
		.mode = COMMAND_ANY,
		.usage = "bytes",
		.help = "Set sizeof(LosTaskCB) for LiteOS-M task enumeration. "
			"Defaults to 128 for the Nuclei RV32 LiteOS-M layout."
	},
	COMMAND_REGISTRATION_DONE
};

static bool liteos_target_is_rv32e(struct target *target)
{
	if (target->reg_cache) {
		struct reg *reg = register_get_by_number(target->reg_cache,
				GDB_REGNO_A6, true);
		if (reg && !reg->exist)
			return true;
	}

	return riscv_supports_extension(target, 'E');
}

static int liteos_read_ptr(struct rtos *rtos, target_addr_t address, target_addr_t *value)
{
	const unsigned int pointer_width = DIV_ROUND_UP(target_address_bits(rtos->target), 8);
	uint8_t buf[8] = { 0 };

	if (pointer_width != 4 && pointer_width != 8) {
		LOG_ERROR("LiteOS: unsupported pointer width %u", pointer_width);
		return ERROR_FAIL;
	}

	int retval = target_read_buffer(rtos->target, address, pointer_width, buf);
	if (retval != ERROR_OK)
		return retval;

	if (pointer_width == 4)
		*value = target_buffer_get_u32(rtos->target, buf);
	else
		*value = target_buffer_get_u64(rtos->target, buf);

	return ERROR_OK;
}

static char *liteos_read_string(struct rtos *rtos, target_addr_t address)
{
	if (address == 0)
		return strdup("No Name");

	char *str = calloc(LITEOS_THREAD_NAME_STR_SIZE + 1, sizeof(char));
	if (!str)
		return NULL;

	for (unsigned int i = 0; i < LITEOS_THREAD_NAME_STR_SIZE; i++) {
		uint8_t ch;
		int retval = target_read_buffer(rtos->target, address + i, 1, &ch);
		if (retval != ERROR_OK) {
			free(str);
			return strdup("No Name");
		}
		if (ch == '\0')
			break;
		str[i] = (char)ch;
	}

	if (str[0] == '\0')
		strcpy(str, "No Name");

	return str;
}

static const char *liteos_task_state(uint16_t status)
{
	if (status & LITEOS_TASK_STATUS_RUNNING)
		return "Running";
	if (status & LITEOS_TASK_STATUS_READY)
		return "Ready";
	if (status & LITEOS_TASK_STATUS_SUSPEND)
		return "Suspended";
	if (status & LITEOS_TASK_STATUS_DELAY)
		return "Delayed";
	if (status & LITEOS_TASK_STATUS_PEND)
		return "Pending";
	if (status & LITEOS_TASK_STATUS_TIMEOUT)
		return "Timeout";
	if (status & LITEOS_TASK_STATUS_EXIT)
		return "Exit";
	return "Unknown";
}

static int liteos_add_current_execution(struct rtos *rtos,
		struct thread_detail *details, unsigned int *tasks_found)
{
	struct thread_detail *thread = &details[*tasks_found];

	thread->threadid = LITEOS_CURRENT_EXECUTION_ID;
	thread->exists = true;
	thread->thread_name_str = strdup("Current Execution");
	if (!thread->thread_name_str)
		return ERROR_FAIL;
	thread->extra_info_str = NULL;

	rtos->current_thread = thread->threadid;
	(*tasks_found)++;
	return ERROR_OK;
}

static int liteos_update_threads(struct rtos *rtos)
{
	if (!rtos->symbols) {
		LOG_ERROR("No symbols for LiteOS");
		return ERROR_FAIL;
	}

	if (rtos->symbols[LITEOS_VAL_G_LOS_TASK].address == 0 ||
			rtos->symbols[LITEOS_VAL_G_TASK_CB_ARRAY].address == 0 ||
			rtos->symbols[LITEOS_VAL_G_TASK_MAX_NUM].address == 0) {
		LOG_ERROR("Missing mandatory LiteOS symbols");
		return ERROR_FAIL;
	}

	uint32_t max_tasks = 0;
	int retval = target_read_u32(rtos->target,
			rtos->symbols[LITEOS_VAL_G_TASK_MAX_NUM].address, &max_tasks);
	if (retval != ERROR_OK) {
		LOG_ERROR("LiteOS: failed to read g_taskMaxNum");
		return retval;
	}

	if (max_tasks > LITEOS_MAX_TASKS) {
		LOG_WARNING("LiteOS: g_taskMaxNum=%" PRIu32 " is too large, clamping to %u",
				max_tasks, LITEOS_MAX_TASKS);
		max_tasks = LITEOS_MAX_TASKS;
	}

	target_addr_t tcb_array = 0;
	retval = liteos_read_ptr(rtos, rtos->symbols[LITEOS_VAL_G_TASK_CB_ARRAY].address,
			&tcb_array);
	if (retval != ERROR_OK) {
		LOG_ERROR("LiteOS: failed to read g_taskCBArray");
		return retval;
	}

	target_addr_t current_tcb = 0;
	retval = liteos_read_ptr(rtos, rtos->symbols[LITEOS_VAL_G_LOS_TASK].address,
			&current_tcb);
	if (retval != ERROR_OK) {
		LOG_ERROR("LiteOS: failed to read g_losTask.runTask");
		return retval;
	}

	uint32_t task_scheduled = current_tcb ? 1 : 0;
	if (rtos->symbols[LITEOS_VAL_G_TASK_SCHEDULED].address != 0) {
		retval = target_read_u32(rtos->target,
				rtos->symbols[LITEOS_VAL_G_TASK_SCHEDULED].address,
				&task_scheduled);
		if (retval != ERROR_OK) {
			LOG_ERROR("LiteOS: failed to read g_taskScheduled");
			return retval;
		}
	}

	rtos_free_threadlist(rtos);

	const unsigned int thread_capacity = max_tasks + 1;
	struct thread_detail *details = calloc(thread_capacity, sizeof(struct thread_detail));
	if (!details)
		return ERROR_FAIL;

	unsigned int tasks_found = 0;
	if (!task_scheduled || current_tcb == 0 || tcb_array == 0) {
		retval = liteos_add_current_execution(rtos, details, &tasks_found);
		if (retval != ERROR_OK)
			goto error;
	}

	for (uint32_t i = 0; i < max_tasks && tasks_found < thread_capacity; i++) {
		const target_addr_t tcb = tcb_array + (target_addr_t)i * liteos_tcb_size;
		uint16_t status;

		retval = target_read_u16(rtos->target,
				tcb + LITEOS_TCB_TASK_STATUS_OFFSET, &status);
		if (retval != ERROR_OK) {
			LOG_ERROR("LiteOS: failed to read task status at " TARGET_ADDR_FMT,
					tcb + LITEOS_TCB_TASK_STATUS_OFFSET);
			goto error;
		}

		if (status & LITEOS_TASK_STATUS_UNUSED)
			continue;

		uint16_t priority = 0;
		uint32_t task_id = i;
		target_addr_t name_ptr = 0;

		retval = target_read_u16(rtos->target,
				tcb + LITEOS_TCB_PRIORITY_OFFSET, &priority);
		if (retval != ERROR_OK)
			goto error;

		retval = target_read_u32(rtos->target,
				tcb + LITEOS_TCB_TASK_ID_OFFSET, &task_id);
		if (retval != ERROR_OK)
			goto error;

		retval = liteos_read_ptr(rtos, tcb + LITEOS_TCB_TASK_NAME_OFFSET,
				&name_ptr);
		if (retval != ERROR_OK)
			goto error;

		struct thread_detail *thread = &details[tasks_found];
		thread->threadid = tcb;
		thread->exists = true;
		thread->thread_name_str = liteos_read_string(rtos, name_ptr);
		thread->extra_info_str = alloc_printf("State: %s, Priority: %" PRIu16
				", ID: %" PRIu32 ", Status: 0x%04" PRIx16,
				liteos_task_state(status), priority, task_id, status);
		if (!thread->thread_name_str || !thread->extra_info_str) {
			retval = ERROR_FAIL;
			goto error;
		}

		if (tcb == current_tcb)
			rtos->current_thread = thread->threadid;

		tasks_found++;
	}

	if (tasks_found == 0) {
		retval = liteos_add_current_execution(rtos, details, &tasks_found);
		if (retval != ERROR_OK)
			goto error;
	} else if (rtos->current_thread == 0) {
		rtos->current_thread = details[0].threadid;
	}

	rtos->thread_details = details;
	rtos->thread_count = tasks_found;
	return ERROR_OK;

error:
	for (unsigned int i = 0; i < tasks_found; i++) {
		free(details[i].thread_name_str);
		free(details[i].extra_info_str);
	}
	free(details);
	rtos->current_thread = 0;
	return retval;
}

static int liteos_get_stacking_info(struct rtos *rtos, threadid_t thread_id,
		const struct rtos_register_stacking **stacking_info,
		target_addr_t *stack_ptr)
{
	if (thread_id == 0 || thread_id == LITEOS_CURRENT_EXECUTION_ID)
		return ERROR_FAIL;

	const unsigned int pointer_width = DIV_ROUND_UP(target_address_bits(rtos->target), 8);
	if (pointer_width != 4) {
		LOG_ERROR("LiteOS: only RV32 Nuclei task contexts are supported");
		return ERROR_FAIL;
	}

	int retval = liteos_read_ptr(rtos,
			(target_addr_t)thread_id + LITEOS_TCB_STACK_POINTER_OFFSET,
			stack_ptr);
	if (retval != ERROR_OK) {
		LOG_ERROR("LiteOS: failed to read stack pointer for thread 0x%" PRIx64,
				(uint64_t)thread_id);
		return retval;
	}

	if (*stack_ptr == 0) {
		LOG_ERROR("LiteOS: null stack pointer for thread 0x%" PRIx64,
				(uint64_t)thread_id);
		return ERROR_FAIL;
	}

	if (liteos_target_is_rv32e(rtos->target))
		*stacking_info = &liteos_nuclei_rv32e_stacking;
	else
		*stacking_info = &liteos_nuclei_rv32_stacking;

	return ERROR_OK;
}

static int liteos_get_thread_reg_list(struct rtos *rtos, threadid_t thread_id,
		struct rtos_reg **reg_list, int *num_regs)
{
	if (thread_id == rtos->current_thread || thread_id == LITEOS_CURRENT_EXECUTION_ID)
		return ERROR_FAIL;

	const struct rtos_register_stacking *stacking_info;
	target_addr_t stack_ptr;
	int retval = liteos_get_stacking_info(rtos, thread_id, &stacking_info,
			&stack_ptr);
	if (retval != ERROR_OK)
		return retval;

	return rtos_generic_stack_read(rtos->target, stacking_info, stack_ptr,
			reg_list, num_regs);
}

static int liteos_set_reg(struct rtos *rtos, uint32_t reg_num, uint8_t *reg_value)
{
	if (rtos->current_threadid == rtos->current_thread ||
			rtos->current_threadid == LITEOS_CURRENT_EXECUTION_ID)
		return ERROR_FAIL;

	const struct rtos_register_stacking *stacking_info;
	target_addr_t stack_ptr;
	int retval = liteos_get_stacking_info(rtos, rtos->current_threadid,
			&stacking_info, &stack_ptr);
	if (retval != ERROR_OK)
		return retval;

	const unsigned int total_count =
		MAX(stacking_info->total_register_count, stacking_info->num_output_registers);

	for (unsigned int i = 0; i < total_count; i++) {
		const struct stack_register_offset *offset = &stacking_info->register_offsets[i];
		if (offset->number != reg_num)
			continue;

		if (offset->offset == -1 || offset->offset == -2) {
			LOG_DEBUG("LiteOS: register %" PRIu32 " is not directly writable in the task context",
					reg_num);
			return ERROR_OK;
		}

		return target_write_buffer(rtos->target, stack_ptr + offset->offset,
				DIV_ROUND_UP(offset->width_bits, 8), reg_value);
	}

	return ERROR_FAIL;
}

static bool liteos_detect_rtos(struct target *target)
{
	if (target->rtos->symbols &&
			target->rtos->symbols[LITEOS_VAL_G_LOS_TASK].address != 0 &&
			target->rtos->symbols[LITEOS_VAL_G_TASK_CB_ARRAY].address != 0 &&
			target->rtos->symbols[LITEOS_VAL_G_TASK_MAX_NUM].address != 0)
		return true;

	return false;
}

static int liteos_create(struct target *target)
{
	if (strcmp(target_type_name(target), "riscv") != 0) {
		LOG_ERROR("LiteOS support currently requires a RISC-V target");
		return ERROR_FAIL;
	}

	return register_commands(target->rtos->cmd_ctx, NULL, liteos_commands);
}

static int liteos_get_symbol_list_to_lookup(struct symbol_table_elem *symbol_list[])
{
	*symbol_list = calloc(ARRAY_SIZE(liteos_symbol_list),
			sizeof(struct symbol_table_elem));
	if (!*symbol_list)
		return ERROR_FAIL;

	for (unsigned int i = 0; i < ARRAY_SIZE(liteos_symbol_list); i++) {
		(*symbol_list)[i].symbol_name = liteos_symbol_list[i].name;
		(*symbol_list)[i].optional = liteos_symbol_list[i].optional;
	}

	return ERROR_OK;
}

const struct rtos_type liteos_rtos = {
	.name = "liteos",
	.detect_rtos = liteos_detect_rtos,
	.create = liteos_create,
	.update_threads = liteos_update_threads,
	.get_thread_reg_list = liteos_get_thread_reg_list,
	.set_reg = liteos_set_reg,
	.get_symbol_list_to_lookup = liteos_get_symbol_list_to_lookup,
};
