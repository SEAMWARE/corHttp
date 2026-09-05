//
// FILE            corHttpParse.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Request parsing, in place.
//
// The parser writes NUL terminators INTO the read buffer and points the request
// slices at it. That is what makes it zero-copy, and it is also what makes the
// order of the two steps below load-bearing.
//
// A request arrives in pieces on a non-blocking socket, so SOMETHING has to
// decide when it is whole. The obvious arrangement - parse what is there, and
// return "again" if it runs out - cannot work here, because by then the parse
// has already overwritten delimiters with NULs. A header value is terminated by
// writing over the byte after it, which is the CR of its own CRLF; where the
// sender used a bare LF, that byte IS the line terminator, and the re-parse
// after the next read can no longer find the end of the line. The request then
// never completes and the connection sits until the idle sweep takes it.
//
// So completeness is decided FIRST, by a scan that writes nothing, and the
// destructive parse runs exactly once on a request already known to be whole.
//
#include <stdlib.h>                              // atoi
#include <string.h>                              // memchr, memcmp, memcpy, strlen, strncasecmp

#include "kalloc/kaAlloc.h"                      // kaAlloc

#include "corHttp/CorHttp.h"                     // CorHttpConn, CorHttpStatus
#include "corHttp/corHttpInternal.h"             // Own interface



