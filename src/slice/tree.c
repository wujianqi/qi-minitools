/* tree.c - the slicing model.
 *
 * Photoshop's slice tool does not divide a picture with two lists of
 * coordinates. It builds a tree: the picture is one region, and dividing a
 * region replaces it with sub-regions covering exactly the same pixels. That
 * is what makes "keep cutting up this one cell" expressible at all - a flat
 * list of column and row lines can only ever describe a grid, and subdividing
 * one cell of a grid would cut every other cell along with it.
 *
 * The invariant everything else leans on: the leaves of the tree always tile
 * the root exactly, with no gaps and no overlaps. That is what makes it safe
 * to delete a cell, resize the picture, or draw a line across an already
 * divided one - the pixels are always accounted for, so nothing silently goes
 * missing from the export.
 */
#include "slice.h"

/* ------------------------------------------------------------------ */
/* regions                                                             */
/* ------------------------------------------------------------------ */

static int is_leaf(const Slice *s)
{
    return (!s || s->nkid == 0);
}

static Slice *region_new(int l, int t, int r, int b)
{
    Slice *s;
    if (r <= l || b <= t) return NULL;
    s = (Slice *)xcalloc(1, sizeof(Slice));
    if (!s) return NULL;
    s->r.left = l; s->r.top = t; s->r.right = r; s->r.bottom = b;
    return s;
}

static void region_free(Slice *s)
{
    int i;
    if (!s) return;
    for (i = 0; i < s->nkid; i++) region_free(s->kid[i]);
    xfree(s->kid);
    xfree(s);
}

/* Drops every sub-region, so the region becomes a single leaf again. This is
 * how a deleted cell hands its pixels back without leaving a hole. */
static void region_clear(Slice *s)
{
    int i;
    if (!s) return;
    for (i = 0; i < s->nkid; i++) region_free(s->kid[i]);
    s->nkid = 0;
}

/* Takes an already-built region as a child. Split out from region_add_kid so
 * the rebuild below can graft a finished subtree in rather than copying its
 * rectangles; it either takes ownership or leaves `k` alone for the caller to
 * free, which is the one rule that keeps a half-built tree from leaking. */
static int region_own(Slice *parent, Slice *k)
{
    if (!parent || !k) return 0;
    if (parent->nkid == parent->ckid) {
        int cap = parent->ckid ? parent->ckid * 2 : 4;
        Slice **nk = (Slice **)xrealloc(parent->kid, (size_t)cap * sizeof(Slice *));
        if (!nk) return 0;
        parent->kid = nk;
        parent->ckid = cap;
    }
    parent->kid[parent->nkid++] = k;
    return 1;
}

static Slice *region_add_kid(Slice *parent, int l, int t, int r, int b)
{
    Slice *k = region_new(l, t, r, b);
    if (!k) return NULL;
    if (!region_own(parent, k)) { region_free(k); return NULL; }
    return k;
}

/* A plain list of rectangles, for the one operation that works out new
 * geometry and rebuilds a tree from it. It owns its rectangles, not any Slice,
 * so nothing here can dangle into the tree it is about to replace. */
typedef struct {
    RECT *r;
    int   n, cap;
} RectList;

static int rectlist_push(RectList *L, RECT r)
{
    if (L->n == L->cap) {
        int cap = L->cap ? L->cap * 2 : 64;
        RECT *nr = (RECT *)xrealloc(L->r, (size_t)cap * sizeof(RECT));
        if (!nr) return 0;
        L->r = nr;
        L->cap = cap;
    }
    L->r[L->n++] = r;
    return 1;
}

static void rectlist_free(RectList *L)
{
    xfree(L->r);
    L->r = NULL;
    L->n = L->cap = 0;
}

/* ------------------------------------------------------------------ */
/* the tree                                                            */
/* ------------------------------------------------------------------ */

void st_init(SliceTree *t, int w, int h)
{
    if (!t) return;
    /* Frees first: re-initialising a live tree is a normal thing to do when a
     * test switches to a differently sized picture, and silently orphaning the
     * old root is exactly the kind of leak the counters are there to catch. */
    region_free(t->root);
    t->imgW = (w > 0) ? w : 1;
    t->imgH = (h > 0) ? h : 1;
    t->root = region_new(0, 0, t->imgW, t->imgH);
}

void st_free(SliceTree *t)
{
    if (!t) return;
    region_free(t->root);
    t->root = NULL;
    t->imgW = t->imgH = 0;
}

void st_set_size(SliceTree *t, int w, int h)
{
    if (!t) return;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    region_free(t->root);
    t->imgW = w;
    t->imgH = h;
    t->root = region_new(0, 0, w, h);
}

