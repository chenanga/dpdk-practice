#!/usr/bin/env python3
"""
pkt_gen.py — Scapy 打流脚本，配合 DPDK l2fwd 使用
支持多线程 + CPU 绑核
"""

import argparse
import time
import threading
import signal
import sys
import os
from datetime import datetime

try:
    from scapy.all import Ether, IP, UDP, conf, get_if_list, get_if_hwaddr, raw
    L2SocketClass = conf.L2socket
except ImportError as e:
    print(f"[ERROR] scapy 导入失败: {e}")
    sys.exit(1)


# ──────────────────────────────────────────────
# 全局统计（原子性靠 GIL 保证，够用）
# ──────────────────────────────────────────────
stats = {
    "tx_pkts":  0,
    "tx_bytes": 0,
    "start_ts": 0.0,
}
stats_lock  = threading.Lock()
stop_event  = threading.Event()


def signal_handler(sig, frame):
    print("\n[INFO] 收到 Ctrl+C，停止发包...")
    stop_event.set()

signal.signal(signal.SIGINT,  signal_handler)
signal.signal(signal.SIGTERM, signal_handler)


# ──────────────────────────────────────────────
# CPU 绑核
# ──────────────────────────────────────────────
def pin_thread_to_cpu(cpu_id):
    """把当前线程绑定到指定 CPU 核。Windows/Linux 均支持。"""
    try:
        import psutil
        p = psutil.Process(os.getpid())
        # 获取当前线程 id（Windows 用 native id）
        tid = threading.get_native_id()
        # psutil 只支持进程级别绑核，线程级需要平台 API
        # Windows: 用 ctypes
        if sys.platform == "win32":
            import ctypes
            ctypes.windll.kernel32.SetThreadAffinityMask(
                ctypes.windll.kernel32.GetCurrentThread(),
                1 << cpu_id
            )
            return True
        else:
            # Linux: os.sched_setaffinity
            os.sched_setaffinity(0, {cpu_id})
            return True
    except Exception as e:
        print(f"[WARN] 绑核失败 (CPU {cpu_id}): {e}")
        return False


def get_cpu_count():
    try:
        return os.cpu_count() or 1
    except Exception:
        return 1


# ──────────────────────────────────────────────
# Windows NPF → 友好名称映射
# ──────────────────────────────────────────────
def get_npf_to_friendly():
    mapping = {}
    try:
        import winreg
        base = (r"SYSTEM\CurrentControlSet\Control"
                r"\Network\{4D36E972-E325-11CE-BFC1-08002BE10318}")
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base) as net_key:
            i = 0
            while True:
                try:
                    guid = winreg.EnumKey(net_key, i)
                    i += 1
                    try:
                        conn_path = f"{base}\\{guid}\\Connection"
                        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                            conn_path) as conn_key:
                            name, _ = winreg.QueryValueEx(conn_key, "Name")
                            mapping[f"\\Device\\NPF_{guid}"] = name
                    except FileNotFoundError:
                        pass
                except OSError:
                    break
    except ImportError:
        pass
    return mapping


def get_iface_ips():
    ips = {}
    try:
        from scapy.arch import get_if_addr
        for iface in get_if_list():
            try:
                addr = get_if_addr(iface)
                if addr and addr != "0.0.0.0":
                    ips[iface] = addr
            except Exception:
                pass
    except Exception:
        pass
    return ips


def list_ifaces_friendly():
    npf_map = get_npf_to_friendly()
    ip_map  = get_iface_ips()
    ifaces  = get_if_list()
    col_w   = max((len(i) for i in ifaces), default=20) + 2
    print(f"\n{'NPF 设备名':<{col_w}}  {'友好名称':<35}  {'IPv4'}")
    print("-" * (col_w + 60))
    for iface in ifaces:
        friendly = npf_map.get(iface, "（未知）")
        ip       = ip_map.get(iface, "")
        print(f"{iface:<{col_w}}  {friendly:<35}  {ip}")
    print()
    print(f"可用 CPU 核数: {get_cpu_count()}")
    print('例如: --iface "VMware Network Adapter VMnet10" --threads 4 --cpus 0,1,2,3')


