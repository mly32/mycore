#!/usr/bin/env python3
"""Launch a local authoritative Dots server with graphical clients and headless bots."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time
from typing import Sequence


READY_PREFIX = "DOTS_SERVER_READY "


def _forward_output(
    label: str,
    process: subprocess.Popen[str],
    messages: queue.Queue[tuple[str, str]],
) -> None:
    assert process.stdout is not None
    with process.stdout:
        for line in process.stdout:
            messages.put((label, line.rstrip("\r\n")))


def _stop_processes(processes: Sequence[subprocess.Popen[str]]) -> None:
    print("dots_session: stopping all processes", file=sys.stderr)
    for process in processes:
        if process.poll() is None:
            process.terminate()
    deadline = time.monotonic() + 3.0
    for process in processes:
        if process.poll() is None:
            try:
                process.wait(timeout=max(0.0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                process.kill()
    for process in processes:
        if process.poll() is None:
            process.wait()


def run_session(
    server_command: Sequence[str],
    client_commands: Sequence[Sequence[str]],
    bot_commands: Sequence[Sequence[str]] = (),
    readiness_timeout: float = 10.0,
) -> int:
    messages: queue.Queue[tuple[str, str]] = queue.Queue()
    processes: list[subprocess.Popen[str]] = []
    clients: list[subprocess.Popen[str]] = []
    bots: list[subprocess.Popen[str]] = []
    output_threads: list[threading.Thread] = []

    def start(label: str, command: Sequence[str]) -> subprocess.Popen[str]:
        process = subprocess.Popen(
            list(command),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        processes.append(process)
        output_thread = threading.Thread(
            target=_forward_output,
            args=(label, process, messages),
            daemon=True,
        )
        output_threads.append(output_thread)
        output_thread.start()
        return process

    try:
        server = start("server", server_command)
        ready = False
        deadline = time.monotonic() + readiness_timeout
        while not ready and time.monotonic() < deadline:
            if server.poll() is not None:
                print(
                    f"dots_session: server exited during startup with code {server.returncode}",
                    file=sys.stderr,
                )
                return server.returncode or 1
            try:
                label, line = messages.get(timeout=0.05)
            except queue.Empty:
                continue
            print(f"[{label}] {line}", flush=True)
            ready = label == "server" and line.startswith(READY_PREFIX)
        if not ready:
            print("dots_session: server readiness timed out", file=sys.stderr)
            return 1

        for index, command in enumerate(client_commands, start=1):
            clients.append(start(f"client {index}", command))
        for index, command in enumerate(bot_commands, start=1):
            bots.append(start(f"bot {index}", command))

        while True:
            try:
                label, line = messages.get(timeout=0.05)
                print(f"[{label}] {line}", flush=True)
            except queue.Empty:
                pass

            if server.poll() is not None:
                print(
                    f"dots_session: server exited with code {server.returncode}",
                    file=sys.stderr,
                )
                return server.returncode or 1
            for client in clients:
                if client.poll() not in (None, 0):
                    print(
                        f"dots_session: client exited with code {client.returncode}",
                        file=sys.stderr,
                    )
                    return client.returncode or 1
            if clients and all(client.poll() == 0 for client in clients):
                print("dots_session: all graphical clients exited; stopping the session", file=sys.stderr)
                return 0
            for bot in bots:
                if bot.poll() not in (None, 0):
                    print(
                        f"dots_session: bot exited with code {bot.returncode}",
                        file=sys.stderr,
                    )
                    return bot.returncode or 1
    except KeyboardInterrupt:
        print("dots_session: interrupted by user", file=sys.stderr)
        return 130
    finally:
        _stop_processes(processes)
        for output_thread in output_threads:
            output_thread.join(timeout=1.0)


def _executable(build_directory: Path, name: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    path = build_directory / "bin" / f"{name}{suffix}"
    if not path.is_file():
        raise FileNotFoundError(f"executable does not exist: {path}")
    return path


def _parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(prog=Path(__file__).name, description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--clients", type=int, default=2)
    parser.add_argument("--bots", type=int, default=0)
    parser.add_argument(
        "--client-config",
        type=Path,
        help="TOML configuration file passed to every client",
    )
    parser.add_argument("--server-address", default="127.0.0.1:27020")
    parser.add_argument("--fake-lag-ms", type=int, default=0)
    parser.add_argument("--fake-loss-percent", type=float, default=0.0)
    parsed = parser.parse_args(arguments)
    if parsed.clients <= 0:
        parser.error("--clients must be positive")
    if parsed.bots < 0:
        parser.error("--bots must be non-negative")
    if parsed.fake_lag_ms < 0:
        parser.error("--fake-lag-ms must be non-negative")
    if not 0.0 <= parsed.fake_loss_percent <= 100.0:
        parser.error("--fake-loss-percent must be in the range 0..100")
    if parsed.client_config is not None:
        parsed.client_config = parsed.client_config.resolve()
        if not parsed.client_config.is_file():
            parser.error(f"--client-config does not exist: {parsed.client_config}")
    return parsed


def _session_commands(arguments: argparse.Namespace) -> tuple[list[str], list[str], list[str]]:
    build_directory = arguments.build_dir.resolve()
    server = _executable(build_directory, "dots_server")
    client = _executable(build_directory, "dots_client")
    bot = _executable(build_directory, "dots_bot")
    impairment: list[str] = [
        "--fake-lag-ms",
        str(arguments.fake_lag_ms),
        "--fake-loss-percent",
        str(arguments.fake_loss_percent),
    ]
    server_command: list[str] = [
        str(server),
        "--listen",
        arguments.server_address,
        *impairment,
    ]
    client_configuration: list[str] = (
        []
        if arguments.client_config is None
        else ["--config", str(arguments.client_config)]
    )
    client_command: list[str] = [
        str(client),
        *client_configuration,
        "--connect",
        arguments.server_address,
        *impairment,
    ]
    bot_command: list[str] = [
        str(bot),
        "--connect",
        arguments.server_address,
        *impairment,
    ]
    return server_command, client_command, bot_command


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parse_arguments(argv)
    server_command, client_command, bot_command = _session_commands(arguments)
    return run_session(
        server_command,
        [client_command for _ in range(arguments.clients)],
        [bot_command for _ in range(arguments.bots)],
    )


if __name__ == "__main__":
    raise SystemExit(main())
