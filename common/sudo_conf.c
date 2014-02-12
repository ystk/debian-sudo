/*
 * Copyright (c) 2009-2011 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <config.h>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <stdio.h>
#ifdef STDC_HEADERS
# include <stdlib.h>
# include <stddef.h>
#else
# ifdef HAVE_STDLIB_H
#  include <stdlib.h>
# endif
#endif /* STDC_HEADERS */
#ifdef HAVE_STDBOOL_H
# include <stdbool.h>
#else
# include "compat/stdbool.h"
#endif
#ifdef HAVE_STRING_H
# include <string.h>
#endif /* HAVE_STRING_H */
#ifdef HAVE_STRINGS_H
# include <strings.h>
#endif /* HAVE_STRINGS_H */
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <ctype.h>
#include <errno.h>

#define SUDO_ERROR_WRAP	0

#include "missing.h"
#include "alloc.h"
#include "error.h"
#include "fileops.h"
#include "pathnames.h"
#include "sudo_plugin.h"
#include "sudo_conf.h"
#include "sudo_debug.h"
#include "secure_path.h"
#include "gettext.h"

#ifdef __TANDEM
# define ROOT_UID	65535
#else
# define ROOT_UID	0
#endif

#ifndef _PATH_SUDO_ASKPASS
# define _PATH_SUDO_ASKPASS	NULL
#endif

extern bool atobool(const char *str); /* atobool.c */

struct sudo_conf_table {
    const char *name;
    unsigned int namelen;
    bool (*setter)(const char *entry);
};

struct sudo_conf_paths {
    const char *pname;
    unsigned int pnamelen;
    const char *pval;
};

static bool set_debug(const char *entry);
static bool set_path(const char *entry);
static bool set_plugin(const char *entry);
static bool set_variable(const char *entry);

static struct sudo_conf_table sudo_conf_table[] = {
    { "Debug", sizeof("Debug") - 1, set_debug },
    { "Path", sizeof("Path") - 1, set_path },
    { "Plugin", sizeof("Plugin") - 1, set_plugin },
    { "Set", sizeof("Set") - 1, set_variable },
    { NULL }
};

static struct sudo_conf_data {
    bool disable_coredump;
    const char *debug_flags;
    struct sudo_conf_paths paths[3];
    struct plugin_info_list plugins;
} sudo_conf_data = {
    true,
    NULL,
    {
#define SUDO_CONF_ASKPASS_IDX	0
	{ "askpass", sizeof("askpass") - 1, _PATH_SUDO_ASKPASS },
#ifdef _PATH_SUDO_NOEXEC
#define SUDO_CONF_NOEXEC_IDX	1
	{ "noexec", sizeof("noexec") - 1, _PATH_SUDO_NOEXEC },
#endif
	{ NULL }
    }
};

/*
 * "Set variable_name value"
 */
static bool
set_variable(const char *entry)
{
#undef DC_LEN
#define DC_LEN (sizeof("disable_coredump") - 1)
    /* Currently the only variable supported is "disable_coredump". */
    if (strncmp(entry, "disable_coredump", DC_LEN) == 0 &&
	isblank((unsigned char)entry[DC_LEN])) {
	entry += DC_LEN + 1;
	while (isblank((unsigned char)*entry))
	    entry++;
	sudo_conf_data.disable_coredump = atobool(entry);
    }
#undef DC_LEN
    return true;
}

/*
 * "Debug progname debug_file debug_flags"
 */
static bool
set_debug(const char *entry)
{
    size_t filelen, proglen;
    const char *progname;
    char *debug_file, *debug_flags;

    /* Is this debug setting for me? */
    progname = getprogname();
    if (strcmp(progname, "sudoedit") == 0)
	progname = "sudo";
    proglen = strlen(progname);
    if (strncmp(entry, progname, proglen) != 0 ||
	!isblank((unsigned char)entry[proglen]))
    	return false;
    entry += proglen + 1;
    while (isblank((unsigned char)*entry))
	entry++;

    debug_flags = strpbrk(entry, " \t");
    if (debug_flags == NULL)
    	return false;
    filelen = (size_t)(debug_flags - entry);
    while (isblank((unsigned char)*debug_flags))
	debug_flags++;

    /* Set debug file and parse the flags (init debug as soon as possible). */
    debug_file = estrndup(entry, filelen);
    debug_flags = estrdup(debug_flags);
    sudo_debug_init(debug_file, debug_flags);
    efree(debug_file);

    sudo_conf_data.debug_flags = debug_flags;

    return true;
}

static bool
set_path(const char *entry)
{
    const char *name, *path;
    struct sudo_conf_paths *cur;

    /* Parse Path line */
    name = entry;
    path = strpbrk(entry, " \t");
    if (path == NULL)
    	return false;
    while (isblank((unsigned char)*path))
	path++;

    /* Match supported paths, ignore the rest. */
    for (cur = sudo_conf_data.paths; cur->pname != NULL; cur++) {
	if (strncasecmp(name, cur->pname, cur->pnamelen) == 0 &&
	    isblank((unsigned char)name[cur->pnamelen])) {
	    cur->pval = estrdup(path);
	    break;
	}
    }

    return true;
}

