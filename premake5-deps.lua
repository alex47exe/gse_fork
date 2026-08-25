require("premake", ">=5.0.0-beta8")

-- don't forget to set env var CMAKE_GENERATOR to one of these values:
-- https://cmake.org/cmake/help/latest/manual/cmake-generators.7.html#manual:cmake-generators(7)
-- common ones
-- ============
-- Unix Makefiles
-- Visual Studio 18 2026
-- MSYS Makefiles

local os_iden = '' -- identifier
if os.target() == "windows" then
    os_iden = 'win'
elseif os.target() == "linux" then
    os_iden = 'linux'
else
    error('Unsupported os target: "' .. os.target() ..'"')
end


-- options
---------
-- general
newoption {
    category = "general",
    trigger = "verbose",
    description = "Verbose output",
}
newoption {
    category = "general",
    trigger = "clean",
    description = "Cleanup before any action",
}
-- tools
newoption {
    category = "tools",
    trigger = "custom-cmake",
    description = "Use custom cmake",
    value = 'path/to/cmake.exe',
    default = nil
}

newoption {
    category = "tools",
    trigger = "cmake-toolchain",
    description = "Use cmake toolchain",
    value = 'path/to/toolchain.cmake',
    default = nil
}

newoption {
    category = "tools",
    trigger = "custom-extractor",
    description = "Use custom extractor",
    value = 'path/to/7z.exe',
    default = nil
}

-- deps extraction
newoption {
    category = "extract",
    trigger = "all-ext",
    description = "Extract all deps",
}
newoption {
    category = "extract",
    trigger = "ext-ssq",
    description = "Extract libssq",
}
newoption {
    category = "extract",
    trigger = "ext-zlib",
    description = "Extract zlib",
}
newoption {
    category = "extract",
    trigger = "ext-curl",
    description = "Extract curl",
}
newoption {
    category = "extract",
    trigger = "ext-protobuf",
    description = "Extract protobuf",
}
newoption {
    category = "extract",
    trigger = "ext-mbedtls",
    description = "Extract mbedtls",
}
newoption {
    category = "extract",
    trigger = "ext-ingame_overlay",
    description = "Extract ingame_overlay",
}
newoption {
    category = "extract",
    trigger = "ext-opus",
    description = "Extract opus",
}
newoption {
    category = "extract",
    trigger = "ext-portaudio",
    description = "Extract portaudio",
}
newoption {
    category = "extract",
    trigger = "ext-sdl",
    description = "Extract sdl",
}

-- deps build
newoption {
    category = "build",
    trigger = "all-build",
    description = "Build all deps",
}
newoption {
    category = "build",
    trigger = "build-ssq",
    description = "Build libssq",
}
newoption {
    category = "build",
    trigger = "build-zlib",
    description = "Build zlib",
}
newoption {
    category = "build",
    trigger = "build-curl",
    description = "Build curl",
}
newoption {
    category = "build",
    trigger = "build-protobuf",
    description = "Build protobuf",
}
newoption {
    category = "build",
    trigger = "build-mbedtls",
    description = "Build mbedtls",
}
newoption {
    category = "build",
    trigger = "build-ingame_overlay",
    description = "Build ingame_overlay",
}
newoption {
    category = "build",
    trigger = "build-opus",
    description = "Build opus",
}
newoption {
    category = "build",
    trigger = "build-portaudio",
    description = "Build portaudio",
}
newoption {
    category = "build",
    trigger = "build-sdl",
    description = "Build sdl",
}
newoption {
    category = "build",
    trigger = "32-build",
    description = "Build for 32-bit arch",
}
newoption {
    category = "build",
    trigger = "64-build",
    description = "Build for 64-bit arch",
}
newoption {
    category = "build",
    trigger = "debug-build",
    description = "Build dependencies in Debug and enable ingame_overlay trace logging",
}

newoption {
    category = "build",
    trigger = "j",
    description = "Parallel jobs for cmake build",
    value = 'number',
    default = nil
}

newoption {
    category = "build",
    trigger = "deps-dir",
    description = "output dir for all built deps",
    value = 'path/to/output/dir',
    default = nil
}

if not _OPTIONS["deps-dir"] then
    error('you must provide the --deps-dir option')
end


