#include "cache.h"
#include "pkt-line.h"
#include "run-command.h"

char packet_buffer[LARGE_PACKET_MAX];
static const char *packet_trace_prefix = "git";
static struct trace_key trace_packet = TRACE_KEY_INIT(PACKET);
static struct trace_key trace_pack = TRACE_KEY_INIT(PACKFILE);

void packet_trace_identity(const char *prog)
{
	packet_trace_prefix = xstrdup(prog);
}

static const char *get_trace_prefix(void)
{
	return in_async() ? "sideband" : packet_trace_prefix;
}

static int packet_trace_pack(const char *buf, unsigned int len, int sideband)
{
	if (!sideband) {
		trace_verbatim(&trace_pack, buf, len);
		return 1;
	} else if (len && *buf == '\1') {
		trace_verbatim(&trace_pack, buf + 1, len - 1);
		return 1;
	} else {
		/* it's another non-pack sideband */
		return 0;
	}
}

static void packet_trace(const char *buf, unsigned int len, int write)
{
	int i;
	struct strbuf out;
	static int in_pack, sideband;

	if (!trace_want(&trace_packet) && !trace_want(&trace_pack))
		return;

	if (in_pack) {
		if (packet_trace_pack(buf, len, sideband))
			return;
	} else if (starts_with(buf, "PACK") || starts_with(buf, "\1PACK")) {
		in_pack = 1;
		sideband = *buf == '\1';
		packet_trace_pack(buf, len, sideband);

		/*
		 * Make a note in the human-readable trace that the pack data
		 * started.
		 */
		buf = "PACK ...";
		len = strlen(buf);
	}

	if (!trace_want(&trace_packet))
		return;

	/* +32 is just a guess for header + quoting */
	strbuf_init(&out, len+32);

	strbuf_addf(&out, "packet: %12s%c ",
		    get_trace_prefix(), write ? '>' : '<');

	/* XXX we should really handle printable utf8 */
	for (i = 0; i < len; i++) {
		/* suppress newlines */
		if (buf[i] == '\n')
			continue;
		if (buf[i] >= 0x20 && buf[i] <= 0x7e)
			strbuf_addch(&out, buf[i]);
		else
			strbuf_addf(&out, "\\%o", buf[i]);
	}

	strbuf_addch(&out, '\n');
	trace_strbuf(&trace_packet, &out);
	strbuf_release(&out);
}

/*
 * If we buffered things up above (we don't, but we should),
 * we'd flush it here
 */
void packet_flush(int fd)
{
	packet_trace("0000", 4, 1);
	if (write_in_full(fd, "0000", 4) < 0)
		die_errno(_("unable to write flush packet"));
}

void packet_delim(int fd)
{
	packet_trace("0001", 4, 1);
	if (write_in_full(fd, "0001", 4) < 0)
		die_errno(_("unable to write delim packet"));
}

void packet_response_end(int fd)
{
	packet_trace("0002", 4, 1);
	if (write_in_full(fd, "0002", 4) < 0)
		die_errno(_("unable to write response end packet"));
}

int packet_flush_gently(int fd)
{
	packet_trace("0000", 4, 1);
	if (write_in_full(fd, "0000", 4) < 0)
		return error(_("flush packet write failed"));
	return 0;
}

void packet_buf_flush(struct strbuf *buf)
{
	packet_trace("0000", 4, 1);
	strbuf_add(buf, "0000", 4);
}

void packet_buf_delim(struct strbuf *buf)
{
	packet_trace("0001", 4, 1);
	strbuf_add(buf, "0001", 4);
}

void set_packet_header(char *buf, int size)
{
	static char hexchar[] = "0123456789abcdef";

	#define hex(a) (hexchar[(a) & 15])
	buf[0] = hex(size >> 12);
	buf[1] = hex(size >> 8);
	buf[2] = hex(size >> 4);
	buf[3] = hex(size);
	#undef hex
}

static void format_packet(struct strbuf *out, const char *prefix,
			  const char *fmt, va_list args)
{
	size_t orig_len, n;

	orig_len = out->len;
	strbuf_addstr(out, "0000");
	strbuf_addstr(out, prefix);
	strbuf_vaddf(out, fmt, args);
	n = out->len - orig_len;

	if (n > LARGE_PACKET_MAX)
		die(_("protocol error: impossibly long line"));

	set_packet_header(&out->buf[orig_len], n);
	packet_trace(out->buf + orig_len + 4, n - 4, 1);
}

