/*
 * Copyright (c) 1993-1996, 1998-2011 Todd C. Miller <Todd.Miller@courtesan.com>
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
 *
 * Sponsored in part by the Defense Advanced Research Projects
 * Agency (DARPA) and Air Force Research Laboratory, Air Force
 * Materiel Command, USAF, under agreement number F39502-99-1-0512.
 *
 * For a brief history of sudo, please see the HISTORY file included
 * with this distribution.
 */

#define _SUDO_MAIN

#ifdef __TANDEM
# include <floss.h>
#endif

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <stdio.h>
#ifdef STDC_HEADERS
# include <stdlib.h>
# include <stddef.h>
#else
# ifdef HAVE_STDLIB_H
#  include <stdlib.h>
# endif
#endif /* STDC_HEADERS */
#ifdef HAVE_STRING_H
# if defined(HAVE_MEMORY_H) && !defined(STDC_HEADERS)
#  include <memory.h>
# endif
# include <string.h>
#endif /* HAVE_STRING_H */
#ifdef HAVE_STRINGS_H
# include <strings.h>
#endif /* HAVE_STRINGS_H */
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <pwd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <grp.h>
#include <time.h>
#ifdef HAVE_SETLOCALE
# include <locale.h>
#endif
#include <netinet/in.h>
#include <netdb.h>
#ifdef HAVE_LOGIN_CAP_H
# include <login_cap.h>
# ifndef LOGIN_DEFROOTCLASS
#  define LOGIN_DEFROOTCLASS	"daemon"
# endif
# ifndef LOGIN_SETENV
#  define LOGIN_SETENV	0
# endif
#endif
#ifdef HAVE_SELINUX
# include <selinux/selinux.h>
#endif
#include <ctype.h>
#include <setjmp.h>
#ifndef HAVE_GETADDRINFO
# include "compat/getaddrinfo.h"
#endif

#include "sudoers.h"
#include "interfaces.h"
#include "sudoers_version.h"
#include "auth/sudo_auth.h"
#include "secure_path.h"

/*
 * Prototypes
 */
static void init_vars(char * const *);
static int set_cmnd(void);
static void set_loginclass(struct passwd *);
static void set_runaspw(const char *);
static void set_runasgr(const char *);
static int cb_runas_default(const char *);
static int sudoers_policy_version(int verbose);
static int deserialize_info(char * const args[], char * const settings[],
    char * const user_info[]);
static char *find_editor(int nfiles, char **files, char ***argv_out);
static void create_admin_success_flag(void);

/*
 * Globals
 */
struct sudo_user sudo_user;
struct passwd *list_pw;
struct interface *interfaces;
int long_list;
uid_t timestamp_uid;
extern int errorlineno;
extern bool parse_error;
extern char *errorfile;
#ifdef HAVE_BSD_AUTH_H
char *login_style;
#endif /* HAVE_BSD_AUTH_H */
sudo_conv_t sudo_conv;
sudo_printf_t sudo_printf;
int sudo_mode;

static int sudo_version;
static char *prev_user;
static char *runas_user;
static char *runas_group;
static struct sudo_nss_list *snl;
static const char *interfaces_string;
static sigaction_t saved_sa_int, saved_sa_quit, saved_sa_tstp;

/* XXX - must be extern for audit bits of sudo_auth.c */
int NewArgc;
char **NewArgv;

/* Declared here instead of plugin_error.c for static sudo builds. */
sigjmp_buf error_jmp;

