#!/usr/bin/env python3
"""End-to-end SSH smoke test for sshforum.

Usage: python3 tests/ssh_smoke.py /path/to/sshforum
Requires Paramiko (available in the project's Fedora toolbox).
"""

from __future__ import annotations

import base64
import contextlib
import os
from pathlib import Path
import re
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

try:
    import paramiko
except ImportError as exc:
    raise SystemExit("ssh_smoke.py requires Python package paramiko") from exc


HOST = "127.0.0.1"
TIMEOUT = 8.0


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind((HOST, 0))
        return sock.getsockname()[1]


def wait_for_server(process: subprocess.Popen[bytes], port: int) -> None:
    deadline = time.monotonic() + TIMEOUT
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"server exited during startup (code {process.returncode})")
        try:
            with socket.create_connection((HOST, port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise AssertionError("server did not listen within eight seconds")


def check_listener_nonblocking(process: subprocess.Popen[bytes], port: int) -> None:
    if not sys.platform.startswith("linux"):
        return
    # Identify the actual listening socket, rather than an accepted connection.
    proc = Path(f"/proc/{process.pid}")
    inodes = {
        fields[9]
        for line in (proc / "net/tcp").read_text().splitlines()[1:]
        if (fields := line.split())[3] == "0A"
        and int(fields[1].split(":")[1], 16) == port
    }
    assert inodes, "test listener is missing from /proc/net/tcp"
    for descriptor in (proc / "fd").iterdir():
        try:
            target = os.readlink(descriptor)
            if target not in {f"socket:[{inode}]" for inode in inodes}:
                continue
            info = (proc / "fdinfo" / descriptor.name).read_text()
        except FileNotFoundError:
            continue  # An unrelated connection may have closed while scanning.
        flags = next(line.split()[1] for line in info.splitlines() if line.startswith("flags:"))
        assert int(flags, 8) & os.O_NONBLOCK, "listening socket can block in accept()"
        return
    raise AssertionError("could not find the server's listening file descriptor")


class Server:
    def __init__(self, binary: Path, directory: Path):
        self.binary = binary
        self.directory = directory
        self.process: subprocess.Popen[bytes] | None = None
        self.log = None
        self.port = 0

    def start(self) -> None:
        self.port = free_port()
        self.log = (self.directory / "server.log").open("ab")
        self.process = subprocess.Popen(
            [
                str(self.binary),
                "--bind", HOST,
                "--port", str(self.port),
                "--db", str(self.directory / "forum.db"),
                "--host-key", str(self.directory / "host_key"),
            ],
            stdout=self.log,
            stderr=subprocess.STDOUT,
        )
        wait_for_server(self.process, self.port)

    def stop(self) -> None:
        if self.process is not None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
            self.process = None
        if self.log is not None:
            self.log.close()
            self.log = None


def authenticated_transport(
    port: int,
    method: str,
    key=None,
    username: str | None = None,
    password: str = "any-password",
    source_ip: str | None = None,
) -> paramiko.Transport:
    connection = (HOST, port)
    if source_ip is not None:
        connection = socket.create_connection(
            connection, timeout=TIMEOUT, source_address=(source_ip, 0)
        )
    transport = paramiko.Transport(connection)
    transport.banner_timeout = TIMEOUT
    transport.auth_timeout = TIMEOUT
    try:
        transport.start_client(timeout=TIMEOUT)
        username = username if username is not None else f"anonymous_{method}"
        if method == "none":
            transport.auth_none(username)
        elif method == "password":
            transport.auth_password(username, password)
        elif method == "publickey":
            transport.auth_publickey(username, key)
        elif method == "interactive":
            transport.auth_interactive(
                username,
                lambda _title, _instructions, _prompts: [],
            )
        else:
            raise ValueError(method)
        assert transport.is_authenticated(), f"{method} authentication was not accepted"
        return transport
    except BaseException:
        transport.close()
        raise


class Terminal:
    def __init__(
        self,
        port: int,
        method: str,
        key=None,
        width: int = 100,
        height: int = 24,
        username: str | None = None,
        password: str = "any-password",
        source_ip: str | None = None,
    ):
        self.transport = authenticated_transport(
            port, method, key, username, password, source_ip
        )
        self.channel = None
        self.output = bytearray()
        try:
            self.channel = self.transport.open_session(timeout=TIMEOUT)
            self.channel.settimeout(TIMEOUT)
            self.channel.get_pty(term="xterm-256color", width=width, height=height)
            self.channel.invoke_shell()
            self.wait_for_output()
        except BaseException:
            self.close()
            raise

    def close(self) -> None:
        if self.channel is not None:
            self.channel.close()
        self.transport.close()

    def mark(self) -> int:
        self.drain()
        return len(self.output)

    def drain(self) -> None:
        while self.channel.recv_ready():
            self.output.extend(self.channel.recv(65536))

    def wait_for_output(self, since: int = 0, timeout: float = TIMEOUT) -> bytes:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.drain()
            if len(self.output) > since:
                return bytes(self.output[since:])
            if self.channel.closed or self.channel.exit_status_ready():
                break
            time.sleep(0.02)
        raise AssertionError("terminal produced no output")

    def wait_for(self, text: str, since: int, timeout: float = TIMEOUT) -> bytes:
        needle = text.encode("utf-8")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.drain()
            recent = bytes(self.output[since:])
            if needle in recent:
                return recent
            if self.channel.closed or self.channel.exit_status_ready():
                break
            time.sleep(0.02)
        tail = bytes(self.output[since:])[-500:].decode("utf-8", "replace")
        raise AssertionError(f"terminal did not show {text!r}; recent output: {tail!r}")

    def send(self, data: bytes | str) -> None:
        if isinstance(data, str):
            data = data.encode("utf-8")
        self.channel.sendall(data)

    def send_and_expect(self, data: bytes | str, text: str) -> bytes:
        mark = self.mark()
        self.send(data)
        return self.wait_for(text, mark)


def submit_post(terminal: Terminal, title: str, body: str, split_utf8: bool = False) -> None:
    terminal.send("n")
    terminal.send(title + "\r")
    if split_utf8:
        for value in body.encode("utf-8"):
            terminal.send(bytes([value]))
            time.sleep(0.004)
    else:
        terminal.send(body)
    terminal.send_and_expect(b"\x04", title)


def submit_reply(terminal: Terminal, body: str) -> None:
    terminal.send("a")
    terminal.send(body)
    terminal.send_and_expect(b"\x04", body)


def assert_rejected(label: str, operation) -> None:
    try:
        value = operation()
    except (paramiko.SSHException, EOFError, OSError):
        return
    else:
        with contextlib.suppress(Exception):
            value.close()
        raise AssertionError(f"server accepted {label}")


def check_restricted_requests(port: int) -> None:
    def probe(label: str, operation) -> None:
        transport = authenticated_transport(port, "none")
        try:
            assert_rejected(label, lambda: operation(transport))
        finally:
            transport.close()

    def exec_request(transport):
        channel = transport.open_session(timeout=TIMEOUT)
        try:
            channel.exec_command("id")
        except BaseException:
            channel.close()
            raise
        return channel

    probe("exec", exec_request)
    probe("SFTP", lambda t: paramiko.SFTPClient.from_transport(t))
    probe(
        "direct TCP forwarding",
        lambda t: t.open_channel(
            "direct-tcpip", (HOST, 1), (HOST, 12345), timeout=TIMEOUT
        ),
    )
    probe("remote TCP forwarding", lambda t: t.request_port_forward(HOST, 0))


def check_exit_restores_screen(terminal: Terminal) -> None:
    terminal.send("b")
    mark = terminal.mark()
    terminal.send("q")
    deadline = time.monotonic() + TIMEOUT
    while time.monotonic() < deadline:
        terminal.drain()
        if terminal.channel.closed or terminal.channel.exit_status_ready():
            break
        time.sleep(0.02)
    output = bytes(terminal.output[mark:])
    assert b"\x1b[?1049l" in output, "shell exit did not restore the alternate screen"


def check_shutdown_restores_screen(terminal: Terminal, since: int) -> None:
    terminal.channel.settimeout(0.5)
    deadline = time.monotonic() + TIMEOUT
    while time.monotonic() < deadline:
        try:
            chunk = terminal.channel.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            break
        terminal.output.extend(chunk)
    else:
        raise AssertionError("active SSH terminal did not reach EOF after SIGTERM")
    assert b"\x1b[?1049l" in terminal.output[since:], (
        "SIGTERM did not restore the active terminal's alternate screen"
    )


def check_navigation(port: int, marker: str) -> None:
    titles = [f"SMOKE_PAGE_{marker}_{index:02d}" for index in range(10)]
    with contextlib.closing(Terminal(port, "none")) as writer:
        for title in titles:
            submit_post(writer, title, "paging body")
            writer.send_and_expect("b", "SSH Forum | Threads")

    with contextlib.closing(Terminal(port, "none", height=8)) as pager:
        initial = bytes(pager.output)
        hidden = [title for title in titles if title.encode() not in initial]
        assert hidden, "small terminal displayed every pagination test thread"
        mark = pager.mark()
        for part in (b"\x1b", b"[", b"B"):
            pager.send(part)
            time.sleep(0.02)
        pager.wait_for("SSH Forum | Threads", mark)
        mark = pager.mark()
        pager.send("j" * len(titles))
        deadline = time.monotonic() + TIMEOUT
        while time.monotonic() < deadline:
            pager.drain()
            if any(title.encode() in pager.output[mark:] for title in hidden):
                break
            time.sleep(0.02)
        else:
            raise AssertionError("moving down did not reveal a later page of threads")
        pager.send_and_expect(b"\x1b[A", "SSH Forum | Threads")
        pager.send_and_expect("k", "SSH Forum | Threads")


def check_editor(port: int, marker: str, database: Path) -> None:
    title = f"SMOKE_EDIT_{marker}_甲乙"
    with contextlib.closing(Terminal(port, "none")) as editor:
        screen = editor.send_and_expect("n", "New thread")
        assert b"\x1b[?25h" in screen, "editor did not show the terminal cursor"
        editor.send(f"SMOKE_EDIT_{marker}_甲丙")
        # Split a left-arrow sequence across SSH writes, then insert/delete
        # whole UTF-8 characters in the middle of the title.
        for part in (b"\x1b", b"[", b"D"):
            editor.send(part)
            time.sleep(0.01)
        editor.send("乙\x1b[3~\x1b[HX\x7f\x1b[F\r")
        editor.send("AB\r甲乙")
        editor.send("\x1b[H\x1b[3~中\x1b[A!\x1b[B\x1b[F\x1b[D\x7f甲")
        # Insert a newline between A and B, preserving the following lines.
        editor.send("\x1b[H\x1b[A\x1b[C\r")
        editor.send_and_expect(b"\x04", title)
        with sqlite3.connect(database) as connection:
            row = connection.execute(
                "SELECT id, body FROM threads WHERE title = ?", (title,)
            ).fetchone()
        assert row is not None, "edited title was not saved correctly"
        thread_id, body = row
        assert body == "A\nB!\n甲乙", f"cursor editing saved unexpected body: {body!r}"

        editor.send("a回丙\x1b[D帖\x1b[3~")
        editor.send_and_expect(b"\x04", "回帖")
        with sqlite3.connect(database) as connection:
            replies = connection.execute(
                "SELECT body FROM posts WHERE thread_id = ? ORDER BY id", (thread_id,)
            ).fetchall()
        assert replies == [("回帖",)], f"reply editing saved unexpected text: {replies!r}"


def check_identity(server: Server, marker: str, database: Path, key) -> None:
    username = f"smoke_identity_user_{marker}"
    other_username = f"smoke_identity_other_{marker}"
    password = f"smoke_identity_password_{marker}_one"
    other_password = f"smoke_identity_password_{marker}_two"
    key2 = paramiko.RSAKey.generate(2048)
    number = 0

    def title() -> str:
        nonlocal number
        number += 1
        return f"SMOKE_ID_{marker}_{number:02d}"

    def label(author_id: str) -> str:
        assert re.fullmatch(r"v1:[0-9a-f]{64}", author_id), (
            f"invalid persisted author ID: {author_id!r}"
        )
        digest = bytes.fromhex(author_id[3:])
        return "Anonymous#" + base64.b32encode(digest[:5]).decode("ascii")

    def thread_author(terminal: Terminal, thread_title: str) -> tuple[int, str]:
        mark = terminal.mark()
        submit_post(terminal, thread_title, f"identity body {number}")
        terminal.wait_for("SSH Forum | Thread #", mark)
        with sqlite3.connect(database) as connection:
            row = connection.execute(
                "SELECT id, author_id FROM threads WHERE title = ?", (thread_title,)
            ).fetchone()
        assert row is not None, f"identity thread {thread_title!r} was not persisted"
        thread_id, author_id = row
        short = label(author_id)
        terminal.wait_for(short, mark)
        terminal.wait_for(f"You: {short}", 0)
        return thread_id, author_id

    def new_thread(method: str, thread_title: str, **credentials) -> str:
        with contextlib.closing(Terminal(server.port, method, **credentials)) as terminal:
            return thread_author(terminal, thread_title)[1]

    def identity_key() -> bytes:
        with sqlite3.connect(database) as connection:
            row = connection.execute(
                "SELECT value FROM forum_metadata WHERE key = 'identity_hmac_key_v1'"
            ).fetchone()
        assert row is not None, "identity HMAC key is missing"
        secret = row[0]
        assert isinstance(secret, bytes) and len(secret) == 32, (
            "identity HMAC key must be a 32-byte SQLite BLOB"
        )
        return secret

    first_title = title()
    second_title = title()
    with contextlib.closing(Terminal(server.port, "none", username=username)) as first:
        with contextlib.closing(Terminal(server.port, "none", username=username)) as second:
            first_peer = first.transport.sock.getsockname()
            second_peer = second.transport.sock.getsockname()
            assert first_peer[0] == second_peer[0] == HOST
            assert first_peer[1] != second_peer[1], "test connections reused a source port"
            _, none_author = thread_author(first, first_title)
            second_id, none_reconnect = thread_author(second, second_title)
    assert none_reconnect == none_author, "none identity changed with the source port"

    with contextlib.closing(Terminal(
        server.port, "password", username=username, password=password
    )) as password_session:
        password_session.send_and_expect("\r", second_title)
        reply_body = f"SMOKE_ID_REPLY_{marker}"
        mark = password_session.mark()
        submit_reply(password_session, reply_body)
        password_session.wait_for("Reply #", mark)
        with sqlite3.connect(database) as connection:
            replies = connection.execute(
                "SELECT author_id FROM posts WHERE thread_id = ? AND body = ?",
                (second_id, reply_body),
            ).fetchall()
        assert len(replies) == 1, "identity reply was not persisted exactly once"
        reply_author = replies[0][0]
        password_session.wait_for(label(reply_author), mark)
        assert reply_author != none_author, "password and none identities share a domain"
        password_session.send_and_expect("b", "SSH Forum | Threads")
        password_author = thread_author(password_session, title())[1]
    assert reply_author == password_author, "reply and thread used different session identities"

    assert new_thread("none", title(), username=other_username) != none_author, (
        "different SSH usernames share a none identity"
    )
    assert new_thread("password", title(), username=username, password=password) == (
        password_author
    ), "same password identity changed after reconnect"
    assert new_thread("password", title(), username=username, password=other_password) != (
        password_author
    ), "different passwords share an identity"

    publickey_author = new_thread("publickey", title(), username=username, key=key)
    assert publickey_author != none_author, "publickey and none identities share a domain"
    assert publickey_author != password_author, "publickey and password identities share a domain"
    assert new_thread("publickey", title(), username=username, key=key) == (
        publickey_author
    ), "same public key identity changed after reconnect"
    assert new_thread("publickey", title(), username=username, key=key2) != (
        publickey_author
    ), "different public keys share an identity"

    interactive_author = new_thread("interactive", title(), username=username)
    assert interactive_author != none_author, "interactive and none identities share a domain"
    assert new_thread("interactive", title(), username=username) == (
        interactive_author
    ), "empty interactive identity changed after reconnect"

    if sys.platform.startswith("linux"):
        try:
            with socket.socket() as probe:
                probe.bind(("127.0.0.2", 0))
        except OSError:
            pass
        else:
            assert new_thread(
                "none", title(), username=username, source_ip="127.0.0.2"
            ) != none_author, "different source IPs share an identity"

    secret_before_restart = identity_key()
    server.stop()
    server.start()
    assert identity_key() == secret_before_restart, "identity HMAC key changed on restart"
    assert new_thread("none", title(), username=username) == none_author, (
        "none identity changed after restart"
    )

    with sqlite3.connect(database) as connection:
        for table in ("threads", "posts"):
            for row in connection.execute(f"SELECT * FROM {table}"):
                values = " ".join(str(value) for value in row if value is not None)
                for credential in (username, other_username, password, other_password,
                                   HOST, "127.0.0.2"):
                    assert credential not in values, (
                        f"raw SSH credential {credential!r} leaked into {table}"
                    )


def check_input_slices(port: int) -> None:
    with contextlib.closing(Terminal(port, "none")) as busy:
        with contextlib.closing(Terminal(port, "none")) as observer:
            mark = busy.mark()
            # One read contains more refresh commands than a processing slice.
            # Intermediate list output proves execution yielded before reaching
            # the editor command at the end of the batch.
            busy.send("r" * 4095 + "n")
            busy.wait_for("SSH Forum | Threads", mark)
            observer.send_and_expect("n", "New thread | Title")
            # No further input is sent: locally buffered work must keep running.
            busy.wait_for("New thread | Title", mark)
            busy.send_and_expect("切片标题\r正文\x1b[D!", "正!文")


def run(binary: Path) -> None:
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise SystemExit(f"server binary is not executable: {binary}")
    marker = str(int(time.time() * 1000))
    title = f"SMOKE_THREAD_{marker}"
    body = f"SMOKE_BODY_{marker}_雪_🌙_END"
    reply_a = f"SMOKE_REPLY_A_{marker}"
    reply_b = f"SMOKE_REPLY_B_{marker}"

    with tempfile.TemporaryDirectory(prefix="sshforum-smoke-") as temp:
        directory = Path(temp)
        server = Server(binary, directory)
        try:
            server.start()
            check_listener_nonblocking(server.process, server.port)
            assert (directory / "host_key").is_file(), "server did not create a host key"
            key = paramiko.RSAKey.generate(2048)
            terminals = []
            try:
                writer = Terminal(server.port, "none")
                terminals.append(writer)
                reader = Terminal(server.port, "password")
                terminals.append(reader)
                submit_post(writer, title, body, split_utf8=True)
                reader.send_and_expect("r", title)
                reader.send_and_expect("\r", body)
                submit_reply(writer, reply_a)
                reader.send_and_expect("r", reply_a)
                submit_reply(reader, reply_b)
                writer.send_and_expect("r", reply_b)

                pubkey = Terminal(server.port, "publickey", key)
                terminals.append(pubkey)
                pubkey.send_and_expect("r", title)
                interactive = Terminal(server.port, "interactive")
                terminals.append(interactive)
                interactive.send_and_expect("r", title)
                interactive.channel.resize_pty(width=120, height=32)
                interactive.send_and_expect("r", title)

                check_restricted_requests(server.port)
                check_exit_restores_screen(writer)
            finally:
                for terminal in terminals:
                    terminal.close()

            host_key_before_restart = (directory / "host_key").read_bytes()
            with contextlib.closing(Terminal(server.port, "none")) as active:
                mark = active.mark()
                server.stop()
                check_shutdown_restores_screen(active, mark)
            assert (directory / "forum.db").is_file(), "server did not create its database"
            server.start()
            assert (directory / "host_key").read_bytes() == host_key_before_restart, (
                "host key changed on server restart"
            )
            with contextlib.closing(Terminal(server.port, "none")) as persisted:
                persisted.send_and_expect("r", title)
                persisted.send_and_expect("\r", reply_a)
                persisted.send_and_expect("r", reply_b)
            check_navigation(server.port, marker)
            check_editor(server.port, marker, directory / "forum.db")
            check_input_slices(server.port)
            check_identity(server, marker, directory / "forum.db", key)
            print("SSH smoke test passed")
        except BaseException:
            server.stop()
            log_path = directory / "server.log"
            if log_path.exists():
                log_tail = log_path.read_text(errors="replace")[-4000:]
                print(f"Server log tail:\n{log_tail}", file=sys.stderr)
            raise
        finally:
            server.stop()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: ssh_smoke.py /path/to/sshforum")
    run(Path(sys.argv[1]).resolve())
