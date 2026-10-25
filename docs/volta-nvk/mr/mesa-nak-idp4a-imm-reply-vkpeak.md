No, it doesn't change it. vkpeak's int8-dotprod shader only takes register
sources (`a = gx`, `b = lx`), so it never hit the immediate form.

RTX 4060 Laptop (AD107), vkpeak f37dab4, `vkpeak 0 int8-dotprod`, 3 runs each:

| | GIOPS |
|---|---|
| NVK, main 02f13375 (without this MR) | 4733 |
| NVK, this MR (on main e7371b0711b) | 4730 |
| NVIDIA 615.78.08 | 7199 |

What's slow is the saturation, not the IDP. The shader uses
`dotPacked4x8AccSatEXT`, and NAK lowers `sdot_4x8_iadd_sat` to a plain
`idp4` plus an overflow check, so each saturating dot product is seven
instructions on one dependency chain:

```
r5 = idp4.i8.i8 r1 r0 r4
p0 = isetp.lt.i32 r5 r2
r6 = sel !p0 r5 0x7fffffff
p0 = isetp.lt.i32 r2 r5
r5 = sel !p0 r5 0x80000000
p0 = isetp.ge.i32 r4 0x1
r4 = sel p0 r6 r5
```

`r2` is `idp4(a, b, 0)`, which is hoisted out of the loop. So int8-dotprod on
NVK is measuring that sequence rather than IDP throughput. A cheaper lowering
of the saturating form, or a native one if the hardware has it, would be the
thing that moves it.

Follow-up: I tried a cheaper lowering. Clamp the accumulator first, then let
IDP do the add, which can then no longer overflow:

    sat(c + d) = clamp(c, INT_MIN - min(d, 0), INT_MAX - max(d, 0)) + d,   d = idp(a, b, 0)

It's exact for any d, and the bounds only depend on the dot product, so the
accumulator chain becomes IMNMX, IMNMX, IDP. It's a late NAK algebraic rule
for SM73+ (Volta has no IMNMX), with `has_sdot_4x8_sat`/`has_sudot_4x8_sat`
set so the op gets that far.

| vkpeak int8-dotprod (GIOPS) | before | after |
|---|---|---|
| RTX 4060 Laptop (AD107) | 4731 | 11078 |
| RTX 3050 (GA107) | 2768 | 6256 |

The NVIDIA driver on the same 4060 gets 7199. When the sources change every
step, so the bounds can't be shared, throughput is unchanged (within noise).
A 262144-case test against a CPU reference (signed and mixed-sign,
accumulators near INT_MAX/INT_MIN, repeated and varying chains) passes. I can
put it up as a separate MR.
