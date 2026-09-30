# INI Runtime Parameters

INI expressions provide access to runtime parameters that are evaluated at the time the expression is executed.

## `FRAME_NUMBER`

Returns the number of the current frame.

```ini
$frame_id = FRAME_NUMBER
```

The frame boundary is defined by the `Present` call.

## `FRAME_ANALYSIS`

Returns `1` while frame analysis is capturing the current frame and `0` otherwise. Use it to run INI commands only during a frame analysis capture:

```ini
if FRAME_ANALYSIS
    ; Run only while a frame analysis capture is active.
endif
```

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