static int packet_write_fmt_1(int fd, int gently, const char *prefix,
			      const char *fmt, va_list args)
{
	static struct strbuf buf = STRBUF_INIT;

	strbuf_reset(&buf);
	format_packet(&buf, prefix, fmt, args);
	if (write_in_full(fd, buf.buf, buf.len) < 0) {
		if (!gently) {
			check_pipe(errno);
			die_errno(_("packet write with format failed"));
		}
		return error(_("packet write with format failed"));
	}

	return 0;
}

void packet_write_fmt(int fd, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	packet_write_fmt_1(fd, 0, "", fmt, args);
	va_end(args);
}

int packet_write_fmt_gently(int fd, const char *fmt, ...)
{
	int status;
	va_list args;

	va_start(args, fmt);
	status = packet_write_fmt_1(fd, 1, "", fmt, args);
	va_end(args);
	return status;
}

static int do_packet_write(const int fd_out, const char *buf, size_t size,
			   struct strbuf *err)
{
	char header[4];
	size_t packet_size;

	if (size > LARGE_PACKET_DATA_MAX) {
		strbuf_addstr(err, _("packet write failed - data exceeds max packet size"));
		return -1;
	}

	packet_trace(buf, size, 1);
	packet_size = size + 4;

	set_packet_header(header, packet_size);

	/*
	 * Write the header and the buffer in 2 parts so that we do
	 * not need to allocate a buffer or rely on a static buffer.
	 * This also avoids putting a large buffer on the stack which
	 * might have multi-threading issues.
	 */

	if (write_in_full(fd_out, header, 4) < 0 ||
	    write_in_full(fd_out, buf, size) < 0) {
		strbuf_addf(err, _("packet write failed: %s"), strerror(errno));
		return -1;
	}
	return 0;
}

int packet_write_gently(const int fd_out, const char *buf, size_t size)
{
	struct strbuf err = STRBUF_INIT;
	if (do_packet_write(fd_out, buf, size, &err)) {
		error("%s", err.buf);
		strbuf_release(&err);
		return -1;
	}
	return 0;
}

void packet_write(int fd_out, const char *buf, size_t size)
{
	struct strbuf err = STRBUF_INIT;
	if (do_packet_write(fd_out, buf, size, &err))
		die("%s", err.buf);
}

void packet_buf_write(struct strbuf *buf, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	format_packet(buf, "", fmt, args);
	va_end(args);
}

void packet_buf_write_len(struct strbuf *buf, const char *data, size_t len)
{
	size_t orig_len, n;

	orig_len = buf->len;
	strbuf_addstr(buf, "0000");
	strbuf_add(buf, data, len);
	n = buf->len - orig_len;

	if (n > LARGE_PACKET_MAX)
		die(_("protocol error: impossibly long line"));

	set_packet_header(&buf->buf[orig_len], n);
	packet_trace(data, len, 1);
}

int write_packetized_from_fd_no_flush(int fd_in, int fd_out)
{
	char *buf = xmalloc(LARGE_PACKET_DATA_MAX);
	int err = 0;
	ssize_t bytes_to_write;

	while (!err) {
		bytes_to_write = xread(fd_in, buf, LARGE_PACKET_DATA_MAX);
		if (bytes_to_write < 0) {
			free(buf);
			return COPY_READ_ERROR;
		}
		if (bytes_to_write == 0)
			break;
		err = packet_write_gently(fd_out, buf, bytes_to_write);
	}
	free(buf);
	return err;
}

int write_packetized_from_buf_no_flush(const char *src_in, size_t len, int fd_out)
{
	int err = 0;
	size_t bytes_written = 0;
	size_t bytes_to_write;

	while (!err) {
		if ((len - bytes_written) > LARGE_PACKET_DATA_MAX)
			bytes_to_write = LARGE_PACKET_DATA_MAX;
		else
			bytes_to_write = len - bytes_written;
		if (bytes_to_write == 0)
			break;
		err = packet_write_gently(fd_out, src_in + bytes_written, bytes_to_write);
		bytes_written += bytes_to_write;
	}
	return err;
}

