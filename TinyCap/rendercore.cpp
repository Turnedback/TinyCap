#include <iostream>
#include <string>

#include "dxgi_duplication.h"
#include "rendercore.h"
#include "texture.h"
#include "scene.h"
#include "mf_h264_encoder.h"
#include "util.h"
#include "timing.h"

#define MAX_FPS 30
const double MAX_FRAME_TIME = (double)(1.00f / MAX_FPS * 1000.00f);
double accumulator = 0.0f;

////
//// globals
////
DXGIDuplication *g_DesktopDuplication; //, *g_DesktopDuplication2;
Scene *g_Scene; // , *g_Scene2;
MF_H264_Encoder *g_MFEncoder;
////
////
////

RenderCore::RenderCore() { };

RenderCore::~RenderCore()
{ 
	m_d3d11DepthStencilBuffer = nullptr;
	m_d3d11RenderTargetView = nullptr;
	m_SwapChain = nullptr;
	m_d3d11Device = nullptr;
	m_d3d11DeviceContext = nullptr;
};

bool RenderCore::Init(int screenWidth, int screenHeight, HWND hWnd)
{
	HRESULT hr;
		
	IDXGIAdapter1 *adapter;
	IDXGIOutput *adapterOutput;
	IDXGIOutput1 *adapterOutput1;

	DXGI_ADAPTER_DESC adapterDesc;	
	DXGI_MODE_DESC *displayModeList;
	DXGI_SWAP_CHAIN_DESC swapChainDesc;

	ID3D11Texture2D *pBackBuffer;

	D3D11_TEXTURE2D_DESC depthBufferDesc;
	D3D11_DEPTH_STENCIL_DESC depthStencilDesc;
	D3D11_DEPTH_STENCIL_DESC depthDisabledStencilDesc;
	D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc;
	D3D11_RASTERIZER_DESC rasterDesc;	

	uint32_t numModes, fpsNumerator, fpsDenominator;
	size_t stringLength;
	int error;
	float fov, aspect;
		
	if (!EnumerateDisplayAdapters(&g_DXGIAdapters)) {
		return false;
	}
	adapter = g_DXGIAdapters.at(0);

	hr = adapter->EnumOutputs(0, &adapterOutput);
	if (FAILED(hr)) {
		return false;
	}
	
	// desktop duplication stuff
	hr = adapterOutput->QueryInterface(&adapterOutput1);
	
	hr = adapterOutput1->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_ENUM_MODES_INTERLACED, &numModes, nullptr);
	if (FAILED(hr)) {
		return false;
	}

	displayModeList = new DXGI_MODE_DESC[numModes];
	if (!displayModeList) {
		return false;
	}

	hr = adapterOutput1->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_ENUM_MODES_INTERLACED, &numModes, displayModeList);
	if (FAILED(hr)) {
		return false;
	}
	
	for (UINT i = 0; i < numModes; i++) {
		if (displayModeList[i].Width == (unsigned int)screenWidth) {
			if (displayModeList[i].Height == (unsigned int)screenHeight) {
				fpsNumerator = displayModeList[i].RefreshRate.Numerator;
				fpsDenominator = displayModeList[i].RefreshRate.Denominator;
			}
		}
	}

	hr = adapter->GetDesc(&adapterDesc);
	if (FAILED(hr)) {
		return false;
	}
		
	// retrieve video adapter memory and name
	m_VideoMemory = (int)(adapterDesc.DedicatedVideoMemory / 1024 / 1024);	
	
	error = wcstombs_s(&stringLength, m_VideoCardDesc, 128, adapterDesc.Description, 128);
	
	if (error != 0) {
		return false;
	}
	DebugOut("Found graphics adapter: %s (%dMB VRAM)\n", m_VideoCardDesc, m_VideoMemory);

	delete[] displayModeList;
	displayModeList = nullptr;

	adapterOutput->Release();
	adapter->Release();
	
	// set single back buffer
	ZeroMemory(&swapChainDesc, sizeof(swapChainDesc));
	swapChainDesc.BufferCount = 1;
	swapChainDesc.BufferDesc.Width = screenWidth;
	swapChainDesc.BufferDesc.Height = screenHeight;
	swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
	swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.OutputWindow = hWnd;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.SampleDesc.Quality = 0;
	swapChainDesc.Windowed = true;
	//swapChainDesc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	//swapChainDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	//swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	//swapChainDesc.Flags = 0;

	// create swap chain, direct3d device, and d3d context	
	uint32_t deviceFlags = 0;
