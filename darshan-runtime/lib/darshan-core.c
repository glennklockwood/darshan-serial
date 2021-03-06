/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#define _XOPEN_SOURCE 500
#define _GNU_SOURCE

#include "darshan-runtime-config.h"

#include <stdio.h>
#ifdef HAVE_MNTENT_H
#include <mntent.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/vfs.h>
#include <zlib.h>
#include <assert.h>
#ifdef __DARSHAN_TRAP_SIGNALS
#include <signal.h>
#endif

#include "uthash.h"
#include "darshan.h"
#include "darshan-core.h"
#include "darshan-dynamic.h"
#include "darshan-mpi.h"

#ifdef DARSHAN_LUSTRE
#include <lustre/lustre_user.h>
#endif

extern char* __progname;
extern char* __progname_full;

/* internal variable delcarations */
static struct darshan_core_runtime *darshan_core = NULL;
static pthread_mutex_t darshan_core_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static int my_rank = -1;
static int nprocs = -1;
static int using_mpi;
static int darshan_mem_alignment = 1;
static long darshan_mod_mem_quota = DARSHAN_MOD_MEM_MAX;

/* paths prefixed with the following directories are not tracked by darshan */
char* darshan_path_exclusions[] = {
    "/etc/",
    "/dev/",
    "/usr/",
    "/bin/",
    "/boot/",
    "/lib/",
    "/opt/",
    "/sbin/",
    "/sys/",
    "/proc/",
    "/var/",
    NULL
};
/* paths prefixed with the following directories are tracked by darshan even if
 * they share a root with a path listed in darshan_path_exclusions
 */
char* darshan_path_inclusions[] = {
    "/var/opt/cray/dws/mounts/",
    NULL
};

/* allow users to override the path exclusions */
char** user_darshan_path_exclusions = NULL;

#ifdef DARSHAN_BGQ
extern void bgq_runtime_initialize();
#endif

/* array of init functions for modules which need to be statically
 * initialized by darshan at startup time
 */
void (*mod_static_init_fns[])(void) =
{
#ifdef DARSHAN_BGQ
    &bgq_runtime_initialize,
#endif
    NULL
};

#ifdef DARSHAN_LUSTRE
/* XXX need to use extern to get Lustre module's instrumentation function
 * since modules have no way of providing this to darshan-core
 */
extern void darshan_instrument_lustre_file(const char *filepath, int fd);
#endif

#define DARSHAN_CORE_LOCK() pthread_mutex_lock(&darshan_core_mutex)
#define DARSHAN_CORE_UNLOCK() pthread_mutex_unlock(&darshan_core_mutex)

/* FS mount information */
#define DARSHAN_MAX_MNTS 64
#define DARSHAN_MAX_MNT_PATH 256
#define DARSHAN_MAX_MNT_TYPE 32
struct mnt_data
{
    char path[DARSHAN_MAX_MNT_PATH];
    char type[DARSHAN_MAX_MNT_TYPE];
    struct darshan_fs_info fs_info;
};
static struct mnt_data mnt_data_array[DARSHAN_MAX_MNTS];
static int mnt_data_count = 0;

/* prototypes for internal helper functions */
#ifdef __DARSHAN_ENABLE_MMAP_LOGS
static void *darshan_init_mmap_log(
    struct darshan_core_runtime* core, int jobid);
#endif
static void darshan_log_record_hints_and_ver(
    struct darshan_core_runtime* core);
static void darshan_get_exe_and_mounts(
    struct darshan_core_runtime *core, int argc, char **argv);
static void darshan_fs_info_from_path(
    const char *path, struct darshan_fs_info *fs_info);
static int darshan_add_name_record_ref(
    struct darshan_core_runtime *core, darshan_record_id rec_id,
    const char *name, darshan_module_id mod_id);
static void darshan_get_user_name(
    char *user);
static void darshan_get_logfile_name(
    char* logfile_name, int jobid, struct tm* start_tm);
static void darshan_get_shared_records(
    struct darshan_core_runtime *core, darshan_record_id **shared_recs,
    int *shared_rec_cnt);
#ifdef HAVE_MPI
static int darshan_log_open_all(
    char *logfile_name, struct darshan_mpi_file *log_fh);
#else
static int darshan_log_open_all(
    char *logfile_name, int *log_fh);
#endif
static int darshan_deflate_buffer(
    void **pointers, int *lengths, int count, char *comp_buf,
    int *comp_buf_length);
#ifdef HAVE_MPI
static int darshan_log_write_name_record_hash(
    struct darshan_mpi_file log_fh, struct darshan_core_runtime *core,
    uint64_t *inout_off);
static int darshan_log_append_all(
    struct darshan_mpi_file log_fh, struct darshan_core_runtime *core, void *buf,
    int count, uint64_t *inout_off);
#else
static int darshan_log_write_name_record_hash(
    int log_fh, struct darshan_core_runtime *core,
    uint64_t *inout_off);
static int darshan_log_append_all(
    int log_fh, struct darshan_core_runtime *core, void *buf,
    int count, uint64_t *inout_off);
#endif /* #ifdef HAVE_MPI */
static void darshan_core_cleanup(
    struct darshan_core_runtime* core);
static double time_nanoseconds();

/* *********************************** */

