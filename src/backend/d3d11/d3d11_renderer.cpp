#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

//Zaama numpy de c++
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

//link librairies
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

//conteneurs
#include <vector>
#include <unordered_map>

//moteur
static ID3D11Device* g_Device = nullptr; //createur d objets
static ID3D11DeviceContext* g_DeviceContext = nullptr; //dessine
static IDXGISwapChain* g_SwapChain = nullptr; //fenetre
static ID3D11RenderTargetView* g_RenderTargetView = nullptr; // ecran couleur
static ID3D11DepthStencilView* g_DepthStencilView = nullptr; //ecran profondeur

//Buffers de données
static ID3D11Buffer* g_VertexBuffer = nullptr; // pts du carré
static ID3D11Buffer* g_IndexBuffer = nullptr; // ordre des pts

// camera
typedef struct CB_PerFrame {
  glm::mat4 view;
  glm::mat4 projection;
};

// position
typedef struct CB_PerObject {
    glm::mat4 model;
};

//variables globales pr stocker les structs --> gpu
typedef struct ID3D11Buffer* g_ CBFrame = nullptr;
static ID3D11Buffer* g_CBObject = nullptr;

//INIT
void D3D11_InitContext(HRL_uint width, HRL_uint height, void* loader) {
    //config fenetre
    DXGI_SWAP_CHAIN_DESC scd = {}; //commence a 0
    scd.BufferCount = 1; //img en attente (double buffering)
    scd.BufferDesc.Format = DXGI_FORAT_R8G8B8A8_UNORM; // couleurs classiques
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.Bufferage = DXGI_USAGE_RENDER_TARGET_OUTPUT; //on va dessiner dedans
    scd.OutputWindow = (HWND)loader; //handle de la fenetre windows
    scd.SampleDesc.Count = 1 // pas d anti aliasing
    scd.Windowed = TRUE;

 // creer et device et swapchain
 D3D11CreateDeviceAndSwapChain(
    nullptr, //adapter par defaut carte graphique
    D3D_DRIVER_TYPE_HARDWARE, // utilisation carte graphique
    nullptr, 
    D3D11_CREATE_SERVICE_DEBUG, //apparition logs erreurs
    nullptr, 0, //feature levels par defaut
    D3D11_SDK_VERSION,
    &scd, 
    &g_SwapChain, // sortie swapchain
    &g_Device, // sortie device
    nullptr, // driver type
    &g_DeviceContext // sortie device context
 );

 //creer apercu sur l ecran
 //recup image brute de swapchain
 ID3D11Texture2D* pBackBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
 g_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
}