#define D3D_DEVICE_DEBUG
#ifdef D3D_DEVICE_DEBUG
	deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	D3D_FEATURE_LEVEL featureLevels[] = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
	};
	uint32_t numFeatureLevels = ARRAYSIZE(featureLevels);

	hr = D3D11CreateDeviceAndSwapChain(		nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags, featureLevels, numFeatureLevels,
											D3D11_SDK_VERSION, &swapChainDesc, &m_SwapChain, &m_d3d11Device, nullptr, &m_d3d11DeviceContext);
	if (FAILED(hr)) { 
		DebugOut("D3D11CreateDeviceAndSwapChain failed!\n");
		return false;
	}

	// enable multithreaded device context protection
	ID3D10Multithread *contextMT = nullptr;
	m_d3d11DeviceContext->QueryInterface(&contextMT);

	if (contextMT) {
		contextMT->SetMultithreadProtected(true);
		contextMT->Release();
	}
	else {
		DebugOut("Fatal error! ID3D10Multithread::SetMultithreadProtected for D3D11 device context failed!\n");
		return false;
	}

	// get pointer to the back buffer
	hr = m_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID *)&pBackBuffer);
	if (FAILED(hr)) {
		DebugOut("IDXGISwapChain::GetBuffer failed!\n");
		return false;
	}

	// create render target view from back buffer
	hr = m_d3d11Device->CreateRenderTargetView(pBackBuffer, nullptr, &m_d3d11RenderTargetView);
	if (FAILED(hr)) {
		DebugOut("ID3D11Device::CreateRenderTargetView failed!\n");
		return false;
	}
	pBackBuffer->Release();

	// set up depth buffer description
	ZeroMemory(&depthBufferDesc, sizeof(depthBufferDesc));		
	depthBufferDesc.Width = screenWidth;
	depthBufferDesc.Height = screenHeight;
	depthBufferDesc.MipLevels = 1;
	depthBufferDesc.ArraySize = 1;
	depthBufferDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	depthBufferDesc.SampleDesc.Count = 1;
	depthBufferDesc.SampleDesc.Quality = 0;
	depthBufferDesc.Usage = D3D11_USAGE_DEFAULT;
	depthBufferDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	depthBufferDesc.CPUAccessFlags = 0;
	depthBufferDesc.MiscFlags = 0;

	// create texture for depth buffer
	hr = m_d3d11Device->CreateTexture2D(&depthBufferDesc, nullptr, &m_d3d11DepthStencilBuffer);
	if (FAILED(hr)) {
		DebugOut("ID3D11Device::CreateTexture2D failed! Could not create texture for depth buffer.\n");
		return false;
	}

	// set up description of stencil state
	ZeroMemory(&depthStencilDesc, sizeof(depthStencilDesc));	
	depthStencilDesc.DepthEnable = true; // z-buffer enabled
	depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS;

	depthStencilDesc.StencilEnable = true;
	depthStencilDesc.StencilReadMask = 0xFF;
	depthStencilDesc.StencilWriteMask = 0xFF;

	// Stencil operations if pixel is front-facing.
	depthStencilDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	depthStencilDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR;
	depthStencilDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	depthStencilDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

	// Stencil operations if pixel is back-facing.
	depthStencilDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	depthStencilDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_DECR;
	depthStencilDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	depthStencilDesc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

	// create depth stencil state
	hr = m_d3d11Device->CreateDepthStencilState(&depthStencilDesc, &m_d3d11DepthStencilState);
	if (FAILED(hr)) {
		DebugOut("ID3D11Device::CreateDepthStencilState failed!\n");
		return false;
	}

	// Now create a second depth stencil state which turns off the Z buffer for 2D rendering.  The only difference is 
	// that DepthEnable is set to false, all other parameters are the same as the other depth stencil state.
	depthDisabledStencilDesc.DepthEnable = false;
	depthDisabledStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depthDisabledStencilDesc.DepthFunc = D3D11_COMPARISON_LESS;
	depthDisabledStencilDesc.StencilEnable = true;
	depthDisabledStencilDesc.StencilReadMask = 0xFF;
	depthDisabledStencilDesc.StencilWriteMask = 0xFF;
	depthDisabledStencilDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	depthDisabledStencilDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR;
	depthDisabledStencilDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	depthDisabledStencilDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	depthDisabledStencilDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	depthDisabledStencilDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_DECR;
	depthDisabledStencilDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	depthDisabledStencilDesc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

	// Create the state using the device.
	hr = m_d3d11Device->CreateDepthStencilState(&depthDisabledStencilDesc, &m_d3d11DepthStencilDisabledState);
	if (FAILED(hr))
	{
		return false;
	}

	// disable the Z-Buffer
	ZBufferState(0);

	// set up depth stencil view description
	ZeroMemory(&depthStencilViewDesc, sizeof(depthStencilViewDesc));	
	depthStencilViewDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	depthStencilViewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	depthStencilViewDesc.Texture2D.MipSlice = 0;

	// create depth stencil view
	hr = m_d3d11Device->CreateDepthStencilView(m_d3d11DepthStencilBuffer, &depthStencilViewDesc, &m_d3d11DepthStencilView);
	if (FAILED(hr)) {
		DebugOut("ID3D11Device::CreateDepthStencilView failed!\n");
		return false;
	}

	// bind render target view and depth stencil buffer to the output render pipeline
	m_d3d11DeviceContext->OMSetRenderTargets(1, &m_d3d11RenderTargetView, m_d3d11DepthStencilView);

	// set up rasterizer description	
	rasterDesc.AntialiasedLineEnable = false;
	rasterDesc.CullMode = D3D11_CULL_NONE;
	rasterDesc.DepthBias = 0;
	rasterDesc.DepthBiasClamp = 0.0f;
	rasterDesc.DepthClipEnable = false;
	rasterDesc.FillMode = D3D11_FILL_SOLID;
	rasterDesc.FrontCounterClockwise = false;
	rasterDesc.MultisampleEnable = false;
	rasterDesc.ScissorEnable = false;
	rasterDesc.SlopeScaledDepthBias = 0.0f;

	// create the rasterizer
	hr = m_d3d11Device->CreateRasterizerState(&rasterDesc, &m_d3d11RasterState);
	if (FAILED(hr)) {
		DebugOut("ID3D11Device::CreateRasterizerState failed!");
		return false;
	}
	
	m_d3d11DeviceContext->RSSetState(m_d3d11RasterState);

	// set up viewport for rendering
	D3D11_VIEWPORT viewport;
	viewport.Width = (float)screenWidth;
	viewport.Height = (float)screenHeight;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;

	// create the viewport
	m_d3d11DeviceContext->RSSetViewports(1, &viewport);
		
	fov = (float)PI / 4.0f;
	aspect = (float)screenWidth / (float)screenHeight;
		
	m_ProjectionMatrix = DirectX::XMMatrixPerspectiveFovLH(fov, aspect, SCREEN_NEAR, SCREEN_DEPTH);
	m_WorldMatrix = DirectX::XMMatrixIdentity();
	m_OrthoMatrix = DirectX::XMMatrixOrthographicLH((float)screenWidth, (float)screenHeight, SCREEN_NEAR, SCREEN_DEPTH);

	// Scene is a textured quad to draw on
	g_Scene = new Scene(GetDevice(), GetDeviceContext(), screenWidth, screenHeight);
	//g_Scene2 = new Scene(GetDevice(), GetDeviceContext(), screenWidth, screenHeight);	

	// new DXGI Desktop Duplication object
	g_DesktopDuplication = new DXGIDuplication();
	if (!g_DesktopDuplication->Init(0, 0, &g_DXGIAdapters, GetDevice())) { DebugOut("Failed to init DXGI Desktop Duplication API!\n"); return false; }
	
	//g_DesktopDuplication2 = new DXGIDuplication();
	//if (!g_DesktopDuplication2->Init(0, 1, &g_DXGIAdapters, GetDevice())) { DebugOut("Failed to init DXGI Desktop Duplication API for Adapter 1/Output 2!\n"); return false; }

	// initialize Media Foundation
	g_MFEncoder = new MF_H264_Encoder(m_d3d11Device, m_d3d11DeviceContext);
	if (!g_MFEncoder->Init()) { DebugOut("Failed to init Media Foundation H.264 Encoder!\n"); return false; }

	return true;
};

