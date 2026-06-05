/* rss_verify.c
 * RSS 分流验证程序
 * - 开 4 个 RX 队列
 * - 每个队列用独立线程 poll
 * - 每秒打印每个队列的收包数和 hash 值分布
 *
 * 编译：
 *   gcc rss_verify.c -o rss_verify \
 *       $(pkg-config --cflags --libs libdpdk) \
 *       -Wl,--whole-archive -lrte_net_i40e -Wl,--no-whole-archive
 *
 * 运行（port0 收包，port1 还给内核用于发包）：
 *   sudo ./rss_verify -l 0-4 -n 4 --
 *
 * 停止：Ctrl+C
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_launch.h>

#define RX_PORT         0           /* 收包的端口 */
#define NB_QUEUES       4           /* RX 队列数 */
#define NB_RXDESC       1024        /* 每个队列的描述符数 */
#define BURST_SIZE      32          /* 每次 burst 收包数 */
#define MBUF_POOL_SIZE  8192        /* mbuf 池大小 */
#define STATS_INTERVAL  1           /* 打印统计间隔（秒） */

/* 每个队列的统计 */
struct queue_stats {
    uint64_t rx_packets;            /* 收包总数 */
    uint64_t rx_bytes;              /* 收字节总数 */
    uint64_t last_packets;          /* 上次统计时的包数（用于计算速率） */
} __rte_cache_aligned;

static struct queue_stats stats[NB_QUEUES];
static volatile int force_quit = 0;

/* 信号处理 */
static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n收到退出信号，正在停止...\n");
        force_quit = 1;
    }
}

/* 每个 lcore 的参数 */
struct lcore_args {
    uint16_t queue_id;
};

static struct lcore_args lcore_args[NB_QUEUES];

/* 每个队列的收包循环，运行在独立 lcore 上 */
static int rx_loop(void *arg)
{
    struct lcore_args *args = (struct lcore_args *)arg;
    uint16_t queue_id = args->queue_id;
    struct rte_mbuf *pkts[BURST_SIZE];

    printf("lcore %u 开始 poll port %u queue %u\n",
           rte_lcore_id(), RX_PORT, queue_id);

    while (!force_quit) {
        uint16_t nb_rx = rte_eth_rx_burst(RX_PORT, queue_id,
                                           pkts, BURST_SIZE);
        if (nb_rx == 0)
            continue;

        stats[queue_id].rx_packets += nb_rx;
        for (int i = 0; i < nb_rx; i++) {
            stats[queue_id].rx_bytes += pkts[i]->pkt_len;
            rte_pktmbuf_free(pkts[i]);
        }
    }

    return 0;
}

/* 主线程：每秒打印统计 */
static void print_stats_loop(void)
{
    uint64_t prev_tsc = rte_rdtsc();
    uint64_t hz = rte_get_tsc_hz();

    printf("\n%-8s %12s %12s %12s %10s\n",
           "队列", "总包数", "总字节", "包/秒", "占比%");
    printf("%-8s %12s %12s %12s %10s\n",
           "----", "--------", "--------", "------", "------");

    while (!force_quit) {
        rte_delay_us_sleep(STATS_INTERVAL * 1000000);

        uint64_t cur_tsc = rte_rdtsc();
        double elapsed = (double)(cur_tsc - prev_tsc) / hz;
        prev_tsc = cur_tsc;

        /* 计算总包数，用于百分比 */
        uint64_t total = 0;
        for (int q = 0; q < NB_QUEUES; q++)
            total += stats[q].rx_packets;

        /* 清屏，重新打印 */
        printf("\033[2J\033[H");
        printf("=== RSS 分流验证  (Ctrl+C 退出) ===\n\n");
        printf("%-8s %12s %12s %12s %10s\n",
               "队列", "总包数", "总字节", "包/秒", "占比%");
        printf("%-8s %12s %12s %12s %10s\n",
               "----", "--------", "--------", "------", "------");

        for (int q = 0; q < NB_QUEUES; q++) {
            uint64_t pps = (uint64_t)((stats[q].rx_packets -
                           stats[q].last_packets) / elapsed);
            double pct = total > 0 ?
                         (double)stats[q].rx_packets / total * 100.0 : 0.0;

            /* 可视化 bar */
            int bar_len = (int)(pct / 5);  /* 每5%一格，最多20格 */
            char bar[24] = {0};
            for (int b = 0; b < bar_len && b < 20; b++)
                bar[b] = '#';

            printf("queue %-2d  %12" PRIu64 " %12" PRIu64
                   " %12" PRIu64 " %8.1f%%  |%-20s|\n",
                   q,
                   stats[q].rx_packets,
                   stats[q].rx_bytes,
                   pps,
                   pct,
                   bar);

            stats[q].last_packets = stats[q].rx_packets;
        }

        printf("\n总计:     %12" PRIu64 " 包\n", total);

        if (total == 0) {
            printf("\n[!] 还没收到包，请从 port1 (enp1s0f1np1) 发包：\n");
            printf("    sudo hping3 -S -p 80 -s 1001 -c 100 2.2.2.2 -I enp1s0f1np1\n");
            printf("    sudo hping3 -S -p 80 -s 1002 -c 100 2.2.2.2 -I enp1s0f1np1\n");
            printf("    sudo hping3 -S -p 80 -s 1003 -c 100 2.2.2.2 -I enp1s0f1np1\n");
            printf("    sudo hping3 -S -p 80 -s 1004 -c 100 2.2.2.2 -I enp1s0f1np1\n");
        }
    }
}

