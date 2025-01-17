/*
 * rar2john utility for RAR 3.x files, written in 2011 by Dhiru Kholia for GSoC.
 * rar2john processes input RAR files into a format suitable for use with JtR.
 *
 * This software is Copyright (c) 2011, Dhiru Kholia <dhiru.kholia at gmail.com>
 * and (c) 2012, magnum and (c) 2014, JimF
 * and it is hereby released to the general public under the following terms:
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted.
 *
 * Huge thanks to Marc Bevand <m.bevand (at) gmail.com> for releasing unrarhp
 * (http://www.zorinaq.com/unrarhp/) and documenting the RAR encryption scheme.
 * This patch is made possible by unrarhp's documentation.
 *
 * Usage:
 *
 * 1. Run rar2john on rar file(s) as "rar2john [rar files]".
 *    Output is written to standard output.
 * 2. Run JtR on the output generated by rar2john as "john [output file]".
 *
 * Output Line Format:
 *
 * For type = 0 for files encrypted with "rar -hp ..." option
 * archive_name:$RAR3$*type*hex(salt)*hex(partial-file-contents):type::::archive_name
 *
 * For type = 1 for files encrypted with "rar -p ..." option
 * archive_name:$RAR3$*type*hex(salt)*hex(crc)*PACK_SIZE*UNP_SIZE*0*archive_name*offset-for-ciphertext*method:type::file_name
 *
 * or
 *
 * archive_name:$RAR3$*type*hex(salt)*hex(crc)*PACK_SIZE*UNP_SIZE*1*hex(full encrypted file)*method:type::file_name
 *
 * TODO:
 * Possibly support some file magics (see zip2john)
 *
 * FIXED:
 * Archive starting with a directory is currently not read (skip it)
 * Archive starting with a plaintext file is currently not read (skip it)
 * Pick smallest possible file in case of -p mode, just like pkzip do
 * If any of the files is uncompressed, this is preferred even if larger
 * Add METHOD to output
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#if !AC_BUILT || HAVE_LIMITS_H
#include <limits.h>
#endif
#include <errno.h>
#include <string.h>
#if  (!AC_BUILT || HAVE_UNISTD_H) && !_MSC_VER
#include <unistd.h>
#endif

#include "jumbo.h"
#include "common.h"
#include "arch.h"
#include "params.h"
#include "crc32.h"
#include "unicode.h"
#include "base64_convert.h"
#include "sha2.h"
#include "rar2john.h"
#ifdef _MSC_VER
#include "missing_getopt.h"
#endif

#define CHUNK_SIZE 4096

/* Max file (path) name length, in characters */
#define PATH_BUF_SIZE 256

/* File magics */
#define RAR_OLD_MAGIC      "\x52\x45\x7e\x5e"
#define RAR3_MAGIC         "\x52\x61\x72\x21\x1a\x07\x00"
#define RAR5_MAGIC         "\x52\x61\x72\x21\x1a\x07\x01\x00"
#define RAR_OLD_MAGIC_SIZE (sizeof(RAR_OLD_MAGIC) - 1)
#define RAR3_MAGIC_SIZE    (sizeof(RAR3_MAGIC) - 1)
#define RAR5_MAGIC_SIZE    (sizeof(RAR5_MAGIC) - 1)

static int verbose;
static char *self_name;

static void hexdump(const void *msg, void *x, unsigned int size)
{
	unsigned int i;

	printf("%s : ", (char *)msg);
	for (i=0;i<size;i++)
	{
		printf("%.2x", ((unsigned char*)x)[i]);
		if ( (i%4)==3 )
		printf(" ");
	}
	printf("\n");
}

static int process_file5(const char *archive_name);

static int check_fread(const size_t buf_size, const size_t size,
                       const size_t nmemb)
{
	if (buf_size < size * nmemb) {
		fprintf(stderr, "Error: check_fread(buf_size="Zu", size="Zu", nmemb="Zu
		        ") failed, buf_size is smaller than size * nmemb.\n",
		        buf_size, size, nmemb);
		return 0;
	}
	return 1;
}

