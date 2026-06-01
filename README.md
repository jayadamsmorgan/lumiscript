<div align="center">

# LumiScript
**A Small Compiled Language for Keyboard Backlight Effects**

</div>

LumiScript is a small language and bytecode toolchain for describing keyboard backlight behavior.
It includes a host-side compiler, a reusable C library, and a static-memory VM designed to stay compatible with MCU-style deployment constraints.

Scripts are compiled from `.lumi` source into `.lbc` bytecode, then executed through a three-stage runtime model:

- `init`: runs once when the effect is loaded or reset
- `update`: runs once per backlight tick
- `render`: runs once per key and produces the final color

The compiler is free to allocate memory on the host, while the VM is designed around caller-provided fixed storage so it can reject scripts that do not fit available static memory before execution starts.

## Key Features

* **Host Compiler in C:** Compile Lumi source files into a compact bytecode format with `lumic`.
* **Static-Memory VM:** Load and execute bytecode with fixed capacities for constants, globals, per-key state, code sections, and stack.
* **MCU-Oriented Design:** The runtime API uses caller-owned buffers and up-front fit checks instead of dynamic allocation.
* **Three-Stage Execution Model:** Separate one-time initialization, per-update shared logic, and per-key rendering logic.
* **Persistent State:** Supports both shared `global var` state and per-key `key var` state.
* **Compile-Time Folding:** Constant `let` expressions and constant-capable builtin calls are folded by the compiler when possible.
* **Math + Color Builtins:** Includes helpers such as `sin`, `cos`, `sqrt`, `ceil`, `floor`, `round`, `lerp`, `dist`, `rand`, `rgb`, and `hsv`.
* **Golden Bytecode Tests:** The project checks not only that scripts compile and run, but also that serialized bytecode matches expected output for selected cases.

> [!IMPORTANT]
>
> **Please note that this is an `alpha` project.**
>
> The core language, bytecode format, and VM are implemented and tested, but the system is still evolving and should be treated as experimental.

## Quickstart

### 1) Build the project

```bash
meson setup build
meson compile -C build
```

### 2) Compile a Lumi script

```bash
./build/lumic examples/example-static.lumi -o build/example-static.lbc
```

### 3) Run the bytecode in the host VM

```bash
printf 'init\nrender 0 0 0 16 1 0 0\n' | ./build/lumivm build/example-static.lbc
```

Example output:

```text
init steps=1
render key=0 color=0x96C8FF steps=6
```

## Language Overview

Example:

```lumi
type animation
global var phase = 0
key var glow = 0

update {
    phase = phase + dt * speed
}

render {
    if pressed {
        glow = 100
    } else {
        glow = clamp(glow - dt * 0.3, 0, 100)
    }

    color hsv(phase + x * 2, 100, glow)
}
```

Supported concepts include:

* `type static` and `type animation`
* `global var` and `key var`
* section-local `let`
* assignments
* statement and expression `if`
* float-based arithmetic and boolean semantics
* builtins such as `abs`, `sin`, `cos`, `sqrt`, `ceil`, `floor`, `round`, `clamp`, `dist`, `lerp`, `min`, `max`, `pow`, `rand`, `rgb`, and `hsv`

## Tools

### `lumic`

Compiles a Lumi source file into serialized bytecode.

```bash
./build/lumic input.lumi -o output.lbc
```

Use `-O`, `-O0`, `-O1`, `-O2`, or `-O3` to choose an optimization level:

```bash
./build/lumic -O2 input.lumi -o output.lbc
```

Higher levels enable constant folding, expression simplification, bytecode cleanup, dedicated builtin opcodes, and conservative common-subexpression elimination.

### `lumivm`

Loads bytecode into fixed-capacity host buffers, checks whether it fits, then executes commands from stdin.

```bash
./build/lumivm build/example-static.lbc
```

Supported commands:

* `init`
* `reset`
* `update <dt> <speed>`
* `render <key> <x> <y> <dt> <speed> <pressed> <press>`

Example interactive session:

```text
init
update 50 2
render 0 3 4 50 2 0 0
render 1 30 40 50 2 1 100
```

You can also force capacity checks with smaller limits:

```bash
./build/lumivm --stack-cap 1 build/example-static.lbc
```

### `lumils`

The repository also includes a small stdio language server with diagnostics, hover, completion, and document symbols.

It is built together with the rest of the project:

```bash
meson setup build
meson compile -C build
```

## Documentation

Project documentation lives in the repository:

* **Language Guide:** [LANGUAGE.md](./LANGUAGE.md)
* **Bytecode Format:** [BYTECODE.md](./BYTECODE.md)

## Examples

The repository includes a small set of example scripts:

* `examples/example-static.lumi`
* `examples/example-animation.lumi`
* `examples/example-conditional.lumi`
* `examples/example-counter.lumi`
* `examples/example-branching.lumi`
* `examples/example-stateful.lumi`
* `examples/example-nested.lumi`
* `examples/example-math.lumi`
* `examples/example-array.lumi`
* `examples/example-rand.lumi`
* `examples/example-rounding.lumi`
* `examples/example-ripple.lumi`

## Testing

Run the full suite with:

```bash
meson test -C build --print-errorlogs
```

The current tests cover:

* successful compilation of example scripts
* compile-time failure cases
* host VM execution results
* bytecode golden checks
* static-capacity rejection paths

## Building From Source

Requirements:

* Meson
* Ninja
* a C11 compiler

Typical build flow:

```bash
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

## Contributing

Contributions are welcome.

Useful reports and patches include:

* compiler or VM bugs
* language design improvements
* new builtin functions
* more bytecode golden tests
* documentation fixes

If you report an issue, include:

* the `.lumi` script
* the command you ran
* the exact compiler or VM output
* whether the failure is in parsing, compilation, loading, or execution
