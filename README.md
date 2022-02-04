# libnexus

Self-contained, dependency-free C Library to generate [Nexus](https://github.com/cnr-isti-vclab/nexus) files.

It's been tested on Arch / Ubuntu Linux and Windows.

## Usage

Download the [prebuilt binaries](https://github.com/DroneDB/libnexus/releases) or compile the library from source:

```bash
git clone https://github.com/DroneDB/libnexus/ --recurse-submodules
cd libnexus
mkdir build
cd build
make -j$(nproc)
```

### Example

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