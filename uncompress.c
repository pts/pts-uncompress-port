/* vi: set sw=4 ts=4: */
/*
 * uncompress.c: a C89 and C++98 implementation of .Z decompressor
 * porting by pts@fazekas.hu at Tue Nov  5 12:50:56 CET 2024
 *
 * Based on busybox-1.21.1/archival/libarchive/decompress_uncompress.c (2013-05-23)
 * https://www.busybox.net/downloads/busybox-1.21.1.tar.bz2
 *
 * No substantial changes since that in
 * busybox-1.37.0/archival/libarchive/decompress_uncompress.c (2023-01-05).
 *
 * More details here: https://en.wikipedia.org/wiki/Compress_(software)
 *
 * uncompress for busybox -- (c) 2002 Robert Griebl
 *
 * based on the original compress42.c source
 * (see disclaimer below)
 */

/* (N)compress42.c - File compression ala IEEE Computer, Mar 1992.
 *
 * Authors:
 *   Spencer W. Thomas   (decvax!harpo!utah-cs!utah-gr!thomas)
 *   Jim McKie           (decvax!mcvax!jim)
 *   Steve Davies        (decvax!vax135!petsd!peora!srd)
 *   Ken Turkowski       (decvax!decwrl!turtlevax!ken)
 *   James A. Woods      (decvax!ihnp4!ames!jaw)
 *   Joe Orost           (decvax!vax135!petsd!joe)
 *   Dave Mack           (csu@alembic.acs.com)
 *   Peter Jannesen, Network Communication Systems
 *                       (peter@ncs.nl)
 *
 * marc@suse.de : a small security fix for a buffer overflow
 *
 * [... History snipped ...]
 *
 */

#include <fcntl.h>  /* O_BINARY. */
#include <string.h>
#if defined(_WIN32) || defined(MSDOS) || defined(__DOS__)
#  include <io.h>
#else
#  include <unistd.h>
#endif

#if defined(_WIN32) && defined(__WATCOMC__)
/* Overrides lib386/nt/clib3r.lib / mbcupper.o
 * Source: https://github.com/open-watcom/open-watcom-v2/blob/master/bld/clib/mbyte/c/mbcupper.c
 * Overridden implementation calls CharUpperA in USER32.DLL:
 * https://docs.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-charuppera
 *
 * This function is a transitive dependency of _cstart() with main() in
 * OpenWatcom. By overridding it, we remove the transitive dependency of all
 * .exe files compiled with `owcc -bwin32' on USER32.DLL.
 *
 * This is a simplified implementation, it keeps non-ASCII characters intact.
 */
unsigned int _mbctoupper(unsigned int c) {
  return (c - 'a' + 0U <= 'z' - 'a' + 0U)  ? c + 'A' - 'a' : c;
}
#endif

/* Default input buffer size */
#define IBUFSIZ 2048

/* Default output buffer size */
#define OBUFSIZ 2048

/* Defines for third byte of header */
#define BIT_MASK        0x1f    /* Mask for 'number of compresssion bits'       */
                                /* Masks 0x20 and 0x40 are free.                */
                                /* I think 0x20 should mean that there is       */
                                /* a fourth header byte (for expansion).        */
#define BLOCK_MODE      0x80    /* Block compression if table is full and       */
                                /* compression rate is dropping flush tables    */
                                /* the next two codes should not be changed lightly, as they must not   */
                                /* lie within the contiguous general code space.                        */
#define FIRST   257     /* first free entry */
#define CLEAR   256     /* table clear output code */

#define INIT_BITS 9     /* initial number of bits/code */


/* machine variants which require cc -Dmachine:  pdp11, z8000, DOS */
#define HBITS      17   /* 50% occupancy */
#define HSIZE      (1<<HBITS)
#define HMASK      (HSIZE-1)    /* unused */
#define HPRIME     9941         /* unused */
#define BITS       16
#undef  MAXSEG_64K              /* unused */
#define MAXCODE(n) (1L << (n))

