#define _XOPEN_SOURCE 500
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <alloca.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <libgen.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/limits.h>
#include <linux/videodev2.h>
#include <android/log.h>
#include <cutils/properties.h>

#ifdef USE_VAAPI
#include <va/va.h>
#include <va/va_android.h>
#endif

#define LOG_TAG "waydroid-init"
#define DMABUF_SYSTEM_HEAP "/dev/dma_heap/system"
#define VAAPI_FAKE_DISPLAY 0xdeada01d
#define WAYDROID_SETTINGS_FILE "/data/misc/waydroid_settings"

#ifndef V4L2_PIX_FMT_AV1_FRAME
#define V4L2_PIX_FMT_AV1_FRAME v4l2_fourcc('A', 'V', '1', 'F')
#endif

#define ALOGD(fmt, args...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, fmt, ##args)
#define ALOGI(fmt, args...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, fmt, ##args)
#define ALOGW(fmt, args...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, fmt, ##args)
#define ALOGE(fmt, args...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, fmt, ##args)

static inline bool codec_already_added(char* codecs, char *c) {
    for (int i = 0; i < PROPERTY_VALUE_MAX; i += 5) {
        if (codecs[i] == '\0') {
            return false;
        } else if (memcmp(codecs + i, c, 5) == 0) {
            return true;
        }
    }

    return false;
}

static inline void load_saved_settings() {
    FILE *config = fopen(WAYDROID_SETTINGS_FILE, "r");
    char buf[150], *delim;

    if (config == NULL) {
        if (errno == ENOENT) return;
        ALOGE("Failed to read settings file: %s", strerror(errno));
        exit(errno);
    }

    while (fgets(buf, sizeof(buf), config) != NULL) {
        if ((delim = strchr(buf, '=')) == NULL) continue;

        buf[strlen(buf) - 1] = '\0'; // Get rid of newline
        delim[0] = '\0';

        ALOGI("Setting %s=%s", buf, delim + 1);
        property_set(buf, delim + 1);
    }
}

