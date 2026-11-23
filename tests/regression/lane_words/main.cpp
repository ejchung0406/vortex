// lane_words — the coalescer must give every lane its own element whether a
// block's lanes fall in one 64-byte word or several, in lane order or not,
// and must place every lane's store, whole or partial, in its own bytes.
#include <vortex2.h>
#include "common.h"

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
uint32_t    num_iters   = 2 * NUM_PATTERNS * 3;

void parse_args(int argc, char** argv) {
    int c;
    while ((c = getopt(argc, argv, "n:k:h")) != -1) {
        switch (c) {
            case 'n': num_iters   = std::atoi(optarg); break;
            case 'k': kernel_file = optarg;            break;
            default:
                std::cout << "Usage: [-n iterations] [-k kernel] [-h]" << std::endl;
                std::exit(c == 'h' ? 0 : -1);
        }
    }
}

bool lane_active(uint32_t lane, uint32_t k) {
    return (k % 3) != 2 || ((lane * 5 + k) % 7) != 0;
}

} // namespace

int main(int argc, char** argv) {
    parse_args(argc, argv);
    if (num_iters == 0) num_iters = 1;

    vx_device_h dev = nullptr;
    CHECK(vx_device_open(0, &dev));

    uint64_t nt = 0, nw = 0, nc = 0;
    CHECK(vx_device_query(dev, VX_CAPS_NUM_THREADS, &nt));
    CHECK(vx_device_query(dev, VX_CAPS_NUM_WARPS,   &nw));
    CHECK(vx_device_query(dev, VX_CAPS_NUM_CORES,   &nc));
    const uint32_t W = static_cast<uint32_t>(nt);
    const uint32_t num_threads = static_cast<uint32_t>(nt * nw * nc);
    const uint32_t num_warps = num_threads / W;
    const uint32_t groups = (W + 7) / 8;
    const uint64_t elems = uint64_t(num_warps) * num_iters * groups * REGION;

    std::cout << "lane_words: iters=" << num_iters << " threads=" << num_threads
              << " warp=" << W << std::endl;

    vx_queue_info_t qi = { sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0 };
    vx_queue_h q = nullptr;
    CHECK(vx_queue_create(dev, &qi, &q));

    vx_buffer_h src_buf = nullptr, dst_buf = nullptr, err_buf = nullptr;
    CHECK(vx_buffer_create(dev, elems * sizeof(uint64_t), VX_MEM_READ, &src_buf));
    CHECK(vx_buffer_create(dev, elems * sizeof(uint64_t), VX_MEM_READ_WRITE, &dst_buf));
    CHECK(vx_buffer_create(dev, num_threads * sizeof(uint32_t), VX_MEM_WRITE, &err_buf));

    vx_module_h mod = nullptr;
    vx_kernel_h kern = nullptr;
    CHECK(vx_module_load_file(dev, kernel_file, &mod));
    CHECK(vx_module_get_kernel(mod, "main", &kern));

    kernel_arg_t kernel_arg{};
    kernel_arg.num_iters   = num_iters;
    kernel_arg.num_threads = num_threads;
    kernel_arg.warp_size   = W;
    kernel_arg.groups      = groups;
    CHECK(vx_buffer_address(src_buf, &kernel_arg.src_addr));
    CHECK(vx_buffer_address(dst_buf, &kernel_arg.dst_addr));
    CHECK(vx_buffer_address(err_buf, &kernel_arg.err_addr));

    std::vector<uint64_t> h_src(elems), h_dst(elems, 0);
    for (uint64_t e = 0; e < elems; ++e) h_src[e] = src_value(e);
    CHECK(vx_enqueue_write(q, src_buf, 0, h_src.data(), elems * sizeof(uint64_t), 0, nullptr, nullptr));
    CHECK(vx_enqueue_write(q, dst_buf, 0, h_dst.data(), elems * sizeof(uint64_t), 0, nullptr, nullptr));

    uint32_t grid[1], block[1];
    CHECK(vx_device_max_occupancy_grid(dev, 1, &num_threads, grid, block));

    vx_launch_info_t li{};
    li.struct_size = sizeof(li);
    li.kernel      = kern;
    li.args_host   = &kernel_arg;
    li.args_size   = sizeof(kernel_arg);
    li.ndim        = 1;
    li.grid_dim[0] = grid[0];
    li.block_dim[0]= block[0];

    std::vector<uint32_t> h_err(num_threads);
    vx_event_h launch_ev = nullptr, read_ev = nullptr;
    CHECK(vx_enqueue_launch(q, &li, 0, nullptr, &launch_ev));
    CHECK(vx_enqueue_read(q, h_err.data(), err_buf, 0, num_threads * sizeof(uint32_t), 1, &launch_ev, nullptr));
    CHECK(vx_enqueue_read(q, h_dst.data(), dst_buf, 0, elems * sizeof(uint64_t), 1, &launch_ev, &read_ev));
    CHECK(vx_event_wait_value(read_ev, 1, VX_TIMEOUT_INFINITE));

    int errors = 0;
    for (uint32_t t = 0; t < num_threads; ++t) {
        if (h_err[t] == 0) continue;
        if (errors < 16)
            std::printf("*** thread %u: %u wrong loads\n", t, h_err[t]);
        errors += h_err[t];
    }

    std::vector<uint64_t> expect(elems, 0);
    for (uint32_t warp = 0; warp < num_warps; ++warp) {
        for (uint32_t k = 0; k < num_iters; ++k) {
            uint32_t p = k % NUM_PATTERNS;
            if (p >= STORE_PATTERNS) continue;
            uint64_t slot = uint64_t(warp) * num_iters + k;
            for (uint32_t lane = 0; lane < W; ++lane) {
                if (!lane_active(lane, k)) continue;
                uint64_t e = (slot * groups + lane / 8) * REGION + lane_elem[p][lane % 8];
                uint64_t v = src_value(e);
                expect[e] = (k & 1) ? uint64_t(uint32_t(v >> 7)) : ~v;
            }
        }
    }
    for (uint64_t e = 0; e < elems; ++e) {
        if (h_dst[e] == expect[e]) continue;
        if (errors < 16)
            std::printf("*** dst[%lu] expected=0x%lx actual=0x%lx\n",
                        (unsigned long)e, (unsigned long)expect[e], (unsigned long)h_dst[e]);
        ++errors;
    }

    vx_event_release(launch_ev);
    vx_event_release(read_ev);
    vx_buffer_release(src_buf);
    vx_buffer_release(dst_buf);
    vx_buffer_release(err_buf);
    vx_kernel_release(kern);
    vx_module_release(mod);
    vx_queue_release(q);
    vx_device_dump_perf(dev, stdout);
    vx_device_release(dev);

    if (errors != 0) {
        std::printf("FAILED! %d errors\n", errors);
        return 1;
    }
    std::printf("PASSED!\n");
    return 0;
}
