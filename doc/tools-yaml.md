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
      required:
        - path
      additionalProperties: false
```

Use these rules:

- Keep the tool order: `read_file`, `write_file`, `apply_patch`, `shell`,
  `ask_user`, `agent`.
- Write each description as a literal block scalar (`|-`) on one line. The
  provider receives the parsed text without a change.
- Give each parameter object `type: object`, `properties`, `required`, and
  `additionalProperties: false`.
- Property types are `string`, `integer`, or `array` of `string`.

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

- A sub-agent tool set drops `ask_user` and `agent`.
- A parent tool set adds the configured persona names and descriptions to the
  `agent` tool `persona` property description.

This logic is in C. `data/tools.yaml` does not contain context-dependent text.

## Limits

There is no schema for `data/tools.yaml`. `make_tools()` reports an error when
it cannot parse the document. The tests in `tests/fyai_tool_spec_test.c` check
the document shape, tool order, and description fields.
