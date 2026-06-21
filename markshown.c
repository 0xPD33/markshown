// markshown.c: a tiny GPU-accelerated live markdown viewer.
// raylib (GPU + text) + md4c (parse). Watches a file via inotify and hot-reloads.
//
//   nix shell nixpkgs#raylib nixpkgs#md4c
//   cc -O2 -o markshown markshown.c -lraylib -lmd4c -lm
//   ./markshown notes.md         # watch a file (inotify hot-reload)
//   some-cmd | markshown -        # or stream stdin live (LLM/agent/build output)
//
// keys:  wheel/j k/PgUp PgDn/g G/Home End scroll · +/- or ctrl+wheel zoom · / search (n/N) · o/Tab outline · ctrl+C copy · q quit.
// mouse: drag or double-click to select · click links/checkboxes/outline · hover a code block for its copy button.
// extras: column-aligned GFM tables, fenced-code syntax highlighting, local + remote images (via `curl`),
//         inline images, GitHub callouts, mermaid diagrams (via `mmdc`), nested blockquotes, raw HTML
//         (block verbatim + inline <br>), hard line breaks, numeric/named HTML entities, a change-glow that
//         flags edits on reload, and stream-robust partial rendering of half-written markdown.
// fonts are resolved at runtime via `fc-match`, so it picks up your fontconfig.

#include "raylib.h"
#include "md4c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>
#include <errno.h>
#include <ctype.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/wait.h>

// ── theme (tokyonight) ──────────────────────────────────────────────────────
static const Color BG     = {0x1a,0x1b,0x26,255};
static const Color FG     = {0xc0,0xca,0xf5,255};
static const Color MUTED  = {0x56,0x5f,0x89,255};
static const Color ACCENT = {0x7a,0xa2,0xf7,255};
static const Color LINKC  = {0x7d,0xcf,0xff,255};
static const Color CODEBG = {0x16,0x16,0x1e,255};
static const Color CODEFG = {0x9e,0xce,0x6a,255};
static const Color ICODEBG= {0x29,0x2e,0x42,255};
static const Color ICODEFG= {0x73,0xda,0xca,255};
static const Color RULE   = {0x29,0x2e,0x42,255};
static const Color SEL    = {0x3d,0x59,0xa1,90};    // translucent text-selection highlight
static const Color SRCH   = {0xe0,0xaf,0x68,70};    // search match
static const Color SRCHC  = {0xff,0x9e,0x3b,150};   // current search match
static const Color KWCOL  = {0xbb,0x9a,0xf7,255};   // code: keyword
static const Color NUMCOL = {0xff,0x9e,0x64,255};   // code: number
static const Color CDEF   = {0xa9,0xb1,0xd6,255};   // code: default identifier/punctuation

// ── inline run flags ────────────────────────────────────────────────────────
enum { F_BOLD=1, F_ITALIC=2, F_CODE=4, F_LINK=8, F_STRIKE=16, F_MUTED=32, F_IMG=64 };

typedef struct { char *text; uint8_t flags; char *href; } Run;

typedef struct { Run *runs; int run_n, run_cap; int align; } Cell;   // align: 0 left, 1 center, 2 right
typedef struct { Cell *cells; int cell_n, cell_cap; int is_header; } Row;

enum BType { B_P, B_H, B_CODE, B_HR, B_IMG, B_TABLE };
enum Marker { M_NONE, M_BULLET, M_NUM, M_CHECK, M_CHECKED };

typedef struct {
    int type, level, indent, marker, list_num, quote, is_row, is_header;
    Run  *runs; int run_n, run_cap;
    Row  *rows; int row_n, row_cap;      // B_TABLE: rows of cells
    char *code; size_t code_n, code_cap;
    char *lang;                          // fenced code language (for syntax highlighting), or NULL
    char *src;                           // image source path (B_IMG), or NULL
    size_t task_off;                     // byte offset of this item's [ ] mark in source
    double born;                         // GetTime() when this block first appeared (0 = old), for change-glow
    uint64_t sig;                        // content hash, for diffing reloads
    int streaming;                       // B_CODE with a synthesized close fence (still being written)
    int callout, callout_head;           // GitHub callout type (1..5, see callout_color) + first-block-of-group flag
} Block;

typedef struct {
    Block *bl; int n, cap, cur;          // -1 == none
    int bold, italic, code, strike, link;
    int in_code, skip, in_thead, first_cell, in_img;
    int in_table, tbl;                   // building a table; tbl = its block index
    struct { int ordered, num; } lstk[32]; int ld;
    int quote;
    int callout, callout_scan;           // active callout type for this blockquote; scanning its first text for [!TYPE]
    int li_pending, li_marker, li_num;
    char *cur_href; size_t li_task_off;  // active link target; pending checkbox source offset
} Doc;

// ── globals (fonts + scroll, to keep signatures short) ──────────────────────
static Font g_reg, g_bold, g_ital, g_mono;
static float g_scroll, g_vh;
static Vector2 g_mouse;   // mouse pos this frame, so render() can do hover affordances
static double g_now;      // GetTime() this frame, for time-based effects (change-glow)
static char g_docdir[4096];   // directory of the markdown file, for resolving relative image paths
static struct { char *src; Texture2D tex; int ok; } g_tc[64]; static int g_tcn;  // image cache (persists across reparses)
// per-frame clickable regions, filled during render(): links + task checkboxes
static struct { Rectangle r; const char *href; } g_links[2048]; static int g_link_n;
static struct { Rectangle r; size_t off;       } g_checks[512]; static int g_check_n;
// text selection in content coords (scroll-independent); flow() paints it, main loop drives it
static float g_sel_ax, g_sel_ay, g_sel_cx, g_sel_cy; static int g_has_sel;
static struct { float x, y, w, h; const char *t; int tn; } g_words[16384]; static int g_word_n;  // every word box (content coords) + source slice: powers copy, search, double-click. ponytail: cap drops the tail of huge docs
static struct { Rectangle r; const char *code; } g_codes[256]; static int g_code_n;  // code-block "copy" buttons (screen coords)
static double g_copy_flash;   // GetTime() of the last clipboard copy, for the toast
// search: 0=off, 1=typing, 2=results/navigate. matches collected post-render in content-y order.
static char g_query[128]; static int g_qn, g_search;
static float g_matches[4096]; static int g_match_n, g_match_i;
// mermaid: one render job per source hash. state 0=rendering 1=ready 2=failed. mmdc runs in a forked child.
static struct { uint64_t hash; pid_t pid; int state; Texture2D tex; } g_mm[64]; static int g_mmn;
static int g_have_mmdc = -1;   // -1 unknown, lazily probed
// TOC sidebar: headings collected during render; panel slides in from the left.
static struct { int level; float y; char text[80]; } g_head[256]; static int g_head_n;
static int g_toc; static float g_toc_anim, g_left;   // open flag · 0..1 slide · left margin render must avoid
static float g_col_x0, g_col_w, g_doc_end_y;         // render exposes the content column + doc end (for carets/minimap)
static struct { float y; double born; } g_glowmarks[256]; static int g_glow_n;   // change-minimap ticks
static float g_change_y = -1; static double g_change_at = 0;   // newest-changed block (content-y + born time), for auto jump-to-change

// ── doc building ────────────────────────────────────────────────────────────
static Block *newblock(Doc *d, int type) {
    if (d->n >= d->cap) { d->cap = d->cap ? d->cap*2 : 64; d->bl = realloc(d->bl, d->cap*sizeof(Block)); }
    Block *b = &d->bl[d->n++]; memset(b, 0, sizeof *b);
    b->type = type;
    b->indent = d->ld;
    b->quote  = d->quote;          // nesting depth (0 = none), for per-level indent + bars
    b->callout = d->callout;
    if (d->li_pending) { b->marker = d->li_marker; b->list_num = d->li_num; b->task_off = d->li_task_off; d->li_pending = 0; }
    d->cur = d->n - 1;
    return b;
}
static void addrun(Doc *d, const char *t, size_t n, uint8_t f) {
    Run **runs; int *run_n, *run_cap;
    if (d->in_table && d->tbl >= 0 && d->tbl < d->n) {   // route into the current table cell
        Block *tb = &d->bl[d->tbl];
        if (tb->row_n == 0) return;
        Row *row = &tb->rows[tb->row_n-1];
        if (row->cell_n == 0) return;
        Cell *cell = &row->cells[row->cell_n-1];
        runs = &cell->runs; run_n = &cell->run_n; run_cap = &cell->run_cap;
    } else {
        if (d->cur < 0) return;
        Block *b = &d->bl[d->cur];
        runs = &b->runs; run_n = &b->run_n; run_cap = &b->run_cap;
    }
    if (*run_n >= *run_cap) { *run_cap = *run_cap ? *run_cap*2 : 8; *runs = realloc(*runs, *run_cap*sizeof(Run)); }
    char *c = malloc(n+1); memcpy(c, t, n); c[n] = 0;
    char *h = d->cur_href ? strdup(d->cur_href) : NULL;   // link target for this run, if any
    (*runs)[(*run_n)++] = (Run){c, f, h};
}
// add an inline image as a run (empty text, F_IMG, href = image src). flows in flow(); a block
// whose only run is an image renders large (see render).
static void addimg(Doc *d, const char *src) {
    char *save = d->cur_href; d->cur_href = (char*)src;   // borrow; addrun strdup's it into the run's href
    addrun(d, "", 0, F_IMG);
    d->cur_href = save;
}
static void addcode(Doc *d, const char *t, size_t n) {
    if (d->cur < 0) return;
    Block *b = &d->bl[d->cur];
    if (b->code_n + n + 1 > b->code_cap) { b->code_cap = (b->code_cap + n + 1)*2; b->code = realloc(b->code, b->code_cap); }
    memcpy(b->code + b->code_n, t, n); b->code_n += n; b->code[b->code_n] = 0;
}
static void freedoc(Doc *d) {
    for (int i=0;i<d->n;i++) {
        Block *b = &d->bl[i];
        for (int r=0;r<b->run_n;r++) { free(b->runs[r].text); free(b->runs[r].href); } free(b->runs);
        for (int r=0;r<b->row_n;r++) {                       // free table rows/cells/runs
            Row *row = &b->rows[r];
            for (int c=0;c<row->cell_n;c++) { Cell *cell = &row->cells[c];
                for (int k=0;k<cell->run_n;k++) { free(cell->runs[k].text); free(cell->runs[k].href); } free(cell->runs); }
            free(row->cells);
        }
        free(b->rows); free(b->code); free(b->lang); free(b->src);
    }
    free(d->bl); free(d->cur_href);
    memset(d, 0, sizeof *d); d->cur = -1; d->tbl = -1;
}

