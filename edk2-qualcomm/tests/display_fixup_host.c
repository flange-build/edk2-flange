/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host UEFI shims plus synthetic DT fixtures; production functions are extracted
 * from DeviceTree.c by test_display_fixup.py, not duplicated here.
 */
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <libfdt.h>

#define STATIC static
#define IN
#define OUT
#define CONST const
#define VOID void
#define EFIAPI
#define BOOLEAN int
#define TRUE 1
#define FALSE 0
#define UINTN size_t
#define UINT32 uint32_t
#define INT32 int32_t
#define CHAR8 char
#define EFI_STATUS int
#define EFI_SUCCESS 0
#define EFI_BUFFER_TOO_SMALL 1
#define EFI_UNSUPPORTED 2
#define EFI_OUT_OF_RESOURCES 3
#define EFI_DEVICE_ERROR 4
#define EFI_INVALID_PARAMETER 5
#define EFI_ERROR(s) ((s) != EFI_SUCCESS)
#define SIZE_1KB 1024
#define EFI_DT_APPLY_FIXUPS 1
#define EFI_DT_RESERVE_MEMORY 2
#define EFI_DT_INSTALL_TABLE 4
#define PixelBlueGreenRedReserved8BitPerColor 1
#define PcdDisplayHandoff 1
#define DEBUG_INFO 1
#define DEBUG_ERROR 2
static void Debug(unsigned level, const char *format, ...) {}
#define DEBUG(x) Debug x
#define FixedPcdGetBool(x) display_handoff
#define CpuToFdt32 cpu_to_fdt32
#define Fdt32ToCpu fdt32_to_cpu
#define RShiftU64(x,n) ((x) >> (n))
#define FdtTotalSize fdt_totalsize
#define FdtCheckHeader fdt_check_header
#define FdtPathOffset fdt_path_offset
#define FdtNodeOffsetByCompatible fdt_node_offset_by_compatible
#define FdtParentOffset fdt_parent_offset
#define FdtGetProp fdt_getprop
#define FdtOpenInto fdt_open_into
#define FdtDelNode fdt_del_node
#define FdtAddSubnode fdt_add_subnode
#define FdtFirstPropertyOffset fdt_first_property_offset
#define FdtNextPropertyOffset fdt_next_property_offset
#define FdtGetString fdt_get_string
#define FdtSetProp fdt_setprop
#define FdtSetPropString fdt_setprop_string
#define AsciiSPrint snprintf
#define FreePool free
struct FDT_PROPERTY { uint32_t Tag, Length, NameOffset; char Data[]; };
typedef struct FDT_PROPERTY FDT_PROPERTY;
#define FdtGetPropertyByOffset(f,n,l) ((const FDT_PROPERTY *)fdt_get_property_by_offset((f),(n),(l)))
typedef struct { uint32_t PixelFormat, HorizontalResolution, VerticalResolution, PixelsPerScanLine; } EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;
typedef struct { EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info; size_t FrameBufferSize; uint64_t FrameBufferBase; } EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;
typedef struct { EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode; } EFI_GRAPHICS_OUTPUT_PROTOCOL;
typedef void EFI_DT_FIXUP_PROTOCOL;
static int display_handoff = 1, gop_available = 1, dsp_calls, install_calls;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info = {1,1920,1080,1920};
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode = {&info,1920*1080*4,UINT64_C(0x123400000)};
static EFI_GRAPHICS_OUTPUT_PROTOCOL gop = {&mode};
static int gEfiGraphicsOutputProtocolGuid, gFdtTableGuid;
static void *mFixupDsps;
static size_t mFixupDspCount;
static int LocateProtocol(void *a, void *b, void **p) { *p = &gop; return gop_available ? 0 : 4; }
static int InstallConfigurationTable(void *a, void *p) { install_calls++; return 0; }
typedef struct {
    int (*LocateProtocol)(void *, void *, void **);
    int (*InstallConfigurationTable)(void *, void *);
} MockBootServices;
static MockBootServices services = {LocateProtocol, InstallConfigurationTable};
static MockBootServices *gBS = &services;
static void *AllocateCopyPool(size_t size, const void *src) { void *p = malloc(size); if (p) memcpy(p,src,size); return p; }
static size_t DtFixupRunningDsps(void *fdt, const void *dsps, size_t count) { dsp_calls++; return 0; }
#include "display_fixup_functions.c"

