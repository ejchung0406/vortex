// libgng_smcount.so: how many SMs does this process's context actually get?
// Loaded with ctypes inside a PyTorch process (same primary context). Launches
// 8 x attr CTAs of 1024 threads that each record %smid and spin 2 ms, so every
// SM the context may use gets at least one CTA; returns the number of
// distinct %smid values and the device attribute MultiProcessorCount.
// (Under an MPS active-thread percentage %smid is virtual, 0..n-1.)
#include <cuda_runtime.h>
#include <set>
#include <vector>
__global__ void k_ids(int* out, long long spin_ns) {
  if (threadIdx.x == 0) {
    unsigned s; asm volatile("mov.u32 %0, %%smid;" : "=r"(s)); out[blockIdx.x] = (int)s;
    unsigned long long t0, t; asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t0));
    do { asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t)); } while ((long long)(t - t0) < spin_ns);
  }
  __syncthreads();
}
extern "C" int gng_count_sms(int* attr_out, int* max_id_out) {
  int dev = 0, attr = 0; cudaGetDevice(&dev); cudaDeviceGetAttribute(&attr, cudaDevAttrMultiProcessorCount, dev);
  const int n = 8 * 132;
  int* d; if (cudaMalloc(&d, n * sizeof(int)) != cudaSuccess) return -1;
  cudaMemset(d, 0xff, n * sizeof(int));
  k_ids<<<n, 1024>>>(d, 2000000);
  if (cudaDeviceSynchronize() != cudaSuccess) return -2;
  std::vector<int> h(n); cudaMemcpy(h.data(), d, n * sizeof(int), cudaMemcpyDeviceToHost); cudaFree(d);
  std::set<int> s; int mx = -1; for (int x : h) if (x >= 0) { s.insert(x); if (x > mx) mx = x; }
  if (attr_out) *attr_out = attr; if (max_id_out) *max_id_out = mx;
  return (int)s.size();
}
