/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 qememe
 */
#define _XOPEN_SOURCE 700
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>
#include "md4c.h"
#include "entity.h"

#define VERSION "1.0.0"
#define MAX_DEPTH 128
#define MAX_WIDTH 10000

enum { BOLD=1, ITALIC=2, UNDER=4, STRIKE=8, ACCENT=16, CODE=32,
       DIM=64, GREEN=128, PURPLE=256, NUMBER=512, BACKGROUND=1024 };
typedef struct { char *data; size_t len, cap; } Buffer;
typedef struct Node Node;
struct Node {
    int kind, type;
    unsigned number;
    int align, task, tight;
    char *value;
    size_t length;
    Node *parent, *first, *last, *next;
};
typedef struct { Node *root, *current; unsigned depth; bool too_deep; } Tree;
typedef struct { char bytes[5]; int width; unsigned style; } Glyph;
typedef struct { Glyph *data; size_t len, cap; } Text;
typedef struct {
    FILE *out;
    int width, lines;
    bool color, light;
    unsigned active;
} Renderer;
typedef struct { int indent, quotes; const char *bullet; } Context;

static void die(const char *message)
{
    fprintf(stderr, "mdv: %s\n", message);
    exit(1);
}

static void *allocate(size_t size)
{
    void *p = calloc(1, size ? size : 1);
    if (!p) die("недостаточно памяти");
    return p;
}

static void *grow(void *p, size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) die("документ слишком большой");
    void *result = realloc(p, count * size);
    if (!result) die("недостаточно памяти");
    return result;
}