local function table_copy(src, dest)
    local src_count = #src
    local res = {}

    for idx = 1, src_count do
        res[idx] = src[idx]
    end

    for idx = 1, #dest do
        res[src_count + idx] = dest[idx]
    end
    return res
end


if _OPTIONS['j'] and not string.match(_OPTIONS['j'], '^[1-9]+$') then
    error("Invalid argument for --j")
end


-- common defs
---------
local deps_dir = _OPTIONS["deps-dir"]
local third_party_dir = path.getabsolute('third-party')
local third_party_deps_dir = path.join(third_party_dir, 'deps', os_iden)
local third_party_common_dir = path.join(third_party_dir, 'deps', 'common')
local extractor = os.realpath(path.join(third_party_deps_dir, '7za', '7za'))
local mycmake = os.realpath(path.join(third_party_deps_dir, 'cmake', 'bin', 'cmake'))

if _OPTIONS["custom-cmake"] then
    mycmake = _OPTIONS["custom-cmake"]
    print('using custom cmake: ' .. _OPTIONS["custom-cmake"])
else
    if os.host() == 'windows' then
        mycmake = mycmake .. '.exe'
    end
    if not os.isfile(mycmake) then
        error('cmake is missing from third-party dir, you can specify custom cmake location, run the script with --help. cmake: ' .. mycmake)
    end
end

if not third_party_dir or not os.isdir(third_party_dir) then
    error('third-party dir is missing')
end

if _OPTIONS["custom-extractor"] then
    extractor = _OPTIONS["custom-extractor"]
    print('using custom extractor: ' .. _OPTIONS["custom-extractor"])
else
    if os.host() == 'windows' then
        extractor = extractor .. '.exe'
    end
    if not extractor or not os.isfile(extractor) then
        error('extractor is missing from third-party dir. extractor: ' .. extractor)
    end
end


-- ############## common CMAKE args ##############
-- https://cmake.org/cmake/help/latest/variable/CMAKE_LANG_FLAGS_CONFIG.html#variable:CMAKE_%3CLANG%3E_FLAGS_%3CCONFIG%3E
local deps_build_config = _OPTIONS["debug-build"] and "Debug" or "Release"
local cmake_common_defs = {
    'CMAKE_BUILD_TYPE=' .. deps_build_config,
    'CMAKE_POSITION_INDEPENDENT_CODE=True',
    'BUILD_SHARED_LIBS=OFF',
    'CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
    'CMAKE_INSTALL_LIBDIR=lib',     -- |
    'CMAKE_INSTALL_BINDIR=bin',         -- |_ ensure consistency on different Linux distros
    'CMAKE_INSTALL_INCLUDEDIR=include', -- |_ ensure consistency on different Linux distros
}