/* Derived from unrar's encname.cpp */
static void DecodeFileName(unsigned char *Name, unsigned char *EncName,
                           size_t EncSize, UTF16 *NameW, size_t MaxDecSize)
{
	unsigned char Flags = 0;
	unsigned int FlagBits = 0;
	size_t EncPos = 0, DecPos = 0;
	unsigned char HighByte = EncName[EncPos++];

	MaxDecSize /= sizeof(UTF16);

	while (EncPos < EncSize - 1 && DecPos < MaxDecSize - 1)
	{
		if (FlagBits == 0)
		{
			Flags = EncName[EncPos++];
			FlagBits = 8;
		}
		switch(Flags >> 6)
		{
		case 0:
#if ARCH_LITTLE_ENDIAN
			NameW[DecPos++] = EncName[EncPos++];
#else
			NameW[DecPos++] = EncName[EncPos++] << 8;
#endif
			break;
		case 1:
#if ARCH_LITTLE_ENDIAN
			NameW[DecPos++] = EncName[EncPos++] + (HighByte << 8);
#else
			NameW[DecPos++] = (EncName[EncPos++] << 8) + HighByte;
#endif
			break;
		case 2:
#if ARCH_LITTLE_ENDIAN
			NameW[DecPos++] = EncName[EncPos] +
				(EncName[EncPos+1]<<8);
#else
			NameW[DecPos++] = (EncName[EncPos] << 8) +
				EncName[EncPos+1];
#endif
			EncPos+=2;
			break;
		case 3:
		{
			int Length = EncName[EncPos++];
			if (Length & 0x80)
			{
				unsigned char Correction = EncName[EncPos++];
				for (Length = (Length & 0x7f) + 2;
				     Length>0 && DecPos < MaxDecSize;
				     Length--, DecPos++)
#if ARCH_LITTLE_ENDIAN
					NameW[DecPos] = ((Name[DecPos] +
					  Correction) & 0xff) + (HighByte << 8);
#else
					NameW[DecPos] = (((Name[DecPos] +
					  Correction) & 0xff) << 8) + HighByte;
#endif
			}
			else
				for (Length += 2;
				     Length>0 && DecPos < MaxDecSize;
				     Length--,DecPos++)
#if ARCH_LITTLE_ENDIAN
					NameW[DecPos] = Name[DecPos];
#else
					NameW[DecPos] = Name[DecPos] << 8;
#endif
		}
		break;
		}
		Flags <<= 2;
		FlagBits -= 2;
	}
	NameW[DecPos < MaxDecSize ? DecPos : MaxDecSize - 1] = 0;
}

