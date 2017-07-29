#!/bin/sh
set +e
set +x
echo Running make-and-run-tests.sh...
mkdir -p /var/zion/build
cd /var/zion/build

ls -tr -la | grep -i cmake
cmake --version
ctest --version

echo Running CMake...
cmake /opt/zion
ls -tr -la | grep -i cmake

echo Running make...
make -j4
ls -tr -la | grep -i cmake

ls -la llz
echo Running ctest...
ctest /opt/zion || (cat Testing/Temporary/LastTest.log ; exit 1)
cat Testing/Temporary/LastTest.log