local function cmake_build(dep_folder, is_32, extra_cmd_defs, c_flags_init, cxx_flags_init)
    local dep_base = path.getabsolute(path.join(deps_dir, dep_folder))
    local arch_iden = ''
    if is_32 then
        arch_iden = '32'
    else
        arch_iden = '64'
    end

    print('\n\nbuilding dep: "' .. dep_base .. '"')
    
    local build_dir = path.getabsolute(path.join(dep_base, 'build' .. arch_iden))
    local install_dir = path.join(dep_base, 'install' .. arch_iden)
    
    -- clean if required
    if _OPTIONS["clean"] then
        print('cleaning dir: ' .. build_dir)
        os.rmdir(build_dir)
        print('cleaning dir: ' .. install_dir)
        os.rmdir(install_dir)
    end

    if not os.mkdir(build_dir) then
        error("failed to create build dir: " .. build_dir)
        return
    end

    local cmake_common_defs_str = '-D' .. table.concat(cmake_common_defs, ' -D') .. ' -DCMAKE_INSTALL_PREFIX="' .. install_dir .. '"'
    local cmd_gen = mycmake .. ' -S "' .. dep_base .. '" -B "' .. build_dir .. '" ' .. cmake_common_defs_str

    local all_cflags_init = {}
    local all_cxxflags_init = {}

    -- c/cxx init flags based on arch/action
    if string.match(_ACTION, 'gmake.*') then
        if is_32 then
            table.insert(all_cflags_init, '-m32')
            table.insert(all_cxxflags_init, '-m32')
        end
    elseif string.match(_ACTION, 'vs.+') then
        -- these 2 are needed because mbedtls doesn't care about 'CMAKE_MSVC_RUNTIME_LIBRARY' for some reason
        table.insert(all_cflags_init, '/MT')
        table.insert(all_cflags_init, '/D_MT')

        table.insert(all_cxxflags_init, '/MT')
        table.insert(all_cxxflags_init, '/D_MT')

        local cmake_generator = os.getenv("CMAKE_GENERATOR") or ""
        if cmake_generator == "" and os.host() == 'windows' or cmake_generator:find("Visual Studio") then
            if is_32 then
                cmd_gen = cmd_gen .. ' -A Win32'
            else
                cmd_gen = cmd_gen .. ' -A x64'
            end
        end
    else
        error("unsupported action for cmake build: " .. _ACTION)
        return
    end
    
    -- add c/cxx extra init flags
    if c_flags_init then
        if type(c_flags_init) ~= 'table' then
            error("unsupported type for c_flags_init: " .. type(c_flags_init))
            return
        end
        for _, cval in pairs(c_flags_init) do
            table.insert(all_cflags_init, cval)
        end
    end
    if cxx_flags_init then
        if type(cxx_flags_init) ~= 'table' then
            error("unsupported type for cxx_flags_init: " .. type(cxx_flags_init))
            return
        end
        for _, cval in pairs(cxx_flags_init) do
            table.insert(all_cxxflags_init, cval)
        end
    end
    -- convert to space-delimited str
    local cflags_init_str = ''
    if #all_cflags_init > 0 then
        cflags_init_str = table.concat(all_cflags_init, " ")
    end
    local cxxflags_init_str = ''
    if #all_cxxflags_init > 0 then
        cxxflags_init_str = table.concat(all_cxxflags_init, " ")
    end

    -- write toolchain file
    local toolchain_file_content = ''
    if _OPTIONS["cmake-toolchain"] then
        toolchain_file_content = 'include(' .. _OPTIONS["cmake-toolchain"] .. ')\n\n'
    end
    if #cflags_init_str > 0 then
        toolchain_file_content = toolchain_file_content .. 'set(CMAKE_C_FLAGS_INIT "' .. cflags_init_str .. '" )\n'
    end
    if #cxxflags_init_str > 0 then
        toolchain_file_content = toolchain_file_content .. 'set(CMAKE_CXX_FLAGS_INIT "' .. cxxflags_init_str .. '" )\n'
    end
    if string.match(_ACTION, 'vs.+') then -- because libssq doesn't care about CMAKE_C/XX_FLAGS_INIT
        toolchain_file_content = toolchain_file_content .. 'set(CMAKE_C_FLAGS_RELEASE  "${CMAKE_C_FLAGS_RELEASE} /MT /D_MT" ) \n'
        toolchain_file_content = toolchain_file_content .. 'set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT /D_MT" ) \n'
    end
    
    if #toolchain_file_content > 0 then
        local toolchain_file = path.join(dep_base, 'toolchain_' .. tostring(is_32) .. '_' .. _ACTION .. '_' .. os_iden .. '.precmt')
        if not io.writefile(toolchain_file, toolchain_file_content) then
            error("failed to write cmake toolchain")
            return
        end
        cmd_gen = cmd_gen .. ' -DCMAKE_TOOLCHAIN_FILE="' .. toolchain_file .. '"'
    end

    -- add extra defs
    if extra_cmd_defs then
        if type(extra_cmd_defs) ~= 'table' then
            error("unsupported type for extra_cmd_defs: " .. type(extra_cmd_defs))
            return
        end
        if #extra_cmd_defs > 0 then
            local extra_defs_str = ' -D' .. table.concat(extra_cmd_defs, ' -D')
            cmd_gen = cmd_gen .. extra_defs_str
        end
    end

    -- verbose generation
    if _OPTIONS['verbose'] then
        print(cmd_gen)
    end

    -- generate cmake config
    local ok = os.execute(cmd_gen)
    if not ok then
        error("failed to generate")
        return
    end

    -- verbose build
    local verbose_build_str = ''
    if _OPTIONS['verbose'] then
        verbose_build_str = ' -v'
    end

    -- build with cmake
    local parallel_str = ' --parallel'
    if _OPTIONS['j'] then
        parallel_str = parallel_str .. ' ' .. _OPTIONS['j']
    end
    local ok = os.execute(mycmake .. ' --build "' .. build_dir .. '" --config ' .. deps_build_config .. parallel_str .. verbose_build_str)
    if not ok then
        error("failed to build")
        return
    end

    -- create dir
    if not os.mkdir(install_dir) then
        error("failed to create install dir: " .. install_dir)
        return
    end

    local cmd_install = mycmake.. ' --install "' .. build_dir .. '" --prefix "' .. install_dir .. '"'
    print(cmd_install)
    local ok = os.execute(cmd_install)
    if not ok then
        error("failed to install")
        return
    end
