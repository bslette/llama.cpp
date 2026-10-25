// rpmtest: does a Vulkan device survive the GPU runtime-suspending while it is open?
// Puts the satdot inputs and a ballast buffer in device-local memory, idles for
// RPM_SLEEP seconds (default 15) so the GPU can suspend, then runs satdot.spv from
// VRAM and reads ballast samples back. RPM_MB = ballast size (default 2048),
// RPM_ROUNDS = sleep/check rounds (default 3).
// Build: gcc -O2 -o rpmtest rpmtest.c -lvulkan
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define N (64 * 4096)
#define CHUNK (1 << 20)
#define SAMPLE 4096
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

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec * 1e-6;
}

static VkPhysicalDevice pd;
static VkDevice dev;

static void make_buffer(VkDeviceSize size, VkMemoryPropertyFlags want, VkBuffer *buf, VkDeviceMemory *mem, void **map)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                               .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    CK(vkCreateBuffer(dev, &bci, NULL, buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, *buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want &&
            (map || !(mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)))
            type = i;
    if (type == UINT32_MAX) { fprintf(stderr, "no memory type\n"); exit(1); }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = type };
    CK(vkAllocateMemory(dev, &mai, NULL, mem));
    CK(vkBindBufferMemory(dev, *buf, *mem, 0));
    if (map)
        CK(vkMapMemory(dev, *mem, 0, size, 0, map));
}

int main(int argc, char **argv)
{
    const char *spv_path = argc > 1 ? argv[1] : "satdot.spv";
    int sleep_s = getenv("RPM_SLEEP") ? atoi(getenv("RPM_SLEEP")) : 15;
    int ballast_mb = getenv("RPM_MB") ? atoi(getenv("RPM_MB")) : 2048;
    int rounds = getenv("RPM_ROUNDS") ? atoi(getenv("RPM_ROUNDS")) : 3;

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
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s, ballast %d MiB, sleep %d s, %d rounds\n", props.deviceName, ballast_mb, sleep_s, rounds);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                             .shaderIntegerDotProduct = VK_TRUE };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci };
    CK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkBuffer hin, hout, vin, ballast, hsample;
    VkDeviceMemory hin_m, hout_m, vin_m, ballast_m, hsample_m;
    void *hin_p, *hout_p, *hsample_p;
    int chunks = ballast_mb;
    make_buffer(N * 16, host, &hin, &hin_m, &hin_p);
    make_buffer(N * 16, host, &hout, &hout_m, &hout_p);
    make_buffer((VkDeviceSize)chunks * SAMPLE, host, &hsample, &hsample_m, &hsample_p);
    make_buffer(N * 16, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &vin, &vin_m, NULL);
    make_buffer((VkDeviceSize)chunks * CHUNK, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ballast, &ballast_m, NULL);

    int32_t *in = hin_p;
    for (int i = 0; i < N; i++) {
        int32_t a = rnd(), b = rnd(), c;
        switch (i % 4) {
        case 0: c = rnd(); break;
        case 1: c = INT32_MAX - (int32_t)(rnd() % 0x40000); break;
        case 2: c = INT32_MIN + (int32_t)(rnd() % 0x40000); break;
        default: c = (int32_t)(rnd() % 0x80000) - 0x40000; break;
        }
        in[4 * i + 0] = a;
        in[4 * i + 1] = b;
        in[4 * i + 2] = c;
        in[4 * i + 3] = i % 37;
    }

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
    VkDescriptorBufferInfo dbi[2] = { { vin, 0, VK_WHOLE_SIZE }, { hout, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
                               .descriptorCount = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               .pBufferInfo = dbi };
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
    VkCommandPool cp;
    CK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2 };
    VkCommandBuffer cbs[2];
    CK(vkAllocateCommandBuffers(dev, &cbai, cbs));
    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT };

    // setup: inputs to VRAM, ballast chunk i filled with 0x5a000000 + i
    CK(vkBeginCommandBuffer(cbs[0], &cbbi));
    VkBufferCopy cin = { 0, 0, N * 16 };
    vkCmdCopyBuffer(cbs[0], hin, vin, 1, &cin);
    for (int i = 0; i < chunks; i++)
        vkCmdFillBuffer(cbs[0], ballast, (VkDeviceSize)i * CHUNK, CHUNK, 0x5a000000u + i);
    vkCmdPipelineBarrier(cbs[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                         NULL, 0, NULL);
    CK(vkEndCommandBuffer(cbs[0]));

    // check: compute from VRAM, read the first 4 KiB of every ballast chunk back
    CK(vkBeginCommandBuffer(cbs[1], &cbbi));
    vkCmdBindPipeline(cbs[1], VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cbs[1], VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cbs[1], N / 64, 1, 1);
    VkBufferCopy *regions = calloc(chunks, sizeof(*regions));
    for (int i = 0; i < chunks; i++)
        regions[i] = (VkBufferCopy){ (VkDeviceSize)i * CHUNK, (VkDeviceSize)i * SAMPLE, SAMPLE };
    vkCmdCopyBuffer(cbs[1], ballast, hsample, chunks, regions);
    CK(vkEndCommandBuffer(cbs[1]));

    double t = now_ms();
    VkSubmitInfo si0 = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cbs[0] };
    CK(vkQueueSubmit(q, 1, &si0, VK_NULL_HANDLE));
    CK(vkQueueWaitIdle(q));
    printf("setup: %.1f ms\n", now_ms() - t);
    memset(hin_p, 0, N * 16);   // the inputs now only exist in VRAM

    int failed = 0;
    for (int round = 1; round <= rounds; round++) {
        printf("round %d: sleeping %d s\n", round, sleep_s);
        fflush(stdout);
        sleep(sleep_s);
        memset(hout_p, 0, N * 16);
        memset(hsample_p, 0, (size_t)chunks * SAMPLE);
        t = now_ms();
        VkSubmitInfo si1 = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cbs[1] };
        VkResult r = vkQueueSubmit(q, 1, &si1, VK_NULL_HANDLE);
        if (!r)
            r = vkQueueWaitIdle(q);
        double dt = now_ms() - t;
        if (r) {
            printf("round %d: submit/wait failed with %d after %.1f ms\n", round, r, dt);
            return 2;
        }

        rng = 12345;
        long bad = 0, bad_ballast = 0;
        const int32_t *out = hout_p;
        for (int i = 0; i < N; i++) {
            int32_t a = rnd(), b = rnd(), c;
            switch (i % 4) {
            case 0: c = rnd(); break;
            case 1: c = INT32_MAX - (int32_t)(rnd() % 0x40000); break;
            case 2: c = INT32_MIN + (int32_t)(rnd() % 0x40000); break;
            default: c = (int32_t)(rnd() % 0x80000) - 0x40000; break;
            }
            if (out[4 * i] != (int32_t)sat32((int64_t)c + dot(a, b, 0)) ||
                out[4 * i + 1] != (int32_t)sat32((int64_t)c + dot(a, b, 1)))
                bad++;
        }
        const uint32_t *smp = hsample_p;
        for (int i = 0; i < chunks; i++)
            for (int j = 0; j < SAMPLE / 4; j++)
                if (smp[i * (SAMPLE / 4) + j] != 0x5a000000u + i) {
                    if (bad_ballast++ < 3)
                        printf("ballast chunk %d word %d: %08x\n", i, j, smp[i * (SAMPLE / 4) + j]);
                    break;
                }
        printf("round %d: %.1f ms, compute mismatches %ld/%d, ballast chunks bad %ld/%d\n", round, dt, bad, N,
               bad_ballast, chunks);
        failed |= bad || bad_ballast;
    }
    return failed;
}
