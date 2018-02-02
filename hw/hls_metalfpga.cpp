#include "action_metalfpga.H"

#include "mf_endian.h"
#include "mf_file.h"
#include "mf_jobstruct.h"


/* #include "hls_globalmem.h" */

/* snap_membus_t * gmem_host_in; */
/* snap_membus_t * gmem_host_out; */
/* snap_membus_t * gmem_ddr; */


#define HW_RELEASE_LEVEL       0x00000013


static mf_retc_t process_action(snap_membus_t * mem_in,
                                snap_membus_t * mem_out,
                                action_reg * act_reg);

static mf_retc_t action_map(snap_membus_t * mem_in, const mf_job_map_t & job);
static mf_retc_t action_query(mf_job_query_t & job);
static mf_retc_t action_access(const mf_job_access_t & job);


// ------------------------------------------------
// -------------- ACTION ENTRY POINT --------------
// ------------------------------------------------
// This design uses FPGA DDR.
// Set Environment Variable "SDRAM_USED=TRUE" before compilation.
void hls_action(snap_membus_t * din,
                snap_membus_t * dout,
                snap_membus_t * ddr,
                action_reg * action_reg,
                action_RO_config_reg * action_config)
{
    // Configure Host Memory AXI Interface
#pragma HLS INTERFACE m_axi port=din bundle=host_mem offset=slave depth=512
#pragma HLS INTERFACE m_axi port=dout bundle=host_mem offset=slave depth=512
#pragma HLS INTERFACE s_axilite port=din bundle=ctrl_reg offset=0x030
#pragma HLS INTERFACE s_axilite port=dout bundle=ctrl_reg offset=0x040

    // Configure Host Memory AXI Lite Master Interface
#pragma HLS DATA_PACK variable=action_config
#pragma HLS INTERFACE s_axilite port=action_config bundle=ctrl_reg  offset=0x010
#pragma HLS DATA_PACK variable=action_reg
#pragma HLS INTERFACE s_axilite port=action_reg bundle=ctrl_reg offset=0x100
#pragma HLS INTERFACE s_axilite port=return bundle=ctrl_reg

    // Configure DDR memory Interface
#pragma HLS INTERFACE m_axi port=ddr bundle=card_mem0 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 
#pragma HLS INTERFACE s_axilite port=ddr bundle=ctrl_reg offset=0x050


    // Make memory ports globally accessible
    /* gmem_host_in = din; */
    /* gmem_host_out = dout; */
    /* gmem_ddr = ddr; */

    // Required Action Type Detection
    switch (action_reg->Control.flags) {
    case 0:
        action_config->action_type = (snapu32_t)METALFPGA_ACTION_TYPE;
        action_config->release_level = (snapu32_t)HW_RELEASE_LEVEL;
        action_reg->Control.Retc = (snapu32_t)0xe00f;
        break;
    default:
        action_reg->Control.Retc = process_action(din, dout, action_reg);
        break;
    }
}


// ------------------------------------------------
// --------------- ACTION FUNCTIONS ---------------
// ------------------------------------------------

// Decode job_type and call appropriate action
static mf_retc_t process_action(snap_membus_t * mem_in,
                                snap_membus_t * mem_out,
                                action_reg * act_reg)
{
    switch(act_reg->Data.job_type)
    {
        case MF_JOB_MAP:
          {
            mf_job_map_t map_job = mf_read_job_map(mem_in, act_reg->Data.job_address);
            return action_map(mem_in, map_job);
          }
        case MF_JOB_QUERY:
          {
            mf_job_query_t query_job = mf_read_job_query(mem_in, act_reg->Data.job_address);
            mf_retc_t retc = action_query(query_job);
            mf_write_job_query(mem_out, act_reg->Data.job_address, query_job);
            return retc;
          }
        case MF_JOB_ACCESS:
          {
            mf_job_access_t access_job = mf_read_job_access(mem_in, act_reg->Data.job_address);
            return action_access(access_job);
          }
        default:
            return SNAP_RETC_FAILURE;
    }
}

// File Map / Unmap Operation:
static mf_retc_t action_map(snap_membus_t * mem_in,
                            const mf_job_map_t & job)
{
    if (job.slot >= MF_SLOT_COUNT)
    {
        return SNAP_RETC_FAILURE;
    }
    mf_slot_offset_t slot = job.slot;

    if (job.map_else_unmap)
    {
        if (job.extent_count > MF_EXTENT_COUNT)
        {
            return SNAP_RETC_FAILURE;
        }
        mf_extent_count_t extent_count = job.extent_count;

        if (!mf_file_open(slot, extent_count, job.extent_address, mem_in))
        {
            mf_file_close(slot);
            return SNAP_RETC_FAILURE;
        }
    }
    else
    {
        if (!mf_file_close(slot))
        {
            return SNAP_RETC_FAILURE;
        }
    }
    return SNAP_RETC_SUCCESS;
}

