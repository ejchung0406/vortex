// war_order — a thread's accesses to its own word complete in program order
// (write after read, read after write, write after write) while the word's
// line fills, forwards and replays the chains of other warps.

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
uint32_t    num_words   = 65536;
uint32_t    delay_sweep = 8;
uint32_t    delay_step  = 3;
uint32_t    store_sweep = 1;
uint32_t    store_step  = 0;
int         layout      = -1; // -1: both, one launch each

void parse_args(int argc, char** argv) {
    int c;
    while ((c = getopt(argc, argv, "n:s:d:S:D:l:k:h")) != -1) {
        switch (c) {
            case 'n': num_words   = std::atoi(optarg); break;
            case 's': delay_sweep = std::atoi(optarg); break;
            case 'd': delay_step  = std::atoi(optarg); break;
            case 'S': store_sweep = std::atoi(optarg); break;
            case 'D': store_step  = std::atoi(optarg); break;
            case 'l': layout      = std::atoi(optarg); break;
            case 'k': kernel_file = optarg;            break;
            default:
                std::cout << "Usage: [-n words] [-s load delay sweep] [-d load delay step] [-S store delay sweep] [-D store delay step] [-l layout 0|1, default both] [-k kernel] [-h]" << std::endl;
                std::exit(c == 'h' ? 0 : -1);
        }
    }
}

uint32_t init_word(uint32_t i) { return i * 7 + 1; }
} // namespace

int main(int argc, char** argv) {
    parse_args(argc, argv);
    if (delay_sweep == 0) delay_sweep = 1;
    if (store_sweep == 0) store_sweep = 1;

    vx_device_h dev = nullptr;
    CHECK(vx_device_open(0, &dev));

    // Fill every warp of every core.
    uint64_t nt = 0, nw = 0, nc = 0;
    CHECK(vx_device_query(dev, VX_CAPS_NUM_THREADS, &nt));
    CHECK(vx_device_query(dev, VX_CAPS_NUM_WARPS,   &nw));
    CHECK(vx_device_query(dev, VX_CAPS_NUM_CORES,   &nc));
    const uint32_t num_threads = static_cast<uint32_t>(nt * nw * nc);
    std::cout << "war_order: words=" << num_words << " threads=" << num_threads
              << " delay=" << delay_sweep << "x" << delay_step
              << " store_delay=" << store_sweep << "x" << store_step << std::endl;
    if (num_words < num_threads) {
        std::cout << "words (" << num_words << ") must cover one per thread (" << num_threads << ")" << std::endl;
        return 1;
    }

    vx_queue_info_t qi = { sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0 };
    vx_queue_h q = nullptr;
    CHECK(vx_queue_create(dev, &qi, &q));

    const uint64_t data_size = uint64_t(num_words) * sizeof(uint32_t);

    vx_module_h mod = nullptr;
    vx_kernel_h kern = nullptr;
    CHECK(vx_module_load_file(dev, kernel_file, &mod));
    CHECK(vx_module_get_kernel(mod, "main", &kern));

    // buffers of their own per launch, all live at once, so every launch
    // starts on cold lines
    vx_buffer_h data_bufs[2] = {}, err_bufs[2] = {};
    for (int lay = 0; lay < 2; ++lay) {
        CHECK(vx_buffer_create(dev, data_size, VX_MEM_READ_WRITE, &data_bufs[lay]));
        CHECK(vx_buffer_create(dev, data_size, VX_MEM_READ_WRITE, &err_bufs[lay]));
    }

    int errors = 0;
    uint32_t bits = 0;
    for (int lay = 0; lay < 2; ++lay) {
        if (layout >= 0 && lay != layout) continue;
        std::cout << "layout " << lay << (lay ? ": a warp's words contiguous" : ": a line's words in different warps") << std::endl;
        vx_buffer_h data_buf = data_bufs[lay], err_buf = err_bufs[lay];
        kernel_arg_t kernel_arg{};
        kernel_arg.num_words   = num_words;
        kernel_arg.num_threads = num_threads;
        kernel_arg.num_warps   = num_threads / static_cast<uint32_t>(nt);
        kernel_arg.warp_size   = static_cast<uint32_t>(nt);
        kernel_arg.delay_sweep = delay_sweep;
        kernel_arg.delay_step  = delay_step;
        kernel_arg.store_sweep = store_sweep;
        kernel_arg.store_step  = store_step;
        kernel_arg.layout      = lay;
        CHECK(vx_buffer_address(data_buf, &kernel_arg.data_addr));
        CHECK(vx_buffer_address(err_buf,  &kernel_arg.err_addr));

        // words past the last whole iteration are not touched: their error word
        // stays at this marker
        const uint32_t untouched = 0xffffffffu;
        std::vector<uint32_t> h_data(num_words), h_err(num_words, untouched);
        for (uint32_t i = 0; i < num_words; ++i) h_data[i] = init_word(i);
        CHECK(vx_enqueue_write(q, data_buf, 0, h_data.data(), data_size, 0, nullptr, nullptr));
        CHECK(vx_enqueue_write(q, err_buf, 0, h_err.data(), data_size, 0, nullptr, nullptr));

        uint32_t grid[1], block[1];
        CHECK(vx_device_max_occupancy_grid(dev, 1, &num_threads, grid, block));
        if (grid[0] * block[0] != num_threads) {
            std::cout << "launch of " << grid[0] << "x" << block[0] << " does not cover "
                      << num_threads << " threads" << std::endl;
            return 1;
        }

        vx_launch_info_t li{};
        li.struct_size = sizeof(li);
        li.kernel      = kern;
        li.args_host   = &kernel_arg;
        li.args_size   = sizeof(kernel_arg);
        li.ndim        = 1;
        li.grid_dim[0] = grid[0];
        li.block_dim[0]= block[0];

        vx_event_h launch_ev = nullptr, read_ev = nullptr;
        CHECK(vx_enqueue_launch(q, &li, 0, nullptr, &launch_ev));
        CHECK(vx_enqueue_read(q, h_err.data(), err_buf, 0, data_size, 1, &launch_ev, nullptr));
        CHECK(vx_enqueue_read(q, h_data.data(), data_buf, 0, data_size, 1, &launch_ev, &read_ev));
        CHECK(vx_event_wait_value(read_ev, 1, VX_TIMEOUT_INFINITE));

        const uint32_t touched = (num_words / num_threads) * num_threads;
        for (uint32_t i = 0; i < num_words; ++i) {
            bool used = (i < touched);
            uint32_t want_data = used ? (WAR_MARK3 + i) : init_word(i);
            uint32_t want_err  = used ? 0 : untouched;
            if (h_data[i] != want_data || h_err[i] != want_err) {
                if (errors < 16)
                    std::printf("*** word %u: data=0x%x (expected 0x%x) err=0x%x\n", i, h_data[i], want_data, h_err[i]);
                if (used && h_err[i] != untouched) bits |= h_err[i];
                ++errors;
            }
        }

        vx_event_release(read_ev);
        vx_event_release(launch_ev);
    }
    for (int lay = 0; lay < 2; ++lay) {
        vx_buffer_release(err_bufs[lay]);
        vx_buffer_release(data_bufs[lay]);
    }

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
