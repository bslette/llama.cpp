// satdot: check dotPacked4x8AccSatEXT (signed and mixed) against a CPU reference.
// Build: gcc -O2 -o satdot satdot.c -lvulkan ; needs satdot.spv next to it.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define N (64 * 4096)
#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s: %d\n", #x, r_); exit(1); } } while (0)

static int64_t sat32(int64_t v) { return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v; }

static int32_t dot(int32_t a, int32_t b, int b_unsigned)
{
    int32_t d = 0;
    for (int k = 0; k < 4; k++) {
        int32_t x = (int8_t)(a >> (8 * k));
        int32_t y = b_unsigned ? (int32_t)(uint8_t)(b >> (8 * k)) : (int8_t)(b >> (8 * k));
        d += x * y;
    }
    return d;
}

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static uint32_t memtype(VkPhysicalDevice pd, uint32_t bits)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    fprintf(stderr, "no memory type\n");
    exit(1);
}

int main(int argc, char **argv)
{
    const char *spv_path = argc > 1 ? argv[1] : "satdot.spv";
    FILE *f = fopen(spv_path, "rb");
    if (!f) { perror(spv_path); return 1; }
    fseek(f, 0, SEEK_END);
    long spv_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *spv = malloc(spv_size);
    if (fread(spv, 1, spv_size, f) != (size_t)spv_size) return 1;
    fclose(f);

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst;
    CK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s\n", props.deviceName);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                             .shaderIntegerDotProduct = VK_TRUE };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci };
    VkDevice dev;
    CK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    VkBuffer buf[2];
    VkDeviceMemory mem[2];
    void *map[2];
    for (int i = 0; i < 2; i++) {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = N * 16,
                                   .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
        CK(vkCreateBuffer(dev, &bci, NULL, &buf[i]));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev, buf[i], &mr);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                     .memoryTypeIndex = memtype(pd, mr.memoryTypeBits) };
        CK(vkAllocateMemory(dev, &mai, NULL, &mem[i]));
        CK(vkBindBufferMemory(dev, buf[i], mem[i], 0));
        CK(vkMapMemory(dev, mem[i], 0, N * 16, 0, &map[i]));
    }

    // inputs: random bytes, accumulators spread over the range and near both limits
    int32_t *in = map[0];
    for (int i = 0; i < N; i++) {
        int32_t a = rnd(), b = rnd(), c;
        switch (i % 4) {
        case 0: c = rnd(); break;
        case 1: c = INT32_MAX - (int32_t)(rnd() % 0x40000); break;
        case 2: c = INT32_MIN + (int32_t)(rnd() % 0x40000); break;
        default: c = (int32_t)(rnd() % 0x80000) - 0x40000; break;
        }
        if (i % 64 == 0) { a = 0x80808080; b = 0x80808080; }        // largest positive dot
        if (i % 64 == 1) { a = 0x80808080; b = 0x7f7f7f7f; }        // most negative dot
        in[4 * i + 0] = a;
        in[4 * i + 1] = b;
        in[4 * i + 2] = c;
        in[4 * i + 3] = i % 37;
    }
    memset(map[1], 0, N * 16);

    VkDescriptorSetLayoutBinding bind[2] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                              .bindingCount = 2, .pBindings = bind };
    VkDescriptorSetLayout dsl;
    CK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                        .pSetLayouts = &dsl };
    VkPipelineLayout pl;
    CK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = spv_size, .pCode = spv };
    VkShaderModule sm;
    CK(vkCreateShaderModule(dev, &smci, NULL, &sm));
    VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                   .module = sm, .pName = "main" },
        .layout = pl };
    VkPipeline pipe;
    CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                        .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dp;
    CK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
                                         .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet ds;
    CK(vkAllocateDescriptorSets(dev, &dsai, &ds));
    VkDescriptorBufferInfo dbi[2] = { { buf[0], 0, VK_WHOLE_SIZE }, { buf[1], 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
                               .descriptorCount = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               .pBufferInfo = dbi };
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
    VkCommandPool cp;
    CK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb;
    CK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CK(vkBeginCommandBuffer(cb, &cbbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, N / 64, 1, 1);
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    CK(vkQueueWaitIdle(q));

    const int32_t *out = map[1];
    long bad[4] = { 0 }, sat_hits = 0;
    for (int i = 0; i < N; i++) {
        int32_t a = in[4 * i], b = in[4 * i + 1], c = in[4 * i + 2], cnt = in[4 * i + 3];
        int64_t s = sat32((int64_t)c + dot(a, b, 0));
        int64_t m = sat32((int64_t)c + dot(a, b, 1));
        int64_t acc = c, acc2 = c;
        for (int k = 0; k < cnt; k++) {
            acc = sat32(acc + dot(a, b, 0));
            acc2 = sat32(acc2 + dot((int32_t)((uint32_t)a + k), b ^ (k * 0x01010101), 0));
        }
        int64_t want[4] = { s, m, acc, acc2 };
        for (int j = 0; j < 4; j++) {
            if (want[j] == INT32_MAX || want[j] == INT32_MIN) sat_hits++;
            if (out[4 * i + j] != (int32_t)want[j]) {
                if (bad[j]++ < 3)
                    printf("mismatch %d.%d: a=%08x b=%08x c=%d n=%d got %d want %lld\n", i, j, a, b, c, cnt,
                           out[4 * i + j], (long long)want[j]);
            }
        }
    }
    printf("%d cases x 4, %ld saturated results; mismatches: single %ld, mixed %ld, chain %ld, chain-varying %ld\n",
           N, sat_hits, bad[0], bad[1], bad[2], bad[3]);
    return (bad[0] | bad[1] | bad[2] | bad[3]) ? 1 : 0;
}