static void process_file(const char *archive_name)
{
	FILE *fp;
	unsigned char marker_block[RAR3_MAGIC_SIZE];
	unsigned char archive_hdr_block[13];
	unsigned char file_hdr_block[40];
	int i, count, type;
	struct {
		size_t pack;
		size_t unp;
		uint8_t method;
	} bestsize = { SIZE_MAX, SIZE_MAX };
	char *base_aname;
	unsigned char buf[CHUNK_SIZE];
	uint16_t archive_hdr_head_flags, file_hdr_head_flags, head_size;
	unsigned char *pos;
	int diff;
	int found = 0;
	char path[PATH_BUFFER_SIZE];
	char *gecos, *best = NULL;
	size_t best_len = 0;
	int gecos_len = 0;

	gecos = mem_calloc(1, LINE_BUFFER_SIZE);

	strnzcpy(path, archive_name, sizeof(path));
	base_aname = basename(path);

	if (!(fp = fopen(archive_name, "rb"))) {
		fprintf(stderr, "! %s: %s\n", archive_name, strerror(errno));
		goto err;
	}

	/* marker block */
	if (fread(marker_block, RAR3_MAGIC_SIZE, 1, fp) != 1) {
		fprintf(stderr, "! %s: Not a RAR file\n", archive_name);
		goto err;
	}

	if (!memcmp(marker_block, RAR_OLD_MAGIC, RAR_OLD_MAGIC_SIZE)) {
		fprintf(stderr, "! %s: Too old RAR file version (pre 1.50), not supported.\n", archive_name);
		goto err;
	}

	if (memcmp(marker_block, RAR3_MAGIC, RAR3_MAGIC_SIZE)) {
		if (memcmp(marker_block, "MZ", 2) == 0) {
			/* Handle SFX archive, find "Rar!" signature */
			while (!feof(fp)) {
				count = fread(buf, 1, CHUNK_SIZE, fp);
				if (count < RAR3_MAGIC_SIZE)
					break;
				if ((pos = memmem(buf, count, RAR3_MAGIC, RAR3_MAGIC_SIZE))) {
					diff = count - (pos - buf);
					jtr_fseek64(fp, - diff, SEEK_CUR);
					jtr_fseek64(fp, RAR3_MAGIC_SIZE, SEEK_CUR);
					found = 1;
					break;
				}
				if (feof(fp)) /* We should examine the EOF before seek back */
					break;
				jtr_fseek64(fp, 1 - RAR3_MAGIC_SIZE, SEEK_CUR);
			}
			if (!found) {
				if (process_file5(archive_name))
					return;
				/* The "Not a RAR file" message already printed by process_file5() at this point */
				goto err;
			}
		}
		else {
			/* try to detect RAR 5 files */
			fclose(fp);
			fp = NULL;
			MEM_FREE(best);
			MEM_FREE(gecos);
			if (process_file5(archive_name))
				return;
			/* The "Not a RAR file" message already printed by process_file5() at this point */
			goto err;
		}
	}

	/* archive header block */
	if (fread(archive_hdr_block, 13, 1, fp) != 1) {
		fprintf(stderr, "%s: Error: read failed: %s.\n",
			archive_name, strerror(errno));
		goto err;
	}
	if (archive_hdr_block[2] != 0x73) {
		fprintf(stderr, "%s: Error: archive_hdr_block[2] must be 0x73.\n",
			archive_name);
		goto err;
	}

	/* find encryption mode used (called type in output line format) */
	archive_hdr_head_flags =
	    archive_hdr_block[4] << 8 | archive_hdr_block[3];
	if (archive_hdr_head_flags & 0x0080) {	/* file header block is encrypted */
		type = 0;	/* RAR file was created using -hp flag */
	} else
		type = 1;

	/*
	 * We need to skip ahead, if there is a comment block in the main header.
	 * It causes that header tp be larger that a simple 13 byte block.
	 */
	head_size = archive_hdr_block[6] << 8 | archive_hdr_block[5];
	if (head_size > 13)
		fseek(fp, head_size-13, SEEK_CUR);

next_file_header:
	if (verbose)
		fprintf(stderr, "\n");

	/* file header block */
	count = fread(file_hdr_block, 32, 1, fp);

	if (feof(fp))  {
		if (verbose) {
			fprintf(stderr, "! %s: End of file\n", archive_name);
		}
		goto BailOut;
	}

	if (count != 1) {
		fprintf(stderr, "%s: Error: read failed: %s.\n",
			archive_name, strerror(errno));
		goto err;
	}

	if (type == 1 && file_hdr_block[2] == 0x7a) {
		if (verbose) {
			fprintf(stderr, "! %s: Comment block present?\n", archive_name);
		}
	}
	else if (type == 1 && file_hdr_block[2] != 0x74) {
		fprintf(stderr, "! %s: Not recognising any more headers.\n",
		        archive_name);
		goto BailOut;
	}

	file_hdr_head_flags =
	    file_hdr_block[4] << 8 | file_hdr_block[3];

	/* process -hp mode files
	   use Marc's end-of-archive block decrypt trick */
	if (type == 0) {
		unsigned char buf[24];

		if (verbose) {
			fprintf(stderr, "! -hp mode entry found in %s\n", base_aname);
		}
		printf("%s:$RAR3$*%d*", base_aname, type);
		jtr_fseek64(fp, -24, SEEK_END);
		if (fread(buf, 24, 1, fp) != 1) {
			fprintf(stderr, "%s: Error: read failed: %s.\n",
				archive_name, strerror(errno));
			goto err;
		}

		for (i = 0; i < 8; i++) { /* salt */
			printf("%c%c", itoa16[ARCH_INDEX(buf[i] >> 4)],
			    itoa16[ARCH_INDEX(buf[i] & 0x0f)]);
		}
		printf("*");
		/* encrypted block with known plaintext */
		for (i = 8; i < 24; i++) {
			printf("%c%c", itoa16[ARCH_INDEX(buf[i] >> 4)],
			       itoa16[ARCH_INDEX(buf[i] & 0x0f)]);
		}
		printf(":%d::::%s\n", type, archive_name);
	} else {
		size_t file_hdr_pack_size = 0, file_hdr_unp_size = 0;
		int ext_time_size;
		uint8_t method;
		uint64_t bytes_left;
		uint16_t file_hdr_head_size, file_name_size;
		unsigned char file_name[4 * PATH_BUF_SIZE], file_crc[4];
		unsigned char salt[8] = { 0 };
		unsigned char rejbuf[32];
		char *p;
		unsigned char s;

		if (!(file_hdr_head_flags & 0x8000)) {
			fprintf(stderr, "File header flag 0x8000 unset, bailing out.\n");
			goto BailOut;
		}

		file_hdr_head_size =
		    file_hdr_block[6] << 8 | file_hdr_block[5];

		/*
		 * Low 32 bits.  If header_flags & 0x100 set, then there are additional
		 * 32 bits of length data later in the header. FIXME!
		 */
		file_hdr_pack_size = file_hdr_block[10];
		file_hdr_pack_size <<= 8; file_hdr_pack_size += file_hdr_block[9];
		file_hdr_pack_size <<= 8; file_hdr_pack_size += file_hdr_block[8];
		file_hdr_pack_size <<= 8; file_hdr_pack_size += file_hdr_block[7];

		file_hdr_unp_size = file_hdr_block[14];
		file_hdr_unp_size <<= 8; file_hdr_unp_size += file_hdr_block[13];
		file_hdr_unp_size <<= 8; file_hdr_unp_size += file_hdr_block[12];
		file_hdr_unp_size <<= 8; file_hdr_unp_size += file_hdr_block[11];

		if (verbose) {
			fprintf(stderr, "! HEAD_SIZE: %d, PACK_SIZE: %"PRIu64
			        ", UNP_SIZE: %"PRIu64"\n",
			        file_hdr_head_size,
			        (uint64_t)file_hdr_pack_size,
			        (uint64_t)file_hdr_unp_size);
			fprintf(stderr, "! file_hdr_block:\n!  ");
			for (i = 0; i < 32; ++i)
				fprintf(stderr, " %02x", file_hdr_block[i]);
			fprintf(stderr, "\n");
		}
		/* calculate EXT_TIME size */
		ext_time_size = file_hdr_head_size - 32;

		if (file_hdr_head_flags & 0x100) {
			uint64_t ex;
			if (fread(rejbuf, 4, 1, fp) != 1) {
				fprintf(stderr, "\n! %s: Error: read failed: %s.\n",
					archive_name, strerror(errno));
				goto err;
			}
			if (verbose) {
				fprintf(stderr, "!  ");
				for (i = 0; i < 4; ++i)
					fprintf(stderr, " %02x", rejbuf[i]);
			}
			ex = rejbuf[3];
			ex <<= 8; ex += rejbuf[2];
			ex <<= 8; ex += rejbuf[1];
			ex <<= 8; ex += rejbuf[0];
			ex <<= 32;
			file_hdr_pack_size += ex;
			ext_time_size -= 4;

			if (fread(rejbuf, 4, 1, fp) != 1) {
				fprintf(stderr, "\n! %s: Error: read failed: %s.\n",
					archive_name, strerror(errno));
				goto err;
			}
			if (verbose) {
				for (i = 0; i < 4; ++i)
					fprintf(stderr, " %02x", rejbuf[i]);
				fprintf(stderr, "   (High Pack/Unp extra header data)\n");
			}
			ex = rejbuf[3];
			ex <<= 8; ex += rejbuf[2];
			ex <<= 8; ex += rejbuf[1];
			ex <<= 8; ex += rejbuf[0];
			ex <<= 32;
			file_hdr_unp_size += ex;
			ext_time_size -= 4;
			if (verbose) {
				fprintf(stderr, "! HIGH_PACK_SIZE present\n");
				fprintf(stderr, "! HIGH_UNP_SIZE present\n");
				if (sizeof(size_t) < 8) {
					fprintf(stderr, "! %s: Error: File contains 64-bit sizes "
					        "but this build of %s doesn't support it.\n",
					        archive_name, self_name);
					goto err;
				}
			}
		}

		/* file name processing */
		file_name_size =
		    file_hdr_block[27] << 8 | file_hdr_block[26];
		if (verbose) {
			fprintf(stderr, "! file name size: %d bytes\n", file_name_size);
		}
		memset(file_name, 0, sizeof(file_name));

		if (!check_fread(sizeof(file_name), file_name_size, 1))
			goto err;
		if (fread(file_name, file_name_size, 1, fp) != 1) {
			fprintf(stderr, "! %s: Error: read failed: %s.\n",
				archive_name, strerror(errno));
			goto err;
		}

		file_name[sizeof(file_name) - 1] = 0;
		ext_time_size -= file_name_size;

		/* If this flag is set, file_name contains some weird
		   wide char encoding that need to be decoded to UTF16
		   and then to UTF-8 (we don't support codepages here) */
		if (file_hdr_head_flags & 0x200) {
			UTF16 FileNameW[PATH_BUF_SIZE];
			int Length = strlen((char*)file_name);

			if (verbose) {
				hexdump("! Encoded filenames", file_name, file_name_size);
			}
			DecodeFileName(file_name, file_name + Length + 1,
			                sizeof(file_name) - Length - 1,
			               FileNameW, sizeof(FileNameW));

			if (*FileNameW) {
				if (verbose) {
					hexdump("! UTF16 filename", FileNameW,
					               strlen16(FileNameW) << 1);
					fprintf(stderr, "OEM name:  %s\n", file_name);
				}
				utf16_to_utf8_r(file_name, PATH_BUF_SIZE, FileNameW);
				fprintf(stderr, "! Unicode:   %s\n", file_name);
			} else
				fprintf(stderr, "! UTF8 name: %s\n", file_name);
		}
        else
			fprintf(stderr, "! file name: %s\n", file_name);

		/* We duplicate file names to the GECOS field, for single mode */
		if (gecos_len + strlen((char*)file_name) < LINE_BUFFER_SIZE)
			gecos_len += snprintf(&gecos[gecos_len], LINE_BUFFER_SIZE - gecos_len - 1, "%s ", (char*)file_name);

		/* salt processing */
		if (file_hdr_head_flags & 0x400) {
			ext_time_size -= 8;
			if (fread(salt, 8, 1, fp) != 1) {
				fprintf(stderr, "! %s: Error: read failed: %s.\n",
					archive_name, strerror(errno));
				goto err;
			}

		}

		/* EXT_TIME processing */
		if (file_hdr_head_flags & 0x1000) {
			if (verbose) {
				fprintf(stderr, "! EXT_TIME present with size %d\n",
				        ext_time_size);
			}

			if (!check_fread(sizeof(rejbuf), ext_time_size, 1))
				goto err;

			if (fread(rejbuf, ext_time_size, 1, fp) != 1) {
				fprintf(stderr, "! %s: Error: read failed: %s.\n",
					archive_name, strerror(errno));
				goto err;
			}
		}

		/* Skip solid files (first file is never solid)
		 * We could probably add support for this
		 */
		if (file_hdr_head_flags & 0x10) {
			fprintf(stderr, "! Solid, can't handle (currently)\n");
			jtr_fseek64(fp, file_hdr_pack_size, SEEK_CUR);
			goto next_file_header;
		}

		if ((file_hdr_head_flags & 0xe0)>>5 == 7) {
			if (verbose) {
				fprintf(stderr, "! Is a directory, skipping\n");
			}
			jtr_fseek64(fp, file_hdr_pack_size, SEEK_CUR);
			goto next_file_header;
		}
		else if (verbose) {
			fprintf(stderr, "! Dictionary size: %u KB\n", 64<<((file_hdr_head_flags & 0xe0)>>5));
		}

		/* Check if encryption is being used */
		if (!(file_hdr_head_flags & 0x04)) {
			fprintf(stderr, "! not encrypted, skipping\n");
			jtr_fseek64(fp, file_hdr_pack_size, SEEK_CUR);
			goto next_file_header;
		}

		method = file_hdr_block[25];

		/*
		 * Prefer shortest pack size, but given two files with single-block
		 * pack size, prefer unpack size >= 8. This gives us better immunity
		 * against false positives.
		 */
		if (bestsize.pack < SIZE_MAX &&
		    (((bestsize.pack < file_hdr_pack_size &&
		       bestsize.unp >= (bestsize.method > 0x30 ? 4 : 1)) ||
		      (bestsize.unp > file_hdr_unp_size &&
		       file_hdr_unp_size < (method > 0x30 ? 4 : 1))) ||
		     (bestsize.pack == file_hdr_pack_size &&
		      ((bestsize.unp > file_hdr_unp_size && file_hdr_unp_size < 8) ||
		       (bestsize.unp <= file_hdr_unp_size && bestsize.unp >= 8))))) {
			if (verbose)
				fprintf(stderr,
				        "! We got a better candidate already, skipping\n");
			jtr_fseek64(fp, file_hdr_pack_size, SEEK_CUR);
			goto next_file_header;
		}

		if (verbose)
			fprintf(stderr, "! This is best candidate so far\n");
		bestsize.pack = file_hdr_pack_size;
		bestsize.unp = file_hdr_unp_size;
		bestsize.method = method;

		MEM_FREE(best);
		best = mem_calloc(1, 2 * LINE_BUFFER_SIZE + 2 * file_hdr_pack_size);

		/* process encrypted data of size "file_hdr_pack_size" */
		best_len = sprintf(best, "%s:$RAR3$*%d*", base_aname, type);
		for (i = 0; i < 8; i++) { /* encode salt */
			best_len += sprintf(&best[best_len], "%c%c", itoa16[ARCH_INDEX(salt[i] >> 4)], itoa16[ARCH_INDEX(salt[i] & 0x0f)]);
		}
		if (verbose) {
			fprintf(stderr, "! salt: '%s'\n", best);
		}
		best_len += sprintf(&best[best_len], "*");
		memcpy(file_crc, file_hdr_block + 16, 4);
		for (i = 0; i < 4; i++) { /* encode file_crc */
			best_len += sprintf(&best[best_len], "%c%c", itoa16[ARCH_INDEX(file_crc[i] >> 4)], itoa16[ARCH_INDEX(file_crc[i] & 0x0f)]);
		}
		if (verbose) {
			/* Minimal version needed to unpack this file */
			fprintf(stderr, "! UNP_VER is %0.1f\n", (float)file_hdr_block[24] / 10.);
		}
		/*
		 * 0x30 - storing
		 * 0x31 - fastest compression
		 * 0x32 - fast compression
		 * 0x33 - normal compression (default)
		 * 0x34 - good compression
		 * 0x35 - best compression
		 *
		 * m3b means 0x33 and a dictionary size of 128KB (a == 64KB .. g == 4096KB)
		 */
		if (verbose) {
			fprintf(stderr, "! METHOD is m%x%c\n", method - 0x30, 'a'+((file_hdr_head_flags&0xe0)>>5));
			//fprintf(stderr, "! file_hdr_flags is 0x%04x\n", file_hdr_head_flags);
		}

		best_len += sprintf(&best[best_len], "*%"PRIu64"*%"PRIu64"*",
		        (uint64_t)file_hdr_pack_size,
		        (uint64_t)file_hdr_unp_size);

		/* We always store it inline */

		best_len += sprintf(&best[best_len], "1*");
		p = &best[best_len];
		bytes_left = file_hdr_pack_size;
		for (i = 0; i < file_hdr_pack_size; i++) {
			unsigned char bytes[64*1024];
			unsigned x, to_read = 64*1024;
			if (bytes_left < 64*1024)
				to_read = bytes_left;
			bytes_left -= to_read;
			if (fread(bytes, 1, to_read, fp) != to_read)
				fprintf(stderr, "! Error while reading archive: %s\n", strerror(errno));
			for (x = 0; x < to_read; ++x) {
				s = bytes[x];
				*p++ = itoa16[s >> 4];
				*p++ = itoa16[s & 0xf];
			}
		}
		best_len += file_hdr_pack_size;
		best_len += sprintf(p, "*%02x:%d::", method, type);

		/* Keep looking for better candidates */
		goto next_file_header;

BailOut:
		if (best && *best) {
			if (verbose) {
				fprintf(stderr, "! Found a valid -p mode candidate in %s\n", base_aname);
			}
			if (bestsize.unp < (bestsize.method > 0x30 ? 5 : 1))
				fprintf(stderr, "! WARNING best candidate found is too small, you may see false positives.\n");
			strncat(best, gecos, LINE_BUFFER_SIZE - best_len - 1);
			puts(best);
		} else
			fprintf(stderr, "! Did not find a valid encrypted candidate in %s\n", base_aname);
	}

err:
	if (fp)
		fclose(fp);
	MEM_FREE(best);
	MEM_FREE(gecos);
}


