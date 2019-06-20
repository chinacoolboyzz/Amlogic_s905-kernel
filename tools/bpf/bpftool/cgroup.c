// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)
// Copyright (C) 2017 Facebook
// Author: Roman Gushchin <guro@fb.com>

#define _XOPEN_SOURCE 500
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <bpf.h>

#include "main.h"

#define HELP_SPEC_ATTACH_FLAGS						\
	"ATTACH_FLAGS := { multi | override }"

#define HELP_SPEC_ATTACH_TYPES						       \
	"       ATTACH_TYPE := { ingress | egress | sock_create |\n"	       \
	"                        sock_ops | device | bind4 | bind6 |\n"	       \
	"                        post_bind4 | post_bind6 | connect4 |\n"       \
	"                        connect6 | sendmsg4 | sendmsg6 | sysctl }"

static const char * const attach_type_strings[] = {
	[BPF_CGROUP_INET_INGRESS] = "ingress",
	[BPF_CGROUP_INET_EGRESS] = "egress",
	[BPF_CGROUP_INET_SOCK_CREATE] = "sock_create",
	[BPF_CGROUP_SOCK_OPS] = "sock_ops",
	[BPF_CGROUP_DEVICE] = "device",
	[BPF_CGROUP_INET4_BIND] = "bind4",
	[BPF_CGROUP_INET6_BIND] = "bind6",
	[BPF_CGROUP_INET4_CONNECT] = "connect4",
	[BPF_CGROUP_INET6_CONNECT] = "connect6",
	[BPF_CGROUP_INET4_POST_BIND] = "post_bind4",
	[BPF_CGROUP_INET6_POST_BIND] = "post_bind6",
	[BPF_CGROUP_UDP4_SENDMSG] = "sendmsg4",
	[BPF_CGROUP_UDP6_SENDMSG] = "sendmsg6",
	[BPF_CGROUP_SYSCTL] = "sysctl",
	[__MAX_BPF_ATTACH_TYPE] = NULL,
};

static enum bpf_attach_type parse_attach_type(const char *str)
{
	enum bpf_attach_type type;

	for (type = 0; type < __MAX_BPF_ATTACH_TYPE; type++) {
		if (attach_type_strings[type] &&
		    is_prefix(str, attach_type_strings[type]))
			return type;
	}

	return __MAX_BPF_ATTACH_TYPE;
}

static int show_bpf_prog(int id, const char *attach_type_str,
			 const char *attach_flags_str,
			 int level)
{
	struct bpf_prog_info info = {};
	__u32 info_len = sizeof(info);
	int prog_fd;

	prog_fd = bpf_prog_get_fd_by_id(id);
	if (prog_fd < 0)
		return -1;

	if (bpf_obj_get_info_by_fd(prog_fd, &info, &info_len)) {
		close(prog_fd);
		return -1;
	}

	if (json_output) {
		jsonw_start_object(json_wtr);
		jsonw_uint_field(json_wtr, "id", info.id);
		jsonw_string_field(json_wtr, "attach_type",
				   attach_type_str);
		jsonw_string_field(json_wtr, "attach_flags",
				   attach_flags_str);
		jsonw_string_field(json_wtr, "name", info.name);
		jsonw_end_object(json_wtr);
	} else {
		printf("%s%-8u %-15s %-15s %-15s\n", level ? "    " : "",
		       info.id,
		       attach_type_str,
		       attach_flags_str,
		       info.name);
	}

	close(prog_fd);
	return 0;
}

static int count_attached_bpf_progs(int cgroup_fd, enum bpf_attach_type type)
{
	__u32 prog_cnt = 0;
	int ret;

	ret = bpf_prog_query(cgroup_fd, type, 0, NULL, NULL, &prog_cnt);
	if (ret)
		return -1;

	return prog_cnt;
}

static int show_attached_bpf_progs(int cgroup_fd, enum bpf_attach_type type,
				   int level)
{
	__u32 prog_ids[1024] = {0};
	char *attach_flags_str;
	__u32 prog_cnt, iter;
	__u32 attach_flags;
	char buf[32];
	int ret;

	prog_cnt = ARRAY_SIZE(prog_ids);
	ret = bpf_prog_query(cgroup_fd, type, 0, &attach_flags, prog_ids,
			     &prog_cnt);
	if (ret)
		return ret;

	if (prog_cnt == 0)
		return 0;

	switch (attach_flags) {
	case BPF_F_ALLOW_MULTI:
		attach_flags_str = "multi";
		break;
	case BPF_F_ALLOW_OVERRIDE:
		attach_flags_str = "override";
		break;
	case 0:
		attach_flags_str = "";
		break;
	default:
		snprintf(buf, sizeof(buf), "unknown(%x)", attach_flags);
		attach_flags_str = buf;
	}

	for (iter = 0; iter < prog_cnt; iter++)
		show_bpf_prog(prog_ids[iter], attach_type_strings[type],
			      attach_flags_str, level);

	return 0;
}

