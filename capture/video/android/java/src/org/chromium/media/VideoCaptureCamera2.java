// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.media;

import android.annotation.TargetApi;
import android.content.Context;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CameraMetadata;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Range;
import android.util.Size;
import android.view.Surface;

import org.chromium.base.Log;
import org.chromium.base.annotations.JNINamespace;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

/**
 * This class implements Video Capture using Camera2 API, introduced in Android
 * API 21 (L Release). Capture takes place in the current Looper, while pixel
 * download takes place in another thread used by ImageReader. A number of
 * static methods are provided to retrieve information on current system cameras
 * and their capabilities, using android.hardware.camera2.CameraManager.
 **/
@JNINamespace("media")
@TargetApi(Build.VERSION_CODES.LOLLIPOP)
public class VideoCaptureCamera2 extends VideoCapture {
    // Inner class to extend a CameraDevice state change listener.
    private class CrStateListener extends CameraDevice.StateCallback {
        @Override
        public void onOpened(CameraDevice cameraDevice) {
            mCameraDevice = cameraDevice;
            changeCameraStateAndNotify(CameraState.CONFIGURING);
            if (!createPreviewObjects()) {
                changeCameraStateAndNotify(CameraState.STOPPED);
                nativeOnError(mNativeVideoCaptureDeviceAndroid, "Error configuring camera");
            }
        }

        @Override
        public void onDisconnected(CameraDevice cameraDevice) {
            cameraDevice.close();
            mCameraDevice = null;
            changeCameraStateAndNotify(CameraState.STOPPED);
        }

        @Override
        public void onError(CameraDevice cameraDevice, int error) {
            cameraDevice.close();
            mCameraDevice = null;
            changeCameraStateAndNotify(CameraState.STOPPED);
            nativeOnError(mNativeVideoCaptureDeviceAndroid,
                    "Camera device error " + Integer.toString(error));
        }
    };

    // Inner class to extend a Capture Session state change listener.
    private class CrPreviewSessionListener extends CameraCaptureSession.StateCallback {
        private final CaptureRequest mPreviewRequest;
        CrPreviewSessionListener(CaptureRequest previewRequest) {
            mPreviewRequest = previewRequest;
        }

        @Override
        public void onConfigured(CameraCaptureSession cameraCaptureSession) {
            Log.d(TAG, "onConfigured");
            mPreviewSession = cameraCaptureSession;
            try {
                // This line triggers the preview. No |listener| is registered, so we will not get
                // notified of capture events, instead, CrImageReaderListener will trigger every
                // time a downloaded image is ready. Since |handler| is null, we'll work on the
                // current Thread Looper.
                mPreviewSession.setRepeatingRequest(mPreviewRequest, null, null);
            } catch (CameraAccessException | SecurityException | IllegalStateException
                    | IllegalArgumentException ex) {
                Log.e(TAG, "setRepeatingRequest: ", ex);
                return;
            }
            // Now wait for trigger on CrImageReaderListener.onImageAvailable();
            changeCameraStateAndNotify(CameraState.STARTED);
        }

        @Override
        public void onConfigureFailed(CameraCaptureSession cameraCaptureSession) {
            // TODO(mcasas): When signalling error, C++ will tear us down. Is there need for
            // cleanup?
            changeCameraStateAndNotify(CameraState.STOPPED);
            nativeOnError(mNativeVideoCaptureDeviceAndroid, "Camera session configuration error");
        }
    };

