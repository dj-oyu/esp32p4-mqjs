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
#include <errno.h>
#include <math.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"   /* VIDIOC_S_DQBUF_TIMEOUT (bounded DQBUF) */
#include "hal/isp_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "linux/videodev2.h"

#include "bc_locate.h"
#include "ean13.h"
#include "qr_marker.h"
#include "quirc.h"
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
static char s_status[256] = "idle";

typedef enum {
    SCAN_EAN13,
    SCAN_QR,
} scan_mode_t;

/* One scan request, posted to the resident owner task's command queue. Cancel
 * is a separate atomic flag (s_cancel), NOT a queued command (see §7 of
 * docs/camera-lifecycle-plan.md): teardown must run on the owner, so cancel
 * only requests a stop. */
typedef struct {
    cam_tab5_cb_t cb;
    void *arg;
    uint32_t timeout_ms;
    char prefix[8];
    scan_mode_t mode;
} cam_scan_req_t;

static cam_scan_req_t s_req;   /* current scan, published by the owner task */

/* network-exclusion hooks (cam_tab5_set_net_hooks): suspend()/resume() the
 * heavy Tailscale/microlink traffic that starves the camera. */
static cam_tab5_net_hook_t s_net_suspend, s_net_resume;

void cam_tab5_set_net_hooks(cam_tab5_net_hook_t suspend_cb,
                            cam_tab5_net_hook_t resume_cb)
{
    s_net_suspend = suspend_cb;
    s_net_resume = resume_cb;
}

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

/* what the viewfinder overlay should show for one frame: the decoded
 * code (ANY EAN-13, prefix ignored on purpose — the user wants to see
 * everything the camera reads) or the best near-miss, with the
 * candidate span as ENDPOINTS in frame px (scanlines can run at any
 * angle now). kind: 0 none, 1 near-miss, 2 decoded. */
typedef struct {
    int kind;
    char code[14];
    int digits;
    int theta;          /* scanline angle (deg) or -1 for grid lines */
    int ax, ay, bx, by; /* candidate span endpoints, frame px */
} FrameHit;

#define HIT_MIN_DIGITS 4 /* below this, "near-misses" are just noise */

static void hit_record(FrameHit *disp, const ean13_scan_t *st, int theta,
                       int ax, int ay, int bx, int by)
{
    if (st->found) {
        if (disp->kind == 2)
            return; /* keep the first decode of the frame */
        disp->kind = 2;
        memcpy(disp->code, st->code, sizeof disp->code);
        disp->digits = 13;
    } else {
        if (disp->kind == 2 || st->digits < HIT_MIN_DIGITS ||
            st->digits <= disp->digits)
            return;
        disp->kind = 1;
        disp->digits = st->digits;
    }
    disp->theta = theta;
    disp->ax = ax;
    disp->ay = ay;
    disp->bx = bx;
    disp->by = by;
}

/* One scanline at (theta, off) through the region center: sample,
 * decode, record overlay info. True on a prefix-matching decode
 * (copied into out); st is left filled for the caller's heuristics.
 * stg = staged luma rect (S2) or NULL; the staged sampler is bit-
 * identical and falls back to the direct frame walk if the line ever
 * leaves the stage (host-tested equivalence, bc_locate_test.c). */
static bool scan_one_line(const uint16_t *px, int w, int h,
                          const bc_region_t *rg, const bc_stage_t *stg,
                          int theta, int off, int half, uint8_t *line,
                          char out[14], FrameHit *disp, ean13_scan_t *st)
{
    float th = theta * (float)M_PI / 180.0f;
    float ux = cosf(th), uy = sinf(th);
    float vx = -uy, vy = ux;
    int n = stg ? bc_sample_line_l8(stg, rg->cx, rg->cy, theta, off, half,
                                    line, CAM_W)
                : -1;
    if (n < 0)
        n = bc_sample_line(px, w, h, rg->cx, rg->cy, theta, off, half,
                           line, CAM_W);
    int hit = ean13_scan_gray_line(line, n, st);
    float sx = rg->cx + vx * off - ux * (n / 2);
    float sy = rg->cy + vy * off - uy * (n / 2);
    hit_record(disp, st, theta,
               (int)(sx + ux * st->x0), (int)(sy + uy * st->x0),
               (int)(sx + ux * st->x1), (int)(sy + uy * st->x1));
    if (hit && prefix_ok(st->code)) {
        memcpy(out, st->code, 14);
        return true;
    }
    return false;
}

/* θ-fan band: a near-miss with this many digits usually means the
 * scanline geometry is slightly off (theta is 1°-quantized and only
 * refreshed every 3rd frame), drifting the line ends out of the bar
 * band and clipping end digits. digits=12 = checksum/parity fail —
 * one digit misread by contrast, not geometry; the fan can't help. */
#define FAN_MIN_DIGITS 8
#define FAN_MAX_DIGITS 11

static int s_fan_runs, s_fan_hits; /* per-scan telemetry, see perf[] */

/* Dense pass across the localized barcode region: 16 scanlines through
 * the region center ALONG the gradient direction theta (so tilt and
 * orientation no longer matter), offset across the bar direction, each
 * with ±1px binning along the bars (bc_sample_line) — this is what
 * reads the weak-contrast/shadowed codes the coarse grid misses.
 * When the pass ends in a fan-band near-miss, retry a small θ±2°/±4°
 * fan around the best offset in the SAME frame (a scanline re-sample
 * is sub-ms; the alternative is waiting a frame for fresh theta). */