static int do_show(int argc, char **argv)
{
	enum bpf_attach_type type;
	int cgroup_fd;
	int ret = -1;

	if (argc < 1) {
		p_err("too few parameters for cgroup show");
		goto exit;
	} else if (argc > 1) {
		p_err("too many parameters for cgroup show");
		goto exit;
	}

	cgroup_fd = open(argv[0], O_RDONLY);
	if (cgroup_fd < 0) {
		p_err("can't open cgroup %s", argv[1]);
		goto exit;
	}

	if (json_output)
		jsonw_start_array(json_wtr);
	else
		printf("%-8s %-15s %-15s %-15s\n", "ID", "AttachType",
		       "AttachFlags", "Name");

	for (type = 0; type < __MAX_BPF_ATTACH_TYPE; type++) {
		/*
		 * Not all attach types may be supported, so it's expected,
		 * that some requests will fail.
		 * If we were able to get the show for at least one
		 * attach type, let's return 0.
		 */
		if (show_attached_bpf_progs(cgroup_fd, type, 0) == 0)
			ret = 0;
	}

	if (json_output)
		jsonw_end_array(json_wtr);

	close(cgroup_fd);
exit:
	return ret;
}

/*
 * To distinguish nftw() errors and do_show_tree_fn() errors
 * and avoid duplicating error messages, let's return -2
 * from do_show_tree_fn() in case of error.
 */
#define NFTW_ERR		-1
#define SHOW_TREE_FN_ERR	-2
static int do_show_tree_fn(const char *fpath, const struct stat *sb,
			   int typeflag, struct FTW *ftw)
{
	enum bpf_attach_type type;
	bool skip = true;
	int cgroup_fd;

	if (typeflag != FTW_D)
		return 0;

	cgroup_fd = open(fpath, O_RDONLY);
	if (cgroup_fd < 0) {
		p_err("can't open cgroup %s: %s", fpath, strerror(errno));
		return SHOW_TREE_FN_ERR;
	}

	for (type = 0; type < __MAX_BPF_ATTACH_TYPE; type++) {
		int count = count_attached_bpf_progs(cgroup_fd, type);

		if (count < 0 && errno != EINVAL) {
			p_err("can't query bpf programs attached to %s: %s",
			      fpath, strerror(errno));
			close(cgroup_fd);
			return SHOW_TREE_FN_ERR;
		}
		if (count > 0) {
			skip = false;
			break;
		}
	}

	if (skip) {
		close(cgroup_fd);
		return 0;
	}

	if (json_output) {
		jsonw_start_object(json_wtr);
		jsonw_string_field(json_wtr, "cgroup", fpath);
		jsonw_name(json_wtr, "programs");
		jsonw_start_array(json_wtr);
	} else {
		printf("%s\n", fpath);
	}

	for (type = 0; type < __MAX_BPF_ATTACH_TYPE; type++)
		show_attached_bpf_progs(cgroup_fd, type, ftw->level);

	if (errno == EINVAL)
		/* Last attach type does not support query.
		 * Do not report an error for this, especially because batch
		 * mode would stop processing commands.
		 */
		errno = 0;

	if (json_output) {
		jsonw_end_array(json_wtr);
		jsonw_end_object(json_wtr);
	}

	close(cgroup_fd);

	return 0;
}

static char *find_cgroup_root(void)
{
	struct mntent *mnt;
	FILE *f;

	f = fopen("/proc/mounts", "r");
	if (f == NULL)
		return NULL;

	while ((mnt = getmntent(f))) {
		if (strcmp(mnt->mnt_type, "cgroup2") == 0) {
			fclose(f);
			return strdup(mnt->mnt_dir);
		}
	}

	fclose(f);
	return NULL;
}

static int do_show_tree(int argc, char **argv)
{
	char *cgroup_root;
	int ret;

	switch (argc) {
	case 0:
		cgroup_root = find_cgroup_root();
		if (!cgroup_root) {
			p_err("cgroup v2 isn't mounted");
			return -1;
		}
		break;
	case 1:
		cgroup_root = argv[0];
		break;
	default:
		p_err("too many parameters for cgroup tree");
		return -1;
	}


	if (json_output)
		jsonw_start_array(json_wtr);
	else
		printf("%s\n"
		       "%-8s %-15s %-15s %-15s\n",
		       "CgroupPath",
		       "ID", "AttachType", "AttachFlags", "Name");

	switch (nftw(cgroup_root, do_show_tree_fn, 1024, FTW_MOUNT)) {
	case NFTW_ERR:
		p_err("can't iterate over %s: %s", cgroup_root,
		      strerror(errno));
		ret = -1;
		break;
	case SHOW_TREE_FN_ERR:
		ret = -1;
		break;
	default:
		ret = 0;
	}

	if (json_output)
		jsonw_end_array(json_wtr);

	if (argc == 0)
		free(cgroup_root);

	return ret;
}