void RenderCore::Shutdown()
{
	delete g_Scene;
	g_Scene = nullptr;

	delete g_DesktopDuplication;
	g_DesktopDuplication = nullptr;
	
	//delete g_DesktopDuplication2;
	//g_DesktopDuplication2 = nullptr;

	g_MFEncoder->Shutdown();
	delete g_MFEncoder;
	g_MFEncoder = nullptr;
	//delete g_Scene2;
	//g_Scene2 = nullptr;

	if (m_d3d11DepthStencilState) { m_d3d11DepthStencilState->Release(); }
	if (m_d3d11DepthStencilDisabledState) { m_d3d11DepthStencilDisabledState->Release(); }
	if (m_d3d11DepthStencilView) { m_d3d11DepthStencilView->Release(); }
	if (m_d3d11RasterState) { m_d3d11RasterState->Release(); }
	if (m_d3d11DepthStencilBuffer) { m_d3d11DepthStencilBuffer->Release(); }
	if (m_d3d11RenderTargetView) { m_d3d11RenderTargetView->Release(); }
	if (m_SwapChain) { m_SwapChain->Release(); }
	if (m_d3d11Device) { m_d3d11Device->Release(); }
	if (m_d3d11DeviceContext) { m_d3d11DeviceContext->Release(); }
};