def resolve_iface(name):
    if name.startswith("\\Device\\NPF_") or name.startswith(r"\Device\NPF_"):
        return name
    npf_map = get_npf_to_friendly()
    for npf, friendly in npf_map.items():
        if friendly.lower() == name.lower():
            return npf
    return name


# ──────────────────────────────────────────────
# 统计打印线程
# ──────────────────────────────────────────────
def stats_printer(nthreads):
    prev_pkts  = 0
    prev_bytes = 0
    prev_time  = time.time()

    print(f"\n{'Timestamp':<22} {'TX PPS':>10} {'TX Mbps':>10} "
          f"{'Total Pkts':>12} {'Total MB':>10}")
    print("-" * 70)

    while not stop_event.is_set():
        time.sleep(1.0)
        now         = time.time()
        cur_pkts    = stats["tx_pkts"]
        cur_bytes   = stats["tx_bytes"]
        delta_t     = now - prev_time
        delta_pkts  = cur_pkts  - prev_pkts
        delta_bytes = cur_bytes - prev_bytes
        pps  = delta_pkts  / delta_t if delta_t > 0 else 0
        mbps = (delta_bytes * 8) / delta_t / 1e6 if delta_t > 0 else 0
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        print(f"{ts:<22} {pps:>10.0f} {mbps:>10.3f} "
              f"{cur_pkts:>12,} {cur_bytes/1e6:>10.2f}")
        prev_pkts  = cur_pkts
        prev_bytes = cur_bytes
        prev_time  = now

    elapsed = time.time() - stats["start_ts"]
    if elapsed > 0:
        avg_pps  = stats["tx_pkts"]  / elapsed
        avg_mbps = (stats["tx_bytes"] * 8) / elapsed / 1e6
        print("-" * 70)
        print(f"[汇总] 共发 {stats['tx_pkts']:,} 包 | "
              f"耗时 {elapsed:.2f}s | "
              f"平均 {avg_pps:.0f} PPS | "
              f"平均 {avg_mbps:.3f} Mbps")


# ──────────────────────────────────────────────
# 单线程发包循环
# ──────────────────────────────────────────────
def send_worker(thread_id, cpu_id, iface, pkt_bytes, rate, burst):
    """
    每个线程独立持有一个 L2pcapSocket。
    rate=0 全速；rate>0 为该线程的目标 PPS。
    """
    # 绑核
    if cpu_id >= 0:
        ok = pin_thread_to_cpu(cpu_id)
        status = f"CPU {cpu_id}" if ok else "绑核失败"
    else:
        status = "不绑核"
    print(f"[Thread-{thread_id}] 启动，{status}")

    # 每线程独立 socket
    try:
        sock = L2SocketClass(iface=iface)
    except Exception as e:
        print(f"[Thread-{thread_id}] socket 创建失败: {e}，降级用 sendp")
        sock = None

    pkt_len  = len(pkt_bytes)
    interval = (1.0 / rate) * burst if rate > 0 else 0

    try:
        while not stop_event.is_set():
            t0 = time.perf_counter()

            if sock is not None:
                for _ in range(burst):
                    sock.send(pkt_bytes)
            else:
                from scapy.all import sendp
                from scapy.all import Ether
                # 重建 pkt 对象（sendp 需要）
                sendp(Ether(pkt_bytes), iface=iface,
                      count=burst, verbose=False)

            with stats_lock:
                stats["tx_pkts"]  += burst
                stats["tx_bytes"] += burst * pkt_len

            if interval > 0:
                elapsed = time.perf_counter() - t0
                sleep_t = interval - elapsed
                if sleep_t > 0:
                    time.sleep(sleep_t)
    finally:
        if sock is not None:
            sock.close()
        print(f"[Thread-{thread_id}] 已退出")


