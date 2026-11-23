// bar_vis — after a barrier, every warp of a CTA sees the stores to shared and
// global memory the CTA's other warps made before it, while warps of other
// CTAs keep reads to memory outstanding.

#include <vortex2.h>
#include "common.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <vector>

#define CHECK(expr) do { \
    vx_result_t _r = (expr); \
    if (_r != VX_SUCCESS) { \
        std::fprintf(stderr, "FAIL %s:%d: '%s' returned %s\n", \
                     __FILE__, __LINE__, #expr, vx_result_string(_r)); \
        std::exit(1); \
    } \
} while (0)

namespace {
const char* kernel_file = "kernel.vxbin";
uint32_t    ctas        = 0;  // 0: four per core
uint32_t    warps       = 0;  // per CTA (0: half the core's warps, at least 2)
uint32_t    rounds      = 16;
uint32_t    delay_sweep = 4;
uint32_t    delay_step  = 16;
uint32_t    noise_loads = 64;
uint32_t    noise_words = 1u << 20;
bool        split_load  = false;

void parse_args(int argc, char** argv) {
    int c;
    while ((c = getopt(argc, argv, "g:w:r:s:d:l:n:k:bh")) != -1) {
        switch (c) {
            case 'g': ctas        = std::atoi(optarg); break;
            case 'w': warps       = std::atoi(optarg); break;
            case 'r': rounds      = std::atoi(optarg); break;
            case 's': delay_sweep = std::atoi(optarg); break;
            case 'd': delay_step  = std::atoi(optarg); break;
            case 'l': noise_loads = std::atoi(optarg); break;
            case 'n': noise_words = std::atoi(optarg); break;
            case 'k': kernel_file = optarg;            break;
            case 'b': split_load  = true;              break;
            default:
                std::cout << "Usage: [-g CTAs, even ones check, odd ones load] [-w warps per CTA] [-r rounds] "
                             "[-s delay sweep] [-d delay step] [-l loads per loading thread] [-n buffer words] "
                             "[-b split loads followed by barriers] [-k kernel] [-h]" << std::endl;
                std::exit(c == 'h' ? 0 : -1);
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    parse_args(argc, argv);
    if (delay_sweep == 0) delay_sweep = 1;

    vx_device_h dev = nullptr;
    CHECK(vx_device_open(0, &dev));

    uint64_t nt = 0, nw = 0, nc = 0;
    CHECK(vx_device_query(dev, VX_CAPS_NUM_THREADS, &nt));
    CHECK(vx_device_query(dev, VX_CAPS_NUM_WARPS,   &nw));
    CHECK(vx_device_query(dev, VX_CAPS_NUM_CORES,   &nc));
    if (warps == 0) warps = std::max<uint32_t>(2, nw / 2);
    if (ctas == 0) ctas = 4 * static_cast<uint32_t>(nc);
    if (warps < 2 || warps > nw) {
        std::cout << "-w must be 2.." << nw << " warps" << std::endl;
        return 1;
    }
    const uint32_t T = warps * static_cast<uint32_t>(nt);
    const uint32_t cold_lines = (noise_words - noise_words / 2) / BAR_TEST_LINE_WORDS;
    if (split_load && (rounds == 0 || uint64_t(ctas) * T > cold_lines / rounds)) {
        std::cerr << "-b requires at least one round and room for a cold cache line per thread per round" << std::endl;
        vx_device_release(dev);
        return 1;
    }
    const uint32_t threads = ctas * T;
    std::cout << "bar_vis: ctas=" << ctas << " warps_per_cta=" << warps << " threads=" << threads
              << " rounds=" << rounds << " delay=" << delay_sweep << "x" << delay_step
              << " noise_loads=" << noise_loads << std::endl;

    vx_queue_info_t qi = { sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0 };
    vx_queue_h q = nullptr;
    CHECK(vx_queue_create(dev, &qi, &q));

    vx_module_h mod = nullptr;
    vx_kernel_h kern = nullptr;
    CHECK(vx_module_load_file(dev, kernel_file, &mod));
    CHECK(vx_module_get_kernel(mod, "main", &kern));

    const uint64_t tsize = uint64_t(threads) * sizeof(uint32_t);
    const uint64_t nsize = uint64_t(noise_words) * sizeof(uint32_t);
    vx_buffer_h noise_buf = nullptr, sink_buf = nullptr, gbuf = nullptr, err_buf = nullptr;
    CHECK(vx_buffer_create(dev, nsize, VX_MEM_READ, &noise_buf));
    CHECK(vx_buffer_create(dev, tsize, VX_MEM_WRITE, &sink_buf));
    CHECK(vx_buffer_create(dev, tsize, VX_MEM_READ_WRITE, &gbuf));
    CHECK(vx_buffer_create(dev, tsize, VX_MEM_READ_WRITE, &err_buf));

    kernel_arg_t kernel_arg{};
    kernel_arg.rounds      = rounds;
    kernel_arg.delay_sweep = delay_sweep;
    kernel_arg.delay_step  = delay_step;
    kernel_arg.noise_loads = noise_loads;
    kernel_arg.noise_words = noise_words;
    kernel_arg.split_load  = split_load;
    CHECK(vx_buffer_address(noise_buf, &kernel_arg.noise_addr));
    CHECK(vx_buffer_address(sink_buf,  &kernel_arg.sink_addr));
    CHECK(vx_buffer_address(gbuf,      &kernel_arg.gbuf_addr));
    CHECK(vx_buffer_address(err_buf,   &kernel_arg.err_addr));

    std::vector<uint32_t> h_noise(noise_words);
    for (uint32_t i = 0; i < noise_words; ++i) h_noise[i] = i;
    const uint32_t untouched = 0xffffffffu;
    std::vector<uint32_t> h_err(threads, untouched), h_g(threads, 0);
    CHECK(vx_enqueue_write(q, noise_buf, 0, h_noise.data(), nsize, 0, nullptr, nullptr));
    CHECK(vx_enqueue_write(q, gbuf, 0, h_g.data(), tsize, 0, nullptr, nullptr));
    CHECK(vx_enqueue_write(q, err_buf, 0, h_err.data(), tsize, 0, nullptr, nullptr));

    vx_launch_info_t li{};
    li.struct_size  = sizeof(li);
    li.kernel       = kern;
    li.args_host    = &kernel_arg;
    li.args_size    = sizeof(kernel_arg);
    li.ndim         = 1;
    li.grid_dim[0]  = ctas;
    li.block_dim[0] = T;
    li.lmem_size    = T * sizeof(uint32_t);

    vx_event_h launch_ev = nullptr, read_ev = nullptr;
    CHECK(vx_enqueue_launch(q, &li, 0, nullptr, &launch_ev));
    CHECK(vx_enqueue_read(q, h_err.data(), err_buf, 0, tsize, 1, &launch_ev, &read_ev));
    CHECK(vx_event_wait_value(read_ev, 1, VX_TIMEOUT_INFINITE));

    int errors = 0;
    uint32_t bits = 0;
    for (uint32_t i = 0; i < threads; ++i) {
        if (!split_load && ((i / T) & 1)) {
            continue;
        }
        if (h_err[i] != 0) {
            if (errors < 16)
                std::printf("*** CTA %u thread %u: err=0x%x\n", i / T, i % T, h_err[i]);
            if (h_err[i] != untouched) bits |= h_err[i];
            ++errors;
        }
    }

    vx_event_release(read_ev);
    vx_event_release(launch_ev);
    vx_buffer_release(err_buf);
    vx_buffer_release(gbuf);
    vx_buffer_release(sink_buf);
    vx_buffer_release(noise_buf);
    vx_kernel_release(kern);
    vx_module_release(mod);
    vx_queue_release(q);
    vx_device_dump_perf(dev, stdout);
    vx_device_release(dev);

    if (errors) {
        std::printf("Found %d errors (error bits 0x%x)!\nFAILED!\n", errors, bits);
        return 1;
    }
    std::cout << "PASSED!" << std::endl;
    return 0;
}
