/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2014.  ALL RIGHTS RESERVED.
*
* $COPYRIGHT$
* $HEADER$
*/

#define _GNU_SOURCE /* For CPUSET_SIZE */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "libperf.h"

#include <ucs/sys/sys.h>
#include <ucs/debug/log.h>
#include <linux/sched.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <netdb.h>
#include <getopt.h>
#include <string.h>
#include <sys/types.h>
#include <locale.h>
#if HAVE_MPI
#  include <mpi.h>
#endif

enum {
    TEST_FLAG_PRINT_RESULTS = UCS_BIT(0),
    TEST_FLAG_PRINT_TEST    = UCS_BIT(1),
    TEST_FLAG_SET_AFFINITY  = UCS_BIT(8),
    TEST_FLAG_NUMERIC_FMT   = UCS_BIT(9)
};


typedef struct sock_rte_group {
    int                          is_server;
    int                          connfd;
    void                         *self;
    size_t                       self_size;
} sock_rte_group_t;


struct perftest_context {
    ucx_perf_test_params_t       params;

    char                         dev_name[UCT_MAX_NAME_LEN];
    char                         tl_name[UCT_MAX_NAME_LEN];

    uct_context_h                ucth;
    const char                   *server_addr;
    int                          port;
    unsigned                     cpu;
    unsigned                     flags;

    sock_rte_group_t             sock_rte_group;
};


static int safe_send(int sock, void *data, size_t size)
{
    size_t total = 0;
    int ret;

    while (total < size) {
        ret = send(sock, (char*)data + total, size - total, 0);
        if (ret < 0) {
            ucs_error("send() failed: %m");
            return -1;
        }
        total += ret;
    }
    return 0;
}

static int safe_recv(int sock, void *data, size_t size)
{
    size_t total = 0;
    int ret;

    while (total < size) {
        ret = recv(sock, (char*)data + total, size - total, 0);
        if (ret < 0) {
            ucs_error("recv() failed: %m");
            return -1;
        }
        total += ret;
    }
    return 0;
}

static void print_progress(ucx_perf_result_t *result, unsigned flags)
{
    static const char *fmt_numeric =  "%'14.0f %9.3f %9.3f %9.3f %10.2f %10.2f %'11.0f %'11.0f\n";
    static const char *fmt_plain   =  "%14.0f %9.3f %9.3f %9.3f %10.2f %10.2f %11.0f %11.0f\n";

    if (flags & TEST_FLAG_PRINT_RESULTS) {
        printf((flags & TEST_FLAG_NUMERIC_FMT) ? fmt_numeric : fmt_plain,
               (double)result->iters,
               result->latency.typical * 1000000.0,
               result->latency.moment_average * 1000000.0,
               result->latency.total_average * 1000000.0,
               result->bandwidth.moment_average / (1024.0 * 1024.0),
               result->bandwidth.total_average / (1024.0 * 1024.0),
               result->msgrate.moment_average,
               result->msgrate.total_average);
        fflush(stdout);
    }
}

static void print_header(struct perftest_context *ctx)
{
    const char *test_cmd_str;
    const char *test_type_str;

    switch (ctx->params.command) {
    case UCX_PERF_TEST_CMD_AM_SHORT:
        test_cmd_str = "uct_am_short()";
        break;
    case UCX_PERF_TEST_CMD_PUT_SHORT:
        test_cmd_str = "uct_put_short()";
        break;
    default:
        test_cmd_str = "(undefined)";
        break;
    }

    switch (ctx->params.test_type) {
    case UCX_PERF_TEST_TYPE_PINGPONG:
        test_type_str = "Ping-pong";
        break;
    default:
        test_type_str = "(undefined)";
        break;
    }

    if (ctx->flags & TEST_FLAG_PRINT_TEST) {
        printf("+------------------------------------------------------------------------------------------+\n");
        printf("| API:          %-60s               |\n", test_cmd_str);
        printf("| Test type:    %-60s               |\n", test_type_str);
        printf("| Message size: %-60Zu               |\n", ctx->params.message_size);
    }

    if (ctx->flags & TEST_FLAG_PRINT_RESULTS) {
        printf("+--------------+-----------------------------+---------------------+-----------------------+\n");
        printf("|              |       latency (usec)        |   bandwidth (MB/s)  |  message rate (msg/s) |\n");
        printf("+--------------+---------+---------+---------+----------+----------+-----------+-----------+\n");
        printf("| # iterations | typical | average | overall |  average |  overall |   average |   overall |\n");
        printf("+--------------+---------+---------+---------+----------+----------+-----------+-----------+\n");
    } else if (ctx->flags & TEST_FLAG_PRINT_TEST) {
        printf("+------------------------------------------------------------------------------------------+\n");
    }
}