#ifdef USE_VAAPI
static inline void get_hwcodec_list_vaapi(char *hwcodecs) {
    VADisplay dpy = vaGetDisplay((void *) &(int){VAAPI_FAKE_DISPLAY});
    VAProfile *profiles;
    VAEntrypoint *entrypoints;
    VAStatus status;

    bool is_encoder;
    char codec[6];
    int libva_version[2], num_profiles, num_entrypoints, num_entries = 0;

    hwcodecs[0] = '\0';

    if ((status = vaInitialize(dpy, &libva_version[0], &libva_version[1])) != VA_STATUS_SUCCESS) {
        ALOGE("vaInitialize() failed: %i", status);
        return;
    }

    ALOGI("VA-API version %i.%i", libva_version[0], libva_version[1]);

    profiles = alloca(vaMaxNumProfiles(dpy) * sizeof(VAProfile));
    entrypoints = alloca(vaMaxNumEntrypoints(dpy) * sizeof(VAEntrypoint));

    if ((status = vaQueryConfigProfiles(dpy, profiles, &num_profiles)) != VA_STATUS_SUCCESS) {
        ALOGW("vaQueryConfigProfiles() failed: %i", status);
        return;
    }

    ALOGI("Supported HW codec(s) by GPU:");

    for (int i = 0; i < num_profiles; i++) {
        if ((status = vaQueryConfigEntrypoints(dpy, profiles[i], entrypoints, &num_entrypoints)) != VA_STATUS_SUCCESS) {
            ALOGW("vaQueryConfigEntrypoints() failed: %i", status);
            return;
        }

        for (int j = 0; j < num_entrypoints; j++) {
            is_encoder = (entrypoints[j] == VAEntrypointEncSlice || entrypoints[j] == VAEntrypointEncSliceLP);

            if (!is_encoder && entrypoints[j] != VAEntrypointVLD) continue;

            switch (profiles[i]) {
                case VAProfileMPEG2Simple:
                case VAProfileMPEG2Main:
                    *(uint32_t *) codec = V4L2_PIX_FMT_MPEG2;
                    break;
                case VAProfileMPEG4Simple:
                case VAProfileMPEG4AdvancedSimple:
                case VAProfileMPEG4Main:
                    *(uint32_t *) codec = V4L2_PIX_FMT_MPEG4;
                    break;
                case VAProfileH263Baseline:
                    *(uint32_t *) codec = V4L2_PIX_FMT_H263;
                    break;
                case VAProfileH264Main:
                case VAProfileH264High:
                    *(uint32_t *) codec = V4L2_PIX_FMT_H264;
                    break;
                case VAProfileHEVCMain:
                    *(uint32_t *) codec = V4L2_PIX_FMT_HEVC;
                    break;
                case VAProfileVP8Version0_3:
                    *(uint32_t *) codec = V4L2_PIX_FMT_VP8;
                    break;
                case VAProfileVP9Profile0:
                case VAProfileVP9Profile1:
                case VAProfileVP9Profile2:
                case VAProfileVP9Profile3:
                    *(uint32_t *) codec = V4L2_PIX_FMT_VP9;
                    break;
                case VAProfileAV1Profile0:
                case VAProfileAV1Profile1:
                    *(uint32_t *) codec = v4l2_fourcc('A', 'V', '1', '0');
                    break;
                default:
                    continue;
            }

            codec[4] = is_encoder ? 'E' : 'D';
            codec[5] = '\0';

            if (!codec_already_added(hwcodecs, codec)) {
                if (++num_entries > PROPERTY_VALUE_MAX / 5) {
                    vaTerminate(dpy);
                    return;
                }

                ALOGI("%s [%s]", codec, is_encoder ? "Encode" : "Decode");
                strcat(hwcodecs, codec);
            }
        }
    }

    vaTerminate(dpy);
}
#else
static inline void get_hwcodec_list_v4l2(char *hwcodecs) {
    char v4l2_decoder[PATH_MAX], codec[6];
    struct v4l2_capability cap;
    struct v4l2_fmtdesc fmt;
    int num_entries = 0, fd, ret;

    hwcodecs[0] = '\0';

    for (int i = 0; i < 64; i++) {
        fmt.index = 0;

        snprintf(v4l2_decoder, PATH_MAX, "/dev/video%i", i);

        if (access(v4l2_decoder, F_OK) != 0) break;

        ALOGI("Open V4L2 device %s", v4l2_decoder);

        if ((fd = open(v4l2_decoder, O_RDONLY)) == -1) {
            ALOGE("Failed to open V4L2 device %s: %s", v4l2_decoder, strerror(errno));
            continue;
        }

        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
            ALOGE("ioctl(VIDIOC_QUERYCAP) failed for %s: %s", v4l2_decoder, strerror(errno));
            close(fd);
            continue;
        }

        if (!(cap.capabilities & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE))) {
            close(fd);
            continue;
        }

        ALOGI("V4L2 M2M device found");
        ALOGI("Supported HW codec(s) by GPU:");

        // Query for supported codecs
        for (int encoder = 0; encoder < 2; encoder++) {
            fmt.index = 0;
            fmt.type = encoder ? (cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE) :
                                 (cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT);

            while (true) {
                ret = ioctl(fd, VIDIOC_ENUM_FMT, &fmt);
                fmt.index++;

                if (ret == -1 && errno == EINVAL) {
                    break;
                } else if (ret == -1) {
                    ALOGW("ioctl(VIDIOC_ENUM_FMT) failed for %s: %s", v4l2_decoder, strerror(errno));
                    continue;
                }

                switch (fmt.pixelformat) {
                    case V4L2_PIX_FMT_MPEG2:
                    case V4L2_PIX_FMT_MPEG2_SLICE:
                    case V4L2_PIX_FMT_MPEG4:
                    case V4L2_PIX_FMT_H263:
                    case V4L2_PIX_FMT_H264:
                    case V4L2_PIX_FMT_H264_SLICE:
                    case V4L2_PIX_FMT_HEVC:
                    case V4L2_PIX_FMT_HEVC_SLICE:
                    case V4L2_PIX_FMT_VP8:
                    case V4L2_PIX_FMT_VP8_FRAME:
                    case V4L2_PIX_FMT_VP9:
                    case V4L2_PIX_FMT_VP9_FRAME:
                    case V4L2_PIX_FMT_AV1_FRAME:
                        break;
                    default:
                        continue;
                }

                *(uint32_t *) codec = fmt.pixelformat;
                codec[4] = encoder ? 'E' : 'D';
                codec[5] = '\0';

                if (!codec_already_added(hwcodecs, codec)) {
                    if (++num_entries > PROPERTY_VALUE_MAX / 5) {
                        close(fd);
                        return;
                    }

                    ALOGI("%s [%s]", codec, encoder ? "Encode" : "Decode");
                    strcat(hwcodecs, codec);
                }
            }
        }

        close(fd);
    }
}
#endif

