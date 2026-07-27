/**
 * Copyright (C) 2012-2015 Yecheng Fu <cofyc.jackson at gmail dot com>
 * All rights reserved.
 *
 * Use of this source code is governed by a MIT-style license that can be found
 * in the LICENSE file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <stdint.h>
#include "argparse.h"

/*
 * Curve Optimiser values are raw 32 bit words: RyzenAdj performs no arithmetic
 * on them, the value lands in the SMU mailbox argument register unchanged
 * (lib/api.c: args.arg0 = value). The caller supplies the already-encoded
 * two's complement word, so the only job here is to reproduce upstream's
 * strtoul() bit pattern without the silent wrap-around on out-of-range input.
 */
#define CO32_LIMIT (1LL << 32)

#define OPT_UNSET 1
#define OPT_LONG  (1 << 1)

static const char *
prefix_skip(const char *str, const char *prefix)
{
	size_t len = strlen(prefix);
	return strncmp(str, prefix, len) ? NULL : str + len;
}

static int
prefix_cmp(const char *str, const char *prefix)
{
	for (;; str++, prefix++)
		if (!*prefix) {
			return 0;
		} else if (*str != *prefix) {
			return (unsigned char)*prefix - (unsigned char)*str;
		}
}

/*
 * Portable strerror wrapper.
 *
 * The old code called strerror_r() and then printed `buf` while ignoring the
 * return value. That is only correct for the XSI variant; when the translation
 * unit is built with _GNU_SOURCE (which several distro toolchains and any
 * `-std=gnu*` + feature-test combination can pull in) glibc provides the GNU
 * variant, which may leave `buf` untouched and return a pointer to a static
 * string - so the user got an empty error message.
 */
static const char *
safe_strerror(int err, char *buf, size_t buflen)
{
	buf[0] = '\0';
#ifdef _WIN32
	strerror_s(buf, buflen, err);
	return buf;
#elif defined(__GLIBC__) && defined(_GNU_SOURCE)
	return strerror_r(err, buf, buflen);
#else
	if (strerror_r(err, buf, buflen) != 0)
		snprintf(buf, buflen, "errno %d", err);
	return buf;
#endif
}

static void
argparse_error(struct argparse *self, const struct argparse_option *opt,
			   const char *reason, int flags)
{
	(void)self;
	if (flags & OPT_LONG) {
		fprintf(stderr, "error: option `--%s` %s\n", opt->long_name, reason);
	} else {
		fprintf(stderr, "error: option `-%c` %s\n", opt->short_name, reason);
	}
	exit(1);
}