end

-- chmod tools
if os.host() == "linux" then
    if not _OPTIONS["custom-extractor"] then
        local ok_chmod, err_chmod = os.chmod(extractor, "777")
        if not ok_chmod then
            error("cannot chmod: " .. err_chmod)
            return
        end
    end
    if not _OPTIONS["custom-cmake"] then
        local ok_chmod, err_chmod = os.chmod(mycmake, "777")
        if not ok_chmod then
            error("cannot chmod: " .. err_chmod)
            return
        end
    end
end


-- extract action
-------
deps_to_extract = {} -- { 'path/to/archive', 'path/to/extraction folder'
if _OPTIONS["ext-ssq"] or _OPTIONS["all-ext"] then
    table.insert(deps_to_extract, { 'libssq/libssq.tar.gz', 'libssq' })
end
if _OPTIONS["ext-zlib"] or _OPTIONS["all-ext"] then
    table.insert(deps_to_extract, { 'zlib/zlib.tar.gz', 'zlib' })
end
if _OPTIONS["ext-curl"] or _OPTIONS["all-ext"] then
    table.insert(deps_to_extract, { 'curl/curl.tar.gz', 'curl' })
end
if _OPTIONS["ext-protobuf"] or _OPTIONS["all-ext"] then
    table.insert(deps_to_extract, { 'protobuf/protobuf.tar.gz', 'protobuf' })
end
if _OPTIONS["ext-mbedtls"] or _OPTIONS["all-ext"] then
    table.insert(deps_to_extract, { 'mbedtls/mbedtls.tar.gz', 'mbedtls' })
end
if _OPTIONS["ext-ingame_overlay"] or _OPTIONS["all-ext"] then
    -- NOTE: after replacing ingame_overlay.tar.gz with a newer upstream snapshot,
    -- reapply these local patches manually:
    --   1) sRGB format detection patch
    --   2) FP16 texture support patch
    --   3) DXGI swap-chain pointer exposure patch
    -- and then update the patch tracking documentation with any conflict resolutions/adaptations.
    table.insert(deps_to_extract, { 'ingame_overlay/ingame_overlay.tar.gz', 'ingame_overlay' })
end
if _OPTIONS["ext-opus"] or _OPTIONS["all-ext"] then
    table.insert(deps_to_extract, { 'opus/opus.tar.gz', 'opus' })
end
if _OPTIONS["ext-portaudio"] or _OPTIONS["all-ext"] then
    table.insert(deps_to_extract, { 'portaudio/portaudio.tar.gz', 'portaudio' })
end
if _OPTIONS["ext-sdl"] or _OPTIONS["all-ext"] then
    table.insert(deps_to_extract, { 'sdl/sdl.tar.gz', 'sdl' })
end

-- apply ingame_overlay .patch files from tools/ingame_overlay_patches/
-- patch files must be named NN-<description>.patch (e.g. 01-srgb-detection.patch) so they
-- are applied in sorted order via 'git apply'
local function apply_ingame_overlay_patches()
    local patches_dir = path.join(third_party_dir, '..', 'tools', 'ingame_overlay_patches')
    patches_dir = path.getabsolute(patches_dir)
    if not os.isdir(patches_dir) then
        print('ingame_overlay patches directory not found, skipping: ' .. patches_dir)
        return
    end

    local overlay_dir = path.join(deps_dir, 'ingame_overlay')
    local patches = os.matchfiles(patches_dir .. '/*.patch')
    if #patches == 0 then
        print('no .patch files found in: ' .. patches_dir)
        return
    end

    -- apply in sorted (numbered) order
    table.sort(patches)
    for _, patch_file in ipairs(patches) do
        print('\napplying ingame_overlay patch: ' .. patch_file)
        local ok = os.execute('git -C "' .. overlay_dir .. '" apply --whitespace=nowarn "' .. patch_file .. '"')
        if not ok then
            error('patch application failed: ' .. patch_file)
        end
    end
    print('all ingame_overlay patches applied')
