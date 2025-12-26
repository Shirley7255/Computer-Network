#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#define PORT 8888
#define MAX_CLIENTS 100
#define BUF_SIZE 1024

// --- 全局控制台句柄和辅助函数 ---
HANDLE hConsole = NULL;

// 辅助函数：将 UTF-8 字符串转换为宽字符并使用 WriteConsoleW 输出
void write_wconsole(const char* utf8_str) {
    if (utf8_str == NULL) return;
    if (hConsole == NULL) {
        printf("%s", utf8_str); // 极端情况下，退回到 printf
        return;
    }

    // 1. 计算所需的宽字符长度
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, NULL, 0);
    if (wlen == 0) return;

    // 2. 转换为宽字符 (wchar_t)
    wchar_t* wstr = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (wstr == NULL) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, wstr, wlen);

    // 3. 使用 WriteConsoleW 直接输出到控制台（wlen - 1 忽略最后的 '\0'）
    DWORD written;
    WriteConsoleW(hConsole, wstr, wlen - 1, &written, NULL);

    free(wstr);
}


// --- 结构体和全局变量 ---
typedef struct {
    SOCKET sock;
    char name[50]; // 存储 UTF-8 编码的昵称
} Client;

Client clients[MAX_CLIENTS];
int client_count = 0;
CRITICAL_SECTION cs; // 线程同步

// --- 函数声明 ---
DWORD WINAPI handle_client(LPVOID arg);

// 广播函数：发送指定长度的原始数据
void broadcast_raw(const char* data, int len, SOCKET exclude_sock) {
    if (len <= 0) return;
    EnterCriticalSection(&cs);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].sock != exclude_sock) {
            send(clients[i].sock, data, len, 0);
        }
    }
    LeaveCriticalSection(&cs);
}


int main() {
    // 关键步骤: 设置控制台为 UTF-8
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // 获取控制台句柄
    hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed.\n");
        return 1;
    }

    InitializeCriticalSection(&cs);

    SOCKET server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock == INVALID_SOCKET) {
        printf("socket failed.\n");
        return 1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("bind failed.\n");
        return 1;
    }

    if (listen(server_sock, 10) == SOCKET_ERROR) {
        printf("listen failed.\n");
        return 1;
    }

    // 初始启动消息使用 WriteConsoleW 确保能正常显示
    write_wconsole("Chat server started on port 8888...\n");

    while (1) {
        struct sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);
        SOCKET client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock == INVALID_SOCKET) {
            printf("accept failed.\n");
            continue;
        }

        SOCKET* pclient = malloc(sizeof(SOCKET));
        *pclient = client_sock;
        CreateThread(NULL, 0, handle_client, pclient, 0, NULL);
    }

    closesocket(server_sock);
    DeleteCriticalSection(&cs);
    WSACleanup();
    return 0;
}

DWORD WINAPI handle_client(LPVOID arg) {
    SOCKET sock = *((SOCKET*)arg);
    free(arg);
    char name[50], buffer[BUF_SIZE]; // name 存储 UTF-8 昵称
    char log_buf[BUF_SIZE]; // 用于构造日志信息的缓冲区

    // 接收 JOIN 消息
    int len = recv(sock, buffer, BUF_SIZE - 1, 0);
    if (len <= 0) {
        closesocket(sock);
        return 0;
    }
    buffer[len] = '\0';

    // 安全解析 JOIN 协议并存储 UTF-8 昵称
    if (strncmp(buffer, "JOIN|", 5) == 0) {
        const char* start = buffer + 5;
        const char* end = strchr(start, '|');
        if (end != NULL) {
            int name_len = (int)(end - start);
            if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
            strncpy(name, start, name_len);
            name[name_len] = '\0';
        }
        else {
            strcpy(name, "Unknown");
        }
    }
    else {
        strcpy(name, "Unknown");
    }

    EnterCriticalSection(&cs);
    strcpy(clients[client_count].name, name); // 存储 UTF-8 昵称
    clients[client_count].sock = sock;
    client_count++;
    LeaveCriticalSection(&cs);

    // --- 日志打印 (使用 WriteConsoleW) ---
    snprintf(log_buf, sizeof(log_buf), "%s joined the chat\n", name);
    write_wconsole(log_buf);

    // 构造系统消息 (仍然使用 UTF-8 编码)
    snprintf(buffer, sizeof(buffer), "SYS|Server|%s joined the chat.\n", name);
    // 广播消息 (发送 UTF-8 字节流)
    broadcast_raw(buffer, (int)strlen(buffer), sock);


    // 聊天循环：接收并广播原始的 UTF-8 消息字节
    while (1) {
        len = recv(sock, buffer, BUF_SIZE - 1, 0);
        if (len <= 0) break;

        if (len >= 4 && strncmp(buffer, "QUIT", 4) == 0) break;

        // 广播接收到的实际长度 len
        broadcast_raw(buffer, len, sock);
    }

    closesocket(sock);

    EnterCriticalSection(&cs);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].sock == sock) {
            clients[i] = clients[client_count - 1];
            client_count--;
            break;
        }
    }
    LeaveCriticalSection(&cs);

    // 构造和广播离开系统消息 (仍然使用 UTF-8 编码)
    snprintf(buffer, sizeof(buffer), "SYS|Server|%s left the chat.\n", name);
    broadcast_raw(buffer, (int)strlen(buffer), INVALID_SOCKET);

    // --- 退出日志打印 (使用 WriteConsoleW) ---
    snprintf(log_buf, sizeof(log_buf), "%s disconnected\n", name);
    write_wconsole(log_buf);

    return 0;
}