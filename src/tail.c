#include "config.h"
#include "conky.h"
#include "logging.h"
#include "tail.h"
#include "text_object.h"

#include <errno.h>
#include <fcntl.h>
//#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

int init_tail_object(struct text_object *obj, const char *arg)
{
	char buf[64];
	int n1, n2;
	struct stat st;
	FILE *fp = NULL;
	int fd;
	int numargs;

	if (!arg) {
		ERR("tail needs arguments");
		return 1;
	}

	numargs = sscanf(arg, "%63s %i %i", buf, &n1, &n2);

	if (numargs < 2 || numargs > 3) {
		ERR("incorrect number of arguments given to tail object");
		return 1;
	}

	if (n1 < 1 || n1 > MAX_TAIL_LINES) {
		ERR("invalid arg for tail, number of lines must be "
				"between 1 and %i", MAX_TAIL_LINES);
		return 1;
	}

	obj->data.tail.fd = -1;

	if (stat(buf, &st) == 0) {
		if (S_ISFIFO(st.st_mode)) {
			fd = open(buf, O_RDONLY | O_NONBLOCK);

			if (fd == -1) {
				ERR("tail logfile does not exist, or you do "
						"not have correct permissions");
				return 1;
			}

			obj->data.tail.fd = fd;
		} else {
			fp = fopen(buf, "r");
		}
	}

	if (fp || obj->data.tail.fd != -1) {
		obj->data.tail.logfile = malloc(text_buffer_size);
		strcpy(obj->data.tail.logfile, buf);
		obj->data.tail.wantedlines = n1;
		obj->data.tail.interval = update_interval * 2;

		if (obj->data.tail.fd == -1) {
			fclose(fp);
		}
	} else {
		// fclose(fp);
		ERR("tail logfile does not exist, or you do not have "
				"correct permissions");
		return 1;
	}
	/* XXX: the following implies update_interval >= 1 ?! */
	if (numargs == 3 && (n2 < 1 || n2 < update_interval)) {
		ERR("tail interval must be greater than "
		    "0 and "PACKAGE_NAME"'s interval, ignoring");
	} else if (numargs == 3) {
			obj->data.tail.interval = n2;
	}
	/* asumming all else worked */
	obj->data.tail.buffer = malloc(text_buffer_size * 20);
	return 0;
}

/* Allows reading from a FIFO (i.e., /dev/xconsole).
 * The file descriptor is set to non-blocking which makes this possible.
 *
 * FIXME: Since lseek cannot seek a file descriptor long lines will break. */
static void tail_pipe(struct text_object *obj, char *dst, size_t dst_size)
{
#define TAIL_PIPE_BUFSIZE	4096
	int lines = 0;
	int line_len = 0;
	int last_line = 0;
	int fd = obj->data.tail.fd;

	while (1) {
		char buf[TAIL_PIPE_BUFSIZE];
		ssize_t len = read(fd, buf, sizeof(buf));
		int i;

		if (len == -1) {
			if (errno != EAGAIN) {
				strcpy(obj->data.tail.buffer, "Logfile Read Error");
				snprintf(dst, dst_size, "Logfile Read Error");
			}

			break;
		} else if (len == 0) {
			strcpy(obj->data.tail.buffer, "Logfile Empty");
			snprintf(dst, dst_size, "Logfile Empty");
			break;
		}

		for (line_len = 0, i = 0; i < len; i++) {
			int pos = 0;
			char *p;

			if (buf[i] == '\n') {
				lines++;

				if (obj->data.tail.readlines > 0) {
					int n;
					int olines = 0;
					int first_line = 0;

					for (n = 0; obj->data.tail.buffer[n]; n++) {
						if (obj->data.tail.buffer[n] == '\n') {
							if (!first_line) {
								first_line = n + 1;
							}

							if (++olines < obj->data.tail.wantedlines) {
								pos = n + 1;
								continue;
							}

							n++;
							p = obj->data.tail.buffer + first_line;
							pos = n - first_line;
							memmove(obj->data.tail.buffer,
									obj->data.tail.buffer + first_line, strlen(p));
							obj->data.tail.buffer[pos] = 0;
							break;
						}
					}
				}

				p = buf + last_line;
				line_len++;
				memcpy(&(obj->data.tail.buffer[pos]), p, line_len);
				obj->data.tail.buffer[pos + line_len] = 0;
				last_line = i + 1;
				line_len = 0;
				obj->data.tail.readlines = lines;
				continue;
			}

			line_len++;
		}
	}

	snprintf(dst, dst_size, "%s", obj->data.tail.buffer);
}

