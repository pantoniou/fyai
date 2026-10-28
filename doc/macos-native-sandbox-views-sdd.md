# Software Design Document: native macOS sandbox and project views

**Status:** Native filesystem sandbox implemented; materialized views proposed
**Date:** 2026-10-28
**Scope:** Native macOS tool execution and private writable project views
**Related designs:** [Project filesystem transactions](agent-filesystem-views-sdd.md),
[Agent transport isolation](agent-transport-isolation-sdd.md),
[Exchange scope](exchange-scope.md)

## 1. Purpose and decisions

Run the platform's native tools against a private project view. Commands must
use macOS executables, SDKs, libraries, and configured toolchains. A Linux guest
does not meet this requirement. The initial design adds no VM, FUSE filesystem,
privileged service, daemon, or system extension.

Use two separate mechanisms:

- Seatbelt confines the files, processes, IPC, and network operations that a
  tool can access.
- A materialized project tree holds private writable files. APFS file clones
  reduce its initial storage cost. Ordinary file copies provide the fallback.

A view is an alternate checkout, not an overlay mounted over the host project.
It has a different absolute path. Preserve the existing canonical project
model, manifests, content-addressed blobs, comparison, and apply policy.

The design does not claim equivalence to Linux mount and PID namespaces.
Unsupported guarantees must be reported and required isolation must fail
closed. Native command compatibility does not imply that every macOS service,
simulator, signing operation, or graphical application is supported.

## 2. Existing implementation and change boundary

`src/fyai_sandbox.c` holds the portable part of the sandbox and calls the
backend of the platform through `struct fyai_sandbox_backend`
(`src/fyai_sandbox_backend.h`). `src/fyai_sandbox_linux.c` implements Landlock,
`src/fyai_sandbox_macos.c` implements Seatbelt, and `src/fyai_sandbox_none.c`
confines nothing. The interfaces expose Landlock ABI and network capabilities.
These ABI values cannot describe Seatbelt capabilities.

`src/fyai_fsview.c` holds the shared manifest operations and calls the backend
of the platform through `struct fyai_fsview_backend`
(`src/fyai_fsview_backend.h`). `src/fyai_fsview_linux.c` has the namespace,
mount, arena-descriptor, and result-capture operations, and
`src/fyai_fsview_macos.c` has the native directory operations. A backend leaves
a member NULL when it does not support the operation. `src/fyai_cmd_fsview.c`
implements the view commands and session integration. The project capture,
manifest, CAS, and diff modules supply the persistent data model.

The Linux backend remains in use on Linux. Add platform-specific confinement
and materialization paths at these existing boundaries. Do not create a second
view registry, apply engine, agent execution path, or durable state format.
Shared functions must not require a namespace identifier or an OverlayFS upper
directory when the operation only needs a project root or manifest.

## 3. Guarantees and threat model

The trusted supervisor owns policy, capture, publication, and host application.
Tool programs and their descendants are untrusted. Model-supplied paths and
result trees are untrusted input. The kernel, installed toolchains, and trusted
fyai code are outside this threat model. A malicious same-user host process
with access to fyai's storage is not contained by this design.

An admitted isolated execution must meet these requirements:

1. Its initial files come from a pinned immutable manifest. Later host edits
   cannot change that baseline.
2. Writes are limited to its view, private scratch, and explicitly granted
   resources. A private view must not implicitly grant writes to the host
   project, canonical storage, or another view.
3. Tools cannot access arenas or credentials through paths, inherited
   descriptors, process inspection, IPC, or an unrestricted helper.
4. Every file tool and process launched for the execution uses the same view
   and effective policy. Changing only a shell's working directory is not
   sufficient.
5. A result is captured only after its writers have stopped. Published blobs
   and manifests are immutable and precede publication of their references.
6. Apply is a separate operation against the host's current state. It uses the
   existing conflict and automatic-application policy.
7. Helpers belong to the invocation. No persistent process or hidden process
   state is introduced.

File confinement, network confinement, descendant retirement, native view
execution, and protected credential transport are distinct capabilities.
Success in one does not establish the others. A diagnostic must identify the
capability that is missing rather than report only that sandboxing failed.

## 4. Seatbelt backend

### 4.1 Policy construction and launch

Translate the existing effective sandbox specification into a default-deny
Seatbelt profile. Keep policy selection and approval in the existing common
code. Only the platform backend emits Seatbelt syntax.

