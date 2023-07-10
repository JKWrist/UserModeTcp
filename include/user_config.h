#ifndef __USER_CONFIG_H__
#define __USER_CONFIG_H__

#define USER_SELF_IP        "192.168.0.107"//"192.168.1.108" //"192.168.1.132" //"192.168.1.131"  //
#define USER_SELF_IP_HEX    0x6B00A8C0 //0x8301A8C0 //
#define USER_SELF_MAC        "00:0c:29:58:6f:f4"

#define USER_MAX_CONCURRENCY        1024
#define USER_SNDBUF_SIZE            8192
#define USER_RCVBUF_SIZE            8192
#define USER_MAX_NUM_BUFFERS        1024
#define USER_BACKLOG_SIZE            1024

#define USER_ENABLE_MULTI_NIC        0
#define USER_ENABLE_BLOCKING        1

#define USER_ENABLE_EPOLL_RB        1
#define USER_ENABLE_SOCKET_C10M        1
#define USER_ENABLE_POSIX_API        1


#define USER_SOCKFD_NR                (1024*1024)
#define USER_BITS_PER_BYTE            8

//#define USER_DEBUG 1
#ifdef USER_DEBUG
#define userdbg(format, ...) 			fprintf(stdout, " [File:"__FILE__", line:%05d] : "format, __LINE__, ##__VA_ARGS__)
#define user_trace_api(format, ...) 	fprintf(stdout, " [File:"__FILE__", line:%05d] : "format, __LINE__, ##__VA_ARGS__)
#define user_trace_tcp(format, ...) 	fprintf(stdout, " [File:"__FILE__", line:%05d] : "format, __LINE__, ##__VA_ARGS__)
#define user_trace_buffer(format, ...) 	fprintf(stdout, " [File:"__FILE__", line:%05d] : "format, __LINE__, ##__VA_ARGS__)
#define user_trace_eth(format, ...) 	fprintf(stdout, " [File:"__FILE__", line:%05d] : "format, __LINE__, ##__VA_ARGS__)
#define user_trace_ip(format, ...) 		fprintf(stdout, " [File:"__FILE__", line:%05d] : "format, __LINE__, ##__VA_ARGS__)
#define user_trace_timer(format, ...) 	fprintf(stdout, " [File:"__FILE__", line:%05d] : "format, __LINE__, ##__VA_ARGS__)
#define user_trace_epoll(format, ...)	fprintf(stdout, " [File:"__FILE__", line:%05d] : "format, __LINE__, ##__VA_ARGS__)
#define user_trace_socket(format, ...)	fprintf(stdout, " [File:"__FILE__", line:%05d] : "format, __LINE__, ##__VA_ARGS__)

#else

#define userdbg(format, ...)
#define user_trace_api(format, ...)
#define user_trace_tcp(format, ...)
#define user_trace_buffer(format, ...)
#define user_trace_eth(format, ...)
#define user_trace_ip(format, ...)
#define user_trace_timer(format, ...)
#define user_trace_epoll(format, ...)
#define user_trace_socket(format, ...)

#endif

#define UNUSED(expr)    do {(void)(expr); } while(0)

#endif