static mf_retc_t action_query(mf_job_query_t & job)
{
    snap_membus_t line;
    if (job.query_mapping)
    {
        job.lblock_to_pblock = mf_file_map_pblock(job.slot, job.lblock_to_pblock);
        /* snapu64_t pblock= mf_file_map_pblock(job.slot, job.lblock); */
        /* mf_set64(line, 0, pblock); */
    }
    if (job.query_state)
    {
        job.is_open = mf_file_is_open(job.slot)? MF_TRUE : MF_FALSE;
        job.is_active = mf_file_is_active(job.slot)? MF_TRUE : MF_FALSE;
        job.extent_count = mf_file_get_extent_count(job.slot);
        job.block_count = mf_file_get_block_count(job.slot);
        job.current_lblock = mf_file_get_lblock(job.slot);
        job.current_pblock = mf_file_get_pblock(job.slot);
        /* mf_set64(line, 8, mf_file_is_open(job.slot)); */
        /* mf_set64(line, 16, mf_file_is_active(job.slot)); */
        /* mf_set64(line, 24, mf_file_get_extent_count(job.slot)); */
        /* mf_set64(line, 32, mf_file_get_block_count(job.slot)); */
        /* mf_set64(line, 40, mf_file_get_lblock(job.slot)); */
        /* mf_set64(line, 48, mf_file_get_pblock(job.slot)); */
    }
    /* MFB_WRITE(gmem_host_out, job.result_address, line); */
    //dout_gmem[MFB_ADDRESS(job.result_address)] = line
    return SNAP_RETC_SUCCESS;
}

static mf_retc_t action_access(const mf_job_access_t & job)
{
    if (! mf_file_is_open(job.slot))
    {
        return SNAP_RETC_FAILURE;
    }

    return SNAP_RETC_FAILURE;
}


//-----------------------------------------------------------------------------
//--- TESTBENCH ---------------------------------------------------------------
//-----------------------------------------------------------------------------

#ifdef NO_SYNTH

#include <stdio.h>

int main()
{
    static snap_membus_t din_gmem[1024];
    static snap_membus_t dout_gmem[1024];
    static snap_membus_t dram_gmem[1024];
    //static snapu32_t nvme_gmem[];
    action_reg act_reg;
    action_RO_config_reg act_config;

    // read action config:
    act_reg.Control.flags = 0x0;
    hls_action(din_gmem, dout_gmem, dram_gmem, &act_reg, &act_config);
    fprintf(stderr, "ACTION_TYPE:   %08x\nRELEASE_LEVEL: %08x\nRETC:          %04x\n",
        (unsigned int)act_config.action_type,
        (unsigned int)act_config.release_level,
        (unsigned int)act_reg.Control.Retc);


    // test action functions:

    fprintf(stderr, "// MAP slot 2 1000:7,2500:3,1700:8\n");
    uint64_t * job_mem = (uint64_t *)din_gmem;
    uint8_t * job_mem_b = (uint8_t *)din_gmem;
    job_mem_b[0] = 2; // slot
    job_mem[1] = true; // map
    job_mem[2] = 3; // extent_count

    job_mem[8]  = 1000; // ext0.begin
    job_mem[9]  = 7;    // ext0.count
    job_mem[10] = 2500; // ext1.begin
    job_mem[11] = 3;    // ext1.count
    job_mem[12] = 1700; // ext2.begin
    job_mem[13] = 8;    // ext2.count
    
    act_reg.Control.flags = 0x1;
    act_reg.Data.job_address = 0;
    act_reg.Data.job_type = MF_JOB_MAP;
    hls_action(din_gmem, dout_gmem, dram_gmem, &act_reg, &act_config);

    fprintf(stderr, "// MAP slot 7 1200:8,1500:24\n");
	job_mem_b[0] = 7; // slot
	job_mem[1] = true; // map
	job_mem[2] = 2; // extent_count

	job_mem[8]  = 1200; // ext0.begin
	job_mem[9]  = 8;    // ext0.count
	job_mem[10] = 1500; // ext1.begin
	job_mem[11] = 24;   // ext1.count

	act_reg.Control.flags = 0x1;
	act_reg.Data.job_address = 0;
	act_reg.Data.job_type = MF_JOB_MAP;
	hls_action(din_gmem, dout_gmem, dram_gmem, &act_reg, &act_config);

    fprintf(stderr, "// MAP query slot 2, lblock 9\n");

    job_mem_b[0] = 2; // slot
    job_mem_b[1] = true; // query_mapping
    job_mem_b[2] = true; // query_state
	job_mem[1] = 9; // lblock

	act_reg.Control.flags = 0x1;
	act_reg.Data.job_address = 0;
	act_reg.Data.job_type = MF_JOB_QUERY;
	hls_action(din_gmem, dout_gmem, dram_gmem, &act_reg, &act_config);

	uint64_t is_open = mf_file_is_open(2)? MF_TRUE : MF_FALSE;
	uint64_t is_active = mf_file_is_active(2)? MF_TRUE : MF_FALSE;
	uint64_t extent_count = mf_file_get_extent_count(2);
	uint64_t block_count = mf_file_get_block_count(2);
	uint64_t current_lblock = mf_file_get_lblock(2);
	uint64_t current_pblock = mf_file_get_pblock(2);

    return 0;
}

#endif /* NO_SYNTH */
