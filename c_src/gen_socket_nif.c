/*
 * Copyright (c) 2010-2012, Travelping GmbH <info@travelping.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * Neither the name of the author nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <err.h>

#include <unistd.h>
#include <sys/types.h>

#include <netinet/in.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>

#include <sys/errno.h>

#include <sys/ioctl.h>

#include "erl_nif.h"
#include "erl_driver.h" // for erl_errno_id

#ifndef UNIX_PATH_MAX
    #define UNIX_PATH_MAX  sizeof(((struct sockaddr_un *)0)->sun_path)
#endif

static ERL_NIF_TERM atom_ok;
static ERL_NIF_TERM atom_eof;
static ERL_NIF_TERM atom_error;

// AF NAMES
static ERL_NIF_TERM atom_unix;
static ERL_NIF_TERM atom_inet4;

// -------------------------------------------------------------------------------------------------
// -- MISC INTERNAL HELPER FUNCTIONS
#define enif_get_ssize enif_get_long

inline static ERL_NIF_TERM
error_tuple(ErlNifEnv *env, int errnum)
{
    return enif_make_tuple2(env, atom_error, enif_make_atom(env, erl_errno_id(errnum)));
}

#define fionread(len)					\
    do {						\
	if (len < 0) {					\
	    int i;					\
	    						\
	    if (ioctl(socket, FIONREAD, &i) < 0)	\
		return error_tuple(env, errno);		\
	    else					\
		len = i;				\
	}						\
							\
	if (len == 0)					\
	    len = 1024;					\
	if (len > SSIZE_MAX)				\
	    len = SSIZE_MAX;				\
    } while (0)

// -------------------------------------------------------------------------------------------------
// -- SOCKADDR TO ERLANG TERM
static ERL_NIF_TERM
sockaddr_unix_to_term(ErlNifEnv* env, const struct sockaddr_un* addr)
{
    ERL_NIF_TERM path_binary;
    size_t path_size = strlen(addr->sun_path);
    void* path_data = enif_make_new_binary(env, path_size, &path_binary);
    memcpy(path_data, addr->sun_path, path_size);

    // {unix, Path}
    return enif_make_tuple2(env, atom_unix, path_binary);
}

static ERL_NIF_TERM
sockaddr_inet4_to_term(ErlNifEnv* env, const struct sockaddr_in* addr)
{
    struct in_addr in_addr = addr->sin_addr;

    // {inet4, {A,B,C,D}, Port}
    return enif_make_tuple3(env,
                            atom_inet4,
                            enif_make_tuple4(env,
                                             enif_make_int(env, (in_addr.s_addr & 0x000000FF)),
                                             enif_make_int(env, (in_addr.s_addr & 0x0000FF00) >> 8),
                                             enif_make_int(env, (in_addr.s_addr & 0x00FF0000) >> 16),
                                             enif_make_int(env, (in_addr.s_addr & 0xFF000000) >> 24)),
                            enif_make_int(env, (int) ntohs(addr->sin_port)));
}

static ERL_NIF_TERM
sockaddr_unknown_to_term(ErlNifEnv* env, const struct sockaddr_storage* addr, socklen_t addrlen)
{
    ERL_NIF_TERM addr_binary;
    void* addr_data = enif_make_new_binary(env, addrlen, &addr_binary);
    memcpy(addr_data, addr, addrlen);

    // Address::binary()
    return addr_binary;
}

static ERL_NIF_TERM
sockaddr_to_term(ErlNifEnv* env, const struct sockaddr_storage* addr, const socklen_t addrlen)
{
    switch (((struct sockaddr *)addr)->sa_family) {
        case AF_UNIX:
            return sockaddr_unix_to_term(env, (struct sockaddr_un*) addr);
        case AF_INET:
            return sockaddr_inet4_to_term(env, (struct sockaddr_in*) addr);
        default:
            return sockaddr_unknown_to_term(env, addr, addrlen);
    }
}

// -------------------------------------------------------------------------------------------------
// -- ERLANG TERM TO SOCKADDR
static socklen_t
inet4_tuple_to_sockaddr(ErlNifEnv* env,
                        int arity,
                        const ERL_NIF_TERM* tuple,
                        struct sockaddr_in* addr,
                        socklen_t* addrlen)
{
    socklen_t required_addrlen = (socklen_t) sizeof(struct sockaddr_in);
    const ERL_NIF_TERM* ip_tuple;
    int ip_arity;
    unsigned int port = 0;
    unsigned int ip[4];

    if (arity != 3
        || !enif_get_tuple(env, tuple[1], &ip_arity, &ip_tuple)
        || ip_arity != 4
        || !enif_get_uint(env, ip_tuple[0], &ip[0]) || ip[0] > 255
        || !enif_get_uint(env, ip_tuple[1], &ip[1]) || ip[1] > 255
        || !enif_get_uint(env, ip_tuple[2], &ip[2]) || ip[2] > 255
        || !enif_get_uint(env, ip_tuple[3], &ip[3]) || ip[3] > 255
        || !enif_get_uint(env, tuple[2], &port) || port > UINT16_MAX) {
        return 0;
    }

    if (*addrlen >= required_addrlen) {
        addr->sin_family = AF_INET;
        addr->sin_port = htons((in_port_t) port);
        addr->sin_addr = (struct in_addr) {
            .s_addr = ip[0] | ip[1] << 8 | ip[2] << 16 | ip[3] << 24
        };
    }

    *addrlen = required_addrlen;
    return required_addrlen;
}

static socklen_t
unix_tuple_to_sockaddr(ErlNifEnv* env,
                       int arity,
                       const ERL_NIF_TERM* tuple,
                       struct sockaddr_un* addr,
                       socklen_t* addrlen)
{
    ErlNifBinary path;

    if (arity != 2
        || !enif_inspect_iolist_as_binary(env, tuple[1], &path)
        || path.size > UNIX_PATH_MAX) {
        return 0;
    }

    socklen_t required_addrlen = (socklen_t) offsetof(struct sockaddr_un, sun_path) + path.size + 1;

    if (*addrlen >= required_addrlen) {
        addr->sun_family = AF_UNIX;
        addr->sun_path[path.size] = 0;
        memcpy(addr->sun_path, path.data, path.size);
    }

    *addrlen = required_addrlen;
    return required_addrlen;
}

static socklen_t
term_to_sockaddr(ErlNifEnv* env, ERL_NIF_TERM term, struct sockaddr* addr, socklen_t* addrlen)
{
    ErlNifBinary addr_bin;
    const ERL_NIF_TERM* tuple;
    int arity;

    // binaries are used as-is
    if (enif_inspect_binary(env, term, &addr_bin)) {
        if (*addrlen >= addr_bin.size)
            memcpy(addr, addr_bin.data, addr_bin.size);
        *addrlen = addr_bin.size;
        return addr_bin.size;
    }

    if (enif_get_tuple(env, term, &arity, &tuple) && (arity > 1)) {
        if (enif_is_identical(tuple[0], atom_inet4)) {
            return inet4_tuple_to_sockaddr(env, arity, tuple, (struct sockaddr_in*) addr, addrlen);
        } else if (enif_is_identical(tuple[0], atom_unix)) {
            return unix_tuple_to_sockaddr(env, arity, tuple, (struct sockaddr_un*) addr, addrlen);
        } else {
            return 0;
        }
    } else {
        return 0;
    }
}

// -------------------------------------------------------------------------------------------------
// -- NIF FUNCTIONS
int load(ErlNifEnv* env, void** priv_data, ERL_NIF_TERM load_info)
{
    atom_ok      = enif_make_atom(env, "ok");
    atom_eof     = enif_make_atom(env, "eof");
    atom_error   = enif_make_atom(env, "error");
    atom_unix    = enif_make_atom(env, "unix");
    atom_inet4   = enif_make_atom(env, "inet4");

    return 0;
}

// param 0: socket address term
static ERL_NIF_TERM
nif_encode_sockaddr(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    struct sockaddr* addr = NULL;
    socklen_t addrlen = 0;
    ERL_NIF_TERM addr_bin;

    // dry run to get size
    if (!term_to_sockaddr(env, argv[0], NULL, &addrlen))
        return enif_make_badarg(env);

    addr = (struct sockaddr*) enif_make_new_binary(env, addrlen, &addr_bin);

    if (!term_to_sockaddr(env, argv[0], addr, &addrlen))
        return enif_make_badarg(env);

    return addr_bin;
}

// param 0: socket address binary ad returned by nif_encode_sockaddr/1
static ERL_NIF_TERM
nif_decode_sockaddr(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    ErlNifBinary addr_bin;

    if (!enif_inspect_binary(env, argv[0], &addr_bin))
        return enif_make_badarg(env);

    return sockaddr_to_term(env, (struct sockaddr_storage *)addr_bin.data, (socklen_t)addr_bin.size);
}

// param 0: socket
static ERL_NIF_TERM
nif_getsockname(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    int socket;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

    if (!enif_get_int(env, argv[0], &socket))
        return enif_make_badarg(env);

    if (getsockname(socket, (struct sockaddr*) &addr, &addrlen) != 0)
        return error_tuple(env, errno);

    return sockaddr_to_term(env, &addr, addrlen);
}

// param 0: socket
static ERL_NIF_TERM
nif_getpeername(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    int socket;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

    if (!enif_get_int(env, argv[0], &socket))
        return enif_make_badarg(env);

    if (getpeername(socket, (struct sockaddr*) &addr, &addrlen) != 0)
        return error_tuple(env, errno);

    return sockaddr_to_term(env, &addr, addrlen);
}

// param 0: socket descriptor
// param 1: socket address term
static ERL_NIF_TERM
nif_bind(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int socket;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

    if (!enif_get_int(env, argv[0], &socket)
	|| !term_to_sockaddr(env, argv[1], (struct sockaddr*) &addr, &addrlen))
        return enif_make_badarg(env);

    if (addrlen > sizeof(addr))
        return error_tuple(env, E2BIG);

    if (bind(socket, (struct sockaddr*) &addr, addrlen))
        return error_tuple(env, errno);

    return atom_ok;
}

// param 0: socket descriptor
// param 1: socket address term
static ERL_NIF_TERM
nif_connect(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int socket;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

    if (!enif_get_int(env, argv[0], &socket)
	|| !term_to_sockaddr(env, argv[1], (struct sockaddr*) &addr, &addrlen))
        return enif_make_badarg(env);

    if (addrlen > sizeof(addr))
        return error_tuple(env, E2BIG);

    if (connect(socket, (struct sockaddr*) &addr, addrlen))
        return error_tuple(env, errno);

    return atom_ok;
}

/*  0: procotol, 1: type, 2: family */
static ERL_NIF_TERM
nif_socket(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int s = -1;
    int family = 0;
    int type = 0;
    int protocol = 0;
    int flags = 0;

    if (!enif_get_int(env, argv[0], &family)
	|| !enif_get_int(env, argv[1], &type)
	|| !enif_get_int(env, argv[2], &protocol))
        return enif_make_badarg(env);

    s = socket(family, type, protocol);
    if (s < 0)
        return error_tuple(env, errno);

    flags = fcntl(s, F_GETFL, 0);
    flags |= O_NONBLOCK;
    (void)fcntl(s, F_SETFL, flags);

    return enif_make_tuple(env, 2,
           atom_ok,
           enif_make_int(env, s));
}

