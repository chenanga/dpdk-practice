/* x710_check.c
 * 检测 X710 网卡特性，对比官方规格与实际支持情况
 *
 * 编译：
    gcc x710_check.c -o x710_check \
    $(pkg-config --cflags --libs libdpdk) \
    -Wl,--whole-archive -lrte_net_i40e -Wl,--no-whole-archive
 *
 * 运行：
 *   sudo ./x710_check
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_version.h>
#include <rte_dev.h>

/* X710 官方规格（XL710 datasheet） */
#define X710_SPEC_MAX_RX_QUEUES     64
#define X710_SPEC_MAX_TX_QUEUES     64
#define X710_SPEC_RETA_SIZE         512
#define X710_SPEC_SPEED_10G         10000
#define X710_SPEC_SPEED_25G         25000

/* RSS 类型名称表 */
struct rss_type_info {
    uint64_t    flag;
    const char *name;
    int         x710_expected;  /* X710 是否应该支持 */
};

static const struct rss_type_info rss_types[] = {
    { RTE_ETH_RSS_IPV4,               "ipv4",               1 },
    { RTE_ETH_RSS_FRAG_IPV4,          "ipv4-frag",          1 },
    { RTE_ETH_RSS_NONFRAG_IPV4_TCP,   "ipv4-tcp",           1 },
    { RTE_ETH_RSS_NONFRAG_IPV4_UDP,   "ipv4-udp",           1 },
    { RTE_ETH_RSS_NONFRAG_IPV4_SCTP,  "ipv4-sctp",          1 },
    { RTE_ETH_RSS_NONFRAG_IPV4_OTHER, "ipv4-other",         1 },
    { RTE_ETH_RSS_IPV6,               "ipv6",               1 },
    { RTE_ETH_RSS_FRAG_IPV6,          "ipv6-frag",          1 },
    { RTE_ETH_RSS_NONFRAG_IPV6_TCP,   "ipv6-tcp",           1 },
    { RTE_ETH_RSS_NONFRAG_IPV6_UDP,   "ipv6-udp",           1 },
    { RTE_ETH_RSS_NONFRAG_IPV6_SCTP,  "ipv6-sctp",          1 },
    { RTE_ETH_RSS_NONFRAG_IPV6_OTHER, "ipv6-other",         1 },
    { RTE_ETH_RSS_L2_PAYLOAD,         "l2-payload",         0 },
    { RTE_ETH_RSS_IPV6_EX,            "ipv6-ex",            0 },
    { RTE_ETH_RSS_IPV6_TCP_EX,        "ipv6-tcp-ex",        0 },
    { RTE_ETH_RSS_IPV6_UDP_EX,        "ipv6-udp-ex",        0 },
    { RTE_ETH_RSS_PORT,               "port",               0 },
    { RTE_ETH_RSS_VXLAN,              "vxlan",              0 },
    { RTE_ETH_RSS_GENEVE,             "geneve",             0 },
    { RTE_ETH_RSS_NVGRE,              "nvgre",              0 },
    { 0, NULL, 0 }
};

/* RX offload 名称表 */
struct offload_info {
    uint64_t    flag;
    const char *name;
    int         x710_expected;
};

static const struct offload_info rx_offloads[] = {
    { RTE_ETH_RX_OFFLOAD_VLAN_STRIP,       "vlan-strip",        1 },
    { RTE_ETH_RX_OFFLOAD_IPV4_CKSUM,       "ipv4-cksum",        1 },
    { RTE_ETH_RX_OFFLOAD_UDP_CKSUM,        "udp-cksum",         1 },
    { RTE_ETH_RX_OFFLOAD_TCP_CKSUM,        "tcp-cksum",         1 },
    { RTE_ETH_RX_OFFLOAD_TCP_LRO,          "tcp-lro",           0 },
    { RTE_ETH_RX_OFFLOAD_QINQ_STRIP,       "qinq-strip",        0 },
    { RTE_ETH_RX_OFFLOAD_OUTER_IPV4_CKSUM, "outer-ipv4-cksum",  1 },
    { RTE_ETH_RX_OFFLOAD_MACSEC_STRIP,     "macsec-strip",      0 },
    { RTE_ETH_RX_OFFLOAD_VLAN_FILTER,      "vlan-filter",       1 },
    { RTE_ETH_RX_OFFLOAD_VLAN_EXTEND,      "vlan-extend",       0 },
    { RTE_ETH_RX_OFFLOAD_SCATTER,          "scatter",           1 },
    { RTE_ETH_RX_OFFLOAD_TIMESTAMP,        "timestamp",         0 },
    { RTE_ETH_RX_OFFLOAD_SECURITY,         "security",          0 },
    { RTE_ETH_RX_OFFLOAD_KEEP_CRC,         "keep-crc",          0 },
    { RTE_ETH_RX_OFFLOAD_RSS_HASH,         "rss-hash",          1 },
    { 0, NULL, 0 }
};

