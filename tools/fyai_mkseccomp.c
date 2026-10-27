/*
 * fyai_mkseccomp.c - build the seccomp filter that denies UDP sockets
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * Usage: fyai_mkseccomp OUTPUT.inc
 *
 * Assemble the classic BPF program from labeled instructions and write it as
 * the C array fyai_seccomp_udp[]. The tool resolves each jump label to a
 * relative offset. Every operand stays a C expression that the target compiler
 * evaluates, so the output does not depend on the build host. The note of each
 * instruction goes to the output as a comment. Jumps go
 * forward only. A label that is missing, a backward jump or an offset above
 * 255 fails the build.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct insn {
	const char *note;	/* What the instruction does; copied to the output. */
	const char *label;	/* Label of this instruction, or NULL. */
	const char *code;	/* BPF opcode expression. */
	const char *k;		/* Operand expression. */
	const char *jt;		/* Label taken when true; NULL falls through. */
	const char *jf;		/* Label taken when false; NULL falls through. */
	int jump;
};

#define STMT(note, ...)		{ note, NULL, __VA_ARGS__, NULL, NULL, 0 }
#define LSTMT(note, label, ...)	{ note, label, __VA_ARGS__, NULL, NULL, 0 }
#define JUMP(note, code, k, jt, jf) { note, NULL, code, k, jt, jf, 1 }

#define ERR	"BPF_RET | BPF_K", "SECCOMP_RET_ERRNO | EPERM"
#define LOAD(field)	"BPF_LD | BPF_W | BPF_ABS", "offsetof(struct seccomp_data, " field ")"
#define JEQ	"BPF_JMP | BPF_JEQ | BPF_K"
#define JSET	"BPF_JMP | BPF_JSET | BPF_K"

/*
 * The program runs on the classic BPF machine of seccomp, for each syscall of
 * the process and its descendants. The machine has one 32-bit accumulator A.
 * The kernel gives the program a read-only struct seccomp_data: the syscall
 * number (nr), the architecture (arch) and six arguments (args[]).
 *
 *   LD   loads a word of seccomp_data into A.
 *   AND  clears the bits of A that the operand does not set.
 *   JEQ  compares A with the operand: equal, take the jt branch; else jf.
 *   JSET tests A against the operand as a bit mask: any bit in common takes
 *        jt; none takes jf.
 *   RET  ends the program with a verdict. The kernel applies the verdict to
 *        the syscall: SECCOMP_RET_ALLOW runs it, and SECCOMP_RET_ERRNO fails
 *        it with the errno in the low 16 bits, without running it.
 *
 * jt and jf count instructions to skip after the jump instruction itself:
 * 0 runs the next instruction. A jump can only go forward, so the kernel
 * knows that the program ends. The program must end with a RET on every
 * path; the kernel checks both.
 *
 * Every verdict is one of two: "deny" fails the syscall with EPERM, and
 * "allow" lets it run. Any syscall that no test names reaches "allow", so the
 * filter takes rights away and never adds one. The tool installs it after
 * PR_SET_NO_NEW_PRIVS, so neither the process nor a program that it runs can
 * remove the filter or drop below it.
 *
 * A word that is a macro below (FYAI_SECCOMP_ARCH, __NR_socket, AF_INET) is
 * a number of the target, which the C compiler of the target resolves.
 * The kernel compares the arguments of socket() in 32 bits, as they stand in
 * the register: socket(int domain, int type, int protocol).
 */
