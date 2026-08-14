<!-- meta:yaml
format: 1
kind: conversation
-->

<!-- meta:yaml
kind: publish
-->
## Publish

<!-- meta:yaml
kind: config
config:
  display:
    markdown: false
-->
## Configuration

<!-- meta:yaml
kind: publish
-->
## Publish

<!-- meta:yaml
kind: config-update
config-update:
  display/markdown: true
  display/stream: false
  api: chat-completions
  tools: true
  api_url: http://127.0.0.1:35739/v1/chat/completions
-->
## Configuration update

<!-- meta:yaml
kind: turn
-->
## Turn

<!-- meta:yaml
kind: message
role: system
-->
## System

You are a concise assistant.

Independent tool calls may be issued together in one response and will execute in parallel. Keep dependent or potentially conflicting tool calls in separate responses.

<!-- meta:yaml
kind: turn
-->
## Turn

<!-- meta:yaml
kind: message
role: user
-->
## User

do things

<!-- meta:yaml
kind: publish
-->
## Publish

<!-- meta:yaml
kind: turn
-->
## Turn

<!-- meta:yaml
kind: message
role: assistant
-->
## Assistant


<!-- meta:yaml
kind: tool_call
tool: read_file
arguments:
  path: hello.c
result: |
  int main(void)
  {
  	puts("old");
  	return 0;
  }
-->
### read_file

<!-- meta:yaml
kind: turn
-->
## Turn

<!-- meta:yaml
kind: turn
-->
## Turn

<!-- meta:yaml
kind: message
role: assistant
-->
## Assistant


<!-- meta:yaml
kind: tool_call
tool: write_file
arguments:
  path: note.txt
  content: 'a note
    
    second line
    
    '
result: ok
-->
### write_file

<!-- meta:yaml
kind: turn
-->
## Turn

<!-- meta:yaml
kind: turn
-->
## Turn

<!-- meta:yaml
kind: message
role: assistant
-->
## Assistant


<!-- meta:yaml
kind: tool_call
tool: apply_patch
arguments:
  patch: |
    --- a/hello.c
    +++ b/hello.c
    @@ -1,5 +1,5 @@
     int main(void)
     {
    -	puts("old");
    +	puts("new");
     	return 0;
     }
result: 'ok: 1 file changed'
-->
### apply_patch

<!-- meta:yaml
kind: turn
-->
## Turn

<!-- meta:yaml
kind: turn
-->
## Turn

<!-- meta:yaml
kind: message
role: assistant
-->
## Assistant


<!-- meta:yaml
kind: tool_call
tool: shell
arguments:
  command: echo one; echo two
  description: say things
result: 'one
  
  two
  
  '
-->
### shell — say things

<!-- meta:yaml
kind: turn
-->
## Turn

<!-- meta:yaml
kind: turn
-->
## Turn

<!-- meta:yaml
kind: message
role: assistant
-->
## Assistant

Prose with a code block:

```c
int x = 1;
```

Done.