bool RenderCore::Render()
{
	HighResolutionTimer timer;
	timer.Start();

	BeginScene(0.0f, 0.125f, 0.3f, 1.0f);	
	
	// Scene::UpdateVertexBuffer allows us to position the Scene
	if (!g_Scene->UpdateVertexBuffer(0, 0, 1280, 720)) { //  || !g_Scene2->UpdateVertexBuffer(100, 100, 640, 360)) {	
		DebugOut("Scene::UpdateVertexBuffer failed!\n");
		return false;
	}	 
	
	// Get Desktop Duplication frame
	if (g_DesktopDuplication->GetFrame()) { // && g_DesktopDuplication2->GetFrame()) {
		// Render Desktop Duplication frame onto our Scene
		g_Scene->Render(g_DesktopDuplication->GetTexture(), m_WorldMatrix, m_OrthoMatrix);
		//g_Scene2->Render(g_DesktopDuplication2->GetTexture(), m_WorldMatrix, m_OrthoMatrix);

		g_MFEncoder->WriteFrame(g_DesktopDuplication->GetTexture());
		
		g_DesktopDuplication->FinishFrame();
		//g_DesktopDuplication2->FinishFrame();
	}
	
	// limit frame rate
	double frameTime = timer.AsMilliseconds();
	while (frameTime < (MAX_FRAME_TIME)) {
		frameTime += 0.001f;
	}
	DebugOut("frame time: %f\n", frameTime);

	EndScene();

	/* const vec3f eyePosition(0.f, 0.f, 0.f);
	const vec3f lookAt(0.f, 0.f, 1.f);
	const vec3f upDir(0.f, 1.f, 0.f);

	DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixLookAtLH(   DirectX::XMLoadFloat3((const DirectX::XMFLOAT3 *)&eyePosition),
	DirectX::XMLoadFloat3((const DirectX::XMFLOAT3 *)&lookAt),
	DirectX::XMLoadFloat3((const DirectX::XMFLOAT3 *)&upDir)); */		
	
	//ZBufferState(1);

	return true;
};

void RenderCore::BeginScene(float r, float g, float b, float a)
{
	float color[4];

	// color used to clear the buffer
	color[0] = r;
	color[1] = g;
	color[2] = b;
	color[3] = a;

	// clear back buffer
	m_d3d11DeviceContext->ClearRenderTargetView(m_d3d11RenderTargetView, color);

	// clear depth buffer
	m_d3d11DeviceContext->ClearDepthStencilView(m_d3d11DepthStencilView, D3D11_CLEAR_DEPTH, 1.0f, 0);
};

void RenderCore::EndScene()
{
	m_SwapChain->Present(0, 0);
};

IDXGISwapChain *RenderCore::GetSwapChain()
{ 
	return m_SwapChain;
};

ID3D11Device *RenderCore::GetDevice()
{
	return m_d3d11Device;
};

ID3D11DeviceContext *RenderCore::GetDeviceContext()
{
	return m_d3d11DeviceContext;
};

void RenderCore::ZBufferState(int state)
{
	if (state == 0) {
		m_d3d11DeviceContext->OMSetDepthStencilState(m_d3d11DepthStencilDisabledState, 1);
	}
	else if (state == 1) {
		m_d3d11DeviceContext->OMSetDepthStencilState(m_d3d11DepthStencilState, 1);
	}	
};

bool RenderCore::EnumerateDisplayAdapters(std::vector<IDXGIAdapter1 *> *dxgiAdapters)
{
	IDXGIAdapter1 *pAdapter;
	IDXGIFactory1 *pFactory = nullptr;
	HRESULT hr;

	hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory);
	if (FAILED(hr)) {
		return false;
	}
	
	for (UINT i = 0; pFactory->EnumAdapters1(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
		dxgiAdapters->push_back(pAdapter);
		//m_VideoAdapterList.push_back(pAdapter);
	}

	if (pFactory) {
		pFactory->Release();
	}

	return true;
};