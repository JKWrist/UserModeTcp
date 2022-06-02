# UserModeTcp

### 一、介绍

本项目是基于netmap，在用户态实现的网络协议栈，在用户空间实现epoll

#### 背景

C10M问题(并发C10兆)，在千万并发时，2次拷贝，存在过大的开销

- 1、客户端的数据，先从网卡，copy到 内核协议栈(TCP/IP协议栈,bsd)
- 2、从内核协议栈，copy到应用程序

把内核的协议栈，做到应用程序，目的是为了减少一次拷贝，即：从网卡直接拷贝到应用程序，中间不经过先拷贝到内核。

#### 千万并发解决方案

基于netmap 实现用户态协议栈，netmap接管网卡eth0
- 1、netmap会接管网卡eth0上的数据，直接将网卡上的数据mmap到内存中
- 2、数据就不会经过内核了，应用层就可以直接从内存中读取数据

### 二、软件架构说明

netmap, dpdk, pf_ring, Tcp Stack for Userspace

```
------- Apps(Nginx Redis，lighttpd.）---------
----------------------------------------------
----- Posix API ---- Epoll ---- Coroutine ----
----------------------------------------------
----------------- TCP/IP Stack ---------------
----------------------------------------------
------- Netmap ---- DPDK ------ PF_RING ------
----------------------------------------------
-------------------- NIC ---------------------
```

### 三、安装教程

#### netmap 安装
```
1.  git clone https://gitee.com/xujunze/netmap.git
2.  ./configure
3.  make
3.  sudo make install
```

#### netmap 安装中的问题

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

### 四、用户态协议栈编译

1. 编译前:
```
$ sudo apt-get install libhugetlbfs-dev
```

2. 更新 include/nty_config.h
```
#define NTY_SELF_IP		"192.168.0.106" 	//your ip
#define NTY_SELF_IP_HEX	0x6A00A8C0 			//your ip hex.
#define NTY_SELF_MAC	"00:0c:29:58:6f:f4" //your mac
```

3. 更新 src/nty_eth.c
```
int ret = nty_nic_init(tctx, "netmap:wlan0");  //your deviece name
```

4. 编译:
```
$ make
```

### 五、使用说明
1. 阻塞服务端运行:
```
$ ./bin/nty_example_block_server
```

2. epoll 服务端运行:
```
$ ./bin/nty_example_epoll_rb_server
```


### Reference
* [Level-IP](https://github.com/saminiir/level-ip) and [saminiir blog](http://www.saminiir.com/)
* [Linux kernel TCP/IP stack](https://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/tree/net/ipv4)
* [NtyTcp](https://github.com/wangbojing/NtyTcp)
