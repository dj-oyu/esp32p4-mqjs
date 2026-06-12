/* Tab5 camera barcode scanner — see include/cam_tab5.h.
 *
 * Bring-up mirrors M5Tab5-UserDemo: SC2356 (SC202CS driver) on MIPI-CSI,
 * 24MHz XCLK from LEDC on GPIO36, SCCB on the shared internal I2C bus
 * (the touch controller's port-1 handle), CAM_RST already released high
 * by ui_tab5's PI4IOE@0x43 init (P6). esp_video exposes /dev/video0;
 * frames are ISP-processed RGB565 1280x720, sampled as luma scanlines
 * for the EAN-13 decoder (24 rows + 16 columns per frame).
 *
 * No serial on the Tab5 in the field: every failure path lands in
 * s_status, surfaced to JS via camera.status().
 */
#include "sdkconfig.h"
#include "cam_tab5.h"

#include <stdio.h>

#if CONFIG_MQJS_CAMERA

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/ppa.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

#include "ean13.h"
#include "ui_tab5.h"

static const char *TAG = "cam_tab5";

#define CAM_XCLK_GPIO   36
#define CAM_XCLK_HZ     24000000
/* full sensor resolution (SC2356 UXGA): +25% pixels per barcode module
 * vs 720p — the fixed-focus lens limits how close the book can get, so
 * resolution is the lever we have. Needs the 1600X1200 format selected
 * in the sensor kconfig (sdkconfig.tab5.defaults). */
#define CAM_W           1600
#define CAM_H           1200
#define CAM_BUFS        2

static i2c_master_bus_handle_t s_bus;
static bool s_video_ready;
static volatile bool s_busy;
static volatile bool s_cancel;
static char s_status[96] = "idle";

static struct {
    cam_tab5_cb_t cb;
    void *arg;
    uint32_t timeout_ms;
    char prefix[8];
} s_req;

static void set_status(const char *fmt, const char *detail)
{
    snprintf(s_status, sizeof s_status, fmt, detail ? detail : "");
    ESP_LOGI(TAG, "%s", s_status);
}

const char *cam_tab5_status(void)
{
    return s_status;
}

void cam_tab5_set_i2c(void *i2c_master_bus_handle)
{
    s_bus = (i2c_master_bus_handle_t)i2c_master_bus_handle;
}

static bool xclk_once(void)
{
    static bool done;
    if (done)
        return true;
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num = LEDC_TIMER_2, /* backlight owns TIMER_0/CH_1 */
        .freq_hz = CAM_XCLK_HZ,
        /* AUTO may pick the 40MHz XTAL, whose minimum divider (1.0)
           cannot reach 24MHz at 1-bit resolution — pin the 80MHz PLL */
        .clk_cfg = LEDC_USE_PLL_DIV_CLK,
    };
    if (ledc_timer_config(&t) != ESP_OK) {
        set_status("xclk timer config failed%s", NULL);
        return false;
    }
    ledc_channel_config_t c = {
        .gpio_num = CAM_XCLK_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_6,
        .timer_sel = LEDC_TIMER_2,
        .duty = 1, /* 50% of a 1-bit period */
        .hpoint = 0,
    };
    if (ledc_channel_config(&c) != ESP_OK) {
        set_status("xclk channel config failed%s", NULL);
        return false;
    }
    done = true;
    return true;
}

static bool video_init_once(void)
{
    if (s_video_ready)
        return true;
    if (!s_bus) {
        set_status("no i2c bus (ui up?)%s", NULL);
        return false;
    }
    if (!xclk_once())
        return false;

    esp_video_init_csi_config_t csi = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = s_bus,
            .freq = 400000,
        },
        .reset_pin = -1, /* CAM_RST = PI4IOE@0x43 P6, released at UI boot */
        .pwdn_pin = -1,
    };
    esp_video_init_config_t cfg = { .csi = &csi };
    esp_err_t err = esp_video_init(&cfg);
    if (err != ESP_OK) {
        /* the DSI panel may already own the MIPI PHY LDO channel */
        csi.dont_init_ldo = true;
        err = esp_video_init(&cfg);
    }
    if (err != ESP_OK) {
        set_status("esp_video_init: %s", esp_err_to_name(err));
        return false;
    }
    s_video_ready = true;
    set_status("video ready%s", NULL);
    return true;
}

