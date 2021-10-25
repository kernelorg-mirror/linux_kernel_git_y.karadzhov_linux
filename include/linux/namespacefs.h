/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * namespacefs.h - a pseudo file system for examining namespaces.
 */

#ifndef _NAMESPACEFS_H_
#define _NAMESPACEFS_H_

#ifdef CONFIG_NAMESPACE_FS

#include <linux/fs.h>

int namespacefs_create_pid_ns_dir(struct pid_namespace *ns);

void namespacefs_remove_pid_ns_dir(struct pid_namespace *ns);

#else

static inline int
namespacefs_create_pid_ns_dir(struct pid_namespace *ns)
{
	return 0;
}

static inline void
namespacefs_remove_pid_ns_dir(struct pid_namespace *ns)
{
}

#endif /* CONFIG_NAMESPACE_FS */

#endif