static int get_packet_data(int fd, char **src_buf, size_t *src_size,
			   void *dst, unsigned size, int options)
{
	ssize_t ret;

	if (fd >= 0 && src_buf && *src_buf)
		BUG("multiple sources given to packet_read");

	/* Read up to "size" bytes from our source, whatever it is. */
	if (src_buf && *src_buf) {
		ret = size < *src_size ? size : *src_size;
		memcpy(dst, *src_buf, ret);
		*src_buf += ret;
		*src_size -= ret;
	} else {
		ret = read_in_full(fd, dst, size);
		if (ret < 0) {
			if (options & PACKET_READ_GENTLE_ON_READ_ERROR)
				return error_errno(_("read error"));
			die_errno(_("read error"));
		}
	}

	/* And complain if we didn't get enough bytes to satisfy the read. */
	if (ret != size) {
		if (options & PACKET_READ_GENTLE_ON_EOF)
			return -1;

		if (options & PACKET_READ_GENTLE_ON_READ_ERROR)
			return error(_("the remote end hung up unexpectedly"));
		die(_("the remote end hung up unexpectedly"));
	}

	return ret;
}

int packet_length(const char lenbuf_hex[4])
{
	int val = hex2chr(lenbuf_hex);
	return (val < 0) ? val : (val << 8) | hex2chr(lenbuf_hex + 2);
}

enum packet_read_status packet_read_with_status(int fd, char **src_buffer,
						size_t *src_len, char *buffer,
						unsigned size, int *pktlen,
						int options)
{
	int len;
	char linelen[4];

	if (get_packet_data(fd, src_buffer, src_len, linelen, 4, options) < 0) {
		*pktlen = -1;
		return PACKET_READ_EOF;
	}

	len = packet_length(linelen);

	if (len < 0) {
		if (options & PACKET_READ_GENTLE_ON_READ_ERROR)
			return error(_("protocol error: bad line length "
				       "character: %.4s"), linelen);
		die(_("protocol error: bad line length character: %.4s"), linelen);
	} else if (!len) {
		packet_trace("0000", 4, 0);
		*pktlen = 0;
		return PACKET_READ_FLUSH;
	} else if (len == 1) {
		packet_trace("0001", 4, 0);
		*pktlen = 0;
		return PACKET_READ_DELIM;
	} else if (len == 2) {
		packet_trace("0002", 4, 0);
		*pktlen = 0;
		return PACKET_READ_RESPONSE_END;
	} else if (len < 4) {
		if (options & PACKET_READ_GENTLE_ON_READ_ERROR)
			return error(_("protocol error: bad line length %d"),
				     len);
		die(_("protocol error: bad line length %d"), len);
	}

	len -= 4;
	if ((unsigned)len >= size) {
		if (options & PACKET_READ_GENTLE_ON_READ_ERROR)
			return error(_("protocol error: bad line length %d"),
				     len);
		die(_("protocol error: bad line length %d"), len);
	}

	if (get_packet_data(fd, src_buffer, src_len, buffer, len, options) < 0) {
		*pktlen = -1;
		return PACKET_READ_EOF;
	}

	if ((options & PACKET_READ_CHOMP_NEWLINE) &&
	    len && buffer[len-1] == '\n')
		len--;

	buffer[len] = 0;
	packet_trace(buffer, len, 0);

	if ((options & PACKET_READ_DIE_ON_ERR_PACKET) &&
	    starts_with(buffer, "ERR "))
		die(_("remote error: %s"), buffer + 4);

	*pktlen = len;
	return PACKET_READ_NORMAL;
}

int packet_read(int fd, char **src_buffer, size_t *src_len,
		char *buffer, unsigned size, int options)
{
	int pktlen = -1;

	packet_read_with_status(fd, src_buffer, src_len, buffer, size,
				&pktlen, options);

	return pktlen;
}

static char *packet_read_line_generic(int fd,
				      char **src, size_t *src_len,
				      int *dst_len)
{
	int len = packet_read(fd, src, src_len,
			      packet_buffer, sizeof(packet_buffer),
			      PACKET_READ_CHOMP_NEWLINE);
	if (dst_len)
		*dst_len = len;
	return (len > 0) ? packet_buffer : NULL;
}

char *packet_read_line(int fd, int *len_p)
{
	return packet_read_line_generic(fd, NULL, NULL, len_p);
}

int packet_read_line_gently(int fd, int *dst_len, char **dst_line)
{
	int len = packet_read(fd, NULL, NULL,
			      packet_buffer, sizeof(packet_buffer),
			      PACKET_READ_CHOMP_NEWLINE|PACKET_READ_GENTLE_ON_EOF);
	if (dst_len)
		*dst_len = len;
	if (dst_line)
		*dst_line = (len > 0) ? packet_buffer : NULL;
	return len;
}

char *packet_read_line_buf(char **src, size_t *src_len, int *dst_len)
{
	return packet_read_line_generic(-1, src, src_len, dst_len);
}

