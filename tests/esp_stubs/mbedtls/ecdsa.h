#pragma once
#include <stdint.h>
#include <stddef.h>
#include "mbedtls/ecp.h"

/* Host-test stub of mbedtls/ecdsa.h — see bignum.h stub header for rationale. */

static inline int mbedtls_ecdsa_verify(const mbedtls_ecp_group *grp,
                                       const unsigned char *buf, size_t blen,
                                       const mbedtls_ecp_point *Q,
                                       const mbedtls_mpi *r, const mbedtls_mpi *s)
{ (void)grp; (void)buf; (void)blen; (void)Q; (void)r; (void)s; return -1; }