static int
sudoers_policy_open(unsigned int version, sudo_conv_t conversation,
    sudo_printf_t plugin_printf, char * const settings[],
    char * const user_info[], char * const envp[], char * const args[])
{
    volatile int sources = 0;
    sigaction_t sa;
    struct sudo_nss *nss;
    debug_decl(sudoers_policy_open, SUDO_DEBUG_PLUGIN)

    sudo_version = version;
    if (!sudo_conv)
	sudo_conv = conversation;
    if (!sudo_printf)
	sudo_printf = plugin_printf;

    /* Plugin args are only specified for API version 1.2 and higher. */
    if (sudo_version < SUDO_API_MKVERSION(1, 2))
	args = NULL;

    if (sigsetjmp(error_jmp, 1)) {
	/* called via error(), errorx() or log_fatal() */
	rewind_perms();
	debug_return_bool(-1);
    }

    bindtextdomain("sudoers", LOCALEDIR);

    /*
     * Signal setup:
     *	Ignore keyboard-generated signals so the user cannot interrupt
     *  us at some point and avoid the logging.
     *  Install handler to wait for children when they exit.
     */
    zero_bytes(&sa, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = SIG_IGN;
    (void) sigaction(SIGINT, &sa, &saved_sa_int);
    (void) sigaction(SIGQUIT, &sa, &saved_sa_quit);
    (void) sigaction(SIGTSTP, &sa, &saved_sa_tstp);

    sudo_setpwent();
    sudo_setgrent();

    /* Initialize environment functions (including replacements). */
    env_init(envp);

    /* Setup defaults data structures. */
    init_defaults();

    /* Parse args, settings and user_info */
    sudo_mode = deserialize_info(args, settings, user_info);

    init_vars(envp);		/* XXX - move this later? */

    /* Parse nsswitch.conf for sudoers order. */
    snl = sudo_read_nss();

    /* LDAP or NSS may modify the euid so we need to be root for the open. */
    set_perms(PERM_INITIAL);
    set_perms(PERM_ROOT);

    /* Open and parse sudoers, set global defaults */
    tq_foreach_fwd(snl, nss) {
	if (nss->open(nss) == 0 && nss->parse(nss) == 0) {
	    sources++;
	    if (nss->setdefs(nss) != 0)
		log_error(NO_STDERR, _("problem with defaults entries"));
	}
    }
    if (sources == 0) {
	warningx(_("no valid sudoers sources found, quitting"));
	debug_return_bool(-1);
    }

    /* XXX - collect post-sudoers parse settings into a function */

    /*
     * Initialize external group plugin, if any.
     */
    if (def_group_plugin) {
	if (group_plugin_load(def_group_plugin) != true)
	    def_group_plugin = NULL;
    }

    /*
     * Set runas passwd/group entries based on command line or sudoers.
     * Note that if runas_group was specified without runas_user we
     * defer setting runas_pw so the match routines know to ignore it.
     */
    if (runas_group != NULL) {
	set_runasgr(runas_group);
	if (runas_user != NULL)
	    set_runaspw(runas_user);
    } else
	set_runaspw(runas_user ? runas_user : def_runas_default);

    if (!update_defaults(SETDEF_RUNAS))
	log_error(NO_STDERR, _("problem with defaults entries"));

    if (def_fqdn)
	set_fqdn();	/* deferred until after sudoers is parsed */

    /* Set login class if applicable. */
    set_loginclass(runas_pw ? runas_pw : sudo_user.pw);

    restore_perms();

    debug_return_bool(true);
}

static void
sudoers_policy_close(int exit_status, int error_code)
{
    debug_decl(sudoers_policy_close, SUDO_DEBUG_PLUGIN)

    if (sigsetjmp(error_jmp, 1)) {
	/* called via error(), errorx() or log_fatal() */
	debug_return;
    }

    /* We do not currently log the exit status. */
    if (error_code)
	warningx(_("unable to execute %s: %s"), safe_cmnd, strerror(error_code));

    /* Close the session we opened in sudoers_policy_init_session(). */
    if (ISSET(sudo_mode, MODE_RUN|MODE_EDIT))
	(void)sudo_auth_end_session(runas_pw);

    /* Free remaining references to password and group entries. */
    pw_delref(sudo_user.pw);
    pw_delref(runas_pw);
    if (runas_gr != NULL)
	gr_delref(runas_gr);
    if (user_group_list != NULL)
	grlist_delref(user_group_list);

    debug_return;
}

/*
 * The init_session function is called before executing the command
 * and before uid/gid changes occur.
 */
static int
sudoers_policy_init_session(struct passwd *pwd, char **user_env[])
{
    debug_decl(sudoers_policy_init, SUDO_DEBUG_PLUGIN)

    /* user_env is only specified for API version 1.2 and higher. */
    if (sudo_version < SUDO_API_MKVERSION(1, 2))
	user_env = NULL;

    if (sigsetjmp(error_jmp, 1)) {
	/* called via error(), errorx() or log_fatal() */
	return -1;
    }

    debug_return_bool(sudo_auth_begin_session(pwd, user_env));
}

static int
sudoers_policy_main(int argc, char * const argv[], int pwflag, char *env_add[],
    char **command_infop[], char **argv_out[], char **user_env_out[])
{
    static char *command_info[32]; /* XXX */
    char **edit_argv = NULL;
    struct sudo_nss *nss;
    int cmnd_status = -1, validated;
    volatile int info_len = 0;
    volatile int rval = true;
    debug_decl(sudoers_policy_main, SUDO_DEBUG_PLUGIN)

    if (sigsetjmp(error_jmp, 1)) {
	/* error recovery via error(), errorx() or log_fatal() */
	rval = -1;
	goto done;
    }

    /* Is root even allowed to run sudo? */
    if (user_uid == 0 && !def_root_sudo) {
        warningx(_("sudoers specifies that root is not allowed to sudo"));
        goto bad;
    }    

    /* Check for -C overriding def_closefrom. */
    if (user_closefrom >= 0 && user_closefrom != def_closefrom) {
	if (!def_closefrom_override) {
	    warningx(_("you are not permitted to use the -C option"));
	    goto bad;
	}
	def_closefrom = user_closefrom;
    }

    set_perms(PERM_INITIAL);

    /* Environment variables specified on the command line. */
    if (env_add != NULL && env_add[0] != NULL)
	sudo_user.env_vars = env_add;

    /*
     * Make a local copy of argc/argv, with special handling
     * for pseudo-commands and the '-i' option.
     */
    if (argc == 0) {
	NewArgc = 1;
	NewArgv = emalloc2(NewArgc + 1, sizeof(char *));
	NewArgv[0] = user_cmnd;
	NewArgv[1] = NULL;
    } else {
	/* Must leave an extra slot before NewArgv for bash's --login */
	NewArgc = argc;
	NewArgv = emalloc2(NewArgc + 2, sizeof(char *));
	memcpy(++NewArgv, argv, argc * sizeof(char *));
	NewArgv[NewArgc] = NULL;
	if (ISSET(sudo_mode, MODE_LOGIN_SHELL) && runas_pw != NULL)
	    NewArgv[0] = estrdup(runas_pw->pw_shell);
    }

    /* If given the -P option, set the "preserve_groups" flag. */
    if (ISSET(sudo_mode, MODE_PRESERVE_GROUPS))
	def_preserve_groups = true;

    /* Find command in path */
    cmnd_status = set_cmnd();
    if (cmnd_status == -1) {
	rval = -1;
	goto done;
    }

#ifdef HAVE_SETLOCALE
    if (!setlocale(LC_ALL, def_sudoers_locale)) {
	warningx(_("unable to set locale to \"%s\", using \"C\""),
	    def_sudoers_locale);
	setlocale(LC_ALL, "C");
    }
#endif

    /*
     * Check sudoers sources.
     */
    validated = FLAG_NO_USER | FLAG_NO_HOST;
    tq_foreach_fwd(snl, nss) {
	validated = nss->lookup(nss, validated, pwflag);

	if (ISSET(validated, VALIDATE_OK)) {
	    /* Handle "= auth" in netsvc.conf */
	    if (nss->ret_if_found)
		break;
	} else {
	    /* Handle [NOTFOUND=return] */
	    if (nss->ret_if_notfound)
		break;
	}
    }

    if (safe_cmnd == NULL)
	safe_cmnd = estrdup(user_cmnd);

#ifdef HAVE_SETLOCALE
    setlocale(LC_ALL, "");
#endif

    /* If only a group was specified, set runas_pw based on invoking user. */
    if (runas_pw == NULL)
	set_runaspw(user_name);

    /*
     * Look up the timestamp dir owner if one is specified.
     */
    if (def_timestampowner) {
	struct passwd *pw;

	if (*def_timestampowner == '#')
	    pw = sudo_getpwuid(atoi(def_timestampowner + 1));
	else
	    pw = sudo_getpwnam(def_timestampowner);
	if (pw != NULL) {
	    timestamp_uid = pw->pw_uid;
	    pw_delref(pw);
	} else {
	    log_error(0, _("timestamp owner (%s): No such user"),
		def_timestampowner);
	    timestamp_uid = ROOT_UID;
	}
    }

    /* If no command line args and "shell_noargs" is not set, error out. */
    if (ISSET(sudo_mode, MODE_IMPLIED_SHELL) && !def_shell_noargs) {
	rval = -2; /* usage error */
	goto done;
    }

    /* Bail if a tty is required and we don't have one.  */
    if (def_requiretty) {
	int fd = open(_PATH_TTY, O_RDWR|O_NOCTTY);
	if (fd == -1) {
	    audit_failure(NewArgv, _("no tty"));
	    warningx(_("sorry, you must have a tty to run sudo"));
	    goto bad;
	} else
	    (void) close(fd);
    }

    /*
     * We don't reset the environment for sudoedit or if the user
     * specified the -E command line flag and they have setenv privs.
     */
    if (ISSET(sudo_mode, MODE_EDIT) ||
	(ISSET(sudo_mode, MODE_PRESERVE_ENV) && def_setenv))
	def_env_reset = false;

    /* Build a new environment that avoids any nasty bits. */
    rebuild_env();

    /* Require a password if sudoers says so.  */
    rval = check_user(validated, sudo_mode);
    if (rval != true)
	goto done;

    /* If run as root with SUDO_USER set, set sudo_user.pw to that user. */
    /* XXX - causes confusion when root is not listed in sudoers */
    if (sudo_mode & (MODE_RUN | MODE_EDIT) && prev_user != NULL) {
	if (user_uid == 0 && strcmp(prev_user, "root") != 0) {
	    struct passwd *pw;

	    if ((pw = sudo_getpwnam(prev_user)) != NULL) {
		    if (sudo_user.pw != NULL)
			pw_delref(sudo_user.pw);
		    sudo_user.pw = pw;
	    }
	}
    }

    /* If the user was not allowed to run the command we are done. */
    if (!ISSET(validated, VALIDATE_OK)) {
	if (ISSET(validated, FLAG_NO_USER | FLAG_NO_HOST)) {
	    audit_failure(NewArgv, _("No user or host"));
	    log_denial(validated, 1);
	} else {
	    if (def_path_info) {
		/*
		 * We'd like to not leak path info at all here, but that can
		 * *really* confuse the users.  To really close the leak we'd
		 * have to say "not allowed to run foo" even when the problem
		 * is just "no foo in path" since the user can trivially set
		 * their path to just contain a single dir.
		 */
		log_denial(validated,
		    !(cmnd_status == NOT_FOUND_DOT || cmnd_status == NOT_FOUND));
		if (cmnd_status == NOT_FOUND)
		    warningx(_("%s: command not found"), user_cmnd);
		else if (cmnd_status == NOT_FOUND_DOT)
		    warningx(_("ignoring `%s' found in '.'\nUse `sudo ./%s' if this is the `%s' you wish to run."), user_cmnd, user_cmnd, user_cmnd);
	    } else {
		/* Just tell the user they are not allowed to run foo. */
		log_denial(validated, 1);
	    }
	    audit_failure(NewArgv, _("validation failure"));
	}
	goto bad;
    }

    /* Create Ubuntu-style dot file to indicate sudo was successful. */
    create_admin_success_flag();

    /* Finally tell the user if the command did not exist. */
    if (cmnd_status == NOT_FOUND_DOT) {
	audit_failure(NewArgv, _("command in current directory"));
	warningx(_("ignoring `%s' found in '.'\nUse `sudo ./%s' if this is the `%s' you wish to run."), user_cmnd, user_cmnd, user_cmnd);
	goto bad;
    } else if (cmnd_status == NOT_FOUND) {
	audit_failure(NewArgv, _("%s: command not found"), user_cmnd);
	warningx(_("%s: command not found"), user_cmnd);
	goto bad;
    }

    /* If user specified env vars make sure sudoers allows it. */
    if (ISSET(sudo_mode, MODE_RUN) && !def_setenv) {
	if (ISSET(sudo_mode, MODE_PRESERVE_ENV)) {
	    warningx(_("sorry, you are not allowed to preserve the environment"));
	    goto bad;
	} else
	    validate_env_vars(sudo_user.env_vars);
    }

    if (ISSET(sudo_mode, (MODE_RUN | MODE_EDIT)) && (def_log_input || def_log_output)) {
	if (def_iolog_file && def_iolog_dir) {
	    command_info[info_len++] = expand_iolog_path("iolog_path=",
		def_iolog_dir, def_iolog_file, &sudo_user.iolog_file);
	    sudo_user.iolog_file++;
	}
	if (def_log_input) {
	    command_info[info_len++] = estrdup("iolog_stdin=true");
	    command_info[info_len++] = estrdup("iolog_ttyin=true");
	}
	if (def_log_output) {
	    command_info[info_len++] = estrdup("iolog_stdout=true");
	    command_info[info_len++] = estrdup("iolog_stderr=true");
	    command_info[info_len++] = estrdup("iolog_ttyout=true");
	}
	if (def_compress_io)
	    command_info[info_len++] = estrdup("iolog_compress=true");
    }

    log_allowed(validated);
    if (ISSET(sudo_mode, MODE_CHECK))
	rval = display_cmnd(snl, list_pw ? list_pw : sudo_user.pw);
    else if (ISSET(sudo_mode, MODE_LIST))
	display_privs(snl, list_pw ? list_pw : sudo_user.pw); /* XXX - return val */

    /* Cleanup sudoers sources */
    tq_foreach_fwd(snl, nss) {
	nss->close(nss);
    }
    if (def_group_plugin)
	group_plugin_unload();

    if (ISSET(sudo_mode, (MODE_VALIDATE|MODE_CHECK|MODE_LIST))) {
	/* rval already set appropriately */
	goto done;
    }

    /*
     * Set umask based on sudoers.
     * If user's umask is more restrictive, OR in those bits too
     * unless umask_override is set.
     */
    if (def_umask != 0777) {
	mode_t mask = def_umask;
	if (!def_umask_override) {
	    mode_t omask = umask(mask);
	    mask |= omask;
	    umask(omask);
	}
	easprintf(&command_info[info_len++], "umask=0%o", (unsigned int)mask);
    }

    if (ISSET(sudo_mode, MODE_LOGIN_SHELL)) {
	char *p;

	/* Convert /bin/sh -> -sh so shell knows it is a login shell */
	if ((p = strrchr(NewArgv[0], '/')) == NULL)
	    p = NewArgv[0];
	*p = '-';
	NewArgv[0] = p;

	/* Set cwd to run user's homedir. */
	command_info[info_len++] = fmt_string("cwd", runas_pw->pw_dir);

	/*
	 * Newer versions of bash require the --login option to be used
	 * in conjunction with the -c option even if the shell name starts
	 * with a '-'.  Unfortunately, bash 1.x uses -login, not --login
	 * so this will cause an error for that.
	 */
	if (NewArgc > 1 && strcmp(NewArgv[0], "-bash") == 0 &&
	    strcmp(NewArgv[1], "-c") == 0) {
	    /* Use the extra slot before NewArgv so we can store --login. */
	    NewArgv--;
	    NewArgc++;
	    NewArgv[0] = NewArgv[1];
	    NewArgv[1] = "--login";
	}

#if defined(_AIX) || (defined(__linux__) && !defined(HAVE_PAM))
	/* Insert system-wide environment variables. */
	read_env_file(_PATH_ENVIRONMENT, true);
#endif
#ifdef HAVE_LOGIN_CAP_H
	/* Set environment based on login class. */
	if (login_class) {
	    login_cap_t *lc = login_getclass(login_class);
	    if (lc != NULL) {
		setusercontext(lc, runas_pw, runas_pw->pw_uid, LOGIN_SETPATH|LOGIN_SETENV);
		login_close(lc);
	    }
	}
#endif /* HAVE_LOGIN_CAP_H */
    }

    /* Insert system-wide environment variables. */
    if (def_env_file)
	read_env_file(def_env_file, false);

    /* Insert user-specified environment variables. */
    insert_env_vars(sudo_user.env_vars);

    /* Restore signal handlers before we exec. */
    (void) sigaction(SIGINT, &saved_sa_int, NULL);
    (void) sigaction(SIGQUIT, &saved_sa_quit, NULL);
    (void) sigaction(SIGTSTP, &saved_sa_tstp, NULL);

    if (ISSET(sudo_mode, MODE_EDIT)) {
	char *editor = find_editor(NewArgc - 1, NewArgv + 1, &edit_argv);
	if (editor == NULL)
	    goto bad;
	command_info[info_len++] = fmt_string("command", editor);
	command_info[info_len++] = estrdup("sudoedit=true");
    } else {
	command_info[info_len++] = fmt_string("command", safe_cmnd);
    }
    if (def_stay_setuid) {
	easprintf(&command_info[info_len++], "runas_uid=%u",
	    (unsigned int)user_uid);
	easprintf(&command_info[info_len++], "runas_gid=%u",
	    (unsigned int)user_gid);
	easprintf(&command_info[info_len++], "runas_euid=%u",
	    (unsigned int)runas_pw->pw_uid);
	easprintf(&command_info[info_len++], "runas_egid=%u",
	    runas_gr ? (unsigned int)runas_gr->gr_gid :
	    (unsigned int)runas_pw->pw_gid);
    } else {
	easprintf(&command_info[info_len++], "runas_uid=%u",
	    (unsigned int)runas_pw->pw_uid);
	easprintf(&command_info[info_len++], "runas_gid=%u",
	    runas_gr ? (unsigned int)runas_gr->gr_gid :
	    (unsigned int)runas_pw->pw_gid);
    }
    if (def_preserve_groups) {
	command_info[info_len++] = "preserve_groups=true";
    } else {
	int i, len;
	gid_t egid;
	size_t glsize;
	char *cp, *gid_list;
	struct group_list *grlist = get_group_list(runas_pw);

	/* We reserve an extra spot in the list for the effective gid. */
	glsize = sizeof("runas_groups=") - 1 +
	    ((grlist->ngids + 1) * (MAX_UID_T_LEN + 1));
	gid_list = emalloc(glsize);
	memcpy(gid_list, "runas_groups=", sizeof("runas_groups=") - 1);
	cp = gid_list + sizeof("runas_groups=") - 1;

	/* On BSD systems the effective gid is the first group in the list. */
	egid = runas_gr ? (unsigned int)runas_gr->gr_gid :
	    (unsigned int)runas_pw->pw_gid;
	len = snprintf(cp, glsize - (cp - gid_list), "%u", egid);
	if (len < 0 || len >= glsize - (cp - gid_list))
	    errorx(1, _("internal error, runas_groups overflow"));
	cp += len;
	for (i = 0; i < grlist->ngids; i++) {
	    if (grlist->gids[i] != egid) {
		len = snprintf(cp, glsize - (cp - gid_list), ",%u",
		     (unsigned int) grlist->gids[i]);
		if (len < 0 || len >= glsize - (cp - gid_list))
		    errorx(1, _("internal error, runas_groups overflow"));
		cp += len;
	    }
	}
	command_info[info_len++] = gid_list;
	grlist_delref(grlist);
    }
    if (def_closefrom >= 0)
	easprintf(&command_info[info_len++], "closefrom=%d", def_closefrom);
    if (def_noexec)
	command_info[info_len++] = estrdup("noexec=true");
    if (def_set_utmp)
	command_info[info_len++] = estrdup("set_utmp=true");
    if (def_use_pty)
	command_info[info_len++] = estrdup("use_pty=true");
    if (def_utmp_runas)
	command_info[info_len++] = fmt_string("utmp_user", runas_pw->pw_name);
#ifdef HAVE_LOGIN_CAP_H
    if (def_use_loginclass)
	command_info[info_len++] = fmt_string("login_class", login_class);
#endif /* HAVE_LOGIN_CAP_H */
#ifdef HAVE_SELINUX
    if (user_role != NULL)
	command_info[info_len++] = fmt_string("selinux_role", user_role);
    if (user_type != NULL)
	command_info[info_len++] = fmt_string("selinux_type", user_type);
#endif /* HAVE_SELINUX */

    /* Must audit before uid change. */
    audit_success(NewArgv);

    *command_infop = command_info;

    *argv_out = edit_argv ? edit_argv : NewArgv;

    /* Get private version of the environment and zero out stashed copy. */
    *user_env_out = env_get();
    env_init(NULL);

    goto done;

bad:
    rval = false;

done:
    rewind_perms();

    /* Close the password and group files and free up memory. */
    sudo_endpwent();
    sudo_endgrent();

    debug_return_bool(rval);
}

static int
sudoers_policy_check(int argc, char * const argv[], char *env_add[],
    char **command_infop[], char **argv_out[], char **user_env_out[])
{
    debug_decl(sudoers_policy_check, SUDO_DEBUG_PLUGIN)

    if (!ISSET(sudo_mode, MODE_EDIT))
	SET(sudo_mode, MODE_RUN);

    debug_return_bool(sudoers_policy_main(argc, argv, 0, env_add, command_infop,
	argv_out, user_env_out));
}

static int
sudoers_policy_validate(void)
{
    debug_decl(sudoers_policy_validate, SUDO_DEBUG_PLUGIN)

    user_cmnd = "validate";
    SET(sudo_mode, MODE_VALIDATE);

    debug_return_bool(sudoers_policy_main(0, NULL, I_VERIFYPW, NULL, NULL, NULL, NULL));
}

static void
sudoers_policy_invalidate(int remove)
{
    debug_decl(sudoers_policy_invalidate, SUDO_DEBUG_PLUGIN)

    user_cmnd = "kill";
    if (sigsetjmp(error_jmp, 1) == 0) {
	remove_timestamp(remove);
	plugin_cleanup(0);
    }

    debug_return;
}

static int
sudoers_policy_list(int argc, char * const argv[], int verbose,
    const char *list_user)
{
    int rval;
    debug_decl(sudoers_policy_list, SUDO_DEBUG_PLUGIN)

    user_cmnd = "list";
    if (argc)
	SET(sudo_mode, MODE_CHECK);
    else
	SET(sudo_mode, MODE_LIST);
    if (verbose)
	long_list = 1;
    if (list_user) {
	list_pw = sudo_getpwnam(list_user);
	if (list_pw == NULL) {
	    warningx(_("unknown user: %s"), list_user);
	    debug_return_bool(-1);
	}
    }
    rval = sudoers_policy_main(argc, argv, I_LISTPW, NULL, NULL, NULL, NULL);
    if (list_user) {
	pw_delref(list_pw);
	list_pw = NULL;
    }

    debug_return_bool(rval);
}

/*
 * Initialize timezone, set umask, fill in ``sudo_user'' struct and
 * load the ``interfaces'' array.
 */
static void
init_vars(char * const envp[])
{
    char * const * ep;
    debug_decl(init_vars, SUDO_DEBUG_PLUGIN)

#ifdef HAVE_TZSET
    (void) tzset();		/* set the timezone if applicable */
#endif /* HAVE_TZSET */

    for (ep = envp; *ep; ep++) {
	/* XXX - don't fill in if empty string */
	switch (**ep) {
	    case 'K':
		if (strncmp("KRB5CCNAME=", *ep, 11) == 0)
		    user_ccname = *ep + 11;
		break;
	    case 'P':
		if (strncmp("PATH=", *ep, 5) == 0)
		    user_path = *ep + 5;
		break;
	    case 'S':
		if (!user_prompt && strncmp("SUDO_PROMPT=", *ep, 12) == 0)
		    user_prompt = *ep + 12;
		else if (strncmp("SUDO_USER=", *ep, 10) == 0)
		    prev_user = *ep + 10;
		break;
	    }
    }

    /*
     * Get a local copy of the user's struct passwd with the shadow password
     * if necessary.  It is assumed that euid is 0 at this point so we
     * can read the shadow passwd file if necessary.
     */
    if ((sudo_user.pw = sudo_getpwuid(user_uid)) == NULL) {
	/*
	 * It is not unusual for users to place "sudo -k" in a .logout
	 * file which can cause sudo to be run during reboot after the
	 * YP/NIS/NIS+/LDAP/etc daemon has died.
	 */
	if (sudo_mode == MODE_KILL || sudo_mode == MODE_INVALIDATE)
	    errorx(1, _("unknown uid: %u"), (unsigned int) user_uid);

	/* Need to make a fake struct passwd for the call to log_fatal(). */
	sudo_user.pw = sudo_fakepwnamid(user_name, user_uid, user_gid);
	log_fatal(0, _("unknown uid: %u"), (unsigned int) user_uid);
	/* NOTREACHED */
    }

    /*
     * Get group list.
     */
    if (user_group_list == NULL)
	user_group_list = get_group_list(sudo_user.pw);

    /* Set runas callback. */
    sudo_defs_table[I_RUNAS_DEFAULT].callback = cb_runas_default;

    /* It is now safe to use log_fatal() and set_perms() */
    debug_return;
}

/*
 * Fill in user_cmnd, user_args, user_base and user_stat variables
 * and apply any command-specific defaults entries.
 */
static int
set_cmnd(void)
{
    int rval;
    char *path = user_path;
    debug_decl(set_cmnd, SUDO_DEBUG_PLUGIN)

    /* Resolve the path and return. */
    rval = FOUND;
    user_stat = ecalloc(1, sizeof(struct stat));

    /* Default value for cmnd, overridden below. */
    if (user_cmnd == NULL)
	user_cmnd = NewArgv[0];

    if (sudo_mode & (MODE_RUN | MODE_EDIT | MODE_CHECK)) {
	if (ISSET(sudo_mode, MODE_RUN | MODE_CHECK)) {
	    if (def_secure_path && !user_is_exempt())
		path = def_secure_path;
	    set_perms(PERM_RUNAS);
	    rval = find_path(NewArgv[0], &user_cmnd, user_stat, path,
		def_ignore_dot);
	    restore_perms();
	    if (rval != FOUND) {
		/* Failed as root, try as invoking user. */
		set_perms(PERM_USER);
		rval = find_path(NewArgv[0], &user_cmnd, user_stat, path,
		    def_ignore_dot);
		restore_perms();
	    }
	}

	/* set user_args */
	if (NewArgc > 1) {
	    char *to, *from, **av;
	    size_t size, n;

	    /* Alloc and build up user_args. */
	    for (size = 0, av = NewArgv + 1; *av; av++)
		size += strlen(*av) + 1;
	    user_args = emalloc(size);
	    if (ISSET(sudo_mode, MODE_SHELL|MODE_LOGIN_SHELL)) {
		/*
		 * When running a command via a shell, the sudo front-end
		 * escapes potential meta chars.  We unescape non-spaces
		 * for sudoers matching and logging purposes.
		 */
		for (to = user_args, av = NewArgv + 1; (from = *av); av++) {
		    while (*from) {
			if (from[0] == '\\' && !isspace((unsigned char)from[1]))
			    from++;
			*to++ = *from++;
		    }
		    *to++ = ' ';
		}
		*--to = '\0';
	    } else {
		for (to = user_args, av = NewArgv + 1; *av; av++) {
		    n = strlcpy(to, *av, size - (to - user_args));
		    if (n >= size - (to - user_args))
			errorx(1, _("internal error, set_cmnd() overflow"));
		    to += n;
		    *to++ = ' ';
		}
		*--to = '\0';
	    }
	}
    }
    if (strlen(user_cmnd) >= PATH_MAX)
	errorx(1, _("%s: %s"), user_cmnd, strerror(ENAMETOOLONG));

    if ((user_base = strrchr(user_cmnd, '/')) != NULL)
	user_base++;
    else
	user_base = user_cmnd;

    if (!update_defaults(SETDEF_CMND))
	log_error(NO_STDERR, _("problem with defaults entries"));

    debug_return_int(rval);
}

/*
 * Open sudoers and sanity check mode/owner/type.
 * Returns a handle to the sudoers file or NULL on error.
 */
FILE *
open_sudoers(const char *sudoers, bool doedit, bool *keepopen)
{
    struct stat sb;
    FILE *fp = NULL;
    debug_decl(open_sudoers, SUDO_DEBUG_PLUGIN)

    set_perms(PERM_SUDOERS);

    switch (sudo_secure_file(sudoers, sudoers_uid, sudoers_gid, &sb)) {
	case SUDO_PATH_SECURE:
	    /*
	     * If we are expecting sudoers to be group readable but
	     * it is not, we must open the file as root, not uid 1.
	     */
	    if (sudoers_uid == ROOT_UID && (sudoers_mode & S_IRGRP)) {
		if ((sb.st_mode & S_IRGRP) == 0) {
		    restore_perms();
		    set_perms(PERM_ROOT);
		}
	    }
	    /*
	     * Open sudoers and make sure we can read it so we can present
	     * the user with a reasonable error message (unlike the lexer).
	     */
	    if ((fp = fopen(sudoers, "r")) == NULL) {
		log_error(USE_ERRNO, _("unable to open %s"), sudoers);
	    } else {
		if (sb.st_size != 0 && fgetc(fp) == EOF) {
		    log_error(USE_ERRNO, _("unable to read %s"),
			sudoers);
		    fclose(fp);
		    fp = NULL;
		} else {
		    /* Rewind fp and set close on exec flag. */
		    rewind(fp);
		    (void) fcntl(fileno(fp), F_SETFD, 1);
		}
	    }
	    break;
	case SUDO_PATH_MISSING:
	    log_error(USE_ERRNO, _("unable to stat %s"), sudoers);
	    break;
	case SUDO_PATH_BAD_TYPE:
	    log_error(0, _("%s is not a regular file"), sudoers);
	    break;
	case SUDO_PATH_WRONG_OWNER:
	    log_error(0, _("%s is owned by uid %u, should be %u"),
		sudoers, (unsigned int) sb.st_uid, (unsigned int) sudoers_uid);
	    break;
	case SUDO_PATH_WORLD_WRITABLE:
	    log_error(0, _("%s is world writable"), sudoers);
	    break;
	case SUDO_PATH_GROUP_WRITABLE:
	    log_error(0, _("%s is owned by gid %u, should be %u"),
		sudoers, (unsigned int) sb.st_gid, (unsigned int) sudoers_gid);
	    break;
	default:
	    /* NOTREACHED */
	    break;
    }

    restore_perms();		/* change back to root */

    debug_return_ptr(fp);
}

#ifdef HAVE_LOGIN_CAP_H
static void
set_loginclass(struct passwd *pw)
{
    const int errflags = NO_MAIL|MSG_ONLY;
    login_cap_t *lc;
    debug_decl(set_loginclass, SUDO_DEBUG_PLUGIN)

    if (!def_use_loginclass)
	debug_return;

    if (login_class && strcmp(login_class, "-") != 0) {
	if (user_uid != 0 &&
	    strcmp(runas_user ? runas_user : def_runas_default, "root") != 0)
	    errorx(1, _("only root can use `-c %s'"), login_class);
    } else {
	login_class = pw->pw_class;
	if (!login_class || !*login_class)
	    login_class =
		(pw->pw_uid == 0) ? LOGIN_DEFROOTCLASS : LOGIN_DEFCLASS;
    }

    /* Make sure specified login class is valid. */
    lc = login_getclass(login_class);
    if (!lc || !lc->lc_class || strcmp(lc->lc_class, login_class) != 0) {
	/*
	 * Don't make it a fatal error if the user didn't specify the login
	 * class themselves.  We do this because if login.conf gets
	 * corrupted we want the admin to be able to use sudo to fix it.
	 */
	if (login_class)
	    log_fatal(errflags, _("unknown login class: %s"), login_class);
	else
	    log_error(errflags, _("unknown login class: %s"), login_class);
	def_use_loginclass = false;
    }
    login_close(lc);
    debug_return;
}
#else
static void
set_loginclass(struct passwd *pw)
{
}
#endif /* HAVE_LOGIN_CAP_H */

/*
 * Look up the fully qualified domain name and set user_host and user_shost.
 */
void
set_fqdn(void)
{
    struct addrinfo *res0, hint;
    char *p;
    debug_decl(set_fqdn, SUDO_DEBUG_PLUGIN)

    zero_bytes(&hint, sizeof(hint));
    hint.ai_family = PF_UNSPEC;
    hint.ai_flags = AI_CANONNAME;
    if (getaddrinfo(user_host, NULL, &hint, &res0) != 0) {
	log_error(MSG_ONLY, _("unable to resolve host %s"), user_host);
    } else {
	if (user_shost != user_host)
	    efree(user_shost);
	efree(user_host);
	user_host = estrdup(res0->ai_canonname);
	freeaddrinfo(res0);
    }
    if ((p = strchr(user_host, '.')) != NULL)
	user_shost = estrndup(user_host, (size_t)(p - user_host));
    else
	user_shost = user_host;
    debug_return;
}

/*
 * Get passwd entry for the user we are going to run commands as
 * and store it in runas_pw.  By default, commands run as "root".
 */
static void
set_runaspw(const char *user)
{
    debug_decl(set_runaspw, SUDO_DEBUG_PLUGIN)

    if (runas_pw != NULL)
	pw_delref(runas_pw);
    if (*user == '#') {
	if ((runas_pw = sudo_getpwuid(atoi(user + 1))) == NULL)
	    runas_pw = sudo_fakepwnam(user, runas_gr ? runas_gr->gr_gid : 0);
    } else {
	if ((runas_pw = sudo_getpwnam(user)) == NULL)
	    log_fatal(NO_MAIL|MSG_ONLY, _("unknown user: %s"), user);
    }
    debug_return;
}

/*
 * Get group entry for the group we are going to run commands as
 * and store it in runas_gr.
 */
static void
set_runasgr(const char *group)
{
    debug_decl(set_runasgr, SUDO_DEBUG_PLUGIN)

    if (runas_gr != NULL)
	gr_delref(runas_gr);
    if (*group == '#') {
	if ((runas_gr = sudo_getgrgid(atoi(group + 1))) == NULL)
	    runas_gr = sudo_fakegrnam(group);
    } else {
	if ((runas_gr = sudo_getgrnam(group)) == NULL)
	    log_fatal(NO_MAIL|MSG_ONLY, _("unknown group: %s"), group);
    }
    debug_return;
}

/*
 * Callback for runas_default sudoers setting.
 */
static int
cb_runas_default(const char *user)
{
    /* Only reset runaspw if user didn't specify one. */
    if (!runas_user && !runas_group)
	set_runaspw(user);
    return true;
}

/*
 * Cleanup hook for error()/errorx()
 */
void
plugin_cleanup(int gotsignal)
{
    struct sudo_nss *nss;

    if (!gotsignal) {
	debug_decl(plugin_cleanup, SUDO_DEBUG_PLUGIN)
	if (snl != NULL) {
	    tq_foreach_fwd(snl, nss)
		nss->close(nss);
	}
	if (def_group_plugin)
	    group_plugin_unload();
	sudo_endpwent();
	sudo_endgrent();
	debug_return;
    }
}

static int
sudoers_policy_version(int verbose)
{
    debug_decl(sudoers_policy_version, SUDO_DEBUG_PLUGIN)

    if (sigsetjmp(error_jmp, 1)) {
	/* error recovery via error(), errorx() or log_fatal() */
	debug_return_bool(-1);
    }

    sudo_printf(SUDO_CONV_INFO_MSG, _("Sudoers policy plugin version %s\n"),
	PACKAGE_VERSION);
    sudo_printf(SUDO_CONV_INFO_MSG, _("Sudoers file grammar version %d\n"),
	SUDOERS_GRAMMAR_VERSION);

    if (verbose) {
	sudo_printf(SUDO_CONV_INFO_MSG, _("\nSudoers path: %s\n"), sudoers_file);
#ifdef HAVE_LDAP
# ifdef _PATH_NSSWITCH_CONF
	sudo_printf(SUDO_CONV_INFO_MSG, _("nsswitch path: %s\n"), _PATH_NSSWITCH_CONF);
# endif
	sudo_printf(SUDO_CONV_INFO_MSG, _("ldap.conf path: %s\n"), _PATH_LDAP_CONF);
	sudo_printf(SUDO_CONV_INFO_MSG, _("ldap.secret path: %s\n"), _PATH_LDAP_SECRET);
#endif
	dump_auth_methods();
	dump_defaults();
	sudo_printf(SUDO_CONV_INFO_MSG, "\n");
	if (interfaces_string != NULL) {
	    dump_interfaces(interfaces_string);
	    sudo_printf(SUDO_CONV_INFO_MSG, "\n");
	}
    }
    debug_return_bool(true);
}

static int
deserialize_info(char * const args[], char * const settings[], char * const user_info[])
{
    char * const *cur;
    const char *p, *groups = NULL;
    const char *debug_flags = NULL;
    int flags = 0;
    debug_decl(deserialize_info, SUDO_DEBUG_PLUGIN)

#define MATCHES(s, v) (strncmp(s, v, sizeof(v) - 1) == 0)

    /* Parse sudo.conf plugin args. */
    if (args != NULL) {
	for (cur = args; *cur != NULL; cur++) {
	    if (MATCHES(*cur, "sudoers_file=")) {
		sudoers_file = *cur + sizeof("sudoers_file=") - 1;
		continue;
	    }
	    if (MATCHES(*cur, "sudoers_uid=")) {
		sudoers_uid = (uid_t) atoi(*cur + sizeof("sudoers_uid=") - 1);
		continue;
	    }
	    if (MATCHES(*cur, "sudoers_gid=")) {
		sudoers_gid = (gid_t) atoi(*cur + sizeof("sudoers_gid=") - 1);
		continue;
	    }
	    if (MATCHES(*cur, "sudoers_mode=")) {
		sudoers_mode = (mode_t) strtol(*cur + sizeof("sudoers_mode=") - 1,
		    NULL, 8);
		continue;
	    }
	}
    }

    /* Parse command line settings. */
    user_closefrom = -1;
    for (cur = settings; *cur != NULL; cur++) {
	if (MATCHES(*cur, "closefrom=")) {
	    user_closefrom = atoi(*cur + sizeof("closefrom=") - 1);
	    continue;
	}
	if (MATCHES(*cur, "debug_flags=")) {
	    debug_flags = *cur + sizeof("debug_flags=") - 1;
	    continue;
	}
	if (MATCHES(*cur, "runas_user=")) {
	    runas_user = *cur + sizeof("runas_user=") - 1;
	    continue;
	}
	if (MATCHES(*cur, "runas_group=")) {
	    runas_group = *cur + sizeof("runas_group=") - 1;
	    continue;
	}
	if (MATCHES(*cur, "prompt=")) {
	    user_prompt = *cur + sizeof("prompt=") - 1;
	    def_passprompt_override = true;
	    continue;
	}
	if (MATCHES(*cur, "set_home=")) {
	    if (atobool(*cur + sizeof("set_home=") - 1) == true)
		SET(flags, MODE_RESET_HOME);
	    continue;
	}
	if (MATCHES(*cur, "preserve_environment=")) {
	    if (atobool(*cur + sizeof("preserve_environment=") - 1) == true)
		SET(flags, MODE_PRESERVE_ENV);
	    continue;
	}
	if (MATCHES(*cur, "run_shell=")) {
	    if (atobool(*cur + sizeof("run_shell=") - 1) == true)
		SET(flags, MODE_SHELL);
	    continue;
	}
	if (MATCHES(*cur, "login_shell=")) {
	    if (atobool(*cur + sizeof("login_shell=") - 1) == true) {
		SET(flags, MODE_LOGIN_SHELL);
		def_env_reset = true;
	    }
	    continue;
	}
	if (MATCHES(*cur, "implied_shell=")) {
	    if (atobool(*cur + sizeof("implied_shell=") - 1) == true)
		SET(flags, MODE_IMPLIED_SHELL);
	    continue;
	}
	if (MATCHES(*cur, "preserve_groups=")) {
	    if (atobool(*cur + sizeof("preserve_groups=") - 1) == true)
		SET(flags, MODE_PRESERVE_GROUPS);
	    continue;
	}
	if (MATCHES(*cur, "ignore_ticket=")) {
	    if (atobool(*cur + sizeof("ignore_ticket=") - 1) == true)
		SET(flags, MODE_IGNORE_TICKET);
	    continue;
	}
	if (MATCHES(*cur, "noninteractive=")) {
	    if (atobool(*cur + sizeof("noninteractive=") - 1) == true)
		SET(flags, MODE_NONINTERACTIVE);
	    continue;
	}
	if (MATCHES(*cur, "sudoedit=")) {
	    if (atobool(*cur + sizeof("sudoedit=") - 1) == true)
		SET(flags, MODE_EDIT);
	    continue;
	}
	if (MATCHES(*cur, "login_class=")) {
	    login_class = *cur + sizeof("login_class=") - 1;
	    def_use_loginclass = true;
	    continue;
	}
#ifdef HAVE_SELINUX
	if (MATCHES(*cur, "selinux_role=")) {
	    user_role = *cur + sizeof("selinux_role=") - 1;
	    continue;
	}
	if (MATCHES(*cur, "selinux_type=")) {
	    user_type = *cur + sizeof("selinux_type=") - 1;
	    continue;
	}
#endif /* HAVE_SELINUX */
#ifdef HAVE_BSD_AUTH_H
	if (MATCHES(*cur, "bsdauth_type=")) {
	    login_style = *cur + sizeof("bsdauth_type=") - 1;
	    continue;
	}
#endif /* HAVE_BSD_AUTH_H */
#if !defined(HAVE_GETPROGNAME) && !defined(HAVE___PROGNAME)
	if (MATCHES(*cur, "progname=")) {
	    setprogname(*cur + sizeof("progname=") - 1);
	    continue;
	}
#endif
	if (MATCHES(*cur, "network_addrs=")) {
	    interfaces_string = *cur + sizeof("network_addrs=") - 1;
	    set_interfaces(interfaces_string);
	    continue;
	}
    }

    for (cur = user_info; *cur != NULL; cur++) {
	if (MATCHES(*cur, "user=")) {
	    user_name = estrdup(*cur + sizeof("user=") - 1);
	    continue;
	}
	if (MATCHES(*cur, "uid=")) {
	    user_uid = (uid_t) atoi(*cur + sizeof("uid=") - 1);
	    continue;
	}
	if (MATCHES(*cur, "gid=")) {
	    p = *cur + sizeof("gid=") - 1;
	    user_gid = (gid_t) atoi(p);
	    continue;
	}
	if (MATCHES(*cur, "groups=")) {
	    groups = *cur + sizeof("groups=") - 1;
	    continue;
	}
	if (MATCHES(*cur, "cwd=")) {
	    user_cwd = estrdup(*cur + sizeof("cwd=") - 1);
	    continue;
	}
	if (MATCHES(*cur, "tty=")) {
	    user_tty = user_ttypath = estrdup(*cur + sizeof("tty=") - 1);
	    if (strncmp(user_tty, _PATH_DEV, sizeof(_PATH_DEV) - 1) == 0)
		user_tty += sizeof(_PATH_DEV) - 1;
	    continue;
	}
	if (MATCHES(*cur, "host=")) {
	    user_host = user_shost = estrdup(*cur + sizeof("host=") - 1);
	    if ((p = strchr(user_host, '.')))
		user_shost = estrndup(user_host, (size_t)(p - user_host));
	    continue;
	}
	if (MATCHES(*cur, "lines=")) {
	    sudo_user.lines = atoi(*cur + sizeof("lines=") - 1);
	    continue;
	}
	if (MATCHES(*cur, "cols=")) {
	    sudo_user.cols = atoi(*cur + sizeof("cols=") - 1);
	    continue;
	}
    }
    if (user_cwd == NULL)
	user_cwd = "unknown";
    if (user_tty == NULL)
	user_tty = "unknown"; /* user_ttypath remains NULL */

    if (groups != NULL && groups[0] != '\0') {
	const char *cp;
	GETGROUPS_T *gids;
	int ngids;

	/* Count number of groups, including passwd gid. */
	ngids = 2;
	for (cp = groups; *cp != '\0'; cp++) {
	    if (*cp == ',')
		ngids++;
	}

	/* The first gid in the list is the passwd group gid. */
	gids = emalloc2(ngids, sizeof(GETGROUPS_T));
	gids[0] = user_gid;
	ngids = 1;
	cp = groups;
	for (;;) {
	    gids[ngids] = atoi(cp);
	    if (gids[0] != gids[ngids])
		ngids++;
	    cp = strchr(cp, ',');
	    if (cp == NULL)
		break;
	    cp++; /* skip over comma */
	}
	set_group_list(user_name, gids, ngids);
	efree(gids);
    }

    /* Setup debugging if indicated. */
    if (debug_flags != NULL) {
	sudo_debug_init(NULL, debug_flags);
	for (cur = settings; *cur != NULL; cur++)
	    sudo_debug_printf(SUDO_DEBUG_INFO, "settings: %s", *cur);
	for (cur = user_info; *cur != NULL; cur++)
	    sudo_debug_printf(SUDO_DEBUG_INFO, "user_info: %s", *cur);
    }

#undef MATCHES
    debug_return_int(flags);
}

static char *
resolve_editor(char *editor, int nfiles, char **files, char ***argv_out)
{
    char *cp, **nargv, *editor_path = NULL;
    int ac, i, nargc;
    bool wasblank;
    debug_decl(resolve_editor, SUDO_DEBUG_PLUGIN)

    editor = estrdup(editor); /* becomes part of argv_out */

    /*
     * Split editor into an argument vector; editor is reused (do not free).
     * The EDITOR and VISUAL environment variables may contain command
     * line args so look for those and alloc space for them too.
     */
    nargc = 1;
    for (wasblank = false, cp = editor; *cp != '\0'; cp++) {
	if (isblank((unsigned char) *cp))
	    wasblank = true;
	else if (wasblank) {
	    wasblank = false;
	    nargc++;
	}
    }
    /* If we can't find the editor in the user's PATH, give up. */
    cp = strtok(editor, " \t");
    if (cp == NULL ||
	find_path(cp, &editor_path, NULL, getenv("PATH"), 0) != FOUND) {
	efree(editor);
	debug_return_str(NULL);
    }
    nargv = (char **) emalloc2(nargc + 1 + nfiles + 1, sizeof(char *));
    for (ac = 0; cp != NULL && ac < nargc; ac++) {
	nargv[ac] = cp;
	cp = strtok(NULL, " \t");
    }
    nargv[ac++] = "--";
    for (i = 0; i < nfiles; )
	nargv[ac++] = files[i++];
    nargv[ac] = NULL;

    *argv_out = nargv;
    debug_return_str(editor_path);
}

/*
 * Determine which editor to use.  We don't need to worry about restricting
 * this to a "safe" editor since it runs with the uid of the invoking user,
 * not the runas (privileged) user.
 */
static char *
find_editor(int nfiles, char **files, char ***argv_out)
{
    char *cp, *editor, *editor_path = NULL, **ev, *ev0[4];
    debug_decl(find_editor, SUDO_DEBUG_PLUGIN)

    /*
     * If any of SUDO_EDITOR, VISUAL or EDITOR are set, choose the first one.
     */
    ev0[0] = "SUDO_EDITOR";
    ev0[1] = "VISUAL";
    ev0[2] = "EDITOR";
    ev0[3] = NULL;
    for (ev = ev0; *ev != NULL; ev++) {
	if ((editor = getenv(*ev)) != NULL && *editor != '\0') {
	    editor_path = resolve_editor(editor, nfiles, files, argv_out);
	    if (editor_path != NULL)
		break;
	}
    }
    if (editor_path == NULL) {
	/* def_editor could be a path, split it up */
	editor = estrdup(def_editor);
	cp = strtok(editor, ":");
	while (cp != NULL && editor_path == NULL) {
	    editor_path = resolve_editor(cp, nfiles, files, argv_out);
	    cp = strtok(NULL, ":");
	}
	if (editor_path)
	    efree(editor);
    }
    if (!editor_path) {
	audit_failure(NewArgv, _("%s: command not found"), editor);
	warningx(_("%s: command not found"), editor);
    }
    debug_return_str(editor_path);
}

#ifdef USE_ADMIN_FLAG
static void
create_admin_success_flag(void)
{
    struct stat statbuf;
    char flagfile[PATH_MAX];
    int fd, n;
    debug_decl(create_admin_success_flag, SUDO_DEBUG_PLUGIN)

    /* Check whether the user is in the admin group. */
    if (!user_in_group(sudo_user.pw, "admin"))
	debug_return;

    /* Build path to flag file. */
    n = snprintf(flagfile, sizeof(flagfile), "%s/.sudo_as_admin_successful",
	user_dir);
    if (n <= 0 || n >= sizeof(flagfile))
	debug_return;

    /* Create admin flag file if it doesn't already exist. */
    set_perms(PERM_USER);
    if (stat(flagfile, &statbuf) != 0) {
	fd = open(flagfile, O_CREAT|O_WRONLY|O_EXCL, 0644);
	close(fd);
    }
    restore_perms();
    debug_return;
}
#else /* !USE_ADMIN_FLAG */
static void
create_admin_success_flag(void)
{
    /* STUB */
}
#endif /* USE_ADMIN_FLAG */

static void
sudoers_policy_register_hooks(int version, int (*register_hook)(struct sudo_hook *hook))
{
    struct sudo_hook hook;

    memset(&hook, 0, sizeof(hook));
    hook.hook_version = SUDO_HOOK_VERSION;

    hook.hook_type = SUDO_HOOK_SETENV;
    hook.hook_fn = sudoers_hook_setenv;
    register_hook(&hook);

    hook.hook_type = SUDO_HOOK_UNSETENV;
    hook.hook_fn = sudoers_hook_unsetenv;
    register_hook(&hook);

    hook.hook_type = SUDO_HOOK_GETENV;
    hook.hook_fn = sudoers_hook_getenv;
    register_hook(&hook);

    hook.hook_type = SUDO_HOOK_PUTENV;
    hook.hook_fn = sudoers_hook_putenv;
    register_hook(&hook);
}

struct policy_plugin sudoers_policy = {
    SUDO_POLICY_PLUGIN,
    SUDO_API_VERSION,
    sudoers_policy_open,
    sudoers_policy_close,
    sudoers_policy_version,
    sudoers_policy_check,
    sudoers_policy_list,
    sudoers_policy_validate,
    sudoers_policy_invalidate,
    sudoers_policy_init_session,
    sudoers_policy_register_hooks
};
