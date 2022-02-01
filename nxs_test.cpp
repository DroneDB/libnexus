#include <iostream>
#include <string.h>
#include "nxs.h"

int main(int argc, char *argv[]){
    if (argc < 2) {
        std::cerr << "Usage: ./nxs_test <input.obj> [--compress]" << std::endl;
        exit(1);
    }

    if (argc >= 2 && strcmp(argv[2], "--compress") == 0){
        nexusBuild(argv[1], "out.nxz");
    }else{
        nexusBuild(argv[1], "out.nxs");
    }

}