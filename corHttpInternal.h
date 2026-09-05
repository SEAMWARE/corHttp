#ifndef CORHTTP_CORHTTPINTERNAL_H_
#define CORHTTP_CORHTTPINTERNAL_H_

//
// FILE            corHttpInternal.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Internal to corHttp. Nothing outside the library includes this.
//
#include "corHttp/CorHttp.h"                     // CorHttpConn, CorHttpServer, CorHttpStatus



// -----------------------------------------------------------------------------
//
// Connection state
//
// Three states and not more: a connection is reading a request, writing the
// response, or idle between the two waiting for the next one on a keep-alive.
// Everything else that could be a state - "parsing", "handling" - happens
// inside one epoll wakeup and never outlives it, so it would be a state the
// event loop could never observe.
//
#define COR_HTTP_CONN_FREE     0
#define COR_HTTP_CONN_READING  1
#define COR_HTTP_CONN_WRITING  2
#define COR_HTTP_CONN_IDLE     3

#define COR_HTTP_MAX_EVENTS    256



// corHttpConnPoolInit  - allocate the pool and thread the free list
extern CorHttpStatus  corHttpConnPoolInit(CorHttpServer* serverP, int size);
extern void           corHttpConnPoolRelease(CorHttpServer* serverP);

// corHttpConnGet/Put   - take one from the free list / give it back (and close the fd)
extern CorHttpConn*   corHttpConnGet(CorHttpServer* serverP, int fd);
extern void           corHttpConnPut(CorHttpServer* serverP, CorHttpConn* connP);

// corHttpConnReset     - between two requests on the SAME connection
extern void           corHttpConnReset(CorHttpConn* connP);

// corHttpConnBufGrow   - make room for at least 'need' more bytes
extern CorHttpStatus  corHttpConnBufGrow(CorHttpServer* serverP, CorHttpConn* connP, int need);

// corHttpParse         - CorHttpAgain until the whole request has arrived
extern CorHttpStatus  corHttpParse(CorHttpConn* connP);

// corHttpResponseRender - build the wire bytes into connP->writeBuf
extern CorHttpStatus  corHttpResponseRender(CorHttpConn* connP);

extern uint64_t       corHttpNowMs(void);

#endif  // CORHTTP_CORHTTPINTERNAL_H_
