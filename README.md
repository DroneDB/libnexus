# libnexus

Self-contained, dependency-free C Library to generate [Nexus](https://github.com/cnr-isti-vclab/nexus) files.

It's been tested on Arch / Ubuntu Linux and Windows.

## Building

### Linux

Requirements for Ubuntu:

```bash
sudo apt-get install -y cmake zip ninja-build autoconf automake python3 \
                                    libtool pkg-config curl build-essential bison
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

## ddb-nexus

The build also produces `ddb-nexus`, a command line front end of the library. DroneDB runs it as a
separate process (it does not load libnxs into its own), so the Nexus and VCGLib code stays on the
other side of a process boundary.

```
ddb-nexus [--threads N] <input> <output>
ddb-nexus --version
```

`<input>` is an OBJ or PLY model; `<output>` is the Nexus file (`.nxz` is compressed). The program
calls `nexusBuildEx()` once, with the default options and `threads` set to N (1 or more; without
`--threads`, one per core), and writes a progress line to stdout (flushed)
every 15 seconds while it works, so a supervisor can tell a long build from a hang. `--version`
prints `ddb-nexus <version>`; the version is `APP_VERSION` when libnexus is built as part of DroneDB,
or whatever `-DDDB_NEXUS_VERSION=...` says (default `unknown`).

| Exit code | Meaning |
|-----------|---------|
| 0  | success |
| 1  | the build failed, the message is on stderr |
| 2  | invalid input (for example the input file does not exist), the message is on stderr |
| 64 | wrong command line, usage on stderr |

A model whose textures cannot be packed (a single Nexus node would need a texture over 16K on a
side) is built once more without textures: the build succeeds, with the warning
`textures could not be packed; built without textures` on stderr.

On Linux `ddb-nexus` finds `libnxs.so` next to itself or in `../lib` (RUNPATH `$ORIGIN:$ORIGIN/../lib`).
On Windows `nxs.dll` must sit next to `ddb-nexus.exe`, where the build puts it.

## Textures

The textures of a model are read by libjpeg-turbo (JPEG), libpng (PNG), libtiff (TIFF) and libwebp
(WebP), and by stb_image for BMP, GIF and TGA; the format is told from the file content, not from
its extension. The alpha channel of a texture is ignored and its colors are used as stored. These
libraries come from vcpkg (`vcpkg.json`); there is no GUI toolkit in the build.

The textures of the Nexus file are WebP images (quality `texture_quality`, 90, encoder effort 2)
written by libwebp, in the order of the nodes, at the resolution of the model's textures. The
build keeps at most `threads` threads busy (0: one per core; `ddb-nexus --threads N`): the one
that builds the nodes and the ones that encode their textures meanwhile.

- A node texture is at most `max_node_texture_side` pixels (1024) on a side. The nodes are cut by
  the texels they cover as well as by their triangles, so that their textures fit that size.
- The texels of a node texture that no triangle draws, past a margin of 4 texels, are one flat
  color: about 60% of a node texture lies outside its triangles, and they would cost as many
  bytes as the rest.
- A model whose texture is much finer than its triangles (photogrammetry) first gets levels that
  keep the triangles and halve the side of the texture, as mipmaps do, as many as the texture
  needs to come down to a few texels per triangle edge (`skip_levels`, -1). Their node error is
  the size of a texel, so a viewer loads the finest textures only where a texel is large enough
  on the screen. The levels after them are simplified as usual.

Not supported: animated WebP and 16 bit TIFF sample precision (read as 8 bit by libtiff).

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

GPL-3.0. The wrapper in this repository is licensed under the GNU General Public License, version 3
(see `LICENSE`; it was AGPL-3.0 before). The vendored [Nexus](https://github.com/cnr-isti-vclab/nexus)
is GPL-2.0-or-later and [VCGLib](https://github.com/cnr-isti-vclab/vcglib) is GPL-3.0.