end

-- start extraction
for _, dep in pairs(deps_to_extract) do
    -- check archive
    local archive_file = path.join(third_party_common_dir, dep[1])
    print('\n\nextracting dep archive: "' .. archive_file .. '"')

    if not os.isfile(archive_file) then
        error("archive not found: " .. archive_file)
        return
    end

    local out_folder = path.join(deps_dir, dep[2])

    -- clean if required
    if _OPTIONS["clean"] then
        print('cleaning dir: ' .. out_folder)
        os.rmdir(out_folder)
    end

    -- create out folder
    print("creating dir: " .. out_folder)
    local ok_mk, err_mk = os.mkdir(out_folder)
    if not ok_mk then
        error("Error: " .. err_mk)
        return
    end

    -- extract
    print("extracting: '" .. archive_file .. "'")
    local ext = string.lower(string.sub(archive_file, -7)) -- ".tar.gz"
    local ok_cmd = false
    if ext == ".tar.gz" then
        ok_cmd = os.execute(extractor .. ' -bso0 -bse2 x "' .. archive_file .. '" -so | "' .. extractor .. '" -bso0 -bse2 x -si -ttar -y -aoa -o"' .. deps_dir .. '"')
    else
        ok_cmd = os.execute(extractor .. ' -bso0 -bse2 x "' .. archive_file .. '" -y -aoa -o"' .. out_folder .. '"')
    end
    if not ok_cmd then
        error('extraction failed')
    end

    -- flatten dir by moving all folders contents outside (one level above)
    -- print('flattening dir: ' .. out_folder)
    -- local folders = os.matchdirs(out_folder .. '/*')
    -- for _, inner_folder in pairs(folders) do
    --     -- the weird "/*" at the end is not a mistake, premake uses cp cpmmand on linux, which won't copy inner dir otherwise
    --     local ok = os.execute('{COPYDIR} "' .. inner_folder  .. '"/* "' .. out_folder .. '"')
    --     if not ok then
    --         error('copy dir failed, src=' .. inner_folder .. ', dest=' .. out_folder)
    --     end
    --     os.rmdir(inner_folder)
    -- end

end


-- apply ingame_overlay patches after extraction
if _OPTIONS["ext-ingame_overlay"] or _OPTIONS["all-ext"] then
    apply_ingame_overlay_patches()
end

-- build action
-------
if _OPTIONS["build-ssq"] or _OPTIONS["all-build"] then
    if _OPTIONS["32-build"] then
        cmake_build('libssq', true)
    end
    if _OPTIONS["64-build"] then
        cmake_build('libssq', false)
    end
end
if _OPTIONS["build-zlib"] or _OPTIONS["all-build"] then
    local zlib_common_defs = {
        "ZLIB_BUILD_EXAMPLES=OFF",
    }
    if _OPTIONS["32-build"] then
        cmake_build('zlib', true, zlib_common_defs)
    end
    if _OPTIONS["64-build"] then
        cmake_build('zlib', false, zlib_common_defs)
    end
