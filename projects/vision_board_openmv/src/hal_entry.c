/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author        Notes
 * 2023-12-10     Rbb666        first version
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "ra/board/ra8d1_ek/board_sdram.h"
#ifdef RT_USING_DFS
    #include <unistd.h>
    #include <fcntl.h>
#endif
#ifdef BSP_USING_OPENMV
    #include <py/compile.h>
    #include <py/runtime.h>
    #include <py/repl.h>
    #include <py/gc.h>
    #include <py/mperrno.h>
    #include <py/stackctrl.h>
    #include <py/frozenmod.h>
    #include <py/mphal.h>
    #include <lib/utils/pyexec.h>
    #include <lib/mp-readline/readline.h>
    #include <framebuffer.h>
    #include <fb_alloc.h>
    #include <ff_wrapper.h>
    #include <usbdbg.h>
    #include <sensor.h>
    #include <tinyusb_debug.h>
    #include <tusb.h>
    #include <led.h>
    #include <mpy_main.h>
#endif /* BSP_USING_OPENMV */
#ifdef RT_USING_FAL
    #include "fal.h"
#endif /* RT_USING_FAL */

#define DRV_DEBUG
#define LOG_TAG             "main"
#include <drv_log.h>

/* MicroPython runs as a task under RT-Thread */
#define MP_TASK_STACK_SIZE      (64 * 1024)

#ifdef BSP_USING_OPENMV
static void *stack_top = RT_NULL;
static char OMV_ATTR_SECTION(OMV_ATTR_ALIGNED(gc_heap[OMV_HEAP_SIZE], 4), ".data");

extern int mount_init(void);
extern void fmath_init(void);
extern int tusb_board_init(void);
static bool exec_boot_script(const char *path, bool interruptible);

void *__signgam_addr(void)
{
    return NULL;
}

void flash_error(int n)
{
    led_state(LED_RED, 0);
    led_state(LED_GREEN, 0);
    led_state(LED_BLUE, 0);
    for (int i = 0; i < n; i++)
    {
        led_state(LED_RED, 0);
        rt_thread_mdelay(100);
        led_state(LED_RED, 1);
        rt_thread_mdelay(100);
    }
    led_state(LED_RED, 0);
}

void NORETURN __fatal_error(const char *msg)
{
    rt_thread_mdelay(100);
    led_state(1, 1);
    led_state(2, 1);
    led_state(3, 1);
    led_state(4, 1);
    mp_hal_stdout_tx_strn("\nFATAL ERROR:\n", 14);
    mp_hal_stdout_tx_strn(msg, strlen(msg));
    for (uint i = 0;;)
    {
        led_toggle(((i++) & 3) + 1);
        rt_thread_mdelay(100);
        if (i >= 16)
        {
            /* to conserve power */
            __WFI();
        }
    }
}

static void omv_entry(void *parameter)
{
    (void) parameter;
    int stack_dummy;
    stack_top = (void *)&stack_dummy;

    bool first_soft_reset = true;

#ifdef __DCACHE_PRESENT
    /* Clean and enable cache */
    SCB_CleanDCache();
#endif
	tusb_board_init();
#ifdef RT_USING_FAL
    fal_init();
#endif
#ifdef BSP_USING_FS
    /* wait sdcard mount */
    extern struct rt_semaphore sem_mnt_lock;;
	rt_sem_take(&sem_mnt_lock, 400);

    struct dfs_fdtable *fd_table_bak = NULL;
#endif
    fmath_init();
#if MICROPY_PY_THREAD
    mp_thread_init(rt_thread_self()->stack_addr, MP_TASK_STACK_SIZE / sizeof(uintptr_t));
#endif
soft_reset:
#ifdef BSP_USING_FS
    mp_sys_resource_bak(&fd_table_bak);
#endif
    led_state(LED_RED, 1);
    led_state(LED_GREEN, 1);
    led_state(LED_BLUE, 1);

    fb_alloc_init0();
    framebuffer_init0();

    /* Stack limit should be less than real stack size, so we have a */
    /* chance to recover from limit hit. (Limit is measured in bytes) */
    mp_stack_set_top(stack_top);
    mp_stack_set_limit(MP_TASK_STACK_SIZE - 1024);

    /* GC init */
    gc_init(&gc_heap[0], &gc_heap[MP_ARRAY_SIZE(gc_heap)]);

    /* MicroPython initialization */
    mp_init();

    readline_init0();
    imlib_init_all();

    usb_cdc_init();
    usbdbg_init();
    file_buffer_init0();
    sensor_init0();

#if MICROPY_PY_SENSOR
    if (sensor_init() != 0 && first_soft_reset)
    {
        LOG_E("sensor init failed!");
    }
#endif

    /* run boot.py and main.py if they exist */
    bool interrupted = exec_boot_script("boot.py", false);

    /* urn boot-up LEDs off */
    led_state(LED_RED, 0);
    led_state(LED_GREEN, 0);
    led_state(LED_BLUE, 0);

    /* Run main.py script on first soft-reset */
    if (first_soft_reset && !interrupted)
    {
        exec_boot_script("main.py", true);
        goto soft_reset_exit;
    }

    /* If there's no script ready, just re-exec REPL */
    while (!usbdbg_script_ready())
    {
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0)
        {
            /* enable IDE interrupt */
            usbdbg_set_irq_enabled(true);
            /* run REPL */
            if (pyexec_mode_kind == PYEXEC_MODE_RAW_REPL)
            {
                if (pyexec_raw_repl() != 0)
                {
                    break;
                }
            }
            else
            {
                if (pyexec_friendly_repl() != 0)
                {
                    break;
                }
            }
            nlr_pop();
        }
    }

    LOG_I("Exit from repy!");

    if (usbdbg_script_ready())
    {
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0)
        {
            /* Enable IDE interrupt */
            usbdbg_set_irq_enabled(true);
            /* Execute the script */
            pyexec_str(usbdbg_get_script());
            /* Disable IDE interrupts */
            usbdbg_set_irq_enabled(false);
            nlr_pop();
        }
        else
        {
            mp_obj_print_exception(&mp_plat_print, (mp_obj_t) nlr.ret_val);
        }

        if (usbdbg_is_busy() && nlr_push(&nlr) == 0)
        {
            /* Enable IDE interrupt */
            usbdbg_set_irq_enabled(true);
            /* Wait for the current command to finish */
            usbdbg_wait_for_command(1000);
            /* Disable IDE interrupts */
            usbdbg_set_irq_enabled(false);
            nlr_pop();
        }
    }