static int
argparse_getvalue(struct argparse *self, const struct argparse_option *opt,
				  int flags)
{
	const char *s = NULL;
	char buf[256];

	buf[0] = 0;

	if (!opt->value)
		goto skipped;
	switch (opt->type) {
	case ARGPARSE_OPT_BOOLEAN:
		if (flags & OPT_UNSET) {
			*(int *)opt->value = *(int *)opt->value - 1;
		} else {
			*(int *)opt->value = *(int *)opt->value + 1;
		}
		if (*(int *)opt->value < 0) {
			*(int *)opt->value = 0;
		}
		break;
	case ARGPARSE_OPT_BIT:
		if (flags & OPT_UNSET) {
			*(int *)opt->value &= ~opt->data;
		} else {
			*(int *)opt->value |= opt->data;
		}
		break;
	case ARGPARSE_OPT_STRING:
		if (self->optvalue) {
			*(const char **)opt->value = self->optvalue;
			self->optvalue             = NULL;
		} else if (self->argc > 1) {
			self->argc--;
			*(const char **)opt->value = *++self->argv;
		} else {
			argparse_error(self, opt, "requires a value", flags);
		}
		break;
	case ARGPARSE_OPT_INTEGER: {
		const char *raw = NULL;
		long lval;

		if (self->optvalue) {
			raw = self->optvalue;
			self->optvalue = NULL;
		} else if (self->argc > 1) {
			self->argc--;
			raw = *++self->argv;
		} else {
			argparse_error(self, opt, "requires a value", flags);
		}

		if (raw == NULL || raw[0] == '\0')
			argparse_error(self, opt, "requires a value", flags);

		errno = 0;
		lval = strtol(raw, (char **)&s, 0);
		if (errno)
			argparse_error(self, opt, safe_strerror(errno, buf, sizeof(buf)), flags);
		if (s == raw || s[0] != '\0')
			argparse_error(self, opt, "expects an integer value", flags);
		if (lval < INT_MIN || lval > INT_MAX)
			argparse_error(self, opt, "value is out of range for a 32-bit signed integer", flags);

		*(int *)opt->value = (int)lval;
		break;
	}
	case ARGPARSE_OPT_U32: {
		const char *raw = NULL;
		const char *scan;
		unsigned long uval;

		if (self->optvalue) {
			raw = self->optvalue;
			self->optvalue = NULL;
		} else if (self->argc > 1) {
			self->argc--;
			raw = *++self->argv;
		} else {
			argparse_error(self, opt, "requires a value", flags);
		}

		if (raw == NULL || raw[0] == '\0')
			argparse_error(self, opt, "requires a value", flags);

		/*
		 * strtoul() cheerfully wraps negative input: "-1" became 0xFFFFFFFF,
		 * which is exactly the sentinel RyzenAdj uses for "option not given",
		 * and "-5" became 4294967291 - a value that was then handed straight
		 * to the SMU as a power/current limit. Reject signs outright.
		 */
		scan = raw;
		while (isspace((unsigned char)*scan))
			scan++;
		if (*scan == '-' || *scan == '+')
			argparse_error(self, opt, "expects a non-negative value", flags);

		errno = 0;
		uval = strtoul(raw, (char **)&s, 0);
		if (errno)
			argparse_error(self, opt, safe_strerror(errno, buf, sizeof(buf)), flags);
		if (s == raw || s[0] != '\0')
			argparse_error(self, opt, "expects an unsigned 32-bit integer value", flags);
		/* on LP64 strtoul returns 64 bit; the assignment used to truncate silently */
		if (uval > UINT32_MAX)
			argparse_error(self, opt, "value is out of range for a 32-bit unsigned integer", flags);

		*(uint32_t *)opt->value = (uint32_t)uval;
		break;
	}
	case ARGPARSE_OPT_CO32: {
		const char *raw = NULL;
		const char *scan;

		if (self->optvalue) {
			raw = self->optvalue;
			self->optvalue = NULL;
		} else if (self->argc > 1) {
			self->argc--;
			raw = *++self->argv;
		} else {
			argparse_error(self, opt, "requires a value", flags);
		}

		if (raw == NULL || raw[0] == '\0')
			argparse_error(self, opt, "requires a value", flags);

		scan = raw;
		while (isspace((unsigned char)*scan))
			scan++;

		errno = 0;

		if (*scan == '-') {
			/*
			 * Curve Optimiser offsets are genuinely signed: "--set-coall=-20"
			 * is the normal undervolt case. The value handed to the SMU is the
			 * two's complement 32-bit pattern, which is what the old strtoul()
			 * wrap-around produced by accident; here it is deliberate.
			 */
			const long long sval = strtoll(raw, (char **)&s, 0);

			if (errno)
				argparse_error(self, opt, safe_strerror(errno, buf, sizeof(buf)), flags);
			if (s == raw || s[0] != '\0')
				argparse_error(self, opt, "expects an integer value", flags);
			if (sval <= -CO32_LIMIT)
				argparse_error(self, opt, "value does not fit in 32 bits", flags);

			*(int64_t *)opt->value = sval;
		} else {
			const unsigned long uval = strtoul(raw, (char **)&s, 0);

			if (errno)
				argparse_error(self, opt, safe_strerror(errno, buf, sizeof(buf)), flags);
			if (s == raw || s[0] != '\0')
				argparse_error(self, opt, "expects an integer value", flags);
			if (uval > UINT32_MAX)
				argparse_error(self, opt, "value is out of range for a 32-bit unsigned integer", flags);

			*(int64_t *)opt->value = (int64_t)uval;
		}
		break;
	}
	case ARGPARSE_OPT_FLOAT: {
		const char *raw = NULL;
		float fval;

		if (self->optvalue) {
			raw = self->optvalue;
			self->optvalue = NULL;
		} else if (self->argc > 1) {
			self->argc--;
			raw = *++self->argv;
		} else {
			argparse_error(self, opt, "requires a value", flags);
		}

		if (raw == NULL || raw[0] == '\0')
			argparse_error(self, opt, "requires a value", flags);

		errno = 0;
		fval = strtof(raw, (char **)&s);
		if (errno)
			argparse_error(self, opt, safe_strerror(errno, buf, sizeof(buf)), flags);
		if (s == raw || s[0] != '\0')
			argparse_error(self, opt, "expects a numerical value", flags);

		*(float *)opt->value = fval;
		break;
	}
	default:
		assert(0);
	}

skipped:
	if (opt->callback) {
		return opt->callback(self, opt);
	}

	return 0;
}

