#include "operator.h"

#include <stdlib.h>
#include <stddef.h>
#include <endian.h>

#include <snap_tools.h>
#include <libsnap.h>
#include <snap_hls_if.h>

#include "../../metal_fpga/include/action_metalfpga.h"
#include "../../metal/metal.h"

static const void* handle_opts(mtl_operator_invocation_args *args, uint64_t *length, bool *valid) {
    *length = 0;
    *valid = true;
    return "";
}

static const char* get_filename() {
    return NULL;
}

static uint64_t block_size = 4096; // TODO: Source this from a constant
static uint64_t dram_baseaddr = 0x00;

uint64_t _offset;
uint64_t _length;

static int apply_config(struct snap_action *action) {
    // Copy file contents to DRAM and point the DRAM read_mem operator to the right address

    uint64_t block_offset = _offset / block_size;
    uint64_t end_block_offset = (_offset + _length) / block_size;
    uint64_t blocks_length = end_block_offset - block_offset + 1;

    // printf("Mounting %lu blocks starting from %lu to addr %lx\n", blocks_length, block_offset, dram_baseaddr);

    uint64_t *job_struct = (uint64_t*)snap_malloc(4 * sizeof(uint64_t));
    job_struct[0] = htobe64(0); // Slot
    job_struct[1] = htobe64(block_offset); // File offset
    job_struct[2] = htobe64(dram_baseaddr); // DRAM target address
    job_struct[3] = htobe64(blocks_length); // number of blocks to load

    metalfpga_job_t mjob;
    mjob.job_type = MF_JOB_MOUNT;
    mjob.job_address = (uint64_t)job_struct;

    struct snap_job cjob;
    snap_job_set(&cjob, &mjob, sizeof(mjob), NULL, 0);

    const unsigned long timeout = 10;
    int rc = snap_action_sync_execute_job(action, &cjob, timeout);

    if (rc != 0)
        // Some error occurred
        return MTL_ERROR_INVALID_ARGUMENT;

    if (cjob.retc != SNAP_RETC_SUCCESS)
        // Some error occurred
        return MTL_ERROR_INVALID_ARGUMENT;

    free(job_struct);

    job_struct = (uint64_t*)snap_malloc(2 * sizeof(uint64_t));
    job_struct[0] = htobe64(dram_baseaddr + (_offset % block_size));
    job_struct[1] = htobe64(_length);

    mjob.job_type = MF_JOB_AFU_MEM_SET_DRAM_READ_BUFFER;
    mjob.job_address = (uint64_t)job_struct;

    snap_job_set(&cjob, &mjob, sizeof(mjob), NULL, 0);

    rc = snap_action_sync_execute_job(action, &cjob, timeout);

    free(job_struct);

    if (rc != 0)
        // Some error occurred
        return MTL_ERROR_INVALID_ARGUMENT;

    if (cjob.retc != SNAP_RETC_SUCCESS)
        // Some error occurred
        return MTL_ERROR_INVALID_ARGUMENT;

    return MTL_SUCCESS;
}

mtl_operator_specification op_read_file_specification = {
    { OP_READ_DRAM_ENABLE_ID, OP_DRAM_STREAM_ID },
    "metal_cat",
    true,

    &handle_opts,
    &apply_config,
    NULL,
    &get_filename
};

void op_read_file_set_buffer(uint64_t offset, uint64_t length) {
    _offset = offset;
    _length = length;
}
