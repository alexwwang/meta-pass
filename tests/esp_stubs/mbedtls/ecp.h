#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "mbedtls/bignum.h"

/* Host-test stub of mbedtls/ecp.h — see bignum.h stub header for rationale. */

#define MBEDTLS_ECP_DP_SECP256R1 23

typedef struct mbedtls_ecp_point {
    mbedtls_mpi X, Y, Z;
} mbedtls_ecp_point;

typedef struct mbedtls_ecp_group {
    int id;
    mbedtls_mpi P, A, B, N;
    mbedtls_ecp_point G;
    size_t pbits, nbits;
} mbedtls_ecp_group;

static inline void mbedtls_ecp_group_init(mbedtls_ecp_group *g) { memset(g, 0, sizeof(*g)); }
static inline void mbedtls_ecp_group_free(mbedtls_ecp_group *g) { (void)g; }
static inline void mbedtls_ecp_point_init(mbedtls_ecp_point *p) { memset(p, 0, sizeof(*p)); }
static inline void mbedtls_ecp_point_free(mbedtls_ecp_point *p) { (void)p; }
static inline int mbedtls_ecp_group_load(mbedtls_ecp_group *g, int id)
{ (void)g; (void)id; return -1; }
static inline int mbedtls_ecp_point_read_binary(const mbedtls_ecp_group *g,
                                                mbedtls_ecp_point *p,
                                                const unsigned char *buf, size_t ilen)
{ (void)g; (void)p; (void)buf; (void)ilen; return -1; }
