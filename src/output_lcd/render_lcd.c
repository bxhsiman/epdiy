#include <stdint.h>
#include <string.h>

#include "../output_common/render_method.h"
#include "../output_common/utils.h"

#ifdef RENDER_METHOD_LCD

#include <esp_log.h>
#include <rom/cache.h>

#include "../epd_internals.h"
#include "../output_common/line_queue.h"
#include "../output_common/lut.h"
#include "../output_common/render_context.h"
#include "epd_board.h"
#include "epdiy.h"
#include "lcd_driver.h"
#include "render_lcd.h"

void epd_apply_line_mask_VE(uint8_t* line, const uint8_t* mask, int mask_len);

static bool IRAM_ATTR fill_line_noop(RenderContext_t* ctx, uint8_t* line) {
    memset(line, 0x00, ctx->display_width / 4);
    ctx->lines_consumed++;
    return false;
}

static bool IRAM_ATTR fill_line_white(RenderContext_t* ctx, uint8_t* line) {
    // do a no-op if we're out of the draw area
    if (ctx->lines_consumed < ctx->area.y
        || ctx->lines_consumed >= ctx->area.y + ctx->area.height) {
        return fill_line_noop(ctx, line);
    }

    memset(line, CLEAR_BYTE, ctx->display_width / 4);
    // we use the non-VE version here, because the buffers are not extended to multiples of 16 bytes
    epd_apply_line_mask(line, ctx->line_mask, ctx->display_width / 4);
    ctx->lines_consumed++;
    return false;
}

static bool IRAM_ATTR fill_line_black(RenderContext_t* ctx, uint8_t* line) {
    // do a no-op if we're out of the draw area
    if (ctx->lines_consumed < ctx->area.y
        || ctx->lines_consumed >= ctx->area.y + ctx->area.height) {
        return fill_line_noop(ctx, line);
    }

    memset(line, DARK_BYTE, ctx->display_width / 4);
    // we use the non-VE version here, because the buffers are not extended to multiples of 16 bytes
    epd_apply_line_mask(line, ctx->line_mask, ctx->display_width / 4);
    ctx->lines_consumed++;
    return false;
}

__attribute__((optimize("O3"))) static bool IRAM_ATTR
retrieve_line_isr(RenderContext_t* ctx, uint8_t* buf) {
    if (ctx->lines_consumed >= ctx->lines_total) {
        return false;
    }
    int thread = ctx->line_threads[ctx->lines_consumed];
    assert(thread < NUM_RENDER_THREADS);

    LineQueue_t* lq = &ctx->line_queues[thread];

    BaseType_t awoken = pdFALSE;

    if (lq_read(lq, buf) != 0) {
        ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
        memset(buf, 0x00, ctx->display_width / 4);
    }

    if (ctx->lines_consumed >= ctx->display_height) {
        memset(buf, 0x00, ctx->display_width / 4);
    }
    ctx->lines_consumed += 1;
    return awoken;
}

/// start the next frame in the current update cycle
static void IRAM_ATTR handle_lcd_frame_done(RenderContext_t* ctx) {
    epd_lcd_frame_done_cb(NULL, NULL);
    epd_lcd_line_source_cb(NULL, NULL);

    BaseType_t task_awoken = pdFALSE;
    xSemaphoreGiveFromISR(ctx->frame_done, &task_awoken);

    portYIELD_FROM_ISR();
}

void lcd_do_update(RenderContext_t* ctx) {
    epd_set_mode(1);

    for (uint8_t k = 0; k < ctx->cycle_frames; k++) {
        epd_lcd_frame_done_cb((frame_done_func_t)handle_lcd_frame_done, ctx);
        prepare_context_for_next_frame(ctx);

        // start both feeder tasks
        xTaskNotifyGive(ctx->feed_tasks[!xPortGetCoreID()]);
        xTaskNotifyGive(ctx->feed_tasks[xPortGetCoreID()]);

        // transmission is started in renderer threads, now wait util it's done
        xSemaphoreTake(ctx->frame_done, portMAX_DELAY);

        for (int i = 0; i < NUM_RENDER_THREADS; i++) {
            xSemaphoreTake(ctx->feed_done_smphr[i], portMAX_DELAY);
        }

        ctx->current_frame++;

        // make the watchdog happy.
        vTaskDelay(0);
    }

    epd_lcd_line_source_cb(NULL, NULL);
    epd_lcd_frame_done_cb(NULL, NULL);

    epd_set_mode(0);
}

