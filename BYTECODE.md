# Lumi Bytecode v1

This document describes the current bytecode format used by the Lumi compiler and VM.

## Execution Model

A compiled program is split into three code sections:

- `init`
- `update`
- `render`

The VM executes them like this:

1. `init` once when state is created or reset
2. `update` once per backlight tick
3. `render` once per key per tick

## State Model

The VM keeps two persistent state spaces:

- global state: one shared array of `global var` slots
- key state: one array of `key var` slots for each key

The compiler emits initial float values for both spaces.

## Value Model

- scalar cells are 32-bit values
- numeric and boolean values are stored as IEEE-754 `float32`
- colors are packed as `0x00RRGGBB` in the same 32-bit cell type

## Serialized Header

All fields are little-endian.

1. `uint32_t magic` = `0x4C554D49`
2. `uint16_t version` = `1`
3. `uint16_t program_type`
4. `uint32_t constant_count`
5. `uint32_t global_count`
6. `uint32_t key_count`
7. `uint16_t max_stack_depth`
8. `uint16_t reserved`
9. `uint32_t init_size`
10. `uint32_t update_size`
11. `uint32_t render_size`

After the header:

1. constant table: `constant_count * float32`
2. initial globals: `global_count * float32`
3. initial key values: `key_count * float32`
4. `init` bytecode
5. `update` bytecode
6. `render` bytecode

## Opcode Set

- `PUSH_CONST_F32`
- `LOAD_INPUT`
- `LOAD_GLOBAL`
- `STORE_GLOBAL`
- `LOAD_KEY`
- `STORE_KEY`
- `ADD`
- `SUB`
- `MUL`
- `DIV`
- `MOD`
- `NEG`
- `NOT`
- `EQ`
- `NE`
- `LT`
- `LE`
- `GT`
- `GE`
- `AND`
- `OR`
- `CALL_BUILTIN`
- `JUMP`
- `JUMP_IF_FALSE`
- `SET_COLOR`
- `HALT`

## Operand Widths

- `PUSH_CONST_F32`: opcode + `uint16_t constant_index`
- `LOAD_INPUT`: opcode + `uint8_t input_slot`
- `LOAD_GLOBAL`: opcode + `uint16_t global_index`
- `STORE_GLOBAL`: opcode + `uint16_t global_index`
- `LOAD_KEY`: opcode + `uint16_t key_index`
- `STORE_KEY`: opcode + `uint16_t key_index`
- `CALL_BUILTIN`: opcode + `uint8_t builtin_id` + `uint8_t arg_count`
- `JUMP`: opcode + `uint16_t target_pc`
- `JUMP_IF_FALSE`: opcode + `uint16_t target_pc`

Jump targets are absolute byte offsets within the current section.

## Inputs By Section

### `init`

No runtime inputs are available.

### `update`

Available inputs:

- `dt`
- `speed`

### `render`

Available inputs:

- `x`
- `y`
- `dt`
- `speed`
- `pressed`
- `press`

## Stack Bound

The compiler emits `max_stack_depth`.

The VM must reject programs when its fixed stack capacity is smaller than this value.

## Builtins

Current builtin functions:

- `abs`
- `sin`
- `cos`
- `sqrt`
- `clamp`
- `dist`
- `lerp`
- `min`
- `max`
- `pow`
- `rgb`
- `hsv`
