/* pakbuild.c — build a Quake PAK from a list of files.
 *
 * Companion to pakmerge.c. The Cronopio Quake cart reads its data (including
 * music) from the baked pak, so to add the soundtrack we pack the loose
 * music/track*.ogg files into a pak that the build then merges into the ROM.
 *
 * Usage:  pakbuild out.pak base_dir file [file ...]
 * Each file's lump name is its path relative to base_dir (forward slashes),
 * e.g. base_dir=basegame/id1 and file=basegame/id1/music/track02.ogg ->
 * lump "music/track02.ogg". Little-endian host (same as pakmerge.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct { char name[56]; int32_t filepos, filelen; } dentry_t;
typedef struct { char id[4]; int32_t dirofs, dirlen; } header_t;

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s out.pak base_dir file [file ...]\n", argv[0]);
        return 2;
    }
    const char *outpath = argv[1];
    const char *base = argv[2];
    size_t baselen = strlen(base);
    int n = argc - 3;

    dentry_t *dir = calloc((size_t)n, sizeof(*dir));
    if (!dir) return 1;

    FILE *out = fopen(outpath, "wb");
    if (!out) { fprintf(stderr, "pakbuild: cannot write %s\n", outpath); return 1; }
    header_t hdr = { {'P','A','C','K'}, 0, 0 };
    fwrite(&hdr, 1, sizeof(hdr), out);

    int32_t pos = (int32_t)sizeof(hdr);
    int kept = 0;
    for (int i = 0; i < n; i++) {
        const char *path = argv[3 + i];
        /* lump name = path minus base_dir prefix + a separator. */
        const char *rel = path;
        if (strncmp(path, base, baselen) == 0) {
            rel = path + baselen;
            while (*rel == '/' || *rel == '\\') rel++;
        }
        if (strlen(rel) >= sizeof(dir[0].name)) {
            fprintf(stderr, "pakbuild: lump name too long: %s\n", rel);
            return 1;
        }

        FILE *f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "pakbuild: cannot open %s\n", path); return 1; }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        void *buf = malloc((size_t)len);
        if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
            fprintf(stderr, "pakbuild: read failed on %s\n", path);
            return 1;
        }
        fclose(f);
        fwrite(buf, 1, (size_t)len, out);
        free(buf);

        memset(dir[kept].name, 0, sizeof(dir[kept].name));
        /* normalise backslashes to forward slashes in the lump name */
        for (size_t k = 0; rel[k]; k++)
            dir[kept].name[k] = (rel[k] == '\\') ? '/' : rel[k];
        dir[kept].filepos = pos;
        dir[kept].filelen = (int32_t)len;
        pos += (int32_t)len;
        kept++;
    }

    int32_t dirofs = pos;
    fwrite(dir, sizeof(dentry_t), (size_t)kept, out);

    hdr.dirofs = dirofs;
    hdr.dirlen = kept * (int)sizeof(dentry_t);
    fseek(out, 0, SEEK_SET);
    fwrite(&hdr, 1, sizeof(hdr), out);
    fclose(out);

    fprintf(stderr, "pakbuild: %s <- %d file(s), %d data bytes\n",
            outpath, kept, dirofs - (int)sizeof(hdr));
    return 0;
}