static inline uint8_t luma565(uint16_t v)
{
    int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return (uint8_t)((77 * (r << 3) + 150 * (g << 2) + 29 * (b << 3)) >> 8);
}

static bool prefix_ok(const char code[14])
{
    size_t n = strlen(s_req.prefix);
    return n == 0 || strncmp(code, s_req.prefix, n) == 0;
}

/* sample a grid of scanlines; true when a (prefix-matching) code hit */
static bool scan_frame(const uint16_t *px, int w, int h, uint8_t *line,
                       char out[14])
{
    for (int r = 0; r < 32; r++) {
        int y = h * (15 + r * 70 / 31) / 100; /* rows across 15%..85% */
        const uint16_t *row = px + y * w;
        for (int x = 0; x < w; x++)
            line[x] = luma565(row[x]);
        if (ean13_decode_gray_line(line, w, out) && prefix_ok(out))
            return true;
    }
    for (int c = 0; c < 24; c++) { /* rotated 90°: columns 10%..90% */
        int x = w * (10 + c * 80 / 23) / 100;
        for (int y = 0; y < h; y++)
            line[y] = luma565(px[y * w + x]);
        if (ean13_decode_gray_line(line, h, out) && prefix_ok(out))
            return true;
    }
    return false;
}

/* ---- persistent capture pipeline ----
   M5Tab5-UserDemo never closes the camera (hal_camera.cpp keeps fd +
   buffers + STREAMON forever and even comments out close()) — and
   indeed re-running REQBUFS/STREAMON on esp_video after a teardown
   fails ("STREAMON failed" on the second scan). So: bring the
   pipeline up once on first use and keep it streaming; scans just
   DQBUF/QBUF for their window. */
static int s_fd = -1;
static void *s_bufs[CAM_BUFS];
static int s_frame_w, s_frame_h;
static ppa_client_handle_t s_ppa; /* hardware rotate+scale for preview */

static bool pipeline_once(void)
{
    if (s_fd >= 0)
        return true;

    const char *fail = NULL;
    int fd = -1;
    do {
        fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
        if (fd < 0) {
            fail = "open /dev/video0 failed";
            break;
        }
        struct v4l2_format fmt = { 0 };
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = CAM_W;
        fmt.fmt.pix.height = CAM_H;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
            fail = "S_FMT failed";
            break;
        }
        s_frame_w = (int)fmt.fmt.pix.width;
        s_frame_h = (int)fmt.fmt.pix.height;
        if (s_frame_w > CAM_W) {
            fail = "unexpected frame width";
            break;
        }

        struct v4l2_requestbuffers req = { 0 };
        req.count = CAM_BUFS;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
            fail = "REQBUFS failed";
            break;
        }
        for (int i = 0; i < CAM_BUFS && !fail; i++) {
            struct v4l2_buffer buf = { 0 };
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = (uint32_t)i;
            if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
                fail = "QUERYBUF failed";
                break;
            }
            s_bufs[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, buf.m.offset);
            if (!s_bufs[i] || s_bufs[i] == MAP_FAILED) {
                s_bufs[i] = NULL;
                fail = "mmap failed";
                break;
            }
            if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
                fail = "QBUF failed";
                break;
            }
        }
        if (fail)
            break;

        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
            fail = "STREAMON failed";
            break;
        }
    } while (0);

    if (fail) {
        set_status("%s", fail);
        if (fd >= 0)
            close(fd);
        for (int i = 0; i < CAM_BUFS; i++)
            s_bufs[i] = NULL;
        return false;
    }
    s_fd = fd;
    char dims[20];
    snprintf(dims, sizeof dims, "%dx%d", s_frame_w, s_frame_h);
    set_status("video ready %s", dims);
    return true;
}

/* Viewfinder blit 1280x720 -> portrait 360x640 via the PPA (the sensor
 * is mounted 90° relative to the portrait panel; mirror keeps the
 * selfie-natural aiming). A CPU transpose here was ~1 cache miss per
 * pixel on PSRAM and dragged the preview to a slideshow — the PPA does
 * rotate+scale+mirror in hardware in a few ms. */