end
if _OPTIONS["build-curl"] or _OPTIONS["all-build"] then
    -- curl requires zlib
    -- https://github.com/curl/curl/blob/curl-7_88_1/CMakeLists.txt#L531-L569
    --       # Depend on ZLIB via imported targets if supported by the running
    --       # CMake version, otherwise fall through to the 
    --       # ZLIB_FOUND/ZLIB_INCLUDE_DIRS/ZLIB_LIBRARIES approach
    --       if(ZLIB_FOUND)
    --         target_link_libraries(${LIB_NAME} PRIVATE ZLIB::ZLIB)
    --         if(LIBCURL_ONLY_SHARED AND UNIX)
    --           install(TARGETS ZLIB::ZLIB ...)
    --         endif()
    --       else()
    --         target_include_directories(${LIB_NAME} PRIVATE ${ZLIB_INCLUDE_DIRS})
    --         target_link_libraries(${LIB_NAME} PRIVATE ${ZLIB_LIBRARIES})
    --       endif()
    local wild_zlib_name = 'zlibstatic*.lib'
    if os.target() ~= 'windows' then
        wild_zlib_name = 'libz*.a'
    end

    local wild_zlib_path_32 = path.join(deps_dir, 'zlib', 'install32', 'lib', wild_zlib_name)
    local zlib_defs_32 = {
        'ZLIB_ROOT="' .. path.join(deps_dir, 'zlib', 'install32') .. '"',
        'ZLIB_INCLUDE_DIR="' .. path.join(deps_dir, 'zlib', 'install32', 'include') .. '"',
        'ZLIB_LIBRARY="' .. wild_zlib_path_32 .. '"',
    }

    local wild_zlib_path_64 = path.join(deps_dir, 'zlib', 'install64', 'lib', wild_zlib_name)
    local zlib_defs_64 = {
        'ZLIB_ROOT="' .. path.join(deps_dir, 'zlib', 'install64') .. '"',
        'ZLIB_INCLUDE_DIR="' .. path.join(deps_dir, 'zlib', 'install64', 'include') .. '"',
        'ZLIB_LIBRARY="' .. wild_zlib_path_64 .. '"',
    }

    local curl_common_defs = {
        "BUILD_CURL_EXE=OFF",
        "BUILD_TESTING=OFF",
        "CURL_DISABLE_INSTALL=OFF",
        "CURL_USE_BEARSSL=OFF",
        "CURL_USE_GNUTLS=OFF",
        "CURL_USE_LIBPSL=OFF",
        "CURL_USE_LIBSSH=OFF",
        "CURL_USE_LIBSSH2=OFF",
        "CURL_USE_MBEDTLS=ON",
        "CURL_USE_NSS=OFF",
        "CURL_USE_OPENSSL=OFF",
        "CURL_USE_SCHANNEL=OFF",
        "CURL_USE_WOLFSSL=OFF",
        "CURL_ZLIB=ON",
        'MBEDTLS_INCLUDE_DIRS="' .. path.join(deps_dir, 'mbedtls', 'install32', 'include') .. '"',
        'MBEDTLS_LIBRARY="' .. path.join(deps_dir, 'mbedtls', 'install32', 'lib', 'mbedtls*.lib') .. '"',
        'MBEDCRYPTO_LIBRARY="' .. path.join(deps_dir, 'mbedtls', 'install32', 'lib', 'mbedcrypto*.lib') .. '"',
        'MBEDX509_LIBRARY="' .. path.join(deps_dir, 'mbedtls', 'install32', 'lib', 'mbedx509*.lib') .. '"',
    }
    if os.target() == 'windows' then
        table.insert(curl_common_defs, 'CURL_USE_SCHANNEL=ON')
        table.insert(curl_common_defs, 'CURL_USE_MBEDTLS=OFF')
    end

    if _OPTIONS["32-build"] then
        local mbedtls_name = 'mbedtls*.lib'
        local mbedcrypto_name = 'mbedcrypto*.lib'
        local mbedx509_name = 'mbedx509*.lib'
        if os.target() ~= 'windows' then
            mbedtls_name = 'libmbedtls*.a'
            mbedcrypto_name = 'libmbedcrypto*.a'
            mbedx509_name = 'libmbedx509*.a'
        end
        local curl_32_defs = table_copy(zlib_defs_32, {
            'MBEDTLS_INCLUDE_DIRS="' .. path.join(deps_dir, 'mbedtls', 'install32', 'include') .. '"',
            'MBEDTLS_LIBRARY="' .. path.join(deps_dir, 'mbedtls', 'install32', 'lib', mbedtls_name) .. '"',
            'MBEDCRYPTO_LIBRARY="' .. path.join(deps_dir, 'mbedtls', 'install32', 'lib', mbedcrypto_name) .. '"',
            'MBEDX509_LIBRARY="' .. path.join(deps_dir, 'mbedtls', 'install32', 'lib', mbedx509_name) .. '"',
        })
        cmake_build('curl', true, table_copy(curl_common_defs, curl_32_defs))
    end
    if _OPTIONS["64-build"] then
        local mbedtls_name = 'mbedtls*.lib'
        local mbedcrypto_name = 'mbedcrypto*.lib'
        local mbedx509_name = 'mbedx509*.lib'
        if os.target() ~= 'windows' then
            mbedtls_name = 'libmbedtls*.a'
            mbedcrypto_name = 'libmbedcrypto*.a'
            mbedx509_name = 'libmbedx509*.a'
        end
        local curl_64_defs = table_copy(zlib_defs_64, {
            'MBEDTLS_INCLUDE_DIRS="' .. path.join(deps_dir, 'mbedtls', 'install64', 'include') .. '"',
            'MBEDTLS_LIBRARY="' .. path.join(deps_dir, 'mbedtls', 'install64', 'lib', mbedtls_name) .. '"',
            'MBEDCRYPTO_LIBRARY="' .. path.join(deps_dir, 'mbedtls', 'install64', 'lib', mbedcrypto_name) .. '"',
            'MBEDX509_LIBRARY="' .. path.join(deps_dir, 'mbedtls', 'install64', 'lib', mbedx509_name) .. '"',
        })
        cmake_build('curl', false, table_copy(curl_common_defs, curl_64_defs))
    end
