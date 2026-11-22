#include "stream.h"

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  stream(arg);
}
