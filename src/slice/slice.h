/* slice.h - "MuskSlice", the picture slicer: one image in, many images out,
 * laid out the way Photoshop's slice tool draws them.
 *
 * Four files, split by what has to be testable without a window:
 *   tree.c   - the slicing model: a tree of nested regions.
 *   image.c  - loading a source image, cutting a rectangle out of it, writing
 *              that rectangle back out as PNG or JPEG.
 *   slice.c  - the window, the canvas, the control panel, the tray, the
 *              command line and the self-test.
 */
#ifndef SLICE_H
#define SLICE_H

#include "../common/winutil.h"
#include "../common/tray.h"
#include "../common/trayicon.h"
#include "../common/uikit.h"
#include <gdiplus.h>
#include <commctrl.h>
#include <commdlg.h>

/* ------------------------------------------------------------------ */
/* tree.c - the slicing model                                          */
/* ------------------------------------------------------------------ */

/* One region, in image pixels. A region with no children is a leaf, and a
 * leaf is one exported file. A region with children is not exported itself -
 * it only exists to be divided - which is exactly how Photoshop's slice list
 * behaves when you divide a slice and then look at what came out. */
typedef struct Slice {
    RECT r;
    struct Slice **kid;
    int  nkid, ckid;
} Slice;

typedef struct {
    Slice *root;      /* the whole image, or NULL when nothing is loaded */
    int    imgW, imgH;
} SliceTree;

/* Zero the struct before the first st_init, exactly as for a window handle:
 * st_init releases an existing tree so that re-initialising one cannot orphan
 * it, and that means it has to be handed something it can trust. */
void  st_init(SliceTree *t, int w, int h);
void  st_free(SliceTree *t);
/* Re-bases the tree on a different picture. The old layout cannot be carried
 * over meaningfully - a subdivision of a 4000px panorama says nothing about a
 * 300px icon - so this starts over from a single region. */
void  st_set_size(SliceTree *t, int w, int h);
void  st_reset(SliceTree *t);                 /* back to one undivided region */

int   st_count(const SliceTree *t);           /* leaves, i.e. exported files */
int   st_count_at(const Slice *s);            /* leaves under one region      */
/* The leaves, ordered the way they are numbered on the canvas: by top edge,
 * then by left edge. That is reading order, so slice 1 is the top-left one
 * whatever order the cuts were drawn in. Caller xfree()s the array; the Slice
 * pointers stay owned by the tree. */
Slice **st_leaves(const SliceTree *t, int *count);
Slice  *st_leaf_at_point(const SliceTree *t, int x, int y);

/* Divide the whole picture: `n` strips along one axis, or an n x n grid. */
int   st_equal_cols(SliceTree *t, int n);
int   st_equal_rows(SliceTree *t, int n);
int   st_equal_grid(SliceTree *t, int cols, int rows);
/* Cut every `cellPx` pixels, keeping the leftover strip as a smaller last
 * cell - losing the last few pixels of a photo to a rounding rule is never
 * what the user meant. */
int   st_fixed(SliceTree *t, int cellPx);

/* A hand-drawn line. `axis` 0 draws a vertical line at x = pos, 1 a
 * horizontal one at y = pos; [spanLo, spanHi) limits it to part of the picture,
 * which is what makes a short drag cut only what it was dragged across.
 * Every leaf the line passes through is divided in two. Returns how many
 * leaves were cut. */
int   st_cut(SliceTree *t, int axis, int pos, int spanLo, int spanHi);

/* Where a grid line would come to rest if it were dragged to `want`, and how
 * far it is allowed to go. axis 0 is a vertical line at x = pos, 1 a
 * horizontal one at y = pos - the same convention as st_cut and st_divide.
 * [spanLo, spanHi) says which part of the picture the line was grabbed over;
 * the run is then grown from there to every collinear edge it is joined to, so
 * one grab on any segment of a grid line moves the whole line rather than the
 * piece under the cursor.
 *
 * Returns 1 when the line can move at all, filling *outPos with the landing
 * spot and *outSpanLo / *outSpanHi with the grown run so the caller can draw
 * the line it is actually about to move. The window calls this every time the
 * pointer moves during a drag, and the same clamping is applied when the button
 * comes up - the line that was previewed is the line that happens. */
int   st_line_target(const SliceTree *t, int axis, int pos, int want,
                     int spanLo, int spanHi,
                     int *outPos, int *outSpanLo, int *outSpanHi);

