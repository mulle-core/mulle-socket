# mulle-socket Library Documentation for AI
<!-- Keywords: socket, tcp, unix, portable, posix, windows, networking -->

## 1. Introduction & Purpose

`mulle-socket` is a small, blocking TCP and unix-domain socket library that
presents one API on POSIX and Windows. It solves the problem that a socket is
not the same thing on the two platforms: on POSIX it is a file descriptor and
`read()`/`write()` work on it, on Windows it is a `SOCKET` handle on which the
CRT's `_read()`/`_write()` fail. The library settles that difference once, in
its own functions, instead of at every call site.

Key features:

- One handle type `mulle_socket_t` (`int` on POSIX, `SOCKET` on Windows).
- `WSAStartup` is performed lazily and idempotently; callers never see it.
- `mulle_socket_error()` always returns an `errno` value, translating Winsock
  codes where a POSIX equivalent exists.
- `SIGPIPE` never reaches the process (`MSG_NOSIGNAL` / `SO_NOSIGPIPE`).
- `EINTR` is retried internally by `read`, `write`, `accept` and `connect`.
- Timeouts are expressed in seconds and expire as `ETIMEDOUT` on every platform.
- Unix domain sockets are offered on POSIX, not on Windows.

It is a foundational component of the `mulle-core` tree; `mulle-core-demo`'s
miniredis is its first consumer. It depends only on `mulle-c11`.

## 2. Key Concepts & Design Philosophy

- **Never pretend a socket is a descriptor.** `mulle_socket_read()` and
  `mulle_socket_write()` are the only I/O entry points, and they behave the
  same on both platforms.
- **Failures are errno values.** `mulle_socket_error()` is the single way to
  learn why the previous call failed. Winsock errors with a POSIX twin are
  translated (`WSAEWOULDBLOCK`→`EAGAIN`, `WSAECONNRESET`→`ECONNRESET`,
  `WSAEADDRINUSE`→`EADDRINUSE`, `WSAESHUTDOWN`→`EPIPE`, and so on); the rest
  arrive as `EIO`.
- **Every socket-returning call yields `MULLE_SOCKET_INVALID` on failure.**
  Callers test the handle, not a separate error flag.
- **Blocking only.** There is no non-blocking mode. A call without a timeout can
  block indefinitely. A caller who needs a deadline on `connect` (bounded
  nowhere) or on `accept` on non-Linux platforms uses the platform's own
  readiness wait (`select()`) on the `mulle_socket_t`.
- **Defaults are the correct ones.** Listeners get `SO_REUSEADDR` (except on
  Windows, where it means something else); accepted and connected sockets get
  `TCP_NODELAY`.
- **Nothing is closed for you, and nothing is closed twice.** `mulle_socket_close()`
  is the one way out and accepts `MULLE_SOCKET_INVALID` so cleanup is unconditional.

## 3. Core API & Data Structures

The entire public API lives in `src/mulle-socket.h`. There is no public struct;
the central type is the opaque scalar `mulle_socket_t`.

### 3.1. `mulle-socket.h`

#### Types and macros

- `mulle_socket_t` — the socket handle. `typedef SOCKET mulle_socket_t;` on
  Windows, `typedef int mulle_socket_t;` on POSIX.
- `MULLE_SOCKET_INVALID` — the invalid handle. `INVALID_SOCKET` on Windows,
  `(-1)` on POSIX. Returned by every call that fails.
- `MULLE_SOCKET_HAVE_UNIX` — defined on POSIX when unix domain sockets are
  available (guards `mulle_socket_listen_unix`/`mulle_socket_connect_unix`).
- `MULLE__SOCKET_VERSION` — packed version `((1UL << 20) | (0 << 8) | 0)`.

#### Version functions

- `static inline unsigned int mulle_socket_get_version_major( void)` — major version.
- `static inline unsigned int mulle_socket_get_version_minor( void)` — minor version.
- `static inline unsigned int mulle_socket_get_version_patch( void)` — patch version.
- `uint32_t mulle_socket_get_version( void)` — packed version number.

#### Lifecycle and diagnostics

- `int mulle_socket_init( void)` — starts whatever the platform needs
  (`WSAStartup` on Windows, nothing on POSIX). Idempotent; returns `0`, or `-1`
  if sockets cannot be used at all. Every other entry point calls it first, so
  it is optional but available to surface failure at a chosen place.
- `int mulle_socket_error( void)` — the reason the last call failed, as an
  `errno` value; `0` when it did not fail.
- `int mulle_socket_close( mulle_socket_t socket)` — the only way to release a
  socket. Accepts `MULLE_SOCKET_INVALID` and does nothing for it. Returns `0`
  or `-1`.