Apply the profile through `sandbox_init` at the existing pre-exec confinement
boundary, as section 8.1 describes. Supply eventual program arguments directly,
without an added shell. Keep the profile in process-owned memory. Escape
profile strings with a dedicated tested encoder; paths must never become
profile expressions. Profile compilation failure must stop execution rather
than truncate rules.

The launcher must sanitize the environment, close unrelated descriptors,
establish the working directory and standard streams, then enter confinement
before executing any untrusted program. Include PTY, signal, and exit-status
handling in the launch test. The native C backend applies the complete profile
once; a tool must not apply a second profile inside that domain.

Seatbelt's command-line and older API interfaces are deprecated. Treat their
availability and behavior as tested platform capabilities. Pin the supported
macOS release matrix to actual test results. Do not infer support from an OS
version string or the presence of `sandbox-exec` alone.

### 4.2 Filesystem and native toolchains

Grant read and execute access to the selected native toolchain and its required
system resources. Candidate paths include the selected Xcode developer
directory, command-line tools, system libraries, and configured Homebrew
prefixes. Resolve these from trusted configuration and the host environment;
do not assume every Mac uses `/opt/homebrew` or the default Xcode location.

Grant writes to the view and execution scratch. Put temporary files and build
caches there where the tool supports this configuration. Preserve native tool
discovery, including an approved `DEVELOPER_DIR`, while filtering inaccessible
PATH entries. Do not grant all of the real home directory to make a build pass.

The original project and both global and project-local canonical storage must
remain denied. Validate overlap between grants and denials. Test canonical
path aliases such as `/var` and `/private/var`, symlinks, hard links, ancestor
renames, and descriptor-relative access. If the requested grant cannot preserve
a mandatory denial, reject the policy.

Ordinary sandboxed execution outside a view can still grant writes to the host
project when its existing policy permits that. It must not be described as
transactional view execution.

### 4.3 Network, IPC, and nested execution

Implement and probe network policy independently of filesystem policy. The
first useful profile can deny network access. Admit existing port restrictions
only after their macOS semantics are tested, including IPv4, IPv6, TCP, UDP,
DNS, and loopback. Do not equate an HTTP proxy environment variable with an
enforced network restriction. Domain filtering and a new proxy are outside
the initial scope.

Mach services, Unix sockets, process inspection, and helper-mediated access
are part of confinement. Grant only services needed by tested workloads.
An unrestricted compiler service or launch service must not perform operations
on behalf of a tool that its profile denies. Signing and Keychain access need
separate explicit policy; ordinary compilation must not grant them.

Do not assume Seatbelt supports Landlock-style restriction stacking. The
supervisor must launch each independently confined execution from a context
that can apply its complete policy. A delegated agent requests launch through
the existing agent execution path; it cannot ask for a weaker policy. Test
tools that install their own sandbox and report unsupported nesting clearly.

## 5. Materialized project views

### 5.1 Baseline and writable tree

Use the recorded baseline manifest selected by the existing view lifecycle.
Do not silently recapture the live project on each tool call. An explicit
create or update retains its current capture semantics. Capture is not an
atomic snapshot of a changing host tree; retain its validation and retry rules.

The supervisor materializes the manifest into a private directory outside the
live project. Prefer a suitable location on the same filesystem as the source
blobs or verified materialized baseline so that file cloning can succeed.
The work tree and temporary files are disposable runtime data, not canonical
storage or a sidecar configuration database.

For each regular file, attempt a copy-on-write clone from verified immutable
bytes. Use `clonefile` or the appropriate `copyfile` clone mode. Fall back to
an ordinary independent copy for unsupported cloning or a cross-filesystem
source. Do not mask permission, I/O, or space errors as successful fallback.
Never hard-link a writable file to a canonical blob or sealed baseline.

Create directories and symbolic links from the manifest without following
links. Restore only metadata represented and permitted by the project model.
Do not blindly import ACLs, resource forks, extended attributes, or privilege
bits through a clone. The implementation must define and test their treatment
against the existing capture policy; unsupported required metadata is an error.
The manifest walker, rather than recursive copying of arbitrary storage,
determines which files enter a view.

Cloning saves file-data storage. It still requires directory entries and file
metadata. Initial materialization is proportional to the number of entries;
ordinary copying also costs the number of bytes copied. There is no claim of
constant-time tree snapshots.

### 5.2 Paths, scratch, and filesystem compatibility

