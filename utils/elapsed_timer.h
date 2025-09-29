#pragma once
#include <chrono>
#include <string>
#include <vector>
#include <iostream>
#include <numeric>
#include <cmath>

class ElapsedTimer {
public:
    explicit ElapsedTimer(const std::string& timername);

    void startTimer();
    void stopTimer();
    void printStatistics() const;

private:
    std::string timername;
    std::chrono::high_resolution_clock::time_point start_time;
    std::vector<double> elapsed_times;
};