static const uint32_t clock_cells[] = {2, 22, 2, 24, 2, 25, 2, 23};
static const uint32_t domain_cells[] = {2, 5, 2, 6};

static void reset(void)
{
    display_handoff = gop_available = 1;
    dsp_calls = install_calls = 0;
    info.PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
    mode.FrameBufferSize = 1920 * 1080 * 4;
}

static void set_cells(void *f, int node, const char *name,
                      const uint32_t *values, size_t count)
{
    fdt32_t cells[8];
    assert(count <= 8);
    for (size_t i = 0; i < count; i++)
        cells[i] = cpu_to_fdt32(values[i]);
    assert(!fdt_setprop(f, node, name, cells, count * sizeof(*cells)));
}

static void assert_cells(void *f, int node, const char *name,
                         const uint32_t *values, size_t count)
{
    int length;
    const fdt32_t *cells = fdt_getprop(f, node, name, &length);
    assert(cells && length == (int)(count * sizeof(*cells)));
    for (size_t i = 0; i < count; i++)
        assert(fdt32_to_cpu(cells[i]) == values[i]);
}

static void *fixture(int chosen, int framebuffer)
{
    void *f = calloc(1, 4096);
    assert(f && !fdt_create_empty_tree(f, 4096));
    if (chosen)
        assert(fdt_add_subnode(f, 0, "chosen") >= 0);
    if (framebuffer) {
        int parent = fdt_path_offset(f, "/chosen");
        int node = fdt_add_subnode(f, parent, "framebuffer@0");
        assert(node >= 0);
        assert(!fdt_setprop_string(f, node, "compatible", "simple-framebuffer"));
        assert(!fdt_setprop_string(f, node, "status", "disabled"));
        assert(!fdt_setprop_string(f, node, "format", "x8r8g8b8"));
        set_cells(f, node, "reg", (uint32_t[]){0, 0, 0, 0}, 4);
        set_cells(f, node, "width", (uint32_t[]){0}, 1);
        set_cells(f, node, "height", (uint32_t[]){0}, 1);
        set_cells(f, node, "stride", (uint32_t[]){0}, 1);
        set_cells(f, node, "clocks", clock_cells, 8);
        set_cells(f, node, "power-domains", domain_cells, 4);
        set_cells(f, node, "display", (uint32_t[]){3}, 1);
    }
    assert(!fdt_pack(f));
    f = realloc(f, fdt_totalsize(f));
    assert(f);
    return f;
}

static void assert_version(void *f, int expected)
{
    int chosen = fdt_path_offset(f, "/chosen"), length;
    const fdt32_t *version = chosen < 0 ? NULL :
        fdt_getprop(f, chosen, "flange,display-fixup-version", &length);
    if (expected)
        assert(version && length == 4 && fdt32_to_cpu(*version) == 1);
    else
        assert(!version);
}

static void assert_live_framebuffer(void *f)
{
    int node = fdt_path_offset(f, "/chosen/framebuffer@123400000");
    assert(node >= 0 && fdt_path_offset(f, "/chosen/framebuffer@0") < 0);
    assert_cells(f, node, "reg", (uint32_t[]){1, 0x23400000, 0, 1920 * 1080 * 4}, 4);
    assert_cells(f, node, "width", (uint32_t[]){1920}, 1);
    assert_cells(f, node, "height", (uint32_t[]){1080}, 1);
    assert_cells(f, node, "stride", (uint32_t[]){7680}, 1);
    assert_cells(f, node, "clocks", clock_cells, 8);
    assert_cells(f, node, "power-domains", domain_cells, 4);
    assert_cells(f, node, "display", (uint32_t[]){3}, 1);
    assert(!strcmp(fdt_getprop(f, node, "status", NULL), "okay"));
    assert(!strcmp(fdt_getprop(f, node, "format", NULL), "x8r8g8b8"));
}

