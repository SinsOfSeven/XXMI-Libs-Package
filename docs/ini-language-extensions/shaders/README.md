# Shader Declarations

Shader targets such as `ps` and `vs` expose CB and SRV declarations from the **original shader bytecode**. These are expression members; they inspect metadata and do not execute commands or read GPU resource contents.

```ini
if ps->srv_declares(3)
    ; The current pixel shader declares t3.
endif
```

| Member | Meaning |
| --- | --- |
| `shader->cb_mask` | A 14-slot constant-buffer presence mask, represented exactly by an INI float. |
| `shader->cb_type(slot)` / `shader->cb_size(slot)` | Declared CB type and size at one slot. |
| `shader->srv_declares(slot)` | Whether the shader declares one SRV slot. Use this in place of a single 128-bit mask value. |
| `shader->srv_type(slot)` / `shader->srv_stride(slot)` / `shader->srv_dimension(slot)` | SRV declaration properties at one slot. |
| `shader->cb_min` / `shader->cb_max` | First and last declared CB slot. |
| `shader->srv_min` / `shader->srv_max` | First and last declared SRV slot. |

The `*_min` and `*_max` values are inclusive. They return `-1` when declarations are known to be empty. Unknown metadata falls back to the full legal slot range, so a script does not silently skip resources when a shader could not be parsed. A missing bound shader also returns `-1` for these range members.

```ini
local $min = ps->srv_min
local $max = ps->srv_max
if $min >= 0
    checktextureoverride = ps-t[$min:$max]
endif
```

The bounds narrow the candidate range. A shader may have gaps between its first and last declaration; `checktextureoverride` continues to skip each SRV slot known to be undeclared. The same declaration rule is used when frame analysis dumps bound SRV textures. Vertex and index buffers are input-assembler state and are not covered by the shader SRV declaration mask.

For the persisted arena format, migration script, and cache lifetime, see the [implementation design](../../shader-declaration-cache.md).