void st_reset(SliceTree *t)
{
    if (!t) return;
    region_clear(t->root);
}

int st_count_at(const Slice *s)
{
    int i, n;
    if (is_leaf(s)) return s ? 1 : 0;
    n = 0;
    for (i = 0; i < s->nkid; i++) n += st_count_at(s->kid[i]);
    return n;
}

int st_count(const SliceTree *t)
{
    return (t && t->root) ? st_count_at(t->root) : 0;
}

/* ------------------------------------------------------------------ */
/* leaves, in reading order                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    Slice **items;
    int     n, cap;
} LeafList;

static void leaf_collect(const Slice *s, LeafList *L)
{
    int i;
    if (!s) return;
    if (is_leaf(s)) {
        if (L->n == L->cap) {
            int cap = L->cap ? L->cap * 2 : 32;
            Slice **ni = (Slice **)xrealloc(L->items, (size_t)cap * sizeof(Slice *));
            if (!ni) return;
            L->items = ni;
            L->cap = cap;
        }
        L->items[L->n++] = (Slice *)s;
        return;
    }
    for (i = 0; i < s->nkid; i++) leaf_collect(s->kid[i], L);
}

/* Top edge first, then left edge. A partition has no two regions sharing a
 * top-left corner, so this is a total order and the numbering is stable no
 * matter what order the cuts were drawn in. */
static int leaf_cmp(const void *a, const void *b)
{
    const Slice *x = *(Slice * const *)a;
    const Slice *y = *(Slice * const *)b;
    if (x->r.top != y->r.top) return (x->r.top < y->r.top) ? -1 : 1;
    if (x->r.left != y->r.left) return (x->r.left < y->r.left) ? -1 : 1;
    return 0;
}

Slice **st_leaves(const SliceTree *t, int *count)
{
    LeafList L;
    if (count) *count = 0;
    if (!t || !t->root) return NULL;
    L.items = NULL; L.n = 0; L.cap = 0;
    leaf_collect(t->root, &L);
    if (L.n > 1) qsort(L.items, (size_t)L.n, sizeof(Slice *), leaf_cmp);
    if (count) *count = L.n;
    return L.items;
}

Slice *st_leaf_at_point(const SliceTree *t, int x, int y)
{
    Slice *s;
    if (!t || !t->root) return NULL;
    s = t->root;
    for (;;) {
        Slice *next = NULL;
        int i;
        if (is_leaf(s)) return s;
        for (i = 0; i < s->nkid; i++) {
            const RECT *k = &s->kid[i]->r;
            if (x >= k->left && x < k->right && y >= k->top && y < k->bottom) {
                next = s->kid[i];
                break;
            }
        }
        /* The tree is a partition, so this cannot happen - but returning the
         * deepest region that does contain the point beats walking off the
         * end if it ever did. */
        if (!next) return s;
        s = next;
    }
}

/* ------------------------------------------------------------------ */
/* dividing                                                            */
/* ------------------------------------------------------------------ */

int st_divide(Slice *s, int axis, int n)
{
    int len, lo, hi, l, t, r, b, i, prev;

    if (!s) return 0;
    len = (axis == 0) ? (s->r.right - s->r.left) : (s->r.bottom - s->r.top);
    if (len < 1) return 0;
    /* A 3-pixel cell cannot become five columns. Clamping here is what keeps
     * every part at least one pixel wide no matter what the panel says. */
    if (n > len) n = len;
    if (n < 1) n = 1;

    region_clear(s);
    if (n <= 1) return 1;

    l = s->r.left; t = s->r.top; r = s->r.right; b = s->r.bottom;
    lo = (axis == 0) ? l : t;
    hi = (axis == 0) ? r : b;
    prev = lo;
    for (i = 1; i < n; i++) {
        int cut = lo + (int)(((long)i * len + n / 2) / n);
        /* Round to nearest, then push inside the neighbours: the boundary has
         * to leave at least one pixel on each side of every part, and with a
         * rounded split of an odd length two of them would otherwise meet. */
        if (cut < prev + 1)     cut = prev + 1;
        if (cut > hi - (n - i)) cut = hi - (n - i);
        if (cut < prev + 1) break;
        if (axis == 0) region_add_kid(s, prev, t, cut, b);
        else           region_add_kid(s, l, prev, r, cut);
        prev = cut;
    }
    if (axis == 0) region_add_kid(s, prev, t, hi, b);
    else           region_add_kid(s, l, prev, r, hi);
    return s->nkid;
}

int st_divide_both(Slice *s, int n)
{
    int i, k;
    if (!s) return 0;
    st_divide(s, 0, n);
    for (i = 0; i < s->nkid; i++) st_divide(s->kid[i], 1, n);
    k = st_count_at(s);
    return k;
}

