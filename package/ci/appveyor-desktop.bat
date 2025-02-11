if "%APPVEYOR_BUILD_WORKER_IMAGE%" == "Visual Studio 2022" call "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvarsall.bat" x64 || exit /b
if "%APPVEYOR_BUILD_WORKER_IMAGE%" == "Visual Studio 2019" call "C:/Program Files (x86)/Microsoft Visual Studio/2019/Community/VC/Auxiliary/Build/vcvarsall.bat" x64 || exit /b
if "%APPVEYOR_BUILD_WORKER_IMAGE%" == "Visual Studio 2017" call "C:/Program Files (x86)/Microsoft Visual Studio/2017/Community/VC/Auxiliary/Build/vcvarsall.bat" x64 || exit /b
if "%APPVEYOR_BUILD_WORKER_IMAGE%" == "Visual Studio 2015" call "C:/Program Files (x86)/Microsoft Visual Studio 14.0/VC/vcvarsall.bat" x64 || exit /b
rem Unlike with Magnum itself which copies OpenAL DLL to its output directory,
rem here we need to do that ourselves as we don't have Magnum's build dir in
rem PATH
set PATH=%APPVEYOR_BUILD_FOLDER%/openal/bin/Win64;%APPVEYOR_BUILD_FOLDER%\deps\bin;%APPVEYOR_BUILD_FOLDER%\devil\unicode;C:\Tools\vcpkg\installed\x64-windows\bin;%PATH%

rem need to explicitly specify a 64-bit target, otherwise CMake+Ninja can't
rem figure that out -- https://gitlab.kitware.com/cmake/cmake/issues/16259
rem for TestSuite we need to enable exceptions explicitly with /EH as these are
rem currently disabled -- https://github.com/catchorg/Catch2/issues/1113
if "%COMPILER%" == "msvc-clang" if "%APPVEYOR_BUILD_WORKER_IMAGE%" == "Visual Studio 2022" set COMPILER_EXTRA=-DCMAKE_C_COMPILER="C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/bin/clang-cl.exe" -DCMAKE_CXX_COMPILER="C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/bin/clang-cl.exe" -DCMAKE_LINKER="C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/bin/lld-link.exe" -DCMAKE_C_FLAGS="-m64 /EHsc" -DCMAKE_CXX_FLAGS="-m64 /EHsc"
if "%COMPILER%" == "msvc-clang" if "%APPVEYOR_BUILD_WORKER_IMAGE%" == "Visual Studio 2019" set COMPILER_EXTRA=-DCMAKE_C_COMPILER="C:/Program Files (x86)/Microsoft Visual Studio/2019/Community/VC/Tools/Llvm/bin/clang-cl.exe" -DCMAKE_CXX_COMPILER="C:/Program Files (x86)/Microsoft Visual Studio/2019/Community/VC/Tools/Llvm/bin/clang-cl.exe" -DCMAKE_LINKER="C:/Program Files (x86)/Microsoft Visual Studio/2019/Community/VC/Tools/Llvm/bin/lld-link.exe" -DCMAKE_C_FLAGS="-m64 /EHsc" -DCMAKE_CXX_FLAGS="-m64 /EHsc"

rem Build libPNG. As of 2020-08-17, vcpkg is broken on the 2019 image and needs
rem updating. Disabling the libPNG build there for now.
IF "%APPVEYOR_BUILD_WORKER_IMAGE%" == "Visual Studio 2019" set EXCEPT_IF_VCPKG_IS_BROKEN=OFF
IF NOT "%APPVEYOR_BUILD_WORKER_IMAGE%" == "Visual Studio 2019" set EXCEPT_IF_VCPKG_IS_BROKEN=ON
IF NOT "%APPVEYOR_BUILD_WORKER_IMAGE%" == "Visual Studio 2019" vcpkg install libpng:x64-windows || exit /b

rem Some dependencies are built on GitHub Actions and there the only available
rem MSVC version is 2019. Better than nothing, but eh.
IF "%APPVEYOR_BUILD_WORKER_IMAGE%" == "Visual Studio 2019" set ONLY_ON_MSVC2019=ON
IF NOT "%APPVEYOR_BUILD_WORKER_IMAGE%" == "Visual Studio 2019" set ONLY_ON_MSVC2019=OFF
IF "%APPVEYOR_BUILD_WORKER_IMAGE%" == "Visual Studio 2015" set EXCEPT_MSVC2015=OFF
IF NOT "%APPVEYOR_BUILD_WORKER_IMAGE%" == "Visual Studio 2015" set EXCEPT_MSVC2015=ON

