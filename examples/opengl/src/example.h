#pragma once

#include <string>
#include <hrl.h>

namespace example
{
	std::string OpenFile(const char* _path, size_t* _size);
	void DrawDebugExamples(HRL_id sceneId);
	// Makes examples/assets the working directory (path given by CMake).
	void EnterAssetDirectory();
}