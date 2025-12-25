/*
 client.cpp - 客户端程序
 
 功能：基于UDP的可靠文件传输客户端
 特性：
 1. 连接管理：三次握手建立连接
 2. 差错检测：校验和机制
 3. 确认重传：选择确认(SACK) + 超时重传
 4. 流量控制：固定窗口大小
 5. 拥塞控制：TCP RENO算法（慢启动、拥塞避免、快速恢复）
 */

#include "common.h"
#include <vector>
#include <fstream>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <condition_variable>
#include <algorithm> 

// ========== 拥塞控制状态定义 ==========
enum CongestionState {
    SLOW_START,           // 慢启动：指数增长cwnd
    CONGESTION_AVOIDANCE, // 拥塞避免：线性增长cwnd
    FAST_RECOVERY         // 快速恢复：收到3个重复ACK后进入
};

// ========== 发送窗口中数据包的状态 ==========
struct PacketState {
    Packet packet;        // 数据包内容
    std::chrono::steady_clock::time_point send_time;  // 发送时间用于超时判断
    bool acked = false;   // 是否已被确认
};

// ========== 线程间共享状态（需要互斥锁保护）==========
std::mutex window_mutex;  // 保护发送窗口的互斥锁
std::map<uint32_t, PacketState> send_window;  // 发送窗口：<序列号, 数据包状态>
uint32_t send_base = 1;         // 发送窗口基序号（最小未确认序列号）
uint32_t next_seq_num = 1;      // 下一个要发送的序列号
std::atomic<bool> transmission_complete(false);  // 传输完成标志（原子变量，线程安全）
std::condition_variable retransmit_cv;  // 条件变量：用于快速重传信号
uint32_t retransmit_seq_num = 0;  // 需要快速重传的序列号（0表示无）

// ========== TCP RENO拥塞控制变量 ==========
double cwnd = 1.0;               // 拥塞窗口（单位：数据包数）因为线性增长会加1/cwnd所以用double
uint32_t ssthresh = 16;          // 慢启动阈值
CongestionState state = SLOW_START;  // 当前拥塞控制状态
int duplicate_ack_count = 0;    // 重复ACK计数（收到3个触发快速重传）

// ========== 统计信息 ==========
std::atomic<uint32_t> total_packets_sent(0);      // 总发送包数
std::atomic<uint32_t> total_retransmissions(0);   // 总重传次数
std::atomic<uint32_t> total_acks_received(0);     // 总接收ACK数

// ========== 全局套接字和地址 ==========
SOCKET client_socket;      // 客户端UDP套接字
sockaddr_in server_addr;   // 服务器地址结构

/*
 ACK接收线程
 功能：独立线程持续接收服务器返回的ACK，并根据TCP RENO算法更新拥塞窗口
 关键逻辑：
  1. 新ACK（ack_num >= send_base）：更新窗口，调整cwnd
  2. 重复ACK（ack_num < send_base）：累计计数，3次触发快速重传
  3. 拥塞控制状态转换
 */
void receive_acks() {
    Packet ack_packet;
    int recv_len;
    // 循环直到传输完成且发送窗口为空
    while (!transmission_complete || !send_window.empty()) {
        // 接收ACK数据包（阻塞调用）
        recv_len = recvfrom(client_socket, (char*)&ack_packet, MAX_BUFFER_SIZE, 0, NULL, NULL);
        if (recv_len > 0) {
            // 验证校验和，确保ACK未损坏
            if (verify_checksum(&ack_packet) && ack_packet.flags & ACK) {
                std::lock_guard<std::mutex> lock(window_mutex);  // 加锁保护共享数据
                total_acks_received++;

                uint32_t acked_num = ack_packet.ack_num;  // 确认号
                std::cout << "ACK received for SEQ=" << acked_num << std::endl;

                // ========== TCP RENO拥塞控制核心逻辑 ==========
                if (acked_num >= send_base) { 
                    // ===== 情况1：收到新ACK（确认了新数据）=====
                    duplicate_ack_count = 0;  // 重置重复ACK计数
                    send_base = acked_num + 1;  // 更新窗口基序号

                    // 从发送窗口中移除所有已确认的数据包
                    for (auto it = send_window.begin(); it != send_window.end(); ) {
                        if (it->first <= acked_num) {
                            it = send_window.erase(it);  // 删除已确认的包
                        }
                        else {
                            ++it;
                        }
                    }

                    // === 根据当前状态调整拥塞窗口 ===
                    if (state == FAST_RECOVERY) {
                        // 快速恢复完成，进入拥塞避免
                        state = CONGESTION_AVOIDANCE;
                        cwnd = ssthresh;  // 将窗口收缩到阈值
                    }
                    else if (state == SLOW_START) {
                        // 慢启动阶段：指数增长
                        cwnd += 1;
                        if (cwnd >= ssthresh) {
                            state = CONGESTION_AVOIDANCE;  // 达到阈值，切换到拥塞避免
                        }
                    }
                    else if (state == CONGESTION_AVOIDANCE) {
                        // 拥塞避免阶段：线性增长（每个RTT增加1个包）
                        cwnd += 1.0 / cwnd;
                    }
                }
                else { 
                    // ===== 情况2：收到重复ACK（确认号小于send_base）=====
                    duplicate_ack_count++;// 因为是重复ACK，计数加1
                    
                    if (state == FAST_RECOVERY) {
                        // 快速恢复期间，每个重复ACK增加窗口（膨胀窗口）
                        cwnd += 1;
                    }
                    else if (duplicate_ack_count == 3) {
                        // 收到3个重复ACK，触发快速重传和快速恢复
                        state = FAST_RECOVERY;
                        ssthresh = (std::max)(2.0, cwnd / 2.0);  // 阈值设为窗口的一半
                        cwnd = ssthresh + 3;  // 窗口设为阈值+3

                        // 通知主线程进行快速重传
                        retransmit_seq_num = send_base;
                        retransmit_cv.notify_one();
                    }
                }
            }
        }
    }
}