// -----------------------------------------------------------------------------
//
// lineEnd - find the end of a line, tolerating a bare LF
//
// Returns the start of the NEXT line and sets *lenP to the length of this one
// WITHOUT its terminator. NULL means the line has not fully arrived.
//
// RFC 9112 § 2.2 requires CRLF and allows a recipient to accept a bare LF; we
// accept it, because rejecting it turns a hand-typed curl or a netcat probe
// into a parse error for no gain in correctness.
//
static char* lineEnd(char* buf, int len, int* lenP)
{
  char* p   = buf;
  char* end = buf + len;

  while (p < end)
  {
    if (*p == '\n')
    {
      *lenP = (p > buf && p[-1] == '\r') ? (int) (p - buf - 1) : (int) (p - buf);
      return p + 1;
    }
    p++;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// requestLine - "METHOD <path>[?<query>] HTTP/1.x"
//
static CorHttpStatus requestLine(CorHttpConn* connP, char* line, int lineLen)
{
  char* p     = line;
  char* end   = line + lineLen;
  char* start = p;

  while ((p < end) && (*p != ' '))
    p++;

  if (p == end)
    return CorHttpParseError;

  connP->method.s   = start;
  connP->method.len = (int) (p - start);
  *p = 0;
  p++;

  while ((p < end) && (*p == ' '))
    p++;

  start = p;
  while ((p < end) && (*p != ' ') && (*p != '?'))
    p++;

  connP->path.s   = start;
  connP->path.len = (int) (p - start);

  if ((p < end) && (*p == '?'))
  {
    *p = 0;
    p++;

    connP->query.s = p;
    while ((p < end) && (*p != ' '))
      p++;
    connP->query.len = (int) (p - connP->query.s);
  }

  if (p < end)
  {
    *p = 0;
    p++;
  }

  while ((p < end) && (*p == ' '))
    p++;

  connP->version.s   = p;
  connP->version.len = (int) (end - p);

  //
  // Terminate the last field on the line, whichever it turned out to be. The
  // loops above only write a NUL where they found a SPACE, so a truncated
  // request line - "GET /x", no version - would leave the path running into
  // the CRLF and the headers after it. The byte written over is the line
  // terminator, and lineEnd() has already handed the caller the start of the
  // next line, so there is nothing left that needs it.
  //
  *end = 0;

  //
  // HTTP/1.1 is keep-alive unless it says otherwise; 1.0 is the reverse. The
  // Connection header, parsed below, overrides either way.
  //
  if ((connP->version.len >= 8) && (memcmp(connP->version.s, "HTTP/1.0", 8) == 0))
    connP->keepAlive = false;

  return CorHttpOk;
}



// -----------------------------------------------------------------------------
//
// header - one "Key: value" line
//
static CorHttpStatus header(CorHttpConn* connP, char* line, int lineLen)
{
  char* colon = (char*) memchr(line, ':', lineLen);

  if (colon == NULL)
    return CorHttpParseError;

  //
  // Past the limit the line is still CONSUMED and only dropped - stopping here
  // would leave the rest of the request unparsed and the body at the wrong
  // offset. The flag lets the caller answer 431 instead of silently serving a
  // request whose headers it never saw.
  //
  if (connP->headers >= COR_HTTP_MAX_HEADERS)
  {
    connP->headersTruncated = true;
    return CorHttpOk;
  }

  CorHttpKeyValue* kvP = &connP->header[connP->headers];

  kvP->key.s   = line;
  kvP->key.len = (int) (colon - line);
  *colon = 0;

  char* value = colon + 1;
  while ((*value == ' ') || (*value == '\t'))
    value++;

  kvP->value.s   = value;
  kvP->value.len = lineLen - (int) (value - line);
  value[kvP->value.len] = 0;

  connP->headers++;

  if ((kvP->key.len == 14) && (strncasecmp(kvP->key.s, "Content-Length", 14) == 0))
    connP->contentLength = atoi(kvP->value.s);
  else if ((kvP->key.len == 10) && (strncasecmp(kvP->key.s, "Connection", 10) == 0))
  {
    if (strncasecmp(kvP->value.s, "close", 5) == 0)
      connP->keepAlive = false;
    else if (strncasecmp(kvP->value.s, "keep-alive", 10) == 0)
      connP->keepAlive = true;
  }

  return CorHttpOk;
}



// -----------------------------------------------------------------------------
//
// uriParams - split the query string on & and =
//
// NOT percent-decoded here. The decoding rules differ per parameter in
// NGSI-LD - a value that is itself a URL, a q-filter carrying its own '=' -
// and a parser that decoded eagerly would destroy the distinction between a
// delimiter and an encoded one before the layer that knows which is which ever
// saw it.
//
// Split on a COPY, which is the one place this parser gives up zero-copy and
// does so deliberately. Splitting in place means writing a NUL over the first
// '=', and the raw query string is then truncated at its first parameter -
// while callers legitimately want BOTH: the parameters, and the query exactly
// as it arrived, to forward verbatim to another context source. The copy costs
// one small allocation from the per-request pool on requests that have a query,
// and it is the difference between the two views being available and one of
// them silently destroying the other.
//
static void uriParams(CorHttpConn* connP)
{
  if ((connP->query.s == NULL) || (connP->query.len == 0))
    return;

  char* copy = kaAlloc(&connP->alloc, connP->query.len + 1);

  if (copy == NULL)
  {
    connP->uriParamsTruncated = true;            // no params rather than a broken query
    return;
  }

  memcpy(copy, connP->query.s, connP->query.len);
  copy[connP->query.len] = 0;

  char* p   = copy;
  char* end = copy + connP->query.len;

  while (p < end)
  {
    if (connP->uriParams >= COR_HTTP_MAX_URI_PARAMS)
    {
      connP->uriParamsTruncated = true;
      return;
    }

    CorHttpKeyValue* kvP = &connP->uriParam[connP->uriParams];

    kvP->key.s = p;
    while ((p < end) && (*p != '=') && (*p != '&'))
      p++;
    kvP->key.len = (int) (p - kvP->key.s);

    if ((p < end) && (*p == '='))
    {
      *p = 0;
      p++;

      kvP->value.s = p;
      while ((p < end) && (*p != '&'))
        p++;
      kvP->value.len = (int) (p - kvP->value.s);

      if (p < end)
      {
        *p = 0;
        p++;
      }
    }
    else
    {
      //
      // A parameter with no '=' is present with an empty value, not absent -
      // "?local" and "?local=" mean the same thing, and neither means "no
      // local". The empty string is a literal rather than a slice of the
      // buffer: there is nothing in the buffer to point at.
      //
      kvP->value.s   = (char*) "";
      kvP->value.len = 0;

      if (p < end)
      {
        *p = 0;
        p++;
      }
    }

    connP->uriParams++;
  }
}



// -----------------------------------------------------------------------------
//
// headerSectionEnd - first byte after the blank line, or NULL if it has not arrived
//
// Reads only. Accepts CRLFCRLF and LFLF, and the mixed forms in between, for
// the same reason lineEnd() does.
//
static char* headerSectionEnd(char* buf, int len)
{
  for (int ix = 0; ix < len; ix++)
  {
    if (buf[ix] != '\n')
      continue;

    if ((ix + 1 < len) && (buf[ix + 1] == '\n'))
      return &buf[ix + 2];

    if ((ix + 2 < len) && (buf[ix + 1] == '\r') && (buf[ix + 2] == '\n'))
      return &buf[ix + 3];
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// contentLengthPeek - the declared body length, without touching the buffer
//
// -1 when the header is absent, which is not the same as 0: absent means "no
// body", 0 means "a body of no bytes", and only the first is allowed to be a
// GET. The caller keeps the distinction.
//
// Matched at a line start only, so a "Content-Length" appearing inside another
// header's VALUE cannot be mistaken for the header itself.
//
static int contentLengthPeek(char* buf, char* headerEnd)
{
  static const char  name[]  = "content-length";
  const int          nameLen = sizeof(name) - 1;
  char*              p       = buf;

  // Past the request line - a request line cannot carry a header.
  while ((p < headerEnd) && (*p != '\n'))
    p++;

  while (p < headerEnd)
  {
    p++;                                         // step over the newline, onto a header name

    if ((headerEnd - p) < nameLen + 1)
      break;

    if (strncasecmp(p, name, nameLen) == 0)
    {
      char* v = p + nameLen;

      while ((v < headerEnd) && ((*v == ' ') || (*v == '\t')))
        v++;

      if ((v < headerEnd) && (*v == ':'))
      {
        v++;
        while ((v < headerEnd) && ((*v == ' ') || (*v == '\t')))
          v++;

        return atoi(v);                          // atoi stops at the CR; nothing is written
      }
    }

    while ((p < headerEnd) && (*p != '\n'))
      p++;
  }

  return -1;
}



// -----------------------------------------------------------------------------
//
// expectContinuePeek - did the client ask to be told to go ahead?
//
// Read-only, like contentLengthPeek and for the same reason: it runs while the
// request is still incomplete, and writing into the buffer before it is whole
// is what the note at the top of this file is about.
//
static bool expectContinuePeek(char* buf, char* headerEnd)
{
  static const char  name[]  = "expect";
  const int          nameLen = sizeof(name) - 1;
  char*              p       = buf;

  while ((p < headerEnd) && (*p != '\n'))       // past the request line
    p++;

  while (p < headerEnd)
  {
    p++;

    if ((headerEnd - p) < nameLen + 1)
      break;

    if (strncasecmp(p, name, nameLen) == 0)
    {
      char* v = p + nameLen;

      while ((v < headerEnd) && ((*v == ' ') || (*v == '\t')))
        v++;

      if ((v < headerEnd) && (*v == ':'))
      {
        v++;
        while ((v < headerEnd) && ((*v == ' ') || (*v == '\t')))
          v++;

        if (((headerEnd - v) >= 12) && (strncasecmp(v, "100-continue", 12) == 0))
          return true;
      }
    }

    while ((p < headerEnd) && (*p != '\n'))
      p++;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// corHttpParse -
//
CorHttpStatus corHttpParse(CorHttpConn* connP)
{
  char*         buf = connP->buf;
  char*         end = buf + connP->bufUsed;
  char*         p   = buf;
  char*         next;
  int           lineLen;
  CorHttpStatus s;

  //
  // Step one, read-only: is the whole request here? Until it is, not a byte of
  // the buffer may be modified - see the note at the top of this file.
  //
  char* headerEnd = headerSectionEnd(buf, connP->bufUsed);

  if (headerEnd == NULL)
    return CorHttpAgain;

  int declaredLength = contentLengthPeek(buf, headerEnd);

  //
  // A body the server has already decided it will not accept is not read at
  // all. The request goes up WITHOUT it, and the caller - which knows what a
  // refusal should look like, and this library does not - answers from the
  // Content-Length header, which is still there.
  //
  // The connection cannot survive it: the rest of the body is on its way and
  // would be read as the start of the next request. So it is answered and
  // closed, which is what `Connection: close` on that answer says.
  //
  if ((connP->serverP != NULL) && (connP->serverP->maxRequestSize > 0) &&
      (declaredLength > connP->serverP->maxRequestSize))
  {
    connP->bodyRefused = true;
    connP->keepAlive   = false;
  }
  else if ((declaredLength > 0) && ((int) (end - headerEnd) < declaredLength))
  {
    //
    // The headers are all here and the body is not - which is exactly the
    // moment a client that sent `Expect: 100-continue` is waiting for an
    // answer before it sends one. Say so on the way out; the loop does the
    // writing. Forgetting this does not fail, it STALLS: curl waits a second
    // and sends the body anyway, so the symptom is a suite that passes slowly.
    //
    if (expectContinuePeek(buf, headerEnd) == true)
      connP->expectContinue = true;

    return CorHttpAgain;
  }

  //
  // Step two, destructive, and reached exactly once per request.
  //
  connP->headers            = 0;
  connP->uriParams          = 0;
  connP->headersTruncated   = false;
  connP->uriParamsTruncated = false;
  connP->contentLength      = -1;
  connP->body.s             = NULL;
  connP->body.len           = 0;
  connP->query.s            = NULL;
  connP->query.len          = 0;

  next = lineEnd(p, connP->bufUsed, &lineLen);
  if (next == NULL)
    return CorHttpAgain;

  if ((s = requestLine(connP, p, lineLen)) != CorHttpOk)
    return s;

  p = next;

  while (p < end)
  {
    next = lineEnd(p, (int) (end - p), &lineLen);
    if (next == NULL)
      return CorHttpAgain;

    if (lineLen == 0)      // the empty line that ends the header section
    {
      p = next;
      break;
    }

    if ((s = header(connP, p, lineLen)) != CorHttpOk)
      return s;

    p = next;
  }

  uriParams(connP);

  if ((connP->contentLength > 0) && (connP->bodyRefused == false))
  {
    if ((int) (end - p) < connP->contentLength)
      return CorHttpAgain;

    connP->body.s   = p;
    connP->body.len = connP->contentLength;

    //
    // Terminate the body too. It is the last thing in the buffer, and the byte
    // after it is inside the allocation - corHttpConnBufGrow keeps one spare
    // for exactly this - so a caller can hand the body to a JSON parser that
    // expects a C string without copying it first.
    //
    connP->body.s[connP->body.len] = 0;
  }

  return CorHttpOk;
}



// -----------------------------------------------------------------------------
//
// corHttpHeader -
//
const char* corHttpHeader(CorHttpConn* connP, const char* key)
{
  int keyLen = strlen(key);

  for (int ix = 0; ix < connP->headers; ix++)
  {
    if ((connP->header[ix].key.len == keyLen) && (strncasecmp(connP->header[ix].key.s, key, keyLen) == 0))
      return connP->header[ix].value.s;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// corHttpUriParam -
//
// Case SENSITIVE, unlike the header lookup: header names are case-insensitive
// per RFC 9110 § 5.1 and URI parameter names are not.
//
const char* corHttpUriParam(CorHttpConn* connP, const char* key)
{
  int keyLen = strlen(key);

  for (int ix = 0; ix < connP->uriParams; ix++)
  {
    if ((connP->uriParam[ix].key.len == keyLen) && (memcmp(connP->uriParam[ix].key.s, key, keyLen) == 0))
      return connP->uriParam[ix].value.s;
  }

  return NULL;
}
