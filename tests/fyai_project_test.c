/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>

#include "fyai_project.h"
#include "fyai_test.h"
#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(project, directory_order, project_directory_order)
FYAI_TEST_ENTRY(project, directory_ancestors, project_directory_ancestors)
FYAI_TEST_ENTRY(project, directory_validation, project_directory_validation)

static struct fy_generic_builder *project_builder(void)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};

	return fy_generic_builder_create(&cfg);
}

static void project_entry(struct fyai_project_entry *entry, const char *name,
			  enum fyai_project_kind kind, char value)
{
	entry->name = (const unsigned char *)name;
	entry->name_length = strlen(name);
	entry->kind = kind;
	memset(entry->digest, value, FYAI_CAS_DIGEST_SIZE - 1);
	entry->digest[FYAI_CAS_DIGEST_SIZE - 1] = '\0';
}

int project_directory_order(void)
{
	struct fy_generic_builder *gb;
	struct fyai_project_metadata meta = { .mode = 0755, .mtime_sec = -1 };
	struct fyai_project_entry entries[3], reverse[3];
	char first[FYAI_CAS_DIGEST_SIZE], second[FYAI_CAS_DIGEST_SIZE];
	fy_generic a, b, children, item;

	gb = project_builder();
	FYAI_TCHECK(gb);
	project_entry(&entries[0], "z", FYAI_PROJECT_FILE, 'a');
	project_entry(&entries[1], "\xff", FYAI_PROJECT_SYMLINK, 'b');
	project_entry(&entries[2], "a", FYAI_PROJECT_DIRECTORY, 'c');
	reverse[0] = entries[2];
	reverse[1] = entries[1];
	reverse[2] = entries[0];
	a = fyai_project_directory(gb, &meta, entries, 3, true, first);
	b = fyai_project_directory(gb, &meta, reverse, 3, true, second);
	FYAI_TCHECK(fy_is_valid(a) && fy_is_valid(b));
	FYAI_TCHECK(fy_equal(a, b) && !strcmp(first, second));
	FYAI_TCHECK(fy_is_invalid(fy_get(a, "blob", fy_invalid)));
	FYAI_TCHECK(fy_is_invalid(fy_get(a, "content", fy_invalid)));
	children = fy_get(a, "entries", fy_invalid);
	item = fy_get_at(children, 0);
	FYAI_TCHECK(fy_equal(fy_get(item, "name_hex", ""), "61"));
	item = fy_get_at(children, 2);
	FYAI_TCHECK(fy_equal(fy_get(item, "name_hex", ""), "ff"));
	memset(entries, 0, sizeof(entries));
	FYAI_TCHECK(fy_equal(a, b));
	fy_generic_builder_destroy(gb);
	return 0;
}

int project_directory_ancestors(void)
{
	struct fy_generic_builder *gb;
	struct fyai_project_metadata meta = { .mode = 0755 };
	struct fyai_project_entry leaf, parent;
	char child1[FYAI_CAS_DIGEST_SIZE], child2[FYAI_CAS_DIGEST_SIZE];
	char root1[FYAI_CAS_DIGEST_SIZE], root2[FYAI_CAS_DIGEST_SIZE];
	fy_generic result;

	gb = project_builder();
	FYAI_TCHECK(gb);
	project_entry(&leaf, "file", FYAI_PROJECT_FILE, 'a');
	result = fyai_project_directory(gb, &meta, &leaf, 1, false, child1);
	FYAI_TCHECK(fy_is_valid(result));
	project_entry(&parent, "src", FYAI_PROJECT_DIRECTORY, '0');
	memcpy(parent.digest, child1, sizeof(parent.digest));
	result = fyai_project_directory(gb, &meta, &parent, 1, true, root1);
	FYAI_TCHECK(fy_is_valid(result));
	leaf.digest[0] = 'b';
	result = fyai_project_directory(gb, &meta, &leaf, 1, false, child2);
	FYAI_TCHECK(fy_is_valid(result) && strcmp(child1, child2));
	memcpy(parent.digest, child2, sizeof(parent.digest));
	result = fyai_project_directory(gb, &meta, &parent, 1, true, root2);
	FYAI_TCHECK(fy_is_valid(result) && strcmp(root1, root2));
	result = fyai_project_directory(gb, &meta, NULL, 0, true, root1);
	FYAI_TCHECK(fy_is_valid(result));
	FYAI_TCHECK(!strcmp(root1,
		"863a5e73f50868cecdcde4ec3d39def7704caae5d4f6d59e86d80ad0fa00c9ca"));
	meta.mode = 0700;
	result = fyai_project_directory(gb, &meta, NULL, 0, true, root2);
	FYAI_TCHECK(fy_is_valid(result) && strcmp(root1, root2));
	fy_generic_builder_destroy(gb);
	return 0;
}

int project_directory_validation(void)
{
	static const char *const bad_names[] = { "", ".", "..", "a/b", ".fyai" };
	struct fy_generic_builder *gb;
	struct fyai_project_metadata meta = { .mode = 0755 };
	struct fyai_project_entry entries[2];
	char digest[FYAI_CAS_DIGEST_SIZE];
	fy_generic result;
	size_t i;

	gb = project_builder();
	FYAI_TCHECK(gb);
	for (i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); i++) {
		project_entry(&entries[0], bad_names[i], FYAI_PROJECT_FILE, 'a');
		result = fyai_project_directory(gb, &meta, entries, 1, true, digest);
		FYAI_TCHECK(fy_is_invalid(result) && errno == EINVAL);
	}
	project_entry(&entries[0], ".fyai", FYAI_PROJECT_DIRECTORY, 'a');
	result = fyai_project_directory(gb, &meta, entries, 1, false, digest);
	FYAI_TCHECK(fy_is_valid(result));
	project_entry(&entries[0], "same", FYAI_PROJECT_FILE, 'a');
	entries[1] = entries[0];
	result = fyai_project_directory(gb, &meta, entries, 2, true, digest);
	FYAI_TCHECK(fy_is_invalid(result) && errno == EEXIST);
	entries[0].name = (const unsigned char *)"a\0b";
	entries[0].name_length = 3;
	result = fyai_project_directory(gb, &meta, entries, 1, true, digest);
	FYAI_TCHECK(fy_is_invalid(result) && errno == EINVAL);
	project_entry(&entries[0], "ok", FYAI_PROJECT_FILE, 'g');
	result = fyai_project_directory(gb, &meta, entries, 1, true, digest);
	FYAI_TCHECK(fy_is_invalid(result) && errno == EINVAL);
	meta.mtime_nsec = 1000000000;
	result = fyai_project_directory(gb, &meta, NULL, 0, true, digest);
	FYAI_TCHECK(fy_is_invalid(result) && errno == EINVAL);
	fy_generic_builder_destroy(gb);
	return 0;
}
