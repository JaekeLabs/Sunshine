/**
 * @file src/platform/windows/display_wgc.cpp
 * @brief Definitions for WinRT Windows.Graphics.Capture API
 */
// platform includes
#include <dxgi1_2.h>

// local includes
#include "display.h"
#include "misc.h"
#include "src/logging.h"

// Gross hack to work around MINGW-packages#22160
#define ____FIReference_1_boolean_INTERFACE_DEFINED__

#include <Windows.Graphics.Capture.Interop.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.foundation.metadata.h>
#include <winrt/windows.graphics.directx.direct3d11.h>

namespace platf {
  using namespace std::literals;
}

namespace winrt {
  using namespace Windows::Foundation;
  using namespace Windows::Foundation::Metadata;
  using namespace Windows::Graphics::Capture;
  using namespace Windows::Graphics::DirectX::Direct3D11;

  extern "C" {
    /**
     * @brief Create direct3 D11 device from DXGI device.
     *
     * @param dxgiDevice DXGI device.
     * @param graphicsDevice Graphics device.
     * @return Created direct3 D11 device from DXGI device object or status.
     */
    HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(::IDXGIDevice *dxgiDevice, ::IInspectable **graphicsDevice);
  }

  /**
   * Windows structures sometimes have compile-time GUIDs. GCC supports this, but in a roundabout way.
   * If WINRT_IMPL_HAS_DECLSPEC_UUID is true, then the compiler supports adding this attribute to a struct. For example, Visual Studio.
   * If not, then MinGW GCC has a workaround to assign a GUID to a structure.
   */
  struct
#if WINRT_IMPL_HAS_DECLSPEC_UUID
    __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
#endif
    IDirect3DDxgiInterfaceAccess: ::IUnknown {
    /**
     * @brief Retrieve a DXGI interface from a WinRT Direct3D object.
     *
     * @param id COM interface ID requested from the WinRT wrapper.
     * @param object Output pointer that receives the requested COM interface.
     * @return HRESULT from the WinRT object's interface query.
     */
    virtual HRESULT __stdcall GetInterface(REFIID id, void **object) = 0;
  };
}  // namespace winrt
#if !WINRT_IMPL_HAS_DECLSPEC_UUID
static constexpr GUID GUID__IDirect3DDxgiInterfaceAccess = {
  0xA9B3D012,
  0x3DF2,
  0x4EE3,
  {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}
  // compare with __declspec(uuid(...)) for the struct above.
};

/**
 * @brief Return the GUID used to request IDirect3DDxgiInterfaceAccess.
 *
 * @return GUID for the WinRT DXGI interface-access helper.
 */
template<>
constexpr auto __mingw_uuidof<winrt::IDirect3DDxgiInterfaceAccess>() -> GUID const & {
  return GUID__IDirect3DDxgiInterfaceAccess;
}
#endif

namespace platf::dxgi {
  namespace {
    bool is_edge_capture_window(HWND window) {
      if (!window || !IsWindowVisible(window) || IsIconic(window) || GetAncestor(window, GA_ROOT) != window) {
        return false;
      }

      DWORD process_id = 0;
      GetWindowThreadProcessId(window, &process_id);
      if (process_id == 0) {
        return false;
      }

      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
      if (!process) {
        return false;
      }

      wchar_t process_path[32768] {};
      DWORD process_path_size = static_cast<DWORD>(sizeof(process_path) / sizeof(process_path[0]));
      const bool queried = QueryFullProcessImageNameW(process, 0, process_path, &process_path_size) != FALSE;
      CloseHandle(process);
      if (!queried) {
        return false;
      }

      const wchar_t *filename = process_path;
      if (const wchar_t *slash = wcsrchr(process_path, L'\\')) {
        filename = slash + 1;
      }

      return _wcsicmp(filename, L"msedge.exe") == 0;
    }

    struct edge_window_search_t {
      HWND window = nullptr;
      unsigned int count = 0;
    };

    BOOL CALLBACK enumerate_edge_capture_windows(HWND window, LPARAM context) {
      auto *search = reinterpret_cast<edge_window_search_t *>(context);
      if (is_edge_capture_window(window)) {
        if (!search->window) {
          search->window = window;
        }
        ++search->count;
      }
      return TRUE;
    }

    HWND find_edge_capture_window() {
      const HWND foreground = GetForegroundWindow();
      if (is_edge_capture_window(foreground)) {
        return foreground;
      }

      edge_window_search_t search;
      EnumWindows(enumerate_edge_capture_windows, reinterpret_cast<LPARAM>(&search));

      if (search.count == 1) {
        return search.window;
      }

      if (search.count > 1) {
        BOOST_LOG(error) << "Multiple Microsoft Edge windows are open. Focus the window you want to stream, then reconnect."sv;
      } else {
        BOOST_LOG(error) << "No visible Microsoft Edge window is available for capture. Open Edge before starting or reconnecting the stream."sv;
      }
      return nullptr;
    }
  }  // namespace

