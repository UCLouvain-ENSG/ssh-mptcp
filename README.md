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

## Troubleshooting

If the SYN carries no `MP_CAPABLE` option and `make check` says *You are not
using MPTCP*, the connection fell back to plain TCP. `ssh-mptcp` detects this
and warns on stderr:

```
ssh-mptcp: warning: host port 22 connected without MPTCP; this is plain TCP
ssh-mptcp: the system refused multipath for this route; a VPN or a filtering
           network extension is the usual cause (-q silences this)
```

Network.framework reports the connection as `ready` whether or not it got
multipath, so the check asks the kernel instead: an MPTCP connection is backed
by a socket in the `PF_MULTIPATH` domain, a plain one by `AF_INET`.

The common causes, in order:

* **A VPN or filtering network extension owns the route.** Enterprise VPN and
  endpoint-security clients install a socket or content-filter network
  extension that attaches a *flow divert* agent to the path.
  Network.framework cannot run MPTCP over a diverted flow and silently
  downgrades it. These products are often split-tunnel, so hosts that bypass
  the tunnel keep MPTCP while everything else loses it — which is why
  `make check` can fail on a machine where `ssh` works. Confirm with:

  ```
  log stream --debug --predicate 'process == "ssh-mptcp" AND category == "connection"'
  ```

  A working connection builds a `multipath-socket-flow`; a downgraded one
  builds a plain `socket-flow` and its path reads `flow divert agg: N`.

* **MPTCP is off in the kernel.** `sysctl net.inet.mptcp.enable` must be `1`.

* **The server does not speak MPTCP.** Here the SYN *does* carry `MP_CAPABLE`
  and the socket stays `PF_MULTIPATH`; the kernel falls back after the
  handshake, so no warning is printed.

`sysctl -n net.inet.mptcp.pcbcount` is a quick root-free check: it goes up by
one while a real MPTCP connection is open.

Check https://perso.uclouvain.be/tom.barbette/tired-of-ssh-and-claude-getting-frozen-when-you-switch-to-wifi-use-mptcp-on-macos/ for the full story