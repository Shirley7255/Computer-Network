/*
 common.h - 公共头文件
 定义了客户端和服务器端共同使用的协议常量、数据结构和工具函数
 实现了基于UDP的可靠数据传输协议的基础组件
 */

#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <cstdint>
#include <chrono>

// Windows套接字相关头文件
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")  // 链接Winsock库
#else
    
#endif

// ========== 协议常量定义 ==========
const int SERVER_PORT = 8888;  // 服务器真实监听端口
const int ROUTER_PORT = 12345; // Router模拟器端口（客户端连接此端口进行测试）
const int MAX_BUFFER_SIZE = 1500; // 最大缓冲区大小，对应以太网MTU
const int HEADER_SIZE = 20;    // 数据包头部大小（字节）
const int MAX_DATA_SIZE = MAX_BUFFER_SIZE - HEADER_SIZE;  // 每个数据包最大数据载荷：1480字节
const int FLOW_CONTROL_WINDOW_SIZE = 64; // 流量控制窗口大小（固定），用于限制未确认数据包数量
const int PACKET_TIMEOUT_MS = 1000; // 数据包超时重传时间（毫秒），1秒适合大文件传输

// ========== 数据包标志位定义 ==========
// 使用位移操作定义标志位，方便进行按位或操作组合多个标志
enum Flags {
    SYN = 1 << 0, // 同步标志 (值=1)，用于建立连接
    ACK = 1 << 1, // 确认标志 (值=2)，用于确认收到数据
    FIN = 1 << 2, // 结束标志 (值=4)，用于关闭连接
};

// ========== 数据包结构定义 ==========

#pragma pack(push, 1)//确保结构体按1字节对齐，避免编译器自动填充字节
struct Packet {
    uint32_t seq_num;     // 序列号：标识数据包的顺序，从1开始
    uint32_t ack_num;     // 确认号：期望接收的下一个序列号
    uint16_t flags;       // 标志位：SYN/ACK/FIN的组合
    uint16_t window_size; // 窗口大小：用于流量控制
    uint16_t data_len;    // 数据长度：实际数据载荷的字节数
    uint16_t checksum;    // 校验和：用于差错检测
    char data[MAX_DATA_SIZE];  // 数据载荷：最多1480字节
};
#pragma pack(pop)  // 恢复默认对齐方式


// ========== 工具函数 ==========

/*
 calculate_checksum - 计算16位校验和
 功能：对数据包进行校验和计算，用于差错检测
 算法：按16位字累加，然后对进位进行折叠，最后取反
 @param packet 指向待计算校验和的数据包
 @return 计算得到的16位校验和
 */
uint16_t calculate_checksum(const Packet* packet) {
    uint32_t sum = 0;
    const uint16_t* p = reinterpret_cast<const uint16_t*>(packet);
    int size = HEADER_SIZE + packet->data_len;  // 计算校验和的范围：头部+数据

    // 临时将校验和字段设为0,计算时必须排除校验和字段本身
    uint16_t original_checksum = packet->checksum;
    const_cast<Packet*>(packet)->checksum = 0;

    // 按16位累加所有数据
    while (size > 1) {
        sum += *p++;
        size -= 2;
    }

    // 如果有剩余的奇数字节，将其作为8位数据加入
    if (size > 0) {
        sum += *reinterpret_cast<const uint8_t*>(p);
    }

    // 将32位和折叠为16位：高16位加到低16位
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    // 恢复原始校验和值（不改变输入参数）
    const_cast<Packet*>(packet)->checksum = original_checksum;

    // 返回反码作为校验和
    return static_cast<uint16_t>(~sum);
}

/*
 verify_checksum - 验证数据包校验和
 @param packet 指向待验证的数据包
 @return true表示校验和正确，false表示数据包损坏
 */
bool verify_checksum(const Packet* packet) {  
    return calculate_checksum(packet) == packet->checksum;
}

/*
 initialize_winsock - 初始化Windows套接字库
 Windows平台使用Socket前必须调用此函数初始化Winsock
 @return true表示初始化成功，false表示失败
 */
bool initialize_winsock() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return false;
    }
#endif
    return true;
}

/*
 cleanup_winsock - 清理Windows套接字库
 程序结束时调用，释放Winsock资源
 */
void cleanup_winsock() {
#ifdef _WIN32
    WSACleanup();
#endif
}