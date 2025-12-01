#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>

using namespace std;

void send_line(int sock, const string &msg) {
    string data = msg + "\n";
    send(sock, data.c_str(), data.size(), 0);
}

bool read_line(int sock, string &out) {
    out.clear();
    char c;
    while (true) {
        ssize_t n = recv(sock, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\n') break;
        out.push_back(c);
    }
    return true;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        cout << "Usage: ./client_test <ip> <port>\n";
        return 1;
    }

    string ip = argv[1];
    int port = atoi(argv[2]);

    // ---- Tạo socket ----
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &serv.sin_addr);

    // ---- Connect ----
    if (connect(sock, (sockaddr*)&serv, sizeof(serv)) < 0) {
        perror("connect");
        return 1;
    }

    cout << "Connected to server " << ip << ":" << port << endl;

    // ---- Vòng lặp nhập lệnh ----
    while (true) {
        cout << "> ";
        string cmd;
        getline(cin, cmd);

        if (cmd == "EXIT" || cmd == "exit") {
            cout << "Bye.\n";
            break;
        }

        // gửi lệnh
        send_line(sock, cmd);

        // đọc trả lời
        string resp;
        if (!read_line(sock, resp)) {
            cout << "Server closed connection.\n";
            break;
        }

        cout << "[SERVER]: " << resp << endl;
    }

    close(sock);
    return 0;
}