soft_reset_exit:
    /* soft reset */
    mp_printf(&mp_plat_print, "MPY: soft reboot\n");

    gc_sweep_all();
    mp_deinit();
#if MICROPY_PY_THREAD
    mp_thread_deinit();
#endif
#ifdef RT_USING_DFS
    mp_sys_resource_gc(fd_table_bak);
#endif
#ifdef OPENMV_USING_KEY
    rt_uint32_t pin = rt_pin_get(USER_KEY_PIN_NAME);
    rt_pin_detach_irq(pin);
#endif
    first_soft_reset = false;
    goto soft_reset;
}

static bool exec_boot_script(const char *path, bool interruptible)
{
    nlr_buf_t nlr;
    bool interrupted = false;
    if (nlr_push(&nlr) == 0)
    {
        /* Enable IDE interrupts if allowed */
        if (interruptible)
        {
            usbdbg_set_irq_enabled(true);
            usbdbg_set_script_running(true);
        }

        /* Parse, compile and execute the script */
        pyexec_file_if_exists(path);
        nlr_pop();
    }
    else
    {
        interrupted = true;
    }

    /* Disable IDE interrupts */
    usbdbg_set_irq_enabled(false);
    usbdbg_set_script_running(false);

    if (interrupted)
    {
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t) nlr.ret_val);
    }

    return interrupted;
}

static void omv_init_func(void)
{
    rt_thread_t tid;

    tid = rt_thread_create("omv", omv_entry, RT_NULL,
                           MP_TASK_STACK_SIZE / sizeof(uint32_t), 22, 20);
    RT_ASSERT(tid != RT_NULL);

    rt_thread_startup(tid);
}
#endif  /* BSP_USING_OPENMV */

void hal_entry(void)
{
	LOG_I("===================================");
    LOG_I("This is OpenMV4.1.0 demo");
    LOG_I("===================================");

#ifdef BSP_USING_OPENMV
    omv_init_func();
#endif
}

// #include "lv_demos.h"

// void lv_user_gui_init(void)
// {
//     lv_demo_widgets();
// }

#include "gui_guider.h"
#include "custom.h"

lv_ui guider_ui;

void lv_user_gui_init(void) {
    lv_obj_clean(lv_scr_act());
    setup_ui(&guider_ui);
    custom_init(&guider_ui);
}


STATIC mp_obj_t lv_print(void)
{
    LOG_I("This is a my module's logging.");

    return mp_const_none;
}

MP_DEFINE_CONST_FUN_OBJ_0(lv_print_obj, lv_print);

#include "py_image.h"

STATIC mp_obj_t lv_canvas_show(size_t n_args, const mp_obj_t *args)
{
    /* set a my buffer to display. */
    static uint16_t img_buffer[240 * 320];
    /* get camera stream. */
    image_t *arg_img = py_image_cobj(args[0]);
    /* copy pixels to my buffer. */
    rt_memcpy(img_buffer, arg_img->pixels, (arg_img->w * arg_img->h) << 1);
    /* change widget size and set buffer. */
    lv_obj_set_size(guider_ui.screen_canvas_1, arg_img->w, arg_img->h);
    lv_canvas_set_buffer(guider_ui.screen_canvas_1, img_buffer, arg_img->w, arg_img->h, LV_IMG_CF_TRUE_COLOR);

    return mp_const_none;
}

MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lv_canvas_show_obj, 1, 1, lv_canvas_show);

STATIC mp_obj_t lv_result_show(mp_obj_t arg)
{
    lv_label_set_text(guider_ui.screen_label_1, mp_obj_str_get_str(arg));
    return mp_const_none;
}

MP_DEFINE_CONST_FUN_OBJ_1(lv_result_show_obj, lv_result_show);



STATIC const mp_rom_map_elem_t mp_module_guider_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_guider) },
    { MP_ROM_QSTR(MP_QSTR_lv_print), MP_ROM_PTR(&lv_print_obj) },
    { MP_ROM_QSTR(MP_QSTR_lv_canvas_show), MP_ROM_PTR(&lv_canvas_show_obj) },
    { MP_ROM_QSTR(MP_QSTR_lv_result_show), MP_ROM_PTR(&lv_result_show_obj) },
};

STATIC MP_DEFINE_CONST_DICT(mp_module_guider_globals, mp_module_guider_globals_table);

const mp_obj_module_t mp_module_guider = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&mp_module_guider_globals,
};