Keep the logical host project root separate from the physical execution root.
Relative file operations resolve below the latter. The trusted file-tool
adapter may translate an explicit logical project path to its corresponding
view path after validation. Do not rewrite shell command strings, program
arguments, symlink targets, or file contents to simulate a mount namespace.

Inside a shell, `pwd` returns the actual view path. An absolute reference to
the original checkout is denied, not redirected. Relative symlinks retain
their bytes; an escaping symlink gains no additional permission. Absolute
symlinks into the host project remain unusable unless a separate policy grants
access, in which case baseline-only reads must no longer be claimed for them.

Build caches containing the old absolute root may need regeneration. Detect
Git worktree files and other external metadata references through normal
access checks; do not grant the referenced host repository automatically.
Diagnostics must give the logical project and physical execution roots when
the distinction explains a failure.

Use one execution-root resolver for project-relative policy grants, working
directories, direct file tools, and environment preparation. On Linux its
logical and physical roots can be equal. On macOS they differ. Keep a stable
physical directory for a named view across invocations where its exclusive
ownership and recovery rules permit reuse. Do not move an active execution
root. Its path is runtime location data, not part of canonical content identity.

Prepare the environment after credential sanitization and the configured
environment overlay, before access validation and launch:

- Translate an environment value that is an absolute project path, matching
  the root or a descendant at a directory boundary. Similar textual prefixes
  outside the project must not match.
- Translate entries separately for known path-list variables, including PATH,
  CPATH, and CMAKE_PREFIX_PATH. Use each variable's actual syntax; do not split
  arbitrary values on colons. Preserve ordering and validate empty entries
  against the existing environment and PATH policy.
- Set PWD from the physical working directory. Translate or remove OLDPWD.
  Set TMPDIR to execution scratch.
- Preserve external toolchain paths such as DEVELOPER_DIR and SDKROOT unless
  they point into the logical project. Their access still needs a grant.
- Do not replace substrings in URLs, shell fragments, compiler option strings,
  or arbitrary structured values. Such values require explicit configuration.

Give the agent the actual execution root for shell commands. Environment
translation cannot repair paths already stored in CMakeCache.txt, build.ninja,
generated Makefiles, or dependency files. Preserve those files and report a
path mismatch; let the normal build workflow reconfigure or clean the affected
output. Do not silently edit generated files or delete build directories.

Use a private `TMPDIR`; do not claim a private `/tmp` mount. A program that
ignores `TMPDIR` and writes to an ungranted shared temporary path must fail.
Do not broaden shared temporary access to conceal this incompatibility.
On macOS, `view/scratch_dir` selects the parent of private execution scratch;
it does not replace the selected directory with tmpfs.

Before launch, reject a manifest whose names cannot be represented distinctly
on the destination filesystem. Include case collisions and Unicode filename
behavior in this check. Never silently merge two entries. Use a compatible
volume when available; creating or mounting disk images is outside this design.

### 5.3 Result capture and application

There is no OverlayFS upper directory or whiteout format. After the writer
barrier, scan the materialized tree through the shared capture implementation.
Compare its manifest with the baseline to find additions, modifications, and
deletions. A full scan is the initial implementation. Filesystem notifications
are not a correctness source and are not required.

Use existing manifest and delta encodings where applicable. Keep backend names,
runtime paths, clone decisions, and filesystem observation data out of content
identity. Publish new blobs, then manifests, then arena references under the
existing durability policy. Report and preserve recoverable work when capture
or publication fails; never publish a partial result as complete.

Host application uses the baseline, completed view result, and current host
state with the existing three-way rules. Preserve ignore behavior and conflict
reporting. Automatic session application, if enabled, uses the same checks as
explicit apply and remains subject to the existing clean-exit policy.

## 6. Ownership, retirement, and agent integration

The invocation owns a trusted supervisor and any launch helpers. Start a view
execution only after materialization and confinement checks succeed. One
managed execution owns writes to a view at a time. Sub-agents either share its
managed execution scope under existing policy or receive an independent view.
Do not allow concurrent independent invocations to write one materialization.

The required lifecycle is:

1. Pin the baseline and acquire exclusive writer ownership.
2. Create the work tree and scratch; derive and validate the effective policy.
3. Launch the execution and register its lifetime before releasing it to run.
4. On completion or cancellation, stop new launches and retire all writers.
5. Establish that no writer remains, then capture and publish the result.
6. Apply only under the existing policy; release runtime storage and pins.

macOS process groups alone do not establish step 4. A descendant can detach or
change its process group. Polling a process tree can miss a fork or race PID
reuse. Neither mechanism is sufficient proof for result capture or a claim
that all descendants die when the invocation dies.