end
if _OPTIONS["build-protobuf"] or _OPTIONS["all-build"] then
    local protobuf_common_defs = {
        "protobuf_BUILD_TESTS=OFF",
        "protobuf_BUILD_EXAMPLES=OFF",
        "protobuf_BUILD_LIBPROTOC=OFF",
        "protobuf_BUILD_PROTOC_BINARIES=ON",
        "protobuf_MSVC_STATIC_RUNTIME=ON",
        "protobuf_WITH_ZLIB=OFF",
    }
    if _OPTIONS["32-build"] then
        cmake_build('protobuf', true, protobuf_common_defs)
    end
    if _OPTIONS["64-build"] then
        cmake_build('protobuf', false, protobuf_common_defs)
    end
end
if _OPTIONS["build-mbedtls"] or _OPTIONS["all-build"] then
    local mbedtls_common_defs = {
        "ENABLE_TESTING=OFF",
        "ENABLE_PROGRAMS=OFF",
        "MBEDTLS_FATAL_WARNINGS=OFF",
    }
    if _OPTIONS["32-build"] then
        cmake_build('mbedtls', true, mbedtls_common_defs)
    end
    if _OPTIONS["64-build"] then
        cmake_build('mbedtls', false, mbedtls_common_defs)
    end
end
if _OPTIONS["build-ingame_overlay"] or _OPTIONS["all-build"] then
    -- fixes 32-bit compilation of DX12
    local overaly_imgui_cfg_file = path.join(deps_dir, 'ingame_overlay', 'imconfig.imcfg')
    if not io.writefile(overaly_imgui_cfg_file, [[
        #pragma once
        #define ImTextureID ImU64
    ]]) then
        error('failed to create ImGui config file for overlay: ' .. overaly_imgui_cfg_file)
    end

    local ingame_overlay_common_defs = {
        'IMGUI_USER_CONFIG="' .. overaly_imgui_cfg_file:gsub('\\', '/') .. '"', -- ensure we use '/' because this lib doesn't handle it well
        'INGAMEOVERLAY_USE_SYSTEM_LIBRARIES=OFF',
        'INGAMEOVERLAY_USE_SPDLOG=' .. (_OPTIONS["debug-build"] and 'ON' or 'OFF'),
        'INGAMEOVERLAY_LOG_LEVEL=' .. (_OPTIONS["debug-build"] and 'trace' or 'off'),
        'INGAMEOVERLAY_BUILD_TESTS=OFF',
        'INGAMEOVERLAY_DYNAMIC_RUNTIME=OFF',
        --'USE_MSVC_RUNTIME_LIBRARY_DLL=OFF', -- Should we?
    }
    -- fix missing standard include/header file for gcc/clang
    local ingame_overlay_fixes = {}
    if string.match(_ACTION, 'gmake.*') then
        -- MinGW fixes
        if os.target() == 'windows' then
            -- MinGW doesn't define _M_AMD64 or _M_IX86, which makes SystemDetector.h fail to recognize os
            -- MinGW throws this error: Filesystem.cpp:139:38: error: no matching function for call to 'stat::stat(const char*, stat*)'
            table.insert(ingame_overlay_fixes, '-include sys/stat.h')
            -- MinGW throws this error: Library.cpp:77:26: error: invalid conversion from 'FARPROC' {aka 'long long int (*)()'} to 'void*' [-fpermissive]'
            table.insert(ingame_overlay_fixes, '-fpermissive')
        end
    end

    if _OPTIONS["32-build"] then
        cmake_build('ingame_overlay/deps/System', true, {
            'SYSTEM_BUILD_TESTS=OFF',
            'SYSTEM_DYNAMIC_RUNTIME=OFF',
        }, nil, ingame_overlay_fixes)
        cmake_build('ingame_overlay/deps/mini_detour', true, {
            'MINIDETOUR_BUILD_TESTS=OFF',
            'MINIDETOUR_DYNAMIC_RUNTIME=OFF',
        })
        cmake_build('ingame_overlay', true, ingame_overlay_common_defs, nil, ingame_overlay_fixes)
    end
    if _OPTIONS["64-build"] then
        cmake_build('ingame_overlay/deps/System', false, {
            'SYSTEM_BUILD_TESTS=OFF',
            'SYSTEM_DYNAMIC_RUNTIME=OFF',
        }, nil, ingame_overlay_fixes)
        cmake_build('ingame_overlay/deps/mini_detour', false, {
            'MINIDETOUR_BUILD_TESTS=OFF',
            'MINIDETOUR_DYNAMIC_RUNTIME=OFF',
        })
        cmake_build('ingame_overlay', false, ingame_overlay_common_defs, nil, ingame_overlay_fixes)
    end
