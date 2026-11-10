# Disclaimer
If you'd like to compile only for x86 and get this
```sh
./third-party/common/linux/premake/premake5 --file=premake5.lua --genproto --os=windows vs2026
Error: /home/twig/Projects/gbe_fork/premake5.lua:171: protoc not found! /home/twig/Projects/gbe_fork/build/deps/win/vs2026/protobuf/install64/bin/protoc.exe
```

Just run;

```sh
cp -r build/deps/win/vs2026/protobuf/install32 build/deps/win/vs2026/protobuf/install64
```

Cross compilation can be done so;


```sh
export WINDOWS_SDK_PATH="/opt/msvc"
export PATH=$WINDOWS_SDK_PATH/bin/x86:$PATH
export WINE=$(command -v wine64 || command -v wine || false)

sudo pacman -Syu --noconfirm install -y git clang lld make cmake extra-cmake-modules python wine wine-mono msitools ca-certificates libwbclient 7zip

git clone https://aur.archlinux.org/msvc-wine-git.git
cd msvc-wine-git
makepkg -si --noconfirm
cd ..

# ON NON-ARCH based distros (since you dont have access to yay)
# copy /opt/msvc/cmake/toolchain-x86.cmake & /opt/msvc/cmake/toolchain-x64.cmake from an arch/yay install to $WINDOWS_SDK_PATH/cmake
# Here's a mirror in case aur is down https://github.com/Twig6943/msvc-wine-git
# see https://github.com/mstorsjo/msvc-wine/issues/229 for more info

git clone --recursive https://github.com/alex47exe/gse_fork

cd gse_fork

chmod +x third-party/common/linux/premake/premake5
chmod +x third-party/deps/linux/7za/7za
chmod +x third-party/deps/linux/cmake/bin/cmake

$WINE wineboot
# x86
./third-party/common/linux/premake/premake5 --file=premake5-deps.lua --32-build --all-ext --all-build --custom-cmake=cmake --cmake-toolchain=$WINDOWS_SDK_PATH/cmake/toolchain-x86.cmake --custom-extractor=7z --j=$(nproc) --os=windows vs2026

export PATH=$WINDOWS_SDK_PATH/bin/x64:$PATH

./third-party/common/linux/premake/premake5 --file=premake5-deps.lua --64-build --all-ext --all-build --custom-cmake=cmake --cmake-toolchain=$WINDOWS_SDK_PATH/cmake/toolchain-x64.cmake --custom-extractor=7z --j=$(nproc) --os=windows vs2026

./third-party/common/linux/premake/premake5 --file=premake5.lua --genproto --os=windows vs2026

cd build/project/vs2026/win
/opt/msvc/bin/x86/msbuild '/nologo' '/v:n' '/p:Configuration=release,Platform=Win32' gse.slnx
/opt/msvc/bin/x64/msbuild '/nologo' '/v:n' '/p:Configuration=release,Platform=x64' gse.slnx
```

See [the ci](./.github/workflows/emu-build-all-win-cross.yml) for more details.