A platform prototype must establish a reliable mechanism for descendant
retirement, including double-fork, `setsid`, rapid spawning, supervisor death,
and indirect service launches. This is a release gate, not an assumed feature
of Seatbelt. If it cannot be established, native isolated views remain
experimental and unavailable to policies requiring that guarantee. Do not
capture a possibly live tree as a completed transaction.

On a crash, durable roots remain authoritative. An incomplete materialization
is not a published result. Recovery must validate ownership and establish
writer retirement before capture or deletion; a stale PID or pathname alone
is not sufficient. Cleanup must not follow tool-created symlinks. No automatic
recovery may write to the host project.

The Linux arena descriptor behind `/proc/self/fd` and non-dumpable-process
mechanism cannot be copied directly to macOS. Keep canonical storage in the
trusted supervisor for the initial standalone command prototype. Before
enabling isolated sessions or delegated agents, define and test the boundary
between the runtime that needs canonical state and its tools. Route direct file
tools through the same view and confinement boundary as shell operations.

Protected credential transport remains governed by the separate transport
design. This SDD does not establish XPC peer identity or signed-helper
requirements and does not enable protected mode on macOS by implication.

## 7. Interfaces, commands, and configuration

Use the existing sandbox specification as policy input. Add only the capability
queries required by the macOS implementation. Keep Linux ABI queries
Linux-specific; do not return a fabricated Landlock ABI for Seatbelt. Probe
confinement, network enforcement, clone support, and execution retirement
separately. A clone failure alone does not remove view support when a safe copy
is possible.

Reuse existing view commands for manifest operations and admitted execution.
`view mount` and `view unmount` keep their real mount semantics and remain
unsupported on macOS in this scope. A copied directory must not be advertised
as a read-only mount. Manifest-based diff and inspection remain usable without
a mount.

Retain the existing distinction between optional ordinary sandboxing and
required isolation. An explicit isolated view must fail closed even when a
general non-strict sandbox configuration could otherwise run unconfined.
No view failure may fall back to execution in the live checkout.

Do not add a user-selected backend or new command grammar in the first phase.
Update platform-specific descriptions in `data/commands.yaml`,
`data/config.schema.yaml`, and `config.yaml.sample` only as capabilities become
available. Regenerate command documentation when definitions change. Any new
configuration that widens access must have `x-fyai-model: read` or `hidden`.
Present capabilities, errors, and progress through the existing command result,
diagnostic, and sink paths.

## 8. Implementation plan

| Phase | Work | Exit condition |
| --- | --- | --- |
| 1. Platform probes | Seatbelt launch, file denial, native compiler access, clone/copy behavior, process retirement investigation | Evidence identifies supported guarantees and unresolved blockers |
| 2. Native tool sandbox | Policy translation, descriptor/environment handling, PTY and signal integration | Required restrictions fail closed; ordinary sandbox regression tests pass |
| 3. Standalone views | Manifest materialization, alternate root, full result scan, existing apply | Retirement gate passes; host remains unchanged until apply |
| 4. Sessions and agents | Direct file tools, runtime/arena separation, shared scopes and delegated execution | Every execution path uses its admitted view; no canonical-state exposure |
| 5. Supported release | Documentation, native workload matrix, performance measurements | Named macOS/architecture combinations pass the complete acceptance matrix |

The repeatable phase-one probe is `tests/macos/native_view_probe.py`. Run it
with `/usr/bin/python3` on the Mac. Its environment and profile-encoding checks
also run on Linux with `--self-check`. It needs CMake and Ninja and creates
only temporary project data. It is a compatibility experiment, not the
production launcher or environment resolver. It kills the detached child it
creates for the lifetime test.

Expected implementation sites are `fyai_sandbox.*`, `fyai_sandbox_profile.*`,
`fyai_fsview.*`, `fyai_cmd_fsview.c`, `fyai_tools.c`, the agent and session
integration, and the existing process-scope/event code. Shared capture and CAS
code needs a clone optimization only where independent destination ownership
is already required. Add build guards and tests in the existing build system.
Do not introduce a general backend framework before these concrete paths need
one.

### 8.1 Implemented native sandbox

`src/fyai_sandbox_macos.c` is the macOS sandbox backend. It uses `sandbox_init` in the tool child before exec. This replaces the
proposed sandbox-exec wrapper for the first implementation: the existing
confinement API applies to the current process and does not own its eventual
argument vector. No second launch path is needed. Both interfaces use the
deprecated Seatbelt mechanism and retain its compatibility limits.

