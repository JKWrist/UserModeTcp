# UserModeTcp

#### Description
用户态协议栈

### 软件架构说明

```
------- Apps(Nginx Redis，lighttpd.）---------
----------------------------------------------
----- Posix API ---- Epoll ---- Coroutine ----
----------------------------------------------
----------------- TCP/IP Stack ---------------
----------------------------------------------
------ UserModeTcp --- DPDK ----- PF_RING ----
----------------------------------------------
-------------------- NIC ---------------------
```

### 安装教程

#### netmap install
```
1.  git clone https://gitee.com/xujunze/netmap.git
2.  ./configure
3.  make
3.  sudo make install
```

#### netmap install complete.

1.  problem : configure --> /bin/sh^M.
```
you should run . 
$ dos2unix configure
$ dos2unix ./LINUX/configure
```

2.  problem : cannot stat 'bridge': No such or directory
```
$ make clean
$ cd build-apps/bridge
$ gcc -O2 -pipe -Werror -Wall -Wunused-function -I ../../sys -I ../../apps/include -Wextra    ../../apps/bridge/bridge.c  -lpthread -lrt    -o bridge
$ sudo make && make install
```

#### netmap install complete.
netmap, dpdk, pf_ring, Tcp Stack for Userspace

1. compile:
```
$ sudo apt-get install libhugetlbfs-dev
$ make
```

2. update include/nty_config.h
```
#define NTY_SELF_IP		"192.168.0.106" 	//your ip
#define NTY_SELF_IP_HEX	0x6A00A8C0 			//your ip hex.
#define NTY_SELF_MAC	"00:0c:29:58:6f:f4" //your mac
```

3. update src/nty_eth.c
```
int ret = nty_nic_init(tctx, "netmap:wlan0");  //your deviece name
```

### 使用说明
1. block server run:
```
$ ./bin/nty_example_block_server
```

2. epoll server run:
```
$ ./bin/nty_example_epoll_rb_server
```


### Reference
* [Level-IP](https://github.com/saminiir/level-ip) and [saminiir blog](http://www.saminiir.com/)
* [Linux kernel TCP/IP stack](https://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/tree/net/ipv4)
* [NtyTcp]https://github.com/wangbojing/NtyTcp