static bool scan_region(const uint16_t *px, int w, int h,
                        const bc_region_t *rg, uint8_t *line, char out[14],
                        FrameHit *disp)
{
    ean13_scan_t st;
    int dx = rg->x1 - rg->x0, dy = rg->y1 - rg->y0;
    float thf = rg->theta * (float)M_PI / 180.0f;
    float aux = fabsf(cosf(thf)), auy = fabsf(sinf(thf));
    /* walk length: bbox PROJECTED onto the bar-run direction u (+quiet
       margin) — the old diag/2 overshot badly on squarish regions,
       inflating both the direct walk and the S2 stage extent */
    int half = (int)(dx * aux + dy * auy) / 2 + 48;
    if (half > CAM_W / 2)
        half = CAM_W / 2; /* line buffer is CAM_W bytes */
    int vext = (dx < dy ? dx : dy) / 2; /* bar extent ~ smaller bbox side */
    if (vext < 16)
        vext = 16;
    int joff = vext / 8 < 2 ? 2 : vext / 8;
    /* S2 staging was measured here and REJECTED (see
       docs/scanline-opt-plan.md §S2 post-mortem): luma conversion is
       ALU-bound (~12 cyc/px) and stages beyond the 128KB L2 scatter
       anyway — staged frames clocked 2x slower than direct ones.
       bc_stage_region/bc_sample_line_l8 stay in bc_locate (host-tested)
       should an L2-sized + PIE-converted variant ever pencil out. */
    const bc_stage_t *stg = NULL;

    int best_dig = 0, best_off = 0;
    /* S1 (docs/scanline-opt-plan.md): walk the 16 offsets center-out
       (7,8,6,9,...) — the code usually straddles the region center, so
       the early lines are the likely hits and a decode frame stops
       after far fewer PSRAM-scattering lines. Full coverage and the
       near-miss bookkeeping are unchanged, only the order differs. */
    for (int s = 0; s < 16; s++) {
        int k = (s & 1) ? 8 + s / 2 : 7 - s / 2;
        int off = vext * (2 * k + 1 - 16) / 16;
        if (scan_one_line(px, w, h, rg, stg, rg->theta, off, half, line,
                          out, disp, &st))
            return true;
        if (!st.found && st.digits > best_dig) {
            best_dig = st.digits;
            best_off = off;
        }
    }
    if (best_dig < FAN_MIN_DIGITS || best_dig > FAN_MAX_DIGITS)
        return false;
    s_fan_runs++;
    static const int dth[4] = { -2, 2, -4, 4 };
    for (int a = 0; a < 4; a++) {
        int t = (rg->theta + dth[a] + 180) % 180; /* keep 0..179 */
        for (int j = -1; j <= 1; j++)
            if (scan_one_line(px, w, h, rg, stg, t, best_off + j * joff,
                              half, line, out, disp, &st)) {
                s_fan_hits++;
                return true;
            }
    }
    return false;
}

/* region pass first (when the localizer found one), then the fixed
 * grid as insurance; true when a PREFIX-MATCHING code hit (ends the
 * scan). disp collects the overlay info either way. While a region is
 * locked the grid only runs when allow_grid says so (every Nth frame)
 * — it is pure PSRAM-bus load the region pass already covers. */
static bool scan_frame(const uint16_t *px, int w, int h, uint8_t *line,
                       char out[14], FrameHit *disp,
                       const bc_region_t *rg, bool allow_grid)
{
    ean13_scan_t st;
    disp->kind = 0;
    disp->digits = 0;
    if (rg->found) {
        if (scan_region(px, w, h, rg, line, out, disp))
            return true;
        if (!allow_grid)
            return false;
    }
    for (int r = 0; r < 32; r++) {
        int y = h * (15 + r * 70 / 31) / 100; /* rows across 15%..85% */
        const uint16_t *row = px + y * w;
        for (int x = 0; x < w; x++)
            line[x] = luma565(row[x]);
        int hit = ean13_scan_gray_line(line, w, &st);
        hit_record(disp, &st, -1, st.x0, y, st.x1, y);
        if (hit && prefix_ok(st.code)) {
            memcpy(out, st.code, 14);
            return true;
        }
    }
    for (int c = 0; c < 24; c++) { /* rotated 90°: columns 10%..90% */
        int x = w * (10 + c * 80 / 23) / 100;
        for (int y = 0; y < h; y++)
            line[y] = luma565(px[y * w + x]);
        int hit = ean13_scan_gray_line(line, h, &st);
        hit_record(disp, &st, -1, x, st.x0, x, st.x1);
        if (hit && prefix_ok(st.code)) {
            memcpy(out, st.code, 14);
            return true;
        }
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
static uint32_t s_frame_pixfmt = V4L2_PIX_FMT_RGB565;
static ppa_client_handle_t s_ppa; /* hardware rotate+scale for preview */
static bool s_logged_dqbuf_bytes;

static const char *fourcc_str(uint32_t v, char out[5])
{
    out[0] = (char)(v & 0xff);
    out[1] = (char)((v >> 8) & 0xff);
    out[2] = (char)((v >> 16) & 0xff);
    out[3] = (char)((v >> 24) & 0xff);
    out[4] = '\0';
    return out;
}

static bool frame_size_supports(const struct v4l2_frmsizeenum *fs,
                                uint32_t want_w, uint32_t want_h)
{
    if (fs->type == V4L2_FRMSIZE_TYPE_DISCRETE)
        return fs->discrete.width == want_w && fs->discrete.height == want_h;
    if (fs->type == V4L2_FRMSIZE_TYPE_STEPWISE)
        if (fs->stepwise.step_width == 0 || fs->stepwise.step_height == 0)
            return false;
    if (fs->type == V4L2_FRMSIZE_TYPE_STEPWISE)
        return fs->stepwise.min_width <= want_w &&
               fs->stepwise.max_width >= want_w &&
               fs->stepwise.min_height <= want_h &&
               fs->stepwise.max_height >= want_h &&
               ((want_w - fs->stepwise.min_width) % fs->stepwise.step_width) == 0 &&
               ((want_h - fs->stepwise.min_height) % fs->stepwise.step_height) == 0;
    return false;
}

static void log_video_format_diag(int fd)
{
    static bool done;
    if (done)
        return;

    ESP_LOGI(TAG, "v4l2 format probe: begin");
    for (uint32_t i = 0;; i++) {
        struct v4l2_fmtdesc fm = { 0 };
        fm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fm.index = i;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fm) != 0) {
            if (errno != EINVAL)
                ESP_LOGW(TAG, "VIDIOC_ENUM_FMT index=%lu failed errno=%d",
                         (unsigned long)i, errno);
            break;
        }
        char fourcc[5];
        bool has_1600x1200 = false;
        bool has_1280x720 = false;
        for (uint32_t k = 0;; k++) {
            struct v4l2_frmsizeenum fs = { 0 };
            fs.pixel_format = fm.pixelformat;
            fs.index = k;
            if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) != 0) {
                if (errno != EINVAL)
                    ESP_LOGW(TAG, "VIDIOC_ENUM_FRAMESIZES %s index=%lu errno=%d",
                             fourcc_str(fm.pixelformat, fourcc),
                             (unsigned long)k, errno);
                break;
            }
            has_1600x1200 = has_1600x1200 || frame_size_supports(&fs, 1600, 1200);
            has_1280x720 = has_1280x720 || frame_size_supports(&fs, 1280, 720);
        }
        ESP_LOGI(TAG, "v4l2 fmt=%s desc=\"%s\" 1600x1200=%c 1280x720=%c",
                 fourcc_str(fm.pixelformat, fourcc), fm.description,
                 has_1600x1200 ? 'Y' : 'N', has_1280x720 ? 'Y' : 'N');
    }
    ESP_LOGI(TAG, "v4l2 format probe: end");
    done = true;
}

