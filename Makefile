PREFIX ?= $(HOME)/.local
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
 
all: ssh-mptcp
 
ssh-mptcp: ssh-mptcp.c
	$(CC) $(CFLAGS) -framework Network -o $@ $<
 
install: ssh-mptcp
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 ssh-mptcp $(DESTDIR)$(PREFIX)/bin/ssh-mptcp
 
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/ssh-mptcp
 
check: ssh-mptcp
	printf 'GET / HTTP/1.1\r\nHost: check.mptcp.dev\r\nUser-Agent: curl/8\r\nConnection: close\r\n\r\n' \
	    | ./ssh-mptcp -s check.mptcp.dev 443 | tail -n 1
 
clean:
	rm -f ssh-mptcp
 
.PHONY: all install uninstall check clean
 
