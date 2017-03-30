/*-
 * Copyright (c) 2003-2009 Tim Kientzle
 * Copyright (c) 2010-2012 Michihiro NAKAJIMA
 * Copyright (c) 2016-2017 Martin Matuska
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "archive_platform.h"

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_ACL_H
#define _ACL_PRIVATE /* For debugging */
#include <sys/acl.h>
#endif

#include "archive_entry.h"
#include "archive_private.h"
#include "archive_read_disk_private.h"
#include "archive_acl_maps.h"

/*
 * Translate FreeBSD ACLs into libarchive internal structure
 */
static int
translate_acl(struct archive_read_disk *a,
    struct archive_entry *entry, acl_t acl, int default_entry_acl_type)
{
#if ARCHIVE_ACL_FREEBSD_NFS4
	int brand;
	acl_flagset_t	 acl_flagset;
#endif
	acl_tag_t	 acl_tag;
	acl_entry_t	 acl_entry;
	acl_permset_t	 acl_permset;
	acl_entry_type_t acl_type;
	int		 i, entry_acl_type, perm_map_size;
	const acl_perm_map_t	*perm_map;
	int		 r, s, ae_id, ae_tag, ae_perm;
	void		*q;
	const char	*ae_name;

#if ARCHIVE_ACL_FREEBSD_NFS4
	// FreeBSD "brands" ACLs as POSIX.1e or NFSv4
	// Make sure the "brand" on this ACL is consistent
	// with the default_entry_acl_type bits provided.
	if (acl_get_brand_np(acl, &brand) != 0) {
		archive_set_error(&a->archive, errno,
		    "Failed to read ACL brand");
		return (ARCHIVE_WARN);
	}
	switch (brand) {
	case ACL_BRAND_POSIX:
		switch (default_entry_acl_type) {
		case ARCHIVE_ENTRY_ACL_TYPE_ACCESS:
		case ARCHIVE_ENTRY_ACL_TYPE_DEFAULT:
			break;
		default:
			archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
			    "Invalid ACL entry type for POSIX.1e ACL");
			return (ARCHIVE_WARN);
		}
		break;
	case ACL_BRAND_NFS4:
		if (default_entry_acl_type & ~ARCHIVE_ENTRY_ACL_TYPE_NFS4) {
			archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
			    "Invalid ACL entry type for NFSv4 ACL");
			return (ARCHIVE_WARN);
		}
		break;
	default:
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Unknown ACL brand");
		return (ARCHIVE_WARN);
	}
