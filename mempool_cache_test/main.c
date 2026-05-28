/* mempool_cache_test.c
 * 测试目标：对比 no-cache vs cache=256 在单线程/多线程下的 alloc/free 性能
 * 编译：见文末 Makefile / meson 说明
 * 运行：./build/mempool_cache_test -l 0-3 -n 4
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include <rte_eal.h>
#include <rte_mempool.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_launch.h>
#include <rte_common.h>

/* ── 可调参数 ─────────────────────────────────────────────── */
#define NUM_OBJS      (1 << 16)   /* mempool 总对象数 65536        */
#define OBJ_SIZE      64          /* 每个对象字节数（模拟 mbuf）    */
#define CACHE_SIZE    256         /* 有 cache 时的 per-lcore 大小   */
#define WARMUP_ITER   2000        /* 预热轮数                        */
#define TEST_ITER     200000      /* 正式计时轮数                    */
/* ─────────────────────────────────────────────────────────── */

/* 每个 lcore 的参数 + 结果 */
// struct lcore_args {
//     struct rte_mempool *mp;
//     uint64_t avg_alloc_cycles;
//     uint64_t avg_free_cycles;
//     int      result;   /* 0=ok, -1=error */
// };
struct lcore_args {
    struct rte_mempool *mp;
    uint64_t avg_alloc_cycles;
    uint64_t avg_free_cycles;
    int      result;   /* 0=ok, -1=error */
}__rte_cache_aligned;
/* 对齐缓存行会比未对齐时候少用8-10 cycles */

/* 打印分隔线 */
static void print_sep(void) {
    printf("─────────────────────────────────────────────────────────\n");
}

/* ── 单个 lcore 的测试函数 ────────────────────────────────── */
static int lcore_test_fn(void *arg)
{
    struct lcore_args *a = (struct lcore_args *)arg;
    unsigned lcore_id = rte_lcore_id();
    void *obj = NULL;
    uint64_t t0;
    uint64_t sum_alloc = 0, sum_free = 0;

    /* ── 1. 预热：排除冷 cache / TLB miss 噪音 ── */
    for (int i = 0; i < WARMUP_ITER; i++) {
        if (rte_mempool_get(a->mp, &obj) == 0)
            rte_mempool_put(a->mp, obj);
    }

    /* ── 2. 正式计时 ── */
    for (int i = 0; i < TEST_ITER; i++) {

        /* alloc */
        t0 = rte_get_tsc_cycles();
        if (unlikely(rte_mempool_get(a->mp, &obj) != 0)) {
            printf("[lcore %u] mempool empty at iter %d!\n", lcore_id, i);
            a->result = -1;
            return -1;
        }
        sum_alloc += rte_get_tsc_cycles() - t0;

        /* free */
        t0 = rte_get_tsc_cycles();
        rte_mempool_put(a->mp, obj);
        sum_free += rte_get_tsc_cycles() - t0;
    }

    a->avg_alloc_cycles = sum_alloc / TEST_ITER;
    a->avg_free_cycles  = sum_free  / TEST_ITER;
    a->result = 0;

    printf("  lcore %-2u  alloc=%4" PRIu64 " cy  free=%4" PRIu64 " cy  "
           "total=%4" PRIu64 " cy\n",
           lcore_id,
           a->avg_alloc_cycles,
           a->avg_free_cycles,
           a->avg_alloc_cycles + a->avg_free_cycles);

    return 0;
}

/* ── 跑一次完整实验（所有可用 lcore 并发） ───────────────── */
static void run_experiment(struct rte_mempool *mp, const char *label,
                           struct lcore_args *args_arr, unsigned n_lcores)
{
    printf("\n[实验] %s  (lcore 数=%u)\n", label, n_lcores);
    print_sep();