// ── md4c callbacks ──────────────────────────────────────────────────────────
// raw copy of a link href. ponytail: doesn't decode &amp; etc, fine for real
// URLs in markdown; walk md4c substrings only if a link ever comes through wrong.
static char *href_dup(const MD_ATTRIBUTE *a) {
    char *s = malloc(a->size + 1);
    memcpy(s, a->text, a->size); s[a->size] = 0;
    return s;
}
static uint8_t curflags(Doc *d, int extra) {
    uint8_t f = 0;
    if (d->bold)   f |= F_BOLD;
    if (d->italic) f |= F_ITALIC;
    if (d->code || extra) f |= F_CODE;
    if (d->strike) f |= F_STRIKE;
    if (d->link)   f |= F_LINK;
    return f;
}
static int cb_enter_block(MD_BLOCKTYPE t, void *detail, void *u) {
    Doc *d = u;
    if (d->skip) { if (t==MD_BLOCK_TABLE) {} return 0; }
    switch (t) {
        case MD_BLOCK_H:  { newblock(d, B_H)->level = ((MD_BLOCK_H_DETAIL*)detail)->level; break; }
        case MD_BLOCK_P:  newblock(d, B_P); break;
        case MD_BLOCK_HR: newblock(d, B_HR); break;
        case MD_BLOCK_CODE: { Block *b = newblock(d, B_CODE); d->in_code = 1;
            MD_BLOCK_CODE_DETAIL *cd = detail; if (cd->lang.text && cd->lang.size) b->lang = href_dup(&cd->lang); break; }
        case MD_BLOCK_HTML: { Block *b = newblock(d, B_CODE); b->lang = strdup("html"); d->in_code = 1; break; }   // raw HTML block → show verbatim
        case MD_BLOCK_QUOTE: if (d->quote == 0) { d->callout_scan = 1; d->callout = 0; } d->quote++; break;
        case MD_BLOCK_UL: d->lstk[d->ld] = (typeof(d->lstk[0])){0,0}; d->ld++; break;
        case MD_BLOCK_OL: d->lstk[d->ld] = (typeof(d->lstk[0])){1, (int)((MD_BLOCK_OL_DETAIL*)detail)->start}; d->ld++; break;
        case MD_BLOCK_LI: {
            MD_BLOCK_LI_DETAIL *li = detail;
            d->li_pending = 1;
            d->li_task_off = li->is_task ? (size_t)li->task_mark_offset : 0;
            if (li->is_task) d->li_marker = (li->task_mark=='x'||li->task_mark=='X') ? M_CHECKED : M_CHECK;
            else if (d->ld>0 && d->lstk[d->ld-1].ordered) { d->li_marker = M_NUM; d->li_num = d->lstk[d->ld-1].num++; }
            else d->li_marker = M_BULLET;
            break;
        }
        case MD_BLOCK_TABLE: { newblock(d, B_TABLE); d->in_table = 1; d->tbl = d->n - 1; break; }
        case MD_BLOCK_THEAD: d->in_thead = 1; break;
        case MD_BLOCK_TBODY: d->in_thead = 0; break;
        case MD_BLOCK_TR: if (d->in_table && d->tbl >= 0) {       // append a row to the table
                Block *tb = &d->bl[d->tbl];
                if (tb->row_n >= tb->row_cap) { tb->row_cap = tb->row_cap ? tb->row_cap*2 : 8; tb->rows = realloc(tb->rows, tb->row_cap*sizeof(Row)); }
                tb->rows[tb->row_n] = (Row){0}; tb->rows[tb->row_n].is_header = d->in_thead; tb->row_n++;
            } break;
        case MD_BLOCK_TH: case MD_BLOCK_TD: if (d->in_table && d->tbl >= 0) {   // append a cell to the current row
                Block *tb = &d->bl[d->tbl];
                if (tb->row_n == 0) break;
                Row *row = &tb->rows[tb->row_n-1];
                if (row->cell_n >= row->cell_cap) { row->cell_cap = row->cell_cap ? row->cell_cap*2 : 8; row->cells = realloc(row->cells, row->cell_cap*sizeof(Cell)); }
                MD_ALIGN a = ((MD_BLOCK_TD_DETAIL*)detail)->align;
                row->cells[row->cell_n] = (Cell){0};
                row->cells[row->cell_n].align = (a==MD_ALIGN_CENTER) ? 1 : (a==MD_ALIGN_RIGHT) ? 2 : 0;
                row->cell_n++;
            } break;
        default: break;
    }
    return 0;
}
static int cb_leave_block(MD_BLOCKTYPE t, void *detail, void *u) {
    (void)detail; Doc *d = u;
    switch (t) {
        case MD_BLOCK_CODE: case MD_BLOCK_HTML: d->in_code = 0; break;
        case MD_BLOCK_TABLE: d->in_table = 0; d->tbl = -1; break;
        case MD_BLOCK_QUOTE: d->quote--; if (d->quote == 0) { d->callout = 0; d->callout_scan = 0; } break;
        case MD_BLOCK_UL: case MD_BLOCK_OL: if (d->ld>0) d->ld--; break;
        default: break;
    }
    return 0;
}
static int cb_enter_span(MD_SPANTYPE t, void *detail, void *u) {
    Doc *d = u;
    switch (t) {
        case MD_SPAN_STRONG: d->bold++; break;
        case MD_SPAN_EM:     d->italic++; break;
        case MD_SPAN_CODE:   d->code++; break;
        case MD_SPAN_DEL:    d->strike++; break;
        case MD_SPAN_A:      d->link++; d->cur_href = href_dup(&((MD_SPAN_A_DETAIL*)detail)->href); break;
        case MD_SPAN_IMG:    { MD_SPAN_IMG_DETAIL *im = detail;
                               if (d->li_pending) newblock(d, B_P);              // image-only list item: open its block
                               if (im->src.text && im->src.size) { char *s = href_dup(&im->src); addimg(d, s); free(s); }
                               d->in_img = 1; break; }
        default: break;
    }
    return 0;
}
static int cb_leave_span(MD_SPANTYPE t, void *detail, void *u) {
    (void)detail; Doc *d = u;
    switch (t) {
        case MD_SPAN_STRONG: d->bold--; break;
        case MD_SPAN_EM:     d->italic--; break;
        case MD_SPAN_CODE:   d->code--; break;
        case MD_SPAN_DEL:    d->strike--; break;
        case MD_SPAN_A:      d->link--; free(d->cur_href); d->cur_href = NULL; break;
        case MD_SPAN_IMG:    d->in_img = 0; break;
        default: break;
    }
    return 0;
}
// GitHub callout marker: "[!NOTE]" etc. returns 1..5 (see callout_color) or 0.
static int match_callout(const char *s, size_t n) {
    if (n < 5 || s[0] != '[' || s[1] != '!') return 0;
    size_t i = 2; while (i < n && s[i] != ']') i++;
    if (i >= n) return 0;
    size_t tl = i - 2; const char *t = s + 2;
    static const char *nm[] = {"NOTE","TIP","IMPORTANT","WARNING","CAUTION"};
    for (int k = 0; k < 5; k++) if (strlen(nm[k]) == tl && !strncasecmp(t, nm[k], tl)) return k + 1;
    return 0;
}
static int utf8_encode(unsigned cp, char *o) {
    if (cp < 0x80)    { o[0]=cp; return 1; }
    if (cp < 0x800)   { o[0]=0xC0|(cp>>6);  o[1]=0x80|(cp&0x3F); return 2; }
    if (cp < 0x10000) { o[0]=0xE0|(cp>>12); o[1]=0x80|((cp>>6)&0x3F); o[2]=0x80|(cp&0x3F); return 3; }
    o[0]=0xF0|(cp>>18); o[1]=0x80|((cp>>12)&0x3F); o[2]=0x80|((cp>>6)&0x3F); o[3]=0x80|(cp&0x3F); return 4;
}
// decode an HTML entity (&name; / &#123; / &#xAB;) into UTF-8 bytes in `out`; returns byte count, or 0 if unknown.
static int decode_entity(const char *s, size_t n, char *out) {
    if (n < 4 || s[0] != '&' || s[n-1] != ';') return 0;
    if (s[1] == '#') {                                   // numeric
        unsigned cp = 0;
        if (s[2]=='x' || s[2]=='X') for (size_t i=3;i<n-1;i++){ char c=s[i]; cp=cp*16+(c<='9'?c-'0':((c|32)-'a'+10)); }
        else                        for (size_t i=2;i<n-1;i++) cp=cp*10+(s[i]-'0');
        if (cp == 0 || cp > 0x10FFFF) return 0;
        return utf8_encode(cp, out);
    }
    static const struct { const char *name; unsigned cp; } tbl[] = {   // common named entities (codepoints in the loaded atlas)
        {"amp",'&'},{"lt",'<'},{"gt",'>'},{"quot",'"'},{"apos",'\''},{"nbsp",' '},
        {"copy",0xA9},{"reg",0xAE},{"trade",0x2122},{"mdash",0x2014},{"ndash",0x2013},
        {"hellip",0x2026},{"ldquo",0x201C},{"rdquo",0x201D},{"lsquo",0x2018},{"rsquo",0x2019},
        {"times",0xD7},{"divide",0xF7},{"deg",0xB0},{"plusmn",0xB1},{"micro",0xB5},{"middot",0xB7},
        {"para",0xB6},{"sect",0xA7},{"bull",0x2022},{"dagger",0x2020},{"Dagger",0x2021},
        {"larr",0x2190},{"uarr",0x2191},{"rarr",0x2192},{"darr",0x2193},{"harr",0x2194},
        {"laquo",0xAB},{"raquo",0xBB},{"frac12",0xBD},{"frac14",0xBC},{"frac34",0xBE},
        {"euro",0x20AC},{"pound",0xA3},{"cent",0xA2},{"yen",0xA5},{"infin",0x221E},{"ne",0x2260},
        {"le",0x2264},{"ge",0x2265},{"hellip",0x2026},
    };
    for (size_t k=0;k<sizeof tbl/sizeof tbl[0];k++) {
        size_t L = strlen(tbl[k].name);
        if (L == n-2 && !memcmp(s+1, tbl[k].name, L)) return utf8_encode(tbl[k].cp, out);
    }
    return 0;
}
static int cb_text(MD_TEXTTYPE tt, const MD_CHAR *txt, MD_SIZE sz, void *u) {
    Doc *d = u;
    if (d->skip || d->in_img) return 0;   // skip image alt text
    if (d->in_code) { addcode(d, txt, sz); return 0; }   // also collects block HTML (in_code set on MD_BLOCK_HTML)
    if (d->li_pending) newblock(d, B_P);   // tight list items have no <p> wrapper; open the block now
    if (d->callout_scan && d->quote > 0 && tt == MD_TEXT_NORMAL) {  // first text of a blockquote: is it a callout?
        d->callout_scan = 0;
        int ct = match_callout(txt, sz);
        if (ct) {
            d->callout = ct;
            if (d->cur >= 0) { d->bl[d->cur].callout = ct; d->bl[d->cur].callout_head = 1; }
            size_t j = 2; while (j < sz && txt[j] != ']') j++; if (j < sz) j++;   // drop "[!TYPE]"
            while (j < sz && txt[j] == ' ') j++;                                  // keep any same-line title text
            if (j < sz) addrun(d, txt + j, sz - j, curflags(d, 0));
            return 0;
        }
    }
    if (tt == MD_TEXT_SOFTBR) { addrun(d, " ", 1, 0); return 0; }
    if (tt == MD_TEXT_BR)     { addrun(d, "\n", 1, 0); return 0; }   // hard break (two trailing spaces / backslash) → real line break
    if (tt == MD_TEXT_HTML) {                                        // inline HTML: <br> breaks the line, other tags drop (their text still arrives)
        if (sz >= 3 && txt[0]=='<' && (txt[1]|32)=='b' && (txt[2]|32)=='r') addrun(d, "\n", 1, 0);
        return 0;
    }
    if (tt == MD_TEXT_ENTITY) {
        char buf[8]; int n = decode_entity(txt, sz, buf);
        if (n) { addrun(d, buf, n, curflags(d,0)); return 0; }
        // unknown entity → fall through, render raw
    }
    addrun(d, txt, sz, curflags(d, tt == MD_TEXT_CODE && !d->in_code));
    return 0;
}

