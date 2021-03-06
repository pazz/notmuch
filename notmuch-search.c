/* notmuch - Not much of an email program, (just index and search)
 *
 * Copyright © 2009 Carl Worth
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/ .
 *
 * Author: Carl Worth <cworth@cworth.org>
 */

#include "notmuch-client.h"
#include "sprinter.h"
#include "string-util.h"

typedef enum {
    /* Search command */
    OUTPUT_SUMMARY	= 1 << 0,
    OUTPUT_THREADS	= 1 << 1,
    OUTPUT_MESSAGES	= 1 << 2,
    OUTPUT_FILES	= 1 << 3,
    OUTPUT_TAGS		= 1 << 4,

    /* Address command */
    OUTPUT_SENDER	= 1 << 5,
    OUTPUT_RECIPIENTS	= 1 << 6,
    OUTPUT_COUNT	= 1 << 7,
} output_t;

typedef enum {
    NOTMUCH_FORMAT_JSON,
    NOTMUCH_FORMAT_TEXT,
    NOTMUCH_FORMAT_TEXT0,
    NOTMUCH_FORMAT_SEXP
} format_sel_t;

typedef struct {
    notmuch_database_t *notmuch;
    format_sel_t format_sel;
    sprinter_t *format;
    notmuch_exclude_t exclude;
    notmuch_query_t *query;
    notmuch_sort_t sort;
    output_t output;
    int offset;
    int limit;
    int dupe;
    GHashTable *addresses;
} search_context_t;

typedef struct {
    const char *name;
    const char *addr;
    int count;
} mailbox_t;

/* Return two stable query strings that identify exactly the matched
 * and unmatched messages currently in thread.  If there are no
 * matched or unmatched messages, the returned buffers will be
 * NULL. */
static int
get_thread_query (notmuch_thread_t *thread,
		  char **matched_out, char **unmatched_out)
{
    notmuch_messages_t *messages;
    char *escaped = NULL;
    size_t escaped_len = 0;

    *matched_out = *unmatched_out = NULL;

    for (messages = notmuch_thread_get_messages (thread);
	 notmuch_messages_valid (messages);
	 notmuch_messages_move_to_next (messages))
    {
	notmuch_message_t *message = notmuch_messages_get (messages);
	const char *mid = notmuch_message_get_message_id (message);
	/* Determine which query buffer to extend */
	char **buf = notmuch_message_get_flag (
	    message, NOTMUCH_MESSAGE_FLAG_MATCH) ? matched_out : unmatched_out;
	/* Add this message's id: query.  Since "id" is an exclusive
	 * prefix, it is implicitly 'or'd together, so we only need to
	 * join queries with a space. */
	if (make_boolean_term (thread, "id", mid, &escaped, &escaped_len) < 0)
	    return -1;
	if (*buf)
	    *buf = talloc_asprintf_append_buffer (*buf, " %s", escaped);
	else
	    *buf = talloc_strdup (thread, escaped);
	if (!*buf)
	    return -1;
    }
    talloc_free (escaped);
    return 0;
}