    // Internal class implementing an ImageReader listener for Preview frames. Gets pinged when a
    // new frame is been captured and downloads it to memory-backed buffers.
    private class CrImageReaderListener implements ImageReader.OnImageAvailableListener {
        @Override
        public void onImageAvailable(ImageReader reader) {
            try (Image image = reader.acquireLatestImage()) {
                if (image == null) return;

                if (image.getFormat() != ImageFormat.YUV_420_888 || image.getPlanes().length != 3) {
                    nativeOnError(mNativeVideoCaptureDeviceAndroid, "Unexpected image format: "
                            + image.getFormat() + " or #planes: " + image.getPlanes().length);
                    throw new IllegalStateException();
                }

                if (reader.getWidth() != image.getWidth()
                        || reader.getHeight() != image.getHeight()) {
                    nativeOnError(mNativeVideoCaptureDeviceAndroid, "ImageReader size ("
                            + reader.getWidth() + "x" + reader.getHeight()
                            + ") did not match Image size (" + image.getWidth() + "x"
                            + image.getHeight() + ")");
                    throw new IllegalStateException();
                }

                nativeOnI420FrameAvailable(mNativeVideoCaptureDeviceAndroid,
                        image.getPlanes()[0].getBuffer(), image.getPlanes()[0].getRowStride(),
                        image.getPlanes()[1].getBuffer(), image.getPlanes()[2].getBuffer(),
                        image.getPlanes()[1].getRowStride(), image.getPlanes()[1].getPixelStride(),
                        image.getWidth(), image.getHeight(), getCameraRotation());
            } catch (IllegalStateException ex) {
                Log.e(TAG, "acquireLatestImage():", ex);
            }
        }
    };

    // Inner class to extend a Photo Session state change listener.
    // Error paths must signal notifyTakePhotoError().
    private class CrPhotoSessionListener extends CameraCaptureSession.StateCallback {
        private final CaptureRequest mPhotoRequest;
        private final long mCallbackId;
        CrPhotoSessionListener(CaptureRequest photoRequest, long callbackId) {
            mPhotoRequest = photoRequest;
            mCallbackId = callbackId;
        }

        @Override
        public void onConfigured(CameraCaptureSession session) {
            Log.d(TAG, "onConfigured");
            try {
                // This line triggers a single photo capture. No |listener| is registered, so we
                // will get notified via a CrPhotoSessionListener. Since |handler| is null, we'll
                // work on the current Thread Looper.
                session.capture(mPhotoRequest, null, null);
            } catch (CameraAccessException e) {
                Log.e(TAG, "capture() error");
                notifyTakePhotoError(mCallbackId);
                return;
            }
        }

        @Override
        public void onConfigureFailed(CameraCaptureSession session) {
            Log.e(TAG, "failed configuring capture session");
            notifyTakePhotoError(mCallbackId);
            return;
        }
    };

    // Internal class implementing an ImageReader listener for encoded Photos.
    // Gets pinged when a new Image is been captured.
    private class CrPhotoReaderListener implements ImageReader.OnImageAvailableListener {
        private final long mCallbackId;
        CrPhotoReaderListener(long callbackId) {
            mCallbackId = callbackId;
        }

        private byte[] readCapturedData(Image image) {
            byte[] capturedData = null;
            try {
                capturedData = image.getPlanes()[0].getBuffer().array();
            } catch (UnsupportedOperationException ex) {
                // Try reading the pixels in a different way.
                final ByteBuffer buffer = image.getPlanes()[0].getBuffer();
                capturedData = new byte[buffer.remaining()];
                buffer.get(capturedData);
            } finally {
                return capturedData;
            }
        }

        @Override
        public void onImageAvailable(ImageReader reader) {
            Log.d(TAG, "CrPhotoReaderListener.mCallbackId " + mCallbackId);
            try (Image image = reader.acquireLatestImage()) {
                if (image == null) {
                    throw new IllegalStateException();
                }

                if (image.getFormat() != ImageFormat.JPEG) {
                    Log.e(TAG, "Unexpected image format: %d", image.getFormat());
                    throw new IllegalStateException();
                }

                final byte[] capturedData = readCapturedData(image);
                nativeOnPhotoTaken(mNativeVideoCaptureDeviceAndroid, mCallbackId, capturedData);

            } catch (IllegalStateException ex) {
                notifyTakePhotoError(mCallbackId);
                return;
            }

            if (createPreviewObjects()) return;

            nativeOnError(mNativeVideoCaptureDeviceAndroid, "Error restarting preview");
        }
    };

