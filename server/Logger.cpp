#include "Logger.hpp"

Logger::Logger(const string &filename) {
    out_.open(filename, ios::app);
}

void Logger::log(const string &user, const string &msg) {
    if (!out_) return;

    auto now = chrono::system_clock::now();
    auto tt  = chrono::system_clock::to_time_t(now);
    tm tmv{};
    localtime_r(&tt, &tmv);

    lock_guard<mutex> lock(mtx_);
    out_ << put_time(&tmv, "%Y-%m-%d %H:%M:%S")
         << " [" << user << "] " << msg << "\n";
    out_.flush();
}