// param 0: socket
// param 1: backlog
static ERL_NIF_TERM
nif_listen(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int socket;
    int backlog = 5;

    if (!enif_get_int(env, argv[0], &socket)
	|| !enif_get_int(env, argv[1], &backlog))
        return enif_make_badarg(env);

    if (listen(socket, backlog) < 0)
        return error_tuple(env, errno);

    return atom_ok;
}

// param 0: socket
static ERL_NIF_TERM
nif_accept(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int socket, newfd;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

    if (!enif_get_int(env, argv[0], &socket))
        return enif_make_badarg(env);

    if ((newfd = accept(socket, (struct sockaddr*)&addr, &addrlen)) < 0)
        return error_tuple(env, errno);

    return enif_make_tuple3(env,
                            atom_ok,
			    enif_make_int(env, newfd),
                            sockaddr_to_term(env, &addr, addrlen));
}

// param 0: socket
// param 1: number of bytes to receive, determined automatically if negative
static ERL_NIF_TERM
nif_recv(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int socket;
    ErlNifBinary buffer;
    ssize_t len = 0;

    if (!enif_get_int(env, argv[0], &socket)
	|| !enif_get_ssize(env, argv[1], &len))
        return enif_make_badarg(env);

    fionread(len);

    if (!enif_alloc_binary(len, &buffer))
        return enif_make_badarg(env);

    while (42) {
	if ((len = recv(socket, buffer.data, (size_t) len, MSG_DONTWAIT)) >= 0)
	    break;

	switch (errno) {
	case EINTR:
	    continue;

	default:
	    enif_release_binary(&buffer);
	    return error_tuple(env, errno);
	}
    }

    if (len == 0) {
	enif_release_binary(&buffer);
	return atom_eof;
    }

    if (len < buffer.size)
        enif_realloc_binary(&buffer, len);

    return enif_make_tuple2(env, atom_ok, enif_make_binary(env, &buffer));
}

