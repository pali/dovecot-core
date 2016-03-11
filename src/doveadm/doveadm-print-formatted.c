#include "lib.h"
#include "array.h"
#include "str.h"
#include "ostream.h"
#include "client-connection.h"
#include "doveadm-server.h"
#include "doveadm-print.h"
#include "doveadm-print-private.h"
#include "var-expand.h"

struct doveadm_print_formatted_context {
	pool_t pool;
	const char *format;
	ARRAY(struct var_expand_table) headers;
	string_t *buf;
	string_t *vbuf;
	unsigned int idx;
};

static struct doveadm_print_formatted_context ctx;

void doveadm_print_formatted_set_format(const char *format)
{
        ctx.format = format;
}

static void doveadm_print_formatted_init(void)
{
	memset(&ctx,0,sizeof(ctx));
	ctx.pool = pool_alloconly_create("doveadm formatted print", 1024);
	ctx.buf = str_new(ctx.pool, 256);
	p_array_init(&ctx.headers, ctx.pool, 8);
	ctx.idx = 0;
}

static void
doveadm_print_formatted_header(const struct doveadm_print_header *hdr)
{
	struct var_expand_table entry;
	memset(&entry, 0, sizeof(entry));
	entry.key = '\0';
	entry.long_key = p_strdup(ctx.pool, hdr->key);
	entry.value = NULL;
	array_append(&ctx.headers, &entry, 1);
}


static void doveadm_print_formatted_flush(void)
{
	o_stream_nsend(doveadm_print_ostream, str_data(ctx.buf), str_len(ctx.buf));
	str_truncate(ctx.buf, 0);
}

static void doveadm_print_formatted_print(const char *value)
{
	struct var_expand_table *entry = array_idx_modifiable(&ctx.headers, ctx.idx++);
	entry->value = value;

	if (ctx.idx >= array_count(&ctx.headers)) {
		var_expand(ctx.buf, ctx.format, array_idx(&ctx.headers,0));
		doveadm_print_formatted_flush();
		ctx.idx = 0;
	}

}

static void doveadm_print_formatted_deinit(void)
{
	pool_unref(&ctx.pool);
}

struct doveadm_print_vfuncs doveadm_print_formatted_vfuncs = {
	"formatted",

	doveadm_print_formatted_init,
	doveadm_print_formatted_deinit,
	doveadm_print_formatted_header,
	doveadm_print_formatted_print,
	NULL,
	doveadm_print_formatted_flush
};