static void log_try_fmt_diag(int fd, uint32_t req_pixfmt, const char *label)
{
    struct v4l2_format fmt = { 0 };
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = CAM_W;
    fmt.fmt.pix.height = CAM_H;
    fmt.fmt.pix.pixelformat = req_pixfmt;
    if (ioctl(fd, VIDIOC_TRY_FMT, &fmt) != 0) {
        char req[5];
        ESP_LOGW(TAG, "VIDIOC_TRY_FMT %s(%s) failed errno=%d",
                 label, fourcc_str(req_pixfmt, req), errno);
        return;
    }
    char req[5], ret[5];
    ESP_LOGI(TAG, "VIDIOC_TRY_FMT %s(%s) -> %dx%d %s bpl=%u size=%u",
             label, fourcc_str(req_pixfmt, req),
             (int)fmt.fmt.pix.width, (int)fmt.fmt.pix.height,
             fourcc_str(fmt.fmt.pix.pixelformat, ret),
             fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);
}

static void log_s_fmt_diag(int fd, uint32_t req_pixfmt, const char *label)
{
    struct v4l2_format fmt = { 0 };
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = CAM_W;
    fmt.fmt.pix.height = CAM_H;
    fmt.fmt.pix.pixelformat = req_pixfmt;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
        char req[5];
        ESP_LOGW(TAG, "VIDIOC_S_FMT(diag) %s(%s) failed errno=%d",
                 label, fourcc_str(req_pixfmt, req), errno);
        return;
    }
    char req[5], ret[5];
    ESP_LOGI(TAG, "VIDIOC_S_FMT(diag) %s(%s) -> %dx%d %s bpl=%u size=%u",
             label, fourcc_str(req_pixfmt, req),
             (int)fmt.fmt.pix.width, (int)fmt.fmt.pix.height,
             fourcc_str(fmt.fmt.pix.pixelformat, ret),
             fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);
}

/* ISP gamma tone-correction, applied directly to the HW registers.
 *
 * Why not the normal path: esp_ipa 2.1.0 + the IDF 6.0 ISP driver produce a
 * gamma curve whose 16 uniform width-16 segments sum to 256, so the final x
 * boundary is 256. The P4 ISP HW rejects that (its x space is 8-bit 0..255)
 * and floods "ISP: gamma xcoord error" every frame -- a driver-vs-silicon gap
 * (the driver's validation and register-writer both special-case the last
 * point as 256, the HW doesn't). Device-verified: a curve whose power-of-2
 * widths sum to *255* (last boundary 255) is accepted with no error. The IPA
 * gamma block is removed from our vendored sensor config, so nothing reprograms
 * gamma behind us; we own it here.
 *
 * Curve: widths [1,2,4,8, 16x9, 32x3] (powers below) -> cumulative x ending at
 * 255, finer at the low end where the gamma curve bends most. y is a power-law
 * gamma (~sensor's original 0.5-0.65 range). Registers are encoded exactly like
 * isp_ll_gamma_set_correction_curve (3-bit log2 width per segment in gamma_x1/2,
 * 8-bit y per point in gamma_y1..4). */
#define CAM_ISP_GAMMA 0.55f

static void apply_isp_gamma(void)
{
    static const uint8_t pw[16] = { 0, 1, 2, 3, 4, 4, 4, 4,
                                    4, 4, 4, 4, 4, 5, 5, 5 };
    static const uint8_t xc[16] = { 1, 3, 7, 15, 31, 47, 63, 79,
                                    95, 111, 127, 143, 159, 191, 223, 255 };
    uint8_t yy[16];
    for (int i = 0; i < 16; i++) {
        float y = powf((float)xc[i] / 255.0f, CAM_ISP_GAMMA) * 255.0f + 0.5f;
        yy[i] = (uint8_t)(y > 255.0f ? 255.0f : y);
    }
    uint32_t x1 = 0, x2 = 0, y1 = 0, y2 = 0, y3 = 0, y4 = 0;
    for (int i = 0; i < 8; i++)
        x1 |= ((uint32_t)pw[i] << (21 - i * 3));
    for (int i = 8; i < 16; i++)
        x2 |= ((uint32_t)pw[i] << (21 - (i - 8) * 3));
    for (int i = 0; i < 4; i++)
        y1 |= ((uint32_t)yy[i] << (24 - i * 8));
    for (int i = 4; i < 8; i++)
        y2 |= ((uint32_t)yy[i] << (24 - (i - 4) * 8));
    for (int i = 8; i < 12; i++)
        y3 |= ((uint32_t)yy[i] << (24 - (i - 8) * 8));
    for (int i = 12; i < 16; i++)
        y4 |= ((uint32_t)yy[i] << (24 - (i - 12) * 8));

    isp_dev_t *hw = ISP_LL_GET_HW(0);
    for (int ch = 0; ch < 3; ch++) {
        hw->gamma_rgb_x[ch].gamma_x1.val = x1;
        hw->gamma_rgb_x[ch].gamma_x2.val = x2;
        hw->gamma_rgb_y[ch].gamma_y1.val = y1;
        hw->gamma_rgb_y[ch].gamma_y2.val = y2;
        hw->gamma_rgb_y[ch].gamma_y3.val = y3;
        hw->gamma_rgb_y[ch].gamma_y4.val = y4;
    }
    hw->gamma_ctrl.gamma_update = 1;
    while (hw->gamma_ctrl.gamma_update)
        ;
    isp_ll_gamma_enable(hw, true);
    ESP_LOGI(TAG, "ISP gamma applied (en=%d x1=%08lx, 255-boundary workaround)",
             (int)hw->cntl.gamma_en,
             (unsigned long)hw->gamma_rgb_x[0].gamma_x1.val);
}

