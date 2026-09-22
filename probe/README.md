# probe

Tools that measure a platform instead of asserting things about it, and so do
not belong in `test/`.

## timeout-probe

`mulle_socket_set_read_timeout` and `mulle_socket_set_write_timeout` rest on
`SO_RCVTIMEO` and `SO_SNDTIMEO`, and the documentation does not agree on whether
those reach `accept()`. So this asks one question at a time and prints as it
goes, which is the whole design: a platform that does not honour the option
hangs rather than fails, and a probe that hangs before its first line measures
nothing. Run it under an external bound and the last line printed is the answer:

```bash
cc -std=gnu11 -isystem <mulle-core>/src -o timeout-probe timeout-probe.c \
   -L<build>/mulle-core -lmulle-core -lpthread    # + -lws2_32 on Windows
timeout 30 ./timeout-probe
```

What it answered, as of 2026-09-22:

| | read | write | accept |
|---|---|---|---|
| Linux | `ETIMEDOUT` @0.25s | `ETIMEDOUT` @0.77s | **`ETIMEDOUT` @0.30s** |
| macOS | `ETIMEDOUT` @0.25s | `ETIMEDOUT` @0.50s | **hangs** |
| Windows (wine) | `ETIMEDOUT` @0.25s | `ETIMEDOUT` @0.25s | **hangs** |

Read and write are uniform: a bounded call returns `-1` with `ETIMEDOUT` on all
three, which is the whole reason the setters exist rather than a documented raw
`setsockopt`. `accept()` is bounded on **Linux only** — its kernel applies a
receive timeout there, and BSD/macOS and Winsock do not.

Two of those numbers need a word, or they look like noise:

- the write timeout takes *longer* on POSIX than the value asked for, because an
  expiring `send` returns the bytes it did move, so `write_all` earns several
  timeouts' worth of progress before giving up; Windows fails the send outright
  and is exact.
- `connect()` is absent from the table because no socket option expresses it on
  any of the three. It is the caller's, and that is documented as such.
