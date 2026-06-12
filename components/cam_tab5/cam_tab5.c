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
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

#include "ean13.h"

static const char *TAG = "cam_tab5";

#define CAM_XCLK_GPIO   36
#define CAM_XCLK_HZ     24000000
#define CAM_W           1280
#define CAM_H           720
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
    for (int r = 0; r < 24; r++) {
        int y = h * (20 + r * 60 / 23) / 100; /* rows across 20%..80% */
        const uint16_t *row = px + y * w;
        for (int x = 0; x < w; x++)
            line[x] = luma565(row[x]);
        if (ean13_decode_gray_line(line, w, out) && prefix_ok(out))
            return true;
    }
    for (int c = 0; c < 16; c++) { /* rotated 90°: columns 15%..85% */
        int x = w * (15 + c * 70 / 15) / 100;
        for (int y = 0; y < h; y++)
            line[y] = luma565(px[y * w + x]);
        if (ean13_decode_gray_line(line, h, out) && prefix_ok(out))
            return true;
    }
    return false;
}

static void scan_task(void *pv)
{
    static uint8_t line[CAM_W]; /* one scanner task at a time (s_busy) */
    char code[14];
    void *bufs[CAM_BUFS] = { 0 };
    size_t lens[CAM_BUFS] = { 0 };
    int fd = -1;
    bool found = false;
    bool streaming = false;
    const char *fail = NULL;

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
        int w = (int)fmt.fmt.pix.width, h = (int)fmt.fmt.pix.height;
        if (w > CAM_W) {
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
            bufs[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, buf.m.offset);
            lens[i] = buf.length;
            if (!bufs[i] || bufs[i] == MAP_FAILED) {
                bufs[i] = NULL;
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
        streaming = true;
        set_status("scanning%s", NULL);

        int64_t deadline =
            esp_timer_get_time() + (int64_t)s_req.timeout_ms * 1000;
        while (!s_cancel && esp_timer_get_time() < deadline && !found) {
            struct v4l2_buffer buf = { 0 };
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            if (ioctl(fd, VIDIOC_DQBUF, &buf) != 0) {
                fail = "DQBUF failed";
                break;
            }
            found = scan_frame((const uint16_t *)bufs[buf.index], w, h,
                               line, code);
            ioctl(fd, VIDIOC_QBUF, &buf);
        }
    } while (0);

    if (streaming) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd, VIDIOC_STREAMOFF, &type);
    }
    for (int i = 0; i < CAM_BUFS; i++)
        if (bufs[i])
            munmap(bufs[i], lens[i]);
    if (fd >= 0)
        close(fd);

    if (found)
        set_status("found %s", code);
    else if (fail)
        set_status("%s", fail);
    else
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
    if (xTaskCreate(scan_task, "cam_scan", 12288, NULL, 4, NULL) != pdPASS) {
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