static void print_footer(struct perftest_context *ctx, ucx_perf_result_t *result)
{
    if (ctx->flags & TEST_FLAG_PRINT_RESULTS) {
        printf("+Overall-------+---------+---------+---------+----------+----------+-----------+-----------+\n");
        print_progress(result, ctx->flags);
    }
}

static void usage(struct perftest_context *ctx, const char *program)
{
    printf("Usage: %s [ server-hostname ] [ options ]\n", program);
    printf("\n");
#if HAVE_MPI
    printf("This test can be also launched as an MPI application\n");
#endif
    printf("  Common options:\n");
    printf("     -h           Show this help message.\n");
    printf("     -p <port>    TCP port to use for data exchange. (%d)\n", ctx->port);
    printf("     -c <cpu>     Set affinity to this CPU. (off)\n");
    printf("\n");
    printf("  Test options:\n");
    printf("     -d <device>  Device to use for testing.\n");
    printf("     -x <tl>      Transport to use for testing.\n");
    printf("     -t <test>    Test to run:\n");
    printf("                     put_lat  : put latency.\n");
    printf("                     put_bw   : put bandwidth / message rate.\n");
    printf("                     am_lat   : active message latency.\n");
    printf("     -n <iters>   Number of iterations to run. (%ld)\n", ctx->params.max_iter);
    printf("     -s <size>    Message size. (%Zu)\n", ctx->params.message_size);
    printf("     -w <iters>   Number of warm-up iterations. (%Zu)\n", ctx->params.warmup_iter);
    printf("     -N           Use numeric formatting - thousands separator.\n");
    printf("\n");
    printf("  Server options:\n");
    printf("     -l           Accept clients in an infinite loop\n");
    printf("\n");
}

const char *__basename(const char *path)
{
    const char *p = strrchr(path, '/');
    return (p == NULL) ? path : p;
}

void print_transports(struct perftest_context *ctx)
{
    uct_resource_desc_t *resources, *res;
    unsigned num_resources;
    ucs_status_t status;

    status = uct_query_resources(ctx->ucth, &resources, &num_resources);
    if (status != UCS_OK) {
        ucs_error("Failed to query resources");
        return;
    }

    printf("+-----------+-------------+-----------------+--------------+\n");
    printf("| device    | transport   | bandwidth       | latency      |\n");
    printf("+-----------+-------------+-----------------+--------------+\n");

    for (res = resources; res < resources + num_resources; ++res) {
       printf("| %-9s | %-11s | %10.2f MB/s | %7.3f usec |\n",
               res->dev_name, res->tl_name,
               res->bandwidth / (1024.0 * 1024.0),
               res->latency / 1000.0);
    }
    printf("+-----------+-------------+-----------------+--------------+\n");

    uct_release_resource_list(resources);
}