static const struct insn prog[] = {
	/* Check the architecture first: nr only means something for one ABI. */
	STMT("A = the audit architecture of the syscall entry that was used",
	     LOAD("arch")),
	JUMP("same architecture as the build: test the syscall; else deny. "
	     "A 32-bit or other-ABI process would reach socket() through a "
	     "different number, so it fails every syscall instead",
	     JEQ, "FYAI_SECCOMP_ARCH", "nr", "deny"),

	/* Reject the numbers of the same machine through another door. */
	LSTMT("A = the syscall number, with the x32 flag bit still in it",
	      "nr", LOAD("nr")),
	JUMP("x32 flag set: deny. On x86-64 the x32 ABI has the same arch but "
	     "numbers its own socket(), which the test below would not match. "
	     "The mask is 0 elsewhere, so this never matches",
	     JSET, "FYAI_SECCOMP_X32", "deny", NULL),
	JUMP("io_uring_setup(): deny. A ring runs IORING_OP_SOCKET, which makes "
	     "a socket and never calls socket(), so the test below would not "
	     "see it",
	     JEQ, "__NR_io_uring_setup", "deny", NULL),

	/* Only socket() makes a new socket here: every other syscall is allowed. */
	JUMP("socket(): look at its arguments; every other syscall: allow. "
	     "Sockets from socketpair() are Unix only and accept() makes a "
	     "stream; a datagram socket can only start at socket()",
	     JEQ, "__NR_socket", NULL, "allow"),

	/* The domain, the first argument. */
	STMT("A = domain, the first argument of socket()", LOAD("args[0]")),
	JUMP("AF_INET (IPv4): check the type",
	     JEQ, "AF_INET", "type", NULL),
	JUMP("AF_INET6 (IPv6): check the type; any other domain (AF_UNIX, "
	     "AF_NETLINK, ...): allow, because it carries no UDP",
	     JEQ, "AF_INET6", NULL, "allow"),

	/* The type, the second argument. */
	LSTMT("A = type, the second argument of socket()",
	      "type", LOAD("args[1]")),
	STMT("keep the low four bits: the type without the flags "
	     "SOCK_NONBLOCK and SOCK_CLOEXEC, which a caller can or into it. "
	     "Without this a flagged type would not match",
	     "BPF_ALU | BPF_AND | BPF_K", "0xf"),
	JUMP("SOCK_DGRAM: deny. With an IP domain this is UDP, UDP-Lite or "
	     "ICMP echo; the protocol argument cannot change that",
	     JEQ, "SOCK_DGRAM", "deny", NULL),
	JUMP("SOCK_RAW: deny, because a raw socket can build a UDP packet by "
	     "hand; any other type, such as SOCK_STREAM (TCP): allow",
	     JEQ, "SOCK_RAW", "deny", "allow"),

	/* The two verdicts. Every jump above ends at one of them. */
	LSTMT("verdict deny: the syscall fails with EPERM and does not run",
	      "deny", ERR),
	LSTMT("verdict allow: the syscall runs as if there were no filter",
	      "allow", "BPF_RET | BPF_K", "SECCOMP_RET_ALLOW"),
};

#define N (sizeof(prog) / sizeof(prog[0]))

static int resolve(const char *label, size_t from, int *offp)
{
	size_t i;

	if (!label) {
		*offp = 0;
		return 0;
	}
	for (i = from + 1; i < N; i++) {
		if (prog[i].label && !strcmp(prog[i].label, label)) {
			*offp = (int)(i - from - 1);
			return *offp > 255 ? -1 : 0;
		}
	}
	fprintf(stderr, "fyai_mkseccomp: no label '%s' after instruction %zu\n",
		label, from);
	return -1;
}

/* Write the note as a comment of at most 80 columns, hanging under the index. */
static void emit_note(FILE *fp, size_t index, const struct insn *in)
{
	char head[96], *word, *text, *save;
	size_t col, len;

	snprintf(head, sizeof(head), "%zu%s%s%s:", index, in->label ? " (" : "",
		 in->label ? in->label : "", in->label ? ")" : "");
	text = strdup(in->note);
	if (!text) {
		perror("strdup");
		exit(1);
	}
	fprintf(fp, "\n\t/* %s", head);
	col = 8 + 3 + strlen(head);
	for (word = strtok_r(text, " ", &save); word; word = strtok_r(NULL, " ", &save)) {
		len = strlen(word);
		if (col + 1 + len > 76) {
			fprintf(fp, "\n\t * ");
			col = 8 + 3;
		} else {
			fputc(' ', fp);
			col++;
		}
		fputs(word, fp);
		col += len;
	}
	free(text);
}

