function linkFBX()
	configuration {"Debug"}
		libdirs { "C:\\Program Files\\Autodesk\\FBX\\FBX SDK\\2017.1\\lib\\vs2015\\x64\\debug"}
	
	configuration {"Release", "RelWithDebInfo"}
		libdirs { "C:\\Program Files\\Autodesk\\FBX\\FBX SDK\\2017.1\\lib\\vs2015\\x64\\release"}
	
	configuration {}

	includedirs { "../../luxmiengine_fbx/src", "C:\\Program Files\\Autodesk\\FBX\\FBX SDK\\2017.1\\include", }
	links { "libfbxsdk" }
	defines {"FBXSDK_SHARED"}
end

project "lumixengine_fbx"
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
	
table.insert(build_app_callbacks, linkFBX)
table.insert(build_studio_callbacks, linkFBX)