// param 0: socket
// param 1: number of bytes to receive, determined automatically if negative
static ERL_NIF_TERM
nif_recvfrom(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int socket;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    ssize_t len = 0;

    ErlNifBinary buffer;

    if (!enif_get_int(env, argv[0], &socket)
	|| !enif_get_ssize(env, argv[1], &len))
        return enif_make_badarg(env);

    fionread(len);

    if (!enif_alloc_binary(len, &buffer))
        return error_tuple(env, ENOMEM);

    while (42) {
	if ((len = recvfrom(socket, buffer.data, len, MSG_DONTWAIT, (struct sockaddr*)&addr, &addrlen)) >= 0)
	    break;

	switch (errno) {
	case EINTR:
	    continue;
	    
	default:
	    enif_release_binary(&buffer);
	    return error_tuple(env, errno);
	}
    }

    if (len == 0) {
	enif_release_binary(&buffer);
	return atom_eof;
    }

    if (len < buffer.size)
        enif_realloc_binary(&buffer, len);

    return enif_make_tuple3(env,
                            atom_ok,
                            sockaddr_to_term(env, &addr, addrlen),
                            enif_make_binary(env, &buffer));
}

// param 0: socket
// param 1: data
// param 2: flags
static ERL_NIF_TERM
nif_send(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int socket;
    ErlNifBinary data;
    int flags;
    ssize_t len;

    if (!enif_get_int(env, argv[0], &socket)
	|| !enif_inspect_binary(env, argv[1], &data)
	|| !enif_get_int(env, argv[2], &flags))
        return enif_make_badarg(env);

    flags |= MSG_NOSIGNAL;
    flags |= MSG_DONTWAIT;

    while (42) {
	if ((len = send(socket, data.data, data.size, flags)) >= 0)
	    break;

	switch (errno) {
	case EINTR:
	    continue;
	    
	default:
	    return error_tuple(env, errno);
	}
    }

    return enif_make_tuple2(env, atom_ok, 
			    enif_make_sub_binary(env, argv[1], len, data.size - len));
}