static int do_attach(int argc, char **argv)
{
	enum bpf_attach_type attach_type;
	int cgroup_fd, prog_fd;
	int attach_flags = 0;
	int ret = -1;
	int i;

	if (argc < 4) {
		p_err("too few parameters for cgroup attach");
		goto exit;
	}

	cgroup_fd = open(argv[0], O_RDONLY);
	if (cgroup_fd < 0) {
		p_err("can't open cgroup %s", argv[1]);
		goto exit;
	}

	attach_type = parse_attach_type(argv[1]);
	if (attach_type == __MAX_BPF_ATTACH_TYPE) {
		p_err("invalid attach type");
		goto exit_cgroup;
	}

	argc -= 2;
	argv = &argv[2];
	prog_fd = prog_parse_fd(&argc, &argv);
	if (prog_fd < 0)
		goto exit_cgroup;

	for (i = 0; i < argc; i++) {
		if (is_prefix(argv[i], "multi")) {
			attach_flags |= BPF_F_ALLOW_MULTI;
		} else if (is_prefix(argv[i], "override")) {
			attach_flags |= BPF_F_ALLOW_OVERRIDE;
		} else {
			p_err("unknown option: %s", argv[i]);
			goto exit_cgroup;
		}
	}

	if (bpf_prog_attach(prog_fd, cgroup_fd, attach_type, attach_flags)) {
		p_err("failed to attach program");
		goto exit_prog;
	}

	if (json_output)
		jsonw_null(json_wtr);

	ret = 0;

exit_prog:
	close(prog_fd);
exit_cgroup:
	close(cgroup_fd);
exit:
	return ret;
}

static int do_detach(int argc, char **argv)
{
	enum bpf_attach_type attach_type;
	int prog_fd, cgroup_fd;
	int ret = -1;

	if (argc < 4) {
		p_err("too few parameters for cgroup detach");
		goto exit;
	}

	cgroup_fd = open(argv[0], O_RDONLY);
	if (cgroup_fd < 0) {
		p_err("can't open cgroup %s", argv[1]);
		goto exit;
	}

	attach_type = parse_attach_type(argv[1]);
	if (attach_type == __MAX_BPF_ATTACH_TYPE) {
		p_err("invalid attach type");
		goto exit_cgroup;
	}

	argc -= 2;
	argv = &argv[2];
	prog_fd = prog_parse_fd(&argc, &argv);
	if (prog_fd < 0)
		goto exit_cgroup;

	if (bpf_prog_detach2(prog_fd, cgroup_fd, attach_type)) {
		p_err("failed to detach program");
		goto exit_prog;
	}

	if (json_output)
		jsonw_null(json_wtr);

	ret = 0;

exit_prog:
	close(prog_fd);
exit_cgroup:
	close(cgroup_fd);
exit:
	return ret;
}

static int do_help(int argc, char **argv)
{
	if (json_output) {
		jsonw_null(json_wtr);
		return 0;
	}

	fprintf(stderr,
		"Usage: %s %s { show | list } CGROUP\n"
		"       %s %s tree [CGROUP_ROOT]\n"
		"       %s %s attach CGROUP ATTACH_TYPE PROG [ATTACH_FLAGS]\n"
		"       %s %s detach CGROUP ATTACH_TYPE PROG\n"
		"       %s %s help\n"
		"\n"
		HELP_SPEC_ATTACH_TYPES "\n"
		"       " HELP_SPEC_ATTACH_FLAGS "\n"
		"       " HELP_SPEC_PROGRAM "\n"
		"       " HELP_SPEC_OPTIONS "\n"
		"",
		bin_name, argv[-2],
		bin_name, argv[-2], bin_name, argv[-2],
		bin_name, argv[-2], bin_name, argv[-2]);

	return 0;
}

static const struct cmd cmds[] = {
	{ "show",	do_show },
	{ "list",	do_show },
	{ "tree",       do_show_tree },
	{ "attach",	do_attach },
	{ "detach",	do_detach },
	{ "help",	do_help },
	{ 0 }
};

int do_cgroup(int argc, char **argv)
{
	return cmd_select(cmds, argc, argv, do_help);
}