#define htabof(i)               htab[i]
#define codetabof(i)            codetab[i]
#define tab_prefixof(i)         codetabof(i)
#define tab_suffixof(i)         ((unsigned char *)(htab))[i]
#define de_stack                ((unsigned char *)&(htab[HSIZE-1]))
#define clear_tab_prefixof()    memset(codetab, 0, 256)

ssize_t full_write(int fd, const void *buf, size_t count) {
	const char *buf0 = (const char*)buf;
	ssize_t got;
	while (count > 0 && (got = write(fd, buf, count)) > 0) {
		buf = (const void*)((const char*)buf + got);
		count -= got;
	}
	return (const char*)buf - buf0;
}

void error_msg(const char *msg) {
	full_write(2, msg, strlen(msg));
}

#if defined(_WIN32) || defined(MSDOS) || defined(__DOS__)
#  define ERROR_MSG(msg) error_msg(msg "\r\n")
#else
#  define ERROR_MSG(msg) error_msg(msg "\n")
#endif

/* 399424 bytes in total in these buffers. */
unsigned char inbuf[IBUFSIZ + 64];  /* wasn't zeroed out before, maybe can xmalloc? */
unsigned char outbuf[OBUFSIZ + 2048];
unsigned char htab[HSIZE];
unsigned short codetab[HSIZE];

/*
 * Decompress stdin to stdout.  This routine adapts to the codes in the
 * file building the "string" table on-the-fly; requiring no table to
 * be stored in the compressed file.
 */
