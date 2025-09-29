// ElapsedTimer.cpp
#include "utils/elapsed_timer.h"

ElapsedTimer::ElapsedTimer(const std::string& timername) : timername(timername) {}

void ElapsedTimer::startTimer() {
    start_time = std::chrono::high_resolution_clock::now();
}

void ElapsedTimer::stopTimer() {
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;
    elapsed_times.push_back(elapsed.count() * 1000.0);
    std::cout << "[Timer|" << timername << "] Elapsed: " << elapsed.count() * 1000.0 << " ms" << std::endl;
}

void ElapsedTimer::printStatistics() const {
    if (elapsed_times.empty()) {
        std::cerr << "[Timer|" << timername << "] No elapsed times recorded." << std::endl;
        return;
    }

    double sum = std::accumulate(elapsed_times.begin(), elapsed_times.end(), 0.0);
    double mean = sum / elapsed_times.size();

    double sq_sum = std::inner_product(elapsed_times.begin(), elapsed_times.end(), elapsed_times.begin(), 0.0);
    double stdev = std::sqrt(sq_sum / elapsed_times.size() - mean * mean);

    std::cout << "[Timer|" << timername << "] Mean elapsed time: " << mean << " ms" << std::endl;
    std::cout << "[Timer|" << timername << "] Std of elapsed time: " << stdev << " ms" << std::endl;
}