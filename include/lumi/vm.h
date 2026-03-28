#ifndef LUMI_VM_H
#define LUMI_VM_H

#include <stddef.h>
#include <stdint.h>

#include "lumi/bytecode.h"

typedef struct lumi_vm_requirements {
    lumi_program_type program_type;
    size_t constant_count;
    size_t global_count;
    size_t key_count;
    size_t init_size;
    size_t update_size;
    size_t render_size;
    uint16_t max_stack_depth;
} lumi_vm_requirements;

typedef struct lumi_vm_error {
    const char *message;
    size_t detail;
} lumi_vm_error;

typedef struct lumi_vm_program_storage {
    float *constants;
    size_t constant_capacity;
    float *initial_globals;
    size_t global_capacity;
    float *initial_keys;
    size_t key_capacity;
    uint8_t *init_code;
    size_t init_capacity;
    uint8_t *update_code;
    size_t update_capacity;
    uint8_t *render_code;
    size_t render_capacity;
} lumi_vm_program_storage;

typedef struct lumi_vm_program {
    lumi_program_type program_type;
    const float *constants;
    size_t constant_count;
    const float *initial_globals;
    size_t global_count;
    const float *initial_keys;
    size_t key_count;
    const uint8_t *init_code;
    size_t init_size;
    const uint8_t *update_code;
    size_t update_size;
    const uint8_t *render_code;
    size_t render_size;
    uint16_t max_stack_depth;
} lumi_vm_program;

typedef struct lumi_vm_state_storage {
    uint32_t *globals;
    size_t global_capacity;
    uint32_t *keys;
    size_t key_cell_capacity;
    uint32_t *stack;
    size_t stack_capacity;
} lumi_vm_state_storage;

typedef struct lumi_vm_state {
    uint32_t *globals;
    size_t global_count;
    uint32_t *keys;
    size_t key_count;
    size_t key_slots;
    uint32_t *stack;
    size_t stack_capacity;
} lumi_vm_state;

typedef struct lumi_vm_inputs {
    float x;
    float y;
    float dt;
    float speed;
    float pressed;
    float press;
} lumi_vm_inputs;

typedef struct lumi_vm_output {
    uint32_t color;
    int has_color;
    size_t instructions_executed;
} lumi_vm_output;

int lumi_vm_measure_bytecode(const uint8_t *data, size_t data_size, lumi_vm_requirements *out_requirements, lumi_vm_error *out_error);
int lumi_vm_can_load(const lumi_vm_requirements *requirements, const lumi_vm_program_storage *program_storage,
    const lumi_vm_state_storage *state_storage, size_t key_slots, lumi_vm_error *out_error);
int lumi_vm_load_program(const uint8_t *data, size_t data_size, const lumi_vm_program_storage *storage,
    lumi_vm_program *out_program, lumi_vm_error *out_error);
int lumi_vm_init_state(const lumi_vm_program *program, const lumi_vm_state_storage *storage,
    size_t key_slots, lumi_vm_state *out_state, lumi_vm_error *out_error);
void lumi_vm_reset_state(const lumi_vm_program *program, lumi_vm_state *state);
int lumi_vm_run_init(const lumi_vm_program *program, lumi_vm_state *state, lumi_vm_output *out_output, lumi_vm_error *out_error);
int lumi_vm_run_update(const lumi_vm_program *program, lumi_vm_state *state, const lumi_vm_inputs *inputs,
    lumi_vm_output *out_output, lumi_vm_error *out_error);
int lumi_vm_run_render(const lumi_vm_program *program, lumi_vm_state *state, size_t key_index,
    const lumi_vm_inputs *inputs, lumi_vm_output *out_output, lumi_vm_error *out_error);

#endif
