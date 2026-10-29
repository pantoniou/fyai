# fyai on macOS

This guide tells you how to install fyai on a Mac and how to give it an
Anthropic API key. For everything else, see the [user guide](user-guide.md).

Each release has two macOS assets for Apple silicon (arm64):

| Asset                      | Contents                                         |
| -------------------------- | ------------------------------------------------ |
| `fyai-macos-arm64.tar.gz`  | the `fyai` command-line program                  |
| `fyai-app-macos-arm64.zip` | `fyai.app`, the desktop app, with its own `fyai` |

Download them from the
[releases page](https://github.com/pantoniou/fyai/releases): a `vX.Y.Z`
release, or `nightly` for the newest build of `master`. There is no Intel
build.

## Gatekeeper

The release is not signed with an Apple Developer ID and is not notarized. The
app and the command-line program have only an ad hoc signature.
A file that a browser downloads gets the quarantine mark
(`com.apple.quarantine`), and Gatekeeper refuses to start a quarantined program
that is not notarized. The message says "Apple could not verify “NAME” is free
of malware that may harm your Mac or compromise your privacy."

Do one of these steps one time for each download:

- Allow the program in System Settings > Privacy & Security, after macOS
  blocks it one time. The sections below show each dialog. According to
  Apple, a right click on the app and Open also does this on macOS 14 and
  earlier. macOS 15 and later do not have that shortcut.
- Remove the quarantine mark with `xattr`, as the sections below show.

`curl` and `gh release download` do not set the quarantine mark. A file that
they download needs neither step:

```sh
gh release download nightly --repo pantoniou/fyai \
	--pattern 'fyai-macos-arm64.tar.gz' --pattern 'fyai-app-macos-arm64.zip'
```

To see whether a file has the mark:

```sh
xattr -l FILE
```

## Install the app

1. Unpack the archive. Finder does this when you double-click it, or:

   ```sh
   ditto -x -k fyai-app-macos-arm64.zip .
   ```

2. Move `fyai.app` to `/Applications`.

3. Allow the app with System Settings or with Terminal, as the next two
   sections show.

4. In the app, File > Open Project selects the project directory. When the
   app starts with no project, it uses
   `~/Library/Application Support/fyai/project`.

### Open the app with System Settings

1. Double-click `fyai.app`. macOS blocks it and shows "“fyai” Not Opened".
   Click Done. Do not click Move to Trash.

   <img src="img/macos-app-blocked.png" width="560" alt="“fyai” Not Opened: Apple could not verify “fyai” is free of malware">

2. Open the Apple menu and select System Settings.

3. Click Privacy & Security in the sidebar.

4. Scroll down to Security. A message says "“fyai” was blocked to protect
   your Mac." Click Open Anyway.

   <img src="img/macos-open-anyway.png" width="660" alt="Privacy & Security: “fyai” was blocked to protect your Mac, with Open Anyway">

5. macOS shows "Open “fyai”?". Click Open Anyway. Do not click Move to Trash.

   <img src="img/macos-open-anyway-2.png" width="560" alt="Open “fyai”?, with Move to Trash, Open Anyway and Done">

6. macOS asks for Touch ID, or for the name and password of an administrator.
   Give one of them. The app then starts.

   <img src="img/macos-open-confirm.png" width="560" alt="Touch ID or an administrator password to continue with Privacy & Security">

macOS keeps this exception, so later starts of this copy of the app are not
blocked. According to Apple, Open Anyway shows for about one hour after the
blocked start. If you do not see it, double-click the app again, or use the
Terminal command.

### Open the app with Terminal

Remove the quarantine mark from the bundle and from all files in it:

```sh
xattr -dr com.apple.quarantine /Applications/fyai.app
```

Replace the path when the app is in a different directory. Then open the app
as usual: macOS shows no warning.

The app uses its own copy of fyai, `fyai.app/Contents/Resources/fyai`. You do
not have to install the command-line program for the app. `web/README.fyai.md` describes the app.

## Install the command-line program

1. Unpack the archive into `~/.local/bin`. It holds one file,
   `fyai-macos-arm64`:

   ```sh
   mkdir -p ~/.local/bin
   tar -xzf fyai-macos-arm64.tar.gz -C ~/.local/bin
   ```

   `tar` copies the quarantine mark of the archive to the file that it
   unpacks.

2. Remove the quarantine mark:

   ```sh
   xattr -d com.apple.quarantine ~/.local/bin/fyai-macos-arm64
   ```

3. Make a symbolic link with the name `fyai`:

   ```sh
   ln -sf fyai-macos-arm64 ~/.local/bin/fyai
   ```

   A new release replaces `fyai-macos-arm64`, and the link stays. Remove the
   quarantine mark from each new file.

   You can copy the program instead of a link. Unpack it in a different
   directory, remove the quarantine mark there, and then do one of these
   steps:

   ```sh
   install -m 0755 fyai-macos-arm64 ~/.local/bin/fyai          # this user
   sudo install -m 0755 fyai-macos-arm64 /usr/local/bin/fyai   # all users
   ```

   `/usr/local/bin` is in the default `PATH`, so step 4 is not necessary for
   it.

4. macOS does not put `~/.local/bin` in `PATH`. Add it to `~/.zshrc`:

   ```sh
   export PATH="$HOME/.local/bin:$PATH"
   ```

5. Make sure that the program starts:

   ```sh
   fyai --help
   ```

If the shell reports `killed` and the exit status is 137, macOS stopped the
program. The terminal shows no other message. There are two possible causes:

- The file still has the quarantine mark. Find it with
  `xattr -l ~/.local/bin/fyai-macos-arm64`, and remove it as step 2 shows.
- The kernel refused the code signature. This occurs when a tool changed the
  file after the build. Give the file a new ad hoc signature:

  ```sh
  codesign --force --sign - ~/.local/bin/fyai-macos-arm64
  ```

### Allow the program with System Settings

In place of step 2, System Settings can allow the program. Do steps 1, 3 and
4 of the installation first, so that `fyai` is in your `PATH`:

1. Run `fyai`. macOS shows "“fyai-macos-arm64” Not Opened", and the shell
   reports `killed`. Click Done. Do not click Move to Trash.

   <img src="img/macos-cli-blocked.png" width="460" alt="“fyai-macos-arm64” Not Opened, with Move to Trash and Done">

2. Open System Settings > Privacy & Security, and scroll down to Security. A
   message says "“fyai-macos-arm64” was blocked to protect your Mac." Click
   Allow Anyway.

   <img src="img/macos-cli-allow-anyway.png" width="660" alt="Privacy & Security: “fyai-macos-arm64” was blocked to protect your Mac, with Allow Anyway">

3. Run `fyai` again. macOS shows "Open “fyai-macos-arm64”?". Click Open
   Anyway. Do not click Move to Trash.

   <img src="img/macos-cli-open-anyway.png" width="560" alt="Open “fyai-macos-arm64”?, with Move to Trash, Open Anyway and Done">

4. macOS asks for Touch ID, or for the name and password of an administrator,
   as for the app. Give one of them. The program then starts, and later runs
   show no dialog.

   <img src="img/macos-cli-confirm.png" width="560" alt="Touch ID or an administrator password to continue with Privacy & Security">

The release program links its dependencies statically. It needs only the
system libraries, so Homebrew is not necessary to run it. To build fyai
yourself, see [Build](user-guide.md#build).

## Anthropic API key

Make a key on the API keys page of the Claude Console,
<https://platform.claude.com/settings/keys>. fyai looks for a key in this
order:

1. the environment variable `ANTHROPIC_API_KEY`;
2. the secret `api-key/anthropic` in the macOS Keychain.

Do not write the key into a configuration file. fyai refuses a raw key in
stored configuration; it accepts only a reference such as
`api_key: {type: env, value: ANTHROPIC_API_KEY}`.

### Keychain

The Keychain is the recommended store. Both the app and the command-line
program read it, and the key stays when the Mac restarts.

```sh
fyai secret set api-key/anthropic      # asks for the key; shows nothing
fyai secret status api-key/anthropic   # tells whether the key is set
fyai secret                            # names the backend: macOS keychain
```

`fyai secret set NAME --stdin` reads the key from standard input, for example
from a password manager. fyai never takes the key as an argument.

fyai keeps the key as a generic password with the service `org.fyai.Secret`
and the account `fyai:api-key/anthropic`. Keychain Access shows it under the
name `org.fyai.Secret`.

The app holds the same `fyai` program as the command-line release of the same
version, so each of them reads a key that the other stored. A different build
of fyai, such as a new release, makes macOS ask for permission to use the item.
Enter the password of your login keychain, normally your Mac password, and
click Always Allow, so that macOS does not ask again for that build. Allow
gives access one time. If you click Deny, `fyai secret status` reports the key
as `unavailable`. It reports a store that does not work in the same way.

<img src="img/macos-keychain-access.png" width="820" alt="fyai wants to use your confidential information stored in “org.fyai.Secret” in your keychain, with Always Allow, Deny and Allow">

A key that you enter in the app, in Settings > General > Provider keys, goes
into the same Keychain item.

### Remove the key

> **WARNING: This command removes the API key from the Keychain.** It does
> not ask for confirmation, and you cannot undo it. The app and the
> command-line program both lose the key. To use Anthropic again, store the
> key again with `fyai secret set api-key/anthropic`. Keep a copy of the key
> in a different place, or make a new key in the Claude Console.

```sh
fyai secret delete api-key/anthropic
```

The command removes only the copy in the Keychain. The key stays valid at
Anthropic. To stop all use of the key, revoke it in the Claude Console.

### Environment variable

```sh
export ANTHROPIC_API_KEY=sk-ant-...
```

Put the line in `~/.zshrc` to keep it for each terminal. An app that Finder or
the Dock starts does not read `~/.zshrc`, so `fyai.app` does not see the
variable. Use the Keychain for the app.

### Select a Claude model

```sh
fyai list models                        # the models of the catalogue
fyai -m claude-sonnet-5.5 "hello"       # one invocation
fyai config set model claude-sonnet-5.5 # store it on the current branch
```

`fyai list models` shows the name `claude-sonnet-5.5`. The Anthropic name,
`claude-sonnet-5-5`, selects the same model.

Without a model setting, fyai uses `gpt-5.4-mini`, and it then asks for an
OpenAI key. To use Claude in every project, do one of these steps:

- Store the model in each project, one time:

  ```sh
  fyai config set model claude-sonnet-5.5
  ```

  `fyai --set model=claude-sonnet-5.5` also stores it.

- Write the model into the user configuration file,
  `~/.config/fyai/config.yaml`:

  ```yaml
  model: claude-sonnet-5.5
  ```

  fyai reads this file only for a project that has no stored configuration,
  and `fyai init` stores one. Thus load the file with `-C` on each
  invocation. An alias in `~/.zshrc` does this for you:

  ```sh
  alias fyai='fyai -C ~/.config/fyai/config.yaml'
  ```

  The alias works only in a shell. The app does not use it.

## Features that are not available

These features need the Linux kernel and are not available on macOS:

- the sandbox (`--sandbox`, sandbox profiles), which uses Landlock and
  namespaces;
- project views (chapter 8 of the user guide);
- the kernel secret store. The Keychain replaces it.

`/session lockdown` takes the strongest isolation that the host can enforce.
On macOS that is less than on Linux: it gives no sandbox and no view.

The default sandbox profile, `dev`, confines nothing on macOS. Each model
request still writes warnings such as these:

```text
config: warning: sandbox 'dev': TCP egress remains open; Landlock ABI 4 is unavailable
config: warning: sandbox 'dev': UDP egress remains open; Landlock ABI 10 is unavailable
```

Two more warnings tell you to install an AppArmor profile or bubblewrap. These
fixes are for Linux only. To stop the warnings, select the profile `normal`
for the project. This removes no protection on macOS:

```sh
fyai config set sandbox_profile normal
```
