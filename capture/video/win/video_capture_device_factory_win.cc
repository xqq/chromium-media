// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/capture/video/win/video_capture_device_factory_win.h"

#include <mfapi.h>
#include <mferror.h>
#include <stddef.h>

#include "base/command_line.h"
#include "base/macros.h"
#include "base/metrics/histogram.h"
#include "base/strings/string_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/win/scoped_co_mem.h"
#include "base/win/scoped_variant.h"
#include "base/win/windows_version.h"
#include "media/base/media_switches.h"
#include "media/base/win/mf_initializer.h"
#include "media/capture/video/win/video_capture_device_mf_win.h"
#include "media/capture/video/win/video_capture_device_win.h"

using base::win::ScopedCoMem;
using base::win::ScopedComPtr;
using base::win::ScopedVariant;
using Descriptor = media::VideoCaptureDeviceDescriptor;
using Descriptors = media::VideoCaptureDeviceDescriptors;

namespace media {

// In Windows device identifiers, the USB VID and PID are preceded by the string
// "vid_" or "pid_".  The identifiers are each 4 bytes long.
const char kVidPrefix[] = "vid_";  // Also contains '\0'.
const char kPidPrefix[] = "pid_";  // Also contains '\0'.
const size_t kVidPidSize = 4;

// Avoid enumerating and/or using certain devices due to they provoking crashes
// or any other reason (http://crbug.com/378494). This enum is defined for the
// purposes of UMA collection. Existing entries cannot be removed.
enum BlacklistedCameraNames {
  BLACKLISTED_CAMERA_GOOGLE_CAMERA_ADAPTER = 0,
  BLACKLISTED_CAMERA_IP_CAMERA = 1,
  BLACKLISTED_CAMERA_CYBERLINK_WEBCAM_SPLITTER = 2,
  BLACKLISTED_CAMERA_EPOCCAM = 3,
  // This one must be last, and equal to the previous enumerated value.
  BLACKLISTED_CAMERA_MAX = BLACKLISTED_CAMERA_EPOCCAM,
};

// Blacklisted devices are identified by a characteristic prefix of the name.
// This prefix is used case-insensitively. This list must be kept in sync with
// |BlacklistedCameraNames|.
static const char* const kBlacklistedCameraNames[] = {
    // Name of a fake DirectShow filter on computers with GTalk installed.
    "Google Camera Adapter",
    // The following software WebCams cause crashes.
    "IP Camera [JPEG/MJPEG]", "CyberLink Webcam Splitter", "EpocCam",
};
static_assert(arraysize(kBlacklistedCameraNames) == BLACKLISTED_CAMERA_MAX + 1,
              "kBlacklistedCameraNames should be same size as "
              "BlacklistedCameraNames enum");

static bool LoadMediaFoundationDlls() {
  static const wchar_t* const kMfDLLs[] = {
      L"%WINDIR%\\system32\\mf.dll",
      L"%WINDIR%\\system32\\mfplat.dll",
      L"%WINDIR%\\system32\\mfreadwrite.dll",
  };

  for (const wchar_t* kMfDLL : kMfDLLs) {
    wchar_t path[MAX_PATH] = {0};
    ExpandEnvironmentStringsW(kMfDLL, path, arraysize(path));
    if (!LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH))
      return false;
  }
  return true;
}

static bool PrepareVideoCaptureAttributesMediaFoundation(
    IMFAttributes** attributes,
    int count) {
  InitializeMediaFoundation();

  if (FAILED(MFCreateAttributes(attributes, count)))
    return false;

  return SUCCEEDED(
      (*attributes)
          ->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
}

static bool CreateVideoCaptureDeviceMediaFoundation(const char* sym_link,
                                                    IMFMediaSource** source) {
  ScopedComPtr<IMFAttributes> attributes;
  if (!PrepareVideoCaptureAttributesMediaFoundation(attributes.Receive(), 2))
    return false;

  attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                        base::SysUTF8ToWide(sym_link).c_str());

  return SUCCEEDED(MFCreateDeviceSource(attributes.get(), source));
}

