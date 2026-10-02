/*
 * Mac driver OpenGL support through EGL
 *
 * Copyright 2026 David Andrews
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "macdrv.h"
#include "wine/debug.h"
#include "wine/opengl_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(wgl);

/* An OpenGL implementation on EGL that renders into a CAMetalLayer, such as Mesa's Zink on a
 * Vulkan driver for Metal, used in place of the system's OpenGL when WINE_MAC_OPENGL is "egl".
 * win32u does the contexts and pixel formats; this gives it the window surfaces. */

static const struct egl_platform *egl;
static const struct opengl_funcs *funcs;
static struct opengl_driver_funcs macdrv_egl_driver_funcs;
static const struct opengl_drawable_funcs macdrv_egl_surface_funcs;

static void macdrv_egl_init_egl_platform(struct egl_platform *platform)
{
    platform->type = EGL_PLATFORM_SURFACELESS_MESA;
    platform->native_display = 0;
    egl = platform;
}

static BOOL macdrv_egl_surface_create(struct client_surface *client, int format, struct opengl_drawable **drawable)
{
    struct macdrv_client_surface *surface = impl_from_client_surface(client);
    EGLConfig config = egl->configs[(format - 1) % egl->config_count];
    struct opengl_drawable *gl;

    TRACE("client %s, format %d\n", debugstr_client_surface(client), format);

    if (!macdrv_client_surface_acquire_metal_swapchain(surface)) return FALSE;
    if (!(gl = opengl_drawable_create(&macdrv_egl_surface_funcs, format, client, NULL))) return FALSE;

    opengl_drawable_map_buffer(gl, GL_FRONT_LEFT, GL_BACK_LEFT);
    opengl_drawable_map_buffer(gl, GL_FRONT, GL_BACK);
    opengl_drawable_map_buffer(gl, GL_FRONT_AND_BACK, GL_BACK);

    gl->surface = funcs->p_eglCreateWindowSurface(egl->display, config,
            (EGLNativeWindowType)macdrv_swapchain_get_layer(surface->metal_swapchain), NULL);
    if (!gl->surface)
    {
        ERR("Failed to create an EGL surface for %s, error %#x\n", debugstr_client_surface(client),
            funcs->p_eglGetError());
        opengl_drawable_release(gl);
        return FALSE;
    }

    TRACE("Created drawable %s with egl_surface %p\n", debugstr_opengl_drawable(gl), gl->surface);
    *drawable = gl;
    return TRUE;
}

static void macdrv_egl_surface_destroy(struct opengl_drawable *base)
{
    TRACE("drawable %s\n", debugstr_opengl_drawable(base));
}

static void macdrv_egl_surface_flush(struct opengl_drawable *base, UINT flags)
{
    TRACE("drawable %s, flags %#x\n", debugstr_opengl_drawable(base), flags);

    if (flags & GL_FLUSH_INTERVAL) funcs->p_eglSwapInterval(egl->display, abs(base->interval));
}

static BOOL macdrv_egl_surface_swap(struct opengl_drawable *base)
{
    client_surface_present(base->client);
    return funcs->p_eglSwapBuffers(egl->display, base->surface);
}

static const struct opengl_drawable_funcs macdrv_egl_surface_funcs =
{
    .size = sizeof(struct opengl_drawable),
    .destroy = macdrv_egl_surface_destroy,
    .flush = macdrv_egl_surface_flush,
    .swap = macdrv_egl_surface_swap,
};

bool macdrv_egl_wanted(void)
{
    const char *env = getenv("WINE_MAC_OPENGL");
    return env && !strcmp(env, "egl");
}

UINT macdrv_egl_init(const struct opengl_funcs *opengl_funcs, const struct opengl_driver_funcs **driver_funcs)
{
    if (!opengl_funcs->egl_handle)
    {
        ERR("WINE_MAC_OPENGL is \"egl\", but no EGL library was loaded.\n");
        return STATUS_NOT_SUPPORTED;
    }
    funcs = opengl_funcs;

    /* win32u's own EGL driver for everything but the platform and the window surfaces */
    macdrv_egl_driver_funcs = **driver_funcs;
    macdrv_egl_driver_funcs.p_init_egl_platform = macdrv_egl_init_egl_platform;
    macdrv_egl_driver_funcs.p_surface_create = macdrv_egl_surface_create;

    *driver_funcs = &macdrv_egl_driver_funcs;
    return STATUS_SUCCESS;
}