static void
argparse_options_check(const struct argparse_option *options)
{
	for (; options->type != ARGPARSE_OPT_END; options++) {
		switch (options->type) {
		case ARGPARSE_OPT_END:
		case ARGPARSE_OPT_BOOLEAN:
		case ARGPARSE_OPT_BIT:
		case ARGPARSE_OPT_INTEGER:
		case ARGPARSE_OPT_U32:
		case ARGPARSE_OPT_CO32:
		case ARGPARSE_OPT_FLOAT:
		case ARGPARSE_OPT_STRING:
		case ARGPARSE_OPT_GROUP:
			continue;
		default:
			fprintf(stderr, "wrong option type: %d", options->type);
			break;
		}
	}
}

static int
argparse_short_opt(struct argparse *self, const struct argparse_option *options)
{
	for (; options->type != ARGPARSE_OPT_END; options++) {
		if (options->short_name == *self->optvalue) {
			self->optvalue = self->optvalue[1] ? self->optvalue + 1 : NULL;
			return argparse_getvalue(self, options, 0);
		}
	}
	return -2;
}

static int
argparse_long_opt(struct argparse *self, const struct argparse_option *options)
{
	for (; options->type != ARGPARSE_OPT_END; options++) {
		const char *rest;
		int opt_flags = 0;
		if (!options->long_name)
			continue;

		rest = prefix_skip(self->argv[0] + 2, options->long_name);
		if (!rest) {
			// negation disabled?
			if (options->flags & OPT_NONEG) {
				continue;
			}
			// only OPT_BOOLEAN/OPT_BIT supports negation
			if (options->type != ARGPARSE_OPT_BOOLEAN && options->type !=
				ARGPARSE_OPT_BIT) {
				continue;
			}

			if (prefix_cmp(self->argv[0] + 2, "no-")) {
				continue;
			}
			rest = prefix_skip(self->argv[0] + 2 + 3, options->long_name);
			if (!rest)
				continue;
			opt_flags |= OPT_UNSET;
		}
		if (*rest) {
			if (*rest != '=')
				continue;
			self->optvalue = rest + 1;
		}
		return argparse_getvalue(self, options, opt_flags | OPT_LONG);
	}
	return -2;
}

int
argparse_init(struct argparse *self, struct argparse_option *options,
			  const char *const *usages, int flags)
{
	memset(self, 0, sizeof(*self));
	self->options     = options;
	self->usages      = usages;
	self->flags       = flags;
	self->description = NULL;
	self->epilog      = NULL;
	return 0;
}

void
argparse_describe(struct argparse *self, const char *description,
				  const char *epilog)
{
	self->description = description;
	self->epilog      = epilog;
}

int
argparse_parse(struct argparse *self, int argc, const char **argv)
{
	self->argc = argc - 1;
	self->argv = argv + 1;
	self->out  = argv;

	argparse_options_check(self->options);
	if(!self->argc) {
		argparse_usage(self);
		exit(1);
	}

	for (; self->argc; self->argc--, self->argv++) {
		const char *arg = self->argv[0];
		if (arg[0] != '-' || !arg[1]) {
			if (self->flags & ARGPARSE_STOP_AT_NON_OPTION) {
				goto end;
			}
			if (self->flags & ARGPARSE_NON_OPTION_IS_INVALID) {
				goto unknown;
			}
			// if it's not option or is a single char '-', copy verbatim
			self->out[self->cpidx++] = self->argv[0];
			continue;
		}
		// short option
		if (arg[1] != '-') {
			self->optvalue = arg + 1;
			switch (argparse_short_opt(self, self->options)) {
			case -1:
				break;
			case -2:
				goto unknown;
			}
			while (self->optvalue) {
				switch (argparse_short_opt(self, self->options)) {
				case -1:
					break;
				case -2:
					goto unknown;
				}
			}
			continue;
		}
		// if '--' presents
		if (!arg[2]) {
			self->argc--;
			self->argv++;
			break;
		}
		// long option
		switch (argparse_long_opt(self, self->options)) {
		case -1:
			break;
		case -2:
			goto unknown;
		}
		continue;

unknown:
		fprintf(stderr, "error: unknown option `%s`\n", self->argv[0]);
		argparse_usage(self);
		exit(1);
	}

end:
	memmove(self->out + self->cpidx, self->argv,
			self->argc * sizeof(*self->out));
	self->out[self->cpidx + self->argc] = NULL;

	return self->cpidx + self->argc;
}