static bool
set_plugin(const char *entry)
{
    struct plugin_info *info;
    const char *name, *path, *cp, *ep;
    char **options = NULL;
    size_t namelen, pathlen;
    unsigned int nopts;

    /* Parse Plugin line */
    name = entry;
    path = strpbrk(entry, " \t");
    if (path == NULL)
    	return false;
    namelen = (size_t)(path - name);
    while (isblank((unsigned char)*path))
	path++;
    if ((cp = strpbrk(path, " \t")) != NULL) {
	/* Convert any options to an array. */
	pathlen = (size_t)(cp - path);
	while (isblank((unsigned char)*cp))
	    cp++;
	/* Count number of options and allocate array. */
	for (ep = cp, nopts = 1; (ep = strpbrk(ep, " \t")) != NULL; nopts++) {
	    while (isblank((unsigned char)*ep))
		ep++;
	}
	options = emalloc2(nopts + 1, sizeof(*options));
	/* Fill in options array, there is at least one element. */
	for (nopts = 0; (ep = strpbrk(cp, " \t")) != NULL; ) {
	    options[nopts++] = estrndup(cp, (size_t)(ep - cp));
	    while (isblank((unsigned char)*ep))
		ep++;
	    cp = ep;
	}
	options[nopts++] = estrdup(cp);
	options[nopts] = NULL;
    } else {
	/* No extra options. */
	pathlen = strlen(path);
    }

    info = ecalloc(1, sizeof(*info));
    info->symbol_name = estrndup(name, namelen);
    info->path = estrndup(path, pathlen);
    info->options = options;
    info->prev = info;
    /* info->next = NULL; */
    tq_append(&sudo_conf_data.plugins, info);

    return true;
}

const char *
sudo_conf_askpass_path(void)
{
    return sudo_conf_data.paths[SUDO_CONF_ASKPASS_IDX].pval;
}

#ifdef _PATH_SUDO_NOEXEC
const char *
sudo_conf_noexec_path(void)
{
    return sudo_conf_data.paths[SUDO_CONF_NOEXEC_IDX].pval;
}
#endif

const char *
sudo_conf_debug_flags(void)
{
    return sudo_conf_data.debug_flags;
}

struct plugin_info_list *
sudo_conf_plugins(void)
{
    return &sudo_conf_data.plugins;
}

bool
sudo_conf_disable_coredump(void)
{
    return sudo_conf_data.disable_coredump;
}

/*
 * Reads in /etc/sudo.conf and populates sudo_conf_data.
 */
void
sudo_conf_read(void)
{
    struct sudo_conf_table *cur;
    struct plugin_info *info;
    struct stat sb;
    FILE *fp;
    char *cp;

    switch (sudo_secure_file(_PATH_SUDO_CONF, ROOT_UID, -1, &sb)) {
	case SUDO_PATH_SECURE:
	    break;
	case SUDO_PATH_MISSING:
	    /* Root should always be able to read sudo.conf. */
	    if (errno != ENOENT && geteuid() == ROOT_UID)
		warning(_("unable to stat %s"), _PATH_SUDO_CONF);
	    goto done;
	case SUDO_PATH_BAD_TYPE:
	    warningx(_("%s is not a regular file"), _PATH_SUDO_CONF);
	    goto done;
	case SUDO_PATH_WRONG_OWNER:
	    warningx(_("%s is owned by uid %u, should be %u"),
		_PATH_SUDO_CONF, (unsigned int) sb.st_uid, ROOT_UID);
	    goto done;
	case SUDO_PATH_WORLD_WRITABLE:
	    warningx(_("%s is world writable"), _PATH_SUDO_CONF);
	    goto done;
	case SUDO_PATH_GROUP_WRITABLE:
	    warningx(_("%s is group writable"), _PATH_SUDO_CONF);
	    goto done;
	default:
	    /* NOTREACHED */
	    goto done;
    }

    if ((fp = fopen(_PATH_SUDO_CONF, "r")) == NULL) {
	if (errno != ENOENT && geteuid() == ROOT_UID)
	    warning(_("unable to open %s"), _PATH_SUDO_CONF);
	goto done;
    }

    while ((cp = sudo_parseln(fp)) != NULL) {
	/* Skip blank or comment lines */
	if (*cp == '\0')
	    continue;

	for (cur = sudo_conf_table; cur->name != NULL; cur++) {
	    if (strncasecmp(cp, cur->name, cur->namelen) == 0 &&
		isblank((unsigned char)cp[cur->namelen])) {
		cp += cur->namelen;
		while (isblank((unsigned char)*cp))
		    cp++;
		if (cur->setter(cp))
		    break;
	    }
	}
    }
    fclose(fp);

done:
    if (tq_empty(&sudo_conf_data.plugins)) {
	/* Default policy plugin */
	info = ecalloc(1, sizeof(*info));
	info->symbol_name = "sudoers_policy";
	info->path = SUDOERS_PLUGIN;
	/* info->options = NULL; */
	info->prev = info;
	/* info->next = NULL; */
	tq_append(&sudo_conf_data.plugins, info);

	/* Default I/O plugin */
	info = ecalloc(1, sizeof(*info));
	info->symbol_name = "sudoers_io";
	info->path = SUDOERS_PLUGIN;
	/* info->options = NULL; */
	info->prev = info;
	/* info->next = NULL; */
	tq_append(&sudo_conf_data.plugins, info);
    }
}
