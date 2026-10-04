# ELFWD

A generic PS2 ELF forwarder: loads a target ELF, optionally disguises
its `argv[0]`/`argv[1]` as a different path, and optionally applies
byte-level patches directly in RAM before executing it — all driven by
a plain-text config file.

## What it does

ELFWD is launched like any other PS2 homebrew ELF. It reads a config
file (`ELFWD.CFG`) placed next to itself, then:

1. Loads and executes a target ELF, optionally passing it a fake
   `argv[0]` (and `argv[1]`) so the target believes it was launched
   from a different path, or with a different argument, than it
   actually was.
2. Optionally applies byte-level patches directly in RAM, after the
   target has been loaded and before it is executed — without ever
   writing to the target file on disk.

ELFWD loads no IOP modules of its own (no storage driver, no memory
card driver): it relies entirely on the IOP module set already
resident from whichever launcher started it. This is what lets it run
from any storage device that launcher already supports, without being
tied to a specific filesystem or driver stack.

## Usage

1. Copy `ELFWD.ELF` and `ELFWD.CFG` to the same folder as the target
   you want to forward to.
2. Launch `ELFWD.ELF` from your PS2 browser/launcher of choice.
3. ELFWD reads `ELFWD.CFG`, loads the target ELF, applies any patches,
   and executes it.

### `ELFWD.CFG` format

**HOMEBREW:**
```
mass:/FILENAME.ELF
@Argument
# comment: load an ELF with an argv[1].
```

**POPSTARTER:**
```
mass:/POPS/POPSTARTER.ELF
mass:/POPS/XX.GAMENAME.ELF
# comment: mask POPSTARTER.ELF as XX.GAMENAME.ELF in argv[0], apply patches for HDTVFIX and USB delay.
$412 0x01
$413 0x05
```

- **Line 1**: path of the target ELF to load and execute.
- **Line 2** *(optional)*: path passed as the target's `argv[0]`. If
  omitted, blank, or if the next non-argv0 directive follows
  immediately, ELFWD passes the real target path from line 1 instead.
- **`@<value>`** *(optional, anywhere after line 1)*: passed as the
  target's `argv[1]`, verbatim (including any quote characters you
  include).
- **Following lines** *(optional)*: patches, one per line.
  - Blank lines and lines starting with `#` are ignored.
  - `// comment` truncates the rest of a line before parsing.
  - `$OFFSET VALUE` — cheat-style single value; write size (1/2/4 bytes,
    little-endian) is inferred from the hex digit count.
  - `$OFFSET B1 B2 B3 ...` — sequential single-byte writes.
  - `$`, `0x`, or no prefix are all accepted on any hex token.
  - `OFFSET` is a **file offset** into the target ELF. ELFWD resolves
    it to the correct RAM address by reading the target's actual ELF
    program headers (`PT_LOAD` segments), so it works correctly
    regardless of how the target was linked.

## Requirements

- [PS2SDK](https://github.com/ps2dev/ps2sdk) toolchain to build.
- A PS2 browser/launcher able to run a homebrew ELF and already
  providing access to whatever storage device the target ELF lives on.

## Building

```
make
```

Produces `ELFWD.ELF`.

## Known limitations

- Patch offsets are matched against `PT_LOAD` segments only; an offset
  outside every loaded segment is silently skipped.
- If the target file is not a valid ELF (e.g. an encrypted container),
  patching is silently skipped and the target still executes unpatched.
- Up to 16 `PT_LOAD` program headers and 64 value tokens per patch line
  are supported; entries beyond that are ignored.
- ELFWD can only patch the target ELF it loads directly. It cannot
  patch a second executable that the target itself loads internally
  (e.g. a PS1 emulator core chain-loaded by a frontend).

## License

This project is licensed under the Academic Free License version 2.0
(AFL-2.0). See [`LICENSE`](LICENSE) for the full text.

## Credits

- [PS2SDK](https://github.com/ps2dev/ps2sdk) / [ps2dev](http://www.ps2dev.org)
