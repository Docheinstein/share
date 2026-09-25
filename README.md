# share

Send files to another machine on the same local network. No accounts, no long tickets:
the receiver is identified by a short name (by default, the name of its current folder).

```sh
# on the receiving machine, inside the folder where files should land
cd ~/Downloads
share recv                 # → Receiving as 'Downloads' ...

# on the sending machine
share send downloads report.pdf photos/
```

## Usage

```
share recv [name]             wait for files, saved into the current directory.
                              <name> defaults to the directory's name.
share send <name> <path>...   send files or directories to the receiver <name>.
share list                    list the receivers on the network and their folders.
share send                    same as 'share list'.
```

- Names are case-insensitive. If two receivers share a name, use `name@host` (IP or hostname).
- `share send 192.168.1.20 file` also works when broadcast is blocked on the network.
- Existing files are never overwritten: `a.txt` becomes `a (1).txt`, `dir` becomes `dir (1)`.
- Files are written as hidden `.name.share-part` and renamed when complete, so an interrupted
  transfer never leaves a truncated file behind.
- The receiver keeps running and accepts any number of transfers until Ctrl-C.

## Build

```sh
cmake -S . -B build && cmake --build build
sudo cmake --install build        # installs /usr/local/bin/share
```

Linux, macOS and Windows, C++23, no dependencies. On Windows, build with Visual Studio or MinGW-w64
(`cmake -S . -B build -G "MinGW Makefiles"`); the result is a single `share.exe`.

Receivers on any platform can exchange files with each other. A Windows receiver rejects file names that Windows
cannot store (e.g. containing `:`, `?`, `*`, ending in `.` or a space, or reserved names like `CON`); on Windows the
partial `.name.share-part` file is also given the hidden attribute.

## How it works

1. `share send` broadcasts a UDP query on port **47001** to every interface (and to localhost).
2. Each `share recv` answers with its name, its folder's name (for `share list`) and TCP port (**47002**, or a random port if taken).
3. The sender connects over TCP and streams the files. The receiver checks the name, rejects
   unsafe paths (`..`, absolute paths), and confirms once everything is written to disk.

If a firewall is on, allow UDP 47001 and TCP 47002 on the receiver, e.g. `sudo ufw allow 47001/udp && sudo ufw allow 47002/tcp`.
On Windows, accept the firewall prompt shown the first time `share recv` runs (allow private networks), or run
`netsh advfirewall firewall add rule name=share dir=in action=allow program="C:\path\to\share.exe"` as administrator.

**Security note:** it is meant for trusted LANs. There is no encryption or authentication, so
anyone on the network who knows (or lists) the name can drop files into the receiving folder
while `share recv` is running.