static const struct offload_info tx_offloads[] = {
    { RTE_ETH_TX_OFFLOAD_VLAN_INSERT,      "vlan-insert",       1 },
    { RTE_ETH_TX_OFFLOAD_IPV4_CKSUM,       "ipv4-cksum",        1 },
    { RTE_ETH_TX_OFFLOAD_UDP_CKSUM,        "udp-cksum",         1 },
    { RTE_ETH_TX_OFFLOAD_TCP_CKSUM,        "tcp-cksum",         1 },
    { RTE_ETH_TX_OFFLOAD_SCTP_CKSUM,       "sctp-cksum",        1 },
    { RTE_ETH_TX_OFFLOAD_TCP_TSO,          "tcp-tso",           1 },
    { RTE_ETH_TX_OFFLOAD_UDP_TSO,          "udp-tso",           0 },
    { RTE_ETH_TX_OFFLOAD_OUTER_IPV4_CKSUM, "outer-ipv4-cksum",  1 },
    { RTE_ETH_TX_OFFLOAD_QINQ_INSERT,      "qinq-insert",       0 },
    { RTE_ETH_TX_OFFLOAD_VXLAN_TNL_TSO,    "vxlan-tso",         1 },
    { RTE_ETH_TX_OFFLOAD_GRE_TNL_TSO,      "gre-tso",           1 },
    { RTE_ETH_TX_OFFLOAD_IPIP_TNL_TSO,     "ipip-tso",          0 },
    { RTE_ETH_TX_OFFLOAD_GENEVE_TNL_TSO,   "geneve-tso",        1 },
    { RTE_ETH_TX_OFFLOAD_MACSEC_INSERT,    "macsec-insert",     0 },
    { RTE_ETH_TX_OFFLOAD_MT_LOCKFREE,      "mt-lockfree",       0 },
    { RTE_ETH_TX_OFFLOAD_MULTI_SEGS,       "multi-segs",        1 },
    { RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE,   "mbuf-fast-free",    1 },
    { RTE_ETH_TX_OFFLOAD_SECURITY,         "security",          0 },
    { 0, NULL, 0 }
};

static void print_separator(char c, int width)
{
    for (int i = 0; i < width; i++) putchar(c);
    putchar('\n');
}

static void print_header(const char *title)
{
    printf("\n");
    print_separator('=', 60);
    printf("  %s\n", title);
    print_separator('=', 60);
}

static void print_check(const char *name, int actual, int expected, const char *actual_str)
{
    const char *status;
    if (actual && expected)
        status = "OK  ";
    else if (!actual && !expected)
        status = "OK  ";
    else if (actual && !expected)
        status = "BONUS";  /* 支持了但官方没说 */
    else
        status = "MISS";   /* 官方说支持但实际没有 */

    printf("  [%s] %-30s %s\n", status, name, actual_str ? actual_str : (actual ? "yes" : "no"));
}