static bool pipeline_once(void)
{
    if (s_fd >= 0)
        return true;
    /* Lazy bring-up: esp_video_init was previously done by a boot-time probe in
       app_main, which left the CSI/ISP streaming (and DMAing ~57MB/s to PSRAM)
       forever. It is now deferred to the first scan, so a device that never
       scans pays zero camera cost. esp_video can't be torn down + re-REQBUFS'd
       (see the persistent-pipeline note above), so this still happens once. */
    if (!video_init_once())
        return false;

    const char *fail = NULL;
    int fd = -1;
    do {
        fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
        if (fd < 0) {
            fail = "open /dev/video0 failed";
            break;
        }
        log_video_format_diag(fd);
        log_try_fmt_diag(fd, V4L2_PIX_FMT_RGB565, "rgb565");
        log_try_fmt_diag(fd, V4L2_PIX_FMT_YUV420, "yuv420");
        log_try_fmt_diag(fd, V4L2_PIX_FMT_UYVY, "uyvy");
        log_try_fmt_diag(fd, V4L2_PIX_FMT_GREY, "grey");
        log_s_fmt_diag(fd, V4L2_PIX_FMT_YUV420, "yuv420");
        log_s_fmt_diag(fd, V4L2_PIX_FMT_UYVY, "uyvy");
        log_s_fmt_diag(fd, V4L2_PIX_FMT_GREY, "grey");
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
        s_frame_pixfmt = fmt.fmt.pix.pixelformat;
        char fourcc[5];
        ESP_LOGI(TAG, "VIDIOC_S_FMT -> %dx%d %s bpl=%u size=%u",
                 s_frame_w, s_frame_h, fourcc_str(fmt.fmt.pix.pixelformat, fourcc),
                 fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);
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
    apply_isp_gamma(); /* HW gamma the driver can't program (256-boundary bug) */
    char dims[20];
    snprintf(dims, sizeof dims, "%dx%d", s_frame_w, s_frame_h);
    s_logged_dqbuf_bytes = false;
    set_status("video ready %s", dims);
    return true;
}

bool cam_tab5_probe_once(void)
{
    if (!video_init_once())
        return false;
    return pipeline_once();
}

static bool ppa_once(void)
{
    if (s_ppa)
        return true;
    ppa_client_config_t cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    return ppa_register_client(&cfg, &s_ppa) == ESP_OK;
}

/* Telemetry on device showed the single rotate+scale PPA pass of the
 * full 3.84MB frame costing 111ms/frame: ROTATED reads wreck the PSRAM
 * access pattern (~43MB/s effective). Split it: pass 1 scales 0.5 with
 * NO rotation (sequential, fast) into s_mid; pass 2 rotates s_mid —
 * only 0.96MB — into the viewfinder. bc_locate analyzes s_mid, which
 * is frame-oriented, so no coordinate gymnastics are needed at all. */
/* The PPA engine is INPUT-PIXEL bound (~17.5Mpx/s measured: a full
 * 1.92Mpx frame costs ~110ms whatever the operation). So the finder
 * pipeline only eats the CENTER 800x600 crop — hardware-cropped by the
 * PPA, 0.48Mpx ≈ 27ms — which is where the user aims anyway. The
 * decoder still sees the FULL-RES frame (region scan + grid). */
#define CROP_X (CAM_W / 4)
#define CROP_Y (CAM_H / 4)
#define CROP_W (CAM_W / 2)
#define CROP_H (CAM_H / 2)
#define MID_W (CROP_W / 2) /* 400x300 frame-oriented analysis image */
#define MID_H (CROP_H / 2)

static uint16_t *s_mid;
static struct quirc *s_quirc;
static struct quirc_code s_qr_code;
static struct quirc_data s_qr_data;
/* QR decode crop: a NATIVE-RES reticle window (camera-lifecycle-plan §6), NOT a
 * downscale. The old 400x300 failed because it DOWNSCALED the 800x600 crop 0.5x,
 * dropping module density below quirc's finder threshold. We instead CROP a
 * window at the sensor's native resolution (full pixel density per module).
 *
 * Sizing is decode-quality-driven (M2), NOT buffer-size-driven: M1 showed the
 * 156KB quirc image can't fit internal SRAM here (esp-hosted holds the P4
 * internal in lwIP/SDIO; largest contiguous free ≈ 34KB), so the SRAM win is off
 * the table and there's no reason to keep the crop tiny. 400x400 proved too
 * small for the dense provisioning QR (version 12 = 65 modules): a ~350px QR
 * gives only ~4.6 px/module → quirc detects it but ECC-fails. 640x640 lets a
 * QR filling the reticle (~540px) reach ~8 px/module — comfortably above the
 * ~6 px/module decode threshold for v12. Buffer ≈ 800KB RGB565 + ~400KB quirc
 * image (PSRAM). See docs/qr-read-performance.md, camera-lifecycle-plan §6/§8. */
#define QR_HI_W 640
#define QR_HI_H 640
/* decode window centered in the 1600x1200 frame, in absolute frame coordinates.
 * Read straight from the captured frame (independent of the 800x600 preview
 * crop), so it may extend slightly past what the viewfinder shows. */
#define QR_CROP_X ((CAM_W - QR_HI_W) / 2) /* 480 */
#define QR_CROP_Y ((CAM_H - QR_HI_H) / 2) /* 280 */
/* visible reticle, drawn smaller than the decode window: the ~50px native slack
 * each side (~6 modules of a v12 QR) absorbs aiming error and gives the QR's
 * quiet zone room inside the decode crop. M2 sweeps the smallest reliable size. */
#define QR_RETICLE_W 540
#define QR_RETICLE_H 540

/* QR decode (gray convert + quirc) runs on a worker task so it never blocks the
 * camera/preview loop. scan_task copies the 640x640 native-res reticle crop into
 * s_qr_rgb, hands it over via s_qr_go, and collects the result via s_qr_result
 * (non-blocking poll) — the preview keeps running at frame rate while quirc
 * grinds in the background on the otherwise-idle core-0 slack. */
static uint16_t *s_qr_rgb;          /* 640x640 RGB565 reticle crop, scan -> worker */
static TaskHandle_t s_qr_task;
static SemaphoreHandle_t s_qr_go;   /* scan_task gives a frame to the worker */
static SemaphoreHandle_t s_qr_result; /* worker signals a finished decode */
static volatile bool s_qr_hit;      /* worker decoded a valid payload */
static char s_qr_payload[CAM_TAB5_QR_PAYLOAD_MAX + 1];
static volatile int64_t s_qr_gray_us, s_qr_id_us, s_qr_dec_us; /* worker timing */
static volatile int s_qr_candidates, s_qr_runs;
/* decode diagnostics: last quirc_decode error + extracted grid size (cells/side
 * = QR version), surfaced in the scan-end MEAS line so a "detected but not
 * decoded" (candidates>0, dec0) can be told apart: ECC failure = read quality
 * (blur / resolution / glare), invalid grid = bad extraction (clipped / skew). */
static volatile int s_qr_last_err, s_qr_last_size;

static bool mid_blit(const uint16_t *px)
{
    if (!s_mid) {
        s_mid = heap_caps_aligned_alloc(64, (size_t)MID_W * MID_H * 2,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!s_mid)
            return false;
    }
    if (!ppa_once())
        return false;
    ppa_srm_oper_config_t srm = {
        .in = {
            .buffer = (void *)px,
            .pic_w = CAM_W,
            .pic_h = CAM_H,
            .block_w = CROP_W,
            .block_h = CROP_H,
            .block_offset_x = CROP_X,
            .block_offset_y = CROP_Y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = s_mid,
            .buffer_size = (uint32_t)MID_W * MID_H * 2,
            .pic_w = MID_W,
            .pic_h = MID_H,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 0.5,
        .scale_y = 0.5,
        .mirror_x = false,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_ppa, &srm) == ESP_OK;
}

/* RGB565 -> 8-bit luma over a contiguous run. (A PIE SIMD kernel was
 * evaluated and dropped: the P4 PIE ISA lacks a 16-bit lane shift, and
 * quirc_end()'s identify — not this convert — dominates each run anyway.) */
static void rgb565_to_luma(const uint16_t *src, uint8_t *dst, int n)
{
    for (int i = 0; i < n; i++)
        dst[i] = luma565(src[i]);
}

/* Worker body: convert the handed-over 640x640 crop to luma straight into
 * quirc's buffer, then identify + decode. Runs off the camera loop. */
static void qr_decode_once(void)
{
    int w, h;
    uint8_t *gray = quirc_begin(s_quirc, &w, &h);
    if (!gray || w < QR_HI_W || h < QR_HI_H) {
        if (gray)
            quirc_end(s_quirc);
        s_qr_runs++;
        return;
    }
    int64_t t = esp_timer_get_time();
    for (int y = 0; y < QR_HI_H; y++)
        rgb565_to_luma(s_qr_rgb + (size_t)y * QR_HI_W, gray + (size_t)y * w,
                       QR_HI_W);
    s_qr_gray_us += esp_timer_get_time() - t;

    t = esp_timer_get_time();
    quirc_end(s_quirc);
    s_qr_id_us += esp_timer_get_time() - t;
    int n = quirc_count(s_quirc);
    s_qr_candidates = n;

    t = esp_timer_get_time();
    for (int i = 0; i < n; i++) {
        quirc_extract(s_quirc, i, &s_qr_code);
        quirc_decode_error_t err = quirc_decode(&s_qr_code, &s_qr_data);
        if (err == QUIRC_ERROR_DATA_ECC) {
            quirc_flip(&s_qr_code);
            err = quirc_decode(&s_qr_code, &s_qr_data);
        }
        s_qr_last_err = err;             /* diag (last candidate wins) */
        s_qr_last_size = s_qr_code.size; /* diag: QR cells/side */
        if (err != QUIRC_SUCCESS || s_qr_data.payload_len <= 0 ||
            (size_t)s_qr_data.payload_len >= sizeof s_qr_payload ||
            memchr(s_qr_data.payload, '\0', s_qr_data.payload_len))
            continue;
        memcpy(s_qr_payload, s_qr_data.payload, s_qr_data.payload_len);
        s_qr_payload[s_qr_data.payload_len] = '\0';
        s_qr_hit = true;
        break;
    }
    s_qr_dec_us += esp_timer_get_time() - t;
    s_qr_runs++;
}

static void qr_worker(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_qr_go, portMAX_DELAY);
        if (s_quirc)
            qr_decode_once();
        xSemaphoreGive(s_qr_result);
    }
}

