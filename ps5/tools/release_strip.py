#!/usr/bin/env python3
"""Remove indirect-call tracing from the lifter header for the PS5 build."""

import sys
from pathlib import Path


def main():
    header = Path(sys.argv[1])
    original = header.read_bytes()
    # Older PS5 builds rewrote this declaration in-place. The guest's
    # exception-frame pointer belongs to the same thread as the guest stack.
    contents = original.replace(
        b"extern uint32_t g_seh_ebp;",
        b"extern RECOMP_TLS uint32_t g_seh_ebp;",
    )
    trace_writes = (
        b"g_icall_trace[g_icall_trace_idx",
        b"g_icall_trace_idx++;",
        b"g_icall_count++;",
    )
    contents = b"".join(
        line for line in contents.splitlines(keepends=True)
        if not any(write in line for write in trace_writes)
    )
    if contents != original:
        header.write_bytes(contents)


if __name__ == "__main__":
    main()