static ucs_status_t parse_opts(struct perftest_context *ctx, int argc, char **argv)
{
    char c;

    ctx->params.command         = UCX_PERF_TEST_CMD_LAST;
    ctx->params.test_type       = UCX_PERF_TEST_TYPE_LAST;
    ctx->params.data_layout     = UCX_PERF_DATA_LAYOUT_BUFFER;
    ctx->params.wait_mode       = UCX_PERF_WAIT_MODE_LAST;
    ctx->params.warmup_iter     = 10000;
    ctx->params.message_size    = 8;
    ctx->params.alignment       = ucs_get_page_size();
    ctx->params.max_iter        = 1000000l;
    ctx->params.max_time        = 0.0;
    ctx->params.report_interval = 1.0;
    strcpy(ctx->dev_name, "");
    strcpy(ctx->tl_name, "");
    ctx->server_addr            = NULL;
    ctx->port                   = 13337;
    ctx->flags                  = 0;

    while ((c = getopt (argc, argv, "p:d:x:t:n:s:c:Nlw:")) != -1) {
        switch (c) {
        case 'p':
            ctx->port = atoi(optarg);
            break;
        case 'd':
            ucs_snprintf_zero(ctx->dev_name, sizeof(ctx->dev_name), "%s", optarg);
            break;
        case 'x':
            ucs_snprintf_zero(ctx->tl_name, sizeof(ctx->tl_name), "%s", optarg);
            break;
        case 't':
            if (0 == strcmp(optarg, "am_lat")) {
                ctx->params.command   = UCX_PERF_TEST_CMD_AM_SHORT;
                ctx->params.test_type = UCX_PERF_TEST_TYPE_PINGPONG;
            } else if (0 == strcmp(optarg, "put_lat")) {
                ctx->params.command   = UCX_PERF_TEST_CMD_PUT_SHORT;
                ctx->params.test_type = UCX_PERF_TEST_TYPE_PINGPONG;
            } else if (0 == strcmp(optarg, "put_bw")) {
                ctx->params.command   = UCX_PERF_TEST_CMD_PUT_SHORT;
                ctx->params.test_type = UCX_PERF_TEST_TYPE_STREAM_UNI;
            } else {
                ucs_error("Invalid option argument for -t");
                return -1;
            }
            break;
        case 'n':
            ctx->params.max_iter = atol(optarg);
            break;
        case 's':
            ctx->params.message_size = atol(optarg);
            break;
        case 'N':
            ctx->flags |= TEST_FLAG_NUMERIC_FMT;
            break;
        case 'c':
            ctx->flags |= TEST_FLAG_SET_AFFINITY;
            ctx->cpu = atoi(optarg);
            break;
        case 'w':
            ctx->params.warmup_iter = atol(optarg);
            break;
        case 'l':
            print_transports(ctx);
            return UCS_ERR_CANCELED;
        case 'h':
        default:
           usage(ctx, __basename(argv[0]));
           return UCS_ERR_INVALID_PARAM;
        }
    }

    if (optind < argc) {
        ctx->server_addr   = argv[optind];
    }

    return UCS_OK;
}

static ucs_status_t validate_params(struct perftest_context *ctx)
{
    if ((ctx->params.command == UCX_PERF_TEST_CMD_LAST) ||
        (ctx->params.test_type == UCX_PERF_TEST_TYPE_LAST))
    {
        ucs_error("Must specify test type");
        return UCS_ERR_INVALID_PARAM;
    }

    if (!strlen(ctx->dev_name)) {
        ucs_error("Must specify device name");
        return UCS_ERR_INVALID_PARAM;
    }

    if (!strlen(ctx->tl_name)) {
        ucs_error("Must specify transport");
        return UCS_ERR_INVALID_PARAM;
    }

    return UCS_OK;
}

unsigned sock_rte_group_size(void *rte_group)
{
    return 2;
}

unsigned sock_rte_group_index(void *rte_group)
{
    sock_rte_group_t *group = rte_group;
    return group->is_server ? 0 : 1;
}

void sock_rte_barrier(void *rte_group)
{
    sock_rte_group_t *group = rte_group;
    const unsigned magic = 0xdeadbeef;
    unsigned sync;

    sync = magic;
    safe_send(group->connfd, &sync, sizeof(unsigned));

    sync = 0;
    safe_recv(group->connfd, &sync, sizeof(unsigned));

    ucs_assert(sync == magic);
}

