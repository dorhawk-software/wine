/*
 * Emulator initialisation code
 *
 * Copyright 2000 Alexandre Julliard
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

#include "config.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <limits.h>
#ifdef HAVE_SYS_SYSCTL_H
# include <sys/sysctl.h>
#endif
#ifdef __APPLE__
# include <mach-o/dyld.h>
# include <mach/mach.h>
# include <mach/mach_vm.h>
#endif

#include "main.h"

#if defined(__APPLE__) && defined(__x86_64__) && !defined(HAVE_WINE_PRELOADER)

/* Not using the preloader on x86_64:
 * Reserve the same areas as the preloader does, but using zero-fill sections
 * (the only way to prevent system frameworks from using them, including allocations
 * before main() runs).
 */
__asm__(".zerofill WINE_RESERVE,WINE_RESERVE");
static char __wine_reserve[0x1fffff000] __attribute__((section("WINE_RESERVE, WINE_RESERVE")));

__asm__(".zerofill WINE_TOP_DOWN,WINE_TOP_DOWN");
static char __wine_top_down[0x001ff0000] __attribute__((section("WINE_TOP_DOWN, WINE_TOP_DOWN")));

static const struct wine_preload_info preload_info[] =
{
    { __wine_reserve,  sizeof(__wine_reserve)  }, /*         0x1000 -    0x200000000: low 8GB */
    { __wine_top_down, sizeof(__wine_top_down) }, /* 0x7ff000000000 - 0x7ff001ff0000: top-down allocations + virtual heap */
    { 0, 0 }                                      /* end of list */
};

const __attribute((visibility("default"))) struct wine_preload_info *wine_main_preload_info = preload_info;

static void init_reserved_areas(void)
{
    int i;

    for (i = 0; wine_main_preload_info[i].size != 0; i++)
    {
        /* Match how the preloader maps reserved areas: */
        mmap(wine_main_preload_info[i].addr, wine_main_preload_info[i].size, PROT_NONE,
             MAP_FIXED | MAP_NORESERVE | MAP_PRIVATE | MAP_ANON, -1, 0);
    }
}

#elif defined(__APPLE__) && defined(__aarch64__)

/* arm64 macOS normally refuses mappings below 4GB. With the cross-architecture entitlement
 * the kernel keeps only one hard page of the 4GB __PAGEZERO and maps the rest as a
 * replaceable PROT_NONE region, which nothing else can have claimed before main() runs.
 * Turn it into Wine's low reserved area. Without the entitlement the mapping fails and
 * Wine runs without low memory. */
static const struct wine_preload_info low_reserve[] =
{
    { (void *)0x10000, 0x100000000 - 0x10000 },  /* 64KB - 4GB */
    { 0, 0 }                                      /* end of list */
};

const __attribute((visibility("default"))) struct wine_preload_info *wine_main_preload_info = NULL;

static void init_reserved_areas(void)
{
    if (mmap( low_reserve[0].addr, low_reserve[0].size, PROT_NONE,
              MAP_FIXED | MAP_NORESERVE | MAP_PRIVATE | MAP_ANON, -1, 0 ) == low_reserve[0].addr)
        wine_main_preload_info = low_reserve;
}

#else

/* the preloader will set this variable */
const __attribute((visibility("default"))) struct wine_preload_info *wine_main_preload_info = NULL;

static void init_reserved_areas(void)
{
}

#endif

/* canonicalize path and return its directory name */
static char *realpath_dirname( const char *name )
{
    char *p, *fullpath = realpath( name, NULL );

    if (fullpath)
    {
        p = strrchr( fullpath, '/' );
        if (p == fullpath) p++;
        if (p) *p = 0;
    }
    return fullpath;
}

/* if string ends with tail, remove it */
static char *remove_tail( const char *str, const char *tail )
{
    size_t len = strlen( str );
    size_t tail_len = strlen( tail );
    char *ret;

    if (len < tail_len) return NULL;
    if (strcmp( str + len - tail_len, tail )) return NULL;
    ret = malloc( len - tail_len + 1 );
    memcpy( ret, str, len - tail_len );
    ret[len - tail_len] = 0;
    return ret;
}