// param 0: socket
// param 1: data
// param 2: flags
// param 3: sockaddr term
static ERL_NIF_TERM
nif_sendto(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int socket;
    ErlNifBinary data;
    int flags;
    ssize_t len;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

    if (!enif_get_int(env, argv[0], &socket)
	|| !enif_inspect_binary(env, argv[1], &data)
	|| !enif_get_int(env, argv[2], &flags)
	|| !term_to_sockaddr(env, argv[3], (struct sockaddr*) &addr, &addrlen))
        return enif_make_badarg(env);

    flags |= MSG_NOSIGNAL;
    flags |= MSG_DONTWAIT;

    while (42) {
	if ((len = sendto(socket, data.data, data.size, flags, (struct sockaddr*) &addr, addrlen)) >= 0)
	    break;

	switch (errno) {
	case EINTR:
	    continue;
	    
	default:
	    return error_tuple(env, errno);
	}
    }

    return enif_make_tuple2(env, atom_ok, 
			    enif_make_sub_binary(env, argv[1], len, data.size - len));
}

// param 0: socket
// param 1: number of bytes to receive, determined automatically if negative
static ERL_NIF_TERM
nif_read(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int socket;
    ErlNifBinary buffer;
    ssize_t len = 0;

    if (!enif_get_int(env, argv[0], &socket)
	|| !enif_get_ssize(env, argv[1], &len))
        return enif_make_badarg(env);

    fionread(len);

    if (!enif_alloc_binary(len, &buffer))
        return enif_make_badarg(env);

    while (42) {
	if ((len = read(socket, buffer.data, (size_t) len)) >= 0)
	    break;

	switch (errno) {
	case EINTR:
	    continue;

	default:
	    enif_release_binary(&buffer);
	    return error_tuple(env, errno);
	}
    }

    if (len == 0) {
	enif_release_binary(&buffer);
	return atom_eof;
    }

    if (len < buffer.size)
        enif_realloc_binary(&buffer, len);

    return enif_make_tuple2(env, atom_ok, enif_make_binary(env, &buffer));
}

// param 0: socket
// param 1: data
static ERL_NIF_TERM
nif_write(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int socket;
    int len;
    ErlNifBinary data;

    if (!enif_get_int(env, argv[0], &socket)
	|| !enif_inspect_binary(env, argv[1], &data))
        return enif_make_badarg(env);

    while (42) {
	if ((len = write(socket, data.data, data.size)) >= 0)
	    break;

	switch (errno) {
	case EINTR:
	    continue;
	    
	default:
	    return error_tuple(env, errno);
	}
    }

    return enif_make_tuple2(env, atom_ok, 
			    enif_make_sub_binary(env, argv[1], len, data.size - len));
}

/* 0: (int)socket descriptor, 1: (int)device dependent request,
 * 2: (char *)argp, pointer to structure
 */
