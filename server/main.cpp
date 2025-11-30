// ===== file: server/main.cpp =====
#include "FileServer.hpp"
#include <string>

using namespace std;

int main(int argc, char *argv[]) {
    string root_dir = "./data";
    int port = 5051;
    if (argc > 1) port = stoi(argv[1]);

    FileServer server(root_dir, port);
    server.run();
    return 0;
}
