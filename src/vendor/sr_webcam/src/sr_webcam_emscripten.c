// Emscripten / browser backend for sr_webcam.
// Uses MediaStream + OffscreenCanvas + requestAnimationFrame to deliver
// RGBA frames through the standard sr_webcam callback.
//
// JS side: the caller (WebChuGL) must populate Module.__webcamStreams[deviceId]
// with a MediaStream before sr_webcam_open is invoked. Typically triggered
// via ck.requestWebcam(deviceId) from a user-gesture context.

#include "sr_webcam_internal.h"

#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ---- JS helpers ---------------------------------------------------------

// Open a device: look up the pre-acquired stream, attach a hidden <video>
// and OffscreenCanvas, stash in Module.__webcamDevices[deviceId]. Returns
// 0 on success, -1 if no stream has been pre-acquired. Fills outWidth /
// outHeight with the actual negotiated dimensions.
EM_JS(int, _sr_webcam_em_open,
      (int deviceId, int reqW, int reqH, int outWidth, int outHeight), {
    var streams = (Module.__webcamStreams = Module.__webcamStreams || {});
    var stream  = streams[deviceId];
    if (!stream) {
        console.error('[sr_webcam] no pre-acquired stream for deviceId=' +
                      deviceId + '; call ck.requestWebcam() first');
        return -1;
    }
    var devices = (Module.__webcamDevices = Module.__webcamDevices || {});
    var d = devices[deviceId];
    if (!d) {
        var video       = document.createElement('video');
        // Off-screen instead of display:none — Chromium pauses decode on display:none.
        video.style.position = 'absolute';
        video.style.left  = '-9999px';
        video.style.top   = '-9999px';
        video.style.width = '1px';
        video.style.height = '1px';
        video.srcObject = stream;
        video.muted     = true;
        video.playsInline = true;
        document.body.appendChild(video);
        // autoplay is unreliable for JS-created videos; call play() explicitly.
        video.play().catch(function (e) {
            console.error('[sr_webcam] video.play() failed:', e && e.message);
        });

        var track    = stream.getVideoTracks()[0];
        var settings = track ? track.getSettings() : {};
        var w = settings.width  || reqW;
        var h = settings.height || reqH;
        var canvas = (typeof OffscreenCanvas !== 'undefined')
                     ? new OffscreenCanvas(w, h)
                     : (function () {
                           var c = document.createElement('canvas');
                           c.width = w; c.height = h;
                           c.style.display = 'none';
                           document.body.appendChild(c);
                           return c;
                       })();
        var ctx = canvas.getContext('2d');
        // Flip Y so drawImage produces bottom-origin pixels (matches
        // ChuGL's desktop webcam backends and its UV layout).
        ctx.translate(0, h);
        ctx.scale(1, -1);

        d = {
            video:  video,
            canvas: canvas,
            ctx:    ctx,
            width:  w,
            height: h,
            running:false,
            rafId:  0,
        };
        devices[deviceId] = d;
    }

    HEAP32[(outWidth  >> 2)] = d.width;
    HEAP32[(outHeight >> 2)] = d.height;
    return 0;
});

// Start the per-frame grab loop: drawImage(video) → getImageData → copy
// to the wasm buffer → invoke the C callback(device, data).
EM_JS(void, _sr_webcam_em_start,
      (int deviceId, int bufPtr, int callbackPtr, int devicePtr), {
    var devices = Module.__webcamDevices || {};
    var d = devices[deviceId];
    if (!d || d.running) return;
    d.running = true;

    var loop = function () {
        if (!d.running) return;
        // readyState >= 2 means HAVE_CURRENT_DATA.
        if (d.video.readyState >= 2) {
            d.ctx.drawImage(d.video, 0, 0, d.width, d.height);
            var img = d.ctx.getImageData(0, 0, d.width, d.height);
            HEAPU8.set(img.data, bufPtr);
            var fn = Module.wasmTable.get(callbackPtr);
            if (fn) fn(devicePtr, bufPtr);
        }
        d.rafId = requestAnimationFrame(loop);
    };
    d.rafId = requestAnimationFrame(loop);
});

// Stop the grab loop but keep the video + canvas around so start can resume.
EM_JS(void, _sr_webcam_em_stop, (int deviceId), {
    var devices = Module.__webcamDevices || {};
    var d = devices[deviceId];
    if (!d) return;
    d.running = false;
    if (d.rafId) { cancelAnimationFrame(d.rafId); d.rafId = 0; }
});

// Full teardown: stop loop, detach video, release MediaStream tracks,
// delete the device entry.
EM_JS(void, _sr_webcam_em_delete, (int deviceId), {
    var devices = Module.__webcamDevices || {};
    var d = devices[deviceId];
    if (!d) return;
    if (d.rafId) cancelAnimationFrame(d.rafId);
    if (d.video) {
        try { d.video.pause(); } catch (e) {}
        if (d.video.parentElement) d.video.parentElement.removeChild(d.video);
    }
    var streams = Module.__webcamStreams || {};
    var s = streams[deviceId];
    if (s) {
        s.getTracks().forEach(function (t) { t.stop(); });
        delete streams[deviceId];
    }
    delete devices[deviceId];
});

// ---- sr_webcam C API ----------------------------------------------------

int sr_webcam_open(sr_webcam_device* device)
{
    int outW = 0, outH = 0;
    int rc = _sr_webcam_em_open(device->deviceId, device->width, device->height,
                                (int)(intptr_t)&outW, (int)(intptr_t)&outH);
    if (rc != 0) return -1;

    // Update the device's dimensions to whatever the browser negotiated.
    // ChuGL's render code asserts the texture size matches these values.
    device->width  = outW;
    device->height = outH;
    strncpy(device->user_friendly_name, "browser-webcam",
            sizeof(device->user_friendly_name) - 1);
    device->user_friendly_name[sizeof(device->user_friendly_name) - 1] = '\0';
    return 0;
}

void sr_webcam_start(sr_webcam_device* device)
{
    if (device->running) return;
    if (!device->stream) {
        size_t size = (size_t)device->width * (size_t)device->height * 4;
        device->stream = malloc(size);
        if (!device->stream) return;
    }
    device->running = 1;
    _sr_webcam_em_start(device->deviceId,
                        (int)(intptr_t)device->stream,
                        (int)(intptr_t)device->callback,
                        (int)(intptr_t)device);
}

void sr_webcam_stop(sr_webcam_device* device)
{
    device->running = 0;
    _sr_webcam_em_stop(device->deviceId);
}

void sr_webcam_delete(sr_webcam_device* device)
{
    _sr_webcam_em_stop(device->deviceId);
    _sr_webcam_em_delete(device->deviceId);
    if (device->stream) { free(device->stream); device->stream = NULL; }
    free(device);
}
