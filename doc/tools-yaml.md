# Built-in tool specifications

The `data/tools.yaml` file defines the built-in tools. The file contains the
tool names, descriptions, and parameter schemas. CMake embeds the file in the
binary. The `make_tools()` function in `src/fyai_tool_spec.c` parses the file
at run time.

The provider, the `fyai catalog` output, and the tool tests use this data. To
change a tool description, edit `data/tools.yaml`.

## Document shape

The document is a sequence of tools in wire shape:

```yaml
- type: function
  function:
    name: read_file
    description: |-
      Read a UTF-8 text file from the workspace.
    parameters:
      type: object
      properties:
        path:
          type: string
          description: |-
            Workspace-relative path to read.
        offset:
          type: integer
          description: |-
            Optional 1-based line to start at. Use it with the offset the result reports to read on through a large file.
        offset_bytes:
          type: integer
          description: |-
            Optional zero-based byte offset. It takes precedence over offset and lets a later call continue after a byte-limited result.
        limit:
          type: integer
          description: |-
            Optional number of lines to return. Omit to read to the end of the file.
        max_bytes:
          type: integer
          description: |-
            Optional cap on the bytes returned. A larger window is truncated and the result says so. Omit for the configured default.
      required:
        - path
      additionalProperties: false
```

Use these rules:

- Keep the tool order: `read_file`, `write_file`, `apply_patch`, `exec_command`,
  `shell_input`, `shell_output`, `shell_close`, `ask_user`, `agent`, `list`,
  `time`, `wait`.
- Write each description as a literal block scalar (`|-`) on one line. The
  provider receives the parsed text without a change.
- Give each parameter object `type: object`, `properties`, `required`, and
  `additionalProperties: false`.
- Property types include `string`, `integer`, `boolean`, and `array` of
  `string`.

## Placeholders

A description can hold `{{name}}`. When the tools are built for a run, each
name is replaced by the text that the run needs, so the model learns what the
run can do.

The run decides the state of a name, and the tool holds the text of each
state under `templates`, beside its description:

```yaml
- type: function
  function:
    name: agent
    description: |-
      ... {{project_isolation_description}}
  templates:
    project_isolation_description:
      default: >-
        Text for a run where the option is on.
      optional: >-
        Text for a run where the option can be chosen.
```

`fyai_tool_template_state()` in `src/fyai_tool_template.c` lists the names and
gives the state of each. A name that has no text in this run, or in a state
that the tool does not list, stands for nothing, and the blank it leaves is
removed. The `templates` key is not sent to the provider.

| Name | States |
| --- | --- |
| `project_isolation_description` | `default`: a sub-agent runs on a private copy of the project unless told otherwise (`agent/isolation: view`). `optional`: it does when asked (`none`). It has no state when the run cannot isolate: then the `isolated` parameter and the `project_view` tool are not offered either. |

A unit test fails when a description names a placeholder that the table does
not list.

## Embedding

`CMakeLists.txt` reads `data/tools.yaml` as hexadecimal data. It writes the
`embedded_tools.inc` file in the build directory. This file supplies
`FYAI_EMBEDDED_TOOLS[]` and `FYAI_EMBEDDED_TOOLS_LEN`. The build uses the same
method for `data/catalog.yaml` and `data/config.schema.yaml`. The tool file is
a CMake configure dependency. Thus, a change to the file runs CMake again.

`make_tools()` parses the embedded bytes into the configuration builder. The
builder keeps the data for the life of the configuration. The context caches
the result from `make_tools_filtered()`. A configuration generation change
invalidates this cache. This change lets the tool description show new persona
settings.

## Filtering

`make_tools_filtered()` in `src/fyai_tool_spec.c` adapts the parsed tools to
the context.

- A sub-agent tool set drops `agent` and `agent_input`; it keeps `ask_user`.
- A parent tool set adds the configured persona names and descriptions to the
  `agent` tool `persona` property description.

This logic is in C. `data/tools.yaml` does not contain context-dependent text.

## Limits

There is no schema for `data/tools.yaml`. `make_tools()` reports an error when
it cannot parse the document. The tests in `tests/fyai_tool_spec_test.c` check
that descriptions are non-empty and check selected property types. They do not
validate the full document shape, exact tool order, every `required` list, or
`additionalProperties`.