/* 初始化端口 */
static int init_port(uint16_t port_id, struct rte_mempool *mbuf_pool)
{
    struct rte_eth_dev_info dev_info;
    int ret;

    ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret != 0) {
        printf("获取端口 %u 信息失败\n", port_id);
        return ret;
    }

    /* 端口配置：开启 RSS */
    struct rte_eth_conf port_conf = {
        .rxmode = {
            .mq_mode = RTE_ETH_MQ_RX_RSS,
        },
        .rx_adv_conf = {
            .rss_conf = {
                .rss_key = NULL,    /* 使用默认 key */
                .rss_hf  = RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP |
                           RTE_ETH_RSS_IPV4 | RTE_ETH_RSS_IPV6,
            },
        },
    };

    /* 对齐到硬件支持的 RSS 类型 */
    port_conf.rx_adv_conf.rss_conf.rss_hf &=
        dev_info.flow_type_rss_offloads;

    printf("Port %u RSS hash 类型: 0x%"PRIx64"\n",
           port_id, port_conf.rx_adv_conf.rss_conf.rss_hf);

    ret = rte_eth_dev_configure(port_id, NB_QUEUES, 1, &port_conf);
    if (ret != 0) {
        printf("配置端口 %u 失败: %d\n", port_id, ret);
        return ret;
    }

    /* 配置每个 RX 队列 */
    for (int q = 0; q < NB_QUEUES; q++) {
        ret = rte_eth_rx_queue_setup(port_id, q, NB_RXDESC,
                                      rte_eth_dev_socket_id(port_id),
                                      NULL, mbuf_pool);
        if (ret != 0) {
            printf("配置 RX queue %d 失败: %d\n", q, ret);
            return ret;
        }
    }

    /* 配置 TX 队列（只需要 1 个，不发包也要有） */
    ret = rte_eth_tx_queue_setup(port_id, 0, 512,
                                  rte_eth_dev_socket_id(port_id), NULL);
    if (ret != 0) {
        printf("配置 TX queue 失败: %d\n", ret);
        return ret;
    }

    /* 启动端口 */
    ret = rte_eth_dev_start(port_id);
    if (ret != 0) {
        printf("启动端口 %u 失败: %d\n", port_id, ret);
        return ret;
    }

    /* 开启混杂模式，确保能收到包 */
    rte_eth_promiscuous_enable(port_id);

    struct rte_eth_link link;
    rte_eth_link_get(port_id, &link);
    printf("Port %u 链路: %s, %u Mbps\n",
           port_id,
           link.link_status ? "UP" : "DOWN",
           link.link_speed);

    return 0;
}

int main(int argc, char *argv[])
{
    int ret;

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* 初始化 EAL */
    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "EAL 初始化失败\n");

    uint16_t nb_ports = rte_eth_dev_count_avail();
    printf("发现 %u 个 DPDK 端口\n", nb_ports);

    if (nb_ports < 1)
        rte_exit(EXIT_FAILURE, "需要至少 1 个端口\n");

    /* 创建 mbuf 池 */
    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create(
        "MBUF_POOL", MBUF_POOL_SIZE,
        256, 0,
        RTE_MBUF_DEFAULT_BUF_SIZE,
        rte_socket_id());

    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "创建 mbuf 池失败\n");

    /* 初始化 port0 */
    ret = init_port(RX_PORT, mbuf_pool);
    if (ret != 0)
        rte_exit(EXIT_FAILURE, "初始化 port %u 失败\n", RX_PORT);

    /* 检查可用 lcore 数量 */
    uint16_t nb_lcores = rte_lcore_count();
    if (nb_lcores < NB_QUEUES + 1)
        rte_exit(EXIT_FAILURE,
                 "需要 %d 个 lcore（当前 %u），请用 -l 0-%d\n",
                 NB_QUEUES + 1, nb_lcores, NB_QUEUES);

    /* 把每个队列的 rx_loop 分配到独立 lcore */
    uint16_t lcore_id;
    int queue_idx = 0;
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (queue_idx >= NB_QUEUES)
            break;
        lcore_args[queue_idx].queue_id = queue_idx;
        rte_eal_remote_launch(rx_loop, &lcore_args[queue_idx], lcore_id);
        queue_idx++;
    }

    printf("\n开始收包，每秒刷新统计...\n");
    printf("请从 enp1s0f1np1 发包（port1 需还给内核驱动）\n\n");

    /* 主线程打印统计 */
    print_stats_loop();

    /* 等待所有 lcore 退出 */
    rte_eal_mp_wait_lcore();

    /* 打印最终统计 */
    printf("\n=== 最终统计 ===\n");
    uint64_t total = 0;
    for (int q = 0; q < NB_QUEUES; q++) {
        total += stats[q].rx_packets;
        printf("queue %d: %" PRIu64 " 包  (%.1f%%)\n",
               q, stats[q].rx_packets,
               total > 0 ? (double)stats[q].rx_packets / total * 100.0 : 0.0);
    }
    printf("总计:    %" PRIu64 " 包\n", total);

    rte_eth_dev_stop(RX_PORT);
    rte_eth_dev_close(RX_PORT);
    rte_eal_cleanup();

    return 0;
}