static int
do_search_threads (search_context_t *ctx)
{
    notmuch_thread_t *thread;
    notmuch_threads_t *threads;
    notmuch_tags_t *tags;
    sprinter_t *format = ctx->format;
    time_t date;
    int i;

    if (ctx->offset < 0) {
	ctx->offset += notmuch_query_count_threads (ctx->query);
	if (ctx->offset < 0)
	    ctx->offset = 0;
    }

    threads = notmuch_query_search_threads (ctx->query);
    if (threads == NULL)
	return 1;

    format->begin_list (format);

    for (i = 0;
	 notmuch_threads_valid (threads) && (ctx->limit < 0 || i < ctx->offset + ctx->limit);
	 notmuch_threads_move_to_next (threads), i++)
    {
	thread = notmuch_threads_get (threads);

	if (i < ctx->offset) {
	    notmuch_thread_destroy (thread);
	    continue;
	}

	if (ctx->output == OUTPUT_THREADS) {
	    format->set_prefix (format, "thread");
	    format->string (format,
			    notmuch_thread_get_thread_id (thread));
	    format->separator (format);
	} else { /* output == OUTPUT_SUMMARY */
	    void *ctx_quote = talloc_new (thread);
	    const char *authors = notmuch_thread_get_authors (thread);
	    const char *subject = notmuch_thread_get_subject (thread);
	    const char *thread_id = notmuch_thread_get_thread_id (thread);
	    int matched = notmuch_thread_get_matched_messages (thread);
	    int total = notmuch_thread_get_total_messages (thread);
	    const char *relative_date = NULL;
	    notmuch_bool_t first_tag = TRUE;

	    format->begin_map (format);

	    if (ctx->sort == NOTMUCH_SORT_OLDEST_FIRST)
		date = notmuch_thread_get_oldest_date (thread);
	    else
		date = notmuch_thread_get_newest_date (thread);

	    relative_date = notmuch_time_relative_date (ctx_quote, date);

	    if (format->is_text_printer) {
                /* Special case for the text formatter */
		printf ("thread:%s %12s [%d/%d] %s; %s (",
			thread_id,
			relative_date,
			matched,
			total,
			sanitize_string (ctx_quote, authors),
			sanitize_string (ctx_quote, subject));
	    } else { /* Structured Output */
		format->map_key (format, "thread");
		format->string (format, thread_id);
		format->map_key (format, "timestamp");
		format->integer (format, date);
		format->map_key (format, "date_relative");
		format->string (format, relative_date);
		format->map_key (format, "matched");
		format->integer (format, matched);
		format->map_key (format, "total");
		format->integer (format, total);
		format->map_key (format, "authors");
		format->string (format, authors);
		format->map_key (format, "subject");
		format->string (format, subject);
		if (notmuch_format_version >= 2) {
		    char *matched_query, *unmatched_query;
		    if (get_thread_query (thread, &matched_query,
					  &unmatched_query) < 0) {
			fprintf (stderr, "Out of memory\n");
			return 1;
		    }
		    format->map_key (format, "query");
		    format->begin_list (format);
		    if (matched_query)
			format->string (format, matched_query);
		    else
			format->null (format);
		    if (unmatched_query)
			format->string (format, unmatched_query);
		    else
			format->null (format);
		    format->end (format);
		}
	    }

	    talloc_free (ctx_quote);

	    format->map_key (format, "tags");
	    format->begin_list (format);

	    for (tags = notmuch_thread_get_tags (thread);
		 notmuch_tags_valid (tags);
		 notmuch_tags_move_to_next (tags))
	    {
		const char *tag = notmuch_tags_get (tags);

		if (format->is_text_printer) {
                  /* Special case for the text formatter */
		    if (first_tag)
			first_tag = FALSE;
		    else
			fputc (' ', stdout);
		    fputs (tag, stdout);
		} else { /* Structured Output */
		    format->string (format, tag);
		}
	    }

	    if (format->is_text_printer)
		printf (")");

	    format->end (format);
	    format->end (format);
	    format->separator (format);
	}

	notmuch_thread_destroy (thread);
    }

    format->end (format);

    return 0;
}

/* Returns TRUE iff name and addr is duplicate. If not, stores the
 * name/addr pair in order to detect subsequent duplicates. */
static notmuch_bool_t
is_duplicate (const search_context_t *ctx, const char *name, const char *addr)
{
    notmuch_bool_t duplicate;
    char *key;
    mailbox_t *mailbox;

    key = talloc_asprintf (ctx->format, "%s <%s>", name, addr);
    if (! key)
	return FALSE;

    duplicate = g_hash_table_lookup_extended (ctx->addresses, key, NULL, (gpointer)&mailbox);

    if (! duplicate) {
	mailbox = talloc (ctx->format, mailbox_t);
	mailbox->name = talloc_strdup (mailbox, name);
	mailbox->addr = talloc_strdup (mailbox, addr);
	mailbox->count = 1;
	g_hash_table_insert (ctx->addresses, key, mailbox);
    } else {
	mailbox->count++;
	talloc_free (key);
    }

    return duplicate;
}

