function linkFBX()

	configuration {"Debug"}
		libdirs { "../src"}
		libdirs { "C:\\Program Files\\Autodesk\\FBX\\FBX SDK\\2020.0.1\\lib\\vs2017\\x64\\debug"}
	
	configuration {"Release or RelWithDebInfo"}
		libdirs { "C:\\Program Files\\Autodesk\\FBX\\FBX SDK\\2020.0.1\\lib\\vs2017\\x64\\release"}
	
	configuration {}

	includedirs { "../../luxmiengine_fbx/src", "C:\\Program Files\\Autodesk\\FBX\\FBX SDK\\2020.0.1\\include", }
	links { "libfbxsdk" }
	defines {"FBXSDK_SHARED"}
	
	configuration {}
end

project "fbx_sdk"
	libType()
	files { 
		"src/**.cpp",
		"src/**.h",
		"genie.lua"
	}
	
	if not build_studio then
		removefiles { "src/editor/*" }
	end
	
	includedirs { "../../luxmiengine_fbx/src", 
		"../LumixEngine/external/lua/include", 
		"../LumixEngine/external/bgfx/include"
	}
	links { "engine" }
	linkFBX()

	defaultConfigurations()
	
linkPlugin("fbx_sdk")


table.insert(build_app_callbacks, linkFBX)
table.insert(build_studio_callbacks, linkFBX)