#define PV_W (CAM_H / 2) /* 360 */
#define PV_H (CAM_W / 2) /* 640 */
static bool preview_blit(const uint16_t *px, uint16_t *dst) /* PPA SRM */
{
    if (!s_ppa) {
        ppa_client_config_t cfg = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        if (ppa_register_client(&cfg, &s_ppa) != ESP_OK)
            return false;
    }
    ppa_srm_oper_config_t srm = {
        .in = {
            .buffer = (void *)px,
            .pic_w = CAM_W,
            .pic_h = CAM_H,
            .block_w = CAM_W,
            .block_h = CAM_H,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = dst,
            .buffer_size = (uint32_t)PV_W * PV_H * 2,
            .pic_w = PV_W,
            .pic_h = PV_H,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        /* 270 + mirror == the transpose the user approved; 90 showed
           the world upside down (PPA's rotation sense vs our math) */
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
        .scale_x = 0.5,
        .scale_y = 0.5,
        .mirror_x = true,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_ppa, &srm) == ESP_OK;
}

static void scan_task(void *pv)
{
    static uint8_t line[CAM_W]; /* one scanner task at a time (s_busy) */
    char code[14];
    bool found = false;
    const char *fail = NULL;
    bool pipe_ok = pipeline_once(); /* on failure it set s_status */

    if (pipe_ok) {
        uint16_t *preview = ui_tab5_cam_canvas(PV_W, PV_H);
        set_status("scanning%s", NULL);

        int64_t deadline =
            esp_timer_get_time() + (int64_t)s_req.timeout_ms * 1000;
        while (!s_cancel && esp_timer_get_time() < deadline && !found) {
            struct v4l2_buffer buf = { 0 };
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) {
                fail = "DQBUF failed";
                break;
            }
            const uint16_t *px = (const uint16_t *)s_bufs[buf.index];
            if (preview && s_frame_w == CAM_W &&
                preview_blit(px, preview))
                ui_tab5_cam_canvas_update();
            found = scan_frame(px, s_frame_w, s_frame_h, line, code);
            ioctl(s_fd, VIDIOC_QBUF, &buf);
        }
        ui_tab5_cam_canvas_hide();
        /* the pipeline stays up and streaming (see pipeline_once) */
    }

    if (found)
        set_status("found %s", code);
    else if (fail)
        set_status("%s", fail);
    else if (pipe_ok)
        set_status(s_cancel ? "cancelled%s" : "timeout (no code)%s", NULL);

    cam_tab5_cb_t cb = s_req.cb;
    void *arg = s_req.arg;
    s_req.cb = NULL;
    s_busy = false;
    if (cb)
        cb(found ? code : NULL, arg);
    vTaskDelete(NULL);
}

bool cam_tab5_scan_start(uint32_t timeout_ms, const char *prefix,
                         cam_tab5_cb_t cb, void *arg)
{
    if (s_busy) {
        set_status("busy%s", NULL);
        return false;
    }
    if (!video_init_once())
        return false;
    s_req.cb = cb;
    s_req.arg = arg;
    s_req.timeout_ms = timeout_ms ? timeout_ms : 15000;
    snprintf(s_req.prefix, sizeof s_req.prefix, "%s", prefix ? prefix : "");
    s_cancel = false;
    s_busy = true;
    if (xTaskCreate(scan_task, "cam_scan", 16384, NULL, 4, NULL) != pdPASS) {
        s_busy = false;
        s_req.cb = NULL;
        set_status("task create failed%s", NULL);
        return false;
    }
    return true;
}

void cam_tab5_cancel(void)
{
    s_cancel = true;
}

#else /* !CONFIG_MQJS_CAMERA: Stamp / PC stubs */

void cam_tab5_set_i2c(void *i2c_master_bus_handle)
{
    (void)i2c_master_bus_handle;
}

bool cam_tab5_scan_start(uint32_t timeout_ms, const char *prefix,
                         cam_tab5_cb_t cb, void *arg)
{
    (void)timeout_ms;
    (void)prefix;
    (void)cb;
    (void)arg;
    return false;
}

void cam_tab5_cancel(void)
{
}

const char *cam_tab5_status(void)
{
    return "no camera in this build";
}

#endif /* CONFIG_MQJS_CAMERA */
