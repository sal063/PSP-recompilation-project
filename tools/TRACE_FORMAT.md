# CPU-state trace format


## Per-step line

```
<step> pc=<hpc> op=<hword> <reg-writes> <mem-writes>
```

- `<step>`     decimal step index, starting at 0, monotonically increasing by 1.
- `pc=<hpc>`   the guest program counter of the instruction, `0x` + 8 lowercase hex digits.
- `op=<hword>` the 32-bit instruction word, `0x` + 8 lowercase hex digits.
- `<reg-writes>` zero or more register-change tokens, space-separated, in this order:
  general-purpose `r<n>` (n in 1..31; r0 is never written), then `hi`, `lo`, then float
  `f<n>` (n in 0..31), then `fcr31`, then VFPU `v<n>` (n in 0..127). Each token is
  `name=0x<8 hex>`. Only registers whose value changed in this step are listed. Float and
  VFPU values are the raw 32-bit IEEE-754 bit pattern, not a decimal rendering, so the
  comparison is exact and not subject to printf rounding.
- `<mem-writes>` zero or more memory-write tokens, space-separated, ascending by address:
  `m<size>[<haddr>]=0x<hex>` where `<size>` is 8, 16, or 32, `<haddr>` is `0x` + 8 hex
  digits (guest virtual address), and the value has 2, 4, or 8 hex digits matching the size.

Empty groups contribute no tokens (no trailing spaces). A step that changes nothing but the
PC is still emitted (it has `pc=` and `op=` and no write tokens), so step counts line up.

## Example

```
0 pc=0x08900100 op=0x27bdfff0 r29=0x09ffff00
1 pc=0x08900104 op=0xafbf000c m32[0x09ffff0c]=0x08900200
2 pc=0x08900108 op=0x8c880000 r8=0x0000002a
```

## Header

The first line of a trace file is a header comment, ignored by the diff tool except that
both files must carry one:

```
# psp-recomp trace v1 target=<name> oracle=<ppsspp|interp|recomp> start_pc=<hpc> steps=<N>
```

The diff tool only compares the step lines, ignoring this header.
