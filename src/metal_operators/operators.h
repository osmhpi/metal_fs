#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct operator_id { uint8_t enable_id; uint8_t stream_id; } operator_id;
struct snap_action;

typedef const void* (*mtl_operator_handle_opts_f)(int argc, char *argv[], uint64_t *length, const char* cwd, bool *valid);
typedef int (*mtl_operator_apply_configuration_f)(struct snap_action *action);
typedef uint64_t (*mtl_operator_get_length_f)();

typedef struct mtl_operator_specification {
    operator_id id;
    const char *name;
    bool is_data_source;

    mtl_operator_handle_opts_f handle_opts;
    mtl_operator_apply_configuration_f apply_config;
    mtl_operator_get_length_f get_file_length;
} mtl_operator_specification;

typedef struct mtl_operator_execution_plan {
    const operator_id* afus;
    uint64_t length;
} mtl_operator_execution_plan;