  wgc_capture_t::wgc_capture_t() {
    InitializeConditionVariable(&frame_present_cv);
  }

  wgc_capture_t::~wgc_capture_t() {
    if (capture_session) {
      capture_session.Close();
    }
    if (frame_pool) {
      frame_pool.Close();
    }
    item = nullptr;
    capture_session = nullptr;
    frame_pool = nullptr;
  }

  /**
   * @brief Initialize the Windows.Graphics.Capture backend.
   * @return 0 on success, -1 on failure.
   */
  int wgc_capture_t::init(display_base_t *display, const ::video::config_t &config) {
    HRESULT status;
    dxgi::dxgi_t dxgi;
    winrt::com_ptr<::IInspectable> d3d_comhandle;
    try {
      if (!winrt::GraphicsCaptureSession::IsSupported()) {
        BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows!"sv;
        return -1;
      }
      if (FAILED(status = display->device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi))) {
        BOOST_LOG(error) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
      if (FAILED(status = winrt::CreateDirect3D11DeviceFromDXGIDevice(*&dxgi, d3d_comhandle.put()))) {
        BOOST_LOG(error) << "Failed to query WinRT DirectX interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire device: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }

    uwp_device = d3d_comhandle.as<winrt::IDirect3DDevice>();

    const HWND edge_window = find_edge_capture_window();
    if (!edge_window) {
      return -1;
    }

    auto capture_factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    if (capture_factory == nullptr || FAILED(status = capture_factory->CreateForWindow(edge_window, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item)))) {
      BOOST_LOG(error) << "Failed to create a Windows.Graphics.Capture item for Microsoft Edge: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    const auto capture_size = item.Size();
    if (capture_size.Width <= 0 || capture_size.Height <= 0) {
      BOOST_LOG(error) << "Microsoft Edge reported an invalid capture size ["sv << capture_size.Width << 'x' << capture_size.Height << ']';
      return -1;
    }

    capture_width = capture_size.Width;
    capture_height = capture_size.Height;

    // Privacy boundary: this prototype never falls back to monitor capture.
    // The WGC item is a window rather than a rotated DXGI output, so update the
    // dimensions inherited from display_base_t to match the captured Edge window.
    display->width = capture_size.Width;
    display->height = capture_size.Height;
    display->width_before_rotation = capture_size.Width;
    display->height_before_rotation = capture_size.Height;
    display->display_rotation = DXGI_MODE_ROTATION_IDENTITY;
    display->offset_x = 0;
    display->offset_y = 0;

    BOOST_LOG(info) << "Capturing Microsoft Edge window ["sv << capture_size.Width << 'x' << capture_size.Height << ']';

    if (config.dynamicRange) {
      display->capture_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    } else {
      display->capture_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    try {
      frame_pool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(uwp_device, static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(display->capture_format), 2, item.Size());
      capture_session = frame_pool.CreateCaptureSession(item);
      frame_pool.FrameArrived({this, &wgc_capture_t::on_frame_arrived});
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to create capture session: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }
    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
        capture_session.IsBorderRequired(false);
      } else {
        BOOST_LOG(warning) << "Can't disable colored border around capture area on this version of Windows";
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Screen capture may not be fully supported on this device for this release of Windows: failed to disable border around capture area: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }
    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
        capture_session.MinUpdateInterval(4ms);  // 250Hz
      } else {
        BOOST_LOG(warning) << "Can't set MinUpdateInterval on this version of Windows";
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Screen capture may be capped to 60fps on this device for this release of Windows: failed to set MinUpdateInterval: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }
    try {
      capture_session.StartCapture();
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to start capture: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }
    return 0;
  }

  /**
   * This function runs in a separate thread spawned by the frame pool and is a producer of frames.
   * To maintain parity with the original display interface, this frame will be consumed by the capture thread.
   * Acquire a read-write lock, make the produced frame available to the capture thread, then wake the capture thread.
   */
  void wgc_capture_t::on_frame_arrived(winrt::Direct3D11CaptureFramePool const &sender, winrt::IInspectable const &) {
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame {nullptr};
    try {
      frame = sender.TryGetNextFrame();
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Failed to capture frame: "sv << e.code();
      return;
    }
    if (frame != nullptr) {
      AcquireSRWLockExclusive(&frame_lock);
      if (produced_frame) {
        produced_frame.Close();
      }

      produced_frame = frame;
      ReleaseSRWLockExclusive(&frame_lock);
      WakeConditionVariable(&frame_present_cv);
    }
  }

  /**
   * @brief Get the next frame from the producer thread.
   * If not available, the capture thread blocks until one is, or the wait times out.
   */
  capture_e wgc_capture_t::next_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out, uint64_t &out_time) {
    // this CONSUMER runs in the capture thread
    release_frame();

    AcquireSRWLockExclusive(&frame_lock);
    if (produced_frame == nullptr && SleepConditionVariableSRW(&frame_present_cv, &frame_lock, timeout.count(), 0) == 0) {
      ReleaseSRWLockExclusive(&frame_lock);
      if (GetLastError() == ERROR_TIMEOUT) {
        return capture_e::timeout;
      } else {
        return capture_e::error;
      }
    }
    if (produced_frame) {
      consumed_frame = produced_frame;
      produced_frame = nullptr;
    }
    ReleaseSRWLockExclusive(&frame_lock);
    if (consumed_frame == nullptr) {  // spurious wakeup
      return capture_e::timeout;
    }

    // WGC frame-pool textures retain the pool's old dimensions until the pool
    // is recreated, so the texture description alone cannot reliably detect a
    // resized window. ContentSize reflects the actual captured window size.
    // Ask Sunshine to rebuild the capture backend before forwarding a stale
    // frame; init() will then create a new frame pool using the new Edge size.
    const auto content_size = consumed_frame.ContentSize();
    if (content_size.Width <= 0 || content_size.Height <= 0) {
      release_frame();
      return capture_e::timeout;
    }
    if (content_size.Width != capture_width || content_size.Height != capture_height) {
      BOOST_LOG(info) << "Microsoft Edge capture size changed ["sv << capture_width << 'x' << capture_height << " -> "sv << content_size.Width << 'x' << content_size.Height << ']';
      release_frame();
      return capture_e::reinit;
    }

    auto capture_access = consumed_frame.Surface().as<winrt::IDirect3DDxgiInterfaceAccess>();
    if (capture_access == nullptr) {
      return capture_e::error;
    }
    capture_access->GetInterface(IID_ID3D11Texture2D, (void **) out);
    out_time = consumed_frame.SystemRelativeTime().count();  // raw ticks from query performance counter
    return capture_e::ok;
  }

