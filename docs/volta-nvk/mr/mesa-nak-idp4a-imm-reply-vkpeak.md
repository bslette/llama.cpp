No, it doesn't change it. vkpeak's int8-dotprod shader only takes register
sources, so it never hit the immediate form (RTX 4060 Laptop: 4733 GIOPS
before, 4730 after; the NVIDIA driver gets 7199).

The bottleneck was the saturation. `dotPacked4x8AccSatEXT` was lowered to an
IDP plus three compares and three selects on the accumulator chain. I tried
clamping the accumulator before the IDP instead, so the add can't overflow:

    sat(c + d) = clamp(c, INT_MIN - min(d, 0), INT_MAX - max(d, 0)) + d

It's exact for any d. On SM73+ that's two IMNMX plus the IDP; Volta, which has
no IMNMX, uses a single compare and select. vkpeak int8-dotprod:

| GPU | before | after |
|---|---|---|
| RTX 4060 Laptop (AD107) | 4731 | 11078 |
| RTX 3050 (GA107) | 2768 | 6263 |
| Xavier NX (GV11B) | 532 | 957 |

When the sources change every step, throughput is unchanged. It's in a
separate MR: !<sdot-sat MR number> (branch bslette/mesa `nak-sdot-sat`).