static long rev_fcharfind(FILE *fp, char val, unsigned int step)
{
#define BUFSZ 0x1000
	long ret = -1;
	unsigned int count = 0;
	static char buf[BUFSZ];
	long orig_pos = ftell(fp);
	long buf_pos = -1;
	long file_pos = orig_pos;
	long buf_size = BUFSZ;
	char *cur_found;

	while (count < step) {
		if (buf_pos <= 0) {
			if (file_pos > BUFSZ) {
				fseek(fp, file_pos - BUFSZ, SEEK_SET);
			} else {
				buf_size = file_pos;
				fseek(fp, 0, SEEK_SET);
			}
			file_pos = ftell(fp);
			buf_pos = fread(buf, 1, buf_size, fp);
		}
		cur_found = memrchr(buf, val, (size_t) buf_pos);
		if (cur_found != NULL) {
			buf_pos = cur_found - buf;
			count++;
		} else {
			buf_pos = -1;
			if (file_pos == 0) {
				break;
			}
		}
	}
	fseek(fp, orig_pos, SEEK_SET);
	if (count == step) {
		ret = file_pos + buf_pos;
	}
	return ret;
}

int print_tail_object(struct text_object *obj, char *p, size_t p_max_size)
{
	FILE *fp;
	long nl = 0, bsize;
	int iter;

	if (current_update_time - obj->data.tail.last_update < obj->data.tail.interval) {
		snprintf(p, p_max_size, "%s", obj->data.tail.buffer);
		return 0;
	}

	obj->data.tail.last_update = current_update_time;

	if (obj->data.tail.fd != -1) {
		tail_pipe(obj, p, p_max_size);
		return 0;
	}

	fp = fopen(obj->data.tail.logfile, "rt");
	if (fp == NULL) {
		/* Send one message, but do not consistently spam
		 * on missing logfiles. */
		if (obj->data.tail.readlines != 0) {
			ERR("tail logfile failed to open");
			strcpy(obj->data.tail.buffer, "Logfile Missing");
		}
		obj->data.tail.readlines = 0;
		snprintf(p, p_max_size, "Logfile Missing");
	} else {
		obj->data.tail.readlines = 0;
		/* -1 instead of 0 to avoid counting a trailing
		 * newline */
		fseek(fp, -1, SEEK_END);
		bsize = ftell(fp) + 1;
		for (iter = obj->data.tail.wantedlines; iter > 0;
				iter--) {
			nl = rev_fcharfind(fp, '\n', iter);
			if (nl >= 0) {
				break;
			}
		}
		obj->data.tail.readlines = iter;
		if (obj->data.tail.readlines
				< obj->data.tail.wantedlines) {
			fseek(fp, 0, SEEK_SET);
		} else {
			fseek(fp, nl + 1, SEEK_SET);
			bsize -= ftell(fp);
		}
		/* Make sure bsize is at least 1 byte smaller than the
		 * buffer max size. */
		if (bsize > (long) ((text_buffer_size * 20) - 1)) {
			fseek(fp, bsize - text_buffer_size * 20 - 1,
					SEEK_CUR);
			bsize = text_buffer_size * 20 - 1;
		}
		bsize = fread(obj->data.tail.buffer, 1, bsize, fp);
		fclose(fp);
		if (bsize > 0) {
			/* Clean up trailing newline, make sure the
			 * buffer is null terminated. */
			if (obj->data.tail.buffer[bsize - 1] == '\n') {
				obj->data.tail.buffer[bsize - 1] = '\0';
			} else {
				obj->data.tail.buffer[bsize] = '\0';
			}
			snprintf(p, p_max_size, "%s",
					obj->data.tail.buffer);
		} else {
			strcpy(obj->data.tail.buffer, "Logfile Empty");
			snprintf(p, p_max_size, "Logfile Empty");
		}	/* bsize > 0 */
	}		/* fp == NULL */
	return 0;
}