/**************************************************************************
 * Here are the functions and tools for RAR5
 *************************************************************************/

// global variables
static int Encrypted = 0;
static unsigned char PswCheck[SIZE_PSWCHECK];
static unsigned rar5_interations=0, UsePswCheck=0;
static unsigned char rar5_salt[SIZE_SALT50];

/**************************************************************************
 * These 4 functions do much of the reading for rar5 files. There is a
 * function to read a 4 byte int (in LE format), one to read a single
 * byte, one to to read a buffer, and one that reads the variable sized
 * numbers used in rar5 (LE format, 7 bits used per byte with high bit
 * used to signify if there are more bytes of data or not)
 *************************************************************************/
int read_uint32 (FILE *fp, uint32_t *n, uint32_t *bytes_read) {
	unsigned char Buf[4];
	int i, shift=0;
	*n = 0;
	if (fread(Buf, 1, 4, fp) < 4)
		return 0;
	for (i = 0; i < 4; ++i) {
		*n  = *n + (Buf[i] << shift);
		shift += 8;
	}
    *bytes_read += 4;
	return 4;
}
int read_uint8 (FILE *fp, uint8_t *n, uint32_t *bytes_read) {
	unsigned char Buf[1];
	if (fread(Buf, 1, 1, fp) < 1)
		return 0;
    *n = Buf[0];
    *bytes_read += 1;
	return 1;
}
int read_buf (FILE *fp, unsigned char *cp, int len, uint32_t *bytes_read) {
	if (fread(cp, 1, len, fp) < 1)
		return 0;
    *bytes_read += len;
	return len;
}
int read_vuint (FILE *fp, uint64_t *n, uint32_t *bytes_read) {
	unsigned char c;
	int i, shift=0;
    uint64_t accum;
	*n = 0;
	for (i = 0; i < 10; ++i) {
		if (fread(&c, 1, 1, fp) != 1)
			return 0;
        accum = (c&0x7f);
		*n = *n + (accum << shift);
		shift += 7;
		if ((c & 0x80) == 0) {
            *bytes_read += i+1;
			return i+1;
		}
	}
	return 0;
}

