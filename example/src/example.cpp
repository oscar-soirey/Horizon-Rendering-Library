#include "example.h"

#include <iostream>
#include <hrl/hrl.h>

namespace example
{
	std::string OpenFile(const char* _path, size_t* _size)
	{
		FILE* f = fopen(_path, "rb");
		if (!f)
		{
			std::cout << "Erreur de lecture du fichier" << std::endl;
			return "";
		}

#ifdef WIN32
		//windows
		_fseeki64(f, 0, SEEK_END);
		__int64 size = _ftelli64(f);
#else
		//compaptibilité
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
#endif

		if (size <= 0 || size > INT_MAX)
		{
			fclose(f);
			std::cout << "Erreur de taille (<= 0 ou superieur a INT_MAX)" << std::endl;
			return "";
		}
#ifdef WIN32
		_fseeki64(f, 0, SEEK_SET);
#else
		fseek(f, 0, SEEK_SET);
#endif

		std::string content((size_t)size, '\0');   // alloue buffer
		if (fread(&content[0], 1, (size_t)size, f) != (size_t)size)
		{
			fclose(f);
			std::cout << "Erreur de lecture complète" << std::endl;
			return "";
		}

		fclose(f);
		//passer la size
		if (_size)
		{
			*_size = (size_t)size;
		}
		return content;
	}


	void DrawDebugExamples(HRL_id sceneId)
	{
		// cercle creux
		HRL_DrawDebugCircle(sceneId, HRL_DebugHollow,
			0.f, 0.f, 0.f,
			1.f, 16,
			1.f, 0.f, 0.f); // rouge

		// cercle plein
		HRL_DrawDebugCircle(sceneId, HRL_DebugSolid,
			3.f, 0.f, 0.f,
			0.5f, 16,
			0.f, 1.f, 0.f); // vert

		// carré creux
		{
			float vx[] = { -0.5f,  0.5f, 0.5f, -0.5f };
			float vy[] = { -0.5f, -0.5f, 0.5f,  0.5f };
			float vz[] = {  0.f,   0.f,  0.f,   0.f  };
			HRL_DrawDebugPolygon(sceneId, HRL_DebugHollow, vx, vy, vz, 4, 0.f, 0.f, 1.f); // bleu
		}

		// carré plein
		{
			float vx[] = {  2.f,  3.f, 3.f,  2.f };
			float vy[] = { -0.5f, -0.5f, 0.5f, 0.5f };
			float vz[] = {  0.f,  0.f,  0.f,  0.f };
			HRL_DrawDebugPolygon(sceneId, HRL_DebugSolid, vx, vy, vz, 4, 1.f, 1.f, 0.f); // jaune
		}

		// capsule
		HRL_DrawDebugCapsule(sceneId, HRL_DebugHollow,
			-2.f, -1.f, 0.f,   // A
			-2.f,  1.f, 0.f,   // B
			0.4f, 16,
			1.f, 0.5f, 0.f);   // orange

		// segment
		HRL_DrawDebugSegment(sceneId,
			-3.f, -1.f, 0.f,
			 3.f,  1.f, 0.f,
			1.f, 1.f, 1.f);    // blanc

		// points
		HRL_DrawDebugPoint(sceneId,  0.f,  2.f, 0.f, 0.1f, 1.f, 0.f, 1.f); // magenta
		HRL_DrawDebugPoint(sceneId,  1.f,  2.f, 0.f, 0.1f, 0.f, 1.f, 1.f); // cyan
		HRL_DrawDebugPoint(sceneId, -1.f,  2.f, 0.f, 0.1f, 1.f, 1.f, 0.f); // jaune
	}
}