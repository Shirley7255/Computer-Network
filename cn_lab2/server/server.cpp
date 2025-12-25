/*
功能：实现基于UDP的可靠文件接收服务器
特性：
    1. 连接管理：三次握手接受连接
    2. 差错检测：校验和验证
    3. 选择确认：支持乱序接收，缓存乱序包
    4. 流量控制：发送ACK通知客户端
 */

#include "common.h"
#include <fstream>
#include <map>
#include <algorithm>
#include <chrono>

/*
错误处理函数
@param message 错误信息
 */
void die(const char* message) {
    perror(message);
    exit(1);
}

/*
 流程：
    1. 初始化并绑定UDP套接字
    2. 三次握手接受连接
    3. 接收数据包，处理乱序
    4. 写入文件
    5. 发送ACK确认
    6. 四次挥手关闭连接
 */
int main() {
    // ========== 初始化Winsock ==========
    if (!initialize_winsock()) {
        return 1;
    }

    SOCKET server_socket;
    sockaddr_in server_addr, client_addr;
    int client_addr_len = sizeof(client_addr);

    // ========== 创建UDP套接字 ==========
    if ((server_socket = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET) {
        die("Could not create socket");
    }

    // ========== 准备服务器地址结构 ==========
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;  // 监听所有网络接口
    server_addr.sin_port = htons(SERVER_PORT);  // 绑定到8888端口

    // ========== 绑定套接字 ==========
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        die("Bind failed");
    }
    std::cout << "Server listening on port " << SERVER_PORT << std::endl;

    // ========== 三次握手接受连接 ==========
    Packet recv_packet, send_packet;
    std::cout << "Waiting for SYN..." << std::endl;
    
    // 第一步：接收客户端的SYN
    recvfrom(server_socket, (char*)&recv_packet, MAX_BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, &client_addr_len);
    if (recv_packet.flags & SYN) {
        std::cout << "SYN received. Sending SYN-ACK..." << std::endl;
        
        // 第二步：发送SYN-ACK
        send_packet = { 0 };
        send_packet.flags = SYN | ACK;// 同时设置SYN和ACK标志
        send_packet.ack_num = recv_packet.seq_num + 1;
        send_packet.checksum = calculate_checksum(&send_packet);
        sendto(server_socket, (const char*)&send_packet, HEADER_SIZE, 0, (struct sockaddr*)&client_addr, client_addr_len);

        // 第三步：接收最终ACK
        recvfrom(server_socket, (char*)&recv_packet, MAX_BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, &client_addr_len);
        if (recv_packet.flags & ACK) {
            std::cout << "Connection established." << std::endl;
        }
    }

    // ========== 文件接收逻辑 ==========
    std::ofstream output_file("received_file", std::ios::binary);  // 以二进制模式写入文件
    uint32_t expected_seq_num = 1;  // 期望接收的序列号（从1开始）
    std::map<uint32_t, Packet> receive_buffer; // 乱序包缓冲区：<序列号, 数据包>
    uint32_t total_packets_received = 0;  // 总接收包数
    uint32_t out_of_order_packets = 0;    // 乱序包数量
    auto start_time = std::chrono::high_resolution_clock::now();  // 记录开始时间

    while (true) {
        // 接收数据包
        int recv_len = recvfrom(server_socket, (char*)&recv_packet, MAX_BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, &client_addr_len);
        if (recv_len <= 0) continue;  // 接收失败，跳过

        // ========== 步骤1：验证校验和 ==========
        if (!verify_checksum(&recv_packet)) {
            std::cerr << "Corrupt packet received, discarding." << std::endl;
            continue;  // 数据包损坏，丢弃
        }

        // ========== 步骤2：检查FIN标志（连接关闭）==========
        if (recv_packet.flags & FIN) {
            std::cout << "FIN received. Sending ACK and closing." << std::endl;
            
            // 发送FIN-ACK
            send_packet = { 0 };
            send_packet.flags = ACK | FIN;
            send_packet.ack_num = recv_packet.seq_num + 1;
            send_packet.checksum = calculate_checksum(&send_packet);
            sendto(server_socket, (const char*)&send_packet, HEADER_SIZE, 0, (struct sockaddr*)&client_addr, client_addr_len);
            
            // 计算并输出接收统计
            auto end_time = std::chrono::high_resolution_clock::now();
            double duration_s = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count() / 1e6;
            std::cout << "\n--- Reception Summary ---" << std::endl;
            std::cout << "Total packets received: " << total_packets_received << std::endl;
            std::cout << "Out-of-order packets: " << out_of_order_packets << std::endl;
            std::cout << "Reception time: " << duration_s << " seconds" << std::endl;
            
            break;  // 退出接收循环
        }

        total_packets_received++;

        // ========== 步骤3：选择确认逻辑 ==========
        
        // 情况1：收到期望的数据包（按序到达）
        if (recv_packet.seq_num == expected_seq_num) {
            // 写入数据到文件
            output_file.write(recv_packet.data, recv_packet.data_len);
            expected_seq_num++;  // 期望序列号+1

            // 检查缓冲区中是否有连续的后续数据包
            while (receive_buffer.count(expected_seq_num)) {  // count()检查map中是否存在expected_seq_num这个键，存在返回1，不存在返回0
                Packet buffered_packet = receive_buffer[expected_seq_num];  // 从缓存中取出该序列号对应的数据包
                output_file.write(buffered_packet.data, buffered_packet.data_len);  // 将缓存的数据包写入文件
                receive_buffer.erase(expected_seq_num);  // 从缓冲区删除已写入的数据包释放内存
                expected_seq_num++;  // 期望序列号+1，继续检查下一个连续的包
            }  // 循环结束条件：缓存中不存在expected_seq_num，说明出现了间隙
        }
        // 情况2：收到未来的数据包（乱序到达）
        else if (recv_packet.seq_num > expected_seq_num) {
            // 将乱序包存入缓冲区，等待前面的包到达
            receive_buffer[recv_packet.seq_num] = recv_packet;
            out_of_order_packets++;
        }
        // 情况3：收到重复的旧数据包（seq_num < expected_seq_num）
        // 直接忽略，仍然发送ACK

        // ========== 步骤4：发送ACK确认 ==========
        // 始终发送当前已按序接收的最高序列号
        std::cout << "Received SEQ=" << recv_packet.seq_num << ". Sending ACK for SEQ=" << expected_seq_num - 1 << std::endl;
        send_packet = { 0 };
        send_packet.flags = ACK;
        send_packet.ack_num = expected_seq_num - 1;  // ACK = 已按序接收的最高序列号
        send_packet.checksum = calculate_checksum(&send_packet);
        sendto(server_socket, (const char*)&send_packet, HEADER_SIZE, 0, (struct sockaddr*)&client_addr, client_addr_len);
    }

    output_file.close();
    std::cout << "File received successfully." << std::endl;

    closesocket(server_socket);
    cleanup_winsock();
    return 0;
}