/**************************************************************************
 * Process an 'extra' block of data. This is where rar5 stores the
 * encryption block.
 *************************************************************************/
static int ProcessExtra50(FILE *fp, uint64_t extra_size, uint64_t HeadSize, uint32_t HeaderType, uint32_t CurBlockPos, const char *archive_name, int *found)
{
	uint64_t FieldSize, FieldType, EncVersion, Flags;
	uint32_t bytes_read=0;
	int bytes_left=(int)extra_size;
	unsigned char Lg2Count;
	char *base_aname = basename(archive_name);

   // fprintf(stderr, "in extra50 extrasize=%d\n", extra_size);
    while (1) {
        int len = read_vuint(fp, &FieldSize, &bytes_read);
        if (!len || len > 3) return 0;  // rar5 technote (http://www.rarlab.com/technote.htm#arcblocks) lists max size of header len is 3 byte vint.
        bytes_left -= len;
        bytes_left -= (uint32_t)FieldSize;
        if (bytes_left < 0) return 0;
        if (!read_vuint(fp, &FieldType, &bytes_read)) return 0;
        // fprintf(stderr, "in Extra50.  FieldSize=%d FieldType=%d\n", FieldSize, FieldType);
        if (HeaderType == HEAD_FILE || HeaderType == HEAD_SERVICE) {
            if (FieldType == FHEXTRA_CRYPT) {
                unsigned char InitV[SIZE_INITV];
                unsigned char Hex1[128], Hex2[128], Hex3[128];
                if (!read_vuint(fp, &EncVersion, &bytes_read)) return 0;
                if (!read_vuint(fp, &Flags, &bytes_read)) return 0;
                if ( (Flags & FHEXTRA_CRYPT_PSWCHECK) == 0) {
                    fprintf(stderr, "UsePswCheck is OFF. We currently don't support such files!\n");
                    return 0;
                }
                if (!read_uint8(fp, &Lg2Count, &bytes_read)) return 0;
                if (Lg2Count >= CRYPT5_KDF_LG2_COUNT_MAX) {
                    fprintf(stderr, "Lg2Count >= CRYPT5_KDF_LG2_COUNT_MAX (problem with file?)");
                    return 0;
                }
                if (!read_buf(fp, rar5_salt, SIZE_SALT50, &bytes_read)) return 0;
                if (!read_buf(fp, InitV, SIZE_INITV, &bytes_read)) return 0;
                if (!read_buf(fp, PswCheck, SIZE_PSWCHECK, &bytes_read)) return 0;
                (*found)++;
                printf("%s:$rar5$%d$%s$%d$%s$%d$%s\n",
                    base_aname,
                    SIZE_SALT50, base64_convert_cp(rar5_salt,e_b64_raw,SIZE_SALT50,Hex1,e_b64_hex,sizeof(Hex1),0, 0),
                    Lg2Count, base64_convert_cp(InitV,e_b64_raw,SIZE_INITV,Hex2,e_b64_hex,sizeof(Hex2),0, 0),
                    SIZE_PSWCHECK, base64_convert_cp(PswCheck,e_b64_raw,SIZE_PSWCHECK,Hex3,e_b64_hex,sizeof(Hex3),0, 0));
                return 0;
            }
        }
    }
    return 1;
 }