#endif

	s = acl_get_entry(acl, ACL_FIRST_ENTRY, &acl_entry);
	if (s == -1) {
		archive_set_error(&a->archive, errno,
		    "Failed to get first ACL entry");
		return (ARCHIVE_WARN);
	}

	while (s == 1) {
		ae_id = -1;
		ae_name = NULL;
		ae_perm = 0;

		if (acl_get_tag_type(acl_entry, &acl_tag) != 0) {
			archive_set_error(&a->archive, errno,
			    "Failed to get ACL tag type");
			return (ARCHIVE_WARN);
		}
		switch (acl_tag) {
		case ACL_USER:
			q = acl_get_qualifier(acl_entry);
			if (q != NULL) {
				ae_id = (int)*(uid_t *)q;
				acl_free(q);
				ae_name = archive_read_disk_uname(&a->archive,
				    ae_id);
			}
			ae_tag = ARCHIVE_ENTRY_ACL_USER;
			break;
		case ACL_GROUP:
			q = acl_get_qualifier(acl_entry);
			if (q != NULL) {
				ae_id = (int)*(gid_t *)q;
				acl_free(q);
				ae_name = archive_read_disk_gname(&a->archive,
				    ae_id);
			}
			ae_tag = ARCHIVE_ENTRY_ACL_GROUP;
			break;
		case ACL_MASK:
			ae_tag = ARCHIVE_ENTRY_ACL_MASK;
			break;
		case ACL_USER_OBJ:
			ae_tag = ARCHIVE_ENTRY_ACL_USER_OBJ;
			break;
		case ACL_GROUP_OBJ:
			ae_tag = ARCHIVE_ENTRY_ACL_GROUP_OBJ;
			break;
		case ACL_OTHER:
			ae_tag = ARCHIVE_ENTRY_ACL_OTHER;
			break;
#if ARCHIVE_ACL_FREEBSD_NFS4
		case ACL_EVERYONE:
			ae_tag = ARCHIVE_ENTRY_ACL_EVERYONE;
			break;
#endif
		default:
			/* Skip types that libarchive can't support. */
			s = acl_get_entry(acl, ACL_NEXT_ENTRY, &acl_entry);
			continue;
		}

		// XXX acl_type maps to allow/deny/audit/YYYY bits
		entry_acl_type = default_entry_acl_type;

#if ARCHIVE_ACL_FREEBSD_NFS4
		if (default_entry_acl_type & ARCHIVE_ENTRY_ACL_TYPE_NFS4) {
			/*
			 * acl_get_entry_type_np() fails with non-NFSv4 ACLs
			 */
			if (acl_get_entry_type_np(acl_entry, &acl_type) != 0) {
				archive_set_error(&a->archive, errno, "Failed "
				    "to get ACL type from a NFSv4 ACL entry");
				return (ARCHIVE_WARN);
			}
			switch (acl_type) {
			case ACL_ENTRY_TYPE_ALLOW:
				entry_acl_type = ARCHIVE_ENTRY_ACL_TYPE_ALLOW;
				break;
			case ACL_ENTRY_TYPE_DENY:
				entry_acl_type = ARCHIVE_ENTRY_ACL_TYPE_DENY;
				break;
			case ACL_ENTRY_TYPE_AUDIT:
				entry_acl_type = ARCHIVE_ENTRY_ACL_TYPE_AUDIT;
				break;
			case ACL_ENTRY_TYPE_ALARM:
				entry_acl_type = ARCHIVE_ENTRY_ACL_TYPE_ALARM;
				break;
			default:
				archive_set_error(&a->archive, errno,
				    "Invalid NFSv4 ACL entry type");
				return (ARCHIVE_WARN);
			}

			/*
			 * Libarchive stores "flag" (NFSv4 inheritance bits)
			 * in the ae_perm bitmap.
			 *
			 * acl_get_flagset_np() fails with non-NFSv4 ACLs
			 */
			if (acl_get_flagset_np(acl_entry, &acl_flagset) != 0) {
				archive_set_error(&a->archive, errno,
				    "Failed to get flagset from a NFSv4 "
				    "ACL entry");
				return (ARCHIVE_WARN);
			}
			for (i = 0; i < acl_nfs4_flag_map_size; ++i) {
				r = acl_get_flag_np(acl_flagset,
				    acl_nfs4_flag_map[i].p_perm);
				if (r == -1) {
					archive_set_error(&a->archive, errno,
					    "Failed to check flag in a NFSv4 "
					    "ACL flagset");
					return (ARCHIVE_WARN);
				} else if (r)
					ae_perm |= acl_nfs4_flag_map[i].a_perm;
			}
		}
#endif

		if (acl_get_permset(acl_entry, &acl_permset) != 0) {
			archive_set_error(&a->archive, errno,
			    "Failed to get ACL permission set");
			return (ARCHIVE_WARN);
		}

#if ARCHIVE_ACL_FREEBSD_NFS4
		if (default_entry_acl_type & ARCHIVE_ENTRY_ACL_TYPE_NFS4) {
			perm_map_size = acl_nfs4_perm_map_size;
			perm_map = acl_nfs4_perm_map;
		} else {
#endif
			perm_map_size = acl_posix_perm_map_size;
			perm_map = acl_posix_perm_map;
#if ARCHIVE_ACL_FREEBSD_NFS4
		}
#endif

		for (i = 0; i < perm_map_size; ++i) {
			r = acl_get_perm_np(acl_permset, perm_map[i].p_perm);
			if (r == -1) {
				archive_set_error(&a->archive, errno,
				    "Failed to check permission in an ACL "
				    "permission set");
				return (ARCHIVE_WARN);
			} else if (r)
				ae_perm |= perm_map[i].a_perm;
		}

		archive_entry_acl_add_entry(entry, entry_acl_type,
					    ae_perm, ae_tag,
					    ae_id, ae_name);

		s = acl_get_entry(acl, ACL_NEXT_ENTRY, &acl_entry);
		if (s == -1) {
			archive_set_error(&a->archive, errno,
			    "Failed to get next ACL entry");
			return (ARCHIVE_WARN);
		}
	}
	return (ARCHIVE_OK);
}

