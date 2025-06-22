# buxn-ls

[![Build status](https://github.com/bullno1/buxn-ls/actions/workflows/build.yml/badge.svg)](https://github.com/bullno1/buxn-ls/actions/workflows/build.yml)

A language server for [uxntal](https://wiki.xxiivv.com/site/uxntal.html).

## Features

* Auto-completion
* Error reporting
* Hover
* Go to definition
* Go to references
* Workspace and document symbol listing

For more info, refer to:

* [Request handlers](src/ls.c#L667)
* [Capabilities](src/initialize.json)

Currently, it is only tested against [YouCompleteMe](https://github.com/ycm-core/YouCompleteMe) and Vim.

### Extensions

The following (optional) extensions are provided:

* `(doc )` comment: When put before a label, the content will become the documentation for the label.
  It will be displayed in the auto-completion list.
* `(buxn:device )`: All zero-page labels after this comment are treated as device ports.
* `(buxn:memory )`: All zero-page labels after this comment are treated as main memory addressses.
* `(buxn:enum )`: The group of zero-page addresses immediately following this comment are treated as enumerations.

The comments do not change the binary output of the program.
However, it provides the language server with more semantic informations.

## Building
### Linux & FreeBSD

Required software:

* clang
* mold
* cmake
* ninja

```sh
./bootstrap  # To update dependencies
BUILD_TYPE=RelWithDebInfo ./build
```

During development, `./watch` will watch the directory and build whenever a change is detected.

### Windows

Run `msvc.bat` to generate the Visual Studio solution.

## Configuration

```vim
let g:ycm_language_server =
  \ [
  \   {
  \     'name': 'uxntal',
  \     'cmdline': [ '<path-to-project>/buxn-ls', '--mode=dev' ],
  \     'filetypes': [ 'uxntal' ]
  \   }
  \ ]
```

You'd also need to install [bellinitte/uxntal.vim](https://github.com/bellinitte/uxntal.vim) so that the language is recognized.

### What are modes?

By default, without any arguments, `buxn-ls` starts in `stdio` mode.
All protocol messages will be exchanged through stdio.
This is the expected behaviour for a language server.

However, it makes debugging difficult.
There is no easy way to attach a debugger to the server process or to view its logs.
Thus, `server` and `shim` mode are created.

In `server` mode, the server started by listening on an (abstract) Unix domain socket.
A process in `shim` mode will connect to that socket and forward all stdio messages through that connection.
This allows the server to be started with a debugger attached.
Moreover, its debug output can be monitored easily.
The language server that the editor interacts with is just a dumb proxy, which hopefully does not contain any bugs.

However, with `--mode=shim`, the proxy would not be functional without a server running in the background.
This is troublesome if one only wants to use `buxn-ls` for editting unxtal source files and not to develoop or debug it.
`dev` mode is a hybrid of `stdio` and `shim`.
It will opportunistically attempt to connect to the server.
But if that fails, it will fallback to stdio and works as a standalone server.

For more info, run: `buxn-ls --help`.