#### Server side

- `mulle_socket_t mulle_socket_listen( char const *address, unsigned int port, unsigned int *p_port)`
  — a listening TCP socket. `address` is a literal IPv4 address or `NULL` for
  every interface. `port` `0` asks the platform for a free port; `*p_port`
  (which may be `NULL`) receives the bound port. Sets `SO_REUSEADDR` on POSIX.
- `mulle_socket_t mulle_socket_accept( mulle_socket_t listener)` — the next
  connection, with `TCP_NODELAY` already set, or `MULLE_SOCKET_INVALID`. Retries
  `EINTR`. A receive timeout bounds `accept` on Linux only.
- `unsigned int mulle_socket_find_free_port( void)` — a port nobody is
  listening on, for a server about to start. Binds port 0, reads the port back
  and closes, so the port is free (not reserved) on return. `0` on failure;
  inherently racy.

#### Client side

- `mulle_socket_t mulle_socket_connect( char const *host, unsigned int port)`
  — connects to `host`:`port`, where `host` may be a name. Tries every address
  the platform returns; sets `TCP_NODELAY` on success; retries `EINTR`. Not
  bounded by any timeout.
- `mulle_socket_t mulle_socket_connect_unix( char const *path)` — connects to a
  unix domain socket path (POSIX only).

#### Socket options

- `int mulle_socket_set_reuseaddr( mulle_socket_t socket)` — sets `SO_REUSEADDR`.
  On Windows this is a no-op that returns `0`, because the option means
  "let another process bind this port" there.
- `int mulle_socket_set_nodelay( mulle_socket_t socket)` — sets `TCP_NODELAY`.
- `int mulle_socket_set_read_timeout( mulle_socket_t socket, double seconds)`
  — bounds a blocking read, in seconds. `0` means wait forever (the default).
  Expiry arrives as `ETIMEDOUT`. Sub-millisecond values round up.
- `int mulle_socket_set_write_timeout( mulle_socket_t socket, double seconds)`
  — bounds a blocking write, in seconds. Same semantics as the read timeout.

#### I/O

- `intptr_t mulle_socket_read( mulle_socket_t socket, void *buffer, size_t length)`
  — reads up to `length` bytes. Negative on error (with `mulle_socket_error()`);
  `0` means the peer has closed. A short read is normal. Retries `EINTR`.
- `intptr_t mulle_socket_write( mulle_socket_t socket, void const *bytes, size_t length)`
  — writes up to `length` bytes. Negative on error; `MSG_NOSIGNAL`/`SO_NOSIGPIPE`
  is used, so a write to a closed peer reports `EPIPE` instead of raising
  `SIGPIPE`. A partial write is normal. Retries `EINTR`.
- `int mulle_socket_write_all( mulle_socket_t socket, void const *bytes, size_t length)`
  — loops `mulle_socket_write` until all bytes are gone. Returns `0` or `-1`.
  Bounded per send, not as a whole.
- `int mulle_socket_shutdown( mulle_socket_t socket)` — half close (sends
  end-of-file to the peer) so in-flight writes are not lost. Returns `0` or `-1`.

#### Unix domain sockets (POSIX only)

- `mulle_socket_t mulle_socket_listen_unix( char const *path)` — binds a unix
  domain socket path. Any existing socket file is the caller's to remove first.
- `mulle_socket_t mulle_socket_connect_unix( char const *path)` — connects to a
  unix domain socket path.

## 4. Performance Characteristics

- All operations are blocking system calls; there is no internal buffering or
  queueing. `mulle_socket_read`/`_write` cost one syscall plus the `EINTR` retry
  loop, so `O(1)`.
- `mulle_socket_write_all()` is `O(n/send_size)` sends; on POSIX a send that
  expires after moving some bytes returns the count, so a large write can take
  several multiples of the timeout before giving up. Windows fails the send
  outright.
- `mulle_socket_connect()` may try multiple resolved addresses, so worst case is
  `O(number of addresses)` connection attempts.
- `mulle_socket_find_free_port()` binds and closes a socket, which is racy and
  carries the cost of a bind plus a getsockname; it does not reserve the port.
- Not thread-safe per socket. Distinct sockets may be used from distinct threads.
  `mulle_socket_init()` is idempotent and safe to call from anywhere.

## 5. AI Usage Recommendations & Patterns

**Best practices**

- Always check the return against `MULLE_SOCKET_INVALID` (or `< 0` for `int`
  results) and read `mulle_socket_error()` immediately, before any other call.
- Use `mulle_socket_write_all()` unless you are doing your own buffering;
  `mulle_socket_write()` may write fewer bytes than requested.
