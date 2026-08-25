if _OPTIONS["ext-ingame_overlay"] or _OPTIONS["all-ext"] then
    -- NOTE: after replacing ingame_overlay.tar.gz with a newer upstream snapshot,
    -- use tools/refresh_ingame_overlay.sh to fetch the new upstream and reapply patches,
    -- then run tools/generate_ingame_overlay_patches.sh to regenerate .patch files.
    -- See tools/ingame_overlay_patches/README.md for the full update workflow.
    -- Patch application after extraction is handled automatically by apply_ingame_overlay_patches().
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
