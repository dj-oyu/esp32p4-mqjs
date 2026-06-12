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
#include <math.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

#include "bc_locate.h"
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

/* Dense pass across the localized barcode region: 16 scanlines through
 * the region center ALONG the gradient direction theta (so tilt and
 * orientation no longer matter), offset across the bar direction, each
 * with ±1px binning along the bars (bc_sample_line) — this is what
 * reads the weak-contrast/shadowed codes the coarse grid misses. */
static bool scan_region(const uint16_t *px, int w, int h,
                        const bc_region_t *rg, uint8_t *line, char out[14],
                        FrameHit *disp)
{
    ean13_scan_t st;
    float th = rg->theta * (float)M_PI / 180.0f;
    float ux = cosf(th), uy = sinf(th);
    float vx = -uy, vy = ux;
    int dx = rg->x1 - rg->x0, dy = rg->y1 - rg->y0;
    int half = (int)(sqrtf((float)(dx * dx + dy * dy)) * 0.5f) + 48;
    if (half > CAM_W / 2)
        half = CAM_W / 2; /* line buffer is CAM_W bytes */
    int vext = (dx < dy ? dx : dy) / 2; /* bar extent ~ smaller bbox side */
    if (vext < 16)
        vext = 16;
    for (int k = 0; k < 16; k++) {
        int off = vext * (2 * k + 1 - 16) / 16;
        int n = bc_sample_line(px, w, h, rg->cx, rg->cy, rg->theta, off,
                               half, line, CAM_W);
        int hit = ean13_scan_gray_line(line, n, &st);
        float sx = rg->cx + vx * off - ux * (n / 2);
        float sy = rg->cy + vy * off - uy * (n / 2);
        hit_record(disp, &st, rg->theta,
                   (int)(sx + ux * st.x0), (int)(sy + uy * st.x0),
                   (int)(sx + ux * st.x1), (int)(sy + uy * st.x1));
        if (hit && prefix_ok(st.code)) {
            memcpy(out, st.code, 14);
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

static void scan_task(void *pv)
{
    static uint8_t line[CAM_W]; /* one scanner task at a time (s_busy) */
    char code[14];
    char lbl[112], lbl_cache[112];
    FrameHit disp, last_hit;
    bc_region_t cached;
    int hit_ttl = 0;
    int frame_no = 0;
    bool found = false;
    const char *fail = NULL;
    bool pipe_ok = pipeline_once(); /* on failure it set s_status */
    /* per-stage averages, surfaced through camera.status() at scan end
       — the remote optimization telemetry (no serial in the field) */
    int64_t t_pv = 0, t_loc = 0, t_scan = 0;
    int n_loc = 0, n_frames = 0;

    lbl_cache[0] = '\0';
    last_hit.kind = 0;
    cached.found = 0;

    if (pipe_ok) {
        uint16_t *preview = ui_tab5_cam_canvas(PV_W, PV_H);
        set_status("scanning%s", NULL);

        /* The pipeline keeps streaming between scans, but with both
         * buffers DONE and nobody dequeuing, the driver stalls on the
         * first two frames captured right AFTER the previous scan
         * ended — typically the user still aiming at the book. Without
         * this flush those ghosts decode instantly on the next scan
         * ("face in view, yet it found the ISBN of the last book"). */
        for (int i = 0; i < CAM_BUFS && !fail; i++) {
            struct v4l2_buffer buf = { 0 };
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0)
                fail = "DQBUF failed (flush)";
            else
                ioctl(s_fd, VIDIOC_QBUF, &buf);
        }

        if (preview)
            ui_tab5_cam_overlay_text("スキャン中 (緑=読取 黄=惜しい)");

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
            int64_t t0 = esp_timer_get_time();
            bool mid_ok = s_frame_w == CAM_W && mid_blit(px);
            bool pv_ok = preview && mid_ok && preview_blit(preview);
            t_pv += esp_timer_get_time() - t0;
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
        ui_tab5_cam_canvas_hide();
        /* the pipeline stays up and streaming (see pipeline_once) */
    }

    char done[96];
    char perf[48] = "";
    if (n_frames)
        snprintf(perf, sizeof perf, " [pv%d loc%d scan%d ms/f %s]",
                 (int)(t_pv / n_frames / 1000),
                 (int)(n_loc ? t_loc / n_loc / 1000 : 0),
                 (int)(t_scan / n_frames / 1000), bc_tensor_impl());
    if (found)
        snprintf(done, sizeof done, "found %s%s", code, perf);
    else if (fail)
        snprintf(done, sizeof done, "%s%s", fail, perf);
    else
        snprintf(done, sizeof done, "%s%s",
                 s_cancel ? "cancelled" : "timeout (no code)", perf);
    if (pipe_ok || fail)
        set_status("%s", done);

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
    /* pinned to core 0 (with the JS task, which outranks it at prio 5):
       unpinned it competed with the core-1 LVGL task for render time */
    if (xTaskCreatePinnedToCore(scan_task, "cam_scan", 16384, NULL, 4, NULL,
                                0) != pdPASS) {
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