    // Inner Runnable to restart capture, must be run on |mContext| looper.
    private final Runnable mRestartCapture = new Runnable() {
        @Override
        public void run() {
            mPreviewSession.close(); // Asynchronously kill the CaptureSession.
            createPreviewObjects();
        }
    };

    private static final double kNanoSecondsToFps = 1.0E-9;
    private static final String TAG = "VideoCapture";

    private static enum CameraState { OPENING, CONFIGURING, STARTED, STOPPED }

    private final Object mCameraStateLock = new Object();

    private CameraDevice mCameraDevice;
    private CameraCaptureSession mPreviewSession;
    private CaptureRequest mPreviewRequest;

    private CameraState mCameraState = CameraState.STOPPED;
    private final float mMaxZoom;
    private Rect mCropRect = new Rect();

    // Service function to grab CameraCharacteristics and handle exceptions.
    private static CameraCharacteristics getCameraCharacteristics(Context appContext, int id) {
        final CameraManager manager =
                (CameraManager) appContext.getSystemService(Context.CAMERA_SERVICE);
        try {
            return manager.getCameraCharacteristics(Integer.toString(id));
        } catch (CameraAccessException ex) {
            Log.e(TAG, "getCameraCharacteristics: ", ex);
        }
        return null;
    }

    // {@link nativeOnPhotoTaken()} needs to be called back if there's any
    // problem after {@link takePhoto()} has returned true.
    private void notifyTakePhotoError(long callbackId) {
        nativeOnPhotoTaken(mNativeVideoCaptureDeviceAndroid, callbackId, new byte[0]);
    }

