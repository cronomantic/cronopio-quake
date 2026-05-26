/* pakmerge.c — merge several Quake PAK archives into one.
 *
 * Cronopio bakes a single --rom blob and cron_rom_mount serves it as one file
 * (pak0.pak). Quake itself loads pak0.pak, pak1.pak, … until one is missing,
 * with later paks taking precedence (their search path is prepended, so they
 * are searched first; COM_SearchPak returns the first directory match). To get
 * the registered episodes 2-4 (which live in pak1) without a multi-file ROM we
 * merge the paks here and bake the result as the single pak0.pak.
 *
 * Usage:  pakmerge out.pak in0.pak [in1.pak …]
 * Inputs are given in Quake load order (pak0 first). On a duplicate lump name
 * the LAST input wins (matching Quake precedence); we emit that entry and skip
 * the earlier ones, and emit higher-priority paks' directories first so a
 * first-match scan returns the winning copy.
 *
 * PAK format (little-endian; host is LE, so no byte-swapping): a 12-byte header
 * { char id[4]="PACK"; int32 dirofs; int32 dirlen; }, file data, then the
 * directory: dirlen/64 entries of { char name[56]; int32 filepos; int32 filelen; }.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct { char name[56]; int32_t filepos, filelen; } dentry_t;
typedef struct { char id[4]; int32_t dirofs, dirlen; } header_t;

static unsigned char *slurp(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "pakmerge: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "pakmerge: read failed on %s\n", path);
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_len = n;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s out.pak in0.pak [in1.pak ...]\n", argv[0]);
        return 2;
    }
    const char *outpath = argv[1];
    int n_in = argc - 2;

    unsigned char **bufs = calloc(n_in, sizeof(*bufs));
    long           *lens = calloc(n_in, sizeof(*lens));
    for (int i = 0; i < n_in; i++) {
        bufs[i] = slurp(argv[2 + i], &lens[i]);
        if (!bufs[i]) return 1;
        if (lens[i] < (long)sizeof(header_t) || memcmp(bufs[i], "PACK", 4) != 0) {
            fprintf(stderr, "pakmerge: %s is not a PACK file\n", argv[2 + i]);
            return 1;
        }
    }

    /* Collect the winning entries. Walk inputs from LAST (highest priority) to
     * first; keep the first time we see each name. */
    dentry_t *merged = NULL;          /* {name, filepos in SOURCE, filelen} */
    int      *srcidx = NULL;          /* which input buffer each entry came from */
    int       n_merged = 0, cap = 0;

    for (int i = n_in - 1; i >= 0; i--) {
        header_t *h = (header_t *)bufs[i];
        int nfiles = h->dirlen / (int)sizeof(dentry_t);
        dentry_t *dir = (dentry_t *)(bufs[i] + h->dirofs);
        for (int j = 0; j < nfiles; j++) {
            int dup = 0;
            for (int k = 0; k < n_merged; k++)
                if (memcmp(merged[k].name, dir[j].name, 56) == 0) { dup = 1; break; }
            if (dup) continue;
            if (n_merged == cap) {
                cap = cap ? cap * 2 : 1024;
                merged = realloc(merged, (size_t)cap * sizeof(*merged));
                srcidx = realloc(srcidx, (size_t)cap * sizeof(*srcidx));
            }
            merged[n_merged] = dir[j];
            srcidx[n_merged] = i;
            n_merged++;
        }
    }

    FILE *out = fopen(outpath, "wb");
    if (!out) { fprintf(stderr, "pakmerge: cannot write %s\n", outpath); return 1; }

    /* Header is fixed up at the end; reserve its 12 bytes. */
    header_t hdr = { {'P','A','C','K'}, 0, 0 };
    fwrite(&hdr, 1, sizeof(hdr), out);

    /* Copy each kept file's data, recording its new offset. */
    int32_t pos = (int32_t)sizeof(hdr);
    for (int k = 0; k < n_merged; k++) {
        int i = srcidx[k];
        int32_t src = merged[k].filepos, len = merged[k].filelen;
        fwrite(bufs[i] + src, 1, (size_t)len, out);
        merged[k].filepos = pos;       /* rewrite to the merged offset */
        pos += len;
    }

    /* Directory. */
    int32_t dirofs = pos;
    for (int k = 0; k < n_merged; k++)
        fwrite(&merged[k], 1, sizeof(dentry_t), out);

    /* Fix up the header. */
    hdr.dirofs = dirofs;
    hdr.dirlen = n_merged * (int)sizeof(dentry_t);
    fseek(out, 0, SEEK_SET);
    fwrite(&hdr, 1, sizeof(hdr), out);
    fclose(out);

    fprintf(stderr, "pakmerge: %s <- %d input(s), %d files, %d data bytes\n",
            outpath, n_in, n_merged, dirofs - (int)sizeof(hdr));
    return 0;
}
