# libnexus

Self-contained, dependency-free C Library to generate [Nexus](https://github.com/cnr-isti-vclab/nexus) files.

It's been tested on Arch / Ubuntu Linux and Windows.

## Building

### Linux

Requirements for Ubuntu:

```bash
sudo apt-get install -y cmake zip ninja-build autoconf automake python3 \
                                    libtool pkg-config curl build-essential bison \
                                    python3-jinja2 libxi-dev libxtst-dev \
                                    ^libxcb.*-dev libx11-xcb-dev libgl1-mesa-dev libxrender-dev libxi-dev \
                                    libxkbcommon-dev libxkbcommon-x11-dev
```

The library uses vcpkg to manage dependencies. You can install it with:

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
```

Clone and compile the library from source:

```bash
git clone https://github.com/DroneDB/libnexus/ --recurse-submodules
cd libnexus
mkdir -p build
cd build
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=x64-linux
    -DBUILD_NXS_TEST=1
make -j$(nproc)
```

### Windows

Assuming vcpkg is installed in `C:\vcpkg`:

```powershell
git clone https://github.com/DroneDB/libnexus/ --recurse-submodules
cd libnexus
mkdir -p build
cd build
cmake .. -A x64 -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake" -DBUILD_NXS_TEST=1
cmake --build . --config Release --target ALL_BUILD -- /maxcpucount:14
```

## Example

```
cmake_minimum_required(VERSION 3.1)
project(test)

find_package(NXS REQUIRED)
add_executable(test main.cpp)
target_link_libraries(test ${NXS_LIBRARIES})
```

```
#include <nxs.h>

int main(){
 nexusBuild("model.obj", "out.nxz");
 return 0;
}
```

## Limitations / Roadmap Ideas

 - [ ] Currently no options are exposed, the nexus default are used.
 - [ ] The code works only for 3D meshes, not point clouds.


## Contributing

We welcome contributions! Feel free to open pull requests to add features you might find useful.

## License

AGPLv3