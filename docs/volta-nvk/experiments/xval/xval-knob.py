import sys
p = sys.argv[1] + '/ggml/src/ggml-vulkan/ggml-vulkan.cpp'
s = open(p).read()
def sub(s, a, b):
    assert s.count(a) == 1, a[:80]
    return s.replace(a, b)
s = sub(s, '''        device->mmvq_bda = device->mmvq_row_groups > 1 && device->buffer_device_address && device->shader_int64;
''', '''        device->mmvq_bda = device->mmvq_row_groups > 1 && device->buffer_device_address && device->shader_int64;
        // EXPERIMENT (cross-GPU validation only): the A_BDA MMVQ variants on any device
        if (getenv("GGML_VK_MMVQ_BDA")) {
            device->mmvq_bda = device->buffer_device_address && device->shader_int64;
        }
''')
s = sub(s, '''    if (src0_type == GGML_TYPE_Q6_K && !mmvq_q6) {''', '''    // EXPERIMENT (cross-GPU validation only): q6_K through MMVQ on any device
    if (getenv("GGML_VK_MMVQ_Q6")) {
        mmvq_q6 = true;
    }
    if (src0_type == GGML_TYPE_Q6_K && !mmvq_q6) {''')
open(p, 'w').write(s)
print('ok')