void
argparse_usage(struct argparse *self)
{
	if (self->usages) {
		fprintf(stdout, "Usage: %s\n", *self->usages++);
		while (*self->usages && **self->usages)
			fprintf(stdout, "   or: %s\n", *self->usages++);
	} else {
		fprintf(stdout, "Usage:\n");
	}

	// print description
	if (self->description)
		fprintf(stdout, "%s\n", self->description);

	fputc('\n', stdout);

	const struct argparse_option *options;

	// figure out best width
	size_t usage_opts_width = 0;
	size_t len;
	options = self->options;
	for (; options->type != ARGPARSE_OPT_END; options++) {
		len = 0;
		if ((options)->short_name) {
			len += 2;
		}
		if ((options)->short_name && (options)->long_name) {
			len += 2;           // separator ", "
		}
		if ((options)->long_name) {
			len += strlen((options)->long_name) + 2;
		}
		if (options->type == ARGPARSE_OPT_INTEGER) {
			len += strlen("=<int>");
		}
		if (options->type == ARGPARSE_OPT_U32) {
			len += strlen("=<u32>");
		}
		if (options->type == ARGPARSE_OPT_CO32) {
			len += strlen("=<int>");
		}
		if (options->type == ARGPARSE_OPT_FLOAT) {
			len += strlen("=<flt>");
		} else if (options->type == ARGPARSE_OPT_STRING) {
			len += strlen("=<str>");
		}
		len = (len + 3) - ((len + 3) & 3);
		if (usage_opts_width < len) {
			usage_opts_width = len;
		}
	}
	usage_opts_width += 4;      // 4 spaces prefix

	options = self->options;
	for (; options->type != ARGPARSE_OPT_END; options++) {
		size_t pos = 0;
		size_t pad = 0;
		if (options->type == ARGPARSE_OPT_GROUP) {
			fputc('\n', stdout);
			fprintf(stdout, "%s", options->help);
			fputc('\n', stdout);
			continue;
		}
		pos = fprintf(stdout, "    ");
		if (options->short_name) {
			pos += fprintf(stdout, "-%c", options->short_name);
		}
		if (options->long_name && options->short_name) {
			pos += fprintf(stdout, ", ");
		}
		if (options->long_name) {
			pos += fprintf(stdout, "--%s", options->long_name);
		}
		if (options->type == ARGPARSE_OPT_INTEGER) {
			pos += fprintf(stdout, "=<int>");
		} else if (options->type == ARGPARSE_OPT_U32) {
			pos += fprintf(stdout, "=<u32>");
		} else if (options->type == ARGPARSE_OPT_CO32) {
			pos += fprintf(stdout, "=<int>");
		} else if (options->type == ARGPARSE_OPT_FLOAT) {
			pos += fprintf(stdout, "=<flt>");
		} else if (options->type == ARGPARSE_OPT_STRING) {
			pos += fprintf(stdout, "=<str>");
		}
		if (pos <= usage_opts_width) {
			pad = usage_opts_width - pos;
		} else {
			fputc('\n', stdout);
			pad = usage_opts_width;
		}
		fprintf(stdout, "%*s%s\n", (int)pad + 2, "", options->help);
	}

	// print epilog
	if (self->epilog)
		fprintf(stdout, "%s\n", self->epilog);
}

int
argparse_help_cb(struct argparse *self, const struct argparse_option *option)
{
	(void)option;
	argparse_usage(self);
	exit(0);
}
