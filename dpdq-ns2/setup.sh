#!/bin/bash

# Writen for Ubuntu 20.04 and bash. Do NOT execute as superuser.
if [ "$EUID" -eq 0 ]
  then echo "Please do NOT run as root"
  exit
fi

ROOT_DIR=$(git rev-parse --show-toplevel)

echo "Installing Dependencies"
sudo apt update
sudo apt install -y build-essential autoconf automake libxmu-dev bc
sudo apt install -y python3-pip
pip3 install psutil
sudo apt install -y python3-numpy
sudo apt install -y python3-matplotlib

sudo apt install -y flex bison zlib1g-dev libxml2-dev sqlite3 libsqlite3-dev zip unzip


# Download and install OMNET++ (Needed to compile shared lib below)
wget https://github.com/omnetpp/omnetpp/releases/download/omnetpp-5.6.2/omnetpp-5.6.2-src-linux.tgz
tar xvfz omnetpp-5.6.2-src-linux.tgz
pushd "./omnetpp-5.6.2/"
. setenv -f

./configure WITH_QTENV=no WITH_OSG=no WITH_OSGEARTH=no
make -j
popd

PATH_FILE="${HOME}/.bashrc"
echo "Adding omnet and ns-2 executables to ${PATH_FILE}"

# Assume ROOT_DIR is defined somewhere above in the script
PATH_ADDITION="${ROOT_DIR}/omnetpp-5.6.2/bin:${ROOT_DIR}/ns2.34/bin:${ROOT_DIR}/ns2.34/tcl8.4.18/unix"
LD_LIBRARY_PATH_ADDITION="${ROOT_DIR}/ns2.34/otcl-1.13:${ROOT_DIR}/ns2.34/lib"
TCL_LIBRARY_ADDITION="${ROOT_DIR}/ns2.34/tcl8.4.18/library"  # Adjusted if duplicate ns2.34 was a mistake

if [ ! -f "${PATH_FILE}" ]; then
  touch "${PATH_FILE}"
fi

echo "PA ${PATH_ADDITION}"
if ! grep -q "omnetpp-5.6.2" "${PATH_FILE}"; then
  # Use single quotes so $PATH gets expanded at runtime, not now.
  echo 'export PATH="$PATH:'"${PATH_ADDITION}"'"' | tee -a "${PATH_FILE}"
  echo 'export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:'"${LD_LIBRARY_PATH_ADDITION}"'"' | tee -a "${PATH_FILE}"
  echo 'export TCL_LIBRARY='"${TCL_LIBRARY_ADDITION}" | tee -a "${PATH_FILE}"
fi

profile_command=$(cat << 'EOF'
if [ -n "$BASH_VERSION" ]; then
    # include .bashrc if it exists
    if [ -f "$HOME/.bashrc" ]; then
        . "$HOME/.bashrc"
    fi
fi
EOF
)
if [ ! -f "${HOME}/.profile" ]; then
  touch "${HOME}/.profile"
  echo "$profile_command" >> "${HOME}/.profile"
fi

PATH="$PATH:${PATH_ADDITION}"

echo "Building library shared with other simulators"
pushd "${ROOT_DIR}/other_simulators/common/src"
make clean
make



echo "Installing ns-2"
pushd "${ROOT_DIR}/ns2.34"
./install || { echo "ns-2 installation failed! Exiting..." >&2; exit 1; }
echo "Exit status: $?"


echo "Building ns-2 in debug mode"
pushd "${ROOT_DIR}/ns2.34"
./configure --enable-debug
make clean
make -j

echo -e "\nSetup complete. Start a new terminal. Then try 'which ns'"