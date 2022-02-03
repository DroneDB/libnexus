#!/bin/bash
__dirname=$(cd $(dirname "$0"); pwd -P)
cd ${__dirname}
set -e -o pipefail

export DEBIAN_FRONTEND=noninteractive
ISSUE=$(cat /etc/issue | xargs echo -n)
PKG_SUFFIX=""
if [[ "$ISSUE" == *"Ubuntu"* ]]; then
  UBUNTU_VERSION=$(echo $ISSUE | cut -c7-12 | xargs echo -n)
  echo "Ubuntu: $UBUNTU_VERSION"
  PKG_SUFFIX="ubuntu-$UBUNTU_VERSION-"
  apt update || sudo apt update
fi

check_command(){
	check_msg_prefix="Checking for $1... "
	check_msg_result="\033[92m\033[1m OK\033[0m\033[39m"
	unset not_found

	hash $1 2>/dev/null || not_found=true 
	if [[ $not_found ]]; then
		
		# Can we attempt to install it?
		if [[ ! -z "$3" ]]; then
			echo -e "$check_msg_prefix \033[93mnot found, we'll attempt to install\033[39m"
			eval "$3 || sudo $3"

			# Recurse, but don't pass the install command
			check_command "$1" "$2"	
		else
			check_msg_result="\033[91m can't find $1! Check that the program is installed and that you have added the proper path to the program to your PATH environment variable. If you change your PATH environment variable, remember to close and reopen your terminal. $2\033[39m"
		fi
	fi

	echo -e "$check_msg_prefix $check_msg_result"
	if [[ $not_found ]]; then
		return 1
	fi
}

check_command "curl" "apt install curl" "apt install curl -y"
check_command "g++" "apt install build-essential" "apt install build-essential -y"
check_command "cmake" "apt install cmake" "apt install cmake -y"

if [ -e build ]; then
	echo "Removing previous build directory"
	rm -fr build
fi

mkdir -p build
cd build

QT_PKG=qt5-5.9.5-minimal-ubuntu-$UBUNTU_VERSION-amd64-dev.deb

if [ ! -e $QT_PKG ]; then
    echo Downloading QT5 Minimal...
    curl -L https://github.com/DroneDB/Qt5-Minimal/releases/download/v1.0.0/$QT_PKG --output $QT_PKG
fi

dpkg -x $QT_PKG ./Qt9.5
cmake -DQt5_DIR=${__dirname}/build/Qt9.5/lib/cmake/Qt5 -DCMAKE_INSTALL_PREFIX=${__dirname}/install ..
make -j$(nproc) && make install

cd $__dirname
mkdir -p install/DEBIAN

# Build dev package
echo "Package: nxs
Version: 1.0.0
Architecture: amd64
Maintainer: Piero Toffanin <pt@masseranolabs.com>
Description: libnexus development library" > install/DEBIAN/control

dpkg-deb --build install
mv -v install.deb nxs-${PKG_SUFFIX}amd64.deb

echo "Done!"