    private boolean createPreviewObjects() {
        Log.d(TAG, "createPreviewObjects");
        if (mCameraDevice == null) return false;

        // Create an ImageReader and plug a thread looper into it to have
        // readback take place on its own thread.
        final ImageReader imageReader = ImageReader.newInstance(mCaptureFormat.getWidth(),
                mCaptureFormat.getHeight(), mCaptureFormat.getPixelFormat(), 2 /* maxImages */);
        HandlerThread thread = new HandlerThread("CameraPreview");
        thread.start();
        final Handler backgroundHandler = new Handler(thread.getLooper());
        final CrImageReaderListener imageReaderListener = new CrImageReaderListener();
        imageReader.setOnImageAvailableListener(imageReaderListener, backgroundHandler);

        // The Preview template specifically means "high frame rate is given
        // priority over the highest-quality post-processing".
        CaptureRequest.Builder previewRequestBuilder = null;
        try {
            previewRequestBuilder =
                    mCameraDevice.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
        } catch (CameraAccessException | IllegalArgumentException | SecurityException ex) {
            Log.e(TAG, "createCaptureRequest: ", ex);
            return false;
        }
        if (previewRequestBuilder == null) {
            Log.e(TAG, "previewRequestBuilder error");
            return false;
        }
        // Construct an ImageReader Surface and plug it into our CaptureRequest.Builder.
        previewRequestBuilder.addTarget(imageReader.getSurface());

        // A series of configuration options in the PreviewBuilder
        previewRequestBuilder.set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_AUTO);
        previewRequestBuilder.set(
                CaptureRequest.NOISE_REDUCTION_MODE, CameraMetadata.NOISE_REDUCTION_MODE_FAST);
        previewRequestBuilder.set(CaptureRequest.EDGE_MODE, CameraMetadata.EDGE_MODE_FAST);
        previewRequestBuilder.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE,
                CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_ON);
        // SENSOR_EXPOSURE_TIME ?
        if (!mCropRect.isEmpty()) {
            previewRequestBuilder.set(CaptureRequest.SCALER_CROP_REGION, mCropRect);
        }

        List<Surface> surfaceList = new ArrayList<Surface>(1);
        surfaceList.add(imageReader.getSurface());

        mPreviewRequest = previewRequestBuilder.build();
        final CrPreviewSessionListener captureSessionListener =
                new CrPreviewSessionListener(mPreviewRequest);
        try {
            mCameraDevice.createCaptureSession(surfaceList, captureSessionListener, null);
        } catch (CameraAccessException | IllegalArgumentException | SecurityException ex) {
            Log.e(TAG, "createCaptureSession: ", ex);
            return false;
        }
        // Wait for trigger on CrPreviewSessionListener.onConfigured();
        return true;
    }

    private void changeCameraStateAndNotify(CameraState state) {
        synchronized (mCameraStateLock) {
            mCameraState = state;
            mCameraStateLock.notifyAll();
        }
    }

    // Finds the closest Size to (|width|x|height|) in |sizes|, and returns it or null.
    // Ignores |width| or |height| if either is zero (== don't care).
    private static Size findClosestSizeInArray(Size[] sizes, int width, int height) {
        if (sizes == null) return null;
        Size closestSize = null;
        int minDiff = Integer.MAX_VALUE;
        for (Size size : sizes) {
            final int diff = ((width > 0) ? Math.abs(size.getWidth() - width) : 0)
                    + ((height > 0) ? Math.abs(size.getHeight() - height) : 0);
            if (diff < minDiff) {
                minDiff = diff;
                closestSize = size;
            }
        }
        if (minDiff == Integer.MAX_VALUE) {
            Log.e(TAG, "Couldn't find resolution close to (%dx%d)", width, height);
            return null;
        }
        return closestSize;
    }

    static boolean isLegacyDevice(Context appContext, int id) {
        final CameraCharacteristics cameraCharacteristics =
                getCameraCharacteristics(appContext, id);
        return cameraCharacteristics != null
                && cameraCharacteristics.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL)
                == CameraMetadata.INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY;
    }

    static int getNumberOfCameras(Context appContext) {
        final CameraManager manager =
                (CameraManager) appContext.getSystemService(Context.CAMERA_SERVICE);
        try {
            return manager.getCameraIdList().length;
        } catch (CameraAccessException | SecurityException ex) {
            // SecurityException is an undocumented exception, but has been seen in
            // http://crbug/605424.
            Log.e(TAG, "getNumberOfCameras: getCameraIdList(): ", ex);
            return 0;
        }
    }

    static int getCaptureApiType(int id, Context appContext) {
        final CameraCharacteristics cameraCharacteristics =
                getCameraCharacteristics(appContext, id);
        if (cameraCharacteristics == null) {
            return VideoCaptureApi.UNKNOWN;
        }

        final int supportedHWLevel =
                cameraCharacteristics.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL);
        switch (supportedHWLevel) {
            case CameraMetadata.INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY:
                return VideoCaptureApi.ANDROID_API2_LEGACY;
            case CameraMetadata.INFO_SUPPORTED_HARDWARE_LEVEL_FULL:
                return VideoCaptureApi.ANDROID_API2_FULL;
            case CameraMetadata.INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED:
                return VideoCaptureApi.ANDROID_API2_LIMITED;
            default:
                return VideoCaptureApi.ANDROID_API2_LEGACY;
        }
    }

    static String getName(int id, Context appContext) {
        final CameraCharacteristics cameraCharacteristics =
                getCameraCharacteristics(appContext, id);
        if (cameraCharacteristics == null) return null;
        final int facing = cameraCharacteristics.get(CameraCharacteristics.LENS_FACING);
        return "camera2 " + id + ", facing "
                + ((facing == CameraCharacteristics.LENS_FACING_FRONT) ? "front" : "back");
    }

    static VideoCaptureFormat[] getDeviceSupportedFormats(Context appContext, int id) {
        final CameraCharacteristics cameraCharacteristics =
                getCameraCharacteristics(appContext, id);
        if (cameraCharacteristics == null) return null;

        final int[] capabilities =
                cameraCharacteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
        // Per-format frame rate via getOutputMinFrameDuration() is only available if the
        // property REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR is set.
        boolean minFrameDurationAvailable = false;
        for (int cap : capabilities) {
            if (cap == CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR) {
                minFrameDurationAvailable = true;
                break;
            }
        }

        ArrayList<VideoCaptureFormat> formatList = new ArrayList<VideoCaptureFormat>();
        final StreamConfigurationMap streamMap =
                cameraCharacteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        final int[] formats = streamMap.getOutputFormats();
        for (int format : formats) {
            final Size[] sizes = streamMap.getOutputSizes(format);
            if (sizes == null) continue;
            for (Size size : sizes) {
                double minFrameRate = 0.0f;
                if (minFrameDurationAvailable) {
                    final long minFrameDuration = streamMap.getOutputMinFrameDuration(format, size);
                    minFrameRate = (minFrameDuration == 0)
                            ? 0.0f
                            : (1.0 / kNanoSecondsToFps * minFrameDuration);
                } else {
                    // TODO(mcasas): find out where to get the info from in this case.
                    // Hint: perhaps using SCALER_AVAILABLE_PROCESSED_MIN_DURATIONS.
                    minFrameRate = 0.0;
                }
                formatList.add(new VideoCaptureFormat(
                        size.getWidth(), size.getHeight(), (int) minFrameRate, 0));
            }
        }
        return formatList.toArray(new VideoCaptureFormat[formatList.size()]);
    }

    VideoCaptureCamera2(Context context, int id, long nativeVideoCaptureDeviceAndroid) {
        super(context, id, nativeVideoCaptureDeviceAndroid);
        final CameraCharacteristics cameraCharacteristics = getCameraCharacteristics(context, id);
        mMaxZoom =
                cameraCharacteristics.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM);
    }

    @Override
    public boolean allocate(int width, int height, int frameRate) {
        Log.d(TAG, "allocate: requested (%d x %d) @%dfps", width, height, frameRate);
        synchronized (mCameraStateLock) {
            if (mCameraState == CameraState.OPENING || mCameraState == CameraState.CONFIGURING) {
                Log.e(TAG, "allocate() invoked while Camera is busy opening/configuring.");
                return false;
            }
        }
        final CameraCharacteristics cameraCharacteristics = getCameraCharacteristics(mContext, mId);
        final StreamConfigurationMap streamMap =
                cameraCharacteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);

        // Find closest supported size.
        final Size[] supportedSizes = streamMap.getOutputSizes(ImageFormat.YUV_420_888);
        final Size closestSupportedSize = findClosestSizeInArray(supportedSizes, width, height);
        if (closestSupportedSize == null) {
            Log.e(TAG, "No supported resolutions.");
            return false;
        }
        Log.d(TAG, "allocate: matched (%d x %d)", closestSupportedSize.getWidth(),
                closestSupportedSize.getHeight());

        // |mCaptureFormat| is also used to configure the ImageReader.
        mCaptureFormat = new VideoCaptureFormat(closestSupportedSize.getWidth(),
                closestSupportedSize.getHeight(), frameRate, ImageFormat.YUV_420_888);
        mCameraNativeOrientation =
                cameraCharacteristics.get(CameraCharacteristics.SENSOR_ORIENTATION);
        // TODO(mcasas): The following line is correct for N5 with prerelease Build,
        // but NOT for N7 with a dev Build. Figure out which one to support.
        mInvertDeviceOrientationReadings =
                cameraCharacteristics.get(CameraCharacteristics.LENS_FACING)
                == CameraCharacteristics.LENS_FACING_BACK;
        return true;
    }

    @Override
    public boolean startCapture() {
        Log.d(TAG, "startCapture");
        changeCameraStateAndNotify(CameraState.OPENING);
        final CameraManager manager =
                (CameraManager) mContext.getSystemService(Context.CAMERA_SERVICE);
        final Handler mainHandler = new Handler(mContext.getMainLooper());
        final CrStateListener stateListener = new CrStateListener();
        try {
            manager.openCamera(Integer.toString(mId), stateListener, mainHandler);
        } catch (CameraAccessException | IllegalArgumentException | SecurityException ex) {
            Log.e(TAG, "allocate: manager.openCamera: ", ex);
            return false;
        }

        return true;
    }

    @Override
    public boolean stopCapture() {
        Log.d(TAG, "stopCapture");

        // With Camera2 API, the capture is started asynchronously, which will cause problem if
        // stopCapture comes too quickly. Without stopping the previous capture properly, the next
        // startCapture will fail and make Chrome no-responding. So wait camera to be STARTED.
        synchronized (mCameraStateLock) {
            while (mCameraState != CameraState.STARTED && mCameraState != CameraState.STOPPED) {
                try {
                    mCameraStateLock.wait();
                } catch (InterruptedException ex) {
                    Log.e(TAG, "CaptureStartedEvent: ", ex);
                }
            }
            if (mCameraState == CameraState.STOPPED) return true;
        }

        try {
            mPreviewSession.abortCaptures();
        } catch (CameraAccessException | IllegalStateException ex) {
            Log.e(TAG, "abortCaptures: ", ex);
            return false;
        }
        if (mCameraDevice == null) return false;
        mCameraDevice.close();
        changeCameraStateAndNotify(CameraState.STOPPED);
        mCropRect = new Rect();
        return true;
    }

    public PhotoCapabilities getPhotoCapabilities() {
        final CameraCharacteristics cameraCharacteristics = getCameraCharacteristics(mContext, mId);

        int minIso = 0;
        int maxIso = 0;
        final Range<Integer> iso_range =
                cameraCharacteristics.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE);
        if (iso_range != null) {
            minIso = iso_range.getLower();
            maxIso = iso_range.getUpper();
        }
        final int currentIso = mPreviewRequest.get(CaptureRequest.SENSOR_SENSITIVITY);

        final StreamConfigurationMap streamMap =
                cameraCharacteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        final Size[] supportedSizes = streamMap.getOutputSizes(ImageFormat.JPEG);
        int minWidth = Integer.MAX_VALUE;
        int minHeight = Integer.MAX_VALUE;
        int maxWidth = 0;
        int maxHeight = 0;
        for (Size size : supportedSizes) {
            if (size.getWidth() < minWidth) minWidth = size.getWidth();
            if (size.getHeight() < minHeight) minHeight = size.getHeight();
            if (size.getWidth() > maxWidth) maxWidth = size.getWidth();
            if (size.getHeight() > maxHeight) maxHeight = size.getHeight();
        }
        final int currentHeight = mCaptureFormat.getHeight();
        final int currentWidth = mCaptureFormat.getWidth();

        // The Min and Max zoom are returned as x100 by the API to avoid using floating point. There
        // is no min-zoom per se, so clamp it to always 100 (TODO(mcasas): make const member).
        final int minZoom = 100;
        final int maxZoom = Math.round(mMaxZoom * 100);
        // Width Ratio x100 is used as measure of current zoom.
        final int currentZoom = 100 * mPreviewRequest.get(CaptureRequest.SCALER_CROP_REGION).width()
                / cameraCharacteristics.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)
                          .width();

        final int focusMode = mPreviewRequest.get(CaptureRequest.CONTROL_AF_MODE);
        Log.d(TAG, "focusMode " + focusMode);
        final boolean isFocusManual = (focusMode == CameraMetadata.CONTROL_AF_MODE_OFF)
                || (focusMode == CameraMetadata.CONTROL_AF_MODE_EDOF);

        return new PhotoCapabilities(minIso, maxIso, currentIso, maxHeight, minHeight,
                currentHeight, maxWidth, minWidth, currentWidth, maxZoom, minZoom, currentZoom,
                !isFocusManual);
    }

    @Override
    public void setZoom(int zoom) {
        final CameraCharacteristics cameraCharacteristics = getCameraCharacteristics(mContext, mId);
        final Rect canvas =
                cameraCharacteristics.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE);

        final float normalizedZoom = Math.max(100, Math.min(zoom, mMaxZoom * 100)) / 100;
        final float cropFactor = (normalizedZoom - 1) / (2 * normalizedZoom);

        mCropRect = new Rect(Math.round(canvas.width() * cropFactor),
                Math.round(canvas.height() * cropFactor),
                Math.round(canvas.width() * (1 - cropFactor)),
                Math.round(canvas.height() * (1 - cropFactor)));
        Log.d(TAG, "zoom level " + normalizedZoom + ", rectangle: " + mCropRect.toString());

        final Handler mainHandler = new Handler(mContext.getMainLooper());
        mainHandler.removeCallbacks(mRestartCapture);
        mainHandler.post(mRestartCapture);
    }

    @Override
    public boolean takePhoto(final long callbackId, int width, int height) {
        Log.d(TAG, "takePhoto " + callbackId);
        if (mCameraDevice == null || mCameraState != CameraState.STARTED) return false;

        final CameraCharacteristics cameraCharacteristics = getCameraCharacteristics(mContext, mId);
        final StreamConfigurationMap streamMap =
                cameraCharacteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        final Size[] supportedSizes = streamMap.getOutputSizes(ImageFormat.JPEG);
        final Size closestSize = findClosestSizeInArray(supportedSizes, width, height);

        Log.d(TAG, "requested resolution: (%dx%d)", width, height);
        if (closestSize != null) {
            Log.d(TAG, " matched (%dx%d)", closestSize.getWidth(), closestSize.getHeight());
        }
        final ImageReader imageReader = ImageReader.newInstance(
                (closestSize != null) ? closestSize.getWidth() : mCaptureFormat.getWidth(),
                (closestSize != null) ? closestSize.getHeight() : mCaptureFormat.getHeight(),
                ImageFormat.JPEG, 1 /* maxImages */);

        HandlerThread thread = new HandlerThread("CameraPicture");
        thread.start();
        final Handler backgroundHandler = new Handler(thread.getLooper());

        final CrPhotoReaderListener photoReaderListener = new CrPhotoReaderListener(callbackId);
        imageReader.setOnImageAvailableListener(photoReaderListener, backgroundHandler);

        final List<Surface> surfaceList = new ArrayList<Surface>(1);
        surfaceList.add(imageReader.getSurface());

        CaptureRequest.Builder photoRequestBuilder = null;
        try {
            photoRequestBuilder =
                    mCameraDevice.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE);
        } catch (CameraAccessException e) {
            Log.e(TAG, "mCameraDevice.createCaptureRequest() error");
            return false;
        }
        if (photoRequestBuilder == null) {
            Log.e(TAG, "photoRequestBuilder error");
            return false;
        }
        photoRequestBuilder.addTarget(imageReader.getSurface());
        photoRequestBuilder.set(CaptureRequest.JPEG_ORIENTATION, getCameraRotation());
        if (!mCropRect.isEmpty()) {
            photoRequestBuilder.set(CaptureRequest.SCALER_CROP_REGION, mCropRect);
        }

        final CaptureRequest photoRequest = photoRequestBuilder.build();
        final CrPhotoSessionListener sessionListener =
                new CrPhotoSessionListener(photoRequest, callbackId);
        try {
            mCameraDevice.createCaptureSession(surfaceList, sessionListener, backgroundHandler);
        } catch (CameraAccessException | IllegalArgumentException | SecurityException ex) {
            Log.e(TAG, "createCaptureSession: " + ex);
            return false;
        }
        return true;
    }

    @Override
    public void deallocate() {
        Log.d(TAG, "deallocate");
    }
}
