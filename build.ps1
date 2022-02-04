md build
cd build
md install

$ProgressPreference = 'SilentlyContinue'
Invoke-WebRequest -Uri "https://github.com/DroneDB/Qt5-Minimal/releases/download/v1.0.0/qt-5.12.12-minimal-windows64-dev.zip" -OutFile "qt-5.12.12-minimal-windows64-dev.zip"
Expand-Archive -Path qt-5.12.12-minimal-windows64-dev.zip -DestinationPath .\qt-install

$qt5_dir=Convert-Path ./qt-install/lib/cmake/Qt5
$install_dir=Convert-Path ./install

cmake "-DQt5_DIR=$qt5_dir" "-DCMAKE_INSTALL_PREFIX=$install_dir" ..
cmake --build . --config Release --target INSTALL

Compress-Archive -Path .\install\* -DestinationPath .\nxs-windows-64bit.zip