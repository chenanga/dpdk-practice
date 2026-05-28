运行命令 sudo ./mempool_cache_test -l 0-3 -n 4 --socket-mem 512 --log-level=mempool:debug

EAL: Detected CPU lcores: 6
EAL: Detected NUMA nodes: 1
EAL: Detected shared linkage of DPDK
EAL: Multi-process socket /var/run/dpdk/rte/mp_socket
EAL: Selected IOVA mode 'PA'
EAL: VFIO support initialized

========================================================
  mempool cache 性能实验
  总 lcore 数=4  (main + 3 workers)
  NUM_OBJS=65536  OBJ_SIZE=64  CACHE_SIZE=256
  WARMUP=2000  TEST_ITER=200000
========================================================

[实验] no cache  (cache_size=0)    (lcore 数=3)
─────────────────────────────────────────────────────────
  lcore 2   alloc= 517 cy  free= 696 cy  total=1213 cy
  lcore 1   alloc= 534 cy  free= 709 cy  total=1243 cy
  lcore 0   alloc= 510 cy  free= 693 cy  total=1203 cy
  lcore 3   alloc= 522 cy  free= 703 cy  total=1225 cy
─────────────────────────────────────────────────────────
  平均 alloc : 520 cycles
  平均 free  : 700 cycles
  平均总计   : 1221 cycles

[实验] with cache(cache_size=256)  (lcore 数=3)
─────────────────────────────────────────────────────────
  lcore 1   alloc=  39 cy  free=  44 cy  total=  83 cy
  lcore 2   alloc=  40 cy  free=  41 cy  total=  81 cy
  lcore 3   alloc=  38 cy  free=  44 cy  total=  82 cy
  lcore 0   alloc=  40 cy  free=  41 cy  total=  81 cy
─────────────────────────────────────────────────────────
  平均 alloc : 39 cycles
  平均 free  : 42 cycles
  平均总计   : 81 cycles

─────────────────────────────────────────────────────────
  汇总对比表（平均 cycles / op）
─────────────────────────────────────────────────────────
  场景                             alloc      free  alloc+free
  ──────────────────────────────  ────────  ────────  ──────────
  见上方各 lcore 输出            ↑       ↑       ↑
─────────────────────────────────────────────────────────

  结论提示：
  - 单线程：cache 通常快 10~20 cycles（少了 ring 函数开销）
  - 多线程：no-cache 因 CAS 竞争可达 300+ cycles；
            cache 版本稳定在 ~15 cycles，差距 10x~30x
