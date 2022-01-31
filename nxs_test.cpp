#include <iostream>

#include "libnxs.h"

int main(int argc, char *argv[]){
    if (argc < 2) {
        std::cerr << "Usage: ./nxs_test <input.obj>" << std::endl;
        exit(1);
    }

    nexusBuild(argv[1], "out.nxs");
}