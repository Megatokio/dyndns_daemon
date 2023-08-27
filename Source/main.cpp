// Copyright (c) 2015 - 2023 kio@little-bat.de
// BSD-2-Clause license
// https://opensource.org/licenses/BSD-2-Clause

#include "Templates/Array.h"
#include "cstrings/cstrings.h"
#include "kio/kio.h"
#include "unix/files.h"
#include <curl/curl.h> // https://curl.haxx.se/libcurl/c/

#undef curl_easy_setopt	 // macro that replaces a call with the exact same call
#undef curl_easy_getinfo // ""

cstr		APPL_NAME;
static cstr logdir		  = "/var/log/dyndns_daemon/";
const char	config_file[] = "~/.dyndns.config";
const char	useragent[]	  = "dyndns_daemon/2.0";
const char	usage[] =
	"dyndns_daemon 2.0 (c) 2015-2023 kio@little-bat.de\n"
	"  https://github.com/Megatokio/dyndns_daemon\n"
	"  usage: dyndns_daemon [-v -i -f -b] [configfile]\n"
	"  -v --verbose\n"
	"  -i --info: show config and state\n"
	"  -f --foreground: run in foreground\n"
	"  -b --background: run as daemon\n";

static bool foreground = false;
static bool background = false;
static bool verbose	   = false;
static bool showinfo   = false;

static cstr mydomain;
static cstr updatehost;
static cstr query_all;	// = "hostname=&{DOMAIN}&myipv4={IP4}&myipv6={IP6}";
static cstr query_ipv4; // = "hostname=&{DOMAIN}&myipv4={IP4}";
static cstr query_ipv6; // = "hostname=&{DOMAIN}&myipv4={IP6}";
static cstr username;
static cstr password;
static cstr pingselfurl = "{DOMAIN}/";
static cstr getmyipurl;


enum IPversion {
	ip_any = CURL_IPRESOLVE_WHATEVER,
	ipv4   = CURL_IPRESOLVE_V4,
	ipv6   = CURL_IPRESOLVE_V6,
};

enum ServerStatus { Stopped, Unreachable, Reachable };

struct Interface
{
	int				enabled;		// in query string?
	const IPversion ip_version;		// ipv4
	const cstr		name;			// "ipv4"
	const cstr		loopback;		// "127.0.0.1"
	cstr (*const extract_ip)(cstr); // &extract_ipv4
	bool (*const is_local)(cstr);	// &is_local_ipv4_address

	std::unique_ptr<char[]> published_address;

	ServerStatus check_status() noexcept;
	bool		 ping_self(cstr pingselfurl);
	cstr		 get_my_ip() noexcept;
};


// -----------------------------------------------------------------

static cstr tostr(IPversion v) noexcept { return v == ipv4 ? "ipv4" : v == ipv6 ? "ipv6" : "ip_any"; }

static cstr tostr(ServerStatus ss) noexcept
{
	static const char s[3][12] = {"stopped", "unreachable", "reachable"};
	return s[ss];
}

static size_t store_url_data(void* data, size_t size, size_t nmemb, char** userdata) noexcept
{
	// helper:
	// store received data from URL:
	// always appends c-string delimiter 0x00

	size *= nmemb;

	char*& bu = *userdata;

	size_t oldsize = bu ? strlen(bu) - 1 : 0;
	size_t newsize = oldsize + size;
	str	   s	   = tempstr(uint32(newsize));
	if (bu) memcpy(s, bu, oldsize);
	memcpy(s + oldsize, data, size);
	bu = s;

	return size;
}

static size_t discard_url_data(void*, size_t size, size_t nmemb, char**) noexcept
{
	// helper:
	// discard received data

	return size * nmemb;
}

static cstr extract_ipv4(cstr s) noexcept
{
	// search for, decode and return ipv4 address:
	// e.g. 127.0.0.1

	for (;;)
	{
		while (*s && !is_dec_digit(*s)) s++; // search start of decimal number
		if (*s == 0) return nullptr;		 // end of text: ipv4 not found

		cptr ipstr = s;
		uint dots  = 0;
		for (;;)
		{
			uint n = 0;
			while (is_dec_digit(*s)) n = n * 10 + uint(*s++) - '0'; // parse decimal number
			if (n > 255) dots = 99;									// too big
			if (*s != '.') break;
			dots++;
			s++;
		}

		if (dots == 3) return substr(ipstr, s); // OK
	}
}