int get_gpu_kernel_driver_name(const char *render_node, char *driver_name, size_t driver_name_maxlen) {
    char driver_symlink_path[PATH_MAX], driver_path[PATH_MAX] = {'\0'};
    int len;
    struct stat node_info;

    if (stat(render_node, &node_info) != 0) {
        ALOGE("Failed to access GPU render node: %s", strerror(errno));
        return -errno;
    }

    if (!S_ISCHR(node_info.st_mode)) {
        ALOGE("GPU render node is not a character device");
        return -1;
    }

    snprintf(driver_symlink_path, PATH_MAX, "/sys/dev/char/%i:%i/device/driver", major(node_info.st_rdev), minor(node_info.st_rdev));

    len = readlink(driver_symlink_path, driver_path, PATH_MAX - 1);

    if (len == -1) {
        ALOGE("Failed to read GPU driver name: %s", strerror(errno));
        return -errno;
    }

    strncpy(driver_name, basename(driver_path), driver_name_maxlen);
    return 0;
}

int get_intel_gpu_generation(const char *render_node) {
    char buf[100], capabilities_path[PATH_MAX];
    int generation = -1;
    struct stat render_node_info;
    FILE *capabilities;

    if (stat(render_node, &render_node_info) != 0) {
        ALOGE("Failed to access GPU render node: %s", strerror(errno));
        return -errno;
    }

    if (!S_ISCHR(render_node_info.st_mode)) {
        ALOGE("GPU render node is not a character device");
        return -1;
    }

    snprintf(capabilities_path, sizeof(capabilities_path), "/sys/kernel/debug/dri/%i/i915_capabilities", minor(render_node_info.st_rdev));
    capabilities = fopen(capabilities_path, "r");

    if (capabilities == NULL) {
        ALOGE("Failed to read GPU capabilities: %s", strerror(errno));
        return -errno;
    }

    while (fgets(buf, sizeof(buf), capabilities) != NULL) {
        if (strncmp(buf, "graphics version: ", 18) == 0) {
            generation = (int) atof(buf + 18);
            break;
        }
    }

    ALOGI("Intel GPU detected (Gen %i)", generation);
    return generation;
}