  capture_e wgc_capture_t::release_frame() {
    if (consumed_frame != nullptr) {
      consumed_frame.Close();
      consumed_frame = nullptr;
    }
    return capture_e::ok;
  }

  int wgc_capture_t::set_cursor_visible(bool x) {
    try {
      if (capture_session.IsCursorCaptureEnabled() != x) {
        capture_session.IsCursorCaptureEnabled(x);
      }
      return 0;
    } catch (winrt::hresult_error &) {
      return -1;
    }
  }

  int display_wgc_ram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    texture.reset();
    return 0;
  }

  /**
   * @brief Get the next frame from the Windows.Graphics.Capture API and copy it into a new snapshot texture.
   */
  capture_e display_wgc_ram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    HRESULT status;
    texture2d_t src;
    uint64_t frame_qpc;
    dup.set_cursor_visible(cursor_visible);
    auto capture_status = dup.next_frame(timeout, &src, frame_qpc);
    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    auto frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), frame_qpc);
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);

    // Create the staging texture if it doesn't exist. It should match the source in size and format.
    if (texture == nullptr) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "Capture format ["sv << dxgi_format_to_string(capture_format) << ']';

      D3D11_TEXTURE2D_DESC t {};
      t.Width = width;
      t.Height = height;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_STAGING;
      t.Format = capture_format;
      t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      auto status = device->CreateTexture2D(&t, nullptr, &texture);

      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create staging texture [0x"sv << util::hex(status).to_string_view() << ']';
        return capture_e::error;
      }
    }

    // It's possible for our display enumeration to race with mode changes and result in
    // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
    if (desc.Width != width || desc.Height != height) {
      BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }
    // It's also possible for the capture format to change on the fly. If that happens,
    // reinitialize capture to try format detection again and create new images.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    // Copy from GPU to CPU
    device_ctx->CopyResource(texture.get(), src.get());

    if (!pull_free_image_cb(img_out)) {
      return capture_e::interrupted;
    }
    auto img = (img_t *) img_out.get();

    // Map the staging texture for CPU access (making it inaccessible for the GPU)
    if (FAILED(status = device_ctx->Map(texture.get(), 0, D3D11_MAP_READ, 0, &img_info))) {
      BOOST_LOG(error) << "Failed to map texture [0x"sv << util::hex(status).to_string_view() << ']';

      return capture_e::error;
    }

    // Now that we know the capture format, we can finish creating the image
    if (complete_img(img, false)) {
      device_ctx->Unmap(texture.get(), 0);
      img_info.pData = nullptr;
      return capture_e::error;
    }

    std::copy_n((std::uint8_t *) img_info.pData, height * img_info.RowPitch, (std::uint8_t *) img->data);

    // Unmap the staging texture to allow GPU access again
    device_ctx->Unmap(texture.get(), 0);
    img_info.pData = nullptr;

    if (img) {
      img->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_wgc_ram_t::release_snapshot() {
    return dup.release_frame();
  }
}  // namespace platf::dxgi