rem Build meshoptimizer
IF NOT EXIST %APPVEYOR_BUILD_FOLDER%\v0.14.zip appveyor DownloadFile https://github.com/zeux/meshoptimizer/archive/v0.14.zip || exit /b
7z x v0.14.zip || exit /b
ren meshoptimizer-0.14 meshoptimizer || exit /b
cd meshoptimizer || exit /b
mkdir build && cd build || exit /b
cmake .. ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_INSTALL_PREFIX=%APPVEYOR_BUILD_FOLDER%/deps ^
    %COMPILER_EXTRA% -G Ninja || exit /b
cmake --build . --target install || exit /b
cd .. && cd .. || exit /b

rem Build zstd
IF NOT EXIST %APPVEYOR_BUILD_FOLDER%\v1.5.0.zip appveyor DownloadFile https://github.com/facebook/zstd/archive/refs/tags/v1.5.0.zip || exit /b
7z x v1.5.0.zip || exit /b
ren zstd-1.5.0 zstd || exit /b
cd zstd || exit /b
rem There's already a directory named `build`
mkdir build_ && cd build_ || exit /b
cmake ../build/cmake ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_INSTALL_PREFIX=%APPVEYOR_BUILD_FOLDER%/deps ^
    -DZSTD_BUILD_PROGRAMS=OFF ^
    -DZSTD_BUILD_SHARED=OFF ^
    -DZSTD_BUILD_STATIC=ON ^
    -DZSTD_MULTITHREAD_SUPPORT=OFF ^
    %COMPILER_EXTRA% -G Ninja || exit /b
cmake --build . --target install || exit /b
cd .. && cd .. || exit /b

rem Build Corrade
git clone --depth 1 https://github.com/mosra/corrade.git || exit /b
cd corrade || exit /b
mkdir build && cd build || exit /b
cmake .. ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_INSTALL_PREFIX=%APPVEYOR_BUILD_FOLDER%/deps ^
    -DCORRADE_WITH_INTERCONNECT=OFF ^
    -DCORRADE_UTILITY_USE_ANSI_COLORS=ON ^
    -DCORRADE_BUILD_STATIC=%BUILD_STATIC% ^
    %COMPILER_EXTRA% -G Ninja || exit /b
cmake --build . || exit /b
cmake --build . --target install || exit /b
cd .. && cd ..

rem Build Magnum
git clone --depth 1 https://github.com/mosra/magnum.git || exit /b
cd magnum || exit /b
mkdir build && cd build || exit /b
cmake .. ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_INSTALL_PREFIX=%APPVEYOR_BUILD_FOLDER%/deps ^
    -DCMAKE_PREFIX_PATH=%APPVEYOR_BUILD_FOLDER%/openal ^
    -DMAGNUM_WITH_AUDIO=ON ^
    -DMAGNUM_WITH_DEBUGTOOLS=ON ^
    -DMAGNUM_WITH_GL=OFF ^
    -DMAGNUM_WITH_MESHTOOLS=ON ^
    -DMAGNUM_WITH_PRIMITIVES=ON ^
    -DMAGNUM_WITH_SCENEGRAPH=OFF ^
    -DMAGNUM_WITH_SCENETOOLS=OFF ^
    -DMAGNUM_WITH_SHADERS=OFF ^
    -DMAGNUM_WITH_TEXT=ON ^
    -DMAGNUM_WITH_TEXTURETOOLS=ON ^
    -DMAGNUM_WITH_ANYIMAGEIMPORTER=ON ^
    -DMAGNUM_BUILD_STATIC=%BUILD_STATIC% ^
    -DMAGNUM_BUILD_PLUGINS_STATIC=%BUILD_STATIC% ^
    %COMPILER_EXTRA% -G Ninja || exit /b
cmake --build . || exit /b
cmake --build . --target install || exit /b
cd .. && cd ..