/*
 @param argc 命令行参数个数
 @param argv 命令行参数数组：argv[1]=服务器IP, argv[2]=文件路径
 流程：
    1. 初始化套接字
    2. 三次握手建立连接
    3. 读取文件到内存
    4. 启动ACK接收线程
    5. 主发送循环（滑动窗口 + 超时重传）
    6. 四次挥手关闭连接
    7. 输出传输统计
 */
int main(int argc, char* argv[]) {
    // ========== 参数检查 （终端情况下使用）==========
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <file_path>" << std::endl;
        return 1;
    }
    const char* server_ip = argv[1];
    const char* file_path = argv[2];

    // ========== 初始化Winsock ==========
    if (!initialize_winsock()) {
        return 1;
    }

    // ========== 创建UDP套接字 ==========
    if ((client_socket = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET) {
        std::cerr << "Socket creation failed" << std::endl;
        return 1;
    }

    // ========== 设置服务器地址 ==========
    // 注意：连接到Router端口进行测试，Router会转发到真实服务器
    server_addr.sin_family = AF_INET;
    //server_addr.sin_port = htons(SERVER_PORT);  // 直连8888，router12345
    server_addr.sin_port = htons(ROUTER_PORT);  // 连接Router端口12345进行丢包/延时测试
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);  // 转换IP地址

    // ========== 三次握手建立连接 ==========
    Packet send_packet = { 0 }, recv_packet = { 0 };
    
    // 第一步：发送SYN
    send_packet.flags = SYN;
    send_packet.seq_num = 0;
    send_packet.checksum = calculate_checksum(&send_packet);
    sendto(client_socket, (const char*)&send_packet, HEADER_SIZE, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
    std::cout << "SYN sent. Waiting for SYN-ACK..." << std::endl;

    // 第二步：接收SYN-ACK
    recvfrom(client_socket, (char*)&recv_packet, MAX_BUFFER_SIZE, 0, NULL, NULL);//阻塞等待服务器返回的数据包
    if (recv_packet.flags == (SYN | ACK)) {//收到的包是不是 SYN-ACK
        std::cout << "SYN-ACK received. Sending final ACK." << std::endl;
        
        // 第三步：发送ACK
        send_packet = { 0 };//整个结构体清零
        send_packet.flags = ACK;
        send_packet.ack_num = recv_packet.seq_num + 1;//期望下一个包
        send_packet.checksum = calculate_checksum(&send_packet);
        sendto(client_socket, (const char*)&send_packet, HEADER_SIZE, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
    }
    std::cout << "Connection established." << std::endl;

    // ========== 读取文件到内存 ==========
    std::ifstream file(file_path, std::ios::binary);  // 以二进制模式打开
    if (!file.is_open()) {
        std::cerr << "Failed to open file: " << file_path << std::endl;
        return 1;
    }
    // 使用迭代器将整个文件读入vector
    std::vector<char> file_buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    // ========== 启动ACK接收线程 ==========
    std::thread ack_thread(receive_acks);

    // ========== 主发送循环 ==========
    auto start_time = std::chrono::high_resolution_clock::now();  // 记录开始时间
    size_t bytes_sent_total = 0;  // 已发送的总字节数

    // 循环条件：还有数据未发送 或 发送窗口不为空（有未确认的包）
    while (bytes_sent_total < file_buffer.size() || !send_window.empty()) {
        std::unique_lock<std::mutex> lock(window_mutex);  // 加锁

        // ========== 步骤1：处理超时和快速重传 ==========
        uint32_t fast_retransmit_target = retransmit_seq_num;
        retransmit_seq_num = 0;  // 清除快速重传信号
        
        if (fast_retransmit_target > 0 && send_window.count(fast_retransmit_target)) {
            // === 快速重传（收到3个重复ACK）===
            std::cout << "--- FAST RETRANSMIT for SEQ=" << fast_retransmit_target << " ---" << std::endl;
            PacketState& ps = send_window.at(fast_retransmit_target);//状态设置为未确认
            ps.packet.checksum = calculate_checksum(&ps.packet);
            // 重传数据包
            sendto(client_socket, (const char*)&ps.packet, HEADER_SIZE + ps.packet.data_len, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
            ps.send_time = std::chrono::steady_clock::now();
            total_retransmissions++;
        }
        else {
            // === 超时重传检测 ===
            for (auto& pair : send_window) {
                auto& ps = pair.second;// 数据包状态
                auto now = std::chrono::steady_clock::now();
                // 计算自发送以来的时间
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - ps.send_time).count() > PACKET_TIMEOUT_MS) {
                    std::cout << "--- TIMEOUT for SEQ=" << ps.packet.seq_num << ". Retransmitting. ---" << std::endl;
                    ps.packet.checksum = calculate_checksum(&ps.packet);
                    // 重传数据包
                    sendto(client_socket, (const char*)&ps.packet, HEADER_SIZE + ps.packet.data_len, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
                    ps.send_time = now;// 更新发送时间
                    total_retransmissions++;// 统计重传次数

                    // 超时事件触发：进入慢启动，窗口减半
                    state = SLOW_START;
                    ssthresh = (std::max)(2.0, cwnd / 2.0);
                    cwnd = 1;
                    duplicate_ack_count = 0;
                }
            }
        }

        // ========== 步骤2：发送新数据包（受窗口限制）==========
        // 窗口大小 = min(流量控制窗口, 拥塞窗口)
        while (send_window.size() <(std::min)((double)FLOW_CONTROL_WINDOW_SIZE, cwnd) && bytes_sent_total < file_buffer.size()) {//条件允许发送新包
            // 计算本次发送的数据量（最多MAX_DATA_SIZE字节）
            int data_to_send = (std::min)((size_t)MAX_DATA_SIZE, file_buffer.size() - bytes_sent_total);

            // 构造新数据包
            Packet new_packet = { 0 };
            new_packet.seq_num = next_seq_num;
            new_packet.data_len = data_to_send;
            memcpy(new_packet.data, file_buffer.data() + bytes_sent_total, data_to_send);  // 复制数据
            new_packet.checksum = calculate_checksum(&new_packet);  // 计算校验和

            // 发送数据包
            sendto(client_socket, (const char*)&new_packet, HEADER_SIZE + data_to_send, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
            std::cout << "Sent SEQ=" << next_seq_num << ", CWND=" << cwnd << ", SSTHRESH=" << ssthresh << std::endl;
            total_packets_sent++;

            // 将数据包加入发送窗口
            PacketState ps;
            ps.packet = new_packet;
            ps.send_time = std::chrono::steady_clock::now();  // 记录发送时间
            send_window[next_seq_num] = ps;// 插入发送窗口

            next_seq_num++;
            bytes_sent_total += data_to_send;// 更新已发送字节数
        }

        // 等待一小段时间或收到快速重传信号（避免忙等待）
        retransmit_cv.wait_for(lock, std::chrono::milliseconds(10));
    }
    transmission_complete = true;  // 标记传输完成

    // ========== 四次挥手关闭连接 ==========
    send_packet = { 0 };// 发送FIN
    send_packet.flags = FIN;// 设置FIN标志
    send_packet.seq_num = next_seq_num;// 设置序列号
    send_packet.checksum = calculate_checksum(&send_packet);// 计算校验和
    // 发送FIN包
    sendto(client_socket, (const char*)&send_packet, HEADER_SIZE, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
    std::cout << "FIN sent. Waiting for final ACK." << std::endl;

    ack_thread.join();  // 等待ACK接收线程结束

    // ========== 计算并输出传输统计 ==========
    auto end_time = std::chrono::high_resolution_clock::now();
    double duration_s = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count() / 1e6;
    double throughput_kbps = (file_buffer.size() * 8) / (duration_s * 1024);  // 吞吐率（Kbps）

    std::cout << "\n--- Transmission Summary ---" << std::endl;
    std::cout << "Total time: " << duration_s << " seconds" << std::endl;
    std::cout << "File size: " << file_buffer.size() / 1024.0 << " KB" << std::endl;
    std::cout << "Average throughput: " << throughput_kbps << " Kbps" << std::endl;
    std::cout << "Total packets sent: " << total_packets_sent.load() << std::endl;
    std::cout << "Total retransmissions: " << total_retransmissions.load() << std::endl;
    std::cout << "Total ACKs received: " << total_acks_received.load() << std::endl;
    if (total_packets_sent > 0) {
        double loss_rate = (double)total_retransmissions.load() / total_packets_sent.load() * 100;
        std::cout << "Packet loss rate: " << loss_rate << "%" << std::endl;
    }

    closesocket(client_socket);
    cleanup_winsock();
    return 0;
}