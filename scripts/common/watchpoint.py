"""Data watchpoints over the GDB remote protocol.

`probe.py` answers "what is the target doing"; this answers "who wrote here".
When guest memory is being corrupted, sampling never catches the culprit -- the
write is one instruction out of millions -- but a watchpoint stops the machine
ON it, and the ARM PC at that stop names the interpreter handler responsible.

gwemu is QEMU-based, so its stub implements Z2/Z3 (write/access watchpoints)
without the four-comparator limit the DWT imposes; on silicon the same packets
reach the DWT through OpenOCD, so keep the count small if a run is ever aimed
there.

`backend.resume()` deliberately does not wait for a reply, because plain
continue only answers when the target stops. That is exactly what a watchpoint
produces, so `wait_for_stop` reads the pending stop packet off the socket
itself rather than going through the request/response helper.
"""

import socket


def set_write_watchpoint(backend, addr, length):
    """Z2 = write watchpoint over [addr, addr+length)."""
    reply = backend._send_command(b"Z2,%x,%x" % (addr, length))
    if reply != b"OK":
        raise RuntimeError(f"target refused a write watchpoint at "
                           f"0x{addr:08x}/{length}: {reply!r}")


def clear_write_watchpoint(backend, addr, length):
    backend._send_command(b"z2,%x,%x" % (addr, length))


def wait_for_stop(backend, timeout):
    """Resume, then block until the target stops. Returns the stop reply, or
    None on timeout -- in which case the target is halted first, because
    reading a running target gives values that never coexisted."""
    backend.resume()
    old = backend._socket.gettimeout()
    backend._socket.settimeout(timeout)
    try:
        return backend._wait_for_packet()
    except Exception:
        backend.halt()
        return None
    finally:
        backend._socket.settimeout(old)
        backend._is_running = False