int st_divide_all(SliceTree *t, int n)
{
    Slice **lv;
    int count = 0, i;
    if (!t || !t->root) return 0;
    lv = st_leaves(t, &count);
    if (!lv) return 0;
    for (i = 0; i < count; i++) st_divide_both(lv[i], n);
    xfree(lv);
    return st_count(t);
}

/* Splits one leaf at an exact position, which is what a hand-drawn line needs
 * - dividing it "in two" would put the boundary in the middle instead of
 * under the cursor. */
static int leaf_split_at(Slice *s, int axis, int pos)
{
    int l = s->r.left, t = s->r.top, r = s->r.right, b = s->r.bottom;
    if (!is_leaf(s)) return 0;
    if (axis == 0) {
        if (pos <= l || pos >= r) return 0;
    } else {
        if (pos <= t || pos >= b) return 0;
    }
    if (axis == 0) {
        if (!region_add_kid(s, l, t, pos, b)) return 0;
    } else {
        if (!region_add_kid(s, l, t, r, pos)) return 0;
    }
    if (axis == 0) {
        if (!region_add_kid(s, pos, t, r, b)) {
            /* the second half could not be allocated: undo rather than leave
             * a region covering only part of itself */
            region_free(s->kid[--s->nkid]);
            return 0;
        }
    } else {
        if (!region_add_kid(s, l, pos, r, b)) {
            region_free(s->kid[--s->nkid]);
            return 0;
        }
    }
    return 2;
}

static int cut_walk(Slice *s, int axis, int pos, int lo, int hi)
{
    int i, n = 0;
    if (!s) return 0;
    if (is_leaf(s)) {
        int a = (axis == 0) ? s->r.left : s->r.top;
        int b = (axis == 0) ? s->r.right : s->r.bottom;
        int sa = (axis == 0) ? s->r.top : s->r.left;
        int sb = (axis == 0) ? s->r.bottom : s->r.right;
        /* The line has to cross this region, and the drag has to have covered
         * part of it. The drag is measured across the line, so the span is on
         * the *other* axis from the cut - comparing a vertical line's x span
         * against the region's y extent would make every short horizontal drag
         * look like it crossed the whole picture. Both halves also have to be
         * at least one pixel, hence the strict comparisons. */
        if (pos > a && pos < b && hi > sa && lo < sb)
            return leaf_split_at(s, axis, pos) ? 1 : 0;
        return 0;
    }
    for (i = 0; i < s->nkid; i++)
        n += cut_walk(s->kid[i], axis, pos, lo, hi);
    return n;
}

int st_cut(SliceTree *t, int axis, int pos, int spanLo, int spanHi)
{
    if (!t || !t->root) return 0;
    return cut_walk(t->root, axis, pos, spanLo, spanHi);
}

/* ------------------------------------------------------------------ */
/* moving a line                                                       */
/* ------------------------------------------------------------------ */

/* A boundary edge sits *on* the line at `pos` when it is exactly there. axis 0
 * is a vertical line, so the edges that count are a cell's left and right; axis
 * 1 is the same with the two axes swapped. */
static int edge_at(const RECT *r, int axis, int pos)
{
    if (axis == 0) return (r->left == pos || r->right == pos);
    return (r->top == pos || r->bottom == pos);
}

/* Does the line show anywhere in the half-open span [lo, hi)? This is what
 * decides whether a cell takes part in a move. */
static int edge_on_line(const RECT *r, int axis, int pos, int lo, int hi)
{
    int sa, sb;
    if (!edge_at(r, axis, pos)) return 0;
    if (axis == 0) { sa = r->top; sb = r->bottom; }
    else            { sa = r->left; sb = r->right; }
    return (sa < hi && lo < sb);
}

/* The run of collinear edges that a grab over [lo, hi) belongs to, grown to a
 * fixpoint.
 *
 * In a grid the three segments of one column line are three separate edges with
 * nothing joining them, so the run has to be discovered by growing: every edge
 * that touches the span widens it to its own full extent, and a pass that finds
 * nothing new means the whole line is in. That is what makes a grab on any one
 * segment move the entire line - which is what a user pointing at a grid line
 * expects - while two lines that happen to sit at the same x far apart in the
 * picture stay separate, because nothing connects them.
 *
 * "Touches" rather than "overlaps" is the whole trick, and it is what makes a
 * grid line behave like a grid line. The cells in a row below a row above it do
 * not overlap it at all - they meet along one horizontal border - so a run grown
 * by overlap would stop dead at the first row and every row would need its own
 * drag. Touching is safe here precisely because the span absorbs a cell together
 * with its full extent: after a pass, every y the span covers is inside a cell
 * that has an edge on the line.
 *
 * That in turn is what makes the move below safe. Once every y in the span is
 * covered by such a cell, the line is a boundary along its whole length, so no
 * cell straddles it and every cell the move touches has a full-length face on
 * it. Without that, half of a cell would move while the cell beside it stayed
 * put, and the two would overlap. */