static void check_queue_config(uint16_t port_id, struct rte_eth_dev_info *info)
{
    print_header("队列配置");

    /* RX 队列 */
    printf("  %-30s 实际: %-6u  规格: %u  %s\n",
           "Max RX queues",
           info->max_rx_queues,
           X710_SPEC_MAX_RX_QUEUES,
           info->max_rx_queues >= X710_SPEC_MAX_RX_QUEUES ? "[OK  ]" : "[MISS]");

    /* TX 队列 */
    printf("  %-30s 实际: %-6u  规格: %u  %s\n",
           "Max TX queues",
           info->max_tx_queues,
           X710_SPEC_MAX_TX_QUEUES,
           info->max_tx_queues >= X710_SPEC_MAX_TX_QUEUES ? "[OK  ]" : "[MISS]");

    /* RETA 大小 */
    printf("  %-30s 实际: %-6u  规格: %u  %s\n",
           "RSS RETA size",
           info->reta_size,
           X710_SPEC_RETA_SIZE,
           info->reta_size >= X710_SPEC_RETA_SIZE ? "[OK  ]" : "[MISS]");

    /* 描述符范围 */
    printf("\n  RX 描述符范围: %u ~ %u (align=%u)\n",
           info->rx_desc_lim.nb_min,
           info->rx_desc_lim.nb_max,
           info->rx_desc_lim.nb_align);
    printf("  TX 描述符范围: %u ~ %u (align=%u)\n",
           info->tx_desc_lim.nb_min,
           info->tx_desc_lim.nb_max,
           info->tx_desc_lim.nb_align);
}

static void check_speed(uint16_t port_id, struct rte_eth_dev_info *info)
{
    print_header("链路速度");

    uint32_t speeds = info->speed_capa;
    struct { uint32_t flag; const char *name; } speed_map[] = {
        { RTE_ETH_LINK_SPEED_1G,  "1G"  },
        { RTE_ETH_LINK_SPEED_10G, "10G" },
        { RTE_ETH_LINK_SPEED_25G, "25G" },
        { RTE_ETH_LINK_SPEED_40G, "40G" },
        { RTE_ETH_LINK_SPEED_100G,"100G"},
        { 0, NULL }
    };
    for (int i = 0; speed_map[i].name; i++) {
        int supported = !!(speeds & speed_map[i].flag);
        printf("  [%s] %s\n", supported ? "OK  " : "    ", speed_map[i].name);
    }

    /* 当前链路状态 */
    int ret;
    struct rte_eth_link link;
    ret = rte_eth_link_get_nowait(port_id, &link);
    (void)ret;
    printf("\n  当前链路: %s, 速度: %u Mbps, %s\n",
           link.link_status ? "UP" : "DOWN",
           link.link_speed,
           link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX ? "全双工" : "半双工");
}

static void check_rss(struct rte_eth_dev_info *info)
{
    print_header("RSS 支持（对比 X710 规格）");
    printf("  %-5s %-30s %-10s %-10s\n", "状态", "RSS 类型", "实际", "X710规格");
    print_separator('-', 60);

    uint64_t actual = info->flow_type_rss_offloads;
    int miss_count = 0;

    for (int i = 0; rss_types[i].name; i++) {
        int supported = !!(actual & rss_types[i].flag);
        int expected  = rss_types[i].x710_expected;
        const char *status;

        if (supported && expected)       status = "OK   ";
        else if (!supported && !expected) status = "N/A  ";
        else if (supported && !expected)  status = "BONUS";
        else { status = "MISS "; miss_count++; }

        if (expected || supported)
            printf("  [%s] %-30s %-10s %-10s\n",
                   status,
                   rss_types[i].name,
                   supported ? "yes" : "no",
                   expected  ? "yes" : "no");
    }

    printf("\n  RSS hash key 长度: %u 字节\n", info->hash_key_size);
    if (miss_count > 0)
        printf("  警告: %d 项应支持但实际不支持\n", miss_count);
    else
        printf("  RSS 特性全部符合规格\n");
}

static void check_rx_offloads(struct rte_eth_dev_info *info)
{
    print_header("RX Offload（对比 X710 规格）");
    printf("  %-5s %-30s %-10s %-10s\n", "状态", "Offload 类型", "实际", "X710规格");
    print_separator('-', 60);

    uint64_t actual = info->rx_offload_capa;
    int miss_count = 0;

    for (int i = 0; rx_offloads[i].name; i++) {
        int supported = !!(actual & rx_offloads[i].flag);
        int expected  = rx_offloads[i].x710_expected;
        const char *status;

        if (supported && expected)        status = "OK   ";
        else if (!supported && !expected) status = "N/A  ";
        else if (supported && !expected)  status = "BONUS";
        else { status = "MISS "; miss_count++; }

        printf("  [%s] %-30s %-10s %-10s\n",
               status,
               rx_offloads[i].name,
               supported ? "yes" : "no",
               expected  ? "yes" : "no");
    }

    if (miss_count > 0)
        printf("\n  警告: %d 项应支持但实际不支持\n", miss_count);
    else
        printf("\n  RX Offload 全部符合规格\n");
}

