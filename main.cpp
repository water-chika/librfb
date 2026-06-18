#include "rfb.hpp"


int main(int argc, char** argv) {
    try {
        char* port;
        if (argc < 2)
            port = "5900";
        else
            port = argv[1];
        rfb_process("127.0.0.1", port);
    }
    catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
    return 0;
}
