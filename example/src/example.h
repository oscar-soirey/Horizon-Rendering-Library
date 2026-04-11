#pragma once

#include <string>
#include <hrl/hrl.h>

namespace example
{
	std::string OpenFile(const char* _path, size_t* _size);
	void DrawDebugExamples(HRL_id sceneId);
}