static cstr extract_ipv6(cstr s) noexcept
{
	// search for, decode and return ipv6 address:
	// e.g. ::1 or 2001:a62:19e9:fa01:16d1:6b0f:c404:cdea

	for (;;)
	{
		while (*s && *s != ':' && !is_hex_digit(*s)) s++; // search start of possible ip number
		if (*s == 0) return nullptr;					  // end of text: ip not found

		cptr ipstr	= s;
		uint colons = 0;
		for (;;)
		{
			uint digits = 0;
			while (is_hex_digit(*s++)) digits++; // skip hex number
			s--;
			if (digits > 4) colons = 99; // too big
			if (*s != ':') break;
			colons++;
			s++;
		}

		if (colons >= 2 && colons <= 7) return substr(ipstr, s); // OK
	}
}

static bool isa_local_ipv4_address(cstr ip) noexcept
{
	return startswith(ip, "10.") || startswith(ip, "127.") || startswith(ip, "192.");
}

static bool isa_local_ipv6_address(cstr ip) noexcept
{
	return eq(ip, "::1") || eq(ip, "0:0:0:0:0:0:0:1") || (startswith(ip, "fd") && strchr(ip, ':') == ip + 4);
}

static str get_url(cstr url, IPversion ip_version, bool httpauth = no) noexcept
{
	// get text from url
	// optionally with username & password
	// returns received data as tempstr
	// returns nullptr on error

	CURL* curl = curl_easy_init();
	if (!curl)
	{
		logline("curl_easy_init failed!");
		return nullptr;
	}

	char* bu = nullptr;

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_IPRESOLVE, ip_version);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, useragent);			// some servers require the user-agent field
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &store_url_data); // register function to receive data
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bu);					// pass bu[] to the callback function

	CURLcode res = CURLE_OK;
	if (httpauth)
	{
		if (res == CURLE_OK) res = curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
		if (res == CURLE_OK) res = curl_easy_setopt(curl, CURLOPT_USERNAME, username);
		if (res == CURLE_OK) res = curl_easy_setopt(curl, CURLOPT_PASSWORD, password);
	}

	if (res == CURLE_OK) res = curl_easy_perform(curl); // do it!
	curl_easy_cleanup(curl);							// always clean up!

	if (res != CURLE_OK)
	{
		logline("get_url: %s: %s: %s", tostr(ip_version), url, curl_easy_strerror(res));
		return nullptr; // failed
	}

	return bu;
}

static bool update_my_ip(cstr new_ipv4, cstr new_ipv6) noexcept
{
	// update my ip addresses:
	// in:         ipv4 and ipv6 strings (one may be NULL)
	// return:     true=ok // "good 1.2.3.4" or "nochg 1.2.3.4" or "nochg"
	// or error:   badsys badagent badauth !donator notfqdn nohost !yours numhost abuse dnserr 911

	cstr query = new_ipv4 && new_ipv6 ? query_all : new_ipv4 ? query_ipv4 : new_ipv6 ? query_ipv6 : nullptr;
	if (!query)
	{
		logline("update_my_ip: no suitable query string");
		return false;
	}

	query = replacedstr(query, "{DOMAIN}", mydomain);
	if (new_ipv4) query = replacedstr(query, "{IP4}", new_ipv4);
	if (new_ipv6) query = replacedstr(query, "{IP6}", new_ipv6);
	cstr url = catstr(updatehost, "?", query);

	str result = get_url(url, ip_any, yes /*auth*/);
	if (!result || !*result)
	{
		if (verbose) logline("update_my_ip: get_url returned no data"); // already reported by get_url
		return false;
	}

	// "good 1.2.3.4" oder "nochg 1.2.3.4" oder "nochg"
	// oder: badsys badagent badauth !donator notfqdn nohost !yours numhost abuse dnserr 911

	tolower(result);
	bool ok = startswith(result, "good") || startswith(result, "nochg");
	if (verbose || !ok) logline("update_my_ip: %s", result);
	return ok;
}


static Interface ifv4 = {false, ipv4, "ipv4", "127.0.0.1", &extract_ipv4, &isa_local_ipv4_address, nullptr};
static Interface ifv6 = {false, ipv6, "ipv6", "[::1]", &extract_ipv6, &isa_local_ipv6_address, nullptr};


