import types
import unittest

from test_backend import mir_modes


class ModeTests(unittest.TestCase):
    def assert_modes(self, args, names):
        modes = mir_modes(args)
        self.assertEqual([name for name, _ in modes], names)
        for name, options in modes:
            isel, allocator = name.split("_")
            self.assertEqual(options, ["-backend=mir", f"-isel={isel}",
                                       f"-regalloc={allocator}", "-dump-mir"])

    def test_default_matrix(self):
        self.assert_modes(types.SimpleNamespace(baseline_only=False, regalloc="both"),
                          ["baseline_stack", "baseline_linear", "greedy_stack", "greedy_linear"])

    def test_baseline_only_retains_both_allocators(self):
        self.assert_modes(types.SimpleNamespace(baseline_only=True, regalloc="both"),
                          ["baseline_stack", "baseline_linear"])

    def test_stack_only_retains_both_selectors(self):
        self.assert_modes(types.SimpleNamespace(baseline_only=False, regalloc="stack"),
                          ["baseline_stack", "greedy_stack"])


if __name__ == "__main__":
    unittest.main()
