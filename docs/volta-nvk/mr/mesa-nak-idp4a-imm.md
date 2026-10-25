<!-- Mesa MR: source bslette/mesa:nak-idp4a-imm (8b1aed83c81, 1d73194871f) -> mesa/mesa:main. Labels: NAK, nouveau -->

# nak: never encode IDP.4A with an immediate source

`OpIDp4::legalize()` only moves src1 to a register when it is a negative
immediate, so a positive immediate is encoded with the immediate form of
IDP.4A (0x826). That form doesn't exist: the GPU raises an Illegal Instruction
Encoding exception, and nvdisasm rejects the encoding for SM72, SM75 and SM86.

llama.cpp's Vulkan backend hits this in its iq1_m mat-vec shaders
(`dp4a(x, 0x01010101)`). Every iq1_m MUL_MAT / MUL_MAT_ID case in
`test-backend-ops` loses the device:

- RTX 4060 Laptop (AD107): `Xid:13 Graphics SM Warp Exception ... Illegal Instruction Encoding`, then the channel is killed
- RTX 3050 (GA107): device lost
- Xavier NX (GV11B, SM72): ILLEGAL_INSTR_ENCODING trap

With this MR, on the RTX 4060 and the RTX 3050 (NVK built from this branch on
main e7371b0711b), all of the following pass:

| test-backend-ops | RTX 4060 | RTX 3050 |
|---|---|---|
| MUL_MAT, iq1_m only | 49/49 | 49/49 |
| MUL_MAT_ID, iq1_m only | 4/4 | 4/4 |
| MUL_MAT (all types) | 2275/2275 | 2275/2275 |
| MUL_MAT_ID (all types) | 1109/1109 | 1109/1109 |
| FLASH_ATTN_EXT | 5368/5368 | 5368/5368 |

The second commit adds a hardware test (`test_op_idp4_imm`) covering both
source orders and all U8/I8 type combinations, using a mixed-sign immediate.