static bool EnumerateVideoDevicesMediaFoundation(IMFActivate*** devices,
                                                 UINT32* count) {
  ScopedComPtr<IMFAttributes> attributes;
  if (!PrepareVideoCaptureAttributesMediaFoundation(attributes.Receive(), 1))
    return false;

  return SUCCEEDED(MFEnumDeviceSources(attributes.get(), devices, count));
}

static bool IsDeviceBlackListed(const std::string& name) {
  DCHECK_EQ(BLACKLISTED_CAMERA_MAX + 1,
            static_cast<int>(arraysize(kBlacklistedCameraNames)));
  for (size_t i = 0; i < arraysize(kBlacklistedCameraNames); ++i) {
    if (base::StartsWith(name, kBlacklistedCameraNames[i],
                         base::CompareCase::INSENSITIVE_ASCII)) {
      DVLOG(1) << "Enumerated blacklisted device: " << name;
      UMA_HISTOGRAM_ENUMERATION("Media.VideoCapture.BlacklistedDevice", i,
                                BLACKLISTED_CAMERA_MAX + 1);
      return true;
    }
  }
  return false;
}

static std::string GetDeviceModelId(const std::string& device_id) {
  const size_t vid_prefix_size = sizeof(kVidPrefix) - 1;
  const size_t pid_prefix_size = sizeof(kPidPrefix) - 1;
  const size_t vid_location = device_id.find(kVidPrefix);
  if (vid_location == std::string::npos ||
      vid_location + vid_prefix_size + kVidPidSize > device_id.size()) {
    return std::string();
  }
  const size_t pid_location = device_id.find(kPidPrefix);
  if (pid_location == std::string::npos ||
      pid_location + pid_prefix_size + kVidPidSize > device_id.size()) {
    return std::string();
  }
  const std::string id_vendor =
      device_id.substr(vid_location + vid_prefix_size, kVidPidSize);
  const std::string id_product =
      device_id.substr(pid_location + pid_prefix_size, kVidPidSize);
  return id_vendor + ":" + id_product;
}

static void GetDeviceDescriptorsDirectShow(Descriptors* device_descriptors) {
  DCHECK(device_descriptors);
  DVLOG(1) << __FUNCTION__;

  ScopedComPtr<ICreateDevEnum> dev_enum;
  HRESULT hr =
      dev_enum.CreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC);
  if (FAILED(hr))
    return;

  ScopedComPtr<IEnumMoniker> enum_moniker;
  hr = dev_enum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                       enum_moniker.Receive(), 0);
  // CreateClassEnumerator returns S_FALSE on some Windows OS
  // when no camera exist. Therefore the FAILED macro can't be used.
  if (hr != S_OK)
    return;

  // Enumerate all video capture devices.
  for (ScopedComPtr<IMoniker> moniker;
       enum_moniker->Next(1, moniker.Receive(), NULL) == S_OK;
       moniker.Release()) {
    ScopedComPtr<IPropertyBag> prop_bag;
    hr = moniker->BindToStorage(0, 0, IID_IPropertyBag, prop_bag.ReceiveVoid());
    if (FAILED(hr))
      continue;

    // Find the description or friendly name.
    ScopedVariant name;
    hr = prop_bag->Read(L"Description", name.Receive(), 0);
    if (FAILED(hr))
      hr = prop_bag->Read(L"FriendlyName", name.Receive(), 0);

    if (FAILED(hr) || name.type() != VT_BSTR)
      continue;

    const std::string device_name(base::SysWideToUTF8(V_BSTR(name.ptr())));
    if (IsDeviceBlackListed(device_name))
      continue;

    name.Reset();
    hr = prop_bag->Read(L"DevicePath", name.Receive(), 0);
    std::string id;
    if (FAILED(hr) || name.type() != VT_BSTR) {
      id = device_name;
    } else {
      DCHECK_EQ(name.type(), VT_BSTR);
      id = base::SysWideToUTF8(V_BSTR(name.ptr()));
    }

    const std::string model_id = GetDeviceModelId(id);

    device_descriptors->emplace_back(device_name, id, model_id,
                                     VideoCaptureApi::WIN_DIRECT_SHOW);
  }
}