/**************************************************************************
 * Common file header processing for rar5
 *************************************************************************/

static size_t read_rar5_header(FILE *fp, size_t CurBlockPos, uint8_t *HeaderType, const char *archive_name, int *found)
{
	uint64_t block_size, flags, extra_size=0, data_size=0;
	uint64_t crypt_version, enc_flags, HeadSize;
	uint32_t head_crc, header_bytes_read = 0, sizeof_vint;
	uint8_t header_type, lg_2count;
	char *base_aname = basename(archive_name);

    if (Encrypted) {
        // The header is encrypted, so we simply find the IV from this block.
        unsigned char HeadersInitV[SIZE_INITV];
        unsigned char Hex1[128], Hex2[128], Hex3[128];
        sizeof_vint = read_buf(fp, HeadersInitV,  SIZE_INITV, &header_bytes_read);
        if (sizeof_vint != SIZE_INITV) {
            fprintf(stderr, "Error, rar file %s too short, could not read IV from header\n", archive_name);
            return 0;
        }
        (*found)++;
        printf("%s:$rar5$%d$%s$%d$%s$%d$%s\n",
            base_aname,
            SIZE_SALT50, base64_convert_cp(rar5_salt,e_b64_raw,SIZE_SALT50,Hex1,e_b64_hex,sizeof(Hex1),0, 0),
            rar5_interations, base64_convert_cp(HeadersInitV,e_b64_raw,SIZE_INITV,Hex2,e_b64_hex,sizeof(Hex2),0, 0),
            SIZE_PSWCHECK, base64_convert_cp(PswCheck,e_b64_raw,SIZE_PSWCHECK,Hex3,e_b64_hex,sizeof(Hex3),0, 0));
        return 0;
    }
	if (!read_uint32(fp, &head_crc, &header_bytes_read)) return 0;

    sizeof_vint = read_vuint(fp, &block_size, &header_bytes_read);
    if (!sizeof_vint) return 0;
    // The HeadSize is full size of this header from the start of the HeaderCRC, to the end of any 'extra-data' section.
    HeadSize = block_size + 4 + sizeof_vint;

	//if (!read_vuint(fp, &header_type, &header_bytes_read)) return 0;
    if (!read_uint8(fp, &header_type, &header_bytes_read)) return 0;
    if (!read_vuint(fp, &flags, &header_bytes_read)) return 0;
    *HeaderType = header_type;
    if ((flags & HFL_EXTRA) != 0) { if (!read_vuint(fp, &extra_size, &header_bytes_read)) return 0; }
    if ((flags & HFL_DATA) != 0)  { if (!read_vuint(fp, &data_size, &header_bytes_read)) return 0; }

    // fprintf(stderr, "curpos=%d bs=%d firstreadsize=%d, sizeBytes=%d headtye=%d flags=%d \n", NowCurPos, block_size, 7, SizeBytes, header_type, flags);

    if (header_type == HEAD_CRYPT) {
       unsigned char chksum[SIZE_PSWCHECK_CSUM];
       if (!read_vuint(fp, &crypt_version, &header_bytes_read)) return 0;
       if (crypt_version > CRYPT_VERSION) { printf("bad rar crypt version byte\n"); return 0; }
       if (!read_vuint(fp, &enc_flags, &header_bytes_read)) return 0;
       UsePswCheck = (enc_flags & CHFL_CRYPT_PSWCHECK) != 0;  // set global
       if (!read_uint8(fp, &lg_2count, &header_bytes_read)) return 0;
       if (lg_2count > CRYPT5_KDF_LG2_COUNT_MAX) { printf("rar PBKDF2 iteration count too large\n"); return 0; }
       rar5_interations = lg_2count; // set global
       // get salt
       if (!read_buf(fp, rar5_salt, SIZE_SALT50, &header_bytes_read)) return 0;
       if (UsePswCheck) {
           unsigned char sha256ch[32];
           SHA256_CTX ctx;
           if (!read_buf(fp, PswCheck, SIZE_PSWCHECK, &header_bytes_read)) return 0;
           if (!read_buf(fp, chksum, SIZE_PSWCHECK_CSUM, &header_bytes_read)) return 0;
           SHA256_Init(&ctx);
		   SHA256_Update(&ctx, PswCheck, SIZE_PSWCHECK);
		   SHA256_Final(sha256ch, &ctx);
           UsePswCheck = !memcmp(sha256ch, chksum, sizeof(chksum));
       }
       Encrypted = 1;
     } else if (header_type == HEAD_MAIN) {
        uint64_t ArcFlags, VolNumber=0;
        if (!read_vuint(fp, &ArcFlags, &header_bytes_read)) return 0;
        if ((ArcFlags & MHFL_VOLNUMBER) != 0)
            if (!read_vuint(fp, &VolNumber, &header_bytes_read)) return 0;
    } else if (header_type == HEAD_FILE || header_type == HEAD_SERVICE) {
        uint64_t FileFlags, UnpSize, FileAttr;
        uint64_t CompInfo, HostOS, NameSize;
        uint32_t FileHashCRC32, tmp;

        if (!read_vuint(fp, &FileFlags, &header_bytes_read)) return 0;
        if (!read_vuint(fp, &UnpSize, &header_bytes_read)) return 0;
        if (!read_vuint(fp, &FileAttr, &header_bytes_read)) return 0;

        if ((FileFlags & FHFL_UTIME) != 0) {
            if (!read_uint32(fp, &tmp, &header_bytes_read)) return 0;
            //mtime = tmp;
        }

        if ((FileFlags & FHFL_CRC32) != 0) {
            if (!read_uint32(fp, &FileHashCRC32, &header_bytes_read)) return 0;
        }

        if (!read_vuint(fp, &CompInfo, &header_bytes_read)) return 0;
        if (!read_vuint(fp, &HostOS, &header_bytes_read)) return 0;
        if (!read_vuint(fp, &NameSize, &header_bytes_read)) return 0;
        // skip the field name.
        jtr_fseek64(fp, NameSize, SEEK_CUR);
        if (extra_size != 0)
	        ProcessExtra50(fp, extra_size, HeadSize, *HeaderType, CurBlockPos, archive_name, found);

    } else if (header_type == HEAD_ENDARC) {
        return 0;
    }
	return CurBlockPos+HeadSize+data_size;
}