bool Interface::ping_self(cstr pingselfurl)
{
	// ping self using the supplied url
	// return true: ok

	CURL* curl = curl_easy_init();
	if (!curl)
	{
		logline("curl_easy_init failed!");
		return false;
	}

	curl_easy_setopt(curl, CURLOPT_URL, pingselfurl);
	curl_easy_setopt(curl, CURLOPT_IPRESOLVE, ip_version);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, useragent);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &discard_url_data);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 299L);
	curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
	CURLcode res = curl_easy_perform(curl);

	bool ok = res == CURLE_OK;
	if (!ok) logline("ping_self %s: %s: %s", name, pingselfurl, curl_easy_strerror(res));
	else
	{
		long result = 666;
		res			= curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result);
		ok			= res == CURLE_OK && result >= 200 && result < 400;
		if (!ok) logline("ping_self %s: %s: http result code = %li", name, pingselfurl, result);
	}

	curl_easy_cleanup(curl);
	return ok;
}

cstr Interface::get_my_ip() noexcept
{
	// get my ip from dyndns server
	// return ip address if OK
	// return nullptr on failure

	str data = get_url(getmyipurl, ip_version);
	if (data == nullptr)
	{
		if (verbose) logline("get_my_ip: %s: get_url returned no data", name); // allready reported by get_url
		return nullptr;
	}

	if (cstr my_ip = extract_ip(data))
	{
		if (!is_local(my_ip)) return my_ip;
		logline("get_my_ip: %s: local address in response: %s", name, my_ip);
	}
	else
	{
		logline("get_by_ip: %s: no ip address in response", name);
		if (verbose) logline("--> %s", escapedstr(leftstr(data, 500)));
	}
	return nullptr;
}


ServerStatus Interface::check_status() noexcept
{
	if (!enabled) return Stopped;
	if (ping_self(replacedstr(pingselfurl, "{DOMAIN}", mydomain))) return Reachable;
	if (ping_self(replacedstr(pingselfurl, "{DOMAIN}", loopback))) return Unreachable;
	return Stopped;
}


__attribute__((noreturn)) static void dyndns_updater() noexcept
{
	logline("%s running.", APPL_NAME);

	for (;;)
	{
		sleep(10);
		TempMemPool z;

		ServerStatus ss4 = ifv4.check_status();
		ServerStatus ss6 = ifv6.check_status();

		//if ((ss4 == Reachable || !ifv4.enabled) && (ss6 == Reachable || !ifv6.enabled)) continue;
		if (ss4 == Stopped && ss6 == Stopped) continue;

		cstr old_ip4 = ifv4.published_address.get();
		cstr old_ip6 = ifv6.published_address.get();

		bool if4_needs_update = ss4 == Unreachable || (ss4 == Stopped && old_ip4 != nullptr);
		bool if6_needs_update = ss6 == Unreachable || (ss6 == Stopped && old_ip6 != nullptr);

		if (!if4_needs_update && !if6_needs_update) continue;

		logline("--- host unreachable ---");

		cstr new_ip4 = ss4 == Stopped ? nullptr : ss4 == Reachable && old_ip4 ? old_ip4 : ifv4.get_my_ip();
		cstr new_ip6 = ss6 == Stopped ? nullptr : ss6 == Reachable && old_ip6 ? old_ip6 : ifv6.get_my_ip();

		if (!new_ip4 && !new_ip6)
		{
			logline("get_my_ip: no answer. network down?\n");
			continue;
		}

		if (ifv4.enabled && verbose) logline("old ipv4: %s", old_ip4 ? old_ip4 : "offline");
		if (ifv4.enabled)
			logline(
				"new ipv4: %s (%s)", new_ip4 ? new_ip4 : "offline",
				eq(new_ip4, old_ip4) ? "no change" : "needs update");

		if (ifv6.enabled && verbose) logline("old ipv6: %s", old_ip6 ? old_ip6 : "offline");
		if (ifv6.enabled)
			logline(
				"new ipv6: %s (%s)", new_ip6 ? new_ip6 : "offline",
				eq(new_ip6, old_ip6) ? "no change" : "needs update");

		if (new_ip4 == old_ip4 && new_ip6 == old_ip6)
		{
			logline("ip address did not change. routing correctly configured?\n");
			continue;
		}

		if (update_my_ip(new_ip4, new_ip6))
		{
			logline("update_my_ip: success");
			ifv4.published_address.reset(newcopy(new_ip4));
			ifv6.published_address.reset(newcopy(new_ip6));
			logline("+++ host reachable +++");
		}
		else logline("update_my_ip: failed");

		logline("sleeping (300s) ...");
		sleep(300);
	}
}

