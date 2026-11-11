/* PixCarsVita - static scene loader. See scene.h and tools/vitascene.py. */
#include "scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *p, *end;
    int err;
} Cur;

static const void *take(Cur *c, size_t n)
{
    size_t padded = (n + 3) & ~(size_t)3;
    if (c->err || (size_t)(c->end - c->p) < padded) {
        c->err = 1;
        return NULL;
    }
    void *r = c->p;
    c->p += padded;
    return r;
}

static uint32_t u32(Cur *c)
{
    const uint32_t *v = take(c, 4);
    return v ? *v : 0;
}

int vcs_load_mem(VcsScene *s, uint8_t *buf, size_t size)
{
    memset(s, 0, sizeof *s);
    s->file = buf;
    Cur c = { buf, buf + size, 0 };
    const char *magic = take(&c, 4);
    if (!magic || memcmp(magic, "VCS1", 4) != 0)
        return -1;
    s->n_tex = u32(&c);
    s->n_mat = u32(&c);
    s->n_mesh = u32(&c);
    s->n_inst = u32(&c);
    const float *st = take(&c, 24);
    if (c.err || !st)
        return -2;
    memcpy(s->start, st, 12);
    memcpy(s->start_dir, st + 3, 12);

    s->tex = calloc(s->n_tex ? s->n_tex : 1, sizeof(VcmTexture));
    for (uint32_t i = 0; i < s->n_tex && !c.err; i++) {
        VcmTexture *t = &s->tex[i];
        t->format = u32(&c);
        t->width = u32(&c);
        t->height = u32(&c);
        t->n_levels = u32(&c);
        if (t->n_levels > 16) {
            c.err = 1;
            break;
        }
        t->levels = calloc(t->n_levels ? t->n_levels : 1, sizeof(VcmLevel));
        for (uint32_t l = 0; l < t->n_levels && !c.err; l++) {
            t->levels[l].size = u32(&c);
            t->levels[l].data = take(&c, t->levels[l].size);
        }
    }
    s->mat = (VcmMaterial *)take(&c, sizeof(VcmMaterial) * s->n_mat);

    s->mesh = calloc(s->n_mesh ? s->n_mesh : 1, sizeof(VcsMesh));
    for (uint32_t i = 0; i < s->n_mesh && !c.err; i++) {
        VcsMesh *m = &s->mesh[i];
        const float *sph = take(&c, 16);
        if (!sph)
            break;
        memcpy(m->center, sph, 12);
        m->radius = sph[3];
        m->n_batch = u32(&c);
        if (m->n_batch > 4096) {
            c.err = 1;
            break;
        }
        m->batch = calloc(m->n_batch ? m->n_batch : 1, sizeof(VcsBatch));
        for (uint32_t b = 0; b < m->n_batch && !c.err; b++) {
            VcsBatch *bt = &m->batch[b];
            bt->material = u32(&c);
            bt->n_verts = u32(&c);
            bt->n_idx = u32(&c);
            if (bt->material >= s->n_mat) {
                c.err = 1;
                break;
            }
            bt->pos = take(&c, 12u * bt->n_verts);
            bt->uv = take(&c, 8u * bt->n_verts);
            bt->rgba = take(&c, 4u * bt->n_verts);
            bt->strip = take(&c, 2u * bt->n_idx);
        }
    }
    s->inst = (VcsInstance *)take(&c, sizeof(VcsInstance) * s->n_inst);
    if (c.err)
        return -3;
    for (uint32_t i = 0; i < s->n_inst; i++)
        if (s->inst[i].mesh >= s->n_mesh)
            return -4;
    return 0;
}

int vcs_load_file(VcsScene *s, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -10;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return -11;
    }
    fclose(f);
    int r = vcs_load_mem(s, buf, (size_t)n);
    if (r)
        vcs_free(s);
    return r;
}

void vcs_free(VcsScene *s)
{
    if (s->tex)
        for (uint32_t i = 0; i < s->n_tex; i++)
            free(s->tex[i].levels);
    if (s->mesh)
        for (uint32_t i = 0; i < s->n_mesh; i++)
            free(s->mesh[i].batch);
    free(s->tex);
    free(s->mesh);
    free(s->file);
    memset(s, 0, sizeof *s);
}
