/*
 * ssh-mptcp: open an MPTCP connection with macOS Network.framework and relay
 * it over stdin/stdout, for use as an OpenSSH ProxyCommand.
 *
 *   Host *.example.org
 *       ProxyCommand ~/bin/ssh-mptcp -m interactive %h %p
 *
 * Build: clang -O2 -Wall -framework Network -o ssh-mptcp ssh-mptcp.c
 *
 * Uses only the public nw_connection API (macOS >= 10.14). Network.framework
 * does not hand out a socket fd, hence the relay instead of ProxyUseFdpass.
 */

#include <Network/Network.h>
#include <dispatch/dispatch.h>

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RELAY_CHUNK (64 * 1024)

static nw_connection_t conn;
static dispatch_queue_t net_q;   /* connection events, stdin -> network */
static dispatch_queue_t out_q;   /* network -> stdout (blocking writes) */
static dispatch_source_t in_src;
static bool ready;
static int verbose;
static const char *host, *port;
static uint8_t in_buf[RELAY_CHUNK];

static void
vlog(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;
	va_start(ap, fmt);
	fprintf(stderr, "ssh-mptcp: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void
die_nw(const char *what, nw_error_t error)
{
	int code = error ? nw_error_get_error_code(error) : 0;

	if (error == NULL)
		fprintf(stderr, "ssh-mptcp: %s %s port %s: failed\n",
		    what, host, port);
	else if (nw_error_get_error_domain(error) == nw_error_domain_dns)
		fprintf(stderr, "ssh-mptcp: could not resolve %s (DNS error %d)\n",
		    host, code);
	else if (nw_error_get_error_domain(error) == nw_error_domain_tls)
		fprintf(stderr, "ssh-mptcp: TLS error %d with %s\n", code, host);
	else
		fprintf(stderr, "ssh-mptcp: %s %s port %s: %s\n", what, host, port,
		    strerror(code));
	exit(255);
}

static void
log_path(const char *what, nw_path_t path)
{
	nw_endpoint_t remote;

	if (!verbose || path == NULL)
		return;
	fprintf(stderr, "ssh-mptcp: %s: status=%s",
	    what, nw_path_get_status(path) == nw_path_status_satisfied ?
	    "satisfied" : "not satisfied");
	nw_path_enumerate_interfaces(path, ^bool(nw_interface_t iface) {
		fprintf(stderr, " if=%s", nw_interface_get_name(iface));
		return true;
	});
	if (nw_path_is_expensive(path))
		fprintf(stderr, " expensive");
	if ((remote = nw_path_copy_effective_remote_endpoint(path)) != NULL) {
		if (nw_endpoint_get_type(remote) == nw_endpoint_type_address) {
			char *a = nw_endpoint_copy_address_string(remote);
			fprintf(stderr, " remote=%s", a);
			free(a);
		}
		nw_release(remote);
	}
	fputc('\n', stderr);
}

/* network -> stdout */
static void
receive_loop(void)
{
	nw_connection_receive(conn, 1, RELAY_CHUNK, ^(dispatch_data_t content,
	    nw_content_context_t context, bool is_complete, nw_error_t error) {
		bool final = is_complete && context != NULL &&
		    nw_content_context_get_is_final(context);
		int err = error ? nw_error_get_error_code(error) : 0;
		bool failed = error != NULL;

		if (content != NULL)
			dispatch_retain(content);
		dispatch_async(out_q, ^{
			if (content != NULL) {
				dispatch_data_apply(content, ^bool(dispatch_data_t r,
				    size_t off, const void *buf, size_t len) {
					(void)r; (void)off;
					const uint8_t *p = buf;
					while (len > 0) {
						ssize_t n = write(STDOUT_FILENO, p, len);
						if (n < 0 && errno == EINTR)
							continue;
						if (n < 0)
							exit(errno == EPIPE ? 0 : 255);
						p += n;
						len -= (size_t)n;
					}
					return true;
				});
				dispatch_release(content);
			}
			if (failed) {
				/* ECONNRESET etc. after the session started */
				vlog("receive error: %s", strerror(err));
				exit(err == 0 ? 0 : 255);
			}
			if (final) {
				vlog("remote closed the connection");
				exit(0);
			}
			receive_loop();
		});
	});
}

/* stdin -> network */
static void
start_stdin(void)
{
	in_src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
	    STDIN_FILENO, 0, net_q);
	dispatch_source_set_event_handler(in_src, ^{
		ssize_t n = read(STDIN_FILENO, in_buf, sizeof(in_buf));

		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				return;
			fprintf(stderr, "ssh-mptcp: stdin: %s\n", strerror(errno));
			exit(255);
		}
		if (n == 0) {
			/* ssh closed its side: half-close (FIN), keep receiving */
			vlog("stdin EOF, sending FIN");
			dispatch_source_cancel(in_src);
			nw_connection_send(conn, NULL,
			    NW_CONNECTION_FINAL_MESSAGE_CONTEXT, true,
			    ^(nw_error_t e) { (void)e; });
			return;
		}
		dispatch_data_t d = dispatch_data_create(in_buf, (size_t)n, NULL,
		    DISPATCH_DATA_DESTRUCTOR_DEFAULT);   /* copies in_buf */
		/* one chunk in flight: natural backpressure */
		dispatch_suspend(in_src);
		nw_connection_send(conn, d, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT,
		    true, ^(nw_error_t e) {
			if (e != NULL)
				die_nw("send to", e);
			dispatch_resume(in_src);
		});
		dispatch_release(d);
	});
	dispatch_resume(in_src);
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: ssh-mptcp [-v] [-4|-6] [-s] [-k secs] [-t secs]\n"
	    "                 [-m none|handover|interactive|aggregate] host port\n"
	    "  -m  multipath service (default: interactive)\n"
	    "  -s  wrap in TLS (testing, e.g. check.mptcp.dev 443)\n"
	    "  -k  TCP keepalive idle time in seconds (default: off)\n"
	    "  -t  connection timeout in seconds (default: system)\n"
	    "  -v  log path/interface changes to stderr\n");
	exit(255);
}

