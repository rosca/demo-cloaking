#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <windowsx.h>

#include <CommCtrl.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <ShellScalingApi.h>
#include <wrl/client.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <stdio.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <cassert>

namespace
{
	using Microsoft::WRL::ComPtr;

	struct XControl {
		HWND hWnd = nullptr;
		
		ComPtr<IDCompositionVisual2> Visual;
		ComPtr<IUnknown> Surface;

		RECT rect;
		int InitialWidth = 0;
		int InitialHeight = 0;

		void CreateControl(HWND parent, LPCWSTR className, int width = 200, int height = 20);
		void CreateResources(const ComPtr<IDCompositionDesktopDevice>& compositionDevice);
		void UpdateRect(float scale, int offsetX, int offsetY);
	};

	struct Data
	{
		~Data()
		{
			if (CaptionFont)
				DeleteObject(CaptionFont);
		}

		HWND lastActiveControl = nullptr;

		HFONT CaptionFont = nullptr;
		float Scale = 1.0f;


		ComPtr<ID2D1Brush> BackgroundBrush;
		ComPtr<ID2D1Brush> BorderBrush;

		ComPtr<ID3D11Device> D3DDevice;
		ComPtr<IDXGIDevice> DXGIDevice;
		ComPtr<ID2D1Device> D2DDevice;
		ComPtr<IDCompositionDesktopDevice> CompositionDevice;
		ComPtr<IDCompositionTarget> CompositionTarget;
		ComPtr<IDCompositionVisual2> CustomSurface;
		ComPtr<IDCompositionVisual2> CompositionRootVisual;
		ComPtr<IDCompositionSurface> CompositionSurface;

		std::vector<XControl> controls;
	};

	Data* GetData(HWND hWnd)
	{
		LONG_PTR ptr = GetWindowLongPtrW(hWnd, GWLP_USERDATA);
		return reinterpret_cast<Data*>(ptr);
	}

	LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

	void OnCreate(HWND hWnd);
	void OnSize(HWND hWnd);

	HWND Create(HINSTANCE hInstance, const wchar_t *const className)
	{
		HWND hWnd = CreateWindowExW
		(0,
			className,
			L"DirectComposition",
            WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT, CW_USEDEFAULT,
			800, 800,
			nullptr,
			nullptr,
			hInstance,
			new Data());
		
		ShowWindow(hWnd, SW_SHOWDEFAULT);
        UpdateWindow(hWnd);

		return hWnd;
	}

	int MessageLoop()
	{
		MSG msg = {};
		while (msg.message != WM_QUIT)
		{
			if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
			{
                if (!IsDialogMessage(msg.hwnd, &msg))
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }

			}
			else
			{
				// Nothing to do
				Sleep(0);
			}
		}

		return static_cast<int>(msg.wParam);
	}

	bool Register(HINSTANCE hInstance, const wchar_t *const className)
	{
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(WNDCLASSEXW);
		wc.lpfnWndProc = WndProc;
		wc.lpszClassName = className;
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wc.style = CS_VREDRAW | CS_HREDRAW | CS_DBLCLKS;
		wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
		wc.hInstance = hInstance;

		return !!RegisterClassExW(&wc);
	}

	void XControl::CreateControl(HWND parent, LPCWSTR className, int width, int height) {
		InitialWidth = width;
		InitialHeight = height;
        static bool group = false;
        auto style = WS_TABSTOP | WS_CHILD | WS_VISIBLE | WS_TABSTOP;
        if (!group) {
            group = true;
            style |= WS_GROUP;
        }

		hWnd = CreateWindowExW(WS_EX_LAYERED, className, L"Demo",
			style, rect.left, rect.top,
			rect.right - rect.left, rect.bottom - rect.top, parent, nullptr, nullptr, nullptr);

		BOOL cloak = TRUE;
		auto hr = DwmSetWindowAttribute(hWnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
		::SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);

		::ShowWindow(hWnd, SW_SHOW);
	}

	void XControl::CreateResources(const ComPtr<IDCompositionDesktopDevice>& compositionDevice) {
		auto hr = compositionDevice->CreateVisual(Visual.ReleaseAndGetAddressOf());
		hr = compositionDevice->CreateSurfaceFromHwnd(hWnd, Surface.ReleaseAndGetAddressOf());
		hr = Visual->SetContent(Surface.Get());

		Visual->SetOffsetX(static_cast<float>(rect.left));
		Visual->SetOffsetY(static_cast<float>(rect.top));
	}


	void XControl::UpdateRect(float scale, int offsetX, int offsetY)
	{
		const int width = static_cast<int>(InitialWidth * scale);
		const int height = static_cast<int>(InitialHeight * scale);

		rect.left = offsetX;
		rect.top = offsetY;

		rect.right = offsetX + width;
		rect.bottom = offsetY + height;

		if (Visual) {
			Visual->SetOffsetX(static_cast<float>(rect.left));
			Visual->SetOffsetY(static_cast<float>(rect.top));
		}

		if (hWnd) {
			SetWindowPos(hWnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER);
		}
	}

	void UpdateDPI(HWND hWnd)
	{
		Data *const data = GetData(hWnd);

		HMONITOR monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY);
		UINT dpiX = 0, dpiY = 0;
		GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
		data->Scale = dpiX / 96.0f;
	}

	void CreateDeviceResources(HWND hWnd)
	{
		Data *const data = GetData(hWnd);

		// DXGI Resources are tied to a D3D Device, so all resources will need re-creating after a new device is made
		data->BackgroundBrush = nullptr;
		data->BorderBrush = nullptr;



		HRESULT hr = S_OK;

		// Direct3D 11 Device
		UINT d3dDeviceFlags = D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if !defined(NDEBUG)
		d3dDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
		hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, d3dDeviceFlags, nullptr, 0, D3D11_SDK_VERSION, data->D3DDevice.ReleaseAndGetAddressOf(), nullptr, nullptr);

		// DXGI Device
		hr = data->D3DDevice.As(&data->DXGIDevice);

		// Direct2D Device (settings must match those in the D3D device)
		D2D1_CREATION_PROPERTIES d2dProperties = {};
		d2dProperties.threadingMode = D2D1_THREADING_MODE_SINGLE_THREADED;
