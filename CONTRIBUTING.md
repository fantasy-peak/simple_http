# Contributing Guide

## Editor Setup

### Generating `compile_commands.json`

These should be generated automatically when running `xmake build`. It's also possible to generate directly by running:

```sh
xmake project -k compile_commands --lsp=clangd build
```

This should create a `./build/compile_commands.json` file.

### Vscode

Developers using `vscode` need to tell their editor where to find the `compile_commands.json`.
Do this by editing `.vscode/c_cpp_properties.json`:

```diff
{
  "configurations": [
    {
      ...
+++   "compileCommands": ["${workspaceFolder}/build/compile_commands.json"]
    }
  ],
  ...
}
```
