# ELFWD

A generic PS2 ELF forwarder: loads a target ELF, optionally disguises its
`argv[0]` and passes it extra arguments, optionally resets the IOP and loads
IRX modules first, and optionally applies byte-level patches directly in RAM
before executing it. Everything is driven by a plain-text config file, and the
target file on disk is never modified.

## What it does

ELFWD is launched like any other PS2 homebrew ELF. It reads `ELFWD.CFG` from
the folder it was launched from, then:

1. Optionally resets the IOP and loads IRX modules (`reset=`, `irx=`).
2. Loads the target ELF (`boot=`) and starts it with a custom `argv`
   (`argv0=`, `arg=`).
3. Optionally patches the target's code/data in RAM after it is loaded and
   before it runs (`patch=`).

By default ELFWD does **not** reset the IOP and loads **no** modules of its
own: it relies on the IOP module set already resident from whichever launcher
started it, so it works from any storage device that launcher already
supports. `reset=1` and `irx=` are opt-in.

## Usage

1. Copy `ELFWD.ELF` and `ELFWD.CFG` to the same folder.
2. Launch `ELFWD.ELF` from your PS2 browser/launcher of choice.
3. ELFWD reads `ELFWD.CFG`, validates it, and runs the sequence below.

## `ELFWD.CFG` format

One `key=value` entry per line.

- Blank lines are ignored. A comment is a line whose first non-blank character
  is `#`; there are **no trailing comments** (`reset=1 # x` is an error).
- CRLF line endings and a UTF-8 BOM are accepted.
- Keys are case-insensitive. Spaces and tabs around `=` are tolerated.
- The value is everything after the first `=`, with leading and trailing
  spaces/tabs removed. Quote characters are kept verbatim.
- Empty values are errors.

| Key      | Count | Meaning |
|----------|-------|---------|
| `boot=`  | 1, required | ELF to load and run. Use a full path with the device (`mass:/...`, `mc0:/...`): it is loaded by the IOP. |
| `argv0=` | 0–1 | Passed as the target's `argv[0]`. If absent, `argv[0]` is the `boot=` path. |
| `arg=`   | 0–12 | `argv[1]`, `argv[2]`, … in line order, verbatim. |
| `reset=` | 0–1 | `0` (default): keep the launcher's IOP. `1`: reset the IOP. Anything else is an error. |
| `irx=`   | 0–16 | IRX module to load, in line order. Path, then optional arguments after the first space. |
| `patch=` | 0 or more | RAM patch, see below. |

### `irx=`

```
irx=<path> [arguments...]
```

- The path cannot contain spaces. Arguments are split on spaces/tabs and
  passed to the module as separate strings (no quote handling).
- `rom0:` modules (e.g. `rom0:SIO2MAN`) are loaded by path.
- **With `reset=1`**, every other module is read into RAM *before* the reset,
  while the launcher's devices are still available, and started from that
  image after the reset. This is what lets you bring your own USB stack
  (`usbd`, `usbhdfsd`) without relying on anything surviving the reset.
  The images share a 512 KB budget; an unreadable, empty or oversized file is
  an error.
- **With `reset=0`**, modules are loaded by path through the IOP, on top of
  whatever the launcher already loaded. Loading a module that is already
  resident (for example `usbd` when the launcher runs from USB) can fail or
  cause conflicts; with `reset=0` only load modules the launcher has not
  loaded. To start from a clean IOP, use `reset=1`.
- A path with no device (no `:`) is relative to the launcher's folder: both
  `usbd.irx` and `sub/usbd.irx` are looked up there. A path with a device
  (`mass:usbd.irx`, `mc0:x.irx`) is used as is.