static void
print_mailbox (const search_context_t *ctx, const mailbox_t *mailbox)
{
    const char *name = mailbox->name;
    const char *addr = mailbox->addr;
    int count = mailbox->count;
    sprinter_t *format = ctx->format;
    InternetAddress *ia = internet_address_mailbox_new (name, addr);
    char *name_addr;

    /* name_addr has the name part quoted if necessary. Compare
     * 'John Doe <john@doe.com>' vs. '"Doe, John" <john@doe.com>' */
    name_addr = internet_address_to_string (ia, FALSE);

    if (format->is_text_printer) {
	if (count > 0) {
	    format->integer (format, count);
	    format->string (format, "\t");
	}
	format->string (format, name_addr);
	format->separator (format);
    } else {
	format->begin_map (format);
	format->map_key (format, "name");
	format->string (format, name);
	format->map_key (format, "address");
	format->string (format, addr);
	format->map_key (format, "name-addr");
	format->string (format, name_addr);
	if (count > 0) {
	    format->map_key (format, "count");
	    format->integer (format, count);
	}
	format->end (format);
	format->separator (format);
    }

    g_object_unref (ia);
    g_free (name_addr);
}

/* Print or prepare for printing addresses from InternetAddressList. */
static void
process_address_list (const search_context_t *ctx,
		      InternetAddressList *list)
{
    InternetAddress *address;
    int i;

    for (i = 0; i < internet_address_list_length (list); i++) {
	address = internet_address_list_get_address (list, i);
	if (INTERNET_ADDRESS_IS_GROUP (address)) {
	    InternetAddressGroup *group;
	    InternetAddressList *group_list;

	    group = INTERNET_ADDRESS_GROUP (address);
	    group_list = internet_address_group_get_members (group);
	    if (group_list == NULL)
		continue;

	    process_address_list (ctx, group_list);
	} else {
	    InternetAddressMailbox *mailbox = INTERNET_ADDRESS_MAILBOX (address);
	    mailbox_t mbx = {
		.name = internet_address_get_name (address),
		.addr = internet_address_mailbox_get_addr (mailbox),
		.count = 0,
	    };

	    if (is_duplicate (ctx, mbx.name, mbx.addr))
		continue;

	    if (ctx->output & OUTPUT_COUNT)
		continue;

	    print_mailbox (ctx, &mbx);
	}
    }
}

/* Print or prepare for printing addresses from a message header. */
static void
process_address_header (const search_context_t *ctx, const char *value)
{
    InternetAddressList *list;

    if (value == NULL)
	return;

    list = internet_address_list_parse_string (value);
    if (list == NULL)
	return;

    process_address_list (ctx, list);

    g_object_unref (list);
}

/* Destructor for talloc-allocated GHashTable keys and values. */
static void
_talloc_free_for_g_hash (void *ptr)
{
    talloc_free (ptr);
}

static void
print_hash_value (unused (gpointer key), gpointer value, gpointer user_data)
{
    const mailbox_t *mailbox = value;
    search_context_t *ctx = user_data;

    print_mailbox (ctx, mailbox);
}

static int
_count_filenames (notmuch_message_t *message)
{
    notmuch_filenames_t *filenames;
    int i = 0;

    filenames = notmuch_message_get_filenames (message);

    while (notmuch_filenames_valid (filenames)) {
        notmuch_filenames_move_to_next (filenames);
        i++;
    }

    notmuch_filenames_destroy (filenames);

    return i;
}

