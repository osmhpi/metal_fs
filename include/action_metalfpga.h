#ifndef __ACTION_METALFPGA_H__
#define __ACTION_METALFPGA_H__

/*
 * TODO: licensing, implemented according to IBM snap examples
 */
#include <snap_types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define METALFPGA_ACTION_TYPE 0x00000216 
 
#define MF_MASK(BHI, BLO) ((0x1<<(BHI+1)) - (0x1<<BLO))

typedef struct mf_extent {
    uint64_t block_begin;
    uint64_t block_count;
} mf_extent_t;

#define MF_FUNC_MAP 0
typedef struct mf_func_map_job {
    uint8_t slot;
    bool map;
    bool indirect;
    uint16_t extent_count;
    union
    {
        mf_extent_t direct[4];
        uint64_t indirect_address;
    } extents;
} mf_func_map_job_t;

#define MF_FUNC_QUERY 1
typedef struct mf_func_query_job {
    bool slot;
    uint8_t query_mapping;
    uint8_t query_state;
    uint64_t lblock_to_pblock;
    bool state_open;
    bool state_active;
    uint16_t state_extent_count;
    uint64_t state_block_count;
    uint64_t state_current_lblock;
    uint64_t state_current_pblock;
} mf_func_query_job_t;

#define MF_FUNC_ACCESS 2
typedef struct mf_func_access_job {
    uint8_t operation;
    uint8_t flags; // TODO-lw define Flag masks and positions
    uint64_t host_buffer;
    uint64_t file_offset;
    uint64_t byte_count;
} mf_func_access_job_t;

typedef struct metalfpga_job {
    uint8_t function;
    union {
        mf_func_map_job_t map;
        mf_func_query_job_t query;
        mf_func_access_job_t access;
    } jspec;
} metalfpga_job_t;

#ifdef __cplusplus
}
#endif
#endif	/* __ACTION_METALFPGA_H__ */
