"""Shared plumbing for the CLI: errors, JSON output, connecting, address and symbol handling, state files."""
import argparse
import json
import os
import re
import sys
from pathlib import Path

from ..client import (DEFAULT_CONSOLE, Client, ConnectionClosed, ProtocolError, ServerError)
from ..symbols import SymbolTable


class CliError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


def emit(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


class JsonParser(argparse.ArgumentParser):
    """argparse that reports usage errors as CliError (so main prints them as JSON)."""

    def error(self, message):
        raise CliError("usage", f"{message} ({self.prog})")


def home_dir():
    """Where the CLI keeps small state files: MELONDS_CLI_HOME, else ~/.melonds-cli."""
    return Path(os.environ.get("MELONDS_CLI_HOME") or Path.home() / ".melonds-cli")


# ----------------------------------------------------------------------------- state file

def _state_path():
    return home_dir() / "state.json"


def load_state():
    try:
        return json.loads(_state_path().read_text())
    except (OSError, ValueError):
        return {}


def save_state(state):
    path = _state_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(state, indent=2))


# ----------------------------------------------------------------------------- arguments

def add_connection_args(p):
    g = p.add_argument_group("connection")
    g.add_argument("--port", type=int, help="server port (else --port-file, else $MELONDS_PORT)")
    g.add_argument("--port-file", help="file holding the server port (written by `launch --port-file`)")
    g.add_argument("--console", type=int, help="console id (default: the server's default console)")
    g.add_argument("--symbols", help="symbol file for names in address arguments (default: remembered one)")
    g.add_argument("--timeout", type=float, default=120.0, help="seconds to wait for a reply (default 120)")


def int_arg(text):
    try:
        return int(str(text), 0)
    except ValueError:
        raise argparse.ArgumentTypeError(f"not a number: {text!r}")


def resolve_port(args):
    if args.port is not None:
        return args.port
    if args.port_file:
        try:
            return int(Path(args.port_file).read_text().strip())
        except (OSError, ValueError) as e:
            raise CliError("no_server", f"cannot read port file {args.port_file}: {e}")
    env = os.environ.get("MELONDS_PORT")
    if env:
        try:
            return int(env)
        except ValueError:
            raise CliError("no_server", f"MELONDS_PORT is not a number: {env!r}")
    raise CliError("no_server", "no server given: use --port, --port-file or $MELONDS_PORT")


class ConsoleView:
    """What SymbolTable and Watches need from a console: id, frames, read(addr, length, cpu)."""

    def __init__(self, session):
        self.s = session

    @property
    def id(self):
        return self.s.console

    @property
    def frames(self):
        for c in self.s.cl.console_list():
            if c.id == self.s.console:
                return c.frames
        return 0

    def read(self, addr, length, cpu=9):
        return self.s.cl.read(addr, length, cpu, self.s.console)


class Session:
    """One connection: hello done, console resolved, symbols loaded on demand."""

    def __init__(self, args, empty_queue="repeat"):
        self.args = args
        port = resolve_port(args)
        try:
            self.cl = Client(port, timeout=args.timeout)
        except OSError as e:
            raise CliError("connect_failed", f"cannot connect to 127.0.0.1:{port}: {e}")
        self.port = port
        try:
            self.info = self.cl.hello(empty_queue=empty_queue)
        except Exception:
            self.cl.close()
            raise
        if args.console is not None:
            self.console = args.console
        else:
            self.console = self.info.default_console
        self._symbols = False
        self.view = ConsoleView(self)

    def close(self):
        self.cl.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # ------------------------------------------------------------------ symbols

    def _state_key(self):
        return f"{self.info.name}/{self.console}"

    def symbol_path(self):
        if getattr(self.args, "symbols", None):
            return self.args.symbols
        return load_state().get("symbols", {}).get(self._state_key())

    def remember_symbols(self, path):
        state = load_state()
        table = state.setdefault("symbols", {})
        if path:
            table[self._state_key()] = str(Path(path).resolve())
        else:
            table.pop(self._state_key(), None)
        save_state(state)

    @property
    def symbols(self):
        """The SymbolTable for this console, or None when no symbol file is known."""
        if self._symbols is False:
            path = self.symbol_path()
            self._symbols = None
            if path:
                table = SymbolTable()
                try:
                    table.load(path)
                except (OSError, ValueError, KeyError, ImportError) as e:
                    raise CliError("bad_symbols", f"cannot load symbols from {path}: {e}")
                self._symbols = table
        return self._symbols

    ADDR_RE = re.compile(r"^([A-Za-z_.$][\w.$:<>@]*)(?:([+-])(0[xX][0-9a-fA-F]+|\d+))?$")

    def addr(self, text):
        """A number (0x.. or decimal) or SYMBOL[+/-offset]."""
        if isinstance(text, int):
            return text
        try:
            return int(text, 0)
        except ValueError:
            pass
        m = self.ADDR_RE.match(text)
        if not m:
            raise CliError("bad_address", f"not an address: {text!r}")
        table = self.symbols
        if table is None:
            raise CliError("no_symbols", f"{text!r} is not a number and no symbol file is known "
                           "(use --symbols FILE or `symbols load FILE`)")
        sym = table.resolve(m.group(1), self.view)
        if sym is None:
            raise CliError("unknown_symbol", f"no symbol named {m.group(1)!r}")
        off = int(m.group(3), 0) if m.group(3) else 0
        return sym.addr + (-off if m.group(2) == "-" else off)

    def describe(self, addr):
        table = self.symbols
        return table.describe(addr, self.view) if table else None

    # ------------------------------------------------------------------ console state

    def console_states(self, ids=None):
        out = []
        for c in self.cl.console_list():
            if ids is None or c.id in ids:
                out.append({"id": c.id, "state": c.state, "frames": c.frames})
        return out


def break_dict(e):
    return {"console": e.console, "kind": e.what, "cpu": 7 if e.cpu in (1, 7) else 9, "id": e.id, "pc": e.pc,
            "addr": e.addr, "value": e.value, "size": e.size}


def run_command(func, args):
    """Run a command function, turning every failure into the JSON error shape. Returns the exit code."""
    try:
        result = func(args)
        if result is not None:
            emit(result)
        return 0
    except CliError as e:
        emit({"error": e.code, "message": e.message})
    except ServerError as e:
        emit({"error": e.name, "message": e.text})
    except ConnectionClosed as e:
        emit({"error": "connection_closed", "message": str(e)})
    except ProtocolError as e:
        emit({"error": "protocol_error", "message": str(e)})
    except TimeoutError as e:
        emit({"error": "timeout", "message": str(e)})
    except ValueError as e:
        emit({"error": "bad_argument", "message": str(e)})
    except OSError as e:
        emit({"error": "os_error", "message": str(e)})
    return 1


__all__ = ["CliError", "Session", "emit", "JsonParser", "int_arg", "add_connection_args", "DEFAULT_CONSOLE",
           "break_dict", "run_command", "home_dir", "load_state", "save_state"]
