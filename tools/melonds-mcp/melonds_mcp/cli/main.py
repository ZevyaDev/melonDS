"""melonds command line: `python -m melonds_mcp.cli <command> ...` or the `melonds` script."""
import sys

from . import analysis_cmds, memory_cmds, session_cmds
from .common import CliError, JsonParser, add_connection_args, emit, run_command


def build_parser():
    parser = JsonParser(prog="melonds", description="Drive a running melonds_server. Every command prints "
                        "JSON on stdout; errors print {\"error\", \"message\"} and exit with code 1.")
    sub = parser.add_subparsers(dest="cmd", required=True, parser_class=JsonParser)

    def leaf(group, name, help_text, func):
        p = group.add_parser(name, help=help_text, allow_abbrev=False)
        add_connection_args(p)
        p.set_defaults(func=func)
        return p

    session_cmds.register(sub, leaf)
    memory_cmds.register(sub, leaf)
    analysis_cmds.register(sub, leaf)
    return parser


def main(argv=None):
    parser = build_parser()
    try:
        args, extra = parser.parse_known_args(argv)
        if getattr(args, "passthrough", False):
            args.server_args = extra
        elif extra:
            raise CliError("usage", f"unrecognized arguments: {' '.join(extra)}")
    except CliError as e:
        emit({"error": e.code, "message": e.message})
        return 1
    return run_command(args.func, args)


if __name__ == "__main__":
    sys.exit(main())