void epd_push_pixels_lcd(RenderContext_t* ctx, short time, int color) {
    epd_set_mode(1);
    ctx->current_frame = 0;
    ctx->lines_consumed = 0;

    epd_populate_area_mask(ctx->line_mask, ctx->area, ctx->display_width);

    epd_lcd_frame_done_cb((frame_done_func_t)handle_lcd_frame_done, ctx);
    if (color == 0) {
        epd_lcd_line_source_cb((line_cb_func_t)&fill_line_black, ctx);
    } else if (color == 1) {
        epd_lcd_line_source_cb((line_cb_func_t)&fill_line_white, ctx);
    } else {
        epd_lcd_line_source_cb((line_cb_func_t)&fill_line_noop, ctx);
    }
    epd_lcd_start_frame();
    xSemaphoreTake(ctx->frame_done, portMAX_DELAY);
    epd_set_mode(0);
}

#define int_min(a, b) (((a) < (b)) ? (a) : (b))
__attribute__((optimize("O3"))) void IRAM_ATTR
lcd_calculate_frame(RenderContext_t* ctx, int thread_id) {
    assert(ctx->lut_lookup_func != NULL);
    uint8_t* input_line = ctx->feed_line_buffers[thread_id];

    LineQueue_t* lq = &ctx->line_queues[thread_id];
    int l = 0;

    // if there is an error, start the frame but don't feed data.
    if (ctx->error) {
        memset(ctx->line_threads, 0, ctx->lines_total);
        epd_lcd_line_source_cb((line_cb_func_t)&retrieve_line_isr, ctx);
        epd_lcd_start_frame();
        ESP_LOGW("epd_lcd", "draw frame draw initiated, but an error flag is set: %X", ctx->error);
        return;
    }

    // line must be able to hold 2-pixel-per-byte or 1-pixel-per-byte data
    memset(input_line, 0x00, ctx->display_width);

    EpdRect area = ctx->area;
    int min_y, max_y, bytes_per_line, _ppB;
    const uint8_t* ptr_start;
    get_buffer_params(ctx, &bytes_per_line, &ptr_start, &min_y, &max_y, &_ppB);

    assert(area.width == ctx->display_width && area.x == 0 && !ctx->error);

    // index of the line that triggers the frame output when processed
    int trigger_line = int_min(63, max_y - min_y);

    while (l = atomic_fetch_add(&ctx->lines_prepared, 1), l < ctx->lines_total) {
        ctx->line_threads[l] = thread_id;

        // queue is sufficiently filled to fill both bounce buffers, frame
        // can begin
        if (l - min_y == trigger_line) {
            epd_lcd_line_source_cb((line_cb_func_t)&retrieve_line_isr, ctx);
            epd_lcd_start_frame();
        }

        if (l < min_y || l >= max_y
            || (ctx->drawn_lines != NULL && !ctx->drawn_lines[l - area.y])) {
            uint8_t* buf = NULL;
            while (buf == NULL) {
                // break in case of errors
                if (ctx->error & EPD_DRAW_EMPTY_LINE_QUEUE) {
                    printf("on err 1: %d %d\n", ctx->lines_prepared, ctx->lines_consumed);
                    lq_reset(lq);
                    return;
                };

                buf = lq_current(lq);
            }
            memset(buf, 0x00, lq->element_size);
            lq_commit(lq);
            continue;
        }

        uint32_t* lp = (uint32_t*)input_line;
        const uint8_t* ptr = ptr_start + bytes_per_line * (l - min_y);

        Cache_Start_DCache_Preload((uint32_t)ptr, ctx->display_width, 0);

        lp = (uint32_t*)ptr;

        uint8_t* buf = NULL;
        while (buf == NULL) {
            // break in case of errors
            if (ctx->error & EPD_DRAW_EMPTY_LINE_QUEUE) {
                lq_reset(lq);
                printf("on err 2: %d %d\n", ctx->lines_prepared, ctx->lines_consumed);
                return;
            };

            buf = lq_current(lq);
        }

        ctx->lut_lookup_func(lp, buf, ctx->conversion_lut, ctx->display_width);

        // apply the line mask
        epd_apply_line_mask_VE(buf, ctx->line_mask, ctx->display_width / 4);

        lq_commit(lq);
    }
}

#endif