void sock_rte_send(void *rte_group, unsigned dest, void *value, size_t size)
{
    sock_rte_group_t *group = rte_group;
    unsigned me = sock_rte_group_index(rte_group);

    if (dest == me) {
        group->self = realloc(group->self, group->self_size + size);
        memcpy(group->self + group->self_size, value, size);
        group->self_size += size;
    } else if (dest == 1 - me) {
        safe_send(group->connfd, value, size);
    }
}

void sock_rte_recv(void *rte_group, unsigned src, void *value, size_t size)
{
    sock_rte_group_t *group = rte_group;
    unsigned me = sock_rte_group_index(rte_group);
    void *prev_self;

    if (src == me) {
        ucs_assert(group->self_size >= size);
        memcpy(value, group->self, size);
        group->self_size -= size;

        prev_self = group->self;
        group->self = malloc(group->self_size);
        memcpy(group->self, prev_self + size, group->self_size);
    } else if (src == 1 - me) {
        safe_recv(group->connfd, value, size);
    }
}

static void sock_rte_report(void *rte_group, ucx_perf_result_t *result)
{
    sock_rte_group_t *group = rte_group;
    unsigned flags =
        ucs_container_of(group, struct perftest_context, sock_rte_group)->flags;

    print_progress(result, flags);
}

ucx_perf_test_rte_t sock_rte = {
    .group_size    = sock_rte_group_size,
    .group_index   = sock_rte_group_index,
    .barrier       = sock_rte_barrier,
    .send          = sock_rte_send,
    .recv          = sock_rte_recv,
    .report        = sock_rte_report,
};

ucs_status_t setup_sock_rte(struct perftest_context *ctx)
{
    struct sockaddr_in inaddr;
    struct hostent *he;
    ucs_status_t status;
    int optval = 1;
    int sockfd, connfd;
    int ret;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        ucs_error("socket() failed: %m");
        status = UCS_ERR_IO_ERROR;
        goto err;
    }

    if (ctx->server_addr == NULL) {
        optval = 1;
        ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
        if (ret < 0) {
            ucs_error("setsockopt(SO_REUSEADDR) failed: %m");
            status = UCS_ERR_INVALID_PARAM;
            goto err_close_sockfd;
        }

        inaddr.sin_family      = AF_INET;
        inaddr.sin_port        = htons(ctx->port);
        inaddr.sin_addr.s_addr = INADDR_ANY;
        memset(inaddr.sin_zero, 0, sizeof(inaddr.sin_zero));
        ret = bind(sockfd, (struct sockaddr*)&inaddr, sizeof(inaddr));
        if (ret < 0) {
            ucs_error("bind() failed: %m");
            status = UCS_ERR_INVALID_ADDR;
            goto err_close_sockfd;
        }

        ret = listen(sockfd, 10);
        if (ret < 0) {
            ucs_error("listen() failed: %m");
            status = UCS_ERR_IO_ERROR;
            goto err_close_sockfd;
        }

        printf("Waiting for connection...\n");

        /* Accept next connection */
        connfd = accept(sockfd, NULL, NULL);
        if (connfd < 0) {
            ucs_error("accept() failed: %m");
            status = UCS_ERR_IO_ERROR;
            goto err_close_sockfd;
        }

        close(sockfd);
        safe_recv(connfd, &ctx->params, sizeof(ctx->params));
        safe_recv(connfd, &ctx->dev_name, sizeof(ctx->dev_name));
        safe_recv(connfd, &ctx->tl_name, sizeof(ctx->tl_name));

        ctx->sock_rte_group.connfd    = connfd;
        ctx->sock_rte_group.is_server = 1;

    } else {
        status = validate_params(ctx);
        if (status != UCS_OK) {
            goto err_close_sockfd;
        }

        he = gethostbyname(ctx->server_addr);
        if (he == NULL || he->h_addr_list == NULL) {
            ucs_error("host %s not found: %s", ctx->server_addr,
                      hstrerror(h_errno));
            status = UCS_ERR_INVALID_ADDR;
            goto err_close_sockfd;
        }

        inaddr.sin_family = he->h_addrtype;
        inaddr.sin_port   = htons(ctx->port);
        ucs_assert(he->h_length == sizeof(inaddr.sin_addr));
        memcpy(&inaddr.sin_addr, he->h_addr_list[0], he->h_length);
        memset(inaddr.sin_zero, 0, sizeof(inaddr.sin_zero));

        ret = connect(sockfd, (struct sockaddr*)&inaddr, sizeof(inaddr));
        if (ret < 0) {
            ucs_error("connect() failed: %m");
            status = UCS_ERR_UNREACHABLE;
            goto err_close_sockfd;
        }

        safe_send(sockfd, &ctx->params, sizeof(ctx->params));
        safe_send(sockfd, &ctx->dev_name, sizeof(ctx->dev_name));
        safe_send(sockfd, &ctx->tl_name, sizeof(ctx->tl_name));

        ctx->sock_rte_group.connfd    = sockfd;
        ctx->sock_rte_group.is_server = 0;
    }

    if (ctx->sock_rte_group.is_server) {
        ctx->flags |= TEST_FLAG_PRINT_TEST;
    } else {
        ctx->flags |= TEST_FLAG_PRINT_RESULTS;
    }
    ctx->sock_rte_group.self      = NULL;
    ctx->sock_rte_group.self_size = 0;
    ctx->params.rte_group         = &ctx->sock_rte_group;
    ctx->params.rte               = &sock_rte;
    return UCS_OK;