static int
do_search_messages (search_context_t *ctx)
{
    notmuch_message_t *message;
    notmuch_messages_t *messages;
    notmuch_filenames_t *filenames;
    sprinter_t *format = ctx->format;
    int i;

    if (ctx->offset < 0) {
	ctx->offset += notmuch_query_count_messages (ctx->query);
	if (ctx->offset < 0)
	    ctx->offset = 0;
    }

    messages = notmuch_query_search_messages (ctx->query);
    if (messages == NULL)
	return 1;

    format->begin_list (format);

    for (i = 0;
	 notmuch_messages_valid (messages) && (ctx->limit < 0 || i < ctx->offset + ctx->limit);
	 notmuch_messages_move_to_next (messages), i++)
    {
	if (i < ctx->offset)
	    continue;

	message = notmuch_messages_get (messages);

	if (ctx->output == OUTPUT_FILES) {
	    int j;
	    filenames = notmuch_message_get_filenames (message);

	    for (j = 1;
		 notmuch_filenames_valid (filenames);
		 notmuch_filenames_move_to_next (filenames), j++)
	    {
		if (ctx->dupe < 0 || ctx->dupe == j) {
		    format->string (format, notmuch_filenames_get (filenames));
		    format->separator (format);
		}
	    }
	    
	    notmuch_filenames_destroy( filenames );

	} else if (ctx->output == OUTPUT_MESSAGES) {
            /* special case 1 for speed */
            if (ctx->dupe <= 1 || ctx->dupe <= _count_filenames (message)) {
                format->set_prefix (format, "id");
                format->string (format,
                                notmuch_message_get_message_id (message));
                format->separator (format);
            }
	} else {
	    if (ctx->output & OUTPUT_SENDER) {
		const char *addrs;

		addrs = notmuch_message_get_header (message, "from");
		process_address_header (ctx, addrs);
	    }

	    if (ctx->output & OUTPUT_RECIPIENTS) {
		const char *hdrs[] = { "to", "cc", "bcc" };
		const char *addrs;
		size_t j;

		for (j = 0; j < ARRAY_SIZE (hdrs); j++) {
		    addrs = notmuch_message_get_header (message, hdrs[j]);
		    process_address_header (ctx, addrs);
		}
	    }
	}

	notmuch_message_destroy (message);
    }

    if (ctx->addresses && ctx->output & OUTPUT_COUNT)
	g_hash_table_foreach (ctx->addresses, print_hash_value, ctx);

    notmuch_messages_destroy (messages);

    format->end (format);

    return 0;
}

static int
do_search_tags (const search_context_t *ctx)
{
    notmuch_messages_t *messages = NULL;
    notmuch_tags_t *tags;
    const char *tag;
    sprinter_t *format = ctx->format;
    notmuch_query_t *query = ctx->query;
    notmuch_database_t *notmuch = ctx->notmuch;

    /* should the following only special case if no excluded terms
     * specified? */

    /* Special-case query of "*" for better performance. */
    if (strcmp (notmuch_query_get_query_string (query), "*") == 0) {
	tags = notmuch_database_get_all_tags (notmuch);
    } else {
	messages = notmuch_query_search_messages (query);
	if (messages == NULL)
	    return 1;

	tags = notmuch_messages_collect_tags (messages);
    }
    if (tags == NULL)
	return 1;

    format->begin_list (format);

    for (;
	 notmuch_tags_valid (tags);
	 notmuch_tags_move_to_next (tags))
    {
	tag = notmuch_tags_get (tags);

	format->string (format, tag);
	format->separator (format);

    }

    notmuch_tags_destroy (tags);

    if (messages)
	notmuch_messages_destroy (messages);

    format->end (format);

    return 0;
}