// content hash of a block (type + structure + text), for reload diffing.
static uint64_t block_hash(Block *b) {
    uint64_t h = 1469598103934665603ULL;   // FNV-1a
    #define MIX(x) h = (h ^ (uint64_t)(x)) * 1099511628211ULL
    MIX(b->type); MIX(b->level); MIX(b->marker); MIX(b->indent); MIX(b->is_header); MIX(b->callout);
    for (int r = 0; r < b->run_n; r++) { for (const char *p = b->runs[r].text; *p; p++) MIX((uint8_t)*p);
        if (b->runs[r].href) for (const char *p = b->runs[r].href; *p; p++) MIX((uint8_t)*p); }   // links/images
    for (int r = 0; r < b->row_n; r++) for (int c = 0; c < b->rows[r].cell_n; c++)   // table cells
        for (int k = 0; k < b->rows[r].cells[c].run_n; k++) for (const char *p = b->rows[r].cells[c].runs[k].text; *p; p++) MIX((uint8_t)*p);
    if (b->code) for (const char *p = b->code; *p; p++) MIX((uint8_t)*p);
    if (b->src)  for (const char *p = b->src;  *p; p++) MIX((uint8_t)*p);
    #undef MIX
    return h;
}
// Streaming-robustness: a mid-stream file can end inside an open ``` fence, which
// makes md4c swallow the rest of the doc as code. Count fence toggles; if one is
// left open, return a copy with a synthetic closing fence so the tail parses as
// prose. Returns NULL (use src as-is) when nothing needs fixing.
static char *sanitize(const char *src, size_t len, size_t *outlen, int *synth) {
    int open = 0; char fch = '`'; int flen = 0;
    for (size_t i = 0; i < len; ) {
        int sp = 0; while (i < len && src[i] == ' ' && sp < 3) { i++; sp++; }   // up to 3 leading spaces
        if (i + 2 < len && ((src[i]=='`'&&src[i+1]=='`'&&src[i+2]=='`') ||
                            (src[i]=='~'&&src[i+1]=='~'&&src[i+2]=='~'))) {
            char ch = src[i]; int n = 0; while (i < len && src[i] == ch) { i++; n++; }
            if (!open) { open = 1; fch = ch; flen = n; }
            else if (ch == fch && n >= flen) open = 0;   // matching close; mismatched fences are content
        }
        while (i < len && src[i] != '\n') i++;
        if (i < len) i++;                                                        // to next line
    }
    if (!open) { *outlen = len; *synth = 0; return NULL; }
    *synth = 1;
    char *buf = malloc(len + 8); memcpy(buf, src, len); size_t p = len;
    if (p && buf[p-1] != '\n') buf[p++] = '\n';
    for (int k = 0; k < flen && k < 6; k++) buf[p++] = fch;
    buf[p++] = '\n'; *outlen = p; return buf;
}
static void parse(Doc *d, const char *src, size_t len) {
    freedoc(d);
    MD_PARSER p = {0};
    p.flags = MD_DIALECT_GITHUB;
    p.enter_block = cb_enter_block; p.leave_block = cb_leave_block;
    p.enter_span  = cb_enter_span;  p.leave_span  = cb_leave_span;
    p.text = cb_text;
    size_t plen; int synth = 0;
    char *clean = sanitize(src, len, &plen, &synth);
    md_parse(clean ? clean : src, (MD_SIZE)plen, &p, d);
    free(clean);
    if (synth) for (int i = d->n - 1; i >= 0; i--) if (d->bl[i].type == B_CODE) { d->bl[i].streaming = 1; break; }

    // ── change-glow diff: a block whose content wasn't in the previous parse is "new" ──
    static uint64_t oldsig[8192]; static int oldn = 0, first = 1;
    double now = GetTime();
    for (int i = 0; i < d->n; i++) {
        uint64_t hh = block_hash(&d->bl[i]); d->bl[i].sig = hh;
        int seen = 0; for (int k = 0; k < oldn; k++) if (oldsig[k] == hh) { seen = 1; break; }
        d->bl[i].born = (first || seen) ? 0.0 : now;   // glow only genuinely-new blocks, and never on first open
    }
    oldn = 0; for (int i = 0; i < d->n && oldn < 8192; i++) oldsig[oldn++] = d->bl[i].sig;
    first = 0;
}

// ── rendering ───────────────────────────────────────────────────────────────
static Font fontfor(uint8_t f) {
    if (f & F_CODE) return g_mono;
    if (f & F_BOLD) return g_bold;
    if (f & F_ITALIC) return g_ital;
    return g_reg;
}
static Color colfor(uint8_t f, Color base) {
    if (f & F_CODE) return ICODEFG;
    if (f & F_MUTED) return MUTED;
    if (f & F_LINK) return LINKC;
    return base;
}
// GitHub callout type color + label (tokyonight): note·tip·important·warning·caution.
static Color callout_color(int t) {
    switch (t) {
        case 1: return (Color){0x7a,0xa2,0xf7,255};   // note     blue
        case 2: return (Color){0x9e,0xce,0x6a,255};   // tip      green
        case 3: return (Color){0xbb,0x9a,0xf7,255};   // important purple
        case 4: return (Color){0xe0,0xaf,0x68,255};   // warning  yellow
        default:return (Color){0xf7,0x76,0x8e,255};   // caution  red
    }
}
static const char *callout_name(int t) {
    static const char *n[] = {"", "NOTE", "TIP", "IMPORTANT", "WARNING", "CAUTION"};
    return (t >= 1 && t <= 5) ? n[t] : "";
}

// paints the selection highlight for one finished line spanning [xl,xr] at content-y lineY.
static void sel_line(float xl, float xr, float lineY, float lh, float size) {
    if (!g_has_sel) return;
    float p0x, p0y, p1x, p1y;                       // order the two endpoints top→bottom
    if (g_sel_ay <= g_sel_cy) { p0x=g_sel_ax; p0y=g_sel_ay; p1x=g_sel_cx; p1y=g_sel_cy; }
    else                      { p0x=g_sel_cx; p0y=g_sel_cy; p1x=g_sel_ax; p1y=g_sel_ay; }
    if (!(lineY + lh > p0y && lineY < p1y)) return; // line outside the selection band
    int hasP0 = (p0y >= lineY && p0y < lineY + lh);
    int hasP1 = (p1y >= lineY && p1y < lineY + lh);
    float lo, hi;
    if (hasP0 && hasP1) { lo = fminf(p0x, p1x); hi = fmaxf(p0x, p1x); }  // both ends on this line
    else if (hasP0)     { lo = p0x; hi = xr; }                          // first line of the selection
    else if (hasP1)     { lo = xl;  hi = p1x; }                         // last line
    else                { lo = xl;  hi = xr; }                          // fully-enclosed line
    if (lo < xl) lo = xl;
    if (hi > xr) hi = xr;
    float h = size * 1.35f;                          // hug the glyphs, not the full line box
    float sy = lineY + (size - h) * 0.5f - g_scroll;
    if (hi > lo && sy + h >= 0 && sy <= g_vh)
        DrawRectangle((int)lo, (int)sy, (int)(hi - lo), (int)h, SEL);
}

static Texture2D *tex_get(const char *src);   // forward decl (inline images flow before the cache is defined)
// flows a block's runs, word-wrapping across mixed fonts. y is absolute; we
// subtract g_scroll only when drawing, and cull anything off-screen.
static void flow(Block *b, float x0, float colw, float size, Color base, uint8_t addf, float lh, float *y) {
    float x = x0, sp = MeasureTextEx(g_reg, " ", size, 0).x;
    float lineY = *y;                                // content-y of the line currently being filled
    char w[2048];
    for (int r = 0; r < b->run_n; r++) {
        Run *run = &b->runs[r];
        uint8_t fl = run->flags | addf;
        Font fn = fontfor(fl); Color col = colfor(fl, base);
        if (fl & F_IMG) {                                // inline image: flow it like a (tall) word
            Texture2D *t = tex_get(run->href);
            float ih = lh, iw = t ? ih * (float)t->width / t->height : ih * 1.6f;
            if (iw > colw) iw = colw;
            if (x > x0 && x + iw > x0 + colw) { sel_line(x0, x, lineY, lh, size); *y += lh; x = x0; lineY = *y; }
            float sy = *y - g_scroll;
            if (sy + ih >= 0 && sy <= g_vh) {
                if (t) DrawTexturePro(*t, (Rectangle){0,0,t->width,t->height}, (Rectangle){x, sy, iw, ih}, (Vector2){0,0}, 0, WHITE);
                else DrawRectangleRounded((Rectangle){x, sy, iw, ih}, 0.15f, 4, ICODEBG);   // loading / missing
            }
            x += iw + sp;
            continue;
        }
        const char *s = run->text; int i = 0;
        while (s[i]) {
            if (s[i]==' ' || s[i]=='\t') { while (s[i]==' '||s[i]=='\t') i++; if (x>x0) x += sp; continue; }
            if (s[i]=='\n') { sel_line(x0, x, lineY, lh, size); i++; *y += lh; x = x0; lineY = *y; continue; }
            int j = 0;
            while (s[i] && s[i]!=' ' && s[i]!='\t' && s[i]!='\n' && j < (int)sizeof(w)-1) w[j++] = s[i++];
            w[j] = 0;
            float ww = MeasureTextEx(fn, w, size, 0).x;
            if (x > x0 && x + ww > x0 + colw) { sel_line(x0, x, lineY, lh, size); *y += lh; x = x0; lineY = *y; }
            if (g_word_n < 16384) {   // every word (even off-screen) so copy/search/double-click can reach it
                g_words[g_word_n].x = x; g_words[g_word_n].y = *y; g_words[g_word_n].w = ww; g_words[g_word_n].h = lh;
                g_words[g_word_n].t = s + (i - j); g_words[g_word_n].tn = j; g_word_n++;
            }
            float sy = *y - g_scroll;
            if (sy + size >= 0 && sy <= g_vh) {
                if (fl & F_CODE) { DrawRectangleRounded((Rectangle){x-3, sy-2, ww+6, size+4}, 0.3f, 4, ICODEBG); DrawTextEx(fn, w, (Vector2){x, sy}, size, 0, col); }
                else { DrawTextEx(fn, w, (Vector2){x, sy}, size, 0, col); if (fl & F_LINK) DrawLineEx((Vector2){x, sy+size}, (Vector2){x+ww, sy+size}, 1, col); }
                if (fl & F_STRIKE) DrawLineEx((Vector2){x, sy+size*0.55f}, (Vector2){x+ww, sy+size*0.55f}, 1.5f, col);
                if (run->href && g_link_n < 2048) { g_links[g_link_n].r = (Rectangle){x, sy, ww, size}; g_links[g_link_n].href = run->href; g_link_n++; }
            }
            x += ww;
        }
    }
    sel_line(x0, x, lineY, lh, size);
    *y += lh;
}