static void span_grow(const SliceTree *t, int axis, int pos, int *lo, int *hi)
{
    Slice **lv;
    int n = 0, i, changed;

    lv = st_leaves(t, &n);
    if (!lv) return;
    for (changed = 1; changed; ) {
        changed = 0;
        for (i = 0; i < n; i++) {
            const RECT *r = &lv[i]->r;
            int sa, sb;
            if (!edge_at(r, axis, pos)) continue;
            if (axis == 0) { sa = r->top; sb = r->bottom; }
            else            { sa = r->left; sb = r->right; }
            if (sb < *lo || *hi < sa) continue;      /* nowhere near it */
            if (sa < *lo) { *lo = sa; changed = 1; }
            if (sb > *hi) { *hi = sb; changed = 1; }
        }
    }
    xfree(lv);
}

/* Everything both halves of a move need: which line, where it is, where it is
 * being asked to go, and the run it belongs to. `want` is left equal to `pos`
 * whenever the line cannot move, which is how both callers recognise that. */
typedef struct {
    int axis, pos, want;
    int lo, hi;
} LineMove;

static void line_prepare(const SliceTree *t, LineMove *M, int axis, int pos,
                         int want, int spanLo, int spanHi)
{
    Slice **lv;
    int n = 0, i, dir, limLo, limHi, size;

    M->axis = axis; M->pos = pos; M->want = pos;
    M->lo = spanLo; M->hi = spanHi;
    if (!t || !t->root) return;
    size = (axis == 0) ? t->imgW : t->imgH;
    /* A line sitting on the picture's own border has no cell on the far side
     * of it, so there is nothing to trade pixels with. */
    if (pos <= 0 || pos >= size) return;
    if (spanHi <= spanLo) return;

    span_grow(t, axis, pos, &M->lo, &M->hi);
    if (M->hi <= M->lo) return;
    if (M->lo < 0) M->lo = 0;
    if (M->hi > size) M->hi = size;

    dir = (want > pos) ? 1 : (want < pos ? -1 : 0);
    if (!dir) return;

    /* The arithmetic in line_effect assumes a clean run: every cell in it has
     * a full-length face on the line, so none of them straddles it, so the two
     * halves of each cell can be reasoned about separately. span_grow gives
     * that for any real line, because a run that reaches a y with no edge on
     * the line keeps growing. What it does not cover is a position that is no
     * longer an edge of anything at all - a line remembered from before the
     * picture was re-laid out - where a cell really can straddle it and the
     * move would tear the tiling. Checked rather than assumed, because the
     * alternative is a silently wrong export. */
    lv = st_leaves(t, &n);
    if (!lv) return;
    for (i = 0; i < n; i++) {
        const RECT *r = &lv[i]->r;
        int a, b, lo, hi;
        if (axis == 0) { a = r->left; b = r->right; lo = r->top; hi = r->bottom; }
        else            { a = r->top;  b = r->bottom; lo = r->left; hi = r->right; }
        if (lo >= M->hi || M->lo >= hi) continue;
        if (a < pos && pos < b) { xfree(lv); return; }
    }

    /* The only hard limit is the cell the line runs into as it grows: every
     * other cell on the line gives ground instead, and a cell the line travels
     * right past simply stops existing. Cells that end up empty are dropped by
     * line_effect, not prevented here - letting the line swallow its neighbour
     * is the whole point of dragging it that far. */
    limLo = 1;
    limHi = size - 1;
    lv = st_leaves(t, &n);
    if (!lv) return;
    for (i = 0; i < n; i++) {
        const RECT *r = &lv[i]->r;
        int a, b;
        if (!edge_on_line(r, axis, pos, M->lo, M->hi)) continue;
        if (axis == 0) { a = r->left;  b = r->right;  }
        else            { a = r->top;   b = r->bottom; }
        /* Travelling one way the cells that grow are the ones behind the line,
         * travelling the other way they are the ones in front of it, so which
         * edge of each cell bounds the movement depends on the direction. */
        if (dir > 0) {
            if (b == pos && a + 1 > limLo) limLo = a + 1;
        } else {
            if (a == pos && b - 1 < limHi) limHi = b - 1;
        }
    }
    xfree(lv);

    /* Squeezed from both sides: there is no position left for it. */
    if (limLo > limHi) return;
    if (want < limLo) want = limLo;
    if (want > limHi) want = limHi;
    M->want = want;
}

