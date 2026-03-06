from __future__ import annotations

import subprocess
import unittest
from unittest import mock

import compose_tools.podman_compose as podman_compose


class PodmanComposeTimeoutsTest(unittest.TestCase):
    def setUp(self) -> None:
        podman_compose._checked_compose = False

    def test_require_podman_compose_has_timeout(self) -> None:
        with (
            mock.patch.object(podman_compose, "need_cmd") as need_cmd,
            mock.patch.object(podman_compose, "run") as run,
        ):
            need_cmd.return_value = None
            run.return_value = subprocess.CompletedProcess(
                ["podman", "compose", "version"], 0, stdout="", stderr=""
            )
            podman_compose.require_podman_compose()
            _, kwargs = run.call_args
            self.assertIn("timeout_sec", kwargs)
            self.assertIsInstance(kwargs["timeout_sec"], float)

    def test_podman_inspect_state_has_timeout(self) -> None:
        with (
            mock.patch.object(podman_compose, "require_podman_compose") as require,
            mock.patch.object(podman_compose, "run") as run,
        ):
            require.return_value = None
            run.return_value = subprocess.CompletedProcess(
                ["podman", "inspect"], 0, stdout="running|healthy|0\n", stderr=""
            )
            state = podman_compose.podman_inspect_state("svc")
            self.assertEqual(state.status, "running")
            _, kwargs = run.call_args
            self.assertIn("timeout_sec", kwargs)
            self.assertIsInstance(kwargs["timeout_sec"], float)

    def test_wait_healthy_healthcheck_run_has_timeout(self) -> None:
        states = [
            podman_compose.ContainerState(status="running", health="", exit_code=0),
            podman_compose.ContainerState(status="running", health="healthy", exit_code=0),
        ]
        with (
            mock.patch.object(podman_compose, "_inspect_with_retries") as inspect,
            mock.patch.object(podman_compose, "run") as run,
            mock.patch.object(podman_compose, "print"),
        ):
            inspect.side_effect = states
            run.return_value = subprocess.CompletedProcess(
                ["podman", "healthcheck", "run"], 0, stdout="", stderr=""
            )
            podman_compose.wait_healthy("svc", timeout_sec=1)
            _, kwargs = run.call_args
            self.assertIn("timeout_sec", kwargs)
            self.assertIsInstance(kwargs["timeout_sec"], float)


if __name__ == "__main__":
    unittest.main()
