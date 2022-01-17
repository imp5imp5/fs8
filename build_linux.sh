rm -rf build
mkdir build
pushd build
cmake -G "Unix Makefiles" ..
cmake --build . --config Release
popd