- A path starting with `/` or `\` is an error.
- A module that fails to load is an error. The module's own start result is
  not checked.
- Modules are started strictly in line order and ELFWD does not resolve
  dependencies for you. After a reset, `rom0:MCMAN` and `rom0:MCSERV` need
  `rom0:SIO2MAN` loaded first:

  ```
  irx=rom0:SIO2MAN
  irx=rom0:MCMAN
  irx=rom0:MCSERV
  ```

### `patch=`

```
patch=<OFFSET> <VALUE>
patch=<OFFSET> <B1> <B2> <B3> ...
```

- Hex tokens, 1–8 digits, with an optional `$` or `0x` prefix (`$412`, `0x412`
  and `412` are the same).
- `OFFSET` is a **file offset** into the `boot=` ELF. ELFWD converts it to the
  RAM address using the ELF's real `PT_LOAD` program headers
  (`p_vaddr + (offset - p_offset)`), so it works regardless of how the target
  was linked.
- One value: cheat-style, written little-endian with a size taken from the
  digit count: 1–2 digits = 1 byte, 3–4 = 2 bytes, 5–8 = 4 bytes.
- Several values: sequential single bytes, each at most `FF`, up to 64.
- The whole patched range must lie inside the file-backed part of a single
  `PT_LOAD` segment. All patches are checked before any is written, so a bad
  patch never leaves RAM half-patched.

### Limits

Exceeding any limit is an error.

| Limit | Value |
|-------|-------|
| Line length | 255 characters |
| File size | 16 KB |
| `arg=` entries | 12 |
| `irx=` entries | 16 |
| Values in one `patch=` | 64 |
| `argv0` + all `arg=` strings, NULs included | 2048 bytes (provisional) |
| IRX images held in RAM (`reset=1`) | 512 KB |
| `PT_LOAD` segments of a patched ELF | 16 |

### Examples

Plain homebrew with an argument:

```
boot=mass:/FILENAME.ELF
arg=Argument
```

POPStarter-style: mask `POPSTARTER.ELF` as another file in `argv[0]` and apply
patches:

```
boot=mass:/POPS/POPSTARTER.ELF
argv0=mass:/POPS/XX.GAMENAME.ELF
patch=$412 0x01
patch=$413 0x05
```

Reset the IOP and bring your own USB drivers (the module files sit next to
`ELFWD.ELF`; they are read into RAM before the reset):

```
reset=1
irx=usbd.irx
irx=usbhdfsd.irx
boot=mass:/EMBER/ember.elf
arg=Some Game\Some Game.cue
```

## Sequence

1. The whole `ELFWD.CFG` is read into memory and validated. Nothing has been
   touched yet.
2. `reset=1` only: every non-`rom0:` `irx=` is read into RAM.
3. `reset=1` only: the IOP is reset, then RPC is re-initialised.
4. LoadFile and the SBV patches are set up.
5. The `irx=` modules are started in line order.
6. `boot=` is loaded. With `irx=` or `reset=1` the load is retried for about
   5 s (every 250 ms) while storage comes up; otherwise it is tried once.
7. `patch=` entries are applied and the caches are flushed.
8. The target is executed.

## Errors

Any error returns to OSDSYS **without showing anything**:

- unreadable CFG, missing `boot=`, a line without `=`;
- unknown key; duplicate `boot=`, `argv0=` or `reset=`;
- an empty value; `reset=` other than `0`/`1`; any limit exceeded;
- a malformed patch, or a patch outside the loaded segments;
- an `irx=` path starting with `/` or `\`, an unreadable IRX, or a module that
  fails to load;
- `boot=` not loadable after the retries, or `boot=` not an ELF while patches
  are present.

> **Autostart loops:** if your console is set to autostart ELFWD (or a launcher
> that runs it), an error that returns to OSDSYS can start it again and loop.
> Know how to skip the autostart on your setup before testing a new CFG.

## Requirements

- [PS2SDK](https://github.com/ps2dev/ps2sdk) toolchain to build.
- A PS2 browser/launcher able to run a homebrew ELF and already providing
  access to the storage device the CFG lives on.

## Building

```
make                # ELFWD.ELF
make DEBUG=1        # ELFWD_DEBUG.ELF, with error diagnostics
make test           # host-side parser tests (no PS2 toolchain needed)
```

Run `make clean` when switching between the normal and debug builds. Do not
pass `EE_CFLAGS` on the `make` command line: it overrides the `-D_EE` that
PS2SDK adds.

### Debug build

Errors normally leave silently. `ELFWD_DEBUG.ELF` instead holds the screen
border for about 3 seconds in a colour that tells which stage failed:

| Colour  | Stage |
|---------|-------|
| red     | CFG path or read |
| yellow  | CFG validation |
| green   | `irx=` (the border first blinks white N times, N = the failing `irx=` line) |
| blue    | `boot=` load |
| magenta | `patch=` |

If the border does not change, the failure happened earlier, or the launcher
leaves the display off.

## Known limitations

- ELFWD can only patch the ELF it loads directly. It cannot patch a second
  executable that the target loads itself (e.g. a PS1 emulator core
  chain-loaded by a frontend).
- If `boot=` is not a valid ELF (e.g. an encrypted container) and patches are
  present, that is an error; without patches the load simply fails.
- The IOP reset loops (`SifIopReset` / `SifIopSync`) have no timeout, as is
  usual in PS2SDK programs: a reset that never completes hangs the console.
- The 2048-byte `argv` limit is provisional and has not been checked against
  `ExecPS2` on hardware.
- `reset=0` with `irx=` has not been confirmed on hardware with every module.
- For HDD/APA launches, put the exact path you want in `argv0=`: ELFWD passes
  it through untouched.

## License

This project is licensed under the Academic Free License version 2.0
(AFL-2.0). See [`LICENSE`](LICENSE) for the full text.

## Credits

- [PS2SDK](https://github.com/ps2dev/ps2sdk) / [ps2dev](http://www.ps2dev.org)