int st_line_target(const SliceTree *t, int axis, int pos, int want,
                   int spanLo, int spanHi,
                   int *outPos, int *outSpanLo, int *outSpanHi)
{
    LineMove M;
    line_prepare(t, &M, axis, pos, want, spanLo, spanHi);
    if (outSpanLo) *outSpanLo = M.lo;
    if (outSpanHi) *outSpanHi = M.hi;
    if (outPos) *outPos = M.want;
    return (M.want != pos) ? 1 : 0;
}

/* What the line does to one cell. Returns 0 to keep it as it is, 1 with *out
 * filled for a cell whose extent changed, and 2 for a cell the line has moved
 * past: it handed every pixel it had to the cell behind the line and no longer
 * exists. *drop is set for that last case so the caller can count it.
 *
 * The band between the old and the new position changes hands, and that is all
 * there is to it. A cell behind the line (the side the band travels away from)
 * gives up the part of itself the band covers; a cell in front takes it. A cell
 * on the receiving side whose far edge is inside the band ends up empty, which
 * is the case the drop is for - its pixels are already inside the cell that
 * grew over it. */
static int line_effect(const RECT *r, const LineMove *M, int *drop, RECT *out)
{
    int a, b, lo, sh;
    int dir = (M->want > M->pos) ? 1 : -1;

    *out = *r;
    if (M->axis == 0) { a = r->left;  b = r->right;  lo = r->top;    sh = r->bottom; }
    else              { a = r->top;   b = r->bottom; lo = r->left;  sh = r->right;  }
    if (lo >= M->hi || M->lo >= sh) return 0;        /* not part of the run */

    if (dir > 0) {
        if (b < M->pos) return 0;                    /* already clear of it  */
        if (a >= M->want) return 0;                  /* clear of where it lands */
        if (b == M->pos) {
            b = M->want;                             /* takes up the band    */
        } else if (a >= M->pos) {
            a = M->want;                             /* gives up the band    */
        } else if (M->want > b) {
            /* Straddles the line. Unreachable inside a run grown by span_grow,
             * which guarantees an edge on the line all the way along - kept so a
             * tree that got here some other way still ends up tiling. */
            b = M->want;
        }
    } else {
        if (a > M->pos) return 0;
        if (b <= M->want) return 0;
        if (a == M->pos) {
            a = M->want;                             /* takes up the band    */
        } else if (b <= M->pos) {
            b = M->want;                             /* gives up the band    */
        } else if (M->want < a) {
            a = M->want;                             /* straddles, as above  */
        }
    }
    if (b <= a) { *drop = 1; return 2; }
    if (M->axis == 0) { out->left = a; out->right = b; }
    else              { out->top = a;  out->bottom = b; }
    return 1;
}

/* ------------------------------------------------------------------ */
/* rebuilding a guillotine partition                                   */
/* ------------------------------------------------------------------ */

/* The move above works out new rectangles; the tree has to be built again from
 * them, because a cell that moved is no longer the rectangle its parent was
 * divided around and a parent whose rectangle no longer matches its children
 * breaks st_leaf_at_point's walk down to the cell under the pointer.
 *
 * Every division in this file cuts a region into strips along one axis, so the
 * partition is always a guillotine one and can be rebuilt by finding those cuts
 * again. The layout is not invented: it comes out the same shape the cuts were
 * originally made in, which matters because the user has been looking at this
 * tree, clicking through it and stepping into it with 细分. */

/* One sorted edge of the sweep below: where it is, and whether it opens a cell
 * or closes one. */
typedef struct { int at, opens; } EdgeEv;

static int edge_cmp(const void *a, const void *b)
{
    const EdgeEv *x = (const EdgeEv *)a, *y = (const EdgeEv *)b;
    if (x->at != y->at) return (x->at < y->at) ? -1 : 1;
    return x->opens - y->opens;
}

/* One guillotine cut of cells [lo, hi) within `box`, or 0 if there is none.
 *
 * A cut is a coordinate no cell crosses - every cell either ends at it or
 * starts at it - with at least one of each, and strictly inside the box. The
 * sweep finds those in one pass over the sorted edges rather than by testing
 * every coordinate against every cell: that is the difference between a drag
 * that stays instant on a few hundred cells and one that visibly stalls on a
 * few thousand. Vertical is tried first, matching how st_equal_grid builds a
 * grid. */