int
archive_read_disk_entry_setup_acls(struct archive_read_disk *a,
    struct archive_entry *entry, int *fd)
{
	const char	*accpath;
	acl_t		acl;
	int		r;

	accpath = NULL;

	if (*fd < 0) {
		accpath = archive_read_disk_entry_setup_path(a, entry, fd);
		if (accpath == NULL)
			return (ARCHIVE_WARN);
	}

	archive_entry_acl_clear(entry);

	acl = NULL;

#if ARCHIVE_ACL_FREEBSD_NFS4
	/* Try NFSv4 ACL first. */
	if (*fd >= 0)
		acl = acl_get_fd_np(*fd, ACL_TYPE_NFS4);
	else if (!a->follow_symlinks)
		acl = acl_get_link_np(accpath, ACL_TYPE_NFS4);
	else
		acl = acl_get_file(accpath, ACL_TYPE_NFS4);

	/* Ignore "trivial" ACLs that just mirror the file mode. */
	if (acl != NULL && acl_is_trivial_np(acl, &r) == 0 && r == 1) {
		acl_free(acl);
		acl = NULL;
		return (ARCHIVE_OK);
	}

	if (acl != NULL) {
		r = translate_acl(a, entry, acl, ARCHIVE_ENTRY_ACL_TYPE_NFS4);
		acl_free(acl);
		acl = NULL;

		if (r != ARCHIVE_OK) {
			archive_set_error(&a->archive, errno,
			    "Couldn't translate NFSv4 ACLs");
		}

		return (r);
	}
#endif

	/* Retrieve access ACL from file. */
	if (*fd >= 0)
		acl = acl_get_fd_np(*fd, ACL_TYPE_ACCESS);
#if HAVE_ACL_GET_LINK_NP
	else if (!a->follow_symlinks)
		acl = acl_get_link_np(accpath, ACL_TYPE_ACCESS);
#else
	else if ((!a->follow_symlinks)
	    && (archive_entry_filetype(entry) == AE_IFLNK))
		/* We can't get the ACL of a symlink, so we assume it can't
		   have one. */
		acl = NULL;
#endif
	else
		acl = acl_get_file(accpath, ACL_TYPE_ACCESS);

#if HAVE_ACL_IS_TRIVIAL_NP
	/* Ignore "trivial" ACLs that just mirror the file mode. */
	if (acl != NULL && acl_is_trivial_np(acl, &r) == 0 && r == 1) {
		acl_free(acl);
		acl = NULL;
	}
#endif

	if (acl != NULL) {
		r = translate_acl(a, entry, acl, ARCHIVE_ENTRY_ACL_TYPE_ACCESS);
		acl_free(acl);
		acl = NULL;

		if (r != ARCHIVE_OK) {
			archive_set_error(&a->archive, errno,
			    "Couldn't translate access ACLs");
			return (r);
		}
	}

	/* Only directories can have default ACLs. */
	if (S_ISDIR(archive_entry_mode(entry))) {
		if (*fd >= 0)
			acl = acl_get_fd_np(*fd, ACL_TYPE_DEFAULT);
		else
			acl = acl_get_file(accpath, ACL_TYPE_DEFAULT);
		if (acl != NULL) {
			r = translate_acl(a, entry, acl,
			    ARCHIVE_ENTRY_ACL_TYPE_DEFAULT);
			acl_free(acl);
			if (r != ARCHIVE_OK) {
				archive_set_error(&a->archive, errno,
				    "Couldn't translate default ACLs");
				return (r);
			}
		}
	}
	return (ARCHIVE_OK);
}
