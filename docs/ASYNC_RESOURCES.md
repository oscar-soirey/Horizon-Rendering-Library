# Asynchronous OpenGL resource loading

HRL keeps all existing synchronous resource APIs. Async loading is opt-in through the `Async` variants.

**HRL never opens files.** The application is always responsible for file I/O and passes `data + size` to HRL, exactly like the synchronous resource APIs.

Worker threads perform CPU-side preparation in parallel. Completed resources are uploaded/compiled on the HRL context thread from `HRL_BeginFrame()` or the explicit wait functions.

## Textures

Load the bytes yourself, then enqueue them:

```cpp
std::vector<char> data = ReadBinaryFile("textures/albedo.jpg");
HRL_id tex = HRL_CreateTextureAsync(data.data(), data.size());

HRL_WaitForTexture(tex);
```

Multiple calls can be queued immediately:

```cpp
HRL_id albedo    = HRL_CreateTextureAsync(albedoData.data(), albedoData.size());
HRL_id normal    = HRL_CreateTextureAsync(normalData.data(), normalData.size());
HRL_id roughness = HRL_CreateTextureAsync(roughnessData.data(), roughnessData.size());
```

The worker threads decode these buffers concurrently; the OpenGL uploads remain on the context thread.

## Shaders

The same rule applies to shaders: the application supplies both source buffers and sizes.

```cpp
HRL_id shader = HRL_CreateShaderAsync(
    vertexData.data(), vertexData.size(),
    fragmentData.data(), fragmentData.size()
);

HRL_WaitForShader(shader);
```

GLSL compilation/linking stays on the active OpenGL context thread.

## Frame integration

`HRL_BeginFrame()` automatically processes completed asynchronous resources, so normal render-loop code does not need to pump a separate loader.