static int find_cut(const RectList *L, int lo, int hi, const RECT *box,
                    int *axis, int *at)
{
    int m = hi - lo, i, k, found = 0;

    if (m < 2) return 0;
    for (k = 0; k < 2 && !found; k++) {
        EdgeEv *ev = (EdgeEv *)xmalloc(sizeof(EdgeEv) * (size_t)(m * 2));
        int n = 0, open = 0;
        int blo = (k == 0) ? box->left : box->top;
        int bhi = (k == 0) ? box->right : box->bottom;
        if (!ev) return 0;
        for (i = lo; i < hi; i++) {
            int s = (k == 0) ? L->r[i].left  : L->r[i].top;
            int e = (k == 0) ? L->r[i].right : L->r[i].bottom;
            ev[n].at = s; ev[n].opens = 1; n++;
            ev[n].at = e; ev[n].opens = 0; n++;
        }
        qsort(ev, (size_t)n, sizeof(EdgeEv), edge_cmp);
        for (i = 0; i < n; ) {
            int x = ev[i].at, starts = 0, ends = 0;
            while (i < n && ev[i].at == x) {
                if (ev[i].opens) starts++; else ends++;
                i++;
            }
            /* Whatever is still open here runs across x, so the position is
             * only a cut when nothing does. */
            open -= ends;
            if (open == 0 && starts > 0 && ends > 0 && x > blo && x < bhi) {
                *axis = k; *at = x; found = 1; break;
            }
            open += starts;
        }
        xfree(ev);
    }
    return found;
}

/* Builds a tree over cells [lo, hi), which must tile `box` exactly. NULL means
 * the cells are not a guillotine partition after all - the caller backs out
 * rather than install a tree whose regions would not tile. */
static Slice *rebuild_rects(RectList *L, int lo, int hi, const RECT *box)
{
    Slice *s;
    RECT b1, b2;
    Slice *k1, *k2;
    int axis, at, mid, k;

    if (hi <= lo) return NULL;
    if (hi - lo == 1)
        return region_new(L->r[lo].left, L->r[lo].top,
                          L->r[lo].right, L->r[lo].bottom);
    if (!find_cut(L, lo, hi, box, &axis, &at)) return NULL;

    s = region_new(box->left, box->top, box->right, box->bottom);
    if (!s) return NULL;

    /* Partitioned in place, so the two halves come out as contiguous runs and
     * neither the list nor the cells have to be copied. */
    mid = lo;
    for (k = lo; k < hi; k++) {
        int b = (axis == 0) ? L->r[k].right : L->r[k].bottom;
        if (b <= at) {
            RECT sw = L->r[mid];
            L->r[mid] = L->r[k];
            L->r[k] = sw;
            mid++;
        }
    }
    b1 = *box; b2 = *box;
    if (axis == 0) { b1.right = at; b2.left = at; }
    else           { b1.bottom = at; b2.top = at; }

    k1 = rebuild_rects(L, lo, mid, &b1);
    k2 = k1 ? rebuild_rects(L, mid, hi, &b2) : NULL;
    if (!k1 || !k2) {
        region_free(k1); region_free(k2); region_free(s);
        return NULL;
    }
    if (!region_own(s, k1)) {
        region_free(k1); region_free(k2); region_free(s);
        return NULL;
    }
    /* k1 belongs to s now, so freeing s disposes of it: only k2 is on hand. */
    if (!region_own(s, k2)) { region_free(s); region_free(k2); return NULL; }
    return s;
}

int st_move_line(SliceTree *t, int axis, int pos, int want,
                 int spanLo, int spanHi)
{
    LineMove M;
    RectList L;
    Slice **lv;
    Slice *fresh;
    RECT box;
    long long area = 0;
    int n = 0, i, moved = 0;

    L.r = NULL; L.n = 0; L.cap = 0;
    line_prepare(t, &M, axis, pos, want, spanLo, spanHi);
    if (M.want == M.pos) return 0;

    lv = st_leaves(t, &n);
    if (!lv) return 0;
    for (i = 0; i < n; i++) {
        RECT nr;
        int drop = 0, what = line_effect(&lv[i]->r, &M, &drop, &nr);
        if (what == 2) { moved++; continue; }   /* travelled past it */
        if (what == 1) moved++;
        if (!rectlist_push(&L, nr)) {
            xfree(lv);
            rectlist_free(&L);
            return 0;
        }
    }
    xfree(lv);

    /* The new cells are only swapped in once they have been shown to add up to
     * the whole picture. That is the one thing the arithmetic above could have
     * got wrong, and it is cheap to check - a set that does not add up is not
     * installed, so a request that cannot be honoured leaves the tree exactly
     * as it was instead of exporting the wrong pixels. */
    for (i = 0; i < L.n; i++) {
        if (L.r[i].left < 0 || L.r[i].top < 0 ||
            L.r[i].right > t->imgW || L.r[i].bottom > t->imgH) {
            rectlist_free(&L);
            return 0;
        }
        area += (long long)(L.r[i].right - L.r[i].left) *
                (L.r[i].bottom - L.r[i].top);
    }
    if (L.n < 1 || area != (long long)t->imgW * t->imgH) {
        rectlist_free(&L);
        return 0;
    }

    SetRect(&box, 0, 0, t->imgW, t->imgH);
    fresh = rebuild_rects(&L, 0, L.n, &box);
    rectlist_free(&L);
    if (!fresh) return 0;

    region_free(t->root);
    t->root = fresh;
    return moved;
}