/* Move the line at `pos` to `want`, clamped exactly as st_line_target clamps
 * it. The band between the two positions changes hands: cells in front of the
 * line give up the pixels it travels over and cells behind it take them, so the
 * leaves still tile the picture exactly. Cells the line travels straight past
 * are the exception - they hand every pixel to the cell behind the line and
 * stop existing, which is what makes dragging a line past its neighbour do the
 * obvious thing instead of stopping dead.
 *
 * Returns how many cells changed, 0 if the line could not be moved. A result
 * of 0 leaves the tree exactly as it was: the new layout is built and checked
 * before anything is swapped in, so a request that cannot be honoured is a
 * no-op rather than a tree that exports the wrong pixels. */
int   st_move_line(SliceTree *t, int axis, int pos, int want,
                   int spanLo, int spanHi);

/* How many exported cells fall inside `r`, and whether the box lines up with
 * cell edges - *aligned comes back 0 when some cell straddles the edge of the
 * box, which is the only reason a merge can be refused. */
int   st_rect_cells(const SliceTree *t, RECT r, int *aligned);

/* Turn every cell inside `r` back into one cell, and report how many cells it
 * swallowed. Refused (0) for a box that clips a cell, because half a cell is
 * not a slice: the border would be drawn across the middle of an exported
 * file. The merged region becomes the selection's new cell, so a box drawn
 * around a 2x2 subdivision collapses that cell back to one slice rather than
 * leaving it as a flat list of four. */
int   st_merge_rect(SliceTree *t, RECT r);

/* Divide one region into `n` equal parts along an axis. If the region is
 * already divided, the new division replaces the old one. */
int   st_divide(Slice *s, int axis, int n);
int   st_divide_both(Slice *s, int n);
/* Divide every leaf, which is what "subdivide all" in the panel means. */
int   st_divide_all(SliceTree *t, int n);
/* Delete one leaf. Its pixels go back to the region that contained it, so the
 * picture stays completely tiled - a gap would silently drop those pixels from
 * every export. Refused for a slice directly under the root, because handing
 * those pixels back means collapsing the whole picture, which is not what
 * deleting one slice means. */
int   st_delete(SliceTree *t, Slice *leaf);
/* The first sub-region, or NULL if `s` is a leaf. The window steps the
 * selection into it after a divide, so pressing 细分 again keeps going deeper
 * instead of re-cutting the same region to the same depth forever. */
Slice *st_first_child(Slice *s);

/* "<prefix>_01.png", zero-padded to the width of the largest index so a file
 * list sorts in export order. Caller xfree()s. */
wchar_t *st_slice_name(const wchar_t *prefix, int index, int total, int asJpeg);

/* ------------------------------------------------------------------ */
/* image.c - loading, cutting, saving                                  */
/* ------------------------------------------------------------------ */

/* Refuses anything a 32bpp BGRA buffer cannot honestly hold, so a 60000x60000
 * PNG fails with a message instead of taking the process down with it. */
#define IMG_MAX_DIM     30000
#define IMG_MAX_PIXELS  120000000LL

typedef struct {
    HBITMAP   dib;      /* top-down 32bpp DIB section; owns `bits` */
    void     *bits;
    GpBitmap *gp;       /* GDI+ view over `bits`, for painting     */
    int       stride;
    int       w, h;
    int       hasAlpha; /* scanned once at load, drives the checkerboard */
    wchar_t   path[MAX_PATH];
} SourceImage;

int   img_load(SourceImage *s, const wchar_t *path);
void  img_free(SourceImage *s);
/* Writes s's rectangle `r` (image coordinates, clipped to the image) to
 * `path`. JPEG output is flattened onto white, because JPEG has no alpha and
 * would otherwise turn every transparent pixel black. */
int   img_save_rect(const SourceImage *s, RECT r, const wchar_t *path,
                    int asJpeg, int quality);

int   img_is_supported(const wchar_t *path);   /* .jpg / .jpeg / .png only */
/* Creates `path` and every missing parent. Returns 1 if the directory exists
 * when it returns. */
int   img_mkdir_p(const wchar_t *path);
wchar_t *img_stem(const wchar_t *path);         /* "a\b\c.png" -> "c"   */
wchar_t *img_default_dir(const wchar_t *path);  /* "a\b\c.png" -> "a\b\c_slices" */

/* ------------------------------------------------------------------ */
/* slice.c - the window                                                */
/* ------------------------------------------------------------------ */

/* Enough that a mis-typed count cannot start a million file writes; the tree
 * maths stays exact well past this. */
#define SLICE_MAX_CELLS 4096

int   slice_selftest(void);

#endif /* SLICE_H */
