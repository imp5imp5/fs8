rd /s /q build
mkdir build
pushd build
cmake -G "Visual Studio 16" -A x64 ..
cmake --build . --config Release
popd