// ── syntax highlighting (one generic lexer for all C-ish/script languages) ──
static int is_keyword(const char *s, int n) {
    static const char *kw[] = {
        "if","else","elif","for","while","do","switch","case","break","continue","return","goto",
        "function","func","def","fn","class","struct","enum","union","interface","impl","trait",
        "let","const","var","val","int","long","float","double","char","void","bool","auto","unsigned",
        "signed","short","static","inline","extern","public","private","protected","new","delete",
        "try","catch","finally","throw","throws","import","from","include","require","package","use",
        "using","namespace","async","await","yield","lambda","pass","with","as","in","is","not","and",
        "or","self","this","super","extends","implements","override","final","abstract","export",
        "default","match","when","where","module","type","typedef","sizeof","true","false","null",
        "nil","None","True","False","print","echo","end","then","begin","local","function", NULL };
    for (int k = 0; kw[k]; k++) if ((int)strlen(kw[k]) == n && !strncmp(kw[k], s, n)) return 1;
    return 0;
}
static int comment_style(const char *lang) {   // 0: // + /* */ ;  1: # ;  2: --
    if (!lang) return 0;
    const char *hash[] = {"python","py","sh","bash","shell","zsh","ruby","rb","yaml","yml","toml","r",
        "perl","pl","makefile","make","dockerfile","docker","ini","conf","cfg","nix","elixir","ex",
        "julia","jl","tcl","awk","fish","cmake","gitignore","env", NULL};
    const char *dash[] = {"sql","lua","haskell","hs","elm","ada","sql","vhdl","applescript", NULL};
    for (int i = 0; hash[i]; i++) if (!strcasecmp(lang, hash[i])) return 1;
    for (int i = 0; dash[i]; i++) if (!strcasecmp(lang, dash[i])) return 2;
    return 0;
}
// draw one code line, colored. *inblock tracks /* */ across lines.
static void hl_line(const char *s, int len, Font fn, float x, float y, float fs, int cm, int *inblock) {
    float cw = MeasureTextEx(fn, "0", fs, 0).x;   // monospace: advance by char width
    char buf[4096]; int i = 0;
    while (i < len) {
        int start = i; Color c;
        if (*inblock) {                                                   // continuing a /* */ block
            while (i < len && !(s[i]=='*' && i+1<len && s[i+1]=='/')) i++;
            if (i+1 < len && s[i]=='*' && s[i+1]=='/') { i += 2; *inblock = 0; } else i = len;
            c = MUTED;
        } else if (cm==0 && s[i]=='/' && i+1<len && s[i+1]=='/') { i = len; c = MUTED; }
        else if (cm==0 && s[i]=='/' && i+1<len && s[i+1]=='*') {
            i += 2; while (i < len && !(s[i]=='*' && i+1<len && s[i+1]=='/')) i++;
            if (i+1 < len && s[i]=='*' && s[i+1]=='/') i += 2; else { *inblock = 1; i = len; }
            c = MUTED;
        } else if (cm==1 && s[i]=='#') { i = len; c = MUTED; }
        else if (cm==2 && s[i]=='-' && i+1<len && s[i+1]=='-') { i = len; c = MUTED; }
        else if (s[i]=='"' || s[i]=='\'' || s[i]=='`') {                  // string literal
            char q = s[i++]; while (i < len && s[i]!=q) { if (s[i]=='\\' && i+1<len) i++; i++; } if (i < len) i++;
            c = CODEFG;
        } else if (isdigit((unsigned char)s[i])) {                        // number
            while (i < len && (isalnum((unsigned char)s[i]) || s[i]=='.' || s[i]=='_')) i++;
            c = NUMCOL;
        } else if (isalpha((unsigned char)s[i]) || s[i]=='_') {           // identifier / keyword
            while (i < len && (isalnum((unsigned char)s[i]) || s[i]=='_')) i++;
            c = is_keyword(s+start, i-start) ? KWCOL : CDEF;
        } else { i++; c = CDEF; }                                         // operator / punctuation
        int n = i - start; if (n > 4095) n = 4095; memcpy(buf, s+start, n); buf[n] = 0;
        DrawTextEx(fn, buf, (Vector2){x, y}, fs, 0, c);
        x += n * cw;
    }
}

// image texture cache, keyed by src. local files load immediately; remote (http) are fetched in the
// background by curl (dl_start) and resolved into this cache by img_poll(). ponytail: never evicts, 64-image ceiling.
static int dl_start(const char *src);   // forward decl (defined after src_hash)
static Texture2D *tex_get(const char *src) {
    if (!src) return NULL;
    for (int i = 0; i < g_tcn; i++) if (!strcmp(g_tc[i].src, src)) return g_tc[i].ok ? &g_tc[i].tex : NULL;
    if (strncmp(src, "http", 4) == 0) { dl_start(src); return NULL; }   // remote → kick off download, placeholder until ready
    if (g_tcn >= 64) return NULL;
    int ok = 0; Texture2D t = {0};
    char full[8192];
    if (src[0] == '/') snprintf(full, sizeof full, "%s", src);
    else snprintf(full, sizeof full, "%s/%s", g_docdir, src);
    t = LoadTexture(full);
    if (t.id) { GenTextureMipmaps(&t); SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR); ok = 1; }
    g_tc[g_tcn].src = strdup(src); g_tc[g_tcn].tex = t; g_tc[g_tcn].ok = ok; g_tcn++;
    return ok ? &g_tc[g_tcn-1].tex : NULL;
}

static uint64_t src_hash(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 1099511628211ULL;
    return h;
}

// ── remote images: curl-fetch http(s) URLs into /tmp in the background, then load like a local file ──
static int have_curl(void) { static int v = -1; if (v < 0) v = (system("command -v curl >/dev/null 2>&1") == 0); return v; }
static struct { char *src; char path[256]; pid_t pid; int done; } g_dl[64]; static int g_dln;
static int dl_find(const char *src) { for (int i = 0; i < g_dln; i++) if (!strcmp(g_dl[i].src, src)) return i; return -1; }
static void url_ext(const char *src, char *ext, size_t cap) {   // ".png"/".jpg"… from the URL (before ?/#), default .png
    const char *q = strpbrk(src, "?#"); size_t len = q ? (size_t)(q - src) : strlen(src);
    size_t dot = 0; for (size_t i = 0; i < len; i++) if (src[i] == '.') dot = i;
    if (dot && len - dot < cap && len - dot <= 6) { memcpy(ext, src + dot, len - dot); ext[len - dot] = 0; }
    else snprintf(ext, cap, ".png");
}
static int dl_start(const char *src) {                          // idempotent: starts at most one curl per URL
    if (dl_find(src) >= 0) return 1;
    if (!have_curl() || g_dln >= 64) return 0;
    int i = g_dln++; g_dl[i].src = strdup(src); g_dl[i].pid = 0; g_dl[i].done = 0;
    char ext[16]; url_ext(src, ext, sizeof ext);
    snprintf(g_dl[i].path, sizeof g_dl[i].path, "/tmp/markshown-img-%016llx%s", (unsigned long long)src_hash(src, strlen(src)), ext);
    pid_t pid = fork();
    if (pid == 0) { int dn = open("/dev/null", O_WRONLY); if (dn >= 0) { dup2(dn,1); dup2(dn,2); }
        execlp("curl", "curl", "-sL", "--max-time", "20", "-o", g_dl[i].path, src, (char*)NULL); _exit(127); }
    g_dl[i].pid = pid; return 1;
}
// reap finished curl downloads and load the file into the texture cache. MUST run outside Begin/EndDrawing.
static void img_poll(void) {
    for (int i = 0; i < g_dln; i++) {
        if (g_dl[i].done) continue;
        int st; if (waitpid(g_dl[i].pid, &st, WNOHANG) != g_dl[i].pid) continue;
        g_dl[i].done = 1;
        Texture2D t = (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? LoadTexture(g_dl[i].path) : (Texture2D){0};
        if (t.id) { GenTextureMipmaps(&t); SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR); }
        if (g_tcn < 64) { g_tc[g_tcn].src = strdup(g_dl[i].src); g_tc[g_tcn].tex = t; g_tc[g_tcn].ok = (t.id != 0); g_tcn++; }   // resolve into tex cache
    }
}

// ── mermaid: shell out to `mmdc`, render to PNG in the background, cache by source hash ──
static int have_mmdc(void) {
    if (g_have_mmdc < 0) g_have_mmdc = (system("command -v mmdc >/dev/null 2>&1") == 0);
    return g_have_mmdc;
}
static void mm_paths(uint64_t h, char *mmd, char *png, size_t cap) {
    snprintf(mmd, cap, "/tmp/markshown-mm-%016llx.mmd", (unsigned long long)h);
    snprintf(png, cap, "/tmp/markshown-mm-%016llx.png", (unsigned long long)h);
}
static int mm_find(uint64_t h) { for (int i = 0; i < g_mmn; i++) if (g_mm[i].hash == h) return i; return -1; }
// fork mmdc for source `s`; returns slot index, or -1 if the table is full. ponytail: 64-job ceiling, no eviction.
static int mm_start(uint64_t h, const char *s, size_t n) {
    if (g_mmn >= 64) return -1;
    char mmd[256], png[256]; mm_paths(h, mmd, png, sizeof mmd);
    int slot = g_mmn++; g_mm[slot] = (typeof(g_mm[0])){.hash = h, .state = 0};
    FILE *f = fopen(mmd, "wb"); if (f) { fwrite(s, 1, n, f); fclose(f); }
    pid_t pid = fork();
    if (pid == 0) {   // child: render quietly, dark theme, transparent bg
        int dn = open("/dev/null", O_WRONLY); if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); }
        execlp("mmdc", "mmdc", "-i", mmd, "-o", png, "-t", "dark", "-b", "transparent", (char*)NULL);
        _exit(127);
    }
    g_mm[slot].pid = pid;
    return slot;
}
// reap finished mmdc children and load their PNGs. MUST run outside Begin/EndDrawing (LoadTexture).
static void mm_poll(void) {
    for (int i = 0; i < g_mmn; i++) {
        if (g_mm[i].state != 0) continue;
        int st; if (waitpid(g_mm[i].pid, &st, WNOHANG) != g_mm[i].pid) continue;
        char mmd[256], png[256]; mm_paths(g_mm[i].hash, mmd, png, sizeof mmd);
        Texture2D t = (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? LoadTexture(png) : (Texture2D){0};
        if (t.id) { GenTextureMipmaps(&t); SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR); g_mm[i].tex = t; g_mm[i].state = 1; }
        else g_mm[i].state = 2;
    }
}