static void check_tx_offloads(struct rte_eth_dev_info *info)
{
    print_header("TX Offload（对比 X710 规格）");
    printf("  %-5s %-30s %-10s %-10s\n", "状态", "Offload 类型", "实际", "X710规格");
    print_separator('-', 60);

    uint64_t actual = info->tx_offload_capa;
    int miss_count = 0;

    for (int i = 0; tx_offloads[i].name; i++) {
        int supported = !!(actual & tx_offloads[i].flag);
        int expected  = tx_offloads[i].x710_expected;
        const char *status;

        if (supported && expected)        status = "OK   ";
        else if (!supported && !expected) status = "N/A  ";
        else if (supported && !expected)  status = "BONUS";
        else { status = "MISS "; miss_count++; }

        printf("  [%s] %-30s %-10s %-10s\n",
               status,
               tx_offloads[i].name,
               supported ? "yes" : "no",
               expected  ? "yes" : "no");
    }

    if (miss_count > 0)
        printf("\n  警告: %d 项应支持但实际不支持\n", miss_count);
    else
        printf("\n  TX Offload 全部符合规格\n");
}

static void check_misc(uint16_t port_id, struct rte_eth_dev_info *info)
{
    print_header("其他特性");

    /* SRIOV */
    printf("  %-30s %u\n", "Max VFs (SRIOV)", info->max_vfs);

    /* MAC 地址 */
    printf("  %-30s %u\n", "Max MAC addresses", info->max_mac_addrs);

    /* MTU */
    uint16_t mtu;
    rte_eth_dev_get_mtu(port_id, &mtu);
    printf("  %-30s %u\n", "当前 MTU", mtu);
    printf("  %-30s %u\n", "Max MTU", info->max_mtu);
    printf("  %-30s %u\n", "Min MTU", info->min_mtu);

    /* 驱动信息 */
    printf("\n  驱动: %s\n", info->driver_name);

    /* Rx/Tx burst 限制 */
    printf("  %-30s %u\n", "Max RX pktlen", info->max_rx_pktlen);
}

int main(int argc, char *argv[])
{
    int ret;

    /* 初始化 EAL */
    ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        fprintf(stderr, "EAL 初始化失败: %d\n", ret);
        return 1;
    }

    uint16_t nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0) {
        fprintf(stderr, "没有找到可用的网卡，请先用 dpdk-devbind 绑定网卡\n");
        return 1;
    }

    printf("\n");
    print_separator('*', 60);
    printf("  X710 特性检测工具  (DPDK %s)\n", rte_version());
    printf("  发现 %u 个端口\n", nb_ports);
    print_separator('*', 60);

    uint16_t port_id;
    RTE_ETH_FOREACH_DEV(port_id) {
        struct rte_eth_dev_info info;

        ret = rte_eth_dev_info_get(port_id, &info);
        if (ret != 0) {
            fprintf(stderr, "获取 port %u 信息失败\n", port_id);
            continue;
        }

        char dev_name[RTE_ETH_NAME_MAX_LEN];
        rte_eth_dev_get_name_by_port(port_id, dev_name);
        printf("\n\n");
        print_separator('#', 60);
        printf("  Port %u: %s\n", port_id, dev_name);
        print_separator('#', 60);

        /* 判断是否是 X710 */
        int is_x710 = (strstr(info.driver_name, "i40e") != NULL);
        if (!is_x710) {
            printf("  警告: 驱动是 %s，不是 i40e，规格对比基于 X710 可能不准确\n",
                   info.driver_name);
        }

        check_speed(port_id, &info);
        check_queue_config(port_id, &info);
        check_rss(&info);
        check_rx_offloads(&info);
        check_tx_offloads(&info);
        check_misc(port_id, &info);
    }

    printf("\n");
    print_separator('*', 60);
    printf("  检测完成\n");
    print_separator('*', 60);
    printf("\n");

    rte_eal_cleanup();
    return 0;
}