/* Index of the instruction that @label names after @from; -1 when none. */
static int target(const char *label, size_t from)
{
	size_t i;

	for (i = from + 1; label && i < N; i++)
		if (prog[i].label && !strcmp(prog[i].label, label))
			return (int)i;
	return -1;
}

/* Describe where a branch of the jump at @from goes. */
static void emit_branch(FILE *fp, size_t from, const char *what, const char *label)
{
	int to = label ? target(label, from) : (int)from + 1;

	if (label)
		fprintf(fp, "\n\t * %-5s jump to %d (%s), skip %d", what, to, label,
			to - (int)from - 1);
	else
		fprintf(fp, "\n\t * %-5s fall through to %d", what, to);
}

/* Write one entry of the list of incoming edges, wrapped at 80 columns. */
static void emit_edge(FILE *fp, size_t *col, const char *sep, size_t from,
		      const char *how)
{
	char buf[48];
	int len;

	len = snprintf(buf, sizeof(buf), "%s %zu (%s)", sep, from, how);
	if (*col + len > 76) {
		fprintf(fp, "\n\t *  ");
		*col = 8 + 4;
	}
	fputs(buf, fp);
	*col += len;
}

/*
 * List the instructions that continue at the labeled instruction @at: a jump
 * that names the label, and an instruction that falls through into it.
 * A RET has no successor.
 */
static void emit_incoming(FILE *fp, size_t at)
{
	const char *sep = "";
	size_t i, col;
	bool any = false;

	if (!prog[at].label)
		return;
	fprintf(fp, "\n\t * reached from:");
	col = 8 + 3 + 13;
	for (i = 0; i < at; i++) {
		if (!prog[i].jump) {
			if (i + 1 == at && strncmp(prog[i].code, "BPF_RET", 7)) {
				emit_edge(fp, &col, sep, i, "falls through");
				sep = ",";
				any = true;
			}
			continue;
		}
		if (prog[i].jt ? !strcmp(prog[i].jt, prog[at].label) : i + 1 == at) {
			emit_edge(fp, &col, sep, i, "true");
			sep = ",";
			any = true;
		}
		if (prog[i].jf ? !strcmp(prog[i].jf, prog[at].label) : i + 1 == at) {
			emit_edge(fp, &col, sep, i, "false");
			sep = ",";
			any = true;
		}
	}
	if (!any)
		fprintf(fp, " none");
}

int main(int argc, char **argv)
{
	FILE *fp;
	size_t i;
	int jt, jf;

	if (argc != 2) {
		fprintf(stderr, "usage: fyai_mkseccomp OUTPUT.inc\n");
		return 2;
	}
	fp = fopen(argv[1], "w");
	if (!fp) {
		perror(argv[1]);
		return 1;
	}
	fprintf(fp, "/* Generated by tools/fyai_mkseccomp.c. */\n"
		"#define FYAI_SECCOMP_UDP_GENERATED 1\n"
		"static struct sock_filter fyai_seccomp_udp[] = {\n");
	for (i = 0; i < N; i++) {
		emit_note(fp, i, &prog[i]);
		if (prog[i].jump) {
			emit_branch(fp, i, "true:", prog[i].jt);
			emit_branch(fp, i, "false:", prog[i].jf);
		}
		emit_incoming(fp, i);
		fprintf(fp, " */\n");
		if (!prog[i].jump) {
			fprintf(fp, "\tBPF_STMT(%s, %s),\n", prog[i].code, prog[i].k);
			continue;
		}
		if (resolve(prog[i].jt, i, &jt) || resolve(prog[i].jf, i, &jf)) {
			fclose(fp);
			remove(argv[1]);
			return 1;
		}
		fprintf(fp, "\tBPF_JUMP(%s, %s, %d, %d),\n", prog[i].code,
			prog[i].k, jt, jf);
	}
	fprintf(fp, "};\n");
	if (fclose(fp)) {
		perror(argv[1]);
		return 1;
	}
	return 0;
}