int
main(int argc, char **argv)
{
	nw_multipath_service_t svc = nw_multipath_service_interactive;
	uint32_t keepalive = 0, timeout = 0;
	nw_ip_version_t ipver = nw_ip_version_any;
	bool tls = false;
	int ch;

	while ((ch = getopt(argc, argv, "46k:m:st:v")) != -1) {
		switch (ch) {
		case '4': ipver = nw_ip_version_4; break;
		case '6': ipver = nw_ip_version_6; break;
		case 'k': keepalive = (uint32_t)strtoul(optarg, NULL, 10); break;
		case 's': tls = true; break;
		case 't': timeout = (uint32_t)strtoul(optarg, NULL, 10); break;
		case 'v': verbose++; break;
		case 'm':
			if (strcmp(optarg, "none") == 0)
				svc = nw_multipath_service_disabled;
			else if (strcmp(optarg, "handover") == 0)
				svc = nw_multipath_service_handover;
			else if (strcmp(optarg, "interactive") == 0)
				svc = nw_multipath_service_interactive;
			else if (strcmp(optarg, "aggregate") == 0)
				svc = nw_multipath_service_aggregate;
			else
				usage();
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc != 2)
		usage();
	host = argv[0];
	port = argv[1];

	signal(SIGPIPE, SIG_IGN);
	net_q = dispatch_queue_create("ssh-mptcp.net", DISPATCH_QUEUE_SERIAL);
	out_q = dispatch_queue_create("ssh-mptcp.out", DISPATCH_QUEUE_SERIAL);

	nw_parameters_configure_protocol_block_t tcp_cfg =
	    ^(nw_protocol_options_t tcp) {
		nw_tcp_options_set_no_delay(tcp, true);
		if (timeout)
			nw_tcp_options_set_connection_timeout(tcp, timeout);
		if (keepalive) {
			nw_tcp_options_set_enable_keepalive(tcp, true);
			nw_tcp_options_set_keepalive_idle_time(tcp, keepalive);
		}
	};
	nw_parameters_t params = nw_parameters_create_secure_tcp(
	    tls ? NW_PARAMETERS_DEFAULT_CONFIGURATION :
	    NW_PARAMETERS_DISABLE_PROTOCOL, tcp_cfg);
	nw_parameters_set_multipath_service(params, svc);
	nw_parameters_set_prefer_no_proxy(params, true);
	if (ipver != nw_ip_version_any) {
		nw_protocol_stack_t stack =
		    nw_parameters_copy_default_protocol_stack(params);
		nw_protocol_options_t ip =
		    nw_protocol_stack_copy_internet_protocol(stack);
		nw_ip_options_set_version(ip, ipver);
		nw_release(ip);
		nw_release(stack);
	}

	nw_endpoint_t ep = nw_endpoint_create_host(host, port);
	conn = nw_connection_create(ep, params);
	nw_release(ep);
	nw_release(params);
	nw_connection_set_queue(conn, net_q);

	nw_connection_set_state_changed_handler(conn,
	    ^(nw_connection_state_t state, nw_error_t error) {
		switch (state) {
		case nw_connection_state_preparing:
			vlog("connecting to %s port %s (multipath service %d)",
			    host, port, (int)svc);
			break;
		case nw_connection_state_waiting:
			/* ssh expects a hard failure, not a silent retry */
			if (!ready)
				die_nw("connect to", error);
			vlog("waiting: %s", error ?
			    strerror(nw_error_get_error_code(error)) : "?");
			break;
		case nw_connection_state_ready: {
			nw_path_t p = nw_connection_copy_current_path(conn);
			ready = true;
			log_path("ready", p);
			if (p != NULL)
				nw_release(p);
			start_stdin();
			receive_loop();
			break;
		}
		case nw_connection_state_failed:
			die_nw(ready ? "connection to" : "connect to", error);
			break;
		case nw_connection_state_cancelled:
			exit(0);
		default:
			break;
		}
	});
	nw_connection_set_path_changed_handler(conn, ^(nw_path_t path) {
		log_path("path changed", path);
	});
	nw_connection_set_viability_changed_handler(conn, ^(bool viable) {
		vlog("viability: %s", viable ? "viable" : "NOT viable");
	});
	nw_connection_set_better_path_available_handler(conn, ^(bool better) {
		vlog("better path available: %s", better ? "yes" : "no");
	});

	nw_connection_start(conn);
	dispatch_main();
}
