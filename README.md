# corHttp

An HTTP/1.1 server. It accepts connections, parses requests, and writes
responses — and does nothing else.

It does not route, and it does not parse JSON. A request arrives at **one
callback** with its method, path, query, headers and body, and the caller
decides everything from there. Routing belongs to the layer that owns the
service table; putting it here would mean two of them. Dropping JSON drops a
dependency and leaves the engine dealing in bytes.

The only dependencies are **kalloc and libc**.

## Design

**Zero copy.** The method, path, query, header keys and values and the body are
all pointers into the connection's read buffer, NUL-terminated in place by the
parser. Nothing is duplicated and nothing survives the callback returning.

The one deliberate exception is the query string: splitting it on `&` and `=` in
place writes a NUL over the first `=`, which truncates the raw query at its
first parameter — while callers legitimately want both views, the parameters and
the query exactly as it arrived, to forward verbatim. So the split runs on a copy
taken from the per-request pool.

**Completeness is decided before anything is written.** A request arrives in
pieces on a non-blocking socket, and the obvious arrangement — parse what is
there, return "need more" — cannot work when the parser terminates in place:
terminating a header value overwrites the byte after it, which is the CR of its
own CRLF, but for a sender using a bare LF *is* the line terminator. The
re-parse after the next read then never finds the end of that line and the
request hangs. A read-only scan decides completeness first; the destructive
parse runs exactly once.

**Edge-triggered epoll, one thread.** Level triggering re-reports a readable
socket until it is drained; edge triggering reports the transition once, so
every accept and every read loops until `EAGAIN` and nothing may return early
having done some. One thread means no lock on the connection pool, no atomic on
the free list, and no way to interleave two responses on one socket.

**Connections are pooled and reused**, allocated once at startup. Read buffers
grow on demand and are never shrunk, so the pool converges on the working set
instead of oscillating around it.

**Suspend and resume.** A caller that does I/O of its own during a request — a
database round-trip, a forward to another server — cannot run it on the event
loop without stalling every other connection. `corHttpSuspend()` takes the
connection out of the loop's care entirely; `corHttpResume()` is the only
function here that may be called from another thread, and it queues the
connection and pokes an eventfd rather than writing the socket, because two
threads writing one response is how two answers end up interleaved.

**The request is over when its bytes are out, not when the callback returns.**
A caller that hangs per-request state on `connP->userData` gets it back through
`server.doneCb` after the last byte of the response has been written — because
the response headers and body are *borrowed* from that state, not copied. It
fires once per request that reached the callback, including one whose connection
died mid-answer, and never for a request the engine refused by itself.

**An oversized body is refused at the announcement.** A `Content-Length` over
`server.maxRequestSize` means the body is never read: the request reaches the
callback with `bodyRefused` set and no body, so the answer is the caller's own —
with whatever error document it wants to send — rather than a bare status from
here. The connection is not reusable afterwards (the rest of the body is still
arriving) and is closed, which is what the `Connection: close` on that answer
says. Only a client that *lies* about its length reaches the buffer limit.

## Not implemented

**HTTP pipelining.** A second request arriving in the same packet as the first
is dropped rather than answered. No client this serves pipelines — curl does
not, browsers disabled it — and doing it properly means driving the read loop
from the response side.

**TLS.** There is no HTTPS listener. The intended deployment puts a proxy in
front, and a server that quietly served an unencrypted port when asked for TLS
would be worse than one that has none.

## Build

```console
make            # libcorHttp.a, libcorHttp.so
make di         # ... and install
make corHttpTest
```

`corHttpTest` is a server that echoes back what it parsed — method, path, query,
a chosen header, the parameter count, the body length. A test that only checked
for `200 OK` would pass with every one of those empty.

```console
$ ./corHttpTest 1041 &
$ curl "localhost:1041/ngsi-ld/v1/entities?type=T&limit=5&local"
{"method":"GET","path":"/ngsi-ld/v1/entities","query":"type=T&limit=5&local",
 "headers":3,"uriParams":3,"accept":"*/*","limit":"5","bodyLen":0}
```

## Status

In use. `corRest` selects it with `COR_HTTP_SERVER=builtin`, and the coraine
NGSI-LD broker then runs with no HTTP dependency beyond libc. Coraine's
functional suite compares captured HTTP responses line by line and goes green on
both servers — 641 tests on libmicrohttpd, 640 on this one, the difference being
the single test whose notification receiver has to serve HTTPS.

Not one expected byte changed, and that is the bar: the caller of this library
percent-decodes the path and the query itself — including `+`-means-space, which
RFC 3986 does not ask for and every HTTP client library produces anyway — so
what reaches the layer above is what reached it before.

## Licence

Apache 2.0. See [LICENSE](LICENSE).