/* handle rar5 files */
static int process_file5(const char *archive_name) {
	unsigned char Magic[RAR5_MAGIC_SIZE], buf[CHUNK_SIZE], *pos;
	size_t count, NextBlockPos, CurBlockPos;
	int diff, found = 0;
	FILE *fp;

	if (!(fp = fopen(archive_name, "rb"))) {
		fprintf(stderr, "! %s: %s\n", archive_name, strerror(errno));
		return 0;
	}

	if (fread(Magic, sizeof(Magic), 1, fp) != 1) {
		fprintf(stderr, "! %s: Not a RAR file\n", archive_name);
		goto err;
	}

	if (memcmp(Magic, "MZ", 2) == 0) {
		/* Handle SFX archive, find "Rar!" signature */
		while (!feof(fp)) {
			count = fread(buf, 1, CHUNK_SIZE, fp);
			if (count < RAR5_MAGIC_SIZE)
				break;
			if ((pos = memmem(buf, count, RAR5_MAGIC, RAR5_MAGIC_SIZE))) {
				diff = count - (pos - buf);
				jtr_fseek64(fp, - diff, SEEK_CUR);
				jtr_fseek64(fp, RAR5_MAGIC_SIZE, SEEK_CUR);
				found = 1;
				break;
			}
			if (feof(fp)) /* We should examine the EOF before seek back */
				break;
			jtr_fseek64(fp, 1 - RAR5_MAGIC_SIZE, SEEK_CUR);
		}
	}

	if (memcmp(Magic, RAR5_MAGIC, RAR5_MAGIC_SIZE) && !found) {
		fprintf(stderr, "! %s: Not a RAR file\n", archive_name);
		goto err;
	}

	found = 0;
	while (1) {
		uint8_t HeaderType;

		CurBlockPos = (size_t)jtr_ftell64(fp);
		NextBlockPos = read_rar5_header(fp, CurBlockPos, &HeaderType, archive_name, &found);
		if (!NextBlockPos)
			break;
		// fprintf(stderr, "NextBlockPos is %d Headertype=%d curblockpos=%d\n", NextBlockPos, HeaderType, CurBlockPos);
		jtr_fseek64(fp, NextBlockPos, SEEK_SET);
	}

	if (fp) fclose(fp);
	if (!found)
		fprintf(stderr, "! Did not find a valid encrypted candidate in %s\n", archive_name);

	return 1;

err:
	if (fp) fclose(fp);
	return 0;
}


static int usage(char *name)
{
	fprintf(stderr, "Usage: %s [-v] <rar file(s)>\n", name);
	fprintf(stderr, " -v\tAdd some verbosity/debug output\n");
	return EXIT_FAILURE;
}

int rar2john(int argc, char **argv)
{
	int c;

	self_name = argv[0];

	/* Parse command line */
	while ((c = getopt(argc, argv, "v")) != -1) {
		switch (c) {
		case 'v':
			verbose = 1;
			break;
		case '?':
		default:
			return usage(argv[0]);
		}
	}
	argc -= optind;
	if (argc == 0)
		return usage(argv[0]);
	argv += optind;

	while (argc--)
		process_file(*argv++);

	return EXIT_SUCCESS;
}
