# INI Runtime Parameters

INI expressions provide access to runtime parameters that are evaluated at the time the expression is executed.

## `FRAME_NUMBER`

Returns the number of the current frame.

```ini
$frame_id = FRAME_NUMBER
```

The frame boundary is defined by the `Present` call.

## `DRAW_NUMBER`

Returns the number of the current draw call within the frame.

```ini
$draw_id = DRAW_NUMBER
```

The counter starts at `1` for each new frame.

## `DISPATCH_NUMBER`

Returns the number of the current compute dispatch within the frame.

```ini
$dispatch_id = DISPATCH_NUMBER
```

The counter starts at `1` for each new frame.

## `TIME`

Returns a number of seconds passed since the game launch. Has microsecond precision (unlike 10-16 ms of original 3dmigoto).

```ini
$run_time = TIME
```

## `FRAME_TIME`

Returns a number of seconds passed between the previous and the current frame. Has microsecond precision.

```ini
$frame_time = FRAME_TIME
```

## `FPS`

Returns average FPS calculated by exponential moving average based counter.

```ini
$fps = FPS
```

## Accumulated draw parameters

A set of read-only parameters that total the element counts of every draw
command actually executed within the current command list run. Unlike the
draw-context parameters (`VERTEX_COUNT`, `FIRST_INDEX`, etc.), which report a
single draw call, the accumulated values keep a running sum across the whole
run. Because a given draw only adds to the counter matching its own draw
type, an override can recover an individual draw by diffing the counter that
`PREVIOUS_DRAW_TYPE` points at against a value captured at the start of its
block.

The counts only accumulate while the application has a **stream output
buffer bound** - they describe what has been written into that buffer, not
arbitrary draw traffic. Consequently they reset whenever the stream output
bindings change (each `SOSetTargets` call starts a new accumulation window):
a fresh run starts at `0`, and an in-run (re)bind zeroes the counts against
the new bindings.

| Parameter | Description |
|---|---|
| `ACCUMULATED_VERTEX_COUNT` | Sum of vertex counts over every `draw` / `drawinstanced` command |
| `ACCUMULATED_INDEX_COUNT` | Sum of index counts over every `drawindexed` / `drawindexedinstanced` command |
| `ACCUMULATED_INSTANCE_COUNT` | Sum of instance counts over every instanced draw command |

## Previous draw parameter

A read-only parameter reporting metadata about the **last draw command
actually executed within the current command list run**.

| Parameter | Description |
|---|---|
| `PREVIOUS_DRAW_TYPE` | `DrawCall` enum value of the last draw command (same value scheme as `draw_type`), `0` if nothing has been constructed yet |

All accumulated values are `0` at the start of each command list run and
update only when a draw command actually executes. Skipped or failed draws
leave everything untouched, and indirect draws (whose counts are resolved on
the GPU) never add to the accumulated counters. Values set in an outer
command list remain visible in nested command lists executed within the same
override block; this is useful for coordinating actions between mods.

```ini
$start = accumulated_vertex_count ; 0.0
draw = 100, 0
$drawn = accumulated_vertex_count - $start ; 100.0
```

## `SKIPPED`

Returns `1.0` if `handling = skip` has been set for the current draw call, `0.0` otherwise:

```ini
if (skipped == 1)
    ; This draw call has been marked to skip its draw command
```

The value is shared across all command lists belonging to the same draw call, so a post-command list (or an override for a later shader stage in the same draw) can detect that an earlier command list marked the draw as skipped.