static void assert_idempotent(void *f, size_t size, unsigned flags)
{
    size_t same_size = size;
    void *copy = AllocateCopyPool(size, f);
    assert(copy);
    assert(DtFixupDisplay(f, &same_size) == EFI_SUCCESS);
    assert(same_size == size && !memcmp(f, copy, size));
    assert(DtFixup(NULL, f, &same_size, flags) == EFI_SUCCESS);
    assert(same_size == size && !memcmp(f, copy, size));
    free(copy);
}

static void *grub_fixup(void *f, size_t initial_size, unsigned flags)
{
    size_t original = fdt_totalsize(f), size = initial_size;
    void *copy = AllocateCopyPool(original, f);
    assert(copy);
    assert(DtFixup(NULL, f, &size, flags) == EFI_BUFFER_TOO_SMALL);
    assert(size >= original && !memcmp(f, copy, original));
    assert(!dsp_calls && !install_calls);
    free(copy);
    /* Ubuntu GRUB reallocates only: no header expansion, exactly one retry. */
    f = realloc(f, size);
    assert(f && fdt_totalsize(f) == original);
    assert(DtFixup(NULL, f, &size, flags) == EFI_SUCCESS);
    assert(dsp_calls == !!(flags & EFI_DT_APPLY_FIXUPS));
    assert(install_calls == !!(flags & EFI_DT_INSTALL_TABLE));
    if (display_handoff && (flags & EFI_DT_APPLY_FIXUPS))
        assert_idempotent(f, size, flags);
    return f;
}

static void test_grub(void)
{
    for (int chosen = 0; chosen < 2; chosen++) {
        for (int framebuffer = 0; framebuffer <= chosen; framebuffer++) {
            reset();
            void *f = grub_fixup(fixture(chosen, framebuffer), 0, 3);
            assert_version(f, 1);
            if (framebuffer)
                assert_live_framebuffer(f);
            free(f);
        }
    }
    /* Nonzero but insufficient capacity must also leave the tree intact. */
    reset();
    void *f = fixture(1, 1);
    f = grub_fixup(f, fdt_totalsize(f), 7);
    assert_live_framebuffer(f);
    assert_version(f, 1);
    free(f);
}

static void test_without_gop(void)
{
    for (int empty_mode = 0; empty_mode < 2; empty_mode++) {
        reset();
        gop_available = empty_mode;
        if (empty_mode)
            mode.FrameBufferSize = 0;
        void *f = grub_fixup(fixture(1, 1), 0, 3);
        assert_version(f, 1);
        assert(fdt_node_offset_by_compatible(f, -1, "simple-framebuffer") < 0);
        free(f);
    }
}

static void test_disabled_fixup(void)
{
    for (int disabled_pcd = 0; disabled_pcd < 2; disabled_pcd++) {
        reset();
        display_handoff = !disabled_pcd;
        void *f = grub_fixup(fixture(1, 1), 0, disabled_pcd ? 3 : 2);
        assert_version(f, 0);
        assert(fdt_path_offset(f, "/chosen/framebuffer@0") >= 0);
        free(f);
    }
}

static void test_initial_firmware_tree(void)
{
    reset();
    void *f = fixture(1, 1);
    size_t size = fdt_totalsize(f) + 4096;
    f = realloc(f, size);
    assert(f);
    /* DtPrepareDisplay exposes all but the last 1 KiB of its allocation. */
    assert(!fdt_open_into(f, f, size - 1024));
    assert(DtFixupDisplay(f, &size) == EFI_SUCCESS);
    assert_live_framebuffer(f);
    assert_version(f, 1);
    assert_idempotent(f, size, 3);
    free(f);
}

static void test_unsupported_format(void)
{
    reset();
    void *f = fixture(1, 1);
    size_t original = fdt_totalsize(f), size = original + 1024;
    f = realloc(f, size);
    assert(f);
    void *copy = AllocateCopyPool(original, f);
    assert(copy);
    info.PixelFormat = 0;
    assert(DtFixup(NULL, f, &size, 3) == EFI_UNSUPPORTED);
    assert(!memcmp(f, copy, original) && !dsp_calls);
    free(copy);
    free(f);
}

int main(void)
{
    test_grub();
    test_without_gop();
    test_disabled_fixup();
    test_initial_firmware_tree();
    test_unsupported_format();
    puts("PASS: display fixup host regression (10 scenarios)");
    return 0;
}