The backend probes availability in a disposable child and leaves the Landlock
ABI value at zero. It emits escaped profile paths, grants native system and
toolchain reads, grants project and scratch writes, applies configured path
denials, and denies the project's `.fyai` directory. PATH filtering removes
relative and inaccessible entries. The native temporary directory is used
when TMPDIR and explicit scratch are absent.

The initial profile permits process operations, sysctl reads, Mach lookups,
filesystem metadata, and listing of the root directory itself. The literal
root rule is not a recursive filesystem read grant. Network access remains
open unless a supported restriction is implemented later. Requested network
restrictions, namespace mounts/covers, and edit/append-only modes are rejected
by the backend with ENOTSUP. Network capability queries return false. Existing
common profile handling can omit unsupported network policy with a warning;
the backend does not claim that such a profile restricts egress.

Configured denials take precedence over explicit grants in this first macOS
backend. Fine-grained explicit exceptions, mandatory-denial ancestor rename
protection, service mediation, and complete filesystem-alias enforcement
remain feature gaps. This is filesystem tool confinement, not the complete
protected agent mode or transactional-view implementation.

`tests/macos/sandbox_backend_test.c` is registered as
`fyai/macos/sandbox` by CMake on macOS. It tests actual C backend application,
project writes, denied secrets and arena access, unrelated-file read denial,
PATH filtering, profile control-character rejection, unsupported policy
rejection, and compilation with the selected native clang and macOS SDK.
Assertions remain enabled in release builds. The test passes on macOS, and
fyai builds there with the backend linked.

The cc/xcrun wrapper can attempt to write its discovery cache outside TMPDIR.
Under private scratch this can print cache-denial diagnostics. The compiler
test resolves clang and the SDK before confinement and supplies `-isysroot`
explicitly. Automatic native toolchain discovery for view executions remains
part of the materialized-view integration.

## 9. Acceptance tests

Run security and native-tool tests on real macOS hosts. A Linux mock of profile
generation cannot establish macOS enforcement. Record OS release, architecture,
filesystem case behavior, and selected toolchain for each run.

| Area | Required checks |
| --- | --- |
| Filesystem policy | Allowed read/write/execute; denied host, arena, credentials, sibling views; symlink and hard-link aliases; ancestor rename; inherited descriptors |
| Profile construction | Quotes, backslashes, newlines, non-ASCII paths, long rule sets, injection attempts, unavailable launcher, rejected profile |
| Clones and copies | Independent writes; immutable baseline; cross-volume fallback; unsupported volume; disk full during clone write or capture; correct executable modes |
| Project fidelity | Nested trees, deletions, renames, symlinks, empty files, arbitrary admitted names, case collisions, metadata policy, reserved `.fyai` |
| Baseline stability | Concurrent host edits do not affect a running view; create/update retains capture consistency checks |
| Native tools | Selected Xcode/command-line clang, SDK lookup, CMake/Ninja, Git, Homebrew tools, a representative unsigned `xcodebuild` build with private output/cache paths |
| IPC and network | Denied direct sockets and service bypasses; every advertised TCP/UDP restriction; required local services under narrow grants |
| Lifetime | Double-fork, `setsid`, rapid forks, ignored signals, cancellation, terminal closure, parent and supervisor death, PID reuse, no writer at capture |
| Integration | Shell, direct read/write/patch, terminal jobs, configured programs, delegated agents, and session execution use the admitted root |
| Publication/apply | Failure before each publish stage; recovery; concurrent host edits; conflict refusal; clean-exit versus interrupted automatic apply |
| Platform regression | Linux confinement and OverlayFS semantics unchanged; unsupported macOS requests fail before execution |

Measure materialization time, allocated blocks, peak memory, and result-scan
time for many small files and a smaller number of large files. Include ignored
build output according to current view policy. Report cold and warm runs and
clone versus copy behavior. Do not adopt an optimization that changes canonical
identity or weakens result validation.

## 10. Open decisions and release gates

- Establish descendant retirement and a writer barrier on macOS. This is the
  main blocker for transactional execution, including crash recovery.
- Establish runtime-to-tool arena separation before session and agent support.
- Determine the minimum native service grants for supported build workloads;
  signing, simulators, and GUI automation are not initial requirements.
