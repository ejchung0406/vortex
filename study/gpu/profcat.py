"""Coarse operator classes for a torch.profiler trace, by self CUDA time."""
from torch.autograd import DeviceType

CLASSES = [
    ("linear/matmul/addmm", ("linear", "addmm", "matmul", "aten::mm", "bmm", "baddbmm", "einsum")),
    ("attention (sdpa)", ("scaled_dot_product", "attention")),
    ("conv", ("conv",)),
    ("norm (pow/mean/rsqrt of RMSNorm, layer_norm)", ("norm", "rsqrt", "aten::pow", "aten::mean")),
    ("elementwise mul/add/silu/sigmoid...", ("aten::mul", "aten::add", "aten::sub", "aten::div", "silu", "sigmoid",
                                             "tanh", "gelu", "softmax", "exp", "neg", "softplus")),
    ("copy/cast/cat/index", ("copy", "cat", "index", "gather", "where", "fill", "clone", "stack", "embedding")),
]


def classify(name):
    low = name.lower()
    for cls, keys in CLASSES:
        if any(k in low for k in keys):
            return cls
    return "other"


def op_rows(prof):
    # operator rows only (the kernel rows repeat the same time); self time of an op is the
    # kernels it launched itself, so aten::linear and its child aten::mm are not counted twice
    return [(e.key, e.self_device_time_total, e.count) for e in prof.key_averages()
            if e.device_type == DeviceType.CPU and e.self_device_time_total > 0]


def cuda_ms(prof):
    """(all kernels, matmul kernels) in ms: the time the GPU spent executing, without the host gaps"""
    rows = op_rows(prof)
    return (sum(t for _, t, _ in rows) / 1e3,
            sum(t for n, t, _ in rows if classify(n) == CLASSES[0][0]) / 1e3)


def report(prof, what, units):
    rows = op_rows(prof)
    total = sum(t for _, t, _ in rows)
    print(f"profile of {what}: self CUDA time total {total/1e3:.2f} ms = {total/1e3/units:.3f} ms per unit")
    by = {}
    for name, t, _ in rows:
        by[classify(name)] = by.get(classify(name), 0) + t
    for cls, t in sorted(by.items(), key=lambda kv: -kv[1]):
        print(f"  class {cls:<48s} {t/1e3:9.2f} ms {100*t/total:5.1f}%")
    print("  top operators:")
    for name, t, n in sorted(rows, key=lambda r: -r[1])[:20]:
        print(f"    {name[:60]:<60s} {t/1e3:9.2f} ms {100*t/total:5.1f}% calls={n} [{classify(name)}]")