static void GetDeviceDescriptorsMediaFoundation(
    Descriptors* device_descriptors) {
  DVLOG(1) << " GetDeviceDescriptorsMediaFoundation";
  ScopedCoMem<IMFActivate*> devices;
  UINT32 count;
  if (!EnumerateVideoDevicesMediaFoundation(&devices, &count))
    return;

  for (UINT32 i = 0; i < count; ++i) {
    ScopedCoMem<wchar_t> name;
    UINT32 name_size;
    HRESULT hr = devices[i]->GetAllocatedString(
        MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &name_size);
    if (SUCCEEDED(hr)) {
      ScopedCoMem<wchar_t> id;
      UINT32 id_size;
      hr = devices[i]->GetAllocatedString(
          MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &id,
          &id_size);
      if (SUCCEEDED(hr)) {
        const std::string device_id =
            base::SysWideToUTF8(std::wstring(id, id_size));
        const std::string model_id = GetDeviceModelId(device_id);
        device_descriptors->emplace_back(
            base::SysWideToUTF8(std::wstring(name, name_size)), device_id,
            model_id, VideoCaptureApi::WIN_MEDIA_FOUNDATION);
      }
    }
    DLOG_IF(ERROR, FAILED(hr)) << "GetAllocatedString failed: "
                               << logging::SystemErrorCodeToString(hr);
    devices[i]->Release();
  }
}

static void GetDeviceSupportedFormatsDirectShow(const Descriptor& descriptor,
                                                VideoCaptureFormats* formats) {
  DVLOG(1) << "GetDeviceSupportedFormatsDirectShow for "
           << descriptor.display_name;
  ScopedComPtr<ICreateDevEnum> dev_enum;
  HRESULT hr =
      dev_enum.CreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC);
  if (FAILED(hr))
    return;

  ScopedComPtr<IEnumMoniker> enum_moniker;
  hr = dev_enum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                       enum_moniker.Receive(), 0);
  // CreateClassEnumerator returns S_FALSE on some Windows OS when no camera
  // exists. Therefore the FAILED macro can't be used.
  if (hr != S_OK)
    return;

  // Walk the capture devices. No need to check for device presence again since
  // that is anyway needed in GetDeviceFilter(). "google camera adapter" and old
  // VFW devices are already skipped previously in GetDeviceNames() enumeration.
  base::win::ScopedComPtr<IBaseFilter> capture_filter;
  hr = VideoCaptureDeviceWin::GetDeviceFilter(descriptor.device_id,
                                              capture_filter.Receive());
  if (!capture_filter.get()) {
    DLOG(ERROR) << "Failed to create capture filter: "
                << logging::SystemErrorCodeToString(hr);
    return;
  }

  base::win::ScopedComPtr<IPin> output_capture_pin(
      VideoCaptureDeviceWin::GetPin(capture_filter.get(), PINDIR_OUTPUT,
                                    PIN_CATEGORY_CAPTURE, GUID_NULL));
  if (!output_capture_pin.get()) {
    DLOG(ERROR) << "Failed to get capture output pin";
    return;
  }

  ScopedComPtr<IAMStreamConfig> stream_config;
  hr = output_capture_pin.QueryInterface(stream_config.Receive());
  if (FAILED(hr)) {
    DLOG(ERROR) << "Failed to get IAMStreamConfig interface from "
                   "capture device: " << logging::SystemErrorCodeToString(hr);
    return;
  }

  int count = 0, size = 0;
  hr = stream_config->GetNumberOfCapabilities(&count, &size);
  if (FAILED(hr)) {
    DLOG(ERROR) << "GetNumberOfCapabilities failed: "
                << logging::SystemErrorCodeToString(hr);
    return;
  }

  std::unique_ptr<BYTE[]> caps(new BYTE[size]);
  for (int i = 0; i < count; ++i) {
    VideoCaptureDeviceWin::ScopedMediaType media_type;
    hr = stream_config->GetStreamCaps(i, media_type.Receive(), caps.get());
    // GetStreamCaps() may return S_FALSE, so don't use FAILED() or SUCCEED()
    // macros here since they'll trigger incorrectly.
    if (hr != S_OK || !media_type.get()) {
      DLOG(ERROR) << "GetStreamCaps failed: "
                  << logging::SystemErrorCodeToString(hr);
      return;
    }

    if (media_type->majortype == MEDIATYPE_Video &&
        media_type->formattype == FORMAT_VideoInfo) {
      VideoCaptureFormat format;
      format.pixel_format =
          VideoCaptureDeviceWin::TranslateMediaSubtypeToPixelFormat(
              media_type->subtype);
      if (format.pixel_format == PIXEL_FORMAT_UNKNOWN)
        continue;
      VIDEOINFOHEADER* h =
          reinterpret_cast<VIDEOINFOHEADER*>(media_type->pbFormat);
      format.frame_size.SetSize(h->bmiHeader.biWidth, h->bmiHeader.biHeight);
      // Trust the frame rate from the VIDEOINFOHEADER.
      format.frame_rate =
          (h->AvgTimePerFrame > 0)
              ? kSecondsToReferenceTime / static_cast<float>(h->AvgTimePerFrame)
              : 0.0f;
      formats->push_back(format);
      DVLOG(1) << descriptor.display_name << " "
               << VideoCaptureFormat::ToString(format);
    }
  }
}