int main(int argc, char **argv) {
#define src_fd 0  /* const int src_fd = 0; */  /* stdin. */
#define dst_fd 1  /* const int dst_fd = 1; */  /* stdout. */
	unsigned char *stackp;
	int finchar;
	long oldcode;
	long incode;
	int inbits;
	int posbits;
	int outpos;
	int insize;
	int bitmask;
	long free_ent;
	long maxcode;
	long maxmaxcode;
	int n_bits;
	int rsize;
	int i, e, o;
	/* Hmm, these were statics - why?! */
	/* user settable max # bits/code */
	int maxbits; /* = BITS; */
	/* block compress mode -C compatible with 2.0 */
	int block_mode; /* = BLOCK_MODE; */

	(void)argc; (void)argv;
#if O_BINARY  /* E.g. for _WIN32 and MSDOS. */
	setmode(src_fd, O_BINARY);
	setmode(dst_fd, O_BINARY);
#endif
	for (i = 0; i < 3; ) {
		if ((rsize = read(src_fd, inbuf + i, 3 - i)) <= 0) {
			ERROR_MSG("error reading header");
			return 3;
		}
		i += rsize;
	}
	rsize = insize = 0;
	if (inbuf[0] != 0x1f || inbuf[1] != 0x9d) {
		ERROR_MSG("invalid magic");
		return 2;
	}

	maxbits = inbuf[2] & BIT_MASK;
	block_mode = inbuf[2] & BLOCK_MODE;
	maxmaxcode = MAXCODE(maxbits);

	if (maxbits > BITS) {
		ERROR_MSG("BITS > 16 not implemented");
		return 3;
	}

	n_bits = INIT_BITS;
	maxcode = MAXCODE(INIT_BITS) - 1;
	bitmask = (1 << INIT_BITS) - 1;
	oldcode = -1;
	finchar = 0;
	outpos = 0;
	posbits = 0 << 3;

	free_ent = ((block_mode) ? FIRST : 256);

	/* As above, initialize the first 256 entries in the table. */
	/*clear_tab_prefixof(); - .bss is automatically zero-initialized */

	for (i = 255; i >= 0; --i)
		tab_suffixof(i) = (unsigned char) i;

	do {
 resetbuf:
		{
			o = posbits >> 3;
			e = insize - o;

			for (i = 0; i < e; ++i)
				inbuf[i] = inbuf[i + o];

			insize = e;
			posbits = 0;
		}

		if (insize < (int) (IBUFSIZ + 64) - IBUFSIZ) {
			rsize = read(src_fd, inbuf + insize, IBUFSIZ);
			if (rsize < 0) {
				ERROR_MSG("read error");
				return 3;
			}
			insize += rsize;
		}

		inbits = ((rsize > 0) ? (insize - insize % n_bits) << 3 :
				  (insize << 3) - (n_bits - 1));

		while (inbits > posbits) {
			long code;

			if (free_ent > maxcode) {
				posbits =
					((posbits - 1) +
					 ((n_bits << 3) -
					  (posbits - 1 + (n_bits << 3)) % (n_bits << 3)));
				++n_bits;
				if (n_bits == maxbits) {
					maxcode = maxmaxcode;
				} else {
					maxcode = MAXCODE(n_bits) - 1;
				}
				bitmask = (1 << n_bits) - 1;
				goto resetbuf;
			}
			{
				unsigned char *p = &inbuf[posbits >> 3];
				code = ((p[0]
					| ((long) (p[1]) << 8)
					| ((long) (p[2]) << 16)) >> (posbits & 0x7)) & bitmask;
			}
			posbits += n_bits;

			if (oldcode == -1) {
				if (code >= 256) { corrupt:
					ERROR_MSG("corrupted data");  /* %ld", code); */
					return 3;
				}
				oldcode = code;
				finchar = (int) oldcode;
				outbuf[outpos++] = (unsigned char) finchar;
				continue;
			}

			if (code == CLEAR && block_mode) {
				clear_tab_prefixof();
				free_ent = FIRST - 1;
				posbits =
					((posbits - 1) +
					 ((n_bits << 3) -
					  (posbits - 1 + (n_bits << 3)) % (n_bits << 3)));
				n_bits = INIT_BITS;
				maxcode = MAXCODE(INIT_BITS) - 1;
				bitmask = (1 << INIT_BITS) - 1;
				goto resetbuf;
			}

			incode = code;
			stackp = de_stack;

			/* Special case for KwKwK string. */
			if (code >= free_ent) {
				if (code > free_ent) goto corrupt;
				*--stackp = (unsigned char) finchar;
				code = oldcode;
			}

			/* Generate output characters in reverse order */
			while (code >= 256) {
				if (stackp <= &htabof(0)) goto corrupt;
				*--stackp = tab_suffixof(code);
				code = tab_prefixof(code);
			}

			finchar = tab_suffixof(code);
			*--stackp = (unsigned char) finchar;

			/* And put them out in forward order */
			{
				int i;

				i = de_stack - stackp;
				if (outpos + i >= OBUFSIZ) {
					do {
						if (i > OBUFSIZ - outpos) {
							i = OBUFSIZ - outpos;
						}

						if (i > 0) {
							memcpy(outbuf + outpos, stackp, i);
							outpos += i;
						}

						if (outpos >= OBUFSIZ) {
							if (full_write(dst_fd, outbuf, outpos) != outpos) { write_error:
								ERROR_MSG("write error");
								return 4;
							}
							outpos = 0;
						}
						stackp += i;
						i = de_stack - stackp;
					} while (i > 0);
				} else {
					memcpy(outbuf + outpos, stackp, i);
					outpos += i;
				}
			}

			/* Generate the new entry. */
			if (free_ent < maxmaxcode) {
				tab_prefixof(free_ent) = (unsigned short) oldcode;
				tab_suffixof(free_ent) = (unsigned char) finchar;
				free_ent++;
			}

			/* Remember previous code.  */
			oldcode = incode;
		}

	} while (rsize > 0);

	if (outpos > 0) {
		if (full_write(dst_fd, outbuf, outpos) != outpos) goto write_error;
	}

	return 0;  /* EXIT_SUCCES. */
}
