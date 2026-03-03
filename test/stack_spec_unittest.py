from __future__ import annotations

import unittest
from pathlib import Path

from compose_tools.common import ToolError
from compose_tools.stack_spec import _parse_services, loadStackSpec


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


class StackSpecTest(unittest.TestCase):
    def test_stack_spec_dev_compose(self) -> None:
        spec = loadStackSpec(mode="dev", compose_dir=_repo_root() / "container/compose")
        self.assertEqual(
            set(spec.containerNamesAll()),
            {"egress_proxy", "servicedb", "seaweedfs", "scalar", "reverse_proxy", "test_target"},
        )
        self.assertEqual(
            set(spec.containerNamesHealthchecked()), {"egress_proxy", "servicedb", "seaweedfs"}
        )

    def test_stack_spec_prodlike_compose(self) -> None:
        spec = loadStackSpec(mode="prodlike", compose_dir=_repo_root() / "container/compose")
        self.assertEqual(set(spec.containerNamesAll()), {"egress_proxy", "servicedb"})
        self.assertEqual(set(spec.containerNamesHealthchecked()), {"egress_proxy", "servicedb"})

    def test_stack_spec_requires_container_name(self) -> None:
        raw = """
version: "3.9"
services:
  a:
    image: foo
"""
        with self.assertRaises(ToolError):
            _parse_services(raw, source="inline")


if __name__ == "__main__":
    unittest.main()