int main(int argc, char **argv) {
    const bool override_gralloc = property_get_bool("ro.gralloc.override", true);

    char gralloc_cmdline[100],
         gralloc_impl[PROPERTY_VALUE_MAX],
         render_node[PROPERTY_VALUE_MAX],
         hwcodecs[PROPERTY_VALUE_MAX],
         gpu_driver_name[20];

    int ret;

#ifdef USE_VAAPI
    get_hwcodec_list_vaapi(hwcodecs);
#else
    get_hwcodec_list_v4l2(hwcodecs);
#endif

    property_set("waydroid.init.start", "1");

    property_get("gralloc.gbm.device", render_node, "/dev/dri/renderD128");
    ALOGI("Using GPU device %s", render_node);

    if (access(render_node, F_OK) != 0) {
        ALOGE("GPU device %s does not exist!", render_node);
        return 1;
    }

    ret = get_gpu_kernel_driver_name(render_node, gpu_driver_name, sizeof(gpu_driver_name));
    if (ret != 0) return ret;

    ALOGI("GPU kernel driver: %s", gpu_driver_name);

    if (strcmp(gpu_driver_name, "amdgpu") == 0) {
        property_set("ro.hardware.vulkan", "radeon");
    } else if (strcmp(gpu_driver_name, "i915") == 0 || strcmp(gpu_driver_name, "xe") == 0) {
        property_set("ro.hardware.vulkan", get_intel_gpu_generation(render_node) < 9 ? "intel_hasvk" : "intel");
    } else if (strcmp(gpu_driver_name, "nouveau") == 0) {
        property_set("ro.hardware.vulkan", "nouveau");
    } else if (strncmp(gpu_driver_name, "virtio", 6) == 0) {
        property_set("ro.hardware.vulkan", "virtio");
    }

    if (override_gralloc) {
        if (strncmp(gpu_driver_name, "amdgpu", 6) == 0 ||
            strncmp(gpu_driver_name, "virtio", 6) == 0 ||
            strncmp(gpu_driver_name, "vmwgfx", 6) == 0) {

            strcpy(gralloc_impl, "minigbm");
        } else if (strcmp(gpu_driver_name, "i915") == 0 || strcmp(gpu_driver_name, "xe") == 0) {
            // Only use minigbm_celadon for Gen 9+ (Skylake) GPUs
            strcpy(gralloc_impl, get_intel_gpu_generation(render_node) < 9 ? "minigbm" : "minigbm_celadon");
        } else {
            strcpy(gralloc_impl, "minigbm_gbm_mesa");
        }
    } else {
        property_get("ro.hardware.gralloc", gralloc_impl, "minigbm_gbm_mesa");
    }

    ALOGI("Using gralloc implementation: %s", gralloc_impl);
    property_set("ro.hardware.gralloc", gralloc_impl);

    property_set("ro.waydroid.codec2-impl", strcmp(gralloc_impl, "minigbm_celadon") == 0 ? "c2.intel" : "c2.ffmpeg");
    property_set("ro.waydroid.hwcodecs", hwcodecs);

    ALOGI("Loading settings from %s", WAYDROID_SETTINGS_FILE);
    load_saved_settings();

    // Update gralloc value in case waydroid_settings changed it
    property_get("ro.hardware.gralloc", gralloc_impl, "minigbm_gbm_mesa");

    if (strcmp(gralloc_impl, "minigbm") == 0) {
        snprintf(gralloc_cmdline, sizeof(gralloc_cmdline), "start vendor.graphics.allocator-4-0");
        system("stop vendor.gralloc-2-0");
        system(gralloc_cmdline);
    } else if (strncmp(gralloc_impl, "minigbm_", 8) == 0) {
        snprintf(gralloc_cmdline, sizeof(gralloc_cmdline), "start vendor.graphics.allocator-4-0-%s", gralloc_impl + 8);
        system("stop vendor.gralloc-2-0");
        system(gralloc_cmdline);
    }

    // gbm/minigbm_vmwgfx/minigbm_gbm_mesa does not support YUV
    if (strncmp(gpu_driver_name, "vmwgfx", 6) && (strcmp(gralloc_impl, "minigbm") == 0 || strcmp(gralloc_impl, "minigbm_celadon") == 0)) {
        property_set("debug.ffmpeg-codec2.pixel_format", "YUV_420");
    } else if (strncmp(gpu_driver_name, "vmwgfx", 6) == 0) {
        // Disable VA-API for vmwgfx
        property_set("media.sf.hwaccel", "0");
        property_set("debug.ffmpeg-codec2.hwaccel.drm", "0");
        property_set("debug.ffmpeg-codec2.pixel_format", "RGB_565");
    } else {
        property_set("debug.ffmpeg-codec2.pixel_format", "RGBX_8888");
    }

    if (access(DMABUF_SYSTEM_HEAP, F_OK) != 0) {
        ALOGE("DMA-BUF system heap does not exist, video playback might not work properly");
        ALOGE("Falling back to gralloc");
        property_set("debug.stagefright.c2-poolmask", "0xfc0000");
    }

    property_set("waydroid.init.done", "1");
    return 0;
}
