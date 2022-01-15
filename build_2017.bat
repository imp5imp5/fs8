rd /s /q build
mkdir build
pushd build
cmake -G "Visual Studio 15 Win64" ..
cmake --build . --config Release
popd