end

if _OPTIONS["build-opus"] or _OPTIONS["all-build"] then
    local opus_common_defs = {
        "OPUS_BUILD_SHARED_LIBRARY=OFF",
        "OPUS_BUILD_TESTING=OFF",
        "OPUS_BUILD_PROGRAMS=OFF",
        "OPUS_CUSTOM_MODES=OFF",
        "OPUS_STATIC_RUNTIME=ON",
    }

    if _OPTIONS["32-build"] then
        cmake_build('opus', true, opus_common_defs)
    end
    if _OPTIONS["64-build"] then
        cmake_build('opus', false, opus_common_defs)
    end
end

if _OPTIONS["build-portaudio"] or _OPTIONS["all-build"] then
    local portaudio_common_defs = {
        "PA_BUILD_SHARED_LIBS=OFF",
        "PA_BUILD_TESTS=OFF",
        "PA_BUILD_EXAMPLES=OFF",
        "PA_ENABLE_DEBUG_OUTPUT=OFF",
        "PA_USE_SKELETON=OFF", -- skeleton idk what that means
    }
    if os.target() == 'windows' then
        -- Windows-only audio backends
        table.insert(portaudio_common_defs, "PA_USE_ASIO=OFF")
        table.insert(portaudio_common_defs, "PA_USE_DS=ON")
        table.insert(portaudio_common_defs, "PA_USE_WMME=ON")
        table.insert(portaudio_common_defs, "PA_USE_WASAPI=ON")
        table.insert(portaudio_common_defs, "PA_USE_WDMKS=OFF")
    else
        -- Linux/macOS backends
        table.insert(portaudio_common_defs, "PA_USE_ALSA=ON")
        -- These make the build fail on some Linux distros where the headers aren't available
        table.insert(portaudio_common_defs, "PA_USE_JACK=OFF")
        table.insert(portaudio_common_defs, "PA_USE_OSS=OFF")
        table.insert(portaudio_common_defs, "PA_USE_PULSEAUDIO=OFF")
    end
    if _OPTIONS["32-build"] then
        cmake_build('portaudio', true, portaudio_common_defs)
    end
    if _OPTIONS["64-build"] then
        cmake_build('portaudio', false, portaudio_common_defs)
    end
end

if _OPTIONS["build-sdl"] or _OPTIONS["all-build"] then
    local sdl_common_defs = {
        "SDL_SHARED=OFF",
        "SDL_STATIC=ON",
        "SDL_TEST_LIBRARY=OFF",
        "SDL_DISABLE_INSTALL_DOCS=ON",
        "SDL_LIBC=ON",
    }
    if os.target() ~= 'windows' then
        table.insert(sdl_common_defs, "SDL_ALSA=ON")
        table.insert(sdl_common_defs, "SDL_JACK=OFF")
        table.insert(sdl_common_defs, "SDL_PULSEAUDIO=OFF")
        table.insert(sdl_common_defs, "SDL_SNDIO=OFF")
    end
    if _OPTIONS["32-build"] then
        cmake_build('sdl', true, sdl_common_defs)
    end
    if _OPTIONS["64-build"] then
        cmake_build('sdl', false, sdl_common_defs)
    end
end