/* ------------------------------------------------------------------ */
/* merging                                                             */
/* ------------------------------------------------------------------ */

int st_rect_cells(const SliceTree *t, RECT r, int *aligned)
{
    Slice **lv;
    int n = 0, i, inside = 0;
    long long area = 0;

    if (aligned) *aligned = 0;
    if (!t || !t->root) return 0;
    if (r.right <= r.left || r.bottom <= r.top) return 0;
    lv = st_leaves(t, &n);
    if (!lv) return 0;
    for (i = 0; i < n; i++) {
        const RECT *q = &lv[i]->r;
        if (q->left >= r.right || q->right <= r.left ||
            q->top >= r.bottom || q->bottom <= r.top) continue;
        if (q->left < r.left || q->top < r.top ||
            q->right > r.right || q->bottom > r.bottom) {
            /* A cell half inside the box. Half a cell is not a slice, and the
             * border that would be drawn across it cuts an exported file in
             * two, so this is the one thing that makes a merge illegal. */
            xfree(lv);
            return 0;
        }
        area += (long long)(q->right - q->left) * (q->bottom - q->top);
        inside++;
    }
    xfree(lv);
    if (area != (long long)(r.right - r.left) * (r.bottom - r.top)) return 0;
    if (aligned) *aligned = 1;
    return inside;
}

/* Clears every region inside `r` so the box comes out as one cell.
 *
 * A region whose own rectangle is already wholly inside the box is dropped
 * whole. That is the case a box drawn around a subdivided cell hits, and it is
 * why merging a cell that was cut into 2x2 gives back one slice rather than a
 * flat list of four - and why the pixels come back as a single region instead
 * of four siblings with no parent to divide them again. */
static int merge_walk(Slice *s, const RECT *r)
{
    int i, took = 0;
    if (!s) return 0;
    if (s->r.left >= r->right || s->r.right <= r->left ||
        s->r.top >= r->bottom || s->r.bottom <= r->top) return 0;
    if (s->r.left >= r->left && s->r.top >= r->top &&
        s->r.right <= r->right && s->r.bottom <= r->bottom) {
        took = st_count_at(s);
        region_clear(s);
        return took;
    }
    for (i = 0; i < s->nkid; i++) took += merge_walk(s->kid[i], r);
    return took;
}

int st_merge_rect(SliceTree *t, RECT r)
{
    int cells, aligned = 0, took;

    if (!t || !t->root) return 0;
    if (r.left < 0 || r.top < 0 || r.right > t->imgW || r.bottom > t->imgH)
        return 0;
    cells = st_rect_cells(t, r, &aligned);
    if (!aligned || cells < 2) return 0;
    took = merge_walk(t->root, &r);
    return took;
}

/* ------------------------------------------------------------------ */
/* whole-picture layouts                                               */
/* ------------------------------------------------------------------ */

int st_equal_cols(SliceTree *t, int n)
{
    if (!t || !t->root) return 0;
    st_reset(t);
    st_divide(t->root, 0, n);
    return st_count(t);
}

int st_equal_rows(SliceTree *t, int n)
{
    if (!t || !t->root) return 0;
    st_reset(t);
    st_divide(t->root, 1, n);
    return st_count(t);
}

int st_equal_grid(SliceTree *t, int cols, int rows)
{
    int i;
    if (!t || !t->root) return 0;
    st_reset(t);
    /* st_divide leaves a region undivided when it is asked for one part, and a
     * region with no children leaves nothing to put the rows into - so a single
     * column has to be divided the other way round. Without this "3 rows, 1
     * column" comes out as one undivided cell, which is what the window does on
     * every open whenever the column box still reads 1. */
    if (cols <= 1) {
        st_divide(t->root, 1, rows);
        return st_count(t);
    }
    st_divide(t->root, 0, cols);
    for (i = 0; i < t->root->nkid; i++) st_divide(t->root->kid[i], 1, rows);
    return st_count(t);
}

