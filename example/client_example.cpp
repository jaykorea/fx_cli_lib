// client_example.cpp
//
// Example usage of the C++ FX CLI library.
// Demonstrates how to send high-level commands to the FX motor controller
// over its text-based UDP AT command protocol using FxCli.

#include "fx_client.h"
#include <iostream>
#include <vector>
#include <exception>

int main() {
    try {
        // Create client (target MCU IP and port)
        FxCli cli("192.168.10.10", 5101);

        // Define motor IDs
        std::vector<uint8_t> ids_specific = {1, 2};

        // START (ACK expected: OK <START ...>)
        if (!cli.motor_start(ids_specific)) {
            std::cerr << "Motor start failed" << std::endl;
        } else {
            std::cout << "Motor start succeeded" << std::endl;
        }

        // MIT operation control (fire-and-forget, no ACK check)
        // std::vector<float> pos = {0.0f, 0.0f};
        // std::vector<float> vel = {0.0f, 0.0f};
        // std::vector<float> kp  = {0.0f, 0.0f};
        // std::vector<float> kd  = {0.001f, 0.001f};
        // std::vector<float> tau = {0.0f, 0.0f};
        // cli.operation_control(ids_specific, pos, vel, kp, kd, tau);
        // std::cout << "operation_control(1,2) sent" << std::endl;

        // Request state (response string)
        std::string state = cli.req(ids_specific);
        std::cout << "REQ response: " << state << std::endl;

        // Diagnostic status (response string)
        std::string status = cli.status();
        std::cout << "STATUS response: " << status << std::endl;

        // STOP
        if (!cli.motor_stop(ids_specific)) {
            std::cerr << "Motor stop failed" << std::endl;
        } else {
            std::cout << "Motor stop succeeded" << std::endl;
        }

        // Optional: E-STOP
        // if (!cli.motor_estop(ids_specific)) {
        //     std::cerr << "Motor estop failed" << std::endl;
        // } else {
        //     std::cout << "Motor estop succeeded" << std::endl;
        // }

        // --- Broadcast example ---
        // std::vector<uint8_t> ids_broadcast; // empty = "<>" → broadcast
        // if (!cli.motor_stop(ids_broadcast)) {
        //     std::cerr << "Broadcast stop failed" << std::endl;
        // } else {
        //     std::cout << "Broadcast stop succeeded" << std::endl;
        // }

    } catch (const std::exception& ex) {
        std::cerr << "Error during execution: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