// change-glow: a fading green left-bar + faint wash over a block that just appeared/changed.
static void glow(Block *b, float x0, float colw, float gy0, float y1) {
    if (b->born <= 0) return;
    double age = g_now - b->born;
    if (age >= 1.5) return;
    float a = age <= 0 ? 1.0f : (float)(1.0 - age/1.5);   // born this frame (parse runs after g_now → age<0): full glow, not suppressed (that caused a reparse-rate strobe)
    float sy0 = gy0 - g_scroll, h = y1 - gy0;
    if (sy0 + h < 0 || sy0 > g_vh) return;
    DrawRectangle((int)x0, (int)sy0, (int)colw, (int)h, (Color){0x9e,0xce,0x6a,(unsigned char)(26*a)});
    DrawRectangle((int)(x0-14), (int)sy0, 4, (int)h, (Color){0x9e,0xce,0x6a,(unsigned char)(210*a)});
}

// returns total content height (absolute). draws as a side effect.
static float render(Doc *d, float W, float base) {
    g_link_n = 0; g_check_n = 0; g_word_n = 0; g_code_n = 0; g_head_n = 0; g_glow_n = 0;
    float avail = W - g_left;                         // TOC panel (if open) eats into the left
    float colw = avail * 0.66f;                       // markdown column tracks ~66% of the usable width
    float x0 = g_left + (avail - colw) / 2.0f;
    g_col_x0 = x0; g_col_w = colw;
    float gutter = base * 1.6f;
    float y = 36.0f;

    for (int i = 0; i < d->n; i++) {
        Block *b = &d->bl[i];
        float gy0 = y;

        if (b->type == B_HR) {
            float sy = y + 10 - g_scroll;
            if (sy >= -2 && sy <= g_vh) DrawRectangle((int)x0, (int)sy, (int)colw, 1, RULE);
            y += 22; glow(b, x0, colw, gy0, y); continue;
        }

        // indent / quote geometry
        float bx = x0 + (b->indent>0 ? (b->indent-1)*gutter : 0);
        float tx = x0 + (b->indent>0 ? b->indent*gutter : 0);
        if (b->quote) { tx = x0 + 22*b->quote; bx = x0 + 22*b->quote; }   // indent per nesting level
        float tw = colw - (tx - x0);

        // mermaid: render via mmdc into a PNG and show it like an image. Skip while the
        // fence is still streaming (source unstable) and when mmdc isn't installed → plain code.
        if (b->type == B_CODE && b->lang && !strcasecmp(b->lang, "mermaid") && !b->streaming &&
            b->code_n && have_mmdc()) {
            uint64_t h = src_hash(b->code, b->code_n);
            int s = mm_find(h); if (s < 0) s = mm_start(h, b->code, b->code_n);
            int state = s >= 0 ? g_mm[s].state : 2;
            if (state == 1) {                                   // ready → draw the diagram
                Texture2D *t = &g_mm[s].tex;
                float iw = t->width, ih = t->height;
                float dw = fminf(colw, iw), dh = dw * ih / iw, ix = x0 + (colw - dw)/2, sy = y - g_scroll;
                if (sy + dh >= 0 && sy <= g_vh)
                    DrawTexturePro(*t, (Rectangle){0,0,iw,ih}, (Rectangle){ix, sy, dw, dh}, (Vector2){0,0}, 0, WHITE);
                y += dh + base*0.5f; glow(b, x0, colw, gy0, y); continue;
            } else if (state == 0) {                            // still rendering → placeholder
                float ph_h = base*2.2f, sy = y - g_scroll;
                if (sy + ph_h >= 0 && sy <= g_vh) {
                    DrawRectangleRounded((Rectangle){x0, sy, colw, ph_h}, 0.06f, 6, CODEBG);
                    DrawTextEx(g_reg, "rendering diagram…", (Vector2){x0+14, sy + base*0.6f}, base*0.9f, 0, MUTED);
                }
                y += ph_h + base*0.4f; glow(b, x0, colw, gy0, y); continue;
            }
            // state == 2 (failed/missing) → fall through to normal highlighted code
        }

        if (b->type == B_CODE) {
            float cs = base*0.92f, clh = cs*1.55f, pad = 12;
            int lines = 1; for (size_t k=0;k<b->code_n;k++) if (b->code[k]=='\n') lines++;
            if (b->code_n && b->code[b->code_n-1]=='\n') lines--;
            if (lines < 1) lines = 1;
            float bh = lines*clh + 2*pad;
            float sy = y - g_scroll;
            if (sy + bh >= 0 && sy <= g_vh) {
                DrawRectangleRounded((Rectangle){x0, sy, colw, bh}, 0.06f, 6, CODEBG);
                BeginScissorMode((int)x0, (int)fmaxf(sy,0), (int)colw, (int)fminf(bh, g_vh));
                float ty = y + pad; const char *p = b->code ? b->code : "";   // empty/just-opened fence (streaming): no code yet
                float cw = MeasureTextEx(g_mono, "0", cs, 0).x, carx = x0+pad, cary = ty;
                int cm = comment_style(b->lang), inblock = 0;
                for (int ln=0; ln<lines; ln++) {
                    const char *nl = strchr(p, '\n'); int len = nl ? (int)(nl-p) : (int)strlen(p);
                    char line[4096]; if (len > 4095) len = 4095; memcpy(line, p, len); line[len] = 0;
                    hl_line(line, len, g_mono, x0+pad, ty - g_scroll, cs, cm, &inblock);
                    carx = x0+pad + len*cw; cary = ty;
                    ty += clh; if (!nl) break; p = nl+1;
                }
                if (b->streaming && fmod(g_now, 1.0) < 0.5)      // blinking caret on the live code block
                    DrawRectangle((int)carx, (int)(cary - g_scroll), (int)fmaxf(2, cw*0.5f), (int)cs, CODEFG);
                EndScissorMode();
                if (b->code && b->code_n && g_code_n < 256 &&                 // "copy" button shown on hover
                    CheckCollisionPointRec(g_mouse, (Rectangle){x0, sy, colw, bh})) {
                    float fs = cs*0.8f, bw = MeasureTextEx(g_mono, "copy", fs, 0).x + 14;
                    Rectangle btn = { x0 + colw - bw - 8, sy + 8, bw, fs + 8 };
                    DrawRectangleRounded(btn, 0.35f, 4, (Color){0x3a,0x42,0x5e,255});
                    DrawTextEx(g_mono, "copy", (Vector2){btn.x+7, btn.y+4}, fs, 0, FG);
                    g_codes[g_code_n].r = btn; g_codes[g_code_n].code = b->code; g_code_n++;
                }
            }
            y += bh + base*0.4f; glow(b, x0, colw, gy0, y); continue;
        }

        if (b->type == B_TABLE) {
            float ts = base*0.95f, tlh = ts*1.5f, cpad = 14;
            int cols = 0; for (int r=0;r<b->row_n;r++) if (b->rows[r].cell_n > cols) cols = b->rows[r].cell_n;
            if (cols > 64) cols = 64;
            float cw_arr[64]; for (int c=0;c<cols;c++) cw_arr[c] = 0;
            #define CELLW(cell,hdr) ({ float _w=0; for (int _k=0;_k<(cell)->run_n;_k++){ uint8_t _f=(cell)->runs[_k].flags|((hdr)?F_BOLD:0); _w+=MeasureTextEx(fontfor(_f),(cell)->runs[_k].text,ts,0).x; } _w; })
            for (int r=0;r<b->row_n;r++) { Row *row=&b->rows[r];               // measure → natural column widths
                for (int c=0;c<row->cell_n && c<cols;c++) { float w=CELLW(&row->cells[c],row->is_header); if (w>cw_arr[c]) cw_arr[c]=w; } }
            float total=0; for (int c=0;c<cols;c++) total += cw_arr[c] + 2*cpad;
            float scale = (total > tw && total > 0) ? tw/total : 1.0f;          // squish to fit; ponytail: cells clip rather than wrap
            float tabw = total*scale, ty = y;
            for (int r=0;r<b->row_n;r++) {
                Row *row = &b->rows[r];
                float sy = ty - g_scroll;
                if (sy + tlh >= 0 && sy <= g_vh) {
                    float cx = tx, tyoff = (tlh-ts)/2;
                    for (int c=0;c<cols;c++) {
                        float cwid = (cw_arr[c] + 2*cpad) * scale;
                        Cell *cell = c < row->cell_n ? &row->cells[c] : NULL;
                        if (cell) {
                            float innerw = cwid - 2*cpad, cellw = CELLW(cell,row->is_header);
                            float ox = cell->align==2 ? innerw-cellw : cell->align==1 ? (innerw-cellw)/2 : 0;
                            if (ox < 0) ox = 0;
                            BeginScissorMode((int)(cx+cpad), (int)fmaxf(sy,0), (int)innerw, (int)fminf(tlh,g_vh));
                            float rx = cx + cpad + ox;
                            for (int k=0;k<cell->run_n;k++) {
                                Run *run = &cell->runs[k]; uint8_t fl = run->flags | (row->is_header?F_BOLD:0);
                                Font fn = fontfor(fl); Color col = colfor(fl, FG);
                                float rw = MeasureTextEx(fn, run->text, ts, 0).x;
                                DrawTextEx(fn, run->text, (Vector2){rx, sy+tyoff}, ts, 0, col);
                                if (fl & F_LINK)   DrawLineEx((Vector2){rx,sy+tyoff+ts},(Vector2){rx+rw,sy+tyoff+ts},1,col);
                                if (fl & F_STRIKE) DrawLineEx((Vector2){rx,sy+tyoff+ts*0.55f},(Vector2){rx+rw,sy+tyoff+ts*0.55f},1.5f,col);
                                if (g_word_n < 16384 && run->text[0]) {   // searchable/selectable (one box per run)
                                    g_words[g_word_n].x=rx; g_words[g_word_n].y=ty+tyoff; g_words[g_word_n].w=rw; g_words[g_word_n].h=ts;
                                    g_words[g_word_n].t=run->text; g_words[g_word_n].tn=(int)strlen(run->text); g_word_n++;
                                }
                                if (run->href && g_link_n < 2048) { g_links[g_link_n].r=(Rectangle){rx,sy+tyoff,rw,ts}; g_links[g_link_n].href=run->href; g_link_n++; }
                                rx += rw;
                            }
                            EndScissorMode();
                        }
                        cx += cwid;
                    }
                    if (row->is_header) DrawRectangle((int)tx,(int)(sy+tlh-1),(int)tabw,2,RULE);
                    else if (r < b->row_n-1) DrawRectangle((int)tx,(int)(sy+tlh-1),(int)tabw,1,(Color){RULE.r,RULE.g,RULE.b,90});
                }
                ty += tlh;
            }
            #undef CELLW
            y = ty + base*0.5f;
            glow(b, x0, colw, gy0, y);
            if (b->born > g_change_at) { g_change_at = b->born; g_change_y = gy0; }
            continue;
        }

        // markers (bullets, numbers, checkboxes)
        if (b->marker) {
            float my = y - g_scroll;
            if (b->marker == M_BULLET) DrawCircleV((Vector2){bx + base*0.55f, my + base*0.55f}, base*0.14f, ACCENT);
            else if (b->marker == M_NUM) { char n[16]; snprintf(n,sizeof n,"%d.", b->list_num); DrawTextEx(g_reg, n, (Vector2){bx, my}, base, 0, MUTED); }
            else { // checkbox
                Rectangle cb = {bx+2, my + base*0.18f, base*0.62f, base*0.62f};
                if (b->marker == M_CHECKED) { DrawRectangleRec(cb, ACCENT);
                    DrawLineEx((Vector2){cb.x+cb.width*0.22f, cb.y+cb.height*0.52f},(Vector2){cb.x+cb.width*0.42f, cb.y+cb.height*0.74f}, 2, BG);
                    DrawLineEx((Vector2){cb.x+cb.width*0.42f, cb.y+cb.height*0.74f},(Vector2){cb.x+cb.width*0.80f, cb.y+cb.height*0.28f}, 2, BG); }
                else DrawRectangleLinesEx(cb, 2, MUTED);
                if (g_check_n < 512) { g_checks[g_check_n].r = cb; g_checks[g_check_n].off = b->task_off; g_check_n++; }
            }
        }

        float ystart = y;
        if (b->type == B_H) {
            if (g_head_n < 256) {                        // collect for the TOC sidebar
                g_head[g_head_n].level = b->level; g_head[g_head_n].y = gy0;
                char *o = g_head[g_head_n].text; int oi = 0;
                for (int r = 0; r < b->run_n && oi < 78; r++)
                    for (const char *p = b->runs[r].text; *p && oi < 78; p++) o[oi++] = *p;
                o[oi] = 0; g_head_n++;
            }
            float hs = b->level==1 ? base*1.7f : b->level==2 ? base*1.4f : b->level==3 ? base*1.2f : base*1.05f;
            Color hc = b->level<=2 ? ACCENT : FG;
            y += hs*0.45f;
            float ry = y;
            flow(b, tx, tw, hs, hc, F_BOLD, hs*1.25f, &y);
            if (b->level<=2) { float ly = y + 2 - g_scroll; if (ly>=0 && ly<=g_vh) DrawRectangle((int)x0,(int)ly,(int)colw,1,RULE); y += 8; }
            (void)ry; y += hs*0.25f;
        } else if (b->run_n == 1 && (b->runs[0].flags & F_IMG)) {   // standalone image → render large, centered
            Texture2D *t = tex_get(b->runs[0].href);
            if (t) {
                float iw = t->width, ih = t->height, dw = fminf(tw, iw), dh = dw*ih/iw, ix = tx + (tw-dw)/2, sy = y - g_scroll;
                if (sy + dh >= 0 && sy <= g_vh) DrawTexturePro(*t, (Rectangle){0,0,iw,ih}, (Rectangle){ix, sy, dw, dh}, (Vector2){0,0}, 0, WHITE);
                y += dh + base*0.5f;
            } else {
                char ph[4200]; snprintf(ph, sizeof ph, "[ image: %s ]", b->runs[0].href ? b->runs[0].href : "?");
                float sy = y - g_scroll, ph_h = base*1.6f;
                if (sy + ph_h >= 0 && sy <= g_vh) { DrawRectangleRounded((Rectangle){tx, sy, tw, ph_h}, 0.1f, 4, CODEBG);
                    DrawTextEx(g_reg, ph, (Vector2){tx+12, sy + base*0.35f}, base*0.85f, 0, MUTED); }
                y += ph_h + base*0.4f;
            }
        } else { // B_P (paragraphs, list text, table rows, quotes, callouts)
            if (b->callout && b->callout_head) {         // callout label header line
                float ly = y - g_scroll;
                if (ly + base >= 0 && ly <= g_vh)
                    DrawTextEx(g_bold, callout_name(b->callout), (Vector2){tx, ly}, base*0.95f, 0, callout_color(b->callout));
                y += base*1.35f;
            }
            Color base_c = b->callout ? FG : (b->quote ? MUTED : FG);
            uint8_t addf = b->is_header ? F_BOLD : 0;
            flow(b, tx, tw, base, base_c, addf, base*1.55f, &y);
            if (b->is_header) { float ly = y - 2 - g_scroll; if (ly>=0 && ly<=g_vh) DrawRectangle((int)x0,(int)ly,(int)colw,1,RULE); }
            y += b->is_row ? base*0.15f : base*0.55f;
        }

        if (b->callout) {                                // tinted box: faint wash (over text) + colored left bar
            Color cc = callout_color(b->callout);
            float wy = ystart - g_scroll - 4, wh = y - ystart + 4;
            if (wy + wh >= 0 && wy <= g_vh) {
                Color wash = cc; wash.a = 22;
                DrawRectangle((int)x0, (int)wy, (int)colw, (int)wh, wash);
                DrawRectangle((int)x0, (int)wy, 4, (int)wh, cc);
            }
        } else if (b->quote) { float qy = ystart - g_scroll, qh = y - ystart;   // one bar per nesting level
            for (int q = 0; q < b->quote; q++) DrawRectangle((int)(x0+6+q*22), (int)qy, 3, (int)qh, MUTED); }
        glow(b, x0, colw, gy0, y);
        if (b->born > 0 && g_now - b->born < 1.5 && g_glow_n < 256) {   // change-minimap tick
            g_glowmarks[g_glow_n].y = gy0; g_glowmarks[g_glow_n].born = b->born; g_glow_n++;
        }
        if (b->born > g_change_at) { g_change_at = b->born; g_change_y = gy0; }   // newest change → auto jump-to-change target
    }
    g_doc_end_y = y;
    return y + 36;
}