int st_fixed(SliceTree *t, int cellPx)
{
    int p, cuts = 0;
    if (!t || !t->root) return 0;
    if (cellPx < 1) cellPx = 1;
    st_reset(t);
    /* Routed through st_cut rather than a special case, so the fixed-size mode
     * and the hand-drawn line share one code path - and the leftover strip
     * falls out of it for free. The span of each line runs the full width of
     * the picture, because a fixed cell size has to reach every column. */
    for (p = cellPx; p < t->imgW; p += cellPx)
        cuts += st_cut(t, 0, p, 0, t->imgH);
    for (p = cellPx; p < t->imgH; p += cellPx)
        cuts += st_cut(t, 1, p, 0, t->imgW);
    (void)cuts;
    return st_count(t);
}

/* ------------------------------------------------------------------ */
/* deleting                                                            */
/* ------------------------------------------------------------------ */

static Slice *find_parent_of(Slice *node, const Slice *target)
{
    int i;
    for (i = 0; i < node->nkid; i++) {
        if (node->kid[i] == target) return node;
        if (!is_leaf(node->kid[i])) {
            Slice *p = find_parent_of(node->kid[i], target);
            if (p) return p;
        }
    }
    return NULL;
}

int st_delete(SliceTree *t, Slice *leaf)
{
    Slice *parent;
    if (!t || !t->root || !leaf || leaf == t->root) return 0;
    parent = find_parent_of(t->root, leaf);
    if (!parent) return 0;
    /* Refused for a direct child of the root. Handing those pixels back means
     * collapsing the whole picture, which is not what "delete this one slice"
     * means and would throw away every other slice on the canvas. Removing
     * just the one region instead would leave a hole, and the pixels in it
     * would belong to no export at all. The caller offers 重置 for the case
     * where the user really does want to go back to one slice. */
    if (parent == t->root) return 0;
    /* The containing region takes its whole area back as one piece, so nothing
     * is left uncovered. */
    region_clear(parent);
    return 1;
}

Slice *st_first_child(Slice *s)
{
    if (!s || s->nkid == 0) return NULL;
    return s->kid[0];
}

/* ------------------------------------------------------------------ */
/* naming                                                              */
/* ------------------------------------------------------------------ */

/* Windows silently drops trailing dots and spaces from a file name, and
 * Explorer refuses to open a name made only of dots. Both turn a slice into
 * something that looks like an unnamed file, so they are stripped here rather
 * than left to surprise the user after the export. */
static wchar_t *safe_stem(const wchar_t *prefix)
{
    static const wchar_t *bad = L"\\/:*?\"<>|";
    wchar_t out[128];
    const wchar_t *p = (prefix && *prefix) ? prefix : L"slice";
    size_t w = 0, i;
    int onlyDots = 1;

    for (i = 0; p[i] && w < 100; i++) {
        if (p[i] < 32 || wcschr(bad, p[i])) continue;
        out[w++] = p[i];
    }
    while (w > 0 && (out[w - 1] == L'.' || out[w - 1] == L' ')) w--;
    out[w] = 0;
    for (i = 0; i < w; i++) {
        if (out[i] != L'.') { onlyDots = 0; break; }
    }
    if (w == 0 || onlyDots) {
        wcsncpy(out, L"slice", 127);
        out[127] = 0;
    }
    return xwcsdup(out);
}

wchar_t *st_slice_name(const wchar_t *prefix, int index, int total, int asJpeg)
{
    wchar_t *stem = safe_stem(prefix);
    wchar_t num[16];
    int digits = 1, n, i, v = index + 1;
    WBuf b;

    /* Zero-pad to the width of the largest index (minimum 2) so the exported
     * files sort in export order in Explorer instead of 1, 10, 2, 3. */
    for (n = total; n >= 10; n /= 10) digits++;
    if (digits < 2) digits = 2;
    if (digits > 8) digits = 8;

    num[digits] = 0;
    for (i = digits - 1; i >= 0; i--) {
        num[i] = (wchar_t)(L'0' + (v % 10));
        v /= 10;
    }

    /* Built by hand rather than with a format string: this toolchain's wide
     * printf and its %s are a known sharp edge, and a file name is not worth
     * finding out about the hard way. */
    wbuf_init(&b);
    wbuf_add(&b, stem);
    wbuf_addc(&b, L'_');
    wbuf_add(&b, num);
    wbuf_addc(&b, L'.');
    wbuf_add(&b, asJpeg ? L"jpg" : L"png");
    {
        wchar_t *out = xwcsdup(b.buf);
        wbuf_free(&b);
        xfree(stem);
        return out;
    }
}