    /* 初始化每个 lcore 的参数 */
    unsigned i = 0;
    unsigned lcore_id;
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (i >= n_lcores) break;
        args_arr[i].mp = mp;
        args_arr[i].avg_alloc_cycles = 0;
        args_arr[i].avg_free_cycles  = 0;
        args_arr[i].result = 0;
        i++;
    }

    /* main lcore 也测，index = n_lcores（最后一个槽） */
    args_arr[n_lcores].mp = mp;
    args_arr[n_lcores].avg_alloc_cycles = 0;
    args_arr[n_lcores].avg_free_cycles  = 0;
    args_arr[n_lcores].result = 0;

    /* 启动 worker lcores */
    i = 0;
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (i >= n_lcores) break;
        rte_eal_remote_launch(lcore_test_fn, &args_arr[i], lcore_id);
        i++;
    }

    /* main lcore 也跑（用最后一个槽） */
    lcore_test_fn(&args_arr[n_lcores]);

    /* 等待所有 worker */
    rte_eal_mp_wait_lcore();

    /* ── 汇总 ── */
    uint64_t total_alloc = 0, total_free = 0;
    unsigned count = 0;
    int any_error = 0;

    /* worker 结果 */
    for (unsigned j = 0; j < n_lcores; j++) {
        if (args_arr[j].result != 0) { any_error = 1; continue; }
        total_alloc += args_arr[j].avg_alloc_cycles;
        total_free  += args_arr[j].avg_free_cycles;
        count++;
    }
    /* main lcore 结果 */
    if (args_arr[n_lcores].result == 0) {
        total_alloc += args_arr[n_lcores].avg_alloc_cycles;
        total_free  += args_arr[n_lcores].avg_free_cycles;
        count++;
    }

    print_sep();
    if (any_error) {
        printf("  ⚠ 部分 lcore 出错（mempool 耗尽），请增大 NUM_OBJS\n");
    }
    if (count > 0) {
        printf("  平均 alloc : %" PRIu64 " cycles\n", total_alloc / count);
        printf("  平均 free  : %" PRIu64 " cycles\n", total_free  / count);
        printf("  平均总计   : %" PRIu64 " cycles\n",
               (total_alloc + total_free) / count);
    }
}

/* ── main ─────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    /* EAL 初始化 */
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eal_init failed\n");

    /* 统计实际可用的 lcore 数（不含 main） */
    unsigned n_workers = rte_lcore_count() - 1;   /* worker 数 */
    unsigned total     = rte_lcore_count();        /* 含 main   */
    printf("\n========================================================\n");
    printf("  mempool cache 性能实验\n");
    printf("  总 lcore 数=%u  (main + %u workers)\n", total, n_workers);
    printf("  NUM_OBJS=%u  OBJ_SIZE=%u  CACHE_SIZE=%u\n",
           NUM_OBJS, OBJ_SIZE, CACHE_SIZE);
    printf("  WARMUP=%u  TEST_ITER=%u\n", WARMUP_ITER, TEST_ITER);
    printf("========================================================\n");

    /* args 数组：最多 RTE_MAX_LCORE 个槽（含 main） */
    struct lcore_args args[RTE_MAX_LCORE];
    memset(args, 0, sizeof(args));

    /* ── 创建两个 mempool ── */
    struct rte_mempool *mp_no_cache = rte_mempool_create(
        "mp_no_cache",
        NUM_OBJS, OBJ_SIZE,
        0,          /* cache_size = 0 */
        0, NULL, NULL, NULL, NULL,
        rte_socket_id(), 0);
    if (!mp_no_cache)
        rte_exit(EXIT_FAILURE, "创建 mp_no_cache 失败\n");

    struct rte_mempool *mp_cache = rte_mempool_create(
        "mp_cache",
        NUM_OBJS, OBJ_SIZE,
        CACHE_SIZE, /* cache_size = 256 */
        0, NULL, NULL, NULL, NULL,
        rte_socket_id(), 0);
    if (!mp_cache)
        rte_exit(EXIT_FAILURE, "创建 mp_cache 失败\n");

    /* ── 实验 1：no cache ── */
    run_experiment(mp_no_cache, "no cache  (cache_size=0)  ", args, n_workers);

    /* ── 实验 2：with cache ── */
    run_experiment(mp_cache,    "with cache(cache_size=256)", args, n_workers);

    /* ── 最终对比表 ── */
    printf("\n");
    print_sep();
    printf("  汇总对比表（平均 cycles / op）\n");
    print_sep();
    printf("  %-30s  %8s  %8s  %8s\n", "场景", "alloc", "free", "alloc+free");
    printf("  %-30s  %8s  %8s  %8s\n", "──────────────────────────────",
           "────────", "────────", "──────────");

    /* 重新跑一次统计（只取 main lcore 的单线程数据用于表格行）
     * 多线程数据已打印在上方；这里只做文字提示 */
    printf("  %-30s  %8s  %8s  %8s\n",
           "见上方各 lcore 输出", "↑", "↑", "↑");
    print_sep();

    rte_eal_cleanup();
    return 0;
}