void show_info()
{
	// run some tests, show configuration and current DNS state

	// configuration:

	printf("mydomain: %s\n", mydomain);
	printf("updatehost: %s\n", updatehost);
	printf("query_all: %s\n", query_all);
	printf("query_ipv4: %s -- ipv4 %s\n", query_ipv4, ifv4.enabled ? "enabled" : "disabled");
	printf("query_ipv6: %s -- ipv6 %s\n", query_ipv6, ifv6.enabled ? "enabled" : "disabled");
	printf("username: %s\n", username);
	printf("password: %s\n", password);
	printf("pingselfurl: %s\n", pingselfurl);
	printf("getmyipurl: %s\n", getmyipurl);

	// some tests:

	assert(eq(extract_ipv4("127.0.0.1"), "127.0.0.1"));
	assert(eq(extract_ipv4("ip = 66.0.0.125\n"), "66.0.0.125"));
	assert(eq(extract_ipv4("today=26.8.2023;ip=1.22.111.0:45231\n"), "1.22.111.0"));
	assert(eq(extract_ipv6("::1"), "::1"));
	assert(eq(extract_ipv6("ip = 123:3211:a234::3\n"), "123:3211:a234::3"));
	assert(eq(extract_ipv6("today:26.8.2023;ip:[0123:44:54:255:2::]:45231\n"), "0123:44:54:255:2::"));

	assert(isa_local_ipv4_address("127.22.22.1"));
	assert(isa_local_ipv4_address("10.100.100.100"));
	assert(!isa_local_ipv4_address("227.0.0.1"));
	assert(!isa_local_ipv4_address("100.255.0.1"));

	assert(isa_local_ipv6_address("::1"));
	assert(isa_local_ipv6_address("fd22:22::"));
	assert(!isa_local_ipv6_address("fd2:22::"));
	assert(!isa_local_ipv6_address("2001:a62:1904:2e01:b480:8e6e:7950:7e8"));

	// status:

	printf("\ninterface ipv4: %s\n", ifv4.enabled ? "enabled" : "disabled");
	printf("ping self (loopback): %s\n", ifv4.ping_self(ifv4.loopback) ? "ok" : "failed");
	printf("ping self (domain): %s\n", ifv4.ping_self(mydomain) ? "ok" : "failed");
	printf("server status: %s\n", tostr(ifv4.check_status()));
	printf("ip address: %s\n", ifv4.get_my_ip());

	printf("\ninterface ipv6: %s\n", ifv6.enabled ? "enabled" : "disabled");
	printf("ping self (loopback): %s\n", ifv6.ping_self(ifv6.loopback) ? "ok" : "failed");
	printf("ping self (domain): %s\n", ifv6.ping_self(mydomain) ? "ok" : "failed");
	printf("server status: %s\n", tostr(ifv6.check_status()));
	printf("ip address: %s\n\n", ifv6.get_my_ip());
}

static void drop_suid() noexcept
{
	uid_t euid = geteuid();
	uid_t egid = getegid();

	if (euid == 0 || egid == 0)
	{
		logline("dropping root");

		uid_t ruid = getuid();
		uid_t rgid = getgid();

		if (ruid == 0)
		{
			cstr uidstr = getenv("SUDO_UID");
			cstr gidstr = getenv("SUDO_GID");
			if (uidstr) ruid = uint(atoi(uidstr));
			if (gidstr) rgid = uint(atoi(gidstr));

			if (ruid == 0) ruid = 1000;
			if (rgid == 0) rgid = ruid;
		}

		logline("switch to user=%u, group=%u", ruid, rgid);
		if (setegid(rgid) || seteuid(ruid)) abort("seteuid failed");
	}
}

static void daemonize() throws
{
	int err = daemon(0, 0);
	assert(err == 0 || err == -1);
	if (err) throw strerror(errno);
	// now forked and pwd="/" and stdin/out/err=/dev/null and no controlling terminal
	// and session leader and group leader and and and ...
	log2console = no;
}