/* build a path from the specified dir and name */
static char *build_path( const char *dir, const char *name )
{
    size_t len = strlen( dir );
    char *ret = malloc( len + strlen( name ) + 2 );

    memcpy( ret, dir, len );
    if (len && ret[len - 1] != '/') ret[len++] = '/';
    strcpy( ret + len, name );
    return ret;
}

static const char *get_self_exe(void)
{
#if defined(__linux__) || defined(__FreeBSD_kernel__) || defined(__NetBSD__)
    return "/proc/self/exe";
#elif defined (__FreeBSD__) || defined(__DragonFly__)
    static int pathname[] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    size_t path_size = PATH_MAX;
    char *path = malloc( path_size );
    if (path && !sysctl( pathname, sizeof(pathname)/sizeof(pathname[0]), path, &path_size, NULL, 0 ))
        return path;
    free( path );
#elif defined(__APPLE__)
    uint32_t path_size = PATH_MAX;
    char *path = malloc( path_size );
    if (path && !_NSGetExecutablePath( path, &path_size ))
        return path;
    free( path );
#endif
    return NULL;
}

static void *try_dlopen( const char *argv0 )
{
    char *dir, *path, *p;
    void *handle;

    if (!argv0) return NULL;
    if (!(dir = realpath_dirname( argv0 ))) return NULL;

    if ((p = remove_tail( dir, "/loader" )))
        path = build_path( p, "dlls/ntdll/ntdll.so" );
#ifdef __APPLE__
    /* the loader can run from an app bundle next to ntdll.so, e.g. wine.app/Contents/MacOS/wine,
     * so that it can carry entitlements granted by a provisioning profile */
    else if ((p = remove_tail( dir, ".app/Contents/MacOS" )) && strrchr( p, '/' ))
    {
        *strrchr( p, '/' ) = 0;
        path = build_path( p, "ntdll.so" );
    }
#endif
    else
        path = build_path( dir, "ntdll.so" );

    handle = dlopen( path, RTLD_NOW );
    free( p );
    free( dir );
    free( path );
    return handle;
}


#if defined(__APPLE__) && defined(__aarch64__)

static int file_exists( const char *dir, const char *name )
{
    char *path = build_path( dir, name );
    int ret = !access( path, F_OK );

    free( path );
    return ret;
}

static void set_default( const char *name, const char *dir, const char *value )
{
    char *path;

    if (getenv( name )) return;
    path = dir ? build_path( dir, value ) : strdup( value );
    setenv( name, path, 0 );
    free( path );
}

/* Hadron's runtime keeps its renderers next to Wine: Mesa's Zink in dist/mesa-zink, KosmicKrisp in
 * dist/mesa, and the libraries Wine opens by name in dist/ext/lib (Homebrew's in a checkout).
 * scripts/play names them in the environment. A loader started without it, e.g. as dist/bin/wine,
 * names them itself, so that OpenGL is Zink and Vulkan is KosmicKrisp there too instead of
 * Apple's OpenGL 4.1 and no Vulkan. dyld reads its library path when a process starts, so the
 * loader then starts again. Settings already in the environment are kept. */