- Treat `mulle_socket_read()` returning `0` as end-of-file, and negative as an
  error.
- Call `mulle_socket_close()` unconditionally in cleanup paths — it accepts
  `MULLE_SOCKET_INVALID`.
- Call `mulle_socket_set_read_timeout()` on a listener only if you are on Linux
  and want a bounded `accept`.

**Common pitfalls**

- Do not use `read()`/`write()`/`_read()`/`_write()` on a `mulle_socket_t`;
  use the library's functions on both platforms.
- Do not expect a write timeout to bound the whole of `mulle_socket_write_all()`.
- Do not expect a read timeout to bound `accept` on macOS/BSD/Windows, or any
  timeout to bound `connect`.
- Do not guard `mulle_socket_set_reuseaddr()` with `#ifdef _WIN32`; it is a
  safe no-op there.
- Do not access `MULLE_SOCKET_HAVE_UNIX` symbols without the guard; they do not
  exist on Windows.

**Idiomatic usage**

```c
#include <mulle-socket/mulle-socket.h>

unsigned int     port;
mulle_socket_t   listener;
mulle_socket_t   connection;

port     = 0;
listener = mulle_socket_listen( "127.0.0.1", 0, &port);
if( listener == MULLE_SOCKET_INVALID)
   return( -1);

connection = mulle_socket_accept( listener);
if( connection == MULLE_SOCKET_INVALID)
   return( -1);

if( mulle_socket_write_all( connection, "+PONG\r\n", 7))
   return( -1);

mulle_socket_close( connection);
mulle_socket_close( listener);
```

## 6. Integration Examples

### Example 1: A client and a server in one process

```c
#include <mulle-socket/mulle-socket.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

int   main( void)
{
   mulle_socket_t   listener;
   mulle_socket_t   client;
   mulle_socket_t   server;
   unsigned int     port;
   char             buffer[ 64];
   intptr_t         n;

   port     = 0;
   listener = mulle_socket_listen( "127.0.0.1", 0, &port);
   if( listener == MULLE_SOCKET_INVALID)
      return( 1);

   client = mulle_socket_connect( "127.0.0.1", port);
   if( client == MULLE_SOCKET_INVALID)
   {
      fprintf( stderr, "connect: %s\n", strerror( mulle_socket_error()));
      mulle_socket_close( listener);
      return( 1);
   }

   server = mulle_socket_accept( listener);
   if( server == MULLE_SOCKET_INVALID)
   {
      mulle_socket_close( client);
      mulle_socket_close( listener);
      return( 1);
   }

   if( mulle_socket_write_all( client, "PING\r\n", 6))
      return( 1);

   memset( buffer, 0, sizeof( buffer));
   n = mulle_socket_read( server, buffer, 6);
   if( n <= 0)
      return( 1);
   printf( "server got %.*s", (int) n, buffer);

   mulle_socket_close( server);
   mulle_socket_close( client);
   mulle_socket_close( listener);
   return( 0);
}
```

### Example 2: Bounded read and write

```c
#include <mulle-socket/mulle-socket.h>
#include <errno.h>
#include <string.h>

int   exchange_with_deadline( mulle_socket_t socket)
{
   char     buffer[ 64];
   intptr_t n;

   if( mulle_socket_set_read_timeout( socket, 0.25))
      return( -1);

   n = mulle_socket_read( socket, buffer, sizeof( buffer));
   if( n < 0)
   {
      if( mulle_socket_error() == ETIMEDOUT)
         return( -2);   /* the read gave up, same test on every platform */
      return( -1);
   }

   if( mulle_socket_set_write_timeout( socket, 0.25))
      return( -1);

   if( mulle_socket_write_all( socket, buffer, (size_t) n))
      return( -1);

   return( 0);
}
```

### Example 3: A unix domain socket (POSIX)

```c
#include <mulle-socket/mulle-socket.h>
#include <unistd.h>

#ifdef MULLE_SOCKET_HAVE_UNIX
int   unix_roundtrip( char const *path)
{
   mulle_socket_t   listener;
   mulle_socket_t   client;
   mulle_socket_t   server;

   unlink( path);   /* a stale socket file is the caller's to remove */

   listener = mulle_socket_listen_unix( path);
   if( listener == MULLE_SOCKET_INVALID)
      return( -1);

   client = mulle_socket_connect_unix( path);
   server = mulle_socket_accept( listener);

   mulle_socket_close( server);
   mulle_socket_close( client);
   mulle_socket_close( listener);
   return( 0);
}
#endif
```

## 7. Dependencies

Direct `mulle-sde` dependencies:

- `mulle-c11`

On Windows the system library `ws2_32` is required (declared as a
platform-only, non-build dependency in the sourcetree config).
