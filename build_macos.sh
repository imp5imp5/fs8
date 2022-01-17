rm -rf build
mkdir build
pushd build
cmake -G Xcode ..
cmake --build . --config Release
popd