static void parse_config(cstr configfile) throws
{
	FD fd(configfile);

	for (;;)
	{
		cstr s = fd.read_str();
		if (!s) break; // end of file
		while (is_space(*s)) s++;
		if (*s == '#') continue; // comment
		if (*s == 0) continue;	 // empty line

		cptr sep = strchr(s, ':');
		if (!sep) throw usingstr("colon missing: %s", s);

		cstr key = lowerstr(substr(s, sep));

		cstr val = croppedstr(sep + 1);
		if (*val == 0) continue; // error
		if (*val == '"') val = unquotedstr(val);

		if (eq(key, "domain"))
		{
			mydomain = curl_escape(val, 0); // on heap!
			continue;
		}
		if (eq(key, "update"))
		{
			updatehost = val;
			continue;
		}
		if (eq(key, "username"))
		{
			username = val;
			continue;
		}
		if (eq(key, "password"))
		{
			password = val;
			continue;
		}

		if (eq(key, "pingself"))
		{
			if (!find(val, "{DOMAIN}")) throw "expected '{DOMAIN}' in pingself";
			pingselfurl = val;
			continue;
		}
		if (eq(key, "getmyip"))
		{
			getmyipurl = val;
			continue;
		}
		if (eq(key, "logdir"))
		{
			logdir = val;
			continue;
		}
		if (eq(key, "query"))
		{
			if (!find(val, "{DOMAIN}")) throw "expected '{DOMAIN}' in query string";
			if (*val == '?') val++;
			bool ip4 = find(val, "{IP4}");
			bool ip6 = find(val, "{IP6}");
			if (ip4 && ip6) query_all = val;
			else if (ip4) query_ipv4 = val;
			else if (ip6) query_ipv6 = val;
			else throw "expected '{IP4}' and/or '{IP6}' in query string";
			continue;
		}

		throw usingstr("unknown option: %s", key);
	}

	if (!mydomain) throw "'domain' missing";
	if (!getmyipurl) throw "'getmyip' missing";
	if (!updatehost) throw "'update' missing";
	if (!username) throw "'username' missing";
	if (!password) throw "'password' missing";
	if (!query_all && !query_ipv4 && !query_ipv6) throw "'query' string missing";

	if (query_all)
	{
		if (!query_ipv4)
		{
			Array<cstr> args;
			split(args, query_all, '&');
			for (uint i = 0; i < args.count(); i++)
				if (contains(args[i], "{IP6}")) args.remove(i);
			query_ipv4 = join(args, '&');
		}
		if (!query_ipv6)
		{
			Array<cstr> args;
			split(args, query_all, '&');
			for (uint i = 0; i < args.count(); i++)
				if (contains(args[i], "{IP4}")) args.remove(i);
			query_ipv6 = join(args, '&');
		}
	}

	ifv4.enabled = query_all || query_ipv4;
	ifv6.enabled = query_all || query_ipv6;
}

int main(int argc, cstr argv[])
{
	try
	{
		APPL_NAME = basename_from_path(argv[0]);
		logdir	  = catstr("/var/log/", APPL_NAME);

		cstr config = config_file;

		for (int i = 1; i < argc; i++)
		{
			cstr s = argv[i];
			if (eq(s, "--verbose") || eq(s, "-v"))
			{
				verbose = true;
				continue;
			}
			if (startswith(s, "--fore") || eq(s, "-f"))
			{
				foreground = true;
				continue;
			}
			if (startswith(s, "--back") || eq(s, "-b"))
			{
				background = true;
				continue;
			}
			if (eq(s, "--info") || eq(s, "-i"))
			{
				showinfo = true;
				continue;
			}
			if (*s != '-' && i == argc - 1)
			{
				config = s;
				continue;
			}
			printf(usage);
			return 1;
		}

		if (foreground + background + showinfo != 1)
		{
			printf("either option -f, -b or -i required. use -h for help.\n");
			return 1;
		}

		parse_config(config);

		if (showinfo)
		{
			curl_global_init(CURL_GLOBAL_ALL);
			show_info();
			//curl_global_cleanup();
			return 0;
		}

		if (background)
		{
			daemonize();
			openLogfile(logdir, LOGROTATION, MAXLOGFILES, yes);
		}
		drop_suid();
		curl_global_init(CURL_GLOBAL_ALL);
		dyndns_updater();
		//curl_global_cleanup();
	}
	catch (std::exception& e)
	{
		abort("%s: %s", APPL_NAME, e.what());
	}
	catch (cstr& e)
	{
		abort("%s: %s", APPL_NAME, e);
	}
	catch (...)
	{
		abort("%s: unknown exception", APPL_NAME);
	}
}


/*






























  
*/