// ── fonts (resolved through fontconfig at runtime) ──────────────────────────
static const char *fcmatch(const char *pattern) {
    static char buf[8][4096]; static int k = 0; char *o = buf[k++ & 7];
    char cmd[256]; snprintf(cmd, sizeof cmd, "fc-match -f '%%{file}' '%s' 2>/dev/null", pattern);
    FILE *f = popen(cmd, "r"); o[0] = 0;
    if (f) { if (!fgets(o, 4096, f)) o[0] = 0; pclose(f); }
    o[strcspn(o, "\n")] = 0; return o;
}
static Font loadfont(const char *pattern, int size, int *cps, int ncp) {
    const char *path = fcmatch(pattern);
    if (!path[0]) return GetFontDefault();
    Font f = LoadFontEx(path, size, cps, ncp);
    if (f.texture.id == 0) return GetFontDefault();
    GenTextureMipmaps(&f.texture);                          // mip levels → clean text when scaled down small
    SetTextureFilter(f.texture, TEXTURE_FILTER_TRILINEAR);  // sample across mips instead of aliasing the 72px atlas
    return f;
}

// ── file io ─────────────────────────────────────────────────────────────────
static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc(n+1); size_t got = fread(buf, 1, n, f); fclose(f);
    buf[got] = 0; *len = got; return buf;
}

// flip a task checkbox in the source file; inotify then reloads + re-renders us.
// ponytail: rewrites the whole file; markdown is small, no atomic temp+rename.
static void toggle_check(const char *path, size_t off) {
    size_t len; char *src = slurp(path, &len);
    if (!src) return;
    if (off < len && (src[off]==' ' || src[off]=='x' || src[off]=='X')) {   // guard stale offset
        src[off] = (src[off]==' ') ? 'x' : ' ';
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(src, 1, len, f); fclose(f); }
    }
    free(src);
}

// is a word box (content coords) at least partially inside the current selection?
// mirrors sel_line's per-line clipping logic, as a boolean test.
static int word_in_sel(float wx, float wr, float wy, float lh) {
    if (!g_has_sel) return 0;
    float p0x,p0y,p1x,p1y;
    if (g_sel_ay <= g_sel_cy) { p0x=g_sel_ax;p0y=g_sel_ay;p1x=g_sel_cx;p1y=g_sel_cy; }
    else                      { p0x=g_sel_cx;p0y=g_sel_cy;p1x=g_sel_ax;p1y=g_sel_ay; }
    if (!(wy + lh > p0y && wy < p1y)) return 0;
    int hasP0 = (p0y >= wy && p0y < wy+lh), hasP1 = (p1y >= wy && p1y < wy+lh);
    float lo, hi;
    if (hasP0 && hasP1) { lo = fminf(p0x,p1x); hi = fmaxf(p0x,p1x); }
    else if (hasP0)     { lo = p0x; hi = 1e9f; }
    else if (hasP1)     { lo = -1e9f; hi = p1x; }
    else                { lo = -1e9f; hi = 1e9f; }
    return wr > lo && wx < hi;
}
// reconstruct selected text (rendered, not source) from the word index → clipboard.
// ponytail: rebuilds rendered text; doesn't round-trip markdown, and skips code blocks.
static int copy_selection(void) {
    if (!g_has_sel) return 0;
    size_t cap = 1; for (int i=0;i<g_word_n;i++) cap += g_words[i].tn + 1;
    char *buf = malloc(cap); size_t n = 0; float prevy = -1e30f; int first = 1;
    for (int i=0;i<g_word_n;i++) {
        if (!word_in_sel(g_words[i].x, g_words[i].x+g_words[i].w, g_words[i].y, g_words[i].h)) continue;
        if (!first) buf[n++] = (g_words[i].y != prevy) ? '\n' : ' ';
        memcpy(buf+n, g_words[i].t, g_words[i].tn); n += g_words[i].tn;
        prevy = g_words[i].y; first = 0;
    }
    buf[n] = 0;
    if (n) SetClipboardText(buf);
    free(buf);
    return n > 0;
}