rem Build. MAGNUM_BUILD_GL_TESTS is enabled just to be sure, it should not be
rem needed by any plugin.
mkdir build && cd build || exit /b
cmake .. ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_INSTALL_PREFIX=%APPVEYOR_BUILD_FOLDER%/deps ^
    -DCMAKE_PREFIX_PATH=%APPVEYOR_BUILD_FOLDER%/openal;%APPVEYOR_BUILD_FOLDER%/devil;C:/Tools/vcpkg/installed/x64-windows ^
    -DMAGNUM_WITH_ASSIMPIMPORTER=%EXCEPT_MSVC2015% ^
    -DMAGNUM_WITH_ASTCIMPORTER=ON ^
    -DMAGNUM_WITH_BASISIMAGECONVERTER=ON ^
    -DMAGNUM_WITH_BASISIMPORTER=ON -DBASIS_UNIVERSAL_DIR=%APPVEYOR_BUILD_FOLDER%/basis_universal ^
    -DMAGNUM_WITH_CGLTFIMPORTER=ON ^
    -DMAGNUM_WITH_DDSIMPORTER=ON ^
    -DMAGNUM_WITH_DEVILIMAGEIMPORTER=ON ^
    -DMAGNUM_WITH_DRFLACAUDIOIMPORTER=ON ^
    -DMAGNUM_WITH_DRMP3AUDIOIMPORTER=ON ^
    -DMAGNUM_WITH_DRWAVAUDIOIMPORTER=ON ^
    -DMAGNUM_WITH_FREETYPEFONT=%EXCEPT_MSVC2015% ^
    -DMAGNUM_WITH_GLSLANGSHADERCONVERTER=%EXCEPT_MSVC2015% ^
    -DMAGNUM_WITH_GLTFIMPORTER=ON ^
    -DMAGNUM_WITH_HARFBUZZFONT=OFF ^
    -DMAGNUM_WITH_ICOIMPORTER=ON ^
    -DMAGNUM_WITH_JPEGIMAGECONVERTER=%EXCEPT_MSVC2015% ^
    -DMAGNUM_WITH_JPEGIMPORTER=%EXCEPT_MSVC2015% ^
    -DMAGNUM_WITH_KTXIMAGECONVERTER=ON ^
    -DMAGNUM_WITH_KTXIMPORTER=ON ^
    -DMAGNUM_WITH_MESHOPTIMIZERSCENECONVERTER=ON ^
    -DMAGNUM_WITH_MINIEXRIMAGECONVERTER=ON ^
    -DMAGNUM_WITH_OPENEXRIMAGECONVERTER=%EXCEPT_MSVC2015% ^
    -DMAGNUM_WITH_OPENEXRIMPORTER=%EXCEPT_MSVC2015% ^
    -DMAGNUM_WITH_OPENGEXIMPORTER=ON ^
    -DMAGNUM_WITH_PNGIMAGECONVERTER=%EXCEPT_IF_VCPKG_IS_BROKEN% ^
    -DMAGNUM_WITH_PNGIMPORTER=%EXCEPT_IF_VCPKG_IS_BROKEN% ^
    -DMAGNUM_WITH_PRIMITIVEIMPORTER=ON ^
    -DMAGNUM_WITH_SPIRVTOOLSSHADERCONVERTER=%EXCEPT_MSVC2015% ^
    -DMAGNUM_WITH_STANFORDIMPORTER=ON ^
    -DMAGNUM_WITH_STANFORDSCENECONVERTER=ON ^
    -DMAGNUM_WITH_STBDXTIMAGECONVERTER=ON ^
    -DMAGNUM_WITH_STBIMAGECONVERTER=ON ^
    -DMAGNUM_WITH_STBIMAGEIMPORTER=ON ^
    -DMAGNUM_WITH_STBTRUETYPEFONT=ON ^
    -DMAGNUM_WITH_STBVORBISAUDIOIMPORTER=ON ^
    -DMAGNUM_WITH_STLIMPORTER=ON ^
    -DMAGNUM_WITH_TINYGLTFIMPORTER=ON ^
    -DMAGNUM_WITH_WEBPIMPORTER=OFF ^
    -DMAGNUM_BUILD_TESTS=ON ^
    -DMAGNUM_BUILD_GL_TESTS=ON ^
    -DMAGNUM_BUILD_STATIC=%BUILD_STATIC% ^
    -DMAGNUM_BUILD_PLUGINS_STATIC=%BUILD_STATIC% ^
    %COMPILER_EXTRA% -G Ninja || exit /b
cmake --build . || exit /b
cmake --build . --target install || exit /b

rem Test
set CORRADE_TEST_COLOR=ON
ctest -V || exit /b
