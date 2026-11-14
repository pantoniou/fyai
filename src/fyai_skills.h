/*
 * fyai_skills.h - the skill files component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_SKILLS_H
#define FYAI_SKILLS_H

struct fyai_arena;
struct fyai_skill_dir;
struct fyai_transient;

/*
 * The files that this process wrote out of the skills of the branch: the
 * private directory under $TMPDIR, the skills written there, and the
 * directory that the settings tool exports to. The process owns them: cleanup
 * removes the trees, and a forked child forgets them. The component reads the
 * skills of the branch through the arena and builds values in the transient
 * builder. The owner is struct fyai_app.
 */
struct fyai_skills {
	/* Dependencies. */
	struct fyai_arena *arena;
	struct fyai_transient *transient;

	char *root;			/* the private directory, or NULL */
	char *export_dir;		/* the directory of the settings tool, or NULL */
	struct fyai_skill_dir *dirs;
};

void fyai_skills_init(struct fyai_skills *skills, struct fyai_arena *arena,
		      struct fyai_transient *transient);

#endif
