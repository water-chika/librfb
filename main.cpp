#include "rfb.hpp"


int main(int argc, char** argv) {
    try {
        char* address;
        char* port;
        if (argc < 3) {
            throw std::logic_error("Usage: rfb_demo <address> <port>");
        }
        address = argv[1];
        port = argv[2];
        rfb::rfb::rfb_process(address, port);
    }
    catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
    return 0;
}