static void set_runtime_defaults( const char *exe, char *argv[] )
{
    char *dir, *dist, *libs, *libpath, *old;
    size_t len;

    if (getenv( "WINE_MAC_OPENGL" ) || !exe) return;
    if (!(dir = realpath_dirname( exe ))) return;
    if (!(dist = remove_tail( dir, "/lib/wine/aarch64-unix/wine.app/Contents/MacOS" )) ||
        !file_exists( dist, "mesa-zink/lib/libEGL.1.dylib" ))
    {
        free( dist );
        free( dir );
        return;
    }

    setenv( "WINE_MAC_OPENGL", "egl", 1 );
    set_default( "MESA_LOADER_DRIVER_OVERRIDE", NULL, "zink" );
    set_default( "DRIRC_CONFIGDIR", dist, "mesa-zink/share/drirc.d" );
    if (file_exists( dist, "mesa/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json" ))
    {
        set_default( "VK_DRIVER_FILES", dist, "mesa/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json" );
        set_default( "MESA_KK_EXPERIMENTAL", NULL, "dgc,xfb,sparse" );
    }

    /* Zink's libEGL first, then the runtime's libraries, then whatever was there */
    libs = file_exists( dist, "ext/lib" ) ? build_path( dist, "ext/lib" ) : strdup( "/opt/homebrew/lib" );
    old = getenv( "DYLD_FALLBACK_LIBRARY_PATH" );
    len = strlen( dist ) + strlen( libs ) + (old ? strlen( old ) : 0) + 32;
    libpath = malloc( len );
    snprintf( libpath, len, "%s/mesa-zink/lib:%s%s%s", dist, libs, old ? ":" : "", old ? old : "" );
    setenv( "DYLD_FALLBACK_LIBRARY_PATH", libpath, 1 );

    free( libpath );
    free( libs );
    free( dist );
    free( dir );
    execv( exe, argv );  /* on failure, go on without the library path */
}

/* An executable that can only load at its preferred base (no relocations) needs the range ntdll
 * names in WINEPRELOADRESERVE free when it maps the image. On Linux the preloader reserves it
 * before libc runs. Here nothing can run that early, and libmalloc maps its metadata at a random
 * 2MB-aligned address before main(), at times inside a range above 4GB such as Hogwarts
 * Legacy's 0x140000000-0x15bcc2000, and the image then fails with STATUS_CONFLICTING_ADDRESSES.
 * Start the loader again while something is in the range: the next start draws new placements.
 * The range is not reserved here: a reserved area there would be taken by the top-down
 * allocations made before the image is mapped. */
static void reexec_while_reserve_taken( const char *exe, char *argv[] )
{
    static const char attempt_var[] = "WINEHADRONRESERVEATTEMPT";
    const char *str = getenv( "WINEPRELOADRESERVE" ), *attempt_str;
    unsigned long long start, end;
    mach_vm_address_t addr;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object;
    char *p, attempt_buf[16];
    int attempt;

    if (!str || !exe) return;
    start = strtoull( str, &p, 16 );
    if (*p != '-') return;
    end = strtoull( p + 1, NULL, 16 );
    /* below 4GB the loader's own low reserve keeps the range free */
    if (end <= start || start < 0x100000000ULL) return;

    addr = start;
    if (mach_vm_region( mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64,
                        (vm_region_info_t)&info, &count, &object ) != KERN_SUCCESS || addr >= end)
    {
        unsetenv( attempt_var );
        return;
    }

    attempt = (attempt_str = getenv( attempt_var )) ? atoi( attempt_str ) : 0;
    if (attempt >= 32)
    {
        fprintf( stderr, "wine: %llx-%llx still in use after %d restarts\n", start, end, attempt );
        unsetenv( attempt_var );
        return;
    }
    snprintf( attempt_buf, sizeof(attempt_buf), "%d", attempt + 1 );
    setenv( attempt_var, attempt_buf, 1 );
    execv( exe, argv );  /* on failure, go on and let the image fail as before */
}

#endif

/**********************************************************************
 *           main
 */
int main( int argc, char *argv[] )
{
    void *handle;

#if defined(__APPLE__) && defined(__aarch64__)
    set_runtime_defaults( get_self_exe(), argv );
    reexec_while_reserve_taken( get_self_exe(), argv );
#endif
    init_reserved_areas();

    if ((handle = try_dlopen( get_self_exe() )) ||
        (handle = try_dlopen( argv[0] )))
    {
        void (*init_func)(int, char **) = dlsym( handle, "__wine_main" );
        if (init_func) init_func( argc, argv );
        fprintf( stderr, "wine: __wine_main function not found in ntdll.so\n" );
        exit(1);
    }

    fprintf( stderr, "wine: could not load ntdll.so: %s\n", dlerror() );
    pthread_detach( pthread_self() );  /* force importing libpthread for OpenGL */
    exit(1);
}
