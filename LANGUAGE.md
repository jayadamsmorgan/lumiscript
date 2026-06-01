# Lumi Language Guide

Lumi is a small language for keyboard backlight effects.

Each script is split into three explicit stages:

- `init`: runs once when the effect is loaded or reset
- `update`: runs once per backlight update tick
- `render`: runs once per key on each tick and produces the final color

## Top-Level Structure

Every script must declare a type and define a `render` block.

Example:

```lumi
type static

render {
    color rgb(150, 200, 255)
}
```

## Program Types

Supported types:

- `type static`
- `type animation`

## Variables

Lumi has two persistent variable kinds.

### `global var`

Shared by the whole script instance.

Used for state updated once per tick in `init` or `update`.

```lumi
global var phase = 0
```

Fixed-size arrays are supported for both persistent storage kinds:

```lumi
global var ripple_x[4] = 0
```

### `key var`

Stored separately for each key.

Used for per-key state such as fades or press memory.

```lumi
key var glow = 0
```

### Variable Initializers

`global var` and `key var` initializers must be compile-time constants.
Array sizes must be positive compile-time integers.

If you need runtime setup, do it in `init`, `update`, or `render`.

## Sections

### `init`

Runs once when the VM state is created or reset.

Use it for one-time setup of global state.

```lumi
init {
    phase = 30
}
```

Available inputs:

- none

Accessible state:

- `global var`

### `update`

Runs once per backlight update.

Use it for shared animation state.

```lumi
update {
    phase = (phase + dt * speed * 0.12) % 360
}
```

Available inputs:

- `dt`
- `speed`

Accessible state:

- `global var`

### `render`

Runs once per key on every update and must assign `color`.

```lumi
render {
    color hsv((x * 3.6 + dt * 2) % 360, 100, 60)
}
```

Available inputs:

- `x`
- `y`
- `dt`
- `speed`
- `pressed`
- `press`
- `pressed_percentage` as an alias of `press`

Accessible state:

- `global var`
- `key var`

## Local Bindings

Use `let` inside section blocks for immutable local names.

```lumi
render {
    let d = dist(x, y, 50, 50)
    color hsv(d, 100, 100)
}
```

`let` is compile-time folded when possible.

Loop variables introduced by `for` are compile-time constants inside the loop body.

## Statements

Supported statements inside sections:

- `let name = expr`
- `name = expr`
- `name[index] = expr`
- `color expr`
- `if expr { ... }`
- `if expr { ... } else { ... }`
- `for i in start..end { ... }`

Only variables can be assigned:

- `global var` may be assigned in `init`, `update`, or `render`
- `key var` may be assigned only in `render`
- array indexing requires a compile-time integer index
- `for` loop bounds must be compile-time integers and loops are unrolled by the compiler

## Expressions

Supported expression forms:

- numeric literals
- variable and input references
- array indexing like `ripple_x[0]` or `ripple_x[i]`
- parentheses
- unary `-` and `!`
- binary `*`, `/`, `%`, `+`, `-`
- comparisons `==`, `!=`, `<`, `<=`, `>`, `>=`
- boolean operators `&&`, `||`
- function calls
- value-form `if`

Example:

```lumi
color if pressed {
    rgb(150, 200, 255)
} else {
    rgb(0, 0, 0)
}
```

## Built-In Inputs

- `x`: key X position
- `y`: key Y position
- `dt`: milliseconds since previous update
- `delta_ms`: alias for `dt`
- `speed`: user-selected speed
- `pressed`: `0` or `1`
- `press`: press percentage `0..100`
- `pressed_percentage`: alias for `press`

## Built-In Functions

- `abs(x)`
- `sin(x)`
- `cos(x)`
- `sqrt(x)`
- `ceil(x)`
- `floor(x)`
- `round(x)`
- `clamp(x, lo, hi)`
- `dist(x1, y1, x2, y2)`
- `lerp(a, b, t)`
- `min(a, b)`
- `max(a, b)`
- `pow(x, y)`
- `rand()`: pseudo-random float in `[0, 1)`, provided by the VM implementation
- `rgb(r, g, b)`
- `hsv(h, s, v)`

## Truthiness

Booleans use float semantics:

- `0` is false
- any non-zero value is true

## Example

```lumi
type animation
global var phase = 0
key var glow = 0

update {
    phase = (phase + dt * speed * 0.12) % 360
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