# ──────────────────────────────────────────────
# 参数解析
# ──────────────────────────────────────────────
def parse_args():
    p = argparse.ArgumentParser(
        description="Scapy 打流脚本 — 多线程绑核版，配合 DPDK l2fwd"
    )
    p.add_argument("--iface",       default=None,
                   help="发包网卡（友好名称或 NPF 设备名）")
    p.add_argument("--dst-mac",     default="ff:ff:ff:ff:ff:ff")
    p.add_argument("--src-mac",     default=None)
    p.add_argument("--src-ip",      default="192.168.20.100")
    p.add_argument("--dst-ip",      default="192.168.30.1")
    p.add_argument("--src-port",    type=int, default=1234)
    p.add_argument("--dst-port",    type=int, default=5678)
    p.add_argument("--pkt-size",    type=int, default=64,
                   help="包大小（字节，含以太网头，最小 64）")
    p.add_argument("--rate",        type=int, default=0,
                   help="总目标 PPS（0=全速，各线程均分）")
    p.add_argument("--count",       type=int, default=0,
                   help="发包总数（0=无限）")
    p.add_argument("--burst",       type=int, default=64,
                   help="每轮发包数")
    p.add_argument("--threads",     type=int, default=1,
                   help="发包线程数（建议 1-4）")
    p.add_argument("--cpus",        default=None,
                   help="绑核列表，逗号分隔，如 0,1,2,3（不填则不绑核）")
    p.add_argument("--list-ifaces", action="store_true",
                   help="列出所有网卡并退出")
    return p.parse_args()


def build_pkt(args):
    src_mac = args.src_mac
    if src_mac is None:
        try:
            src_mac = get_if_hwaddr(args.iface)
        except Exception:
            src_mac = "00:00:00:00:00:01"
    payload_size = max(0, args.pkt_size - 14 - 20 - 8)
    pkt = (
        Ether(src=src_mac, dst=args.dst_mac) /
        IP(src=args.src_ip, dst=args.dst_ip) /
        UDP(sport=args.src_port, dport=args.dst_port) /
        (b"X" * payload_size)
    )
    return raw(pkt)


# ──────────────────────────────────────────────
# 入口
# ──────────────────────────────────────────────
def main():
    args = parse_args()

    if args.list_ifaces:
        list_ifaces_friendly()
        sys.exit(0)

    if args.iface is None:
        print("[ERROR] 请指定 --iface，用 --list-ifaces 查看可用网卡")
        sys.exit(1)

    args.iface = resolve_iface(args.iface)
    pkt_bytes  = build_pkt(args)

    # 解析绑核列表
    cpu_ids = []
    if args.cpus:
        try:
            cpu_ids = [int(x.strip()) for x in args.cpus.split(",")]
        except ValueError:
            print("[ERROR] --cpus 格式错误，示例: 0,1,2,3")
            sys.exit(1)

    # 每线程分配 cpu_id（不够则 -1 表示不绑）
    thread_cpus = []
    for i in range(args.threads):
        thread_cpus.append(cpu_ids[i] if i < len(cpu_ids) else -1)

    # 每线程分配 rate
    per_thread_rate = (args.rate // args.threads) if args.rate > 0 else 0

    total_cpus = get_cpu_count()
    print("=" * 60)
    print(f"  接口      : {args.iface}")
    print(f"  {args.src_ip}:{args.src_port} → {args.dst_ip}:{args.dst_port}")
    print(f"  包大小    : {len(pkt_bytes)} bytes")
    print(f"  目标      : {'全速' if args.rate == 0 else str(args.rate) + ' PPS'}")
    print(f"  线程数    : {args.threads}  (系统共 {total_cpus} 核)")
    print(f"  线程→CPU  : {dict(enumerate(thread_cpus))}")
    print(f"  burst     : {args.burst}")
    print(f"  socket    : {L2SocketClass.__name__}")
    print("=" * 60)
    print("按 Ctrl+C 停止\n")

    stats["start_ts"] = time.time()

    # 启动统计线程
    t_stats = threading.Thread(
        target=stats_printer, args=(args.threads,), daemon=True)
    t_stats.start()

    # 启动发包线程
    workers = []
    for i in range(args.threads):
        t = threading.Thread(
            target=send_worker,
            args=(i, thread_cpus[i], args.iface,
                  pkt_bytes, per_thread_rate, args.burst),
            daemon=True,
        )
        workers.append(t)

    for t in workers:
        t.start()
    for t in workers:
        t.join()

    stop_event.set()
    t_stats.join(timeout=2)


if __name__ == "__main__":
    main()