void darshan_core_initialize(int argc, char **argv)
{
    struct darshan_core_runtime *init_core = NULL;
    int internal_timing_flag = 0;
    double init_start, init_time, init_max;
    char *envstr;
    char *jobid_str;
    int jobid;
    int ret;
    int i;
    int tmpval;
    double tmpfloat;

    /* bail out _before_ attempting to [re]set using_mpi */
    if (darshan_core != NULL)
        return;

#ifdef HAVE_MPI
    PMPI_Initialized(&using_mpi);

    darshan_mpi_comm_size(MPI_COMM_WORLD, &nprocs);
    darshan_mpi_comm_rank(MPI_COMM_WORLD, &my_rank);
#else
    my_rank = 0;
    nprocs = 1;
#endif

    if(getenv("DARSHAN_INTERNAL_TIMING"))
        internal_timing_flag = 1;

    if(internal_timing_flag)
        init_start = time_nanoseconds();

    /* setup darshan runtime if darshan is enabled and hasn't been initialized already */
    if(!getenv("DARSHAN_DISABLE") && !darshan_core)
    {
        #if (__DARSHAN_MEM_ALIGNMENT < 1)
            #error Darshan must be configured with a positive value for --with-mem-align
        #endif
        envstr = getenv(DARSHAN_MEM_ALIGNMENT_OVERRIDE);
        if(envstr)
        {
            ret = sscanf(envstr, "%d", &tmpval);
            /* silently ignore if the env variable is set poorly */
            if(ret == 1 && tmpval > 0)
            {
                darshan_mem_alignment = tmpval;
            }
        }
        else
        {
            darshan_mem_alignment = __DARSHAN_MEM_ALIGNMENT;
        }

        /* avoid floating point errors on faulty input */
        if(darshan_mem_alignment < 1)
        {
            darshan_mem_alignment = 1;
        }

        /* Use DARSHAN_JOBID_OVERRIDE for the env var for __DARSHAN_JOBID */
        envstr = getenv(DARSHAN_JOBID_OVERRIDE);
        if(!envstr)
        {
            envstr = __DARSHAN_JOBID;
        }

        /* find a job id */
        jobid_str = getenv(envstr);
        if(jobid_str)
        {
            /* in cobalt we can find it in env var */
            ret = sscanf(jobid_str, "%d", &jobid);
        }
        if(!jobid_str || ret != 1)
        {
            /* use pid as fall back */
            jobid = getpid();
        }

        /* set the memory quota for darshan modules' records */
        envstr = getenv(DARSHAN_MOD_MEM_OVERRIDE);
        if(envstr)
        {
            ret = sscanf(envstr, "%lf", &tmpfloat);
            /* silently ignore if the env variable is set poorly */
            if(ret == 1 && tmpfloat > 0)
            {
                darshan_mod_mem_quota = tmpfloat * 1024 * 1024; /* convert from MiB */
            }
        }

        /* allocate structure to track darshan core runtime information */
        init_core = malloc(sizeof(*init_core));
        if(init_core)
        {
            memset(init_core, 0, sizeof(*init_core));
            init_core->wtime_offset = time_nanoseconds();

        /* TODO: do we alloc new memory as we go or just do everything up front? */

#ifndef __DARSHAN_ENABLE_MMAP_LOGS
            /* just allocate memory for each log file region */
            init_core->log_hdr_p = malloc(sizeof(struct darshan_header));
            init_core->log_job_p = malloc(sizeof(struct darshan_job));
            init_core->log_exemnt_p = malloc(DARSHAN_EXE_LEN+1);
            init_core->log_name_p = malloc(DARSHAN_NAME_RECORD_BUF_SIZE);
            init_core->log_mod_p = malloc(darshan_mod_mem_quota);

            if(!(init_core->log_hdr_p) || !(init_core->log_job_p) ||
               !(init_core->log_exemnt_p) || !(init_core->log_name_p) ||
               !(init_core->log_mod_p))
            {
                free(init_core);
                return;
            }
            /* if allocation succeeds, zero fill memory regions */
            memset(init_core->log_hdr_p, 0, sizeof(struct darshan_header));
            memset(init_core->log_job_p, 0, sizeof(struct darshan_job));
            memset(init_core->log_exemnt_p, 0, DARSHAN_EXE_LEN+1);
            memset(init_core->log_name_p, 0, DARSHAN_NAME_RECORD_BUF_SIZE);
            memset(init_core->log_mod_p, 0, darshan_mod_mem_quota);
#else
            /* if mmap logs are enabled, we need to initialize the mmap region
             * before setting the corresponding log file region pointers
             */
            void *mmap_p = darshan_init_mmap_log(init_core, jobid);
            if(!mmap_p)
            {
                free(init_core);
                return;
            }

            /* set the memory pointers for each log file region */
            init_core->log_hdr_p = (struct darshan_header *)mmap_p;
            init_core->log_job_p = (struct darshan_job *)
                ((char *)init_core->log_hdr_p + sizeof(struct darshan_header));
            init_core->log_exemnt_p = (char *)
                ((char *)init_core->log_job_p + sizeof(struct darshan_job));
            init_core->log_name_p = (void *)
                ((char *)init_core->log_exemnt_p + DARSHAN_EXE_LEN + 1);
            init_core->log_mod_p = (void *)
                ((char *)init_core->log_name_p + DARSHAN_NAME_RECORD_BUF_SIZE);

            /* set header fields needed for the mmap log mechanism */
            init_core->log_hdr_p->comp_type = DARSHAN_NO_COMP;
            init_core->log_hdr_p->name_map.off =
                ((char *)init_core->log_name_p - (char *)init_core->log_hdr_p);
#endif

            /* set known header fields for the log file */
            strcpy(init_core->log_hdr_p->version_string, DARSHAN_LOG_VERSION);
            init_core->log_hdr_p->magic_nr = DARSHAN_MAGIC_NR;

            /* set known job-level metadata fields for the log file */
            init_core->log_job_p->uid = getuid();
            init_core->log_job_p->start_time = time(NULL);
            init_core->log_job_p->nprocs = nprocs;
            init_core->log_job_p->jobid = (int64_t)jobid;

            /* if we are using any hints to write the log file, then record those
             * hints with the darshan job information
             */
            darshan_log_record_hints_and_ver(init_core);

            /* collect information about command line and mounted file systems */
            darshan_get_exe_and_mounts(init_core, argc, argv);

            /* if darshan was successfully initialized, set the global pointer
             * and bootstrap any modules with static initialization routines
             */
            DARSHAN_CORE_LOCK();
            darshan_core = init_core;
            DARSHAN_CORE_UNLOCK();

            i = 0;
            while(mod_static_init_fns[i])
            {
                (*mod_static_init_fns[i])();
                i++;
            }
        }
    }

    if(internal_timing_flag)
    {
        init_time = time_nanoseconds() - init_start;
#ifdef HAVE_MPI
        darshan_mpi_reduce(&init_time, &init_max, 1,
            MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
#else
        init_max = init_time;
#endif
        if(my_rank == 0)
        {
            darshan_core_fprintf(stderr, "#darshan:<op>\t<nprocs>\t<time>\n");
            darshan_core_fprintf(stderr, "darshan:init\t%d\t%f\n", nprocs, init_max);
        }
    }

    return;
}

void darshan_core_shutdown()
{
    struct darshan_core_runtime *final_core;
    int internal_timing_flag = 0;
    struct tm *start_tm;
    time_t start_time_tmp;
    int64_t first_start_time;
    int64_t last_end_time;
    double start_log_time;
    double tm_end;
    double open1 = 0, open2 = 0;
    double job1 = 0, job2 = 0;
    double rec1 = 0, rec2 = 0;
    double mod1[DARSHAN_MAX_MODS] = {0};
    double mod2[DARSHAN_MAX_MODS] = {0};
    double header1 = 0, header2 = 0;
    char *logfile_name;
    int local_mod_use[DARSHAN_MAX_MODS] = {0};
    int global_mod_use_count[DARSHAN_MAX_MODS] = {0};
    darshan_record_id *shared_recs;
    darshan_record_id *mod_shared_recs;
    int shared_rec_cnt = 0;
    int ret = 0;
    int all_ret = 0;
    int i;
    uint64_t gz_fp = 0;
#ifdef HAVE_MPI
    struct darshan_mpi_file log_fh;
    MPI_Status status; /* if used, darshan_mpi_file_* needs to be updated */
#else
    int log_fh;
#endif

    if(getenv("DARSHAN_INTERNAL_TIMING"))
        internal_timing_flag = 1;

    /* synchronize before getting start time */
#ifdef HAVE_MPI
    darshan_mpi_barrier(MPI_COMM_WORLD);
#endif
    start_log_time = time_nanoseconds();

    /* disable darhan-core while we shutdown */
    DARSHAN_CORE_LOCK();
    if(!darshan_core)
    {
        DARSHAN_CORE_UNLOCK();
        return;
    }
    final_core = darshan_core;
    darshan_core = NULL;
    DARSHAN_CORE_UNLOCK();

#ifdef __DARSHAN_ENABLE_MMAP_LOGS
    /* remove the temporary mmap log files */
    /* NOTE: this unlink is not immediate as it must wait for the mapping
     * to no longer be referenced, which in our case happens when the
     * executable exits. If the application terminates mid-shutdown, then
     * there will be no mmap files and no final log file.
     */
    unlink(final_core->mmap_log_name);
#endif

    final_core->log_job_p->end_time = time(NULL);

#ifdef HAVE_MPI
    /* reduce to report first start and last end time across all ranks at rank 0 */
    darshan_mpi_reduce(&final_core->log_job_p->start_time, &first_start_time,
        1, MPI_INT64_T, MPI_MIN, 0, MPI_COMM_WORLD);
    darshan_mpi_reduce(&final_core->log_job_p->end_time, &last_end_time,
        1, MPI_INT64_T, MPI_MAX, 0, MPI_COMM_WORLD);
    if(my_rank == 0)
    {
        final_core->log_job_p->start_time = first_start_time;
        final_core->log_job_p->end_time = last_end_time;
    }
#endif

    final_core->comp_buf = malloc(darshan_mod_mem_quota);
    if(!(final_core->comp_buf))
    {
        darshan_core_cleanup(final_core);
        return;
    }

    logfile_name = malloc(PATH_MAX);
    if(!logfile_name)
    {
        darshan_core_cleanup(final_core);
        return;
    }

    /* set the log file name on rank 0 */
    if(my_rank == 0)
    {
        /* use human readable start time format in log filename */
        start_time_tmp = final_core->log_job_p->start_time;
        start_tm = localtime(&start_time_tmp);

        darshan_get_logfile_name(logfile_name, final_core->log_job_p->jobid, start_tm);
    }

#ifdef HAVE_MPI
    /* broadcast log file name */
    darshan_mpi_bcast(logfile_name, PATH_MAX, MPI_CHAR, 0,
        MPI_COMM_WORLD);
#endif

    if(strlen(logfile_name) == 0)
    {
        /* failed to generate log file name */
        darshan_core_fprintf(stderr, "darshan library warning: unable to determine log file path\n");
        free(logfile_name);
        darshan_core_cleanup(final_core);
        return;
    }

    /* set which modules were registered locally, and call into
     * them to disable further instrumentation and to perform any
     * other pre-shutdown steps
     */
    for(i = 0; i < DARSHAN_MAX_MODS; i++)
    {
        if(final_core->mod_array[i])
            local_mod_use[i] = 1;
    }

#ifdef HAVE_MPI
    /* reduce the number of times a module was opened globally and bcast to everyone */
    darshan_mpi_allreduce(local_mod_use, global_mod_use_count, DARSHAN_MAX_MODS, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
#else
    memcpy(global_mod_use_count, local_mod_use, DARSHAN_MAX_MODS * sizeof(*local_mod_use));
#endif

    /* get a list of records which are shared across all processes */
    darshan_get_shared_records(final_core, &shared_recs, &shared_rec_cnt);


    if(internal_timing_flag)
        open1 = time_nanoseconds();
    /* collectively open the darshan log file */
    ret = darshan_log_open_all(logfile_name, &log_fh);
    if(internal_timing_flag)
        open2 = time_nanoseconds();

#ifdef HAVE_MPI
    /* error out if unable to open log file */
    darshan_mpi_allreduce(&ret, &all_ret, 1, MPI_INT, MPI_LOR, MPI_COMM_WORLD);
    if(all_ret != 0)
    {
        if(my_rank == 0)
        {
            darshan_core_fprintf(stderr, "darshan library warning: unable to create log file %s\n",
                logfile_name);
        }
        free(logfile_name);
        darshan_core_cleanup(final_core);
        return;
    }
#endif

    if(internal_timing_flag)
        job1 = time_nanoseconds();
    /* rank 0 is responsible for writing the compressed darshan job information */
    if(my_rank == 0)
    {
        void *pointers[2] = {final_core->log_job_p, final_core->log_exemnt_p};
        int lengths[2] = {sizeof(struct darshan_job), strlen(final_core->log_exemnt_p)};
        int comp_buf_sz = 0;
        ssize_t written;

        /* compress the job info and the trailing mount/exe data */
        all_ret = darshan_deflate_buffer(pointers, lengths, 2,
            final_core->comp_buf, &comp_buf_sz);
        if(all_ret)
        {
            darshan_core_fprintf(stderr, "darshan library warning: unable to compress job data\n");
            unlink(logfile_name);
        }
        else
        {
            /* write the job information, preallocing space for the log header */
            gz_fp += sizeof(struct darshan_header);
#ifdef HAVE_MPI
            all_ret = darshan_mpi_file_write_at(log_fh, gz_fp,
                final_core->comp_buf, comp_buf_sz, MPI_BYTE, &status);
            if(all_ret != MPI_SUCCESS)
#else
            /*ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);*/
            written = pwrite(log_fh, final_core->comp_buf, comp_buf_sz, gz_fp);
            if (written != comp_buf_sz)
#endif
            {
                darshan_core_fprintf(stderr,
                        "darshan library warning: unable to write job data to log file %s\n",
                        logfile_name);
                unlink(logfile_name);

            }
            gz_fp += comp_buf_sz;
        }
    }

#ifdef HAVE_MPI
    /* error out if unable to write job information */
    darshan_mpi_bcast(&all_ret, 1, MPI_INT, 0, MPI_COMM_WORLD);
    if(all_ret != 0)
    {
        free(logfile_name);
        darshan_core_cleanup(final_core);
        return;
    }
#endif
    if(internal_timing_flag)
        job2 = time_nanoseconds();

    if(internal_timing_flag)
        rec1 = time_nanoseconds();
    /* write the record name->id hash to the log file */
    final_core->log_hdr_p->name_map.off = gz_fp;
    ret = darshan_log_write_name_record_hash(log_fh, final_core, &gz_fp);
    final_core->log_hdr_p->name_map.len = gz_fp - final_core->log_hdr_p->name_map.off;

#ifdef HAVE_MPI
    /* error out if unable to write the name record hash */
    darshan_mpi_allreduce(&ret, &all_ret, 1, MPI_INT, MPI_LOR, MPI_COMM_WORLD);
    if(all_ret != 0)
    {
        if(my_rank == 0)
        {
            darshan_core_fprintf(stderr,
                "darshan library warning: unable to write record hash to log file %s\n",
                logfile_name);
            unlink(logfile_name);
        }
        free(logfile_name);
        darshan_core_cleanup(final_core);
        return;
    }
#endif
    if(internal_timing_flag)
        rec2 = time_nanoseconds();

    mod_shared_recs = malloc(shared_rec_cnt * sizeof(darshan_record_id));
    assert(mod_shared_recs);

    /* loop over globally used darshan modules and:
     *      - get final output buffer
     *      - compress (zlib) provided output buffer
     *      - append compressed buffer to log file
     *      - add module map info (file offset/length) to log header
     *      - shutdown the module
     */
    for(i = 0; i < DARSHAN_MAX_MODS; i++)
    {
        struct darshan_core_module* this_mod = final_core->mod_array[i];
        struct darshan_core_name_record_ref *ref = NULL;
        int mod_shared_rec_cnt = 0;
        void* mod_buf = NULL;
        int mod_buf_sz = 0;
        int j;

        if(global_mod_use_count[i] == 0)
        {
            if(my_rank == 0)
            {
                final_core->log_hdr_p->mod_map[i].off = 0;
                final_core->log_hdr_p->mod_map[i].len = 0;
            }
            continue;
        }

        if(internal_timing_flag)
            mod1[i] = time_nanoseconds();

        /* set the shared record list for this module */
        for(j = 0; j < shared_rec_cnt; j++)
        {
            HASH_FIND(hlink, final_core->name_hash, &shared_recs[j],
                sizeof(darshan_record_id), ref);
            assert(ref);
            if(DARSHAN_MOD_FLAG_ISSET(ref->global_mod_flags, i))
            {
                mod_shared_recs[mod_shared_rec_cnt++] = shared_recs[j];
            }
        }

        /* if module is registered locally, get the corresponding output buffer
         * 
         * NOTE: this function can be used to run collective operations across
         * modules, if there are records shared globally.
         */
        if(this_mod)
        {
#ifdef HAVE_MPI
            MPI_Comm mod_comm = MPI_COMM_WORLD;
#else
            int mod_comm = 0;
#endif
            mod_buf = final_core->mod_array[i]->rec_buf_start;
            mod_buf_sz = final_core->mod_array[i]->rec_buf_p - mod_buf;
            this_mod->mod_shutdown_func(&mod_comm, mod_shared_recs,
                mod_shared_rec_cnt, &mod_buf, &mod_buf_sz);
        }

        /* append this module's data to the darshan log */
        final_core->log_hdr_p->mod_map[i].off = gz_fp;
        ret = darshan_log_append_all(log_fh, final_core, mod_buf, mod_buf_sz, &gz_fp);
        final_core->log_hdr_p->mod_map[i].len =
            gz_fp - final_core->log_hdr_p->mod_map[i].off;

        /* XXX: DXT manages its own module memory buffers, so we need to
         * explicitly free them
         */
        if(i == DXT_POSIX_MOD || i == DXT_MPIIO_MOD)
            free(mod_buf);

#ifdef HAVE_MPI
        /* error out if the log append failed */
        darshan_mpi_allreduce(&ret, &all_ret, 1, MPI_INT, MPI_LOR, MPI_COMM_WORLD);
        if(all_ret != 0)
        {
            if(my_rank == 0)
            {
                darshan_core_fprintf(stderr,
                    "darshan library warning: unable to write %s module data to log file %s\n",
                    darshan_module_names[i], logfile_name);
                unlink(logfile_name);
            }
            free(logfile_name);
            darshan_core_cleanup(final_core);
            return;
        }
#endif

        if(internal_timing_flag)
            mod2[i] = time_nanoseconds();
    }

    if(internal_timing_flag)
        header1 = time_nanoseconds();
    /* write out log header, after running 2 reductions on header variables:
     *  1) reduce 'partial_flag' variable to determine which modules ran out
     *     of memory for storing data
     *  2) reduce 'mod_ver' array to determine which log format version each
     *     module used for this output log
     */
    if(my_rank == 0)
    {
        ssize_t written;
        /* rank 0 is responsible for writing the log header */
        final_core->log_hdr_p->comp_type = DARSHAN_ZLIB_COMP;

#ifdef HAVE_MPI
        darshan_mpi_reduce(
            MPI_IN_PLACE, &(final_core->log_hdr_p->partial_flag),
            1, MPI_UINT32_T, MPI_BOR, 0, MPI_COMM_WORLD);
        darshan_mpi_reduce(
            MPI_IN_PLACE, &(final_core->log_hdr_p->mod_ver),
            DARSHAN_MAX_MODS, MPI_UINT32_T, MPI_MAX, 0, MPI_COMM_WORLD);


        all_ret = darshan_mpi_file_write_at(log_fh, 0, final_core->log_hdr_p,
            sizeof(struct darshan_header), MPI_BYTE, &status);
        if(all_ret != MPI_SUCCESS)
#else
        written = pwrite(log_fh, final_core->log_hdr_p, sizeof(struct darshan_header), 0);

        if (written != sizeof(struct darshan_header))
#endif
        {
            darshan_core_fprintf(stderr, "darshan library warning: unable to write header to log file %s\n",
                    logfile_name);
            unlink(logfile_name);
        }
    }
#ifdef HAVE_MPI
    else
    {
        darshan_mpi_reduce(
            &(final_core->log_hdr_p->partial_flag), &(final_core->log_hdr_p->partial_flag),
            1, MPI_UINT32_T, MPI_BOR, 0, MPI_COMM_WORLD);
        darshan_mpi_reduce(
            &(final_core->log_hdr_p->mod_ver), &(final_core->log_hdr_p->mod_ver),
            DARSHAN_MAX_MODS, MPI_UINT32_T, MPI_MAX, 0, MPI_COMM_WORLD);
    }

    /* error out if unable to write log header */
    darshan_mpi_bcast(&all_ret, 1, MPI_INT, 0, MPI_COMM_WORLD);
    if(all_ret != 0)
    {
        free(logfile_name);
        darshan_core_cleanup(final_core);
        return;
    }
#endif
    if(internal_timing_flag)
        header2 = time_nanoseconds();

#ifdef HAVE_MPI
    darshan_mpi_file_close(&log_fh);
#else
    close(log_fh);
#endif

    /* if we got this far, there are no errors, so rename from *.darshan_partial
     * to *-<logwritetime>.darshan, which indicates that this log file is
     * complete and ready for analysis
     */
    if(my_rank == 0)
    {
        mode_t chmod_mode = S_IRUSR;
#ifdef __DARSHAN_GROUP_READABLE_LOGS
        chmod_mode |= S_IRGRP;
#endif

        if(getenv("DARSHAN_LOGFILE"))
        {
            chmod(logfile_name, chmod_mode);
        }
        else
        {
            char* tmp_index;
            double end_log_time;
            char* new_logfile_name;

            new_logfile_name = malloc(PATH_MAX);
            if(new_logfile_name)
            {
                new_logfile_name[0] = '\0';
                end_log_time = time_nanoseconds();
                strcat(new_logfile_name, logfile_name);
                tmp_index = strstr(new_logfile_name, ".darshan_partial");
                sprintf(tmp_index, "_%d.darshan", (int)(end_log_time-start_log_time+1));
                rename(logfile_name, new_logfile_name);
                /* set permissions on log file */
                chmod(new_logfile_name, chmod_mode);
                free(new_logfile_name);
            }
        }
    }

    free(logfile_name);
    free(shared_recs);
    free(mod_shared_recs);
    darshan_core_cleanup(final_core);

    if(internal_timing_flag)
    {
        double open_tm, open_slowest;
        double header_tm, header_slowest;
        double job_tm, job_slowest;
        double rec_tm, rec_slowest;
        double mod_tm[DARSHAN_MAX_MODS], mod_slowest[DARSHAN_MAX_MODS];
        double all_tm, all_slowest;

        tm_end = time_nanoseconds();

        open_tm = open2 - open1;
        header_tm = header2 - header1;
        job_tm = job2 - job1;
        rec_tm = rec2 - rec1;
        all_tm = tm_end - start_log_time;
        for(i = 0;i < DARSHAN_MAX_MODS; i++)
        {
            mod_tm[i] = mod2[i] - mod1[i];
        }

#ifdef HAVE_MPI
        darshan_mpi_reduce(&open_tm, &open_slowest, 1,
            MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
        darshan_mpi_reduce(&header_tm, &header_slowest, 1,
            MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
        darshan_mpi_reduce(&job_tm, &job_slowest, 1,
            MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
        darshan_mpi_reduce(&rec_tm, &rec_slowest, 1,
            MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
        darshan_mpi_reduce(&all_tm, &all_slowest, 1,
            MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
        darshan_mpi_reduce(mod_tm, mod_slowest, DARSHAN_MAX_MODS,
            MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
#else
        open_slowest = open_tm;
        header_slowest = header_tm;
        job_slowest = job_tm;
        rec_slowest = rec_tm;
        all_slowest = all_tm;
        memcpy(mod_slowest, mod_tm, sizeof(*mod_tm) * DARSHAN_MAX_MODS);
#endif

        if(my_rank == 0)
        {
            darshan_core_fprintf(stderr, "#darshan:<op>\t<nprocs>\t<time>\n");
            darshan_core_fprintf(stderr, "darshan:log_open\t%d\t%f\n", nprocs, open_slowest);
            darshan_core_fprintf(stderr, "darshan:job_write\t%d\t%f\n", nprocs, job_slowest);
            darshan_core_fprintf(stderr, "darshan:hash_write\t%d\t%f\n", nprocs, rec_slowest);
            darshan_core_fprintf(stderr, "darshan:header_write\t%d\t%f\n", nprocs, header_slowest);
            for(i = 0; i < DARSHAN_MAX_MODS; i++)
            {
                if(global_mod_use_count[i])
                    darshan_core_fprintf(stderr, "darshan:%s_shutdown\t%d\t%f\n", darshan_module_names[i],
                        nprocs, mod_slowest[i]);
            }
            darshan_core_fprintf(stderr, "darshan:core_shutdown\t%d\t%f\n", nprocs, all_slowest);
        }
    }

    return;
}

/* *********************************** */

#ifdef __DARSHAN_ENABLE_MMAP_LOGS
static void *darshan_init_mmap_log(struct darshan_core_runtime* core, int jobid)
{
    int ret;
    int mmap_fd;
    int mmap_size;
    int sys_page_size;
    char cuser[L_cuserid] = {0};
    uint64_t hlevel;
    char hname[HOST_NAME_MAX];
    uint64_t logmod;
    char *envstr;
    char *mmap_log_path;
    void *mmap_p;

    sys_page_size = sysconf(_SC_PAGESIZE);
    assert(sys_page_size > 0);

    mmap_size = sizeof(struct darshan_header) + DARSHAN_JOB_RECORD_SIZE +
        + DARSHAN_NAME_RECORD_BUF_SIZE + darshan_mod_mem_quota;
    if(mmap_size % sys_page_size)
        mmap_size = ((mmap_size / sys_page_size) + 1) * sys_page_size;

    envstr = getenv(DARSHAN_MMAP_LOG_PATH_OVERRIDE);
    if(envstr)
        mmap_log_path = envstr;
    else
        mmap_log_path = DARSHAN_DEF_MMAP_LOG_PATH;

    darshan_get_user_name(cuser);

    /* generate a random number to help differentiate the temporary log */
    /* NOTE: job id is not sufficient for constructing a unique log file name,
     * since a job could be composed of multiple application runs, so we also
     * add a random number component to the log name
     */
    if(my_rank == 0)
    {
        hlevel = time_nanoseconds() * 1000000;
        (void)gethostname(hname, sizeof(hname));
        logmod = darshan_hash((void*)hname,strlen(hname),hlevel);
    }
#ifdef HAVE_MPI
    darshan_mpi_bcast(&logmod, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
#endif

    /* construct a unique temporary log file name for this process
     * to write mmap log data to
     */
    snprintf(core->mmap_log_name, PATH_MAX,
        "/%s/%s_%s_id%d_mmap-log-%" PRIu64 "-%d.darshan",
        mmap_log_path, cuser, __progname, jobid, logmod, my_rank);

    /* create the temporary mmapped darshan log */
    mmap_fd = open(core->mmap_log_name, O_CREAT|O_RDWR|O_EXCL , 0644);
    if(mmap_fd < 0)
    {
        darshan_core_fprintf(stderr, "darshan library warning: "
            "unable to create darshan log file %s\n", core->mmap_log_name);
        return(NULL);
    }

    /* TODO: ftruncate or just zero fill? */
    /* allocate the necessary space in the log file */
    ret = ftruncate(mmap_fd, mmap_size);
    if(ret < 0)
    {
        darshan_core_fprintf(stderr, "darshan library warning: "
            "unable to allocate darshan log file %s\n", core->mmap_log_name);
        close(mmap_fd);
        unlink(core->mmap_log_name);
        return(NULL);
    }

    /* create the memory map for darshan's data structures so they are
     * persisted to file as the application executes
     */
    mmap_p = mmap(NULL, mmap_size, PROT_WRITE, MAP_SHARED, mmap_fd, 0);
    if(mmap_p == MAP_FAILED)
    {
        darshan_core_fprintf(stderr, "darshan library warning: "
            "unable to mmap darshan log file %s\n", core->mmap_log_name);
        close(mmap_fd);
        unlink(core->mmap_log_name);
        return(NULL);
    }

    /* close darshan log file (this does *not* unmap the log file) */
    close(mmap_fd);

    return(mmap_p);
}
#endif

/* record any hints used to write the darshan log in the job data */
static void darshan_log_record_hints_and_ver(struct darshan_core_runtime* core)
{
    char* hints;
    char* job_hints;
    int meta_remain = 0;
    char* m;

    /* check environment variable to see if the default MPI file hints have
     * been overridden
     */
    hints = getenv(DARSHAN_LOG_HINTS_OVERRIDE);
    if(!hints)
    {
        hints = __DARSHAN_LOG_HINTS;
    }

    if(!hints || strlen(hints) < 1)
        return;

    job_hints = strdup(hints);
    if(!job_hints)
        return;

    meta_remain = DARSHAN_JOB_METADATA_LEN -
        strlen(core->log_job_p->metadata) - 1;
    if(meta_remain >= (strlen(PACKAGE_VERSION) + 9))
    {
        sprintf(core->log_job_p->metadata, "lib_ver=%s\n", PACKAGE_VERSION);
        meta_remain -= (strlen(PACKAGE_VERSION) + 9);
    }
    if(meta_remain >= (3 + strlen(job_hints)))
    {
        m = core->log_job_p->metadata + strlen(core->log_job_p->metadata);
        /* We have room to store the hints in the metadata portion of
         * the job structure.  We just prepend an h= to the hints list.  The
         * metadata parser will ignore = characters that appear in the value
         * portion of the metadata key/value pair.
         */
        sprintf(m, "h=%s\n", job_hints);
    }
    free(job_hints);

    return;
}

static int mnt_data_cmp(const void* a, const void* b)
{
    const struct mnt_data *d_a = (const struct mnt_data*)a;
    const struct mnt_data *d_b = (const struct mnt_data*)b;

    if(strlen(d_a->path) > strlen(d_b->path))
        return(-1);
    else if(strlen(d_a->path) < strlen(d_b->path))
        return(1);
    else
        return(0);
}

/* adds an entry to table of mounted file systems */
static void add_entry(char* buf, int* space_left, struct mntent* entry)
{
    int i;
    int ret;
    char tmp_mnt[256];
    struct statfs statfsbuf;

    /* avoid adding the same mount points multiple times -- to limit
     * storage space and potential statfs, ioctl, etc calls
     */
    for(i = 0; i < mnt_data_count; i++)
    {
        if((strncmp(mnt_data_array[i].path, entry->mnt_dir, DARSHAN_MAX_MNT_PATH) == 0) &&
           (strncmp(mnt_data_array[i].type, entry->mnt_type, DARSHAN_MAX_MNT_PATH) == 0))
            return;
    }

    strncpy(mnt_data_array[mnt_data_count].path, entry->mnt_dir,
        DARSHAN_MAX_MNT_PATH-1);
    strncpy(mnt_data_array[mnt_data_count].type, entry->mnt_type,
        DARSHAN_MAX_MNT_TYPE-1);
    /* NOTE: we now try to detect the preferred block size for each file 
     * system using fstatfs().  On Lustre we assume a size of 1 MiB 
     * because fstatfs() reports 4 KiB. 
     */
#ifndef LL_SUPER_MAGIC
#define LL_SUPER_MAGIC 0x0BD00BD0
#endif
    ret = statfs(entry->mnt_dir, &statfsbuf);
    mnt_data_array[mnt_data_count].fs_info.fs_type = statfsbuf.f_type;
    if(ret == 0 && statfsbuf.f_type != LL_SUPER_MAGIC)
        mnt_data_array[mnt_data_count].fs_info.block_size = statfsbuf.f_bsize;
    else if(ret == 0 && statfsbuf.f_type == LL_SUPER_MAGIC)
        mnt_data_array[mnt_data_count].fs_info.block_size = 1024*1024;
    else
        mnt_data_array[mnt_data_count].fs_info.block_size = 4096;

#ifdef DARSHAN_LUSTRE
    /* attempt to retrieve OST and MDS counts from Lustre */
    mnt_data_array[mnt_data_count].fs_info.ost_count = -1;
    mnt_data_array[mnt_data_count].fs_info.mdt_count = -1;
    if ( statfsbuf.f_type == LL_SUPER_MAGIC )
    {
        int n_ost, n_mdt;
        int ret_ost, ret_mdt;
        DIR *mount_dir;

        mount_dir = opendir( entry->mnt_dir );
        if ( mount_dir  ) 
        {
            /* n_ost and n_mdt are used for both input and output to ioctl */
            n_ost = 0;
            n_mdt = 1;

            ret_ost = ioctl( dirfd(mount_dir), LL_IOC_GETOBDCOUNT, &n_ost );
            ret_mdt = ioctl( dirfd(mount_dir), LL_IOC_GETOBDCOUNT, &n_mdt );

            if ( !(ret_ost < 0 || ret_mdt < 0) )
            {
                mnt_data_array[mnt_data_count].fs_info.ost_count = n_ost;
                mnt_data_array[mnt_data_count].fs_info.mdt_count = n_mdt;
            }
            closedir( mount_dir );
        }
    }
#endif

    /* store mount information with the job-level metadata in darshan log */
    ret = snprintf(tmp_mnt, 256, "\n%s\t%s",
        entry->mnt_type, entry->mnt_dir);
    if(ret < 256 && strlen(tmp_mnt) <= (*space_left))
    {
        strcat(buf, tmp_mnt);
        (*space_left) -= strlen(tmp_mnt);
    }

    mnt_data_count++;
    return;
}

/* darshan_get_exe_and_mounts()
 *
 * collects command line and list of mounted file systems into a string that
 * will be stored with the job-level metadata
 */
static void darshan_get_exe_and_mounts(struct darshan_core_runtime *core,
    int argc, char **argv)
{
    FILE* tab;
    struct mntent *entry;
    char* exclude;
    char* truncate_string = "<TRUNCATED>";
    int truncate_offset;
    int space_left = DARSHAN_EXE_LEN;
    FILE *fh;
    int i, ii;
    char cmdl[DARSHAN_EXE_LEN];
    int tmp_index = 0;
    int skip = 0;
    char* env_exclusions;
    char* string;
    char* token;

    /* skip these fs types */
    static char* fs_exclusions[] = {
        "tmpfs",
        "proc",
        "sysfs",
        "devpts",
        "binfmt_misc",
        "fusectl",
        "debugfs",
        "securityfs",
        "nfsd",
        "none",
        "rpc_pipefs",
        "hugetlbfs",
        "cgroup",
        NULL
    };

    /* Check if user has set the env variable DARSHAN_EXCLUDE_DIRS */
    env_exclusions = getenv("DARSHAN_EXCLUDE_DIRS");
    if(env_exclusions)
    {
        fs_exclusions[0]=NULL;
        /* if DARSHAN_EXCLUDE_DIRS=none, do not exclude any dir */
        if(strncmp(env_exclusions,"none",strlen(env_exclusions))>=0)
        {
            if (my_rank == 0) 
                darshan_core_fprintf(stderr, "Darshan info: no system dirs will be excluded\n");
            darshan_path_exclusions[0]=NULL;
        }
        else
        {
            if (my_rank == 0) 
                darshan_core_fprintf(stderr, "Darshan info: the following system dirs will be excluded: %s\n",
                    env_exclusions);
            string = strdup(env_exclusions);
            i = 0;
            /* get the comma separated number of directories */
            token = strtok(string, ",");
            while (token != NULL)
            {
                i++;
                token = strtok(NULL, ",");
            }
            user_darshan_path_exclusions=(char **)malloc((i+1)*sizeof(char *));
            assert(user_darshan_path_exclusions);

            i = 0;
            strcpy(string, env_exclusions);
            token = strtok(string, ",");
            while (token != NULL)
            {
                user_darshan_path_exclusions[i]=(char *)malloc(strlen(token)+1);
                assert(user_darshan_path_exclusions[i]);
                strcpy(user_darshan_path_exclusions[i],token);
                i++;
                token = strtok(NULL, ",");
            }
            user_darshan_path_exclusions[i]=NULL;
            free(string);
        }
    }

    /* record exe and arguments */
    for(i=0; i<argc; i++)
    {
        strncat(core->log_exemnt_p, argv[i], space_left);
        space_left = DARSHAN_EXE_LEN-strlen(core->log_exemnt_p);
        if(i < (argc-1))
        {
            strncat(core->log_exemnt_p, " ", space_left);
            space_left = DARSHAN_EXE_LEN-strlen(core->log_exemnt_p);
        }
    }

    /* if we don't see any arguments, then use glibc symbol to get
     * program name at least (this happens in fortran)
     */
    if(argc == 0)
    {
        /* get the name of the executable and the arguments from 
           /proc/self/cmdline */

        cmdl[0] = '\0';
        fh = fopen("/proc/self/cmdline","r");
        if(fh) {
            ii = 0;
            fgets(cmdl,DARSHAN_EXE_LEN,fh);
            for(i=1;i<DARSHAN_EXE_LEN;i++)  {
                if(cmdl[i]==0 && ii == 0) {
                  cmdl[i]=' '; ii = 1;
                } else if(cmdl[i]==0 && ii == 1) {
                  break;
                } else {
                  ii = 0;
                }
            }
            fclose(fh);
        } else {
           sprintf(cmdl, "%s <unknown args>", __progname_full);
        }
        strncat(core->log_exemnt_p, cmdl, space_left);
        space_left = DARSHAN_EXE_LEN-strlen(core->log_exemnt_p);
    }

    if(space_left == 0)
    {
        /* we ran out of room; mark that string was truncated */
        truncate_offset = DARSHAN_EXE_LEN - strlen(truncate_string);
        sprintf(&(core->log_exemnt_p[truncate_offset]), "%s",
            truncate_string);
    }

    /* we make two passes through mounted file systems; in the first pass we
     * grab any non-nfs mount points, then on the second pass we grab nfs
     * mount points
     */
    mnt_data_count = 0;

    tab = setmntent("/etc/mtab", "r");
    if(!tab)
        return;
    /* loop through list of mounted file systems */
    while(mnt_data_count<DARSHAN_MAX_MNTS && (entry = getmntent(tab)) != NULL)
    {
        /* filter out excluded fs types */
        tmp_index = 0;
        skip = 0;
        while((exclude = fs_exclusions[tmp_index]))
        {
            if(!(strcmp(exclude, entry->mnt_type)))
            {
                skip =1;
                break;
            }
            tmp_index++;
        }

        if(skip || (strcmp(entry->mnt_type, "nfs") == 0))
            continue;

        add_entry(core->log_exemnt_p, &space_left, entry);
    }
    endmntent(tab);

    tab = setmntent("/etc/mtab", "r");
    if(!tab)
        return;
    /* loop through list of mounted file systems */
    while(mnt_data_count<DARSHAN_MAX_MNTS && (entry = getmntent(tab)) != NULL)
    {
        if(strcmp(entry->mnt_type, "nfs") != 0)
            continue;

        add_entry(core->log_exemnt_p, &space_left, entry);
    }
    endmntent(tab);

    /* sort mount points in order of longest path to shortest path.  This is
     * necessary so that if we try to match file paths to mount points later
     * we don't match on "/" every time.
     */
    qsort(mnt_data_array, mnt_data_count, sizeof(mnt_data_array[0]), mnt_data_cmp);
    return;
}

static void darshan_fs_info_from_path(const char *path, struct darshan_fs_info *fs_info)
{
    int i;
    fs_info->fs_type = -1;
    fs_info->block_size = -1;

    for(i=0; i<mnt_data_count; i++)
    {
        if(!(strncmp(mnt_data_array[i].path, path, strlen(mnt_data_array[i].path))))
        {
            *fs_info = mnt_data_array[i].fs_info;
            return;
        }
    }

    return;
}

static int darshan_add_name_record_ref(struct darshan_core_runtime *core,
    darshan_record_id rec_id, const char *name, darshan_module_id mod_id)
{
    struct darshan_core_name_record_ref *ref;
    int record_size = sizeof(darshan_record_id) + strlen(name) + 1;

    if((record_size + core->name_mem_used) > DARSHAN_NAME_RECORD_BUF_SIZE)
        return(0);

    ref = malloc(sizeof(*ref));
    if(!ref)
        return(0);
    memset(ref, 0, sizeof(*ref));

    /* initialize the name record */
    ref->name_record = (struct darshan_name_record *)
        ((char *)core->log_name_p + core->name_mem_used);
    memset(ref->name_record, 0, record_size);
    ref->name_record->id = rec_id;
    strcpy(ref->name_record->name, name);
    DARSHAN_MOD_FLAG_SET(ref->mod_flags, mod_id);

    /* add the record to the hash table */
    HASH_ADD(hlink, core->name_hash, name_record->id,
        sizeof(darshan_record_id), ref);
    core->name_mem_used += record_size;
#ifdef __DARSHAN_ENABLE_MMAP_LOGS
    core->log_hdr_p->name_map.len += record_size;
#endif

    return(1);
}

static void darshan_get_user_name(char *cuser)
{
    char* logname_string;

    /* get the username for this job.  In order we will try each of the
     * following until one of them succeeds:
     *
     * - cuserid()
     * - getenv("LOGNAME")
     * - snprintf(..., geteuid());
     *
     * Note that we do not use getpwuid() because it generally will not
     * work in statically compiled binaries.
     */

#ifndef __DARSHAN_DISABLE_CUSERID
    cuserid(cuser);
#endif

    /* if cuserid() didn't work, then check the environment */
    if(strcmp(cuser, "") == 0)
    {
        logname_string = getenv("LOGNAME");
        if(logname_string)
        {
            strncpy(cuser, logname_string, (L_cuserid-1));
        }
    }

    /* if cuserid() and environment both fail, then fall back to uid */
    if(strcmp(cuser, "") == 0)
    {
        uid_t uid = geteuid();
        snprintf(cuser, L_cuserid, "%u", uid);
    }

    return;
}

/* construct the darshan log file name */
static void darshan_get_logfile_name(char* logfile_name, int jobid, struct tm* start_tm)
{
    char* user_logfile_name;
    char* logpath;
    char* logpath_override = NULL;
#ifdef __DARSHAN_LOG_ENV
    char env_check[256];
    char* env_tok;
#endif
    uint64_t hlevel;
    char hname[HOST_NAME_MAX];
    uint64_t logmod;
    char cuser[L_cuserid] = {0};
    int ret;

    /* first, check if user specifies a complete logpath to use */
    user_logfile_name = getenv("DARSHAN_LOGFILE");
    if(user_logfile_name)
    {
        if(strlen(user_logfile_name) >= (PATH_MAX-1))
        {
            darshan_core_fprintf(stderr, "darshan library warning: user log file name too long.\n");
            logfile_name[0] = '\0';
        }
        else
        {
            strcpy(logfile_name, user_logfile_name);
        }
    }
    else
    {
        /* otherwise, generate the log path automatically */

        /* Use DARSHAN_LOG_PATH_OVERRIDE for the value or __DARSHAN_LOG_PATH */
        logpath = getenv(DARSHAN_LOG_PATH_OVERRIDE);
        if(!logpath)
        {
#ifdef __DARSHAN_LOG_PATH
            logpath = __DARSHAN_LOG_PATH;
#endif
        }

        darshan_get_user_name(cuser);

        /* generate a random number to help differentiate the log */
        hlevel = time_nanoseconds() * 1000000;
        (void)gethostname(hname, sizeof(hname));
        logmod = darshan_hash((void*)hname,strlen(hname),hlevel);

        /* see if darshan was configured using the --with-logpath-by-env
         * argument, which allows the user to specify an absolute path to
         * place logs via an env variable.
         */
#ifdef __DARSHAN_LOG_ENV
        /* just silently skip if the environment variable list is too big */
        if(strlen(__DARSHAN_LOG_ENV) < 256)
        {
            /* copy env variable list to a temporary buffer */
            strcpy(env_check, __DARSHAN_LOG_ENV);
            /* tokenize the comma-separated list */
            env_tok = strtok(env_check, ",");
            if(env_tok)
            {
                do
                {
                    /* check each env variable in order */
                    logpath_override = getenv(env_tok);
                    if(logpath_override)
                    {
                        /* stop as soon as we find a match */
                        break;
                    }
                }while((env_tok = strtok(NULL, ",")));
            }
        }
#endif

        if(logpath_override)
        {
            ret = snprintf(logfile_name, PATH_MAX,
                "%s/%s_%s_id%d_%d-%d-%d-%" PRIu64 ".darshan_partial",
                logpath_override,
                cuser, __progname, jobid,
                (start_tm->tm_mon+1),
                start_tm->tm_mday,
                (start_tm->tm_hour*60*60 + start_tm->tm_min*60 + start_tm->tm_sec),
                logmod);
            if(ret == (PATH_MAX-1))
            {
                /* file name was too big; squish it down */
                snprintf(logfile_name, PATH_MAX,
                    "%s/id%d.darshan_partial",
                    logpath_override, jobid);
            }
        }
        else if(logpath)
        {
            ret = snprintf(logfile_name, PATH_MAX,
                "%s/%d/%d/%d/%s_%s_id%d_%d-%d-%d-%" PRIu64 ".darshan_partial",
                logpath, (start_tm->tm_year+1900),
                (start_tm->tm_mon+1), start_tm->tm_mday,
                cuser, __progname, jobid,
                (start_tm->tm_mon+1),
                start_tm->tm_mday,
                (start_tm->tm_hour*60*60 + start_tm->tm_min*60 + start_tm->tm_sec),
                logmod);
            if(ret == (PATH_MAX-1))
            {
                /* file name was too big; squish it down */
                snprintf(logfile_name, PATH_MAX,
                    "%s/id%d.darshan_partial",
                    logpath, jobid);
            }
        }
        else
        {
            logfile_name[0] = '\0';
        }
    }

    return;
}

static void darshan_get_shared_records(struct darshan_core_runtime *core,
    darshan_record_id **shared_recs, int *shared_rec_cnt)
{
#ifdef HAVE_MPI
    int i, j;
    int tmp_cnt = HASH_CNT(hlink, core->name_hash);
    struct darshan_core_name_record_ref *tmp, *ref;
    darshan_record_id *id_array;
    uint64_t *mod_flags;
    uint64_t *global_mod_flags;

    /* broadcast root's number of records to all other processes */
    darshan_mpi_bcast(&tmp_cnt, 1, MPI_INT, 0, MPI_COMM_WORLD);

    /* use root record count to allocate data structures */
    id_array = malloc(tmp_cnt * sizeof(darshan_record_id));
    mod_flags = malloc(tmp_cnt * sizeof(uint64_t));
    global_mod_flags = malloc(tmp_cnt * sizeof(uint64_t));
    *shared_recs = malloc(tmp_cnt * sizeof(darshan_record_id));
    assert(id_array && mod_flags && global_mod_flags && *shared_recs);

    memset(mod_flags, 0, tmp_cnt * sizeof(uint64_t));
    memset(global_mod_flags, 0, tmp_cnt * sizeof(uint64_t));
    memset(*shared_recs, 0, tmp_cnt * sizeof(darshan_record_id));

    /* first, determine list of records root process has opened */
    if(my_rank == 0)
    {
        i = 0;
        HASH_ITER(hlink, core->name_hash, ref, tmp)
        {
            id_array[i++] = ref->name_record->id;
        }
    }

    /* broadcast root's list of records to all other processes */
    darshan_mpi_bcast(id_array, (tmp_cnt * sizeof(darshan_record_id)),
        MPI_BYTE, 0, MPI_COMM_WORLD);

    /* everyone looks to see if they opened the same records as root */
    for(i=0; i<tmp_cnt; i++)
    {
        HASH_FIND(hlink, core->name_hash, &id_array[i], sizeof(darshan_record_id), ref);
        if(ref)
        {
            /* we opened that record too, save the mod_flags */
            mod_flags[i] = ref->mod_flags;
        }
    }

    /* now allreduce so everyone agrees which records are shared and
     * which modules accessed them collectively
     */
    darshan_mpi_allreduce(mod_flags, global_mod_flags, tmp_cnt, MPI_UINT64_T, MPI_BAND, MPI_COMM_WORLD);

    j = 0;
    for(i=0; i<tmp_cnt; i++)
    {
        if(global_mod_flags[i] != 0)
        {
            (*shared_recs)[j++] = id_array[i];

            /* set global_mod_flags so we know which modules collectively
             * accessed this module. we need this info to support shared
             * record reductions
             */
            HASH_FIND(hlink, core->name_hash, &id_array[i], sizeof(darshan_record_id), ref);
            assert(ref);
            ref->global_mod_flags = global_mod_flags[i];
        }
    }
    *shared_rec_cnt = j;

    free(id_array);
    free(mod_flags);
    free(global_mod_flags);
#else
    int j;
    struct darshan_core_name_record_ref *tmp, *ref;

    /* allocate the thing we're returning */
    *shared_recs = malloc(HASH_CNT(hlink, core->name_hash) * sizeof(darshan_record_id));
    assert(*shared_recs);

    /* do a gather of all record ids */
    j = 0;
    HASH_ITER(hlink, core->name_hash, ref, tmp)
    {
        (*shared_recs)[j++] = ref->name_record->id;
    }
    *shared_rec_cnt = j;

#endif /* #ifdef HAVE_MPI */
    return;
}

#ifdef HAVE_MPI
static int darshan_log_open_all(char *logfile_name, struct darshan_mpi_file *log_fh)
{
    char *hints;
    char *tok_str;
    char *orig_tok_str;
    char *key;
    char *value;
    char *saveptr = NULL;
    int ret;
    MPI_Info info;

    /* check environment variable to see if the default MPI file hints have
     * been overridden
     */
    darshan_mpi_info_create(&info);

    hints = getenv(DARSHAN_LOG_HINTS_OVERRIDE);
    if(!hints)
    {
        hints = __DARSHAN_LOG_HINTS;
    }

    if(hints && strlen(hints) > 0)
    {
        tok_str = strdup(hints);
        if(tok_str)
        {
            orig_tok_str = tok_str;
            do
            {
                /* split string on semicolon */
                key = strtok_r(tok_str, ";", &saveptr);
                if(key)
                {
                    tok_str = NULL;
                    /* look for = sign splitting key/value pairs */
                    value = index(key, '=');
                    if(value)
                    {
                        /* break key and value into separate null terminated strings */
                        value[0] = '\0';
                        value++;
                        if(strlen(key) > 0)
                            darshan_mpi_info_set(info, key, value);
                    }
                }
            }while(key != NULL);
            free(orig_tok_str);
        }
    }

    /* open the darshan log file for writing */
    ret = darshan_mpi_file_open(MPI_COMM_WORLD, logfile_name,
        MPI_MODE_CREATE | MPI_MODE_WRONLY | MPI_MODE_EXCL, info, log_fh);
    if(ret != MPI_SUCCESS)
        return(-1);

    darshan_mpi_info_free(&info);
    return(0);
}
#else
static int darshan_log_open_all(char *logfile_name, int *log_fh)
{
    /* open the darshan log file for writing */
    *log_fh = open(logfile_name, O_CREAT|O_WRONLY|O_EXCL, S_IRWXU|S_IRWXG|S_IRWXO);

    return(0);
}
#endif

static int darshan_deflate_buffer(void **pointers, int *lengths, int count,
    char *comp_buf, int *comp_buf_length)
{
    int ret = 0;
    int i;
    int total_target = 0;
    z_stream tmp_stream;

    /* just return if there is no data */
    for(i = 0; i < count; i++)
    {
        total_target += lengths[i];
    }
    if(total_target)
    {
        total_target = 0;
    }
    else
    {
        *comp_buf_length = 0;
        return(0);
    }

    memset(&tmp_stream, 0, sizeof(tmp_stream));
    tmp_stream.zalloc = Z_NULL;
    tmp_stream.zfree = Z_NULL;
    tmp_stream.opaque = Z_NULL;

    /* initialize the zlib compression parameters */
    /* TODO: check these parameters? */
//    ret = deflateInit2(&tmp_stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
//        15 + 16, 8, Z_DEFAULT_STRATEGY);
    ret = deflateInit(&tmp_stream, Z_DEFAULT_COMPRESSION);
    if(ret != Z_OK)
    {
        return(-1);
    }

    tmp_stream.next_out = (unsigned char *)comp_buf;
    tmp_stream.avail_out = darshan_mod_mem_quota;

    /* loop over the input pointers */
    for(i = 0; i < count; i++)
    {
        total_target += lengths[i];
        tmp_stream.next_in = pointers[i];
        tmp_stream.avail_in = lengths[i];
        /* while we have not finished consuming all of the data available to
         * this point in the loop
         */
        while(tmp_stream.total_in < total_target)
        {
            if(tmp_stream.avail_out == 0)
            {
                /* We ran out of buffer space for compression.  In theory,
                 * we could start using some of the file_array buffer space
                 * without having to malloc again.  In practice, this case 
                 * is going to be practically impossible to hit.
                 */
                deflateEnd(&tmp_stream);
                return(-1);
            }

            /* compress data */
            ret = deflate(&tmp_stream, Z_NO_FLUSH);
            if(ret != Z_OK)
            {
                deflateEnd(&tmp_stream);
                return(-1);
            }
        }
    }

    /* flush compression and end */
    ret = deflate(&tmp_stream, Z_FINISH);
    if(ret != Z_STREAM_END)
    {
        deflateEnd(&tmp_stream);
        return(-1);
    }
    deflateEnd(&tmp_stream);

    *comp_buf_length = tmp_stream.total_out;
    return(0);
}

/* NOTE: the map written to file may contain duplicate id->name entries if a
 *       record is opened by multiple ranks, but not all ranks
 */
#ifdef HAVE_MPI
static int darshan_log_write_name_record_hash(struct darshan_mpi_file log_fh,
    struct darshan_core_runtime *core, uint64_t *inout_off)
{
    struct darshan_core_name_record_ref *ref;
    struct darshan_name_record *name_rec;
    char *my_buf, *shared_buf;
    char *tmp_p;
    int rec_len;
    int shared_buf_len;
    int name_rec_buf_len;
    int ret;

    /* remove globally shared name records from non-zero ranks */
    name_rec_buf_len = core->name_mem_used;
    if(my_rank > 0)
    {
        name_rec = core->log_name_p;
        my_buf = core->log_name_p;
        shared_buf = core->comp_buf;
        shared_buf_len = 0;
        while(name_rec_buf_len > 0)
        {
            HASH_FIND(hlink, core->name_hash, &(name_rec->id),
                sizeof(darshan_record_id), ref);
            assert(ref);
            rec_len = sizeof(darshan_record_id) + strlen(name_rec->name) + 1;

            if(ref->global_mod_flags)
            {
                /* this record is shared globally, move to the temporary
                 * shared record buffer and update hash references
                 */
                HASH_DELETE(hlink, core->name_hash, ref);
                memcpy(shared_buf, name_rec, rec_len);
                ref->name_record = (struct darshan_name_record *)shared_buf;
                HASH_ADD(hlink, core->name_hash, name_record->id,
                    sizeof(darshan_record_id), ref);

                shared_buf += rec_len;
                shared_buf_len += rec_len;
            }
            else
            {
                /* this record is not shared, but we still may need to
                 * move it forward in our buffer and update hash references
                 */
                if(my_buf != (char *)name_rec)
                {
                    HASH_DELETE(hlink, core->name_hash, ref);
                    memcpy(my_buf, name_rec, rec_len);
                    ref->name_record =(struct darshan_name_record *)my_buf;
                    HASH_ADD(hlink, core->name_hash, name_record->id,
                        sizeof(darshan_record_id), ref);
                }
                my_buf += rec_len;
            }

            tmp_p = (char *)name_rec + rec_len;
            name_rec = (struct darshan_name_record *)tmp_p;
            name_rec_buf_len -= rec_len;
        }
        name_rec_buf_len = core->name_mem_used - shared_buf_len;

        /* append the shared records back to the end of the name record
         * buffer and update hash table references so we can still
         * reference these records as modules shutdown
         */
        name_rec = (struct darshan_name_record *)core->comp_buf;
        while(shared_buf_len > 0)
        {
            HASH_FIND(hlink, core->name_hash, &(name_rec->id),
                sizeof(darshan_record_id), ref);
            assert(ref);
            rec_len = sizeof(darshan_record_id) + strlen(name_rec->name) + 1;

            HASH_DELETE(hlink, core->name_hash, ref);
            memcpy(my_buf, name_rec, rec_len);
            ref->name_record = (struct darshan_name_record *)my_buf;
            HASH_ADD(hlink, core->name_hash, name_record->id,
                sizeof(darshan_record_id), ref);

            tmp_p = (char *)name_rec + rec_len;
            name_rec = (struct darshan_name_record *)tmp_p;
            my_buf += rec_len;
            shared_buf_len -= rec_len;
        }
    }

    /* collectively write out the record hash to the darshan log */
    ret = darshan_log_append_all(log_fh, core, core->log_name_p,
        name_rec_buf_len, inout_off);

    return(ret);
}
#else
static int darshan_log_write_name_record_hash(int log_fh,
    struct darshan_core_runtime *core, uint64_t *inout_off)
{
    return darshan_log_append_all(log_fh, core, core->log_name_p, core->name_mem_used, inout_off);
}
#endif /* #ifdef HAVE_MPI */

/* NOTE: inout_off contains the starting offset of this append at the beginning
 *       of the call, and contains the ending offset at the end of the call.
 *       This variable is only valid on the root rank (rank 0).
 */
#ifdef HAVE_MPI
static int darshan_log_append_all(struct darshan_mpi_file log_fh, struct darshan_core_runtime *core,
    void *buf, int count, uint64_t *inout_off)
{
    MPI_Offset send_off, my_off;
    MPI_Status status; /* if used, darshan_mpi_file_* needs to be updated */
    int comp_buf_sz = 0;
    int ret;

    /* compress the input buffer */
    ret = darshan_deflate_buffer((void **)&buf, &count, 1,
        core->comp_buf, &comp_buf_sz);
    if(ret < 0)
        comp_buf_sz = 0;

    /* figure out where everyone is writing using scan */
    send_off = comp_buf_sz;
    if(my_rank == 0)
    {
        send_off += *inout_off; /* rank 0 knows the beginning offset */
    }

    darshan_mpi_scan(&send_off, &my_off, 1, MPI_OFFSET,
        MPI_SUM, MPI_COMM_WORLD);
    /* scan is inclusive; subtract local size back out */
    my_off -= comp_buf_sz;

    if(ret == 0)
    {
        /* no compression errors, proceed with the collective write */
        ret = darshan_mpi_file_write_at_all(log_fh, my_off,
            core->comp_buf, comp_buf_sz, MPI_BYTE, &status);
    }
    else
    {
        /* error during compression. preserve and return error to caller,
         * but participate in collective write to avoid deadlock.
         */
        (void)darshan_mpi_file_write_at_all(log_fh, my_off,
            core->comp_buf, comp_buf_sz, MPI_BYTE, &status);
    }

    if(nprocs > 1)
    {
        /* send the ending offset from rank (n-1) to rank 0 
         *
         * we don't bother factoring out MPI_Send because it should never be
         * called in serial mode when nprocs == 1
         */
        if(my_rank == (nprocs-1))
        {
            my_off += comp_buf_sz;
            PMPI_Send(&my_off, 1, MPI_OFFSET, 0, 0,
                MPI_COMM_WORLD);
        }
        if(my_rank == 0)
        {
            PMPI_Recv(&my_off, 1, MPI_OFFSET, (nprocs-1), 0,
                MPI_COMM_WORLD, &status);

            *inout_off = my_off;
        }
    }
    else
    {
        *inout_off = my_off + comp_buf_sz;
    }

    if(ret != 0)
        return(-1);
    return(0);
}
#else
static int darshan_log_append_all(int log_fh, struct darshan_core_runtime *core,
    void *buf, int count, uint64_t *inout_off)
{
    ssize_t written;
    int comp_buf_sz = 0;
    int ret;

    /* compress the input buffer */
    ret = darshan_deflate_buffer((void **)&buf, &count, 1, core->comp_buf, &comp_buf_sz);

    if(ret != 0)
        return(-1);

    written = pwrite(log_fh, core->comp_buf, comp_buf_sz, *inout_off);

    if (written != comp_buf_sz)
        return(-1);

    *inout_off += comp_buf_sz;
    return(0);
}
#endif /* #ifdef HAVE_MPI */

/* free darshan core data structures to shutdown */
static void darshan_core_cleanup(struct darshan_core_runtime* core)
{
    struct darshan_core_name_record_ref *tmp, *ref;
    int i;

    HASH_ITER(hlink, core->name_hash, ref, tmp)
    {
        HASH_DELETE(hlink, core->name_hash, ref);
        free(ref);
    }

    for(i = 0; i < DARSHAN_MAX_MODS; i++)
    {
        if(core->mod_array[i])
        {        
            free(core->mod_array[i]);
            core->mod_array[i] = NULL;
        }
    }

#ifndef __DARSHAN_ENABLE_MMAP_LOGS
    free(core->log_hdr_p);
    free(core->log_job_p);
    free(core->log_exemnt_p);
    free(core->log_name_p);
    free(core->log_mod_p);
#endif

    if(core->comp_buf)
        free(core->comp_buf);
    free(core);

    return;
}

/* crude benchmarking hook into darshan-core to benchmark Darshan
 * shutdown overhead using a variety of application I/O workloads
 */
extern void darshan_posix_shutdown_bench_setup();
extern void darshan_mpiio_shutdown_bench_setup();
void darshan_shutdown_bench(int argc, char **argv)
{
    /* clear out existing core runtime structure */
    if(darshan_core)
    {
        darshan_core_cleanup(darshan_core);
        darshan_core = NULL;
    }

    /***********************************************************/
    /* restart darshan */
    darshan_core_initialize(argc, argv);

    darshan_posix_shutdown_bench_setup(1);
    darshan_mpiio_shutdown_bench_setup(1);

    if(my_rank == 0)
        fprintf(stderr, "# 1 unique file per proc\n");
#ifdef HAVE_MPI
    darshan_mpi_barrier(MPI_COMM_WORLD);
#endif
    darshan_core_shutdown();
    darshan_core = NULL;

    sleep(1);

    /***********************************************************/
    /* restart darshan */
    darshan_core_initialize(argc, argv);

    darshan_posix_shutdown_bench_setup(2);
    darshan_mpiio_shutdown_bench_setup(2);

    if(my_rank == 0)
        fprintf(stderr, "# 1 shared file per proc\n");
#ifdef HAVE_MPI
    darshan_mpi_barrier(MPI_COMM_WORLD);
#endif
    darshan_core_shutdown();
    darshan_core = NULL;

    sleep(1);

    /***********************************************************/
    /* restart darshan */
    darshan_core_initialize(argc, argv);

    darshan_posix_shutdown_bench_setup(3);
    darshan_mpiio_shutdown_bench_setup(3);

    if(my_rank == 0)
        fprintf(stderr, "# 1024 unique files per proc\n");
#ifdef HAVE_MPI
    darshan_mpi_barrier(MPI_COMM_WORLD);
#endif
    darshan_core_shutdown();
    darshan_core = NULL;

    sleep(1);

    /***********************************************************/
    /* restart darshan */
    darshan_core_initialize(argc, argv);

    darshan_posix_shutdown_bench_setup(4);
    darshan_mpiio_shutdown_bench_setup(4);

    if(my_rank == 0)
        fprintf(stderr, "# 1024 shared files per proc\n");
#ifdef HAVE_MPI
    darshan_mpi_barrier(MPI_COMM_WORLD);
#endif
    darshan_core_shutdown();
    darshan_core = NULL;

    sleep(1);

    /***********************************************************/

    return;
}

/* ********************************************************* */

void darshan_core_register_module(
    darshan_module_id mod_id,
    darshan_module_shutdown mod_shutdown_func,
    int *inout_mod_buf_size,
    int *rank,
    int *sys_mem_alignment)
{
    struct darshan_core_module* mod;
    int mod_mem_req = *inout_mod_buf_size;
    int mod_mem_avail;

    *inout_mod_buf_size = 0;

    DARSHAN_CORE_LOCK();
    if((darshan_core == NULL) ||
       (mod_id >= DARSHAN_MAX_MODS) ||
       (darshan_core->mod_array[mod_id] != NULL))
    {
        /* just return if darshan not initialized, the module id
         * is invalid, or if the module is already registered
         */
        DARSHAN_CORE_UNLOCK();
        return;
    }

    mod = malloc(sizeof(*mod));
    if(!mod)
    {
        DARSHAN_CORE_UNLOCK();
        return;
    }
    memset(mod, 0, sizeof(*mod));

    /* set module's record buffer and max memory usage */
    mod_mem_avail = darshan_mod_mem_quota - darshan_core->mod_mem_used;
    if(mod_mem_avail >= mod_mem_req)
        mod->rec_mem_avail = mod_mem_req;
    else
        mod->rec_mem_avail = mod_mem_avail;
    mod->rec_buf_start = darshan_core->log_mod_p + darshan_core->mod_mem_used;
    mod->rec_buf_p = mod->rec_buf_start;
    mod->mod_shutdown_func = mod_shutdown_func;

    /* register module with darshan */
    darshan_core->mod_array[mod_id] = mod;
    darshan_core->mod_mem_used += mod->rec_mem_avail;
    darshan_core->log_hdr_p->mod_ver[mod_id] = darshan_module_versions[mod_id];
#ifdef __DARSHAN_ENABLE_MMAP_LOGS
    darshan_core->log_hdr_p->mod_map[mod_id].off =
        ((char *)mod->rec_buf_start - (char *)darshan_core->log_hdr_p);
#endif

    *inout_mod_buf_size = mod->rec_mem_avail;
    DARSHAN_CORE_UNLOCK();

    /* set the memory alignment and calling process's rank, if desired */
    if(sys_mem_alignment)
        *sys_mem_alignment = darshan_mem_alignment;
    if(rank)
        *rank = my_rank;

    return;
}

/* NOTE: we currently don't really have a simple way of returning the
 * memory allocated to this module back to darshan to hand out to
 * other modules, so all we do is disable the module so darshan does
 * not attempt to call into it at shutdown time
 */
void darshan_core_unregister_module(
    darshan_module_id mod_id)
{
    DARSHAN_CORE_LOCK();
    if(!darshan_core)
    {
        DARSHAN_CORE_UNLOCK();
        return;
    }

    /* update darshan internal structures and header */
    free(darshan_core->mod_array[mod_id]);
    darshan_core->mod_array[mod_id] = NULL;
    darshan_core->log_hdr_p->mod_ver[mod_id] = 0;
#ifdef __DARSHAN_ENABLE_MMAP_LOGS
    darshan_core->log_hdr_p->mod_map[mod_id].off =
        darshan_core->log_hdr_p->mod_map[mod_id].len = 0;
#endif

    DARSHAN_CORE_UNLOCK();
    return;
}

darshan_record_id darshan_core_gen_record_id(
    const char *name)
{
    /* hash the input name to get a unique id for this record */
    return darshan_hash((unsigned char *)name, strlen(name), 0);
}

void *darshan_core_register_record(
    darshan_record_id rec_id,
    const char *name,
    darshan_module_id mod_id,
    int rec_len,
    struct darshan_fs_info *fs_info)
{
    struct darshan_core_name_record_ref *ref;
    void *rec_buf;
    int ret;

    DARSHAN_CORE_LOCK();
    if(!darshan_core)
    {
        DARSHAN_CORE_UNLOCK();
        return(NULL);
    }

    /* check to see if this module has enough space to store a new record */
    if(darshan_core->mod_array[mod_id]->rec_mem_avail < rec_len)
    {
        DARSHAN_MOD_FLAG_SET(darshan_core->log_hdr_p->partial_flag, mod_id);
        DARSHAN_CORE_UNLOCK();
        return(NULL);
    }

    /* register a name record if a name is given for this record */
    if(name)
    {
        /* check to see if we've already stored the id->name mapping for
         * this record, and add a new name record if not
         */
        HASH_FIND(hlink, darshan_core->name_hash, &rec_id,
            sizeof(darshan_record_id), ref);
        if(!ref)
        {
            ret = darshan_add_name_record_ref(darshan_core, rec_id, name, mod_id);
            if(ret == 0)
            {
                DARSHAN_MOD_FLAG_SET(darshan_core->log_hdr_p->partial_flag, mod_id);
                DARSHAN_CORE_UNLOCK();
                return(NULL);
            }
        }
        else
        {
            DARSHAN_MOD_FLAG_SET(ref->mod_flags, mod_id);
        }
    }

    rec_buf = darshan_core->mod_array[mod_id]->rec_buf_p;
    darshan_core->mod_array[mod_id]->rec_buf_p += rec_len;
    darshan_core->mod_array[mod_id]->rec_mem_avail -= rec_len;
#ifdef __DARSHAN_ENABLE_MMAP_LOGS
    darshan_core->log_hdr_p->mod_map[mod_id].len += rec_len;
#endif
    DARSHAN_CORE_UNLOCK();

    if(fs_info)
        darshan_fs_info_from_path(name, fs_info);

    return(rec_buf);;
}

char *darshan_core_lookup_record_name(darshan_record_id rec_id)
{
    struct darshan_core_name_record_ref *ref;
    char *name = NULL;

    DARSHAN_CORE_LOCK();
    HASH_FIND(hlink, darshan_core->name_hash, &rec_id,
        sizeof(darshan_record_id), ref);
    if(ref)
        name = ref->name_record->name;
    DARSHAN_CORE_UNLOCK();

    return(name);
}

void darshan_instrument_fs_data(int fs_type, const char *path, int fd)
{
#ifdef DARSHAN_LUSTRE
    /* allow lustre to generate a record if we configured with lustre support */
    if(fs_type == LL_SUPER_MAGIC)
    {
        darshan_instrument_lustre_file(path, fd);
        return;
    }
#endif
    return;
}

double darshan_core_wtime()
{
    DARSHAN_CORE_LOCK();
    if(!darshan_core)
    {
        DARSHAN_CORE_UNLOCK();
        return(0);
    }
    DARSHAN_CORE_UNLOCK();

    return(time_nanoseconds() - darshan_core->wtime_offset);
}

#ifdef DARSHAN_PRELOAD
extern int (*__real_vfprintf)(FILE *stream, const char *format, va_list);
#else
extern int __real_vfprintf(FILE *stream, const char *format, va_list);
#endif
void darshan_core_fprintf(
    FILE *stream, const char *format, ...)
{
    va_list ap;

    MAP_OR_FAIL(vfprintf);

    va_start(ap, format);
    __real_vfprintf(stream, format, ap);
    va_end(ap);

    return;
}

int darshan_core_excluded_path(const char *path)
{
    char *exclude, *include;
    int tmp_index = 0;
    int tmp_jndex;

    /* if user has set DARSHAN_EXCLUDE_DIRS, override the default ones */
    if (user_darshan_path_exclusions != NULL) {
        while((exclude = user_darshan_path_exclusions[tmp_index++])) {
            if(!(strncmp(exclude, path, strlen(exclude))))
                return(1);
        }
    }
    else
    {
        /* scan blacklist for paths to exclude */
        while((exclude = darshan_path_exclusions[tmp_index++])) {
            if(!(strncmp(exclude, path, strlen(exclude)))) {
                /* before excluding path, ensure it's not in whitelist */
                tmp_jndex = 0;
                while((include = darshan_path_inclusions[tmp_jndex++])) {
                    if(!(strncmp(include, path, strlen(include))))
                        return(0); /* whitelist hits are always tracked */
                }
                return(1); /* if not in whitelist, then blacklist it */
            }
        }
    }

    /* if not in blacklist, no problem */
    return(0);
}

int darshan_core_disabled_instrumentation()
{
    int ret;

    DARSHAN_CORE_LOCK();
    if(darshan_core)
        ret = 0;
    else
        ret = 1;
    DARSHAN_CORE_UNLOCK();

    return(ret);
}



/*
 * Darshan MPI stubs: pass through to PMPI_* if MPI is initialized; otherwise
 * fake the MPI call.
 */
#ifdef HAVE_MPI
static double time_nanoseconds()
{
    return darshan_mpi_wtime();
}
#else
static double time_nanoseconds()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec) + (double)(t.tv_nsec) * 1.0e-9;
}
#endif

#ifdef HAVE_MPI
static size_t sizeof_mpi_datatype(MPI_Datatype datatype)
{
    int i;
    size_t size;
    switch (datatype)
    {
        case MPI_INT:       size = sizeof(int); break;
        case MPI_FLOAT:     size = sizeof(float); break;
        case MPI_DOUBLE:    size = sizeof(double); break;
        case MPI_CHAR:
        case MPI_BYTE:      size = sizeof(char); break;
        case MPI_LONG:      size = sizeof(long); break;
        case MPI_LONG_LONG:
        case MPI_UINT32_T:  size = sizeof(uint32_t); break;
        case MPI_UINT64_T:  size = sizeof(uint64_t); break;
        case MPI_INT64_T:   size = sizeof(int64_t); break;
        case MPI_OFFSET:    size = sizeof(off_t); break;
        default:            size = 0;
    }
    return size;
}

double darshan_mpi_wtime()
{
    struct timespec t;

    if (using_mpi)
        return PMPI_Wtime();

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec) + (double)(t.tv_nsec) * 1.0e-9;
}

int darshan_mpi_comm_size(MPI_Comm comm, int *_nprocs)
{
    if (using_mpi)
        return PMPI_Comm_size(comm, _nprocs);
    *_nprocs = 1;
    return MPI_SUCCESS;
}

int darshan_mpi_comm_rank(MPI_Comm comm, int *me)
{
    if (using_mpi)
        return PMPI_Comm_rank(comm, me);
    *me = 0;
    return MPI_SUCCESS;
}

int generic_serial_reduce(void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype)
{
    size_t size = sizeof_mpi_datatype(datatype);

    if (sendbuf == MPI_IN_PLACE || recvbuf == MPI_IN_PLACE)
        return MPI_SUCCESS;

    if (size > 0)
    {
        memcpy(recvbuf, sendbuf, count * size);
        return MPI_SUCCESS;
    }

    return MPI_ERR_TYPE;
}

int darshan_mpi_allreduce(void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
{
    if (using_mpi)
        return PMPI_Allreduce(sendbuf, recvbuf, count, datatype, op, comm);

    return generic_serial_reduce(sendbuf, recvbuf, count, datatype);
}

int darshan_mpi_reduce(void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm)
{
    if (using_mpi)
        return PMPI_Reduce(sendbuf, recvbuf, count, datatype, op, root, comm);

    return generic_serial_reduce(sendbuf, recvbuf, count, datatype);
}

int darshan_mpi_reduce_records(void *sendbuf, void *recvbuf, int count, int record_size, MPI_Op op, int root, MPI_Comm comm)
{
    /* this rolls up MPI_Type_contiguous, MPI_Type_commit, MPI_Reduce, and
     * MPI_Type_free into a single function so that we don't have to implement
     * MPI derived types for serial mode */

    MPI_Datatype red_type;
    if (using_mpi)
    {
        /* construct a datatype for a module's file record.  This is serving no
         * purpose except to make sure we can do a reduction on proper boundaries
         */
        PMPI_Type_contiguous(record_size, MPI_BYTE, &red_type);
        PMPI_Type_commit(&red_type);

        /* reduce shared file records */
        PMPI_Reduce(sendbuf, recvbuf, count, red_type, op, 0, comm);

        /* clean up */
        PMPI_Type_free(&red_type);
    }
    else if (sendbuf != MPI_IN_PLACE && recvbuf != MPI_IN_PLACE)
        memcpy(recvbuf, sendbuf, count * record_size);

    return MPI_SUCCESS;
}

int darshan_mpi_scan(void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
{
    if (using_mpi)
        return PMPI_Scan(sendbuf, recvbuf, count, datatype, op, comm);

    return generic_serial_reduce(sendbuf, recvbuf, count, datatype);
}

int darshan_mpi_gather(void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm)
{
    if (using_mpi)
        return PMPI_Gather(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, root, comm);

    return generic_serial_reduce(sendbuf, recvbuf, sendcount, sendtype);
}

int darshan_mpi_barrier(MPI_Comm comm)
{
    if (using_mpi)
        return PMPI_Barrier(comm);

    return MPI_SUCCESS;
}

int darshan_mpi_bcast(void *buf, int count, MPI_Datatype datatype, int root, MPI_Comm comm)
{
    if (using_mpi)
        return PMPI_Bcast(buf, count, datatype, root, comm);

    return MPI_SUCCESS;
}

int darshan_mpi_op_create(MPI_User_function *user_fn, int commute, MPI_Op *op)
{
    if (using_mpi)
        return PMPI_Op_create(user_fn, commute, op);

    return MPI_SUCCESS;
}

int darshan_mpi_op_free(MPI_Op *op)
{
    if (using_mpi)
        return PMPI_Op_free(op);

    return MPI_SUCCESS;
}

/*
 * Emulate MPI-IO
 */
int darshan_mpi_file_open(MPI_Comm comm, const char *filename, int amode, MPI_Info info, struct darshan_mpi_file *fh)
{
    int ret;
    int posix_flags = 0;
    if (using_mpi)
    {
        ret = PMPI_File_open(comm, filename, amode, info, &(fh->fh.mpi));
        fh->fhtype = FH_TYPE_MPI;
        return ret;
    }

    if (amode & MPI_MODE_RDWR) posix_flags |= O_RDWR;
    if (amode & MPI_MODE_RDONLY) posix_flags |= O_RDONLY;
    if (amode & MPI_MODE_WRONLY) posix_flags |= O_WRONLY;
    if (amode & MPI_MODE_CREATE) posix_flags |= O_CREAT;
    if (amode & MPI_MODE_EXCL) posix_flags |= O_EXCL;
    if (amode & MPI_MODE_APPEND) posix_flags |= O_APPEND;

    /* note that darshan_mpi_file_open is only called after instrumentation has
     * been disabled so it's safe to use the otherwise-instrumented open() call
     */
    if (posix_flags & O_CREAT)
        fh->fh.posix = open(filename, posix_flags, S_IRUSR|S_IWUSR);
    else
        fh->fh.posix = open(filename, posix_flags);
    fh->fhtype = FH_TYPE_POSIX;

    return MPI_SUCCESS;
}

int darshan_mpi_file_close(struct darshan_mpi_file *fh)
{
    if (fh->fhtype == FH_TYPE_MPI)
        return PMPI_File_close(&(fh->fh.mpi));

    close(fh->fh.posix);
    return MPI_SUCCESS;
}

int darshan_mpi_file_write_at(struct darshan_mpi_file fh, MPI_Offset offset,
    const void *buf, int count, MPI_Datatype datatype, MPI_Status *status)
{
    ssize_t ret;
    if (fh.fhtype == FH_TYPE_MPI)
        return PMPI_File_write_at(fh.fh.mpi, offset, buf, count, datatype, status);

    ret = pwrite(fh.fh.posix, buf, count * sizeof_mpi_datatype(datatype), offset);
    /* note that at this stage, status is left untouched because Darshan never
       actually checks it!  if we ever check status, we will have to create a
       union object to pass meaningful info back via both MPI and non-MPI
       mechanisms */
    return MPI_SUCCESS;
}

int darshan_mpi_file_write_at_all(struct darshan_mpi_file fh, MPI_Offset offset,
    const void *buf, int count, MPI_Datatype datatype, MPI_Status *status)
{
    if (fh.fhtype == FH_TYPE_MPI)
        return PMPI_File_write_at_all(fh.fh.mpi, offset, buf, count, datatype, status);

    return darshan_mpi_file_write_at(fh, offset, buf, count, datatype, status);
}

int darshan_mpi_info_create(MPI_Info *info)
{
    if (using_mpi)
        return MPI_Info_create(info);
    return MPI_SUCCESS;
}

int darshan_mpi_info_set(MPI_Info info, char *key, char *value)
{
    if (using_mpi)
        return MPI_Info_set(info, key, value);
    return MPI_SUCCESS;
}

int darshan_mpi_info_free(MPI_Info *info)
{
    if (using_mpi)
        return MPI_Info_free(info);
    return MPI_SUCCESS;
}

/*
 * Emulate derived MPI datatypes
 */
int darshan_mpi_type_contiguous(int count, MPI_Datatype oldtype, MPI_Datatype *newtype)
{
    if (using_mpi)
        return PMPI_Type_contiguous(count, oldtype, newtype);

    *newtype = MPI_DATATYPE_NULL;
    return MPI_SUCCESS;
}

int darshan_mpi_type_commit(MPI_Datatype *datatype)
{
    if (using_mpi)
        return PMPI_Type_commit(datatype);

    return MPI_SUCCESS;
}

int darshan_mpi_type_free(MPI_Datatype *datatype)
{
    if (using_mpi)
        return PMPI_Type_free(datatype);

    return MPI_SUCCESS;
}
#endif /* #ifdef HAVE_MPI */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