static ERL_NIF_TERM
nif_ioctl(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int socket;
    int req = 0;
    ErlNifBinary arg;

    if (!enif_get_int(env, argv[0], &socket)
	|| !enif_get_int(env, argv[1], &req)
	|| !enif_inspect_binary(env, argv[2], &arg))
        return enif_make_badarg(env);

    if (ioctl(socket, req, arg.data) < 0)
        return error_tuple(env, errno);

    return enif_make_tuple2(env, atom_ok, enif_make_binary(env, &arg));
}

/* 0: int socket descriptor, 1: int level,
 * 2: int optname, 3: void *optval
 */
static ERL_NIF_TERM
nif_setsockopt(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int s = -1;
    int level = 0;
    int name = 0;
    ErlNifBinary val;

    if (!enif_get_int(env, argv[0], &s)
	|| !enif_get_int(env, argv[1], &level)
	|| !enif_get_int(env, argv[2], &name)
	|| !enif_inspect_binary(env, argv[3], &val))
        return enif_make_badarg(env);

    if (setsockopt(s, level, name, (void *)val.data, val.size) < 0)
        return error_tuple(env, errno);

    return atom_ok;
}

/* 0: int socket descriptor, 1: int level,
 * 2: int optname
 * ret: void *optval
 */
static ERL_NIF_TERM
nif_getsockopt(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int s = -1;
    int level = 0;
    int name = 0;
    socklen_t optlen = 64;
    ErlNifBinary val;

    if (!enif_get_int(env, argv[0], &s)
	|| !enif_get_int(env, argv[1], &level)
	|| !enif_get_int(env, argv[2], &name)
	|| !enif_get_uint(env, argv[3], &optlen))
        return enif_make_badarg(env);

    if (!enif_alloc_binary(optlen, &val))
	    return atom_error;

    if (getsockopt(s, level, name, (void *)val.data, &optlen) < 0)
        return error_tuple(env, errno);

    enif_realloc_binary(&val, optlen);
    return enif_make_binary(env, &val);
}

/* 0: int socket descriptor
 * ret: posix_error()
 */
static ERL_NIF_TERM
nif_getsock_error(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int s = -1;
    int opt = 0;
    socklen_t optlen = sizeof(opt);

    if (!enif_get_int(env, argv[0], &s))
        return enif_make_badarg(env);

    if (getsockopt(s, SOL_SOCKET, SO_ERROR, &opt, &optlen) < 0)
        return error_tuple(env, errno);

    if (opt != 0)
	return enif_make_atom(env, erl_errno_id(opt));
    else
	return atom_ok;
}

/* 0: int socket descriptor
 * ret: posix_error()
 */
static ERL_NIF_TERM
nif_close(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int s = -1;

    if (!enif_get_int(env, argv[0], &s))
        return enif_make_badarg(env);

    if (close(s) < 0)
        return error_tuple(env, errno);

    return atom_ok;
}

/* 0: int socket descriptor
 * 1: int how
 * ret: posix_error()
 */
static ERL_NIF_TERM
nif_shutdown(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int s = -1;
    int how;

    if (!enif_get_int(env, argv[0], &s)
	|| !enif_get_int(env, argv[1], &how))
        return enif_make_badarg(env);

    if (shutdown(s, how) < 0)
        return error_tuple(env, errno);

    return atom_ok;
}

static ErlNifFunc nif_funcs[] = {
    {"nif_encode_sockaddr", 1, nif_encode_sockaddr},
    {"nif_decode_sockaddr", 1, nif_decode_sockaddr},
    {"nif_getsockname",     1, nif_getsockname},
    {"nif_getpeername",     1, nif_getpeername},
    {"nif_setsockopt",      4, nif_setsockopt},
    {"nif_getsockopt",      4, nif_getsockopt},
    {"nif_getsock_error",   1, nif_getsock_error},
    {"nif_accept",          1, nif_accept},
    {"nif_connect",         2, nif_connect},
    {"nif_bind",            2, nif_bind},
    {"nif_recv",            2, nif_recv},
    {"nif_recvfrom",        2, nif_recvfrom},
    {"nif_send",            3, nif_send},
    {"nif_sendto",          4, nif_sendto},
    {"nif_read",            2, nif_read},
    {"nif_write",           2, nif_write},
    {"nif_ioctl",           3, nif_ioctl},
    {"nif_socket",          3, nif_socket},
    {"nif_listen",          2, nif_listen},
    {"nif_shutdown",        2, nif_shutdown},
    {"nif_close",           1, nif_close}
};

ERL_NIF_INIT(gen_socket, nif_funcs, load, NULL, NULL, NULL)