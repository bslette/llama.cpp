<!-- Mesa MR: source bslette/mesa:nak-sdot-sat (a1d05768327) -> mesa/mesa:main. Labels: NAK, nouveau -->

# nak: lower saturating dot products by clamping the accumulator

NAK sets `lower_iadd_sat`, so `sdot_4x8_iadd_sat` / `sudot_4x8_iadd_sat`
become an IDP plus three compares and three selects, all on the accumulator
chain. vkpeak's int8-dotprod benchmark (`dotPacked4x8AccSatEXT`) is bound by
those ALU ops. This came up in the review of !<IDP.4A MR number>.

This MR instead clamps the accumulator before the IDP, so the add can't
overflow:

    sat(c + d) = clamp(c, INT_MIN - min(d, 0), INT_MAX - max(d, 0)) + d,   d = idp(a, b, 0)

That's exact for any d. The bounds only depend on the dot product, so they
are shared when the same dot product is accumulated repeatedly.

- **SM73+:** two IMNMX plus the IDP on the chain.
- **Volta (no IMNMX):** only one side of the clamp matters for a given sign
  of d, and both bounds are `INT_MAX - d + (d < 0)`. One ISETP, flipped by the
  sign of d, then a SEL, choose between the accumulator and the bound.

## Results

vkpeak f37dab4, `int8-dotprod`, GIOPS:

| GPU | before | after | NVIDIA driver |
|---|---|---|---|
| RTX 4060 Laptop (AD107) | 4731 | 11078 | 7199 (615.78) |
| RTX 3050 (GA107) | 2768 | 6263 | 4162 (595.91) |
| Xavier NX (GV11B) | 532 | 957 | |

When the sources change every step, the bounds can't be shared. In that case
throughput is unchanged within noise on all three GPUs (4060 ~363 -> ~395,
3050 ~236 -> ~240, Xavier 44.0 -> 43.7 G dots/s).

I tried a single lowering for all generations. Using the Volta form on SM73+
loses about a quarter of the vkpeak gain (4060: 8680, 3050: 4815). Using the
IMNMX form on Volta, where each IMNMX becomes a compare and a select, costs
24% in the varying-source case. That's why there are two rules.

Correctness: a standalone Vulkan test runs 262144 cases against a CPU
reference, covering signed and mixed-sign products, accumulators near
INT_MAX/INT_MIN, and repeated and varying accumulation chains. It passes on
all three GPUs before and after, and on the NVIDIA driver and ANV, which
were used to check the test itself. Test source:
https://github.com/bslette/llama.cpp/tree/volta-nvk/docs/volta-nvk/experiments/satdot