static void GetDeviceSupportedFormatsMediaFoundation(
    const Descriptor& descriptor,
    VideoCaptureFormats* formats) {
  DVLOG(1) << "GetDeviceSupportedFormatsMediaFoundation for "
           << descriptor.display_name;
  ScopedComPtr<IMFMediaSource> source;
  if (!CreateVideoCaptureDeviceMediaFoundation(descriptor.device_id.c_str(),
                                               source.Receive())) {
    return;
  }

  base::win::ScopedComPtr<IMFSourceReader> reader;
  HRESULT hr =
      MFCreateSourceReaderFromMediaSource(source.get(), NULL, reader.Receive());
  if (FAILED(hr)) {
    DLOG(ERROR) << "MFCreateSourceReaderFromMediaSource failed: "
                << logging::SystemErrorCodeToString(hr);
    return;
  }

  DWORD stream_index = 0;
  ScopedComPtr<IMFMediaType> type;
  while (SUCCEEDED(reader->GetNativeMediaType(kFirstVideoStream, stream_index,
                                              type.Receive()))) {
    UINT32 width, height;
    hr = MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE, &width, &height);
    if (FAILED(hr)) {
      DLOG(ERROR) << "MFGetAttributeSize failed: "
                  << logging::SystemErrorCodeToString(hr);
      return;
    }
    VideoCaptureFormat capture_format;
    capture_format.frame_size.SetSize(width, height);

    UINT32 numerator, denominator;
    hr = MFGetAttributeRatio(type.get(), MF_MT_FRAME_RATE, &numerator,
                             &denominator);
    if (FAILED(hr)) {
      DLOG(ERROR) << "MFGetAttributeSize failed: "
                  << logging::SystemErrorCodeToString(hr);
      return;
    }
    capture_format.frame_rate =
        denominator ? static_cast<float>(numerator) / denominator : 0.0f;

    GUID type_guid;
    hr = type->GetGUID(MF_MT_SUBTYPE, &type_guid);
    if (FAILED(hr)) {
      DLOG(ERROR) << "GetGUID failed: " << logging::SystemErrorCodeToString(hr);
      return;
    }
    VideoCaptureDeviceMFWin::FormatFromGuid(type_guid,
                                            &capture_format.pixel_format);
    type.Release();
    ++stream_index;
    if (capture_format.pixel_format == PIXEL_FORMAT_UNKNOWN)
      continue;
    formats->push_back(capture_format);

    DVLOG(1) << descriptor.display_name << " "
             << VideoCaptureFormat::ToString(capture_format);
  }
}

