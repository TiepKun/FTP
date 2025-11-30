// ===== file: server/Logger.hpp =====
#pragma once
#include <fstream>
#include <mutex>
#include <string>
#include <chrono>
#include <iomanip>

using namespace std;

class Logger {
public:
    explicit Logger(const string &filename);
    void log(const string &user, const string &msg);

private:
    ofstream out_;
    mutex mtx_;
};
