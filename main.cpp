#include "rfb.hpp"


int main(int argc, char** argv) {
    try {
        char* address;
        uint16_t port;
        if (argc < 3) {
            throw std::logic_error("Usage: rfb_demo <address> <port>");
        }
        address = argv[1];
        port = strtol(argv[2], NULL, 10);
        rfb::rfb_process(address, port);
    }
    catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
    return 0;
}