// Returns true iff the current platform supports the Media Foundation API
// and that the DLLs are available.  On Vista this API is an optional download
// but the API is advertised as a part of Windows 7 and onwards.  However,
// we've seen that the required DLLs are not available in some Win7
// distributions such as Windows 7 N and Windows 7 KN.
// static
bool VideoCaptureDeviceFactoryWin::PlatformSupportsMediaFoundation() {
  // Even though the DLLs might be available on Vista, we get crashes
  // when running our tests on the build bots.
  if (base::win::GetVersion() < base::win::VERSION_WIN7)
    return false;

  static bool g_dlls_available = LoadMediaFoundationDlls();
  return g_dlls_available;
}

VideoCaptureDeviceFactoryWin::VideoCaptureDeviceFactoryWin()
    : use_media_foundation_(base::win::GetVersion() >=
                                base::win::VERSION_WIN7 &&
                            base::CommandLine::ForCurrentProcess()->HasSwitch(
                                switches::kForceMediaFoundationVideoCapture)) {}

std::unique_ptr<VideoCaptureDevice> VideoCaptureDeviceFactoryWin::CreateDevice(
    const Descriptor& device_descriptor) {
  DCHECK(thread_checker_.CalledOnValidThread());
  std::unique_ptr<VideoCaptureDevice> device;
  if (device_descriptor.capture_api == VideoCaptureApi::WIN_MEDIA_FOUNDATION) {
    DCHECK(PlatformSupportsMediaFoundation());
    device.reset(new VideoCaptureDeviceMFWin(device_descriptor));
    DVLOG(1) << " MediaFoundation Device: " << device_descriptor.display_name;
    ScopedComPtr<IMFMediaSource> source;
    if (!CreateVideoCaptureDeviceMediaFoundation(
            device_descriptor.device_id.c_str(), source.Receive())) {
      return std::unique_ptr<VideoCaptureDevice>();
    }
    if (!static_cast<VideoCaptureDeviceMFWin*>(device.get())->Init(source))
      device.reset();
  } else if (device_descriptor.capture_api ==
             VideoCaptureApi::WIN_DIRECT_SHOW) {
    device.reset(new VideoCaptureDeviceWin(device_descriptor));
    DVLOG(1) << " DirectShow Device: " << device_descriptor.display_name;
    if (!static_cast<VideoCaptureDeviceWin*>(device.get())->Init())
      device.reset();
  } else {
    NOTREACHED();
  }
  return device;
}

void VideoCaptureDeviceFactoryWin::GetDeviceDescriptors(
    VideoCaptureDeviceDescriptors* device_descriptors) {
  DCHECK(thread_checker_.CalledOnValidThread());
  if (use_media_foundation_)
    GetDeviceDescriptorsMediaFoundation(device_descriptors);
  else
    GetDeviceDescriptorsDirectShow(device_descriptors);
}

void VideoCaptureDeviceFactoryWin::GetSupportedFormats(
    const Descriptor& device,
    VideoCaptureFormats* formats) {
  DCHECK(thread_checker_.CalledOnValidThread());
  if (use_media_foundation_)
    GetDeviceSupportedFormatsMediaFoundation(device, formats);
  else
    GetDeviceSupportedFormatsDirectShow(device, formats);
}

// static
VideoCaptureDeviceFactory*
VideoCaptureDeviceFactory::CreateVideoCaptureDeviceFactory(
    scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner) {
  return new VideoCaptureDeviceFactoryWin();
}

}  // namespace media
