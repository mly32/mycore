from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock


LAUNCHER_PATH = Path(__file__).parents[1] / "games" / "dots" / "tools" / "dots_session.py"
SPEC = importlib.util.spec_from_file_location("dots_session", LAUNCHER_PATH)
assert SPEC is not None and SPEC.loader is not None
dots_session = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dots_session)


class DotsSessionLauncherTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory(prefix="dots-session-test-")
        self.fixture = Path(self.directory.name) / "fixture.py"
        self.fixture.write_text(
            textwrap.dedent(
                """
                import sys
                import time
                from pathlib import Path
                import signal

                mode = sys.argv[1]
                if mode == "server":
                    print("DOTS_SERVER_READY 127.0.0.1:27020", flush=True)
                    while True:
                        time.sleep(0.05)
                if mode == "silent-server":
                    time.sleep(5)
                if mode == "client":
                    time.sleep(float(sys.argv[3]) if len(sys.argv) > 3 else 0.05)
                    raise SystemExit(int(sys.argv[2]))
                if mode == "bot":
                    marker = Path(sys.argv[2])
                    def stop(*_args):
                        marker.write_text("stopped", encoding="utf-8")
                        raise SystemExit(0)
                    signal.signal(signal.SIGTERM, stop)
                    while True:
                        time.sleep(0.05)
                """
            ),
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.directory.cleanup()

    def command(self, *arguments: str) -> list[str]:
        return [sys.executable, str(self.fixture), *arguments]

    def test_successful_clients_end_session_and_stop_server(self) -> None:
        result = dots_session.run_session(
            self.command("server"),
            [self.command("client", "0"), self.command("client", "0")],
        )
        self.assertEqual(result, 0)

    def test_client_failure_propagates(self) -> None:
        result = dots_session.run_session(
            self.command("server"),
            [self.command("client", "7")],
        )
        self.assertEqual(result, 7)

    def test_client_exit_stops_running_bots(self) -> None:
        marker = Path(self.directory.name) / "bot-stopped.txt"
        launched_processes = []
        original_popen = subprocess.Popen

        def start_process(*arguments, **keyword_arguments):
            process = original_popen(*arguments, **keyword_arguments)
            launched_processes.append(process)
            return process

        with mock.patch.object(dots_session.subprocess, "Popen", side_effect=start_process):
            result = dots_session.run_session(
                self.command("server"),
                [self.command("client", "0")],
                [self.command("bot", str(marker))],
            )
        self.assertEqual(result, 0)
        self.assertEqual(len(launched_processes), 3)
        self.assertIsNotNone(launched_processes[2].poll())

    def test_session_waits_for_every_graphical_client(self) -> None:
        result = dots_session.run_session(
            self.command("server"),
            [self.command("client", "0", "0"), self.command("client", "7", "0.1")],
        )
        self.assertEqual(result, 7)

    def test_readiness_timeout_fails(self) -> None:
        result = dots_session.run_session(
            self.command("silent-server"),
            [],
            readiness_timeout=0.1,
        )
        self.assertEqual(result, 1)

    def test_bounded_headless_session_stops_healthy_processes(self) -> None:
        marker = Path(self.directory.name) / "bot-stopped.txt"
        result = dots_session.run_session(
            self.command("server"),
            [],
            [self.command("bot", str(marker))],
            duration_seconds=0.1,
        )
        self.assertEqual(result, 0)
        self.assertEqual(marker.read_text(encoding="utf-8"), "stopped")

    def test_client_config_is_resolved_and_passed_to_every_client(self) -> None:
        build_directory = Path(self.directory.name) / "build"
        binary_directory = build_directory / "bin"
        binary_directory.mkdir(parents=True)
        executable_suffix = ".exe" if os.name == "nt" else ""
        (binary_directory / f"dots_server{executable_suffix}").touch()
        (binary_directory / f"dots_client{executable_suffix}").touch()
        (binary_directory / f"dots_bot{executable_suffix}").touch()
        client_config = Path(self.directory.name) / "client.toml"
        client_config.write_text('[network]\nmode = "offline"\n', encoding="utf-8")

        arguments = dots_session._parse_arguments(
            [
                "--build-dir",
                str(build_directory),
                "--clients",
                "3",
                "--bots",
                "2",
                "--client-config",
                str(client_config),
            ]
        )
        server_command, client_command, bot_command = dots_session._session_commands(arguments)

        self.assertEqual(arguments.client_config, client_config.resolve())
        self.assertEqual(server_command[1:3], ["--listen", "127.0.0.1:27020"])
        self.assertEqual(
            client_command[1:5],
            ["--config", str(client_config.resolve()), "--connect", "127.0.0.1:27020"],
        )
        self.assertEqual(arguments.clients, 3)
        self.assertEqual(arguments.bots, 2)
        self.assertEqual(bot_command[1:3], ["--connect", "127.0.0.1:27020"])

        with mock.patch.object(dots_session, "run_session", return_value=0) as run_session:
            result = dots_session.main(
                [
                    "--build-dir",
                    str(build_directory),
                    "--clients",
                    "3",
                    "--bots",
                    "2",
                    "--client-config",
                    str(client_config),
                ]
            )
        self.assertEqual(result, 0)
        launched_server, launched_clients, launched_bots = run_session.call_args.args
        self.assertEqual(launched_server, server_command)
        self.assertEqual(launched_clients, [client_command] * 3)
        self.assertEqual(launched_bots, [bot_command] * 2)
        self.assertIsNone(run_session.call_args.kwargs["duration_seconds"])

    def test_missing_client_config_is_rejected(self) -> None:
        with self.assertRaises(SystemExit):
            dots_session._parse_arguments(
                [
                    "--build-dir",
                    self.directory.name,
                    "--client-config",
                    str(Path(self.directory.name) / "missing.toml"),
                ]
            )

    def test_negative_bot_count_is_rejected(self) -> None:
        with self.assertRaises(SystemExit):
            dots_session._parse_arguments(
                ["--build-dir", self.directory.name, "--bots", "-1"]
            )

    def test_headless_bounded_arguments_are_supported(self) -> None:
        build_directory = Path(self.directory.name) / "build"
        binary_directory = build_directory / "bin"
        binary_directory.mkdir(parents=True)
        executable_suffix = ".exe" if os.name == "nt" else ""
        (binary_directory / f"dots_server{executable_suffix}").touch()
        (binary_directory / f"dots_bot{executable_suffix}").touch()

        arguments = dots_session._parse_arguments(
            [
                "--build-dir",
                str(build_directory),
                "--clients",
                "0",
                "--bots",
                "5",
                "--duration-seconds",
                "30",
            ]
        )
        _, client_command, bot_command = dots_session._session_commands(arguments)
        self.assertEqual(client_command, [])
        self.assertEqual(arguments.duration_seconds, 30.0)
        self.assertEqual(Path(bot_command[0]).name, f"dots_bot{executable_suffix}")

    def test_empty_or_nonpositive_bounded_session_is_rejected(self) -> None:
        with self.assertRaises(SystemExit):
            dots_session._parse_arguments(
                ["--build-dir", self.directory.name, "--clients", "0"]
            )
        with self.assertRaises(SystemExit):
            dots_session._parse_arguments(
                [
                    "--build-dir",
                    self.directory.name,
                    "--duration-seconds",
                    "0",
                ]
            )


if __name__ == "__main__":
    unittest.main()
