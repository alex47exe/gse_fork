## :large_orange_diamond: **Goldberg Steam Emu**
Fork of https://gitlab.com/Mr_Goldberg/goldberg_emulator with a lot of fixes, improvements, additional features and a completely reworked file structure.

### Feel free to make a PR.

---

:red_circle:

**This fork is not a takeover, not a resurrection of the original project, and not a replacement.**  
**You are highly encouraged to fork/clone it and do whatever you want with it.**  

:red_circle:

---

## **Compatibility**
This fork is incompatible with the original repo, lots of things has changed and might be even broken.
If something doesn't work, feel free to create a pull request with the appropriate fix, otherwise ignore this fork and use the original emu.

---

## **Credits**
Thanks to everyone contributing to this project in any way possible, we try to keep the [CHANGELOG.md](./CHANGELOG.md) updated with all the changes and their authors.

This project depends on many third-party libraries and tools, credits to them for their amazing work, you can find their listing here in [CREDITS.md](./CREDITS.md).

---

# How to use the emu
* **Always generate the interfaces file using the `generate_interfaces` tool.**
* **If things don't work, try the `ColdClientLoader` setup.**

You can find some guides, helper tools and scripts here:

**These guides, tools and scripts are maintained by their authors. 
Before using them, it's always a good idea to first make sure they are updated and designed to support all features of this fork of the emulator.**

