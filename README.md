# ssh-mptcp

Multipath TCP for SSH on macOS.

Unplug the Ethernet cable, walk away with the laptop on WiFi, and the SSH
session keeps going. So does everything that runs over SSH: `git`, `rsync`,
VS Code Remote-SSH, and the Claude Code session running inside it.

`ssh-mptcp` is a ~300-line `ProxyCommand` that opens the connection with
Multipath TCP ([RFC 8684](https://datatracker.ietf.org/doc/html/rfc8684))
through macOS' Network.framework, and relays it to `ssh` over stdin/stdout.

```
Host myserver
    ProxyCommand ~/.local/bin/ssh-mptcp %h %p
```

## Why

A TCP connection is bound to one source address. When you move from Ethernet
to WiFi, that address disappears and the connection hangs until something times
out. Multipath TCP lets one connection run over several subflows, one per
interface, and move traffic between them without the application noticing.

The macOS kernel has had an MPTCP client for years, but it does not expose it
through the socket API: there is no `IPPROTO_MPTCP` on macOS, so the Linux
tricks (`mptcpize`, `LD_PRELOAD`) do not apply. The supported way in is
Network.framework (`nw_parameters_set_multipath_service`, macOS 10.14+).
OpenSSH itself does not support MPTCP: the upstream maintainers declined it
([#335](https://github.com/openssh/openssh-portable/pull/335),
[#547](https://github.com/openssh/openssh-portable/pull/547)).

## Install

Requires the Xcode command line tools (`xcode-select --install`).

```
git clone https://github.com/UCLouvain-ENSG/ssh-mptcp.git
cd ssh-mptcp
make && make install          # installs to ~/.local/bin (PREFIX=... to change)
make check                    # prints "You are using MPTCP." if your Mac can do MPTCP
```

Then add a `ProxyCommand` to the hosts you want in `~/.ssh/config`:

```
Host *.example.org
    ProxyCommand ~/.local/bin/ssh-mptcp %h %p
```

This works with any `ssh` client that reads `~/.ssh/config`: Apple's
`/usr/bin/ssh`, Homebrew's, and VS Code Remote-SSH, which needs no extra
setting.

Check https://perso.uclouvain.be/tom.barbette/tired-of-ssh-and-claude-getting-frozen-when-you-switch-to-wifi-use-mptcp-on-macos/ for the full story