- Verify Seatbelt network semantics before advertising existing port policies.
- Define metadata handling for clones against the shared project model.
- Select supported macOS releases and Intel/Apple Silicon combinations from
  test evidence. Native cloning and confinement must not be gated on Apple
  Silicon solely because VM alternatives require it.

### 10.1 Initial native probe

On 2026-10-28, the probe ran on a host with macOS 27.0.1, build 26A434, arm64,
and `/Applications/Xcode.app/Contents/Developer` as its selected developer
directory. CMake and Ninja were available under `/opt/homebrew/bin`. The
compiler identified itself as AppleClang 21.0.0.21000334.

The probe used a temporary project outside the checkout and removed it on
exit. It copied the source and a configured CMake build directory with
`cp -cR`, changed a header in the copy, and verified that the original header
did not change. The shell entered the copied directory, with CPATH and
PROJECT_ROOT set to that directory and TMPDIR set to private scratch. These
environment values were set explicitly; no automatic resolver was tested.

| Probe | Observed result |
| --- | --- |
| Read original project under Seatbelt | Denied |
| Write original project under Seatbelt | Denied |
| Configure using copied CMake cache | Rejected because source and build paths differed |
| Configure and compile in a fresh view build directory | Passed with native AppleClang, CMake, and Ninja |
| Execute compiled program | Passed |
| Repeat build at the same physical path | Passed; Ninja reported no work |
| Apply a second sandbox inside the first | Rejected with `sandbox_apply: Operation not permitted` |

The test profile used default deny, allowed process operations, sysctl reads,
Mach lookups, and general file reads, allowed writes only to the view, scratch,
and `/dev/null`, and explicitly denied reads and writes below the original
project. Its broad read and Mach permissions make it a compatibility probe,
not a production confinement profile. The test did not measure shared blocks,
compile fyai itself, build an Xcode project, test automatic environment
translation, or establish descendant retirement, network policy, credential
protection, or adversarial path confinement. All corresponding release gates
remain open.

### 10.2 Repeatable probe and implementation gate

The repeatable probe subsequently passed on the same host. It exercised actual
environment translation, including CPATH, and verified root-boundary matching,
path-list translation, preservation of option strings, and profile quoting.
The native configure, compile, execution, and incremental build passed with
the translated environment. The existing fyai checkout also built successfully
with `cmake --build build -j4`; this was a baseline build, not a build with a
new confinement backend.

A compiled native child called `setsid`, closed its standard descriptors, and
announced readiness to its parent through a pipe. After the parent and
`sandbox-exec` exited, the child remained alive. The probe explicitly killed
that child. Thus launcher exit is not a writer barrier. The prototype must
not infer completed-view state from that exit or enable transactional views
on this basis. A supported execution-retirement mechanism remains required.

The maintained probe still uses broad file-read and Mach-service permissions.
It does not establish the production profile, automatic command integration,
network confinement, adversarial alias protection, or runtime arena separation.
Phase one has evidence for the storage/build approach and a confirmed lifetime
gap. The user accepted deferring feature gaps while implementation continues.
Section 8.1 records the implemented portion of phase two. View execution will
initially document its descendant-retirement limit instead of claiming a
verified writer barrier. Clone-backed command integration, session integration,
and protected transport remain pending.

## 11. Research references

The sources below informed this proposal. Project documentation describes
available mechanisms; it is not evidence that fyai meets its release gates.

- [Apple `clonefile(2)` source manual](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/man/man2/clonefile.2):
  independent copy-on-write file data, filesystem limits, and the recommendation
  to use `copyfile` for directory trees.
- [Apple `copyfile(3)` source manual](https://github.com/apple-oss-distributions/copyfile/blob/main/copyfile.3):
  recursive per-entry cloning, copy fallback, symlink handling, and metadata.
- [Anthropic Sandbox Runtime](https://github.com/anthropic-experimental/sandbox-runtime):
  a reference implementation using dynamically generated Seatbelt profiles on
  macOS. It is not a proposed fyai dependency.
- [Apple developer discussion of sandbox-exec](https://developer.apple.com/forums/thread/661939):
  context for the deprecated interface; deployment still requires direct
  compatibility and enforcement tests.

VM alternatives considered during research include
[libkrun](https://github.com/libkrun/libkrun),
[Apple Containerization](https://github.com/apple/containerization), and
[NVX](https://github.com/microsoft/nvx). Linux guest execution does not satisfy
the native macOS tooling requirement. A macOS guest would introduce a separate
OS installation and toolchain environment and is outside this feature.