* **[gbe_fork_tools](https://github.com/Detanup01/gbe_fork_tools)**
* **[gen.emu.sharp](https://github.com/otavepto/gen.emu.sharp)**
* **[gse_fork_tools](https://github.com/alex47exe/gse_fork_tools)**
* **[Semuexec](https://gitlab.com/detiam/Semuexec)**
* **[Steam Emu Utility](https://github.com/turusudiro/SteamEmuUtility)**
* **[How to use Goldberg Emulator](https://rentry.co/goldberg_emulator)**
* **[GSE-Generator](https://github.com/brunolee-GIT/GSE-Generator)**
* **If you created a generator tool create a Feature PR**

You can also find instructions here in [README.release.md](./post_build/README.release.md)

### Included tools

Tools bundled in this repository:

* **[generate_interfaces](./tools/generate_interfaces/)** — scans the game’s `steam_api(64).dll` / `libsteam_api.so` and writes the required `steam_interfaces.txt`
* **[lobby_connect](./tools/lobby_connect/)** — discovers LAN peers and launches your game with the right join parameters
* **[steam_stats_converter](./tools/steam_stats_converter/)** — converts between Steam’s local achievements/stats binary cache and GSE save files; see its [README](./tools/steam_stats_converter/README.md)
* **[steamclient_loader](./tools/steamclient_loader/linux/)** — Linux launcher for the experimental `steamclient.so` / `steamclient64.so` build, taking the exe path, appid and optional working directory from the command line or `ldr_*.txt` files; see its [README](./tools/steamclient_loader/linux/README.md)

---
# **Compiling**
## One time setup
### **Cloning the repo**

 Clone the repo and its submodules **recursively**
 ```shell
 git clone --recurse-submodules -j8 https://github.com/alex47exe/gse_fork.git
 ```
 The switch `-j8` is optional, it allows Git to fetch up to 8 submodules

 It is advisable to always checkout submodules every now and then, to make sure they're up to date
 ```shell
 git submodule update --init --recursive --remote
 ```

### For Windows:
* You need Windows 10 or 8.1 + WDK
* Using Visual Studio, install `Visual Studio 2022 Community`: https://visualstudio.microsoft.com/vs/community/
   * Select the Workload `Desktop development with C++`
   * In the `Individual components` scroll to the bottom and select the **latest** version of `Windows XX SDK (XX.X...)`  
      For example `Windows 11 SDK (10.0.22621.0)`
* Using `MSYS2` **this is currently experimental and will not work due to ABI differences**: https://www.msys2.org/
  <details>
    <summary>steps</summary>
  
    * To build 64-bit binaries use either the [environment](https://www.msys2.org/docs/environments/) `UCRT64` or `MINGW64` then install the GCC toolchain  
      `UCRT64`  
      ```shell
      pacman -S mingw-w64-ucrt-x86_64-gcc
      ```
      `MINGW64`
      ```shell
      pacman -S mingw-w64-i686-gcc
      ```
    * To build 32-bit binaries use the environment `MINGW32` then install the GCC toolchain
      ```shell
      pacman -S mingw-w64-i686-gcc
      ```
  
  </details> 
* Python 3.10 or above: https://www.python.org/downloads/windows/  
   After installation, make sure it works
   ```batch
   python --version
   ```
* *(Optional)* Install a GUI for Git like [GitHub Desktop](https://desktop.github.com/), or [Sourcetree](https://www.sourcetreeapp.com/)

### For Linux:

* Ubuntu 22.04 LTS: https://ubuntu.com/download/desktop
* Ubuntu required packages:
  ```shell
  sudo apt update -y
  sudo apt install -y coreutils # echo, printf, etc...
  sudo apt install -y build-essential
  sudo apt install -y gcc-multilib # needed for 32-bit builds
  sudo apt install -y g++-multilib
  sudo apt install -y libglx-dev # needed for overlay build (header files   such as GL/glx.h)
  sudo apt install -y libgl-dev # needed for overlay build (header files   such as GL/gl.h)
  ```
  *(Optional)* Additional packages
  ```shell
  sudo apt install -y clang # clang compiler
  sudo apt install -y binutils # contains the tool 'readelf' mainly, and   other usefull binary stuff
  ```
* Python 3.10 or above
   ```shell
   sudo apt update -y
   sudo apt install -y software-properties-common
   sudo add-apt-repository ppa:deadsnakes/ppa -y
   sudo apt update -y
   sudo apt install -y "python3.12"
   sudo apt install -y "python3.12-dev"
   sudo apt install -y "python3.12-venv"
   sudo apt install -y python3-dev

   # make sure it works
   python3.12 --version
   ```

### For cross compiling see [the docs](./dev.notes)

### **Building dependencies**

These are third party libraries needed to build the emu later, they are linked with the emu during its build process.  
You don't need to build these dependencies every time, they rarely get updated.  
The only times you'll need to rebuild them is either when their separate build folder was accidentally deleted, or when the dependencies were updated.  

<br/>

#### On Windows:
Open CMD in the repo folder, then run the following
* To build using `Visual Studio`
  ```batch
  set "CMAKE_GENERATOR=Visual Studio 18 2026"
  third-party\common\win\premake\premake5.exe --file=premake5-deps.lua --64-build --32-build   --all-ext --all-build --verbose --os=windows vs2026
  ```
* To build using `MSYS2` **this is currently experimental and will not work due to ABI differences**
  <details>
    <summary>steps</summary>
  
    *(Optional)* In both cases below, you can use `Clang` compiler instead of `GCC` by running these 2 commands in the same terminal instance
    ```shell
    export CC="clang"
    export CXX="clang++"
    ```
    * To build 64-bit binaries (`UCRT64` or `MINGW64`)
    ```shell
    export CMAKE_GENERATOR="MSYS Makefiles"
    ./third-party/common/win/premake/premake5.exe --file=premake5-deps.lua --64-build --all-ext --all-build --verbose   --os=windows gmake2
    ```
    * To build 32-bit binaries (`MINGW32`)
    ```shell
    export CMAKE_GENERATOR="MSYS Makefiles"
    ./third-party/common/win/premake/premake5.exe --file=premake5-deps.lua --32-build --all-ext --all-build --verbose   --os=windows gmake2
    ```
  
  </details> 

This will:
* Extract all third party dependencies from the folder `third-party` into the folder `build\deps\win`
* Build all dependencies

#### On Linux:
Open a terminal in the repo folder
*(Optional)* You can use `Clang` compiler instead of `GCC` by running these 2 commands in the current terminal instance
```shell
export CC="clang"
export CXX="clang++"
```
Then run the following
```shell
export CMAKE_GENERATOR="Unix Makefiles"
./third-party/common/linux/premake/premake5 --file=premake5-deps.lua --64-build --32-build --all-ext --all-build --verbose --os=linux gmake2
```
This will:
* Extract all third party dependencies from the folder `third-party` into the folder `build/deps/linux`
* Build all dependencies (32-bit and 64-bit)

---

## **Building the emu**
### On Windows:
Open CMD in the repo folder, then run the following
* For `Visual Studio 2022`
  ```batch
  third-party\common\win\premake\premake5.exe --file=premake5.lua --genproto --os=windows vs2026
  ```
  You can then go to the folder `build\project\vs2026\win` and open the produced `.sln` file in Visual Studio.
  Or, if you prefer to do it from command line, open the `Developer Command Prompt for VS 2026` inside the above folder, then:
  ```batch
  msbuild /nologo /v:n /p:Configuration=release,Platform=Win32 gse.slnx

  msbuild /nologo /v:n /p:Configuration=release,Platform=x64 gse.slnx
  ```

* For `MSYS2` **this is currently experimental and will not work due to ABI differences**
  <details>
    <summary>steps</summary>
  
    ```shell
    ./third-party/common/win/premake/premake5.exe --file=premake5.lua --genproto --os=windows gmake2
  
    cd ./build/project/gmake2/win
    ```
    *(Optional)* You can use `Clang` compiler instead of `GCC` by running these 2 commands in the current terminal instance
    ```shell
    export CC="clang"
    export CXX="clang++"
    ```
    * 64-bit build (`UCRT64` or `MINGW64`)
      ```shell
      make config=release_x64 -j 8 all
      ```
    * 32-bit build (`MINGW32`)
      ```shell
      make config=release_x86 -j 8 all
      ```
      To see all possible build targets
    ```shell
    make help
    ```
  
  </details> 

  </details>

This will build a release version of the emu in the folder `build\win\<toolchain>\release`
An example script `build_win_premake.bat` is available, check it out

<br/>

### On Linux:
Open a terminal in the repo folder, then run the following
```shell
./third-party/common/linux/premake/premake5 --file=premake5.lua --genproto --os=linux gmake2
cd ./build/project/gmake2/linux
```
*(Optional)* You can use `Clang` compiler instead of `GCC` by running these 2 commands in the current terminal instance
```shell
export CC="clang"
export CXX="clang++"
```
Then run the following
```shell
make config=release_x86 -j 8 all
make config=release_x64 -j 8 all
```

To see all possible build targets
```shell
make help
```

This will build a release version of the emu in the folder `build/linux/<toolchain>/release`
An example script `build_linux_premake.sh` is available, check it out

---

## **Using Github CI as a builder**

This is really slow and mainly intended for the CI Workflow scripts, but you can use it as another outlet if you can't build locally.
**You have to fork the repo first**.

### Initial setup
In your fork, open the `Settings` tab from the top, then:
* From the left side panel select `Actions` -> `General`
* In the section `Actions permissions` select `Allow all actions and reusable workflows`
* Scroll down, and in the section `Workflow permissions` select `Read and write permissions`
* *(Optional)* In the section `Artifact and log retention`, you can specify the amount of days to keep the build artifacts/archives.
  It is recommended to set a reasonable number like 3-4 days, otherwise you may consume your packages storage if you use Github as a builder frequently, more details here: https://docs.github.com/en/get-started/learning-about-github/githubs-plans

### Manual trigger
1. Go to the `Actions` tab in your fork
2. Select the emu dependencies Workflow (ex: `Emu deps (Windows, VS2026, x64/Win32)`) and run it on the **main** branch (ex: `dev`).
   Dependencies not created on the main branch won't be recognized by other branches or subsequent runs
3. Select one of the Workflow scripts from the left side panel, for example `Build emu (Windows, VS2026, x64/Win32)`
3. On the top-right, select `Run workflow` -> select the desired branch (for example `dev`) -> press the button `Run workflow`
4. When it's done, many packages (called build artifacts) will be created for that workflow.
   Make sure to select the workflow again to view its history, then select the last run at the very top to view its artifacts

The `Prepare release` workflow has a `vs_version` input to pick which Windows toolchain to build:
`vs2026` (default), `vs2022`, or `both`. With `both`, each toolchain is packaged separately and the
release assets are suffixed with the toolchain (`emu-win-release-vs2026.7z`, `emu-win-release-vs2022.7z`).
Windows ARM64 is **VS2026-only**: it runs on the `windows-11-vs2026-arm` runner and its dependencies are
built with `--arm-build` using the `Visual Studio 18 2026` generator, which the VS2022 pipeline does not provide.

<br/>

Important note:
---

When you build the dependencies workflows, they will be cached to decrease the build times of the next triggers and avoid unnecessary/wasteful build process.
This will cause a problem if at any time the third-party dependencies were updated, in that case you need to manually delete the cache, in your fork:
1. Go to the `Actions` tab at the top
2. Select `Caches` from the left side panel
3. Delete the corresponding cache

<br/>

---

## ***(Optional)* Packaging**
This step is intended for Github CI/Workflow, but you can create a package locally.

### On Windows:
Open CMD in the repos's directory, then run this script
```batch
package_win.bat <build_folder>
```
`build_folder` is any folder inside `build\win`, for example: `vs2026\release`
The above example will create a `.7z` archive inside `build\package\win\`

### On Linux:
Open bash terminal in the repos's directory, then run this script
```shell
package_linux.sh <build_folder>
```
`build_folder` is any folder inside `build/linux`, for example: `gmake2/release`
The above example will create a compressed `.tar` archive inside `build/package/linux/`
