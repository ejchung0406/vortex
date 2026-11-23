#!/usr/bin/env python3
# Copyright © 2019-2026
#
# Licensed under the Apache License, Version 2.0 (the "License").
"""ci/testcase.py: the xlen a cycle gate is planned at.

  python3 -m pytest ci/test_testcase.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testcase as tc  # noqa: E402


def _spec(entry, defaults):
    entry = dict({"id": "t", "app": "sgemm"}, **entry)
    return tc.Spec("core", entry, "rtlsim", defaults)


def test_cycle_gate_defaults_to_rv32_whatever_the_file_default():
    for check in ("model_parity", "perf_gate"):
        assert _spec({"check": check}, {}).xlens == [32]
        assert _spec({"check": check}, {"xlen": [32, 64]}).xlens == [32]
        assert _spec({"check": check}, {"xlen": [64]}).xlens == [32]


def test_cycle_gate_runs_at_the_xlen_the_case_names():
    for check in ("model_parity", "perf_gate"):
        assert _spec({"check": check, "xlen": [64]}, {"xlen": [32]}).xlens == [64]
        assert _spec({"check": check, "xlen": [32]}, {"xlen": [32, 64]}).xlens == [32]
        assert _spec({"check": check, "xlen": [32, 64]}, {}).xlens == [32, 64]


def test_functional_case_follows_the_file_default():
    assert _spec({}, {"xlen": [32, 64]}).xlens == [32, 64]
    assert _spec({"xlen": [64]}, {"xlen": [32, 64]}).xlens == [64]
