#include "lumi/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_CONST_CAPACITY 256
#define HOST_GLOBAL_CAPACITY 128
#define HOST_KEY_VAR_CAPACITY 64
#define HOST_KEY_SLOTS 16
#define HOST_CODE_CAPACITY 4096
#define HOST_STACK_CAPACITY 128
#define HOST_RANDOM_SEED 0x4C554D49u

typedef union host_cell {
    uint32_t u32;
    float f32;
} host_cell;

static float host_rand(void *user_data) {
    (void)user_data;
    return (float)rand() / ((float)RAND_MAX + 1.0f);
}

static int size_mul_overflow(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > ((size_t)-1) / a) {
        return 1;
    }
    *out = a * b;
    return 0;
}

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *file;
    long size;
    uint8_t *buffer;
    size_t read_size;

    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    buffer = malloc((size_t)size);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }
    read_size = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (read_size != (size_t)size) {
        free(buffer);
        return NULL;
    }
    *out_size = (size_t)size;
    return buffer;
}

static int parse_args(int argc, char **argv, size_t *const_cap, size_t *global_cap, size_t *key_var_cap,
    size_t *key_slots, size_t *code_cap, size_t *stack_cap, const char **path) {
    int i;
    *const_cap = HOST_CONST_CAPACITY;
    *global_cap = HOST_GLOBAL_CAPACITY;
    *key_var_cap = HOST_KEY_VAR_CAPACITY;
    *key_slots = HOST_KEY_SLOTS;
    *code_cap = HOST_CODE_CAPACITY;
    *stack_cap = HOST_STACK_CAPACITY;
    *path = NULL;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--const-cap") == 0 && i + 1 < argc) {
            *const_cap = (size_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--global-cap") == 0 && i + 1 < argc) {
            *global_cap = (size_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--key-var-cap") == 0 && i + 1 < argc) {
            *key_var_cap = (size_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            *key_slots = (size_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--code-cap") == 0 && i + 1 < argc) {
            *code_cap = (size_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--stack-cap") == 0 && i + 1 < argc) {
            *stack_cap = (size_t)strtoul(argv[++i], NULL, 10);
        } else if (argv[i][0] != '-') {
            *path = argv[i];
        } else {
            return 0;
        }
    }
    return *path != NULL;
}

int main(int argc, char **argv) {
    const char *path;
    size_t const_cap;
    size_t global_cap;
    size_t key_var_cap;
    size_t key_slots;
    size_t code_cap;
    size_t stack_cap;
    size_t total_key_cells;
    uint8_t *data;
    size_t data_size;
    lumi_vm_requirements req;
    lumi_vm_error error;
    float *constants;
    float *initial_globals;
    float *initial_keys;
    uint8_t *init_code;
    uint8_t *update_code;
    uint8_t *render_code;
    uint32_t *globals;
    uint32_t *keys;
    uint32_t *stack;
    lumi_vm_program_storage program_storage;
    lumi_vm_state_storage state_storage;
    lumi_vm_program program;
    lumi_vm_state state;
    char line[256];

    if (!parse_args(argc, argv, &const_cap, &global_cap, &key_var_cap, &key_slots, &code_cap, &stack_cap, &path)) {
        fprintf(stderr, "usage: %s [--const-cap N] [--global-cap N] [--key-var-cap N] [--keys N] [--code-cap N] [--stack-cap N] <file.lbc>\n", argv[0]);
        return 1;
    }

    data = read_file(path, &data_size);
    if (data == NULL) {
        fprintf(stderr, "failed to read %s\n", path);
        return 1;
    }
    if (!lumi_vm_measure_bytecode(data, data_size, &req, &error)) {
        fprintf(stderr, "measure failed: %s (%zu)\n", error.message, error.detail);
        free(data);
        return 1;
    }

    constants = calloc(const_cap == 0 ? 1 : const_cap, sizeof(*constants));
    initial_globals = calloc(global_cap == 0 ? 1 : global_cap, sizeof(*initial_globals));
    initial_keys = calloc(key_var_cap == 0 ? 1 : key_var_cap, sizeof(*initial_keys));
    init_code = calloc(code_cap == 0 ? 1 : code_cap, sizeof(*init_code));
    update_code = calloc(code_cap == 0 ? 1 : code_cap, sizeof(*update_code));
    render_code = calloc(code_cap == 0 ? 1 : code_cap, sizeof(*render_code));
    globals = calloc(global_cap == 0 ? 1 : global_cap, sizeof(*globals));
    if (size_mul_overflow(key_var_cap, key_slots, &total_key_cells)) {
        fprintf(stderr, "host capacity overflow for key storage\n");
        free(data);
        return 1;
    }
    keys = calloc(total_key_cells == 0 ? 1 : total_key_cells, sizeof(*keys));
    stack = calloc(stack_cap == 0 ? 1 : stack_cap, sizeof(*stack));
    if (constants == NULL || initial_globals == NULL || initial_keys == NULL || init_code == NULL
        || update_code == NULL || render_code == NULL || globals == NULL || keys == NULL || stack == NULL) {
        fprintf(stderr, "host allocation failure\n");
        free(data);
        free(constants); free(initial_globals); free(initial_keys);
        free(init_code); free(update_code); free(render_code);
        free(globals); free(keys); free(stack);
        return 1;
    }

    program_storage = (lumi_vm_program_storage){
        constants, const_cap,
        initial_globals, global_cap,
        initial_keys, key_var_cap,
        init_code, code_cap,
        update_code, code_cap,
        render_code, code_cap
    };
    state_storage = (lumi_vm_state_storage){globals, global_cap, keys, total_key_cells, stack, stack_cap};

    if (!lumi_vm_can_load(&req, &program_storage, &state_storage, key_slots, &error)) {
        fprintf(stderr, "capacity check failed: %s (%zu)\n", error.message, error.detail);
        goto fail;
    }
    if (!lumi_vm_load_program(data, data_size, &program_storage, &program, &error)) {
        fprintf(stderr, "load failed: %s (%zu)\n", error.message, error.detail);
        goto fail;
    }
    if (!lumi_vm_init_state(&program, &state_storage, key_slots, &state, &error)) {
        fprintf(stderr, "state init failed: %s (%zu)\n", error.message, error.detail);
        goto fail;
    }
    srand(HOST_RANDOM_SEED);
    lumi_vm_set_random(&state, host_rand, NULL);

    while (fgets(line, sizeof(line), stdin) != NULL) {
        lumi_vm_output output;
        if (line[0] == '\n' || line[0] == '#') {
            continue;
        }
        if (strncmp(line, "reset", 5) == 0) {
            lumi_vm_reset_state(&program, &state);
            srand(HOST_RANDOM_SEED);
            printf("reset\n");
            continue;
        }
        if (strncmp(line, "init", 4) == 0) {
            if (!lumi_vm_run_init(&program, &state, &output, &error)) {
                fprintf(stderr, "init failed: %s (%zu)\n", error.message, error.detail);
                goto fail;
            }
            printf("init steps=%zu\n", output.instructions_executed);
            continue;
        }
        if (strncmp(line, "update ", 7) == 0) {
            lumi_vm_inputs inputs;
            memset(&inputs, 0, sizeof(inputs));
            if (sscanf(line + 7, "%f %f", &inputs.dt, &inputs.speed) != 2) {
                fprintf(stderr, "invalid update line: %s", line);
                goto fail;
            }
            if (!lumi_vm_run_update(&program, &state, &inputs, &output, &error)) {
                fprintf(stderr, "update failed: %s (%zu)\n", error.message, error.detail);
                goto fail;
            }
            printf("update");
            for (size_t i = 0; i < state.global_count; ++i) {
                host_cell cell;
                cell.u32 = state.globals[i];
                printf(" g%zu=%.6f", i, cell.f32);
            }
            printf(" steps=%zu\n", output.instructions_executed);
            continue;
        }
        if (strncmp(line, "render ", 7) == 0) {
            size_t key_index;
            lumi_vm_inputs inputs;
            memset(&inputs, 0, sizeof(inputs));
            if (sscanf(line + 7, "%zu %f %f %f %f %f %f",
                    &key_index, &inputs.x, &inputs.y,
                    &inputs.dt, &inputs.speed, &inputs.pressed, &inputs.press) != 7) {
                fprintf(stderr, "invalid render line: %s", line);
                goto fail;
            }
            if (!lumi_vm_run_render(&program, &state, key_index, &inputs, &output, &error)) {
                fprintf(stderr, "render failed: %s (%zu)\n", error.message, error.detail);
                goto fail;
            }
            printf("render key=%zu color=0x%06X", key_index, output.color & 0xFFFFFFu);
            for (size_t i = 0; i < state.global_count; ++i) {
                host_cell cell;
                cell.u32 = state.globals[i];
                printf(" g%zu=%.6f", i, cell.f32);
            }
            for (size_t i = 0; i < state.key_count; ++i) {
                host_cell cell;
                cell.u32 = state.keys[key_index * state.key_count + i];
                printf(" k%zu=%.6f", i, cell.f32);
            }
            printf(" steps=%zu\n", output.instructions_executed);
            continue;
        }
        fprintf(stderr, "unknown command: %s", line);
        goto fail;
    }

    free(data);
    free(constants); free(initial_globals); free(initial_keys);
    free(init_code); free(update_code); free(render_code);
    free(globals); free(keys); free(stack);
    return 0;

fail:
    free(data);
    free(constants); free(initial_globals); free(initial_keys);
    free(init_code); free(update_code); free(render_code);
    free(globals); free(keys); free(stack);
    return 1;
}