static void append(Buffer *b, const char *s, size_t n)
{
    if (n > SIZE_MAX - b->len - 1) die("документ слишком большой");
    size_t need = b->len + n + 1;
    if (need > b->cap) {
        b->cap = need > SIZE_MAX / 2 ? need : need * 2;
        b->data = grow(b->data, b->cap, 1);
    }
    if (n) memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static char *copy(const char *s, size_t n)
{
    char *result = allocate(n + 1);
    if (n) memcpy(result, s, n);
    return result;
}

/* Decode strictly: reject overlong sequences, surrogates and out-of-range values. */
static size_t decode(const char *s, size_t n, uint32_t *cp)
{
    if (!n) return 0;
    unsigned char c = (unsigned char)s[0];
    size_t length;
    uint32_t value, minimum;
    if (c < 0x80) { *cp = c; return 1; }
    if (c >= 0xc2 && c <= 0xdf) { length=2; value=c&31; minimum=0x80; }
    else if (c >= 0xe0 && c <= 0xef) { length=3; value=c&15; minimum=0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { length=4; value=c&7; minimum=0x10000; }
    else return 0;
    if (n < length) return 0;
    for (size_t i=1; i<length; i++) {
        c = (unsigned char)s[i];
        if ((c & 0xc0) != 0x80) return 0;
        value = (value << 6) | (c & 63);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        return 0;
    *cp = value;
    return length;
}

static size_t encode(uint32_t cp, char out[5])
{
    size_t n;
    if (cp < 0x80) { out[0]=(char)cp; n=1; }
    else if (cp < 0x800) { out[0]=(char)(0xc0|(cp>>6)); out[1]=(char)(0x80|(cp&63)); n=2; }
    else if (cp < 0x10000) {
        out[0]=(char)(0xe0|(cp>>12)); out[1]=(char)(0x80|((cp>>6)&63));
        out[2]=(char)(0x80|(cp&63)); n=3;
    } else {
        out[0]=(char)(0xf0|(cp>>18)); out[1]=(char)(0x80|((cp>>12)&63));
        out[2]=(char)(0x80|((cp>>6)&63)); out[3]=(char)(0x80|(cp&63)); n=4;
    }
    out[n]='\0';
    return n;
}

static void push(Text *t, uint32_t cp, unsigned style)
{
    if (t->len == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 128;
        t->data = grow(t->data, t->cap, sizeof(*t->data));
    }
    Glyph *g = &t->data[t->len++];
    /* Input must not inject terminal escape sequences or cursor controls. */
    if ((cp < 32 && cp != '\n') || (cp >= 0x7f && cp < 0xa0)) cp=0xfffd;
    g->width = cp == '\n' ? 0 : wcwidth((wchar_t)cp);
    if (g->width < 0) g->width = 1;
    encode(cp, g->bytes);
    g->style = style;
}

static void text_add(Text *t, const char *s, size_t n, unsigned style)
{
    for (size_t i=0; i<n;) {
        uint32_t cp;
        size_t length = decode(s+i, n-i, &cp);
        if (!length) { cp=0xfffd; length=1; }
        if (cp == '\t') {
            for (int j=0; j<4; j++) push(t, ' ', style);
        } else push(t, cp, style);
        i += length;
    }
}

static void text_literal(Text *t, const char *s, unsigned style)
{
    text_add(t, s, strlen(s), style);
}

static void entity_text(Text *t, const char *s, size_t n, unsigned style)
{
    if (n > 3 && s[1]=='#') {
        char *tmp = copy(s+2, n-3), *end;
        int base = 10;
        char *start = tmp;
        if (*start=='x' || *start=='X') { start++; base=16; }
        errno = 0;
        unsigned long cp = strtoul(start, &end, base);
        if (!errno && end != start && !*end) {
            if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp=0xfffd;
            push(t, (uint32_t)cp, style);
            free(tmp);
            return;
        }
        free(tmp);
    } else {
        const ENTITY *entity = entity_lookup(s, n);
        if (entity) {
            push(t, entity->codepoints[0], style);
            if (entity->codepoints[1]) push(t, entity->codepoints[1], style);
            return;
        }
    }
    text_add(t, s, n, style);
}

static char *attribute(const MD_ATTRIBUTE *attr)
{
    Text text = {0};
    for (unsigned i=0; attr->size && attr->substr_offsets[i]<attr->size; i++) {
        MD_OFFSET from = attr->substr_offsets[i], to = attr->substr_offsets[i+1];
        if (attr->substr_types[i] == MD_TEXT_ENTITY)
            entity_text(&text, attr->text+from, to-from, 0);
        else text_add(&text, attr->text+from, to-from, 0);
    }
    Buffer b = {0};
    for (size_t i=0; i<text.len; i++) append(&b, text.data[i].bytes, strlen(text.data[i].bytes));
    free(text.data);
    if (!b.data) return copy("", 0);
    return b.data;
}

static Node *node_new(Tree *tree, int kind, int type)
{
    Node *n = allocate(sizeof(*n));
    n->kind=kind; n->type=type; n->parent=tree->current;
    if (tree->current) {
        if (tree->current->last) tree->current->last->next=n;
        else tree->current->first=n;
        tree->current->last=n;
    } else tree->root=n;
    return n;
}

static int enter_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
    Tree *tree=userdata;
    if (tree->depth++ >= MAX_DEPTH) { tree->too_deep=true; return -1; }
    Node *n=node_new(tree, 0, type);
    tree->current=n;
    switch (type) {
    case MD_BLOCK_H: n->number=((MD_BLOCK_H_DETAIL *)detail)->level; break;
    case MD_BLOCK_OL:
        n->number=((MD_BLOCK_OL_DETAIL *)detail)->start;
        n->tight=((MD_BLOCK_OL_DETAIL *)detail)->is_tight;
        break;
    case MD_BLOCK_UL: n->tight=((MD_BLOCK_UL_DETAIL *)detail)->is_tight; break;
    case MD_BLOCK_LI: {
        MD_BLOCK_LI_DETAIL *d=detail;
        n->task=d->is_task ? (d->task_mark==' ' ? 1 : 2) : 0;
        break;
    }
    case MD_BLOCK_CODE: n->value=attribute(&((MD_BLOCK_CODE_DETAIL *)detail)->lang); break;
    case MD_BLOCK_TH: case MD_BLOCK_TD: n->align=((MD_BLOCK_TD_DETAIL *)detail)->align; break;
    default: break;
    }
    return 0;
}

static int leave_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
    (void)type; (void)detail;
    Tree *tree=userdata;
    tree->current=tree->current->parent;
    tree->depth--;
    return 0;
}

static int enter_span(MD_SPANTYPE type, void *detail, void *userdata)
{
    Tree *tree=userdata;
    if (tree->depth++ >= MAX_DEPTH) { tree->too_deep=true; return -1; }
    Node *n=node_new(tree, 1, type);
    tree->current=n;
    if (type==MD_SPAN_A) n->value=attribute(&((MD_SPAN_A_DETAIL *)detail)->href);
    return 0;
}

static int leave_span(MD_SPANTYPE type, void *detail, void *userdata)
{
    (void)type; (void)detail;
    Tree *tree=userdata;
    tree->current=tree->current->parent;
    tree->depth--;
    return 0;
}

static int text_callback(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *userdata)
{
    Tree *tree=userdata;
    Node *n=node_new(tree, 2, type);
    n->value=copy(text, size);
    n->length=size;
    return 0;
}

static void free_node(Node *n)
{
    while (n) {
        Node *next=n->next;
        free_node(n->first);
        free(n->value); free(n);
        n=next;
    }
}

static void inline_text(Node *node, Text *out, unsigned style)
{
    for (Node *n=node; n; n=n->next) {
        if (n->kind==2) {
            if (n->type==MD_TEXT_BR) push(out, '\n', style);
            else if (n->type==MD_TEXT_SOFTBR) push(out, ' ', style);
            else if (n->type==MD_TEXT_NULLCHAR) push(out, 0xfffd, style);
            else if (n->type==MD_TEXT_ENTITY) entity_text(out, n->value, n->length, style);
            else text_add(out, n->value, n->length, style);
            continue;
        }
        unsigned nested=style;
        if (n->kind==1) {
            switch (n->type) {
            case MD_SPAN_EM: nested|=ITALIC; break;
            case MD_SPAN_STRONG: nested|=BOLD; break;
            case MD_SPAN_CODE: nested|=CODE; break;
            case MD_SPAN_DEL: nested|=STRIKE; break;
            case MD_SPAN_A: nested|=ACCENT|UNDER; break;
            default: break;
            }
        }
        size_t start=out->len;
        inline_text(n->first, out, nested);
        if (n->kind==1 && n->type==MD_SPAN_A && n->value && *n->value) {
            Buffer label={0};
            for (size_t i=start; i<out->len; i++)
                append(&label, out->data[i].bytes, strlen(out->data[i].bytes));
            if (!label.data || strcmp(label.data, n->value)) {
                text_literal(out, " (", style|DIM);
                text_literal(out, n->value, style|DIM);
                text_literal(out, ")", style|DIM);
            }
            free(label.data);
        }
    }
}

static void set_style(Renderer *r, unsigned s)
{
    if (!r->color || r->active==s) return;
    fputs("\033[0m", r->out);
    if (s) {
        fputs("\033[", r->out);
        bool separator=false;
#define ANSI(code) do { if (separator) fputc(';', r->out); fputs(code, r->out); separator=true; } while (0)
        if (s&BOLD) ANSI("1");
        if (s&ITALIC) ANSI("3");
        if (s&UNDER) ANSI("4");
        if (s&STRIKE) ANSI("9");
        if (s&DIM) ANSI("2");
        if (s&CODE) ANSI(r->light ? "35" : "93");
        else if (s&GREEN) ANSI(r->light ? "32" : "92");
        else if (s&PURPLE) ANSI(r->light ? "35" : "95");
        else if (s&NUMBER) ANSI(r->light ? "34" : "96");
        else if (s&ACCENT) ANSI(r->light ? "34" : "96");
        if (s&BACKGROUND) ANSI(r->light ? "47" : "48;5;235");
        fputc('m', r->out);
#undef ANSI
    }
    r->active=s;
}

static void spaces(Renderer *r, int count)
{
    for (int i=0; i<count; i++) fputc(' ', r->out);
}

static void newline(Renderer *r)
{
    set_style(r, 0);
    fputc('\n', r->out);
    r->lines++;
}

static int string_width(const char *s)
{
    Text text={0};
    text_literal(&text, s, 0);
    int width=0;
    for (size_t i=0; i<text.len; i++) width+=text.data[i].width;
    free(text.data);
    return width;
}

static int prefix_width(Context c)
{
    return c.indent + c.quotes*2 + (c.bullet ? string_width(c.bullet) : 0);
}

static void prefix(Renderer *r, Context c, bool first)
{
    set_style(r, 0);
    spaces(r, c.indent);
    set_style(r, ACCENT);
    for (int i=0; i<c.quotes; i++) fputs("│ ", r->out);
    if (c.bullet) {
        if (first) fputs(c.bullet, r->out);
        else spaces(r, string_width(c.bullet));
    }
    set_style(r, 0);
}

static int glyph_width(const Text *t, size_t start, size_t end)
{
    int total=0;
    for (size_t i=start; i<end; i++) total+=t->data[i].width;
    return total;
}

typedef struct { size_t end, next; } Slice;
static Slice slice(const Text *t, size_t start, int width, bool words)
{
    int used=0;
    size_t end=start, space=SIZE_MAX;
    while (end<t->len && t->data[end].bytes[0]!='\n') {
        if (used+t->data[end].width>width && end>start) break;
        used+=t->data[end].width;
        if (t->data[end].bytes[0]==' ') space=end;
        end++;
    }
    size_t next=end;
    if (end<t->len && t->data[end].bytes[0]=='\n') next++;
    else if (end<t->len && words && space!=SIZE_MAX && space>start) {
        end=space; next=space+1;
        while (next<t->len && t->data[next].bytes[0]==' ') next++;
    }
    return (Slice){end, next};
}

static void emit(Renderer *r, const Text *t, size_t start, size_t end)
{
    for (size_t i=start; i<end; i++) {
        set_style(r, t->data[i].style);
        fputs(t->data[i].bytes, r->out);
    }
}

static void paragraph(Renderer *r, Text *text, Context c, bool code)
{
    int available=r->width-prefix_width(c);
    if (available<2) available=2;
    size_t start=0;
    bool first=true;
    do {
        Slice part=slice(text, start, available, !code);
        prefix(r, c, first);
        emit(r, text, start, part.end);
        if (code) {
            set_style(r, BACKGROUND);
            spaces(r, available-glyph_width(text, start, part.end));
        }
        newline(r);
        first=false;
        start=part.next;
    } while (start<text->len);
}

static void rule(Renderer *r, Context c)
{
    prefix(r, c, false);
    set_style(r, ACCENT|DIM);
    int width=r->width-prefix_width(c);
    for (int i=0; i<width; i++) fputs("─", r->out);
    newline(r);
}

static bool word_is(const char *s, size_t n, const char *words)
{
    for (const char *p=words; *p;) {
        const char *end=strchr(p, ' ');
        size_t length=end ? (size_t)(end-p) : strlen(p);
        if (length==n && !memcmp(s, p, n)) return true;
        if (!end) break;
        p=end+1;
    }
    return false;
}

/* A small lexer keeps code readable without invoking an external highlighter. */
static void highlight(Text *t, const char *s, size_t n, const char *lang)
{
    bool c_like=word_is(lang, strlen(lang), "c h cpp c++ cc hpp java js javascript ts typescript go rust rs cs csharp css");
    bool hash=word_is(lang, strlen(lang), "py python sh bash zsh fish ruby rb yaml yml toml");
    bool known=c_like || hash || !strcmp(lang,"json");
    const char *keywords="auto break case char class const continue def default delete do double elif else enum except export extern false False finally float for from func function if import in include inline int interface lambda let long namespace new None null nullptr package pass print private protected public raise return short signed sizeof static string struct super switch template this throw true True try typedef typeof union unsigned using var virtual void volatile while with yield bool boolean async await fn impl match mut pub self use";
    for (size_t i=0; i<n;) {
        size_t end=i+1;
        unsigned style=BACKGROUND;
        unsigned char ch=(unsigned char)s[i];
        if (known && ((hash && ch=='#') || (c_like && ch=='/' && i+1<n && s[i+1]=='/'))) {
            while (end<n && s[end]!='\n') end++;
            style|=DIM;
        } else if (c_like && ch=='/' && i+1<n && s[i+1]=='*') {
            end=i+2;
            while (end+1<n && !(s[end]=='*' && s[end+1]=='/')) end++;
            end=end+1<n ? end+2 : n;
            style|=DIM;
        } else if (known && (ch=='\'' || ch=='"' || ch=='`')) {
            while (end<n) {
                if (s[end]=='\\' && end+1<n) { end+=2; continue; }
                if (s[end++]==(char)ch) break;
            }
            style|=GREEN;
        } else if (known && (isalpha(ch) || ch=='_')) {
            while (end<n && (isalnum((unsigned char)s[end]) || s[end]=='_')) end++;
            if (word_is(s+i, end-i, keywords)) style|=PURPLE|BOLD;
        } else if (known && isdigit(ch)) {
            while (end<n && (isalnum((unsigned char)s[end]) || s[end]=='.' || s[end]=='_')) end++;
            style|=NUMBER;
        } else {
            uint32_t cp;
            size_t length=decode(s+i,n-i,&cp);
            end=i+(length ? length : 1);
        }
        text_add(t, s+i, end-i, style);
        i=end;
    }
}

static void render_nodes(Renderer *, Node *, Context);

typedef struct { Text text; size_t pos; int align; } Cell;
typedef struct { Cell *cells; size_t count; bool header; } Row;

static void table(Renderer *r, Node *node, Context c)
{
    Row *rows=NULL;
    size_t count=0, cols=0;
    for (Node *section=node->first; section; section=section->next) {
        for (Node *tr=section->first; tr; tr=tr->next) {
            rows=grow(rows,count+1,sizeof(*rows));
            Row *row=&rows[count++];
            *row=(Row){0}; row->header=section->type==MD_BLOCK_THEAD;
            for (Node *td=tr->first; td; td=td->next) {
                row->cells=grow(row->cells,row->count+1,sizeof(*row->cells));
                Cell *cell=&row->cells[row->count++];
                *cell=(Cell){0}; cell->align=td->align;
                inline_text(td->first,&cell->text,row->header ? BOLD|ACCENT : 0);
            }
            if (row->count>cols) cols=row->count;
        }
    }
    int available=r->width-prefix_width(c);
    /* When columns cannot fit, preserve all cell contents as stacked rows. */
    bool stacked=cols>(size_t)(available>1 ? (available-1)/5 : 0);
    int *widths=allocate((cols ? cols : 1)*sizeof(*widths));
    if (!stacked && cols) {
        for (size_t i=0; i<cols; i++) widths[i]=1;
        for (size_t j=0; j<count; j++) for (size_t i=0; i<rows[j].count; i++) {
            int w=glyph_width(&rows[j].cells[i].text,0,rows[j].cells[i].text.len);
            if (w>widths[i]) widths[i]=w;
        }
        int budget=available-(int)(3*cols+1);
        int total=0;
        for (size_t i=0; i<cols; i++) { if (widths[i]>budget) widths[i]=budget; total+=widths[i]; }
        while (total>budget) {
            size_t largest=0;
            for (size_t i=1; i<cols; i++) if (widths[i]>widths[largest]) largest=i;
            widths[largest]--; total--;
        }
    }
    for (size_t j=0; j<count; j++) {
        Row *row=&rows[j];
        if (stacked) {
            for (size_t i=0; i<row->count; i++) paragraph(r,&row->cells[i].text,c,false);
        } else {
            bool more;
            do {
                more=false;
                prefix(r,c,false);
                set_style(r,ACCENT|DIM); fputs("│",r->out);
                for (size_t i=0; i<cols; i++) {
                    set_style(r,0); fputc(' ',r->out);
                    if (i<row->count) {
                        Cell *cell=&row->cells[i];
                        Slice part=slice(&cell->text,cell->pos,widths[i],true);
                        int padding=widths[i]-glyph_width(&cell->text,cell->pos,part.end);
                        int left=cell->align==MD_ALIGN_RIGHT ? padding : cell->align==MD_ALIGN_CENTER ? padding/2 : 0;
                        spaces(r,left);
                        emit(r,&cell->text,cell->pos,part.end);
                        set_style(r,0); spaces(r,padding-left);
                        cell->pos=part.next;
                        if (cell->pos<cell->text.len) more=true;
                    } else spaces(r,widths[i]);
                    set_style(r,ACCENT|DIM); fputs(" │",r->out);
                }
                newline(r);
            } while (more);
        }
        if (row->header || stacked) {
            if (stacked) rule(r,c);
            else {
                prefix(r,c,false); set_style(r,ACCENT|DIM); fputs("├",r->out);
                for (size_t i=0; i<cols; i++) {
                    for (int k=0; k<widths[i]+2; k++) fputs("─",r->out);
                    fputs(i+1<cols ? "┼" : "┤",r->out);
                }
                newline(r);
            }
        }
        for (size_t i=0; i<row->count; i++) free(row->cells[i].text.data);
        free(row->cells);
    }
    free(rows); free(widths);
    newline(r);
}

static void render_list(Renderer *r, Node *node, Context c)
{
    unsigned long number=node->number;
    for (Node *item=node->first; item; item=item->next) {
        char bullet[40];
        if (item->task) snprintf(bullet,sizeof(bullet),"[%c] ",item->task==2 ? 'x' : ' ');
        else if (node->type==MD_BLOCK_OL) snprintf(bullet,sizeof(bullet),"%lu. ",number++);
        else snprintf(bullet,sizeof(bullet),"• ");
        Context entry=c;
        if (c.bullet) entry.indent+=string_width(c.bullet);
        entry.bullet=bullet;
        render_nodes(r,item->first,entry);
        if (!node->tight) newline(r);
    }
    if (!node->parent || node->parent->type!=MD_BLOCK_LI) newline(r);
}

static void render_nodes(Renderer *r, Node *nodes, Context c)
{
    for (Node *n=nodes; n; n=n->next) {
        if (n->kind!=0) {
            /* MD4C omits paragraph blocks inside tight list items. */
            Node *last=n;
            while (last->next && last->next->kind!=0) last=last->next;
            Node *next=last->next;
            last->next=NULL;
            Text text={0};
            inline_text(n,&text,0);
            last->next=next;
            paragraph(r,&text,c,false);
            free(text.data);
            n=last;
        } else switch (n->type) {
        case MD_BLOCK_DOC: render_nodes(r,n->first,c); break;
        case MD_BLOCK_QUOTE: {
            Context nested=c; nested.quotes++;
            render_nodes(r,n->first,nested);
            break;
        }
        case MD_BLOCK_UL: case MD_BLOCK_OL: render_list(r,n,c); break;
        case MD_BLOCK_HR: rule(r,c); newline(r); break;
        case MD_BLOCK_H: case MD_BLOCK_P: {
            Text text={0};
            inline_text(n->first,&text,n->type==MD_BLOCK_H ? BOLD|ACCENT : 0);
            paragraph(r,&text,c,false);
            free(text.data);
            if (n->type==MD_BLOCK_H && n->number<=2) rule(r,c);
            if (!(n->parent && n->parent->type==MD_BLOCK_LI && n->parent->parent->tight)) newline(r);
            break;
        }
        case MD_BLOCK_CODE: case MD_BLOCK_HTML: {
            Buffer code={0};
            for (Node *line=n->first; line; line=line->next) append(&code,line->value,line->length);
            Text text={0};
            highlight(&text,code.data ? code.data : "",code.len,n->value ? n->value : "");
            paragraph(r,&text,c,true);
            newline(r);
            free(code.data); free(text.data);
            break;
        }
        case MD_BLOCK_TABLE: table(r,n,c); break;
        default: render_nodes(r,n->first,c); break;
        }
        if (c.bullet) {
            c.indent+=string_width(c.bullet);
            c.bullet=NULL;
        }
    }
}

static void help(void)
{
    puts("mdv — чтение Markdown в терминале\n\n"
         "Использование: mdv [параметры] [файл|-]\n\n"
         "  --no-pager          вывести без просмотрщика\n"
         "  --no-color          отключить цвет\n"
         "  -w, --width N       ширина текста (от 20 колонок)\n"
         "  --theme dark|light  тема терминала (по умолчанию dark)\n"
         "  -h, --help          справка\n"
         "  --version           версия\n\n"
         "Без файла: чтение stdin или справка при запуске в терминале.\n"
         "В less: ↑/↓, PgUp/PgDn — прокрутка; / — поиск; q — выход.");
}

static bool read_document(FILE *file, Buffer *b)
{
    char chunk[8192];
    size_t size;
    while ((size=fread(chunk,1,sizeof(chunk),file))>0) {
        if (b->len>UINT_MAX-size) { errno=EFBIG; return false; }
        append(b,chunk,size);
    }
    return !ferror(file);
}

static int copy_output(FILE *from)
{
    char data[8192];
    size_t n;
    while ((n=fread(data,1,sizeof(data),from))>0)
        if (fwrite(data,1,n,stdout)!=n) return errno==EPIPE ? 0 : 1;
    if (ferror(from)) return 1;
    if (fflush(stdout)==EOF) return errno==EPIPE ? 0 : 1;
    return 0;
}

static int show_output(FILE *rendered, bool pager)
{
    rewind(rendered);
    if (!pager) return copy_output(rendered);
    fflush(stdout);
    pid_t pid=fork();
    if (pid<0) return copy_output(rendered);
    if (!pid) {
        if (dup2(fileno(rendered),STDIN_FILENO)<0) _exit(1);
        execlp("less","less","-R","-F","-X","-+S","-+N","-+n",(char *)NULL);
        /* less is optional; fall back to ordinary output when unavailable. */
        _exit(copy_output(rendered));
    }
    int status;
    while (waitpid(pid,&status,0)<0) if (errno!=EINTR) return 1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 128+WTERMSIG(status) : 1;
}

int main(int argc, char **argv)
{
    if (!setlocale(LC_CTYPE,"")) setlocale(LC_CTYPE,"C.UTF-8");
    signal(SIGPIPE,SIG_IGN);
    bool no_pager=false, no_color=false, light=false;
    int width=100;
    static const struct option options[]={
        {"no-pager",no_argument,NULL,1000}, {"no-color",no_argument,NULL,1001},
        {"theme",required_argument,NULL,1002}, {"version",no_argument,NULL,1003},
        {"width",required_argument,NULL,'w'}, {"help",no_argument,NULL,'h'}, {NULL,0,NULL,0}
    };
    int option;
    opterr=0;
    while ((option=getopt_long(argc,argv,"hw:",options,NULL))!=-1) {
        switch (option) {
        case 'h': help(); return 0;
        case 'w': {
            char *end;
            errno=0;
            long n=strtol(optarg,&end,10);
            if (errno || !*optarg || *end || n<20 || n>MAX_WIDTH) {
                fprintf(stderr,"mdv: ширина должна быть целым числом от 20 до %d\n",MAX_WIDTH);
                return 2;
            }
            width=(int)n; break;
        }
        case 1000: no_pager=true; break;
        case 1001: no_color=true; break;
        case 1002:
            if (!strcmp(optarg,"light")) light=true;
            else if (!strcmp(optarg,"dark")) light=false;
            else { fputs("mdv: тема должна быть dark или light\n",stderr); return 2; }
            break;
        case 1003: puts("mdv " VERSION); return 0;
        default: fputs("mdv: неверные параметры; см. mdv --help\n",stderr); return 2;
        }
    }
    if (argc-optind>1) { fputs("mdv: укажите один файл\n",stderr); return 2; }
    if (optind==argc && isatty(STDIN_FILENO)) { help(); return 0; }
    const char *path=optind<argc ? argv[optind] : "-";
    FILE *input=!strcmp(path,"-") ? stdin : fopen(path,"rb");
    if (!input) { fprintf(stderr,"mdv: не удалось прочитать %s: %s\n",path,strerror(errno)); return 1; }
    Buffer source={0};
    bool read_ok=read_document(input,&source);
    int read_errno=errno;
    if (input!=stdin) fclose(input);
    if (!read_ok) {
        fprintf(stderr,"mdv: не удалось прочитать %s: %s\n",path,strerror(read_errno));
        free(source.data); return 1;
    }
    if (!source.data) source.data=copy("",0);
    size_t offset=source.len>=3 && !memcmp(source.data,"\xef\xbb\xbf",3) ? 3 : 0;
    for (size_t i=offset; i<source.len;) {
        uint32_t cp;
        size_t n=decode(source.data+i,source.len-i,&cp);
        if (!n) { free(source.data); fputs("mdv: файл должен быть в UTF-8\n",stderr); return 1; }
        i+=n;
    }
    Tree tree={0};
    MD_PARSER parser={0};
    parser.flags=MD_DIALECT_GITHUB;
    parser.enter_block=enter_block; parser.leave_block=leave_block;
    parser.enter_span=enter_span; parser.leave_span=leave_span; parser.text=text_callback;
    int parsed=md_parse(source.data+offset,(MD_SIZE)(source.len-offset),&parser,&tree);
    free(source.data);
    if (parsed) {
        free_node(tree.root);
        fputs(tree.too_deep ? "mdv: слишком большая вложенность Markdown\n" : "mdv: ошибка разбора Markdown\n",stderr);
        return 1;
    }
    struct winsize size={0};
    bool terminal=isatty(STDOUT_FILENO);
    if (terminal && ioctl(STDOUT_FILENO,TIOCGWINSZ,&size)==0 && size.ws_col && width>size.ws_col)
        width=size.ws_col;
    FILE *output=terminal && !no_pager ? tmpfile() : stdout;
    if (!output) { free_node(tree.root); die("не удалось создать временный файл для просмотра"); }
    const char *term=getenv("TERM");
    Renderer r={.out=output,.width=width,.light=light,
        .color=terminal && !no_color && !getenv("NO_COLOR") && (!term || strcmp(term,"dumb"))};
    Context context={.indent=width>=24 ? 2 : 0};
    newline(&r);
    Text title={0};
    const char *name=!strcmp(path,"-") ? "Markdown · stdin" : strrchr(path,'/');
    if (strcmp(path,"-")) name=name ? name+1 : path;
    text_literal(&title,name,BOLD|ACCENT);
    paragraph(&r,&title,context,false);
    free(title.data);
    rule(&r,context); newline(&r);
    render_nodes(&r,tree.root,context);
    free_node(tree.root);
    if (ferror(output) || fflush(output)==EOF) {
        int result=errno==EPIPE ? 0 : 1;
        if (output!=stdout) fclose(output);
        return result;
    }
    if (output==stdout) return 0;
    int result=show_output(output,r.lines>=(size.ws_row ? size.ws_row : 24));
    fclose(output);
    return result;
}
