#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Host-test stub of mbedtls/bignum.h — types compile, calls are never exercised
 * (host tests never drive req_token_ok's signature path; verify returns -1). */

typedef struct mbedtls_mpi {
    int s;
    size_t n;
    uint64_t *p;
} mbedtls_mpi;

static inline void mbedtls_mpi_init(mbedtls_mpi *x) { memset(x, 0, sizeof(*x)); }
static inline void mbedtls_mpi_free(mbedtls_mpi *x) { (void)x; }
static inline int mbedtls_mpi_read_binary(mbedtls_mpi *x, const unsigned char *buf,
                                          size_t buflen)
{ (void)x; (void)buf; (void)buflen; return -1; }
