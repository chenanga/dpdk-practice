编译：
gcc rss_verify.c -o rss_verify \
    $(pkg-config --cflags --libs libdpdk) \
    -Wl,--whole-archive -lrte_net_i40e -Wl,--no-whole-archive

运行前准备：
# port0 绑 DPDK，port1 还给内核
sudo dpdk-devbind.py -b vfio-pci 0000:01:00.0
sudo dpdk-devbind.py -b i40e    0000:01:00.1
sudo ip link set enp1s0f1np1 up

运行：
sudo ./rss_verify -l 0-4 -n 4 --


另一个终端发包：

```
sudo python3 -c "
from scapy.all import *
for sport in [1001, 1002, 1003, 1004]:
    pkt = Ether(dst='6C:92:BF:68:E1:E3') / IP(src='1.1.1.1', dst='2.2.2.2') / TCP(sport=sport, dport=80)
    sendp(pkt * 100, iface='enp1s0f1np1', verbose=False)
print('发包完成')"
```


```
sudo python3 -c "
from scapy.all import *
pkt = Ether(dst='6C:92:BF:68:E1:E3') / IP(src='1.1.1.1', dst='2.2.2.2') / TCP(sport=1001, dport=80)
sendp(pkt * 500, iface='enp1s0f1np1', verbose=False)
print('发包完成')
"
```