err_close_sockfd:
    close(sockfd);
err:
    return status;
}

static ucs_status_t cleanup_sock_rte(struct perftest_context *ctx)
{
    close(ctx->sock_rte_group.connfd);
    free(ctx->sock_rte_group.self);
    return UCS_OK;
}

#if HAVE_MPI
unsigned mpi_rte_group_size(void *rte_group)
{
    int size;
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    return size;
}

unsigned mpi_rte_group_index(void *rte_group)
{
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    return rank;
}

void mpi_rte_barrier(void *rte_group)
{
    MPI_Barrier(MPI_COMM_WORLD);
}

void mpi_rte_send(void *rte_group, unsigned dest, void *value, size_t size)
{
    MPI_Send(value, size, MPI_CHAR, dest, 1, MPI_COMM_WORLD);
}

void mpi_rte_recv(void *rte_group, unsigned src, void *value, size_t size)
{
    MPI_Recv(value, size, MPI_CHAR, src, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
}

static void mpi_rte_report(void *rte_group, ucx_perf_result_t *result)
{
    struct perftest_context *ctx = rte_group;
    print_progress(result, ctx->flags);
}

ucx_perf_test_rte_t mpi_rte = {
    .group_size    = mpi_rte_group_size,
    .group_index   = mpi_rte_group_index,
    .barrier       = mpi_rte_barrier,
    .send          = mpi_rte_send,
    .recv          = mpi_rte_recv,
    .report        = mpi_rte_report,
};
#endif

static ucs_status_t setup_mpi_rte(struct perftest_context *ctx)
{
#if HAVE_MPI
    ucs_status_t status;
    int rank, size;

    status = validate_params(ctx);
    if (status != UCS_OK) {
        return status;;
    }

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if (rank == 0) {
        ctx->flags |= TEST_FLAG_PRINT_RESULTS;
    }

    ctx->sock_rte_group.self      = NULL;
    ctx->sock_rte_group.self_size = 0;
    ctx->params.rte_group         = ctx;
    ctx->params.rte               = &mpi_rte;
#endif
    return UCS_OK;
}

static ucs_status_t cleanup_mpi_rte(struct perftest_context *ctx)
{
    return UCS_OK;
}

static ucs_status_t check_system(struct perftest_context *ctx)
{
    cpu_set_t cpuset;
    unsigned i, count, nr_cpus;
    int ret;

    ret = sysconf(_SC_NPROCESSORS_CONF);
    if (ret < 0) {
        ucs_error("failed to get local cpu count: %m");
        return UCS_ERR_INVALID_PARAM;
    }
    nr_cpus = ret;

    memset(&cpuset, 0, sizeof(cpuset));
    if (ctx->flags & TEST_FLAG_SET_AFFINITY) {
        if (ctx->cpu >= nr_cpus) {
            ucs_error("cpu (%u) ot of range (0..%u)", ctx->cpu, nr_cpus - 1);
            return UCS_ERR_INVALID_PARAM;
        }
        CPU_SET(ctx->cpu, &cpuset);

        ret = sched_setaffinity(0, sizeof(cpuset), &cpuset);
        if (ret) {
            ucs_warn("sched_setaffinity() failed: %m");
            return UCS_ERR_INVALID_PARAM;
        }
    } else {
        ret = sched_getaffinity(0, sizeof(cpuset), &cpuset);
        if (ret) {
            ucs_warn("sched_getaffinity() failed: %m");
            return UCS_ERR_INVALID_PARAM;
        }

        count = 0;
        for (i = 0; i < CPU_SETSIZE; ++i) {
            if (CPU_ISSET(i, &cpuset)) {
                ++count;
            }
        }
        if (count > 2) {
            ucs_warn("CPU affinity is not set (bound to %u cpus)."
                            " Performance may be impacted.", count);
        }
    }

    return UCS_OK;
}

static ucs_status_t run_test(struct perftest_context *ctx)
{
    uct_iface_config_t *iface_config;
    ucx_perf_result_t result;
    ucs_status_t status;

    setlocale(LC_ALL, "en_US");

    status = uct_iface_config_read(ctx->ucth, ctx->tl_name, NULL, NULL, &iface_config);
    if (status != UCS_OK) {
        goto out;
    }

    print_header(ctx);
    status = uct_perf_test_run(ctx->ucth, &ctx->params, ctx->tl_name, ctx->dev_name,
                               iface_config, &result);
    if (status != UCS_OK) {
        ucs_error("Failed to run test: %s", ucs_status_string(status));
        goto out_release_cfg;
    }

    print_footer(ctx, &result);

out_release_cfg:
    uct_iface_config_release(iface_config);
out:
    return status;
}

int main(int argc, char **argv)
{
    struct perftest_context ctx;
    ucs_status_t status;
    int mpi = 0;
    int ret;

#ifdef __COVERITY__
    mpi = rand(); /* Shut up deadcode error */
#endif

#if HAVE_MPI
    /* Don't try MPI when running interactively */
    if (!isatty(0) && (MPI_Init(&argc, &argv) == 0)) {
        mpi = 1;
    }
#endif

    /* Create application context */
    status = uct_init(&ctx.ucth);
    if (status != UCS_OK) {
        ucs_error("Failed to initialize UCT: %s", ucs_status_string(status));
        ret = -1;
        goto out;
    }

    /* Parse command line */
    if (parse_opts(&ctx, argc, argv) != UCS_OK) {
        ret = -127;
        goto out_cleanup;
    }

    status = check_system(&ctx);
    if (status != UCS_OK) {
        ret = -1;
        goto out_cleanup;
    }

    /* Create RTE */
    status = (mpi) ? setup_mpi_rte(&ctx) : setup_sock_rte(&ctx);
    if (status != UCS_OK) {
        ret = -1;
        goto out_cleanup;
    }

    /* Run the test */

    status = run_test(&ctx);
    if (status != UCS_OK) {
        ret = -1;
        goto out_cleanup_rte;
    }

    ret = 0;

out_cleanup_rte:
    (mpi) ? cleanup_mpi_rte(&ctx) : cleanup_sock_rte(&ctx);
out_cleanup:
    uct_cleanup(ctx.ucth);
out:
#if HAVE_MPI
    if (mpi) {
        MPI_Finalize();
    }
#endif
    return ret;
}