static int
_notmuch_search_prepare (search_context_t *ctx, notmuch_config_t *config, int argc, char *argv[])
{
    char *query_str;
    unsigned int i;
    char *status_string = NULL;

    switch (ctx->format_sel) {
    case NOTMUCH_FORMAT_TEXT:
	ctx->format = sprinter_text_create (config, stdout);
	break;
    case NOTMUCH_FORMAT_TEXT0:
	if (ctx->output == OUTPUT_SUMMARY) {
	    fprintf (stderr, "Error: --format=text0 is not compatible with --output=summary.\n");
	    return EXIT_FAILURE;
	}
	ctx->format = sprinter_text0_create (config, stdout);
	break;
    case NOTMUCH_FORMAT_JSON:
	ctx->format = sprinter_json_create (config, stdout);
	break;
    case NOTMUCH_FORMAT_SEXP:
	ctx->format = sprinter_sexp_create (config, stdout);
	break;
    default:
	/* this should never happen */
	INTERNAL_ERROR("no output format selected");
    }

    notmuch_exit_if_unsupported_format ();

    if (notmuch_database_open_verbose (
	    notmuch_config_get_database_path (config),
	    NOTMUCH_DATABASE_MODE_READ_ONLY, &ctx->notmuch, &status_string)) {

	if (status_string) {
	    fputs (status_string, stderr);
	    free (status_string);
	}

	return EXIT_FAILURE;
    }

    query_str = query_string_from_args (ctx->notmuch, argc, argv);
    if (query_str == NULL) {
	fprintf (stderr, "Out of memory.\n");
	return EXIT_FAILURE;
    }
    if (*query_str == '\0') {
	fprintf (stderr, "Error: notmuch search requires at least one search term.\n");
	return EXIT_FAILURE;
    }

    ctx->query = notmuch_query_create (ctx->notmuch, query_str);
    if (ctx->query == NULL) {
	fprintf (stderr, "Out of memory\n");
	return EXIT_FAILURE;
    }

    notmuch_query_set_sort (ctx->query, ctx->sort);

    if (ctx->exclude == NOTMUCH_EXCLUDE_FLAG && ctx->output != OUTPUT_SUMMARY) {
	/* If we are not doing summary output there is nowhere to
	 * print the excluded flag so fall back on including the
	 * excluded messages. */
	fprintf (stderr, "Warning: this output format cannot flag excluded messages.\n");
	ctx->exclude = NOTMUCH_EXCLUDE_FALSE;
    }

    if (ctx->exclude != NOTMUCH_EXCLUDE_FALSE) {
	const char **search_exclude_tags;
	size_t search_exclude_tags_length;

	search_exclude_tags = notmuch_config_get_search_exclude_tags
	    (config, &search_exclude_tags_length);
	for (i = 0; i < search_exclude_tags_length; i++)
	    notmuch_query_add_tag_exclude (ctx->query, search_exclude_tags[i]);
	notmuch_query_set_omit_excluded (ctx->query, ctx->exclude);
    }

    return 0;
}

static void
_notmuch_search_cleanup (search_context_t *ctx)
{
    notmuch_query_destroy (ctx->query);
    notmuch_database_destroy (ctx->notmuch);

    talloc_free (ctx->format);
}

static search_context_t search_context = {
    .format_sel = NOTMUCH_FORMAT_TEXT,
    .exclude = NOTMUCH_EXCLUDE_TRUE,
    .sort = NOTMUCH_SORT_NEWEST_FIRST,
    .output = 0,
    .offset = 0,
    .limit = -1, /* unlimited */
    .dupe = -1,
};

static const notmuch_opt_desc_t common_options[] = {
    { NOTMUCH_OPT_KEYWORD, &search_context.sort, "sort", 's',
      (notmuch_keyword_t []){ { "oldest-first", NOTMUCH_SORT_OLDEST_FIRST },
			      { "newest-first", NOTMUCH_SORT_NEWEST_FIRST },
			      { 0, 0 } } },
    { NOTMUCH_OPT_KEYWORD, &search_context.format_sel, "format", 'f',
      (notmuch_keyword_t []){ { "json", NOTMUCH_FORMAT_JSON },
			      { "sexp", NOTMUCH_FORMAT_SEXP },
			      { "text", NOTMUCH_FORMAT_TEXT },
			      { "text0", NOTMUCH_FORMAT_TEXT0 },
			      { 0, 0 } } },
    { NOTMUCH_OPT_INT, &notmuch_format_version, "format-version", 0, 0 },
    { 0, 0, 0, 0, 0 }
};

