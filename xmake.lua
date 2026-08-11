-- include subprojects
includes("lib/commonlibf4")

-- ---------------------------------------------------------------------------
-- Plugin identity. project.lua is GENERATED from project.conf by the build
-- scripts (scripts/_common.sh) — edit project.conf, never project.lua.
--
-- Why the indirection: xmake's description scope has no `io` module, so this
-- file cannot read project.conf directly. includes() does share globals, so a
-- generated Lua mirror is the way to keep one editable source of truth.
-- ---------------------------------------------------------------------------
includes("project.lua")

-- xmake's description scope has no assert/error/raise either, so a bad config
-- is reported by hand and then forced to abort by indexing nil. Ugly, but the
-- alternative is silently building a plugin with no name.
local function die(a_message)
    print("")
    print("xmake.lua: " .. a_message)
    print("")
    local abort = nil
    abort.bad_project_configuration = true
end

if not CONF_PLUGIN_NAME or not CONF_VERSION then
    die("project.lua is missing or stale. It is GENERATED from project.conf — " ..
        "run ./gen_compile_commands.sh (or any build script) to regenerate it.")
end

local plugin_name = CONF_PLUGIN_NAME
local plugin_version = CONF_VERSION
local plugin_author = CONF_AUTHOR or ""
local plugin_description = CONF_DESCRIPTION or ""

local version_major, version_minor, version_patch = plugin_version:match("^(%d+)%.(%d+)%.(%d+)$")
if not version_major then
    die("project.conf: VERSION must be MAJOR.MINOR.PATCH, got '" .. plugin_version .. "'")
end

-- set project constants
set_project(plugin_name)
set_version(plugin_version)
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules (Added "mode.release" here)
add_rules("mode.debug", "mode.releasedbg", "mode.release")
add_rules("plugin.vsxmake.autoupdate")

-- Multi-runtime: REL::ID / REL::Offset carry one entry per runtime, indexed by
-- REX::FModule::Runtime { kOG = 0, kNG = 1, kAE = 2 }. This is what makes a
-- single DLL work on OG (1.10.163), NG (1.10.980+) and AE. Do not lower it.
add_defines("COMMONLIB_RUNTIMECOUNT=3")

-- 🔒 Static-link the MSVC runtime (/MT, /MTd).
-- The plugin then carries NO external CRT dependency (no msvcp140.dll /
-- vcruntime140*.dll / api-ms-win-crt-*.dll imports), so it is immune to a
-- user's — or a Wine/Proton prefix's — broken, missing, or mismatched VC++
-- redistributable. That CRT fragility was the root cause of a
-- "couldn't load plugin (000000C1 / 193)" failure seen during development.
-- Setting this at root scope also forces xrepo deps (spdlog) to rebuild with
-- /MT, so there is no LNK2038 runtime mismatch.
if is_mode("debug") then
	set_runtimes("MTd")
else
	set_runtimes("MT")
end

-- 🚀 Global Optimization Settings for Release Mode
if is_mode("release") then
	set_optimize("aggressive")     -- Enforces -O3 (GCC/Clang) or /Ox /O2 (MSVC)
	set_fpmodels("fast")           -- Speeds up floating-point math (highly recommended for games/F4SE)
	add_vectorexts("sse2", "avx2") -- Tailors code to modern CPU instruction sets
	end

	-- define targets
	target(plugin_name)
	add_rules("commonlibf4.plugin", {
		name = plugin_name,
		author = plugin_author,
		description = plugin_description
	})

	-- Nuke the Windows.h min/max macros globally
	add_defines("NOMINMAX")

	-- 🛡️ THE CETCOMPAT KILLER 🛡️
	-- Forces MSVC to disable CETCOMPAT during the DLL linking phase
	add_shflags("/CETCOMPAT:NO")
	add_ldflags("/CETCOMPAT:NO")

	-- For absolute peak performance in Release mode: Link-Time Optimization
	-- 	if is_mode("release") then
	-- 		set_policy("build.optimization.lto", true) -- Enables Interprocedural Optimization / GL / LTCG
	-- 		end

		-- add src files
		add_files("src/**.cpp")
		remove_files("src/**Test.cpp")
		add_headerfiles("src/**.h")
		add_headerfiles("src/**.hpp")
		set_pcxxheader("src/pch.h")
