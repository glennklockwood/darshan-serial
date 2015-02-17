/*
 *  (C) 2014 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 */

#ifndef __DARSHAN_CORE_H
#define __DARSHAN_CORE_H

#include <unistd.h>
#include <sys/types.h>
#include <stdint.h>

#include "darshan.h"

/* TODO: enforce this when handing out ids */
#define DARSHAN_CORE_MAX_RECORDS 1024

struct darshan_core_module
{
    darshan_module_id id;
    struct darshan_module_funcs mod_funcs;
};

/* in memory structure to keep up with job level data */
struct darshan_core_runtime
{
    struct darshan_job log_job;
    char exe[CP_EXE_LEN+1];
    struct darshan_core_record_ref *rec_hash;
    struct darshan_core_module* mod_array[DARSHAN_MAX_MODS];
    double wtime_offset;
    char *trailing_data;
};

struct darshan_core_record_ref
{
    struct darshan_record rec;
    UT_hash_handle hlink;
};

uint32_t darshan_hashlittle(const void *key, size_t length, uint32_t initval);
uint64_t darshan_hash(const register unsigned char *k, register uint64_t length, register uint64_t level);

#endif /* __DARSHAN_CORE_H */
