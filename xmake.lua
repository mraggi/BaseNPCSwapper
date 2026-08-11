-- include subprojects
includes("lib/commonlibf4")

-- set project constants
set_project("BaseNPCSwapper")
set_version("0.9.1")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules (Added "mode.release" here)
add_rules("mode.debug", "mode.releasedbg", "mode.release")
add_rules("plugin.vsxmake.autoupdate")

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

	-- Project-local plugin rule. Same as commonlibf4.plugin except it
	-- does NOT auto-generate a commonlibf4-plugin.cpp pinning the DLL to
	-- F4SE::RUNTIME_LATEST. We provide our own F4SEPlugin_Version in
	-- src/main.cpp with an explicit CompatibleVersions list spanning the
	-- full NG/AE runtime range.
	rule("bns.plugin", function()
		add_deps("commonlib.plugin")

		on_load(function(target)
			target:data_set("commonlib.plugin.config", target:extraconf("rules", "bns.plugin"))
			target:data_set("commonlib.plugin.package", { prefixdir = "Data" })
		end)

		on_config(function(target)
			target:add("deps", "commonlibf4")

			if os.getenv("XSE_FO4_MODS_PATH") then
				target:set("installdir", path.join(os.getenv("XSE_FO4_MODS_PATH"), target:name()))
			elseif os.getenv("XSE_FO4_GAME_PATH") then
				target:set("installdir", path.join(os.getenv("XSE_FO4_GAME_PATH"), "Data"))
			end

			target:add("installfiles", target:targetfile(), { prefixdir = "F4SE/Plugins" })
			target:add("installfiles", target:symbolfile(), { prefixdir = "F4SE/Plugins" })
		end)
	end)

	-- define targets
	target("BaseNPCSwapper")
	add_rules("bns.plugin", {
		name = "BaseNPCSwapper",
		author = "mraggi",
		description = "F4SE plugin: runtime NPC swapper / modifier"
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