#if !defined(NDEBUG)
		d2dProperties.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
		hr = D2D1CreateDevice(data->DXGIDevice.Get(), d2dProperties, data->D2DDevice.ReleaseAndGetAddressOf());

		// DirectComposition Device
		hr = DCompositionCreateDevice2(data->D2DDevice.Get(), IID_PPV_ARGS(data->CompositionDevice.ReleaseAndGetAddressOf()));


		// DirectComposition Target
		hr = data->CompositionDevice->CreateTargetForHwnd(hWnd, TRUE, data->CompositionTarget.ReleaseAndGetAddressOf());
		hr = data->CompositionDevice->CreateVisual(data->CompositionRootVisual.ReleaseAndGetAddressOf());


		// DirectComposition (root) Visual
		
		hr = data->CompositionDevice->CreateVisual(data->CustomSurface.ReleaseAndGetAddressOf());
		hr = data->CompositionRootVisual->AddVisual(data->CustomSurface.Get(), FALSE, nullptr);


		for (auto& control : data->controls) {
			control.CreateResources(data->CompositionDevice);
			hr = data->CompositionRootVisual->AddVisual(control.Visual.Get(), TRUE, data->CustomSurface.Get());
		}

		hr = data->CompositionTarget->SetRoot(data->CompositionRootVisual.Get());

		hr = data->CompositionDevice->Commit();

	}

	void CreateWindowSizeDependentResources(HWND hWnd)
	{
		Data *const data = GetData(hWnd);

		RECT rect = {};
		::GetClientRect(hWnd, &rect);

		int offsetX = rect.left + 20;
		int offsetY = rect.top + 20;
		int maxRight = offsetX;

		for (auto& control : data->controls) {
			if (offsetY + control.InitialHeight * data->Scale > rect.bottom) {
				maxRight += 20;
				offsetX = maxRight;
				offsetY = rect.top + 20;
			}

			control.UpdateRect(data->Scale, offsetX, offsetY);
			offsetY = control.rect.bottom + 5;
			maxRight = max(maxRight, control.rect.right);
		}

		if (!data->CompositionDevice) {
			return;
		}

		HRESULT hr = S_OK;


		const int width = rect.right - rect.left;
		const int height = rect.bottom - rect.top;

		if (width <= 0 || height <= 0)
		{
			data->CompositionSurface = nullptr;
			return;
		}

		// DirectComposition Surface
		hr = data->CompositionDevice->CreateSurface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, data->CompositionSurface.ReleaseAndGetAddressOf());

		hr = data->CustomSurface->SetContent(data->CompositionSurface.Get());
	}

	void OnCreate(HWND hWnd)
	{
		UpdateDPI(hWnd);

		Data *const data = GetData(hWnd);

		data->controls.resize(5);
		size_t index = 0;

		data->controls[index++].CreateControl(hWnd, WC_EDITW, 400, 100);
        data->controls[index++].CreateControl(hWnd, WC_EDITW, 400, 100);
        data->controls[index++].CreateControl(hWnd, WC_EDITW, 400, 100);
        data->controls[index++].CreateControl(hWnd, WC_EDITW, 400, 100);
		data->controls[index++].CreateControl(hWnd, WC_BUTTONW);

		assert(index == data->controls.size());
	}

	void OnDPIChanged(HWND hWnd)
	{
		UpdateDPI(hWnd);

		CreateWindowSizeDependentResources(hWnd);
	}

	

	void Render(Data *const data, ComPtr<ID2D1DeviceContext> deviceContext, const D2D1_RECT_F &area)
	{
		deviceContext->Clear();

		if (!data->BackgroundBrush)
		{
			HRESULT hr = S_OK;

			ComPtr<ID2D1SolidColorBrush> brush;
			hr = deviceContext->CreateSolidColorBrush(D2D1::ColorF(0.75f, 0.25f, 0.25f, 0.5f), &brush);
			data->BackgroundBrush = brush;
		}

		if (!data->BorderBrush)
		{
			HRESULT hr = S_OK;

			ComPtr<ID2D1SolidColorBrush> brush;
			hr = deviceContext->CreateSolidColorBrush(D2D1::ColorF(0.25f, 0.75f, 0.25f, 1.0f), &brush);
			data->BorderBrush = brush;
		}

		deviceContext->FillRectangle(area, data->BackgroundBrush.Get());

		D2D_RECT_F const rect = D2D1::RectF(area.left + 100.0f, area.top + 100.0f, area.right - 100.0f, area.bottom - 100.0f);
		deviceContext->DrawRectangle(rect, data->BorderBrush.Get(), 50.0f);
	}

	void OnPaint(HWND hWnd)
	{
		Data *const data = GetData(hWnd);

		PAINTSTRUCT ps = {};
		BeginPaint(hWnd, &ps);

		if (!data->D3DDevice) {
			CreateDeviceResources(hWnd);
			CreateWindowSizeDependentResources(hWnd);
		}

		if (data->CompositionSurface)
		{
			HRESULT hr = S_OK;

			ComPtr<ID2D1DeviceContext> deviceContext;
			POINT offset = {};
			hr = data->CompositionSurface->BeginDraw(nullptr, IID_PPV_ARGS(&deviceContext), &offset);

			deviceContext->SetDpi(96.0f * data->Scale, 96.0f * data->Scale);
			deviceContext->SetTransform(D2D1::Matrix3x2F::Translation(offset.x / data->Scale, offset.y / data->Scale));

			RECT clientRect = {};
			GetClientRect(hWnd, &clientRect);

			D2D1_RECT_F area = D2D1::RectF(clientRect.left / data->Scale, clientRect.top / data->Scale, clientRect.right / data->Scale, clientRect.bottom / data->Scale);

			Render(data, deviceContext, area);

			hr = data->CompositionSurface->EndDraw();

			hr = data->CompositionDevice->Commit();
		}

		EndPaint(hWnd, &ps);
	}

	void OnSize(HWND hWnd)
	{
		CreateWindowSizeDependentResources(hWnd);
	}


	LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (msg == WM_CREATE)
		{
			OnCreate(hWnd);
		}
		else if (msg == WM_DESTROY)
		{
			PostQuitMessage(0);
		}
		else if (msg == WM_DPICHANGED)
		{
			OnDPIChanged(hWnd);
		}
		else if (msg == WM_NCCREATE)
		{
			CREATESTRUCTW *const creationData = reinterpret_cast<CREATESTRUCTW*>(lParam);
			SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creationData->lpCreateParams));
		}
		else if (msg == WM_NCDESTROY)
		{
			delete GetData(hWnd);
		}
		else if (msg == WM_PAINT)
		{
			OnPaint(hWnd);
			return 0;
		}
		else if (msg == WM_SIZE)
		{
			OnSize(hWnd);
		}

		if (msg == WM_SETCURSOR) {
			if (auto data = GetData(hWnd)) {
				if (data->lastActiveControl) {
					return ::SendMessage(data->lastActiveControl, msg, (WPARAM)data->lastActiveControl, lParam);
				}
			}
		}

		if (msg >= WM_NCPOINTERUPDATE && msg <= WM_POINTERROUTEDRELEASED 
				|| msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) {
			if (auto data = GetData(hWnd)) {
				auto x = GET_X_LPARAM(lParam);
				auto y = GET_Y_LPARAM(lParam);

				for (auto& control : data->controls) {
					if (x >= control.rect.left && x < control.rect.right && y >= control.rect.top && y < control.rect.bottom) {
						data->lastActiveControl = control.hWnd;
						x -= control.rect.left;
						y -= control.rect.top;

						auto childLParam = MAKELONG(x, y);
						return ::SendMessage(control.hWnd, msg, wParam, childLParam);
					}
				}
				data->lastActiveControl = nullptr;
			}
		}

		return DefWindowProcW(hWnd, msg, wParam, lParam);
	}
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
	// To support Windows 8.1 you could change this to SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	const wchar_t *const className = L"DCompWnd";

	if (!Register(hInstance, className))
		return -1;

	if (!Create(hInstance, className))
		return -1;

	return MessageLoop();
}