ssize_t read_packetized_to_strbuf(int fd_in, struct strbuf *sb_out, int options)
{
	int packet_len;

	size_t orig_len = sb_out->len;
	size_t orig_alloc = sb_out->alloc;

	for (;;) {
		strbuf_grow(sb_out, LARGE_PACKET_DATA_MAX);
		packet_len = packet_read(fd_in, NULL, NULL,
			/* strbuf_grow() above always allocates one extra byte to
			 * store a '\0' at the end of the string. packet_read()
			 * writes a '\0' extra byte at the end, too. Let it know
			 * that there is already room for the extra byte.
			 */
			sb_out->buf + sb_out->len, LARGE_PACKET_DATA_MAX+1,
			options);
		if (packet_len <= 0)
			break;
		sb_out->len += packet_len;
	}

	if (packet_len < 0) {
		if (orig_alloc == 0)
			strbuf_release(sb_out);
		else
			strbuf_setlen(sb_out, orig_len);
		return packet_len;
	}
	return sb_out->len - orig_len;
}

int recv_sideband(const char *me, int in_stream, int out)
{
	char buf[LARGE_PACKET_MAX + 1];
	int len;
	struct strbuf scratch = STRBUF_INIT;
	enum sideband_type sideband_type;

	while (1) {
		int status = packet_read_with_status(in_stream, NULL, NULL,
						     buf, LARGE_PACKET_MAX,
						     &len,
						     PACKET_READ_GENTLE_ON_EOF);
		if (!demultiplex_sideband(me, status, buf, len, 0, &scratch,
					  &sideband_type))
			continue;
		switch (sideband_type) {
		case SIDEBAND_PRIMARY:
			write_or_die(out, buf + 1, len - 1);
			break;
		default: /* errors: message already written */
			if (scratch.len > 0)
				BUG("unhandled incomplete sideband: '%s'",
				    scratch.buf);
			return sideband_type;
		}
	}
}

/* Packet Reader Functions */
void packet_reader_init(struct packet_reader *reader, int fd,
			char *src_buffer, size_t src_len,
			int options)
{
	memset(reader, 0, sizeof(*reader));

	reader->fd = fd;
	reader->src_buffer = src_buffer;
	reader->src_len = src_len;
	reader->buffer = packet_buffer;
	reader->buffer_size = sizeof(packet_buffer);
	reader->options = options;
	reader->me = "git";
	reader->hash_algo = &hash_algos[GIT_HASH_SHA1];
}

enum packet_read_status packet_reader_read(struct packet_reader *reader)
{
	struct strbuf scratch = STRBUF_INIT;

	if (reader->line_peeked) {
		reader->line_peeked = 0;
		return reader->status;
	}

	/*
	 * Consume all progress packets until a primary payload packet is
	 * received
	 */
	while (1) {
		enum sideband_type sideband_type;
		reader->status = packet_read_with_status(reader->fd,
							 &reader->src_buffer,
							 &reader->src_len,
							 reader->buffer,
							 reader->buffer_size,
							 &reader->pktlen,
							 reader->options);
		if (!reader->use_sideband)
			break;
		if (demultiplex_sideband(reader->me, reader->status,
					 reader->buffer, reader->pktlen, 1,
					 &scratch, &sideband_type))
			break;
	}

	if (reader->status == PACKET_READ_NORMAL)
		/* Skip the sideband designator if sideband is used */
		reader->line = reader->use_sideband ?
			reader->buffer + 1 : reader->buffer;
	else
		reader->line = NULL;

	return reader->status;
}

enum packet_read_status packet_reader_peek(struct packet_reader *reader)
{
	/* Only allow peeking a single line */
	if (reader->line_peeked)
		return reader->status;

	/* Peek a line by reading it and setting peeked flag */
	packet_reader_read(reader);
	reader->line_peeked = 1;
	return reader->status;
}

void packet_writer_init(struct packet_writer *writer, int dest_fd)
{
	writer->dest_fd = dest_fd;
	writer->use_sideband = 0;
}

void packet_writer_write(struct packet_writer *writer, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	packet_write_fmt_1(writer->dest_fd, 0,
			   writer->use_sideband ? "\001" : "", fmt, args);
	va_end(args);
}

void packet_writer_error(struct packet_writer *writer, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	packet_write_fmt_1(writer->dest_fd, 0,
			   writer->use_sideband ? "\003" : "ERR ", fmt, args);
	va_end(args);
}

void packet_writer_delim(struct packet_writer *writer)
{
	packet_delim(writer->dest_fd);
}

void packet_writer_flush(struct packet_writer *writer)
{
	packet_flush(writer->dest_fd);
}
