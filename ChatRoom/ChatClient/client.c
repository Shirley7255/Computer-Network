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
#define BUF_SIZE 1024

SOCKET sock;
char name[50];

// 统一的输出函数，直接使用 printf，依赖用户设置 chcp 65001
void safe_print(const char* str) {
    printf("%s", str);
    fflush(stdout);
}

DWORD WINAPI recv_handler(LPVOID arg) {
    char buffer[BUF_SIZE];
    char type[5], sender[50], content[BUF_SIZE];

    while (1) {
        int len = recv(sock, buffer, BUF_SIZE - 1, 0);
        if (len <= 0) break;
        buffer[len] = '\0';

        // 解析消息
        char* token_ctx = NULL;
        char temp_buffer[BUF_SIZE];
        strncpy(temp_buffer, buffer, BUF_SIZE);
        temp_buffer[BUF_SIZE - 1] = '\0';

        char* msg_type = strtok_s(temp_buffer, "|", &token_ctx);
        if (msg_type == NULL) goto skip_print;

        char* msg_sender = strtok_s(NULL, "|", &token_ctx);
        if (msg_sender == NULL) goto skip_print;

        char* msg_content = strtok_s(NULL, "\n", &token_ctx);
        if (msg_content == NULL) goto skip_print;

        strncpy(type, msg_type, sizeof(type) - 1);
        type[sizeof(type) - 1] = '\0';

        strncpy(sender, msg_sender, sizeof(sender) - 1);
        sender[sizeof(sender) - 1] = '\0';

        strncpy(content, msg_content, sizeof(content) - 1);
        content[sizeof(content) - 1] = '\0';

        // 打印消息
        if (strcmp(type, "SYS") == 0) {
            printf("[系统消息] %s\n", content);
        }
        else if (strcmp(type, "MSG") == 0) {
            printf("[%s]: %s\n", sender, content);
        }
        else {
            printf("Raw: %s\n", buffer);
        }
        fflush(stdout);
        continue;

    skip_print:
        printf("Unparsed: %s\n", buffer);
        fflush(stdout);
    }

    // 退出提示：使用 printf
    safe_print("服务器连接已断开。\n");
    exit(0);
}

int main() {
    // 设置控制台编码为UTF-8 (必需)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    safe_print("请输入你的昵称：\n");

    if (fgets(name, sizeof(name), stdin) == NULL) {
        return 1;
    }
    name[strcspn(name, "\n")] = '\0';

    // 初始化Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        safe_print("WSAStartup失败\n");
        return 1;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        safe_print("创建socket失败\n");
        WSACleanup();
        return 1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    InetPton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        safe_print("连接服务器失败\n");
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // --- 协议修复：JOIN 消息必须以 \n 结束 ---
    char msg[BUF_SIZE];
    snprintf(msg, sizeof(msg), "JOIN|%s|\n", name); // 加上 \n
    send(sock, msg, (int)strlen(msg), 0);

    HANDLE hThread = CreateThread(NULL, 0, recv_handler, NULL, 0, NULL);
    if (hThread == NULL) {
        safe_print("创建接收线程失败\n");
        closesocket(sock);
        WSACleanup();
        return 1;
    }
    CloseHandle(hThread);

    safe_print("已连接服务器，可输入消息\n");

    // 消息发送循环
    while (1) {
        char content[BUF_SIZE - 100];

        if (fgets(content, sizeof(content), stdin) == NULL) {
            break;
        }
        content[strcspn(content, "\n")] = '\0';

        if (strcmp(content, "/quit") == 0) {
            // --- 协议修复：QUIT 消息必须以 \n 结束 ---
            snprintf(msg, sizeof(msg), "QUIT|%s|\n", name); // 加上 \n
            send(sock, msg, (int)strlen(msg), 0);
            break;
        }

        // MSG 协议已包含 \n
        snprintf(msg, sizeof(msg), "MSG|%s|%s\n", name, content);
        send(sock, msg, (int)strlen(msg), 0);
    }

    closesocket(sock);
    WSACleanup();
    // 退出提示：使用 printf
    safe_print("已退出聊天\n");
    return 0;
}