// case-insensitive substring search; returns byte offset of match or -1.
static int ci_find(const char *h, int hn, const char *n, int nn) {
    if (nn == 0 || nn > hn) return -1;
    for (int i = 0; i + nn <= hn; i++) {
        int ok = 1;
        for (int j = 0; j < nn; j++) if (tolower((unsigned char)h[i+j]) != tolower((unsigned char)n[j])) { ok = 0; break; }
        if (ok) return i;
    }
    return -1;
}

int main(int argc, char **argv) {
    // input source: explicit "-", a file path, or a pipe on stdin.
    int use_stdin = 0; const char *path = NULL;
    if (argc >= 2 && strcmp(argv[1], "-") == 0) use_stdin = 1;
    else if (argc >= 2) path = argv[1];
    else if (!isatty(0)) use_stdin = 1;
    else { fprintf(stderr, "usage: %s file.md   (or:  cmd | %s)\n", argv[0], argv[0]); return 1; }

    char db[4096] = {0}, bb[4096] = {0};
    const char *bname = "stdin";
    int ifd = -1;
    if (use_stdin) {
        if (!getcwd(g_docdir, sizeof g_docdir)) snprintf(g_docdir, sizeof g_docdir, ".");  // relative images vs cwd
        fcntl(0, F_SETFL, fcntl(0, F_GETFL, 0) | O_NONBLOCK);                               // poll stdin per frame
    } else {
        // watch the *directory* + filter basename so atomic rename-writes survive.
        strncpy(db, path, sizeof db - 1); char *dir = dirname(db);
        strncpy(bb, path, sizeof bb - 1); bname = basename(bb);
        snprintf(g_docdir, sizeof g_docdir, "%s", dir);
        ifd = inotify_init1(IN_NONBLOCK);
        if (ifd >= 0) inotify_add_watch(ifd, dir, IN_CLOSE_WRITE|IN_MODIFY|IN_MOVED_TO|IN_CREATE);
    }

    SetTraceLogLevel(LOG_WARNING);
    // no FLAG_MSAA_4X_HINT: under raylib 6.0 + HiDPI Wayland its MSAA resolve mis-blits a
    // logical-sized region on content-heavy frames → black screen. Text/shape edges stay clean enough.
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_WINDOW_HIGHDPI);
    char title[4200]; snprintf(title, sizeof title, "markshown - %s", bname);
    InitWindow(1000, 820, title);
    SetTargetFPS(120);

    // codepoints: Latin + general punctuation (smart quotes, dashes, …) + arrows
    int cap = 2048, ncp = 0; int *cps = malloc(cap*sizeof(int));
    #define ADD(a,b) for (int c=(a); c<=(b); c++) { if (ncp>=cap){cap*=2;cps=realloc(cps,cap*sizeof(int));} cps[ncp++]=c; }
    ADD(32, 0x24F) ADD(0x2010, 0x22FF)   // Latin+ · punctuation, currency, letterlike (™), arrows, math operators (for decoded entities)
    #undef ADD
    int atlas = 72;
    g_reg  = loadfont("sans-serif",                atlas, cps, ncp);
    g_bold = loadfont("sans-serif:weight=bold",    atlas, cps, ncp);
    g_ital = loadfont("sans-serif:slant=italic",   atlas, cps, ncp);
    g_mono = loadfont("monospace",                 atlas, cps, ncp);
    free(cps);

    Doc doc = {0}; doc.cur = -1; doc.tbl = -1;
    char *sbuf = NULL; size_t slen = 0, scap = 0; int eof = 0;   // stdin: growing input buffer
    if (!use_stdin) { size_t len; char *src = slurp(path, &len); if (src) { parse(&doc, src, len); free(src); } }

    float base = 19, target = 0, scroll = 0, content = 1, last_reload = 0;
    double change_seen = 0;   // born-time of the last change we've already scrolled to
    bool follow = true, dirty = false, dragging = false, just_dbl = false;
    Vector2 downscr = {0}, lastclk = {0};
    double last_click_t = -1;

    while (!WindowShouldClose()) {
        double now = GetTime();

        // ── input drain: stdin stream, or inotify on the watched file ──
        if (use_stdin) {
            if (!eof) {
                char rb[65536]; ssize_t r = -1;
                while ((r = read(0, rb, sizeof rb)) > 0) {
                    if (slen == 0 && memchr(rb, 0, (size_t)r)) { fprintf(stderr, "markshown: binary input, ignoring\n"); eof = 1; break; }
                    if (slen + (size_t)r + 1 > scap) {              // grow, capped at 64 MB
                        size_t want = (slen + (size_t)r + 1) * 2; if (want > (64u<<20)) want = 64u<<20;
                        if (want > scap) { scap = want; sbuf = realloc(sbuf, scap); }
                    }
                    size_t take = (size_t)r; if (slen + take + 1 > scap) take = scap - 1 - slen;
                    if (!take) break;
                    memcpy(sbuf + slen, rb, take); slen += take; sbuf[slen] = 0; dirty = true;
                }
                if (r == 0) eof = 1;   // true EOF → stop polling, keep the final doc static
            }
        } else if (ifd >= 0) {
            char ev[8192] __attribute__((aligned(__alignof__(struct inotify_event))));
            ssize_t r;
            while ((r = read(ifd, ev, sizeof ev)) > 0)
                for (char *p = ev; p < ev + r; ) {
                    struct inotify_event *e = (void*)p;
                    if (e->len && strcmp(e->name, bname) == 0) dirty = true;
                    p += sizeof(struct inotify_event) + e->len;
                }
        }
        if (dirty && now - last_reload > 0.033) {       // coalesce bursts to ~30/s
            if (use_stdin) { if (sbuf) parse(&doc, sbuf, slen); }
            else { size_t len; char *src = slurp(path, &len); if (src) { parse(&doc, src, len); free(src); } }
            dirty = false; last_reload = now; g_has_sel = 0;   // text changed → drop stale selection
        }

        // ── input ──
        float vh = (float)GetScreenHeight();
        float base_in = base;
        bool typing = (g_search == 1);   // search box has keyboard focus
        float mw = GetMouseWheelMove();
        bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        if (ctrl && mw != 0) base = fmaxf(11, fminf(44, base * (mw > 0 ? 1.1f : 0.9f)));  // ctrl+wheel zooms
        else { if (mw != 0) follow = false; target -= mw * base * 3.2f; }                 // bare wheel scrolls (pauses auto-follow)
        if (!typing) {
            if (IsKeyDown(KEY_J) || IsKeyDown(KEY_DOWN)) { target += base * 0.9f; follow = false; }
            if (IsKeyDown(KEY_K) || IsKeyDown(KEY_UP))   { target -= base * 0.9f; follow = false; }
            if (IsKeyPressed(KEY_PAGE_DOWN)) { target += vh * 0.85f; follow = false; }
            if (IsKeyPressed(KEY_PAGE_UP))   { target -= vh * 0.85f; follow = false; }
            if (IsKeyPressed(KEY_HOME) || IsKeyPressed(KEY_G)) { follow = !IsKeyDown(KEY_LEFT_SHIFT); if (!follow) target = 0; }
            if (IsKeyPressed(KEY_END)) { follow = true; change_seen = 0; }   // resume + snap to the latest change
            if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))      base = fminf(base*1.1f, 44);
            if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) base = fmaxf(base*0.9f, 11);
            if (ctrl && IsKeyPressed(KEY_C) && copy_selection()) g_copy_flash = now;   // Ctrl+C copies the selection
            if (IsKeyPressed(KEY_O) || IsKeyPressed(KEY_TAB)) g_toc = !g_toc;          // toggle the outline sidebar
            if (IsKeyPressed(KEY_Q)) break;
        }
        if (base != base_in) g_has_sel = 0;   // zoom reflows the text → drop stale selection

        // ── search ── ( / opens, type to filter, Enter/n/N to jump, Esc closes )
        bool just_open = false;
        if (g_search != 1 && IsKeyPressed(KEY_SLASH)) { g_search = 1; g_qn = 0; g_query[0] = 0; just_open = true; while (GetCharPressed()){} }
        if (g_search == 1 && !just_open) {
            int ch; while ((ch = GetCharPressed())) if (ch >= 32 && ch < 127 && g_qn < 126) { g_query[g_qn++] = (char)ch; g_query[g_qn] = 0; }
            if (IsKeyPressed(KEY_BACKSPACE) && g_qn > 0) g_query[--g_qn] = 0;
            if (IsKeyPressed(KEY_ESCAPE)) { g_search = 0; g_qn = 0; g_query[0] = 0; }
            else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                g_search = g_qn ? 2 : 0; g_match_i = 0;
                if (g_search == 2 && g_match_n > 0) { target = g_matches[0] - vh*0.35f; follow = false; }
            }
        } else if (g_search == 2) {
            bool sh = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            if ((IsKeyPressed(KEY_N) || IsKeyPressed(KEY_ENTER)) && g_match_n > 0) {
                g_match_i = (g_match_i + (sh ? -1 : 1) + g_match_n) % g_match_n;
                target = g_matches[g_match_i] - vh*0.35f; follow = false;
            }
            if (IsKeyPressed(KEY_SLASH)) g_search = 1;                                  // edit the query again
            if (IsKeyPressed(KEY_ESCAPE)) { g_search = 0; g_qn = 0; g_query[0] = 0; }
        }

        float maxs   = fmaxf(0, content - vh);                  // natural bottom
        float botmax = fmaxf(maxs, content - vh*0.12f);         // overscroll room: any heading/change can ride up to the top, empty space below is fine
        target = fmaxf(0, fminf(target, botmax));               // auto jump-to-change sets `target` after render (below); here we just clamp + smooth
        // framerate-independent smoothing toward target
        scroll += (target - scroll) * (1.0f - expf(-GetFrameTime() * 18.0f));
        g_scroll = scroll; g_vh = vh;
        g_toc_anim += ((g_toc ? 1.0f : 0.0f) - g_toc_anim) * (1.0f - expf(-GetFrameTime() * 16.0f));
        g_left = 240.0f * g_toc_anim;

        g_now = now;
        // load textures OUTSIDE the draw batch (mid-frame LoadTexture corrupts the frame)
        mm_poll();   // reap finished mermaid renders
        img_poll();  // reap finished remote-image downloads
        for (int i = 0; i < doc.n; i++) {   // kick off / cache image textures (inline runs + table cells)
            Block *b = &doc.bl[i];
            for (int r = 0; r < b->run_n; r++) if (b->runs[r].flags & F_IMG) tex_get(b->runs[r].href);
            for (int r = 0; r < b->row_n; r++) for (int c = 0; c < b->rows[r].cell_n; c++) {
                Cell *cell = &b->rows[r].cells[c];
                for (int k = 0; k < cell->run_n; k++) if (cell->runs[k].flags & F_IMG) tex_get(cell->runs[k].href);
            }
        }

        // ── draw ──
        BeginDrawing();
        ClearBackground(BG);
        g_mouse = GetMousePosition();
        content = render(&doc, (float)GetScreenWidth(), base);

        // auto jump-to-change: while following (not manually scrolled), smooth-scroll to frame the
        // newest changed block ~1/3 down, so we're always where the last edit/append landed.
        // (only when the doc is taller than the screen; if it all fits, the change is already visible.)
        if (follow && content > vh && g_change_at > change_seen && g_change_y >= 0) {
            change_seen = g_change_at;
            float bm = fmaxf(fmaxf(0, content - vh), content - vh*0.12f);
            target = fmaxf(0, fminf(g_change_y - vh*0.30f, bm));
        }

        // search: highlight matching words (over the text) + collect their content-y for n/N
        if (g_search && g_qn > 0) {
            g_match_n = 0;
            for (int i = 0; i < g_word_n; i++) {
                if (ci_find(g_words[i].t, g_words[i].tn, g_query, g_qn) < 0) continue;
                float sy = g_words[i].y - g_scroll;
                if (sy + g_words[i].h >= 0 && sy <= g_vh)
                    DrawRectangle((int)g_words[i].x, (int)sy, (int)g_words[i].w, (int)g_words[i].h,
                                  g_match_n == g_match_i ? SRCHC : SRCH);
                if (g_match_n < 4096) g_matches[g_match_n++] = g_words[i].y;
            }
            if (g_match_i >= g_match_n) g_match_i = g_match_n ? g_match_n - 1 : 0;
        }

        // ── TOC sidebar panel (drawn over the reserved left margin) ──
        float panelW = 240.0f * g_toc_anim;
        struct { Rectangle r; float y; } tocitems[256]; int tocitem_n = 0;
        if (panelW > 1) {
            DrawRectangle(0, 0, (int)panelW, (int)vh, (Color){0x16,0x16,0x1e,245});
            DrawRectangle((int)panelW, 0, 1, (int)vh, RULE);
            int cur = -1; for (int i = 0; i < g_head_n; i++) if (g_head[i].y <= scroll + 50) cur = i;
            BeginScissorMode(0, 0, (int)panelW, (int)vh);
            float ly = 44;                                  // ponytail: no panel scroll; long outlines clip at the bottom
            for (int i = 0; i < g_head_n; i++) {
                float fs = 15, ind = 10 + (g_head[i].level - 1) * 12;
                if (i == cur) DrawRectangle(0, (int)(ly-3), (int)panelW, (int)(fs+8), (Color){0x29,0x2e,0x42,255});
                DrawTextEx(g_reg, g_head[i].text, (Vector2){ind, ly}, fs, 0, i==cur ? ACCENT : FG);
                if (tocitem_n < 256) { tocitems[tocitem_n].r = (Rectangle){0, ly-3, panelW, fs+8}; tocitems[tocitem_n].y = g_head[i].y; tocitem_n++; }
                ly += fs + 9;
            }
            EndScissorMode();
        }
        // change-minimap: a thin right-edge strip with a tick per recently-changed block (click to jump)
        if (g_glow_n > 0 && content > vh) {
            float sx = (float)GetScreenWidth() - 6;
            DrawRectangle((int)sx, 0, 6, (int)vh, (Color){0x16,0x16,0x1e,160});
            for (int i = 0; i < g_glow_n; i++) {
                float a = (float)(1.0 - (g_now - g_glowmarks[i].born)/1.5); if (a < 0) a = 0;
                float ty = (g_glowmarks[i].y / content) * vh;
                DrawRectangle((int)sx, (int)ty, 6, 3, (Color){0x9e,0xce,0x6a,(unsigned char)(255*a)});
            }
        }

        // interaction: drag = select text, plain click = link/checkbox (hit lists filled by render)
        Vector2 mp = GetMousePosition();
        const char *hover = NULL;
        for (int i = 0; i < g_link_n; i++) if (CheckCollisionPointRec(mp, g_links[i].r)) { hover = g_links[i].href; break; }
        bool over_btn = false;
        for (int i = 0; i < g_check_n && !over_btn; i++) if (CheckCollisionPointRec(mp, g_checks[i].r)) over_btn = true;
        for (int i = 0; i < g_code_n  && !over_btn; i++) if (CheckCollisionPointRec(mp, g_codes[i].r))  over_btn = true;
        SetMouseCursor((hover || over_btn) ? MOUSE_CURSOR_POINTING_HAND : MOUSE_CURSOR_IBEAM);

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && panelW > 4 && mp.x < panelW) {
            for (int i = 0; i < tocitem_n; i++) if (CheckCollisionPointRec(mp, tocitems[i].r)) { target = fmaxf(0, tocitems[i].y - 30); follow = false; break; }
        } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && mp.x > GetScreenWidth() - 8 && g_glow_n > 0) {
            float best = 1e9f; int bi = -1;                      // minimap → jump to the nearest change tick
            for (int i = 0; i < g_glow_n; i++) { float ty = (g_glowmarks[i].y / content) * vh; if (fabsf(ty-mp.y) < best) { best = fabsf(ty-mp.y); bi = i; } }
            if (bi >= 0) { target = fmaxf(0, g_glowmarks[bi].y - 30); follow = false; }
        } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            bool dbl = (now - last_click_t < 0.30 && fabsf(mp.x-lastclk.x) + fabsf(mp.y-lastclk.y) < 6);
            last_click_t = now; lastclk = mp;
            if (dbl) {                                           // double-click → select the word under the cursor
                just_dbl = true; dragging = false; g_has_sel = 0;
                float cy = mp.y + scroll;
                for (int i = 0; i < g_word_n; i++)
                    if (mp.x >= g_words[i].x && mp.x <= g_words[i].x+g_words[i].w && cy >= g_words[i].y && cy <= g_words[i].y+g_words[i].h) {
                        g_sel_ax = g_words[i].x; g_sel_cx = g_words[i].x + g_words[i].w;
                        g_sel_ay = g_sel_cy = g_words[i].y + g_words[i].h*0.5f; g_has_sel = 1; break;
                    }
            } else {                                             // single click → anchor a (so far empty) selection
                just_dbl = false; downscr = mp; dragging = true;
                g_sel_ax = mp.x; g_sel_ay = mp.y + scroll;
                g_sel_cx = g_sel_ax; g_sel_cy = g_sel_ay; g_has_sel = 0;
            }
        }
        if (dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {  // extend while dragging
            g_sel_cx = mp.x; g_sel_cy = mp.y + scroll;
            if (fabsf(mp.x-downscr.x) + fabsf(mp.y-downscr.y) > 3) g_has_sel = 1;
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            dragging = false;
            if (just_dbl) just_dbl = false;                      // word already selected; keep it, no click action
            else if (fabsf(mp.x-downscr.x) + fabsf(mp.y-downscr.y) <= 3) {   // a plain click
                g_has_sel = 0;
                int hit = 0;
                for (int i = 0; i < g_code_n; i++) if (CheckCollisionPointRec(mp, g_codes[i].r)) { SetClipboardText(g_codes[i].code); g_copy_flash = now; hit = 1; break; }
                for (int i = 0; i < g_check_n && !hit; i++) if (CheckCollisionPointRec(mp, g_checks[i].r)) { if (path) toggle_check(path, g_checks[i].off); hit = 1; break; }
                if (!hit && hover) OpenURL(hover);
            }
        }

        // hovering a link → floating tooltip with its target near the cursor
        if (hover) {
            float ts = 14, pad = 6, sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
            Vector2 tsz = MeasureTextEx(g_reg, hover, ts, 0);
            float tw = tsz.x + pad*2, th = tsz.y + pad*2;
            float tx = mp.x + 14, ty = mp.y + 20;
            if (tx + tw > sw) tx = sw - tw - 4;   // keep on-screen
            if (tx < 4) tx = 4;                    // ponytail: very long URLs may still clip; no wrap
            if (ty + th > sh) ty = mp.y - th - 8;
            DrawRectangleRounded((Rectangle){tx, ty, tw, th}, 0.25f, 6, ICODEBG);
            DrawTextEx(g_reg, hover, (Vector2){tx + pad, ty + pad}, ts, 0, LINKC);
        }

        // live stream → blinking caret at the document end (skip if the last block is a streaming code block, which has its own)
        if (use_stdin && !eof && fmod(now, 1.0) < 0.5 &&
            !(doc.n && doc.bl[doc.n-1].type == B_CODE && doc.bl[doc.n-1].streaming)) {
            float cy = g_doc_end_y - g_scroll;
            if (cy >= 0 && cy <= vh) DrawRectangle((int)g_col_x0, (int)(cy - base), (int)fmaxf(2, base*0.5f), (int)base, FG);
        }

        // search bar pinned to the bottom
        if (g_search) {
            float bh = 30, by = vh - bh;
            DrawRectangle(0, (int)by, GetScreenWidth(), (int)bh, CODEBG);
            DrawRectangle(0, (int)by, GetScreenWidth(), 1, RULE);
            char info[64];
            if (g_qn == 0) info[0] = 0;
            else if (g_match_n) snprintf(info, sizeof info, "%d/%d", g_match_i+1, g_match_n);
            else snprintf(info, sizeof info, "no matches");
            char line[256]; snprintf(line, sizeof line, "/%s%s   %s", g_query, g_search==1?"_":"", info);
            DrawTextEx(g_reg, line, (Vector2){14, by+7}, 16, 0, FG);
        }

        // brief "copied" toast after any clipboard copy
        if (g_copy_flash > 0 && now - g_copy_flash < 1.0) {
            float fs = 15, pad = 8, a = (float)(1.0 - (now - g_copy_flash));
            Vector2 ms = MeasureTextEx(g_reg, "copied", fs, 0);
            float tw = ms.x + pad*2, th = ms.y + pad*2;
            Color bg = ICODEBG; bg.a = (unsigned char)(220*a);
            Color fg = CODEFG;  fg.a = (unsigned char)(255*a);
            Rectangle tr = { GetScreenWidth() - tw - 16, vh - th - 16, tw, th };
            DrawRectangleRounded(tr, 0.3f, 6, bg);
            DrawTextEx(g_reg, "copied", (Vector2){tr.x+pad, tr.y+pad}, fs, 0, fg);
        }

        EndDrawing();
    }
    freedoc(&doc);
    for (int i = 0; i < g_tcn; i++) { if (g_tc[i].ok) UnloadTexture(g_tc[i].tex); free(g_tc[i].src); }
    for (int i = 0; i < g_mmn; i++) if (g_mm[i].state == 1) UnloadTexture(g_mm[i].tex);
    free(sbuf);
    CloseWindow();
    if (ifd >= 0) close(ifd);
    return 0;
}
