
#### AetherScript (Lua 5.4 port) — porting plan

| Lua subsystem | AetherOS equivalent |
|---------------|---------------------|
| `malloc/free` | `kmalloc` / kernel heap |
| `fopen/fread/fwrite` | `sys_fs_open/read/close` wrappers in a shim header |
| `printf / stdout` | libaether `printf` → framebuffer/UART |
| `time()` | `sys_get_ticks() / 100` (seconds) |
| `stdin` | Disabled in interpreter mode (script only) |
| `os.execute()` | Maps to `sys_spawn()` — allows scripts to launch apps |
| `io.write()` | Routes to stdout pipe → editor output panel |

Compile with `aarch64-none-elf-gcc -nostdlib` using the ported Lua sources.
Link as a static ELF in initrd alongside aether_term and files.

#### AetherEditor window layout (libwidget)

```
┌─────────────────────────────────────────────────────┐
│  ●  AetherEditor — script.as          [Open][Save]  │
├─────────────────────────────────────────────────────┤
│  1  -- Hello from AetherScript                      │
│  2  print("AetherOS " .. os.version)                │
│  3  for i = 1, 10 do                                │
│  4    print(i)                                      │
│  5  end                                             │
│                              [textarea / libwidget] │
├─────────────────────────────────────────────────────┤
│  [Run ▶]   [Clear output]                           │
├─────────────────────────────────────────────────────┤
│  > AetherOS 0.5.5                                   │
│  > 1 2 3 4 5 6 7 8 9 10                             │
│                              [output panel]         │
└─────────────────────────────────────────────────────┘
```