int
notmuch_search_command (notmuch_config_t *config, int argc, char *argv[])
{
    search_context_t *ctx = &search_context;
    int opt_index, ret;

    notmuch_opt_desc_t options[] = {
	{ NOTMUCH_OPT_KEYWORD, &ctx->output, "output", 'o',
	  (notmuch_keyword_t []){ { "summary", OUTPUT_SUMMARY },
				  { "threads", OUTPUT_THREADS },
				  { "messages", OUTPUT_MESSAGES },
				  { "files", OUTPUT_FILES },
				  { "tags", OUTPUT_TAGS },
				  { 0, 0 } } },
        { NOTMUCH_OPT_KEYWORD, &ctx->exclude, "exclude", 'x',
          (notmuch_keyword_t []){ { "true", NOTMUCH_EXCLUDE_TRUE },
                                  { "false", NOTMUCH_EXCLUDE_FALSE },
                                  { "flag", NOTMUCH_EXCLUDE_FLAG },
                                  { "all", NOTMUCH_EXCLUDE_ALL },
                                  { 0, 0 } } },
	{ NOTMUCH_OPT_INT, &ctx->offset, "offset", 'O', 0 },
	{ NOTMUCH_OPT_INT, &ctx->limit, "limit", 'L', 0  },
	{ NOTMUCH_OPT_INT, &ctx->dupe, "duplicate", 'D', 0  },
	{ NOTMUCH_OPT_INHERIT, (void *) &common_options, NULL, 0, 0 },
	{ 0, 0, 0, 0, 0 }
    };

    ctx->output = OUTPUT_SUMMARY;
    opt_index = parse_arguments (argc, argv, options, 1);
    if (opt_index < 0)
	return EXIT_FAILURE;

    if (ctx->output != OUTPUT_FILES && ctx->output != OUTPUT_MESSAGES &&
	ctx->dupe != -1) {
        fprintf (stderr, "Error: --duplicate=N is only supported with --output=files and --output=messages.\n");
        return EXIT_FAILURE;
    }

    if (_notmuch_search_prepare (ctx, config,
				 argc - opt_index, argv + opt_index))
	return EXIT_FAILURE;

    switch (ctx->output) {
    case OUTPUT_SUMMARY:
    case OUTPUT_THREADS:
	ret = do_search_threads (ctx);
	break;
    case OUTPUT_MESSAGES:
    case OUTPUT_FILES:
	ret = do_search_messages (ctx);
	break;
    case OUTPUT_TAGS:
	ret = do_search_tags (ctx);
	break;
    default:
	INTERNAL_ERROR ("Unexpected output");
    }

    _notmuch_search_cleanup (ctx);

    return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}

int
notmuch_address_command (notmuch_config_t *config, int argc, char *argv[])
{
    search_context_t *ctx = &search_context;
    int opt_index, ret;

    notmuch_opt_desc_t options[] = {
	{ NOTMUCH_OPT_KEYWORD_FLAGS, &ctx->output, "output", 'o',
	  (notmuch_keyword_t []){ { "sender", OUTPUT_SENDER },
				  { "recipients", OUTPUT_RECIPIENTS },
				  { "count", OUTPUT_COUNT },
				  { 0, 0 } } },
	{ NOTMUCH_OPT_KEYWORD, &ctx->exclude, "exclude", 'x',
	  (notmuch_keyword_t []){ { "true", NOTMUCH_EXCLUDE_TRUE },
				  { "false", NOTMUCH_EXCLUDE_FALSE },
				  { 0, 0 } } },
	{ NOTMUCH_OPT_INHERIT, (void *) &common_options, NULL, 0, 0 },
	{ 0, 0, 0, 0, 0 }
    };

    opt_index = parse_arguments (argc, argv, options, 1);
    if (opt_index < 0)
	return EXIT_FAILURE;

    if (! (ctx->output & (OUTPUT_SENDER | OUTPUT_RECIPIENTS)))
	ctx->output |= OUTPUT_SENDER;

    if (_notmuch_search_prepare (ctx, config,
				 argc - opt_index, argv + opt_index))
	return EXIT_FAILURE;

    ctx->addresses = g_hash_table_new_full (g_str_hash, g_str_equal,
					    _talloc_free_for_g_hash, _talloc_free_for_g_hash);

    ret = do_search_messages (ctx);

    g_hash_table_unref (ctx->addresses);


    _notmuch_search_cleanup (ctx);

    return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