static bool qr_once(void)
{
    if (s_quirc)
        return true;
    struct quirc *q = quirc_new();
    if (!q)
        return false;
    if (quirc_resize(q, QR_HI_W, QR_HI_H) < 0) {
        quirc_destroy(q);
        return false;
    }
    s_qr_rgb = heap_caps_aligned_alloc(64, (size_t)QR_HI_W * QR_HI_H * 2,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_qr_go = xSemaphoreCreateBinary();
    s_qr_result = xSemaphoreCreateBinary();
    if (!s_qr_rgb || !s_qr_go || !s_qr_result) {
        quirc_destroy(q);
        return false;
    }
    /* core 0 (with the cam_owner task at prio 4), one below it: the owner
     * preempts to keep the viewfinder at frame rate, the worker takes the
     * slack. Off core 1 so it never steals LVGL render time. */
    if (xTaskCreatePinnedToCore(qr_worker, "qr_worker", 12288, NULL, 3,
                                &s_qr_task, 0) != pdPASS) {
        quirc_destroy(q);
        return false;
    }
    s_quirc = q; /* publish last: worker checks s_quirc before touching it */
    return true;
}

/* Viewfinder = the rotated analysis image (sensor sits 90° to the
 * portrait panel; mirror keeps selfie-natural aiming). The PPA scales
 * it to the FINAL on-screen size (600x800) here, so LVGL blends the
 * canvas 1:1 — its software bilinear transform (lv_image_set_scale)
 * was the viewfinder bottleneck: per-pixel CPU resampling of the whole
 * window on every frame (UserDemo does the same: PPA makes the pixels,
 * LVGL only presents them). PPA cost is INPUT-pixel bound, and the
 * input (s_mid 400x300) is unchanged — only the PSRAM write grows. */
#define PV_SCALE 2
#define PV_W (MID_H * PV_SCALE) /* 600 */
#define PV_H (MID_W * PV_SCALE) /* 800 */
/* viewfinder = rotate + 2x upscale of the (already half-res) s_mid */
static bool preview_blit(uint16_t *dst)
{
    ppa_srm_oper_config_t srm = {
        .in = {
            .buffer = s_mid,
            .pic_w = MID_W,
            .pic_h = MID_H,
            .block_w = MID_W,
            .block_h = MID_H,
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
        .scale_x = PV_SCALE,
        .scale_y = PV_SCALE,
        .mirror_x = true,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_ppa, &srm) == ESP_OK;
}

/* ---- viewfinder overlay drawing (into the preview RGB565 buffer,
 * after the PPA blit so it survives exactly one frame) ----
 * Geometry: preview = transpose of the frame at PV_SCALE/2 scale (the
 * crop is half-res in s_mid, the PPA doubles it back), so a frame point
 * (col=cx, row=ry) lands at preview (x=PV_MAP(ry-CROP_Y),
 * y=PV_MAP(cx-CROP_X)). A frame-ROW scanline therefore shows as a
 * VERTICAL preview segment and a frame-COLUMN scanline as a horizontal
 * one. The "underline" sits beside the scanline; the leader (ひげ線)
 * runs to the bottom edge where the telemetry label box hangs. */
#define PV_MAP(v) ((v) * PV_SCALE / 2)
#define PV_GREEN 0x07E0
#define PV_AMBER 0xFE60

static void pv_px(uint16_t *pv, int x, int y, uint16_t c)
{
    if (x >= 0 && x < PV_W && y >= 0 && y < PV_H)
        pv[y * PV_W + x] = c;
}

static void pv_seg(uint16_t *pv, int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0; /* -abs */
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        /* ~3px thick: the buffer is 1:1 on screen now (was displayed
           2x), so the old 2px stroke would look half as bold */
        for (int ty = 0; ty <= PV_SCALE; ty++)
            for (int tx = 0; tx <= PV_SCALE; tx++)
                pv_px(pv, x0 + tx, y0 + ty, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

#define PV_CYAN 0x07FF

/* localized barcode region as a cyan box (transpose + crop mapping:
 * frame (fx,fy) -> preview ((fy-CROP_Y)/2, (fx-CROP_X)/2); off-crop
 * coordinates simply clip at the preview edges) */
static void draw_region(uint16_t *pv, const bc_region_t *rg)
{
    int px0 = PV_MAP(rg->y0 - CROP_Y), px1 = PV_MAP(rg->y1 - CROP_Y) - 1;
    int py0 = PV_MAP(rg->x0 - CROP_X), py1 = PV_MAP(rg->x1 - CROP_X) - 1;
    pv_seg(pv, px0, py0, px1, py0, PV_CYAN);
    pv_seg(pv, px0, py1, px1, py1, PV_CYAN);
    pv_seg(pv, px0, py0, px0, py1, PV_CYAN);
    pv_seg(pv, px1, py0, px1, py1, PV_CYAN);
}

static void draw_overlay(uint16_t *pv, const FrameHit *hit)
{
    uint16_t col = hit->kind == 2 ? PV_GREEN : PV_AMBER;
    /* same transpose + crop mapping; underline shifted beside the line */
    int off = 6 * PV_SCALE;
    int x0 = PV_MAP(hit->ay - CROP_Y) + off, y0 = PV_MAP(hit->ax - CROP_X);
    int x1 = PV_MAP(hit->by - CROP_Y) + off, y1 = PV_MAP(hit->bx - CROP_X);
    pv_seg(pv, x0, y0, x1, y1, col);
    pv_seg(pv, (x0 + x1) / 2, (y0 + y1) / 2, PV_W / 2, PV_H - 2,
           col); /* ひげ線 → ラベルへ */
}

/* QR aiming reticle (Phase 2 §6): the decode window is QR_HI_W×QR_HI_H centered
 * in the finder crop, so it maps to the preview center; draw the smaller display
 * reticle there as corner brackets. Same §932 transpose: a frame ROW span maps
 * to preview X, a frame COLUMN span to preview Y. Both square here, and the
 * reticle is centered, so we just inset from the preview center by half the
 * display size (PV_MAP is identity at PV_SCALE=2). Redrawn every frame after the
 * PPA blit, like draw_region/draw_overlay. */
static void draw_qr_reticle(uint16_t *pv)
{
    int hx = PV_MAP(QR_RETICLE_H) / 2; /* rows -> preview x */
    int hy = PV_MAP(QR_RETICLE_W) / 2; /* cols -> preview y */
    int x0 = PV_W / 2 - hx, x1 = PV_W / 2 + hx;
    int y0 = PV_H / 2 - hy, y1 = PV_H / 2 + hy;
    int lx = (x1 - x0) / 4, ly = (y1 - y0) / 4; /* bracket leg length */
    const uint16_t c = 0xFFFF;                  /* white */
    pv_seg(pv, x0, y0, x0 + lx, y0, c); /* TL */
    pv_seg(pv, x0, y0, x0, y0 + ly, c);
    pv_seg(pv, x1 - lx, y0, x1, y0, c); /* TR */
    pv_seg(pv, x1, y0, x1, y0 + ly, c);
    pv_seg(pv, x0, y1 - ly, x0, y1, c); /* BL */
    pv_seg(pv, x0, y1, x0 + lx, y1, c);
    pv_seg(pv, x1 - lx, y1, x1, y1, c); /* BR */
    pv_seg(pv, x1, y1 - ly, x1, y1, c);
}

/* bounded DQBUF (camera-lifecycle-plan §1/§2.6): VIDIOC_S_DQBUF_TIMEOUT makes
 * VIDIOC_DQBUF return after this long with no frame, so control returns to the
 * owner to re-check the deadline/cancel. The old unbounded DQBUF hung 113s past
 * the 45s deadline under network contention. CAM_DQBUF_MAX_FAILS consecutive
 * empty waits (≈ that many × the timeout) means the stream is dead → bail early
 * instead of spinning to the full deadline. */
#define CAM_DQBUF_TIMEOUT_MS 500
#define CAM_DQBUF_MAX_FAILS  12   /* ≈6s of zero frames before "stalled" */

/* Network exclusion taken for this scan (resumed on teardown). Phase 1 suspends
 * microlink for both modes; the QR-only full Wi-Fi-off (§4/§8) is a Phase 2
 * measured experiment, so CAM_NET_WIFI is reserved but not yet wired. */
typedef enum {
    CAM_NET_NONE,
    CAM_NET_MICROLINK,
    CAM_NET_WIFI,
} cam_net_policy_t;

/* Per-scan ownership ledger: exactly what cam_run_scan acquired, so teardown
 * releases the same set on every exit path (success / timeout / cancel /
 * init-failure). Persistent resources (esp_video, REQBUFS/mmap, the PPA client,
 * the quirc worker + decode buffers) are NOT here — they are acquire-once and
 * kept (see pipeline_once / qr_once and the esp_video re-REQBUFS constraint,
 * §9). STREAMON likewise stays on between scans (per-scan STREAMOFF is a Phase 2
 * measured item). */
typedef struct {
    cam_net_policy_t net;   /* network suspended via the hook (resume on exit) */
    bool dismiss_cb;        /* ui_tab5_cam_set_dismiss_cb installed */
    bool canvas;            /* viewfinder canvas shown */
} cam_ledger_t;

/* One bounded scan, start to finish, on the resident owner task: acquire
 * per-scan resources (network exclusion, UI), run the capture/decode loop, tear
 * the ledger down on every exit, then publish the result. Returns (does NOT
 * delete the task) — the owner loops for the next command. */
static void cam_run_scan(void)
{
    static uint8_t line[CAM_W]; /* one scan at a time (s_busy) */
    char code[CAM_TAB5_QR_PAYLOAD_MAX + 1];
    char lbl[112], lbl_cache[112];
    FrameHit disp, last_hit;
    bc_region_t cached;
    int hit_ttl = 0;
    int frame_no = 0;
    bool found = false;
    const char *fail = NULL;
    cam_ledger_t led = { 0 };
    bool qr_outstanding = false;
    /* per-stage averages, surfaced through camera.status() at scan end
       — the remote optimization telemetry (no serial in the field) */
    int64_t t_pv = 0, t_loc = 0, t_scan = 0;
    int64_t scan_started = 0;
    int n_loc = 0, n_frames = 0;
    s_fan_runs = 0;
    s_fan_hits = 0;

    lbl_cache[0] = '\0';
    last_hit.kind = 0;
    cached.found = 0;

    /* §4 mutual exclusion: own the CPU/DMA bus BEFORE streaming. The hook BLOCKS
       until the network owner reports stop-completion (microlink tasks/sockets/
       DMA down), so the camera is not fighting DERP traffic when it starts —
       that contention drove the camera to 0.2 fps (quirc identify 26s). Barcode
       keeps Wi-Fi up for the post-scan NDL lookup; QR's full Wi-Fi-off is the
       Phase 2 experiment — Phase 1 suspends microlink for both, which removes
       the measured contender. */
    if (s_net_suspend) {
        s_net_suspend();
        led.net = CAM_NET_MICROLINK;
    }

    bool pipe_ok = pipeline_once(); /* on failure it set s_status */

    if (pipe_ok) {
        /* bound DQBUF so the loop's deadline/cancel check actually runs */
        struct timeval dqto = {
            .tv_sec = 0, .tv_usec = CAM_DQBUF_TIMEOUT_MS * 1000,
        };
        ioctl(s_fd, VIDIOC_S_DQBUF_TIMEOUT, &dqto);

        /* web-modal viewfinder: tap outside it = cancel (and the scrim
           keeps every touch away from the UI behind, see ui_tab5) */
        ui_tab5_cam_set_dismiss_cb(cam_tab5_cancel);
        led.dismiss_cb = true;
        uint16_t *preview = ui_tab5_cam_canvas(PV_W, PV_H);
        led.canvas = true;
        set_status(s_req.mode == SCAN_QR ? "scanning QR%s" : "scanning%s",
                   NULL);

        /* The pipeline keeps streaming between scans, but with both
         * buffers DONE and nobody dequeuing, the driver stalls on the
         * first two frames captured right AFTER the previous scan
         * ended — typically the user still aiming at the book. Without
         * this flush those ghosts decode instantly on the next scan
         * ("face in view, yet it found the ISBN of the last book").
         * A timed-out DQBUF here just means nothing stale is queued. */
        for (int i = 0; i < CAM_BUFS; i++) {
            struct v4l2_buffer buf = { 0 };
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0)
                break;
            ioctl(s_fd, VIDIOC_QBUF, &buf);
        }

        if (preview)
            ui_tab5_cam_overlay_text(s_req.mode == SCAN_QR
                ? "QRコードを枠の中に入れてください"
                : "スキャン中 (緑=読取 黄=惜しい)");

        if (s_req.mode == SCAN_QR && qr_once()) {
            xSemaphoreTake(s_qr_result, 0); /* drop any stale completion */
            s_qr_hit = false;
            s_qr_gray_us = s_qr_id_us = s_qr_dec_us = 0;
            s_qr_runs = 0;
            s_qr_candidates = 0;
            s_qr_last_err = 0; /* QUIRC_SUCCESS */
            s_qr_last_size = 0;
        }

        int dq_fail = 0;
        int64_t deadline =
            esp_timer_get_time() + (int64_t)s_req.timeout_ms * 1000;
        scan_started = esp_timer_get_time();
        while (!s_cancel && esp_timer_get_time() < deadline && !found) {
            struct v4l2_buffer buf = { 0 };
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) {
                /* bounded timeout: re-check the loop condition. cancel/deadline
                   is a normal end; a run of empty waits = a dead stream. */
                if (s_cancel || esp_timer_get_time() >= deadline)
                    break;
                if (++dq_fail >= CAM_DQBUF_MAX_FAILS) {
                    fail = "no frames (camera stalled)";
                    break;
                }
                continue;
            }
            dq_fail = 0;
            if (!s_logged_dqbuf_bytes) {
                char fourcc[5];
                ESP_LOGI(TAG, "DQBUF sample fmt=%s idx=%u bytesused=%u seq=%u",
                         fourcc_str(s_frame_pixfmt, fourcc), buf.index,
                         buf.bytesused, buf.sequence);
                s_logged_dqbuf_bytes = true;
            }
            const uint16_t *px = (const uint16_t *)s_bufs[buf.index];
            int64_t t0 = esp_timer_get_time();
            bool mid_ok = s_frame_w == CAM_W && mid_blit(px);
            bool pv_ok = preview && mid_ok && preview_blit(preview);
            t_pv += esp_timer_get_time() - t0;
            if (s_req.mode == SCAN_QR) {
                frame_no++;
                n_frames++;
                /* Hand a fresh full-res crop to the decode worker when it is
                   idle. The heavy gray+quirc work (~220ms) runs off this loop,
                   so the viewfinder keeps updating every frame instead of
                   freezing for a fifth of a second per QR attempt. */
                if (mid_ok && !qr_outstanding && s_qr_rgb &&
                    (frame_no % 2) == 0) {
                    for (int y = 0; y < QR_HI_H; y++)
                        memcpy(s_qr_rgb + (size_t)y * QR_HI_W,
                               px + (size_t)(QR_CROP_Y + y) * s_frame_w + QR_CROP_X,
                               (size_t)QR_HI_W * 2);
                    qr_outstanding = true;
                    xSemaphoreGive(s_qr_go);
                }
                ioctl(s_fd, VIDIOC_QBUF, &buf); /* worker has its own copy */
                if (qr_outstanding &&
                    xSemaphoreTake(s_qr_result, 0) == pdTRUE) {
                    qr_outstanding = false;
                    if (s_qr_hit) {
                        memcpy(code, s_qr_payload, sizeof code);
                        found = true;
                    }
                }
                if (pv_ok) {
                    draw_qr_reticle(preview);
                    ui_tab5_cam_canvas_update();
                }
                continue;
            }
            /* localize on every 3rd frame (regions move at hand speed,
               not frame speed) and reuse the cached result between */
            if (mid_ok && (frame_no % 3) == 0) {
                t0 = esp_timer_get_time();
                if (bc_locate(s_mid, MID_W, MID_H, 2, &cached)) {
                    /* s_mid is the center crop: shift into frame px */
                    cached.x0 += CROP_X;
                    cached.x1 += CROP_X;
                    cached.cx += CROP_X;
                    cached.y0 += CROP_Y;
                    cached.y1 += CROP_Y;
                    cached.cy += CROP_Y;
                } else {
                    cached.found = 0;
                }
                t_loc += esp_timer_get_time() - t0;
                n_loc++;
            }
            frame_no++;
            n_frames++;
            bool allow_grid = !cached.found || (frame_no % 5) == 0;
            t0 = esp_timer_get_time();
            found = scan_frame(px, s_frame_w, s_frame_h, line, code, &disp,
                               &cached, allow_grid);
            t_scan += esp_timer_get_time() - t0;
            ioctl(s_fd, VIDIOC_QBUF, &buf);
            if (pv_ok && cached.found)
                draw_region(preview, &cached);

            /* overlay: keep the last hit on screen ~2/3s (the PPA blit
               wiped the previous frame's drawing) */
            if (disp.kind) {
                last_hit = disp;
                hit_ttl = 20;
            }
            if (pv_ok && hit_ttl > 0) {
                draw_overlay(preview, &last_hit);
                int mx = (last_hit.ax + last_hit.bx) / 2;
                int my = (last_hit.ay + last_hit.by) / 2;
                char tht[24] = "";
                if (last_hit.theta >= 0)
                    snprintf(tht, sizeof tht, " θ=%d°", last_hit.theta);
                if (last_hit.kind == 2)
                    snprintf(lbl, sizeof lbl, "%s%s   (%d,%d)px%s",
                             last_hit.code,
                             prefix_ok(last_hit.code) ? "" : " (ISBN以外)",
                             mx, my, tht);
                else
                    snprintf(lbl, sizeof lbl,
                             "惜しい %d/13 桁   (%d,%d)px%s",
                             last_hit.digits, mx, my, tht);
                if (strcmp(lbl, lbl_cache) != 0) {
                    snprintf(lbl_cache, sizeof lbl_cache, "%s", lbl);
                    ui_tab5_cam_overlay_text(lbl);
                }
                hit_ttl--;
            }
            if (pv_ok)
                ui_tab5_cam_canvas_update();
        }
    }

    /* ---- teardown: release the ledger on EVERY exit path, in completion order
       (decode drain -> UI dismiss -> network resume -> busy release -> result
       callback; §12). Persistent resources are kept; the pipeline stays
       streaming (see pipeline_once / §9). */
    if (qr_outstanding)
        xSemaphoreTake(s_qr_result, portMAX_DELAY); /* worker stops touching bufs */
    if (led.canvas)
        ui_tab5_cam_canvas_hide();
    if (led.dismiss_cb)
        ui_tab5_cam_set_dismiss_cb(NULL);
    if (led.net != CAM_NET_NONE && s_net_resume)
        s_net_resume();   /* re-arm the network now the camera released the bus */

    char done[256];
    char perf[192] = "";
    if (s_req.mode == SCAN_QR && s_qr_runs)
        snprintf(perf, sizeof perf,
                 " [pv%d gray%d id%d dec%d ms/run cand%d runs%d sz%d/%s total%dms]",
                 (int)(t_pv / (n_frames ? n_frames : 1) / 1000),
                 (int)(s_qr_gray_us / s_qr_runs / 1000),
                 (int)(s_qr_id_us / s_qr_runs / 1000),
                 (int)(s_qr_dec_us / s_qr_runs / 1000),
                 s_qr_candidates, s_qr_runs,
                 s_qr_last_size, quirc_strerror((quirc_decode_error_t)s_qr_last_err),
                 (int)((esp_timer_get_time() - scan_started) / 1000));
    else if (n_frames)
        snprintf(perf, sizeof perf,
                 " [pv%d loc%d scan%d ms/f %s fan%d/%d]",
                 (int)(t_pv / n_frames / 1000),
                 (int)(n_loc ? t_loc / n_loc / 1000 : 0),
                 (int)(t_scan / n_frames / 1000), bc_tensor_impl(),
                 s_fan_runs, s_fan_hits);
    if (found) {
        if (s_req.mode == SCAN_QR)
            snprintf(done, sizeof done, "found QR%s", perf);
        else
            snprintf(done, sizeof done, "found %.13s%s", code, perf);
    }
    else if (fail)
        snprintf(done, sizeof done, "%s%s", fail, perf);
    else
        snprintf(done, sizeof done, "%s%s",
                 s_cancel ? "cancelled" : "timeout (no code)", perf);
    if (pipe_ok || fail)
        set_status("%s", done);

    /* MEASUREMENT (S0): scan summary + FPS + heap to serial, for the on-demand /
       contention / quirc-window investigation. */
    {
        int64_t scan_ms = scan_started ? (esp_timer_get_time() - scan_started) / 1000 : 0;
        int fps10 = (scan_ms > 0 && n_frames) ? (int)((int64_t)n_frames * 10000 / scan_ms) : 0;
        ESP_LOGW(TAG, "MEAS scan-end: %s | %d frames %d.%dfps in %lldms | "
                      "INT free=%u largest=%u | DMA free=%u largest=%u | PSRAM free=%u",
                 done, n_frames, fps10 / 10, fps10 % 10, (long long)scan_ms,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    cam_tab5_cb_t cb = s_req.cb;
    void *arg = s_req.arg;
    s_req.cb = NULL;
    s_busy = false;          /* release before the callback so a re-scan can arm */
    if (cb)
        cb(found ? code : NULL, arg);
    /* resident owner: no vTaskDelete — loop back for the next command */
}

/* ---- single-owner lifecycle (camera-lifecycle-plan §3) -------------------
   One resident task owns every scan: it serializes start/teardown (no
   scan-vs-cancel double-free races), and — unlike the old self-deleting
   per-scan task — survives between scans so repeated-cycle behavior is
   observable. A second SCAN while busy is rejected at post time (the public
   API returns 0), never queued (§3). Cancel is the atomic s_cancel flag, not a
   command. */
static QueueHandle_t s_cam_queue;
static TaskHandle_t s_cam_owner;

static void cam_owner_task(void *arg)
{
    (void)arg;
    for (;;)
        if (xQueueReceive(s_cam_queue, &s_req, portMAX_DELAY) == pdTRUE)
            cam_run_scan();   /* publishes s_req, runs bounded, tears down, fires cb */
}

static bool cam_owner_once(void)
{
    if (s_cam_owner)
        return true;
    if (!s_cam_queue) {
        /* depth 1: the busy-reject guarantees at most one outstanding scan */
        s_cam_queue = xQueueCreate(1, sizeof(cam_scan_req_t));
        if (!s_cam_queue)
            return false;
    }
    /* core 0, prio 4 (JS task at prio 5 outranks it; unpinned it competed with
       the core-1 LVGL render task) — same placement as the old per-scan task. */
    if (xTaskCreatePinnedToCore(cam_owner_task, "cam_owner", 16384, NULL, 4,
                                &s_cam_owner, 0) != pdPASS)
        return false;
    return true;
}

static bool scan_start(uint32_t timeout_ms, const char *prefix,
                       scan_mode_t mode, cam_tab5_cb_t cb, void *arg)
{
    if (s_busy) {
        set_status("busy%s", NULL);
        return false;
    }
    if (!video_init_once())
        return false;
    if (!cam_owner_once()) {
        set_status("owner task create failed%s", NULL);
        return false;
    }
    cam_scan_req_t req = {
        .cb = cb,
        .arg = arg,
        .timeout_ms = timeout_ms ? timeout_ms : 15000,
        .mode = mode,
    };
    snprintf(req.prefix, sizeof req.prefix, "%s", prefix ? prefix : "");
    s_cancel = false;
    s_busy = true;   /* set before posting so a second immediate call rejects */
    if (xQueueSend(s_cam_queue, &req, 0) != pdTRUE) {
        s_busy = false;
        set_status("scan queue full%s", NULL);
        return false;
    }
    return true;
}

bool cam_tab5_scan_start(uint32_t timeout_ms, const char *prefix,
                         cam_tab5_cb_t cb, void *arg)
{
    return scan_start(timeout_ms, prefix, SCAN_EAN13, cb, arg);
}

bool cam_tab5_qr_scan_start(uint32_t timeout_ms, cam_tab5_cb_t cb, void *arg)
{
    return scan_start(timeout_ms, NULL, SCAN_QR, cb, arg);
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

void cam_tab5_set_net_hooks(cam_tab5_net_hook_t suspend_cb,
                            cam_tab5_net_hook_t resume_cb)
{
    (void)suspend_cb;
    (void)resume_cb;
}

bool cam_tab5_probe_once(void)
{
    return false;
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

bool cam_tab5_qr_scan_start(uint32_t timeout_ms, cam_tab5_cb_t cb, void *arg)
{
    (void)timeout_ms;
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
