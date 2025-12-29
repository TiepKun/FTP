#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

using namespace std;

int main(int argc, char** argv) {
    if (argc < 3) {
        cerr << "Usage: test_client <host> <port>\n";
        cerr << "Then type commands; each line sent as-is. Ctrl+D to quit." << endl;
        return 1;
    }

    string host = argv[1];
    int port = stoi(argv[2]);

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        cerr << "WSAStartup failed" << endl;
        return 1;
    }
#endif

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &srv.sin_addr) <= 0) {
        cerr << "Invalid host" << endl; return 1;
    }

    if (connect(sock, (sockaddr*)&srv, sizeof(srv)) != 0) { perror("connect"); return 1; }

    cout << "Connected to " << host << ":" << port << "\n";
    cout << "Type commands (one per line). 'QUIT' to exit.\n";

    string line;
    while (true) {
        if (!getline(cin, line)) break;
        if (line == "QUIT") break;
        line += "\n"; // server expects newline-terminated lines
        ssize_t sent = send(sock, line.c_str(), (int)line.size(), 0);
        if (sent <= 0) { perror("send"); break; }

        // Read response (simple): read until newline then print
        string resp;
        char buf[1024];
        ssize_t n = recv(sock, buf, sizeof(buf)-1, 0);
        if (n <= 0) { cout << "Connection closed by server\n"; break; }
        buf[n] = '\0';
        cout << buf;

        // if server indicates multi-line (OK 200 <count>), attempt to read more
        // naive: if response starts with OK and contains count, read that many bytes lines
    }

#ifdef _WIN32
    closesocket(sock);
    WSACleanup();
#else
    close(sock);
#endif
    cout << "Exited." << endl;
    return 0;
}
