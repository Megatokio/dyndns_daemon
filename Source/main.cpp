/*	Copyright  (c)	Günter Woigk 2015 - 2021
					mailto:kio@little-bat.de

	This file is free software.

	Permission to use, copy, modify, distribute, and sell this software
	and its documentation for any purpose is hereby granted without fee,
	provided that the above copyright notice appears in all copies and
	that both that copyright notice, this permission notice and the
	following disclaimer appear in supporting documentation.

	THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT ANY WARRANTY, NOT EVEN THE
	IMPLIED WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE
	AND IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY DAMAGES
	ARISING FROM THE USE OF THIS SOFTWARE,
	TO THE EXTENT PERMITTED BY APPLICABLE LAW.
*/

#include "kio/kio.h"
#include "cstrings/cstrings.h"
#include "unix/files.h"
#include <curl/curl.h>		// https://curl.haxx.se/libcurl/c/
#include "Templates/Array.h"

#undef curl_easy_setopt		// macro that replaces a call with the exact same call
#undef curl_easy_getinfo	// ""

cstr APPL_NAME;
static cstr logdir;

#ifdef RELEASE
static bool foreground = no;
static bool verbose    = yes;
#else
static bool foreground = yes;
static bool verbose    = yes;
#endif

static bool update_ip4 = yes;
static bool update_ip6 = no;  // TODO

static cstr mydomain;
static cstr	updatehost;
static cstr	username;
static cstr	password;
static cstr pingselfurl;

static Array<cstr> getmyiphosts;


// -----------------------------------------------------------------


static size_t store_url_data (void* data, size_t size, size_t nmemb, char** userdata)
{
	// helper:
	// store received data from URL:
	// always appends c-string delimiter 0x00

	size *= nmemb;

	char*& bu = *userdata;

	size_t oldsize = bu ? strlen(bu)-1 : 0;
	size_t newsize = oldsize + size;
	str s = tempstr(uint32(newsize));
	if (bu) memcpy(s,bu,oldsize);
	memcpy(s+oldsize,data,size);
	bu = s;

	return size;
}

static str get_url (cstr url, bool httpauth=no)
{
	// get text from url
	// optionally with username & password
	// returns tempstr if ok
	// returns nullptr on error

	CURL* curl = curl_easy_init();
	if (!curl) throw AnyError("curl_easy_init failed");

	char* bu = nullptr;

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "dyndns_daemon/1.0");	// some servers require the user-agent field
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &store_url_data);	// register function to receive data
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bu);					// pass bu[] to the callback function

	CURLcode res = CURLE_OK;
	if (httpauth)
	{
		if (res==CURLE_OK) res = curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
		if (res==CURLE_OK) res = curl_easy_setopt(curl, CURLOPT_USERNAME, username);
		if (res==CURLE_OK) res = curl_easy_setopt(curl, CURLOPT_PASSWORD, password);
	}

	if (res==CURLE_OK) res = curl_easy_perform(curl);	// do it!
	curl_easy_cleanup(curl);							// always clean up!

	if (res != CURLE_OK)
	{
		logline("%s: %s", url, curl_easy_strerror(res));
		return nullptr; // failed
	}

	return bu;
}

static size_t discard_url_data (void*, size_t size, size_t nmemb, char**)
{
	// helper:
	// discard received data

	return size * nmemb;
}

static bool ping_self_ok (cstr url)
{
	// ping self using the supplied url
	// return true: ok

	CURL* curl = curl_easy_init();
	if (!curl) throw AnyError("curl_easy_init failed");
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &discard_url_data);	// register function to receive data
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 299L);
	curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
	CURLcode res = curl_easy_perform(curl);

	bool ok = res == CURLE_OK;
	if (ok)
	{
		long result = 666;
		res = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result);
		ok = res == CURLE_OK && result >= 200 && result < 400;
		if (!ok) logline("%s: http result code = %li", url, result);
	}
	else
	{
		logline("%s: %s", url, curl_easy_strerror(res));
	}

	curl_easy_cleanup(curl);
	return ok;
}

__attribute__((unused))
static inline cstr ip4addrstr (uint32 ip)
{
	// convert ip address to string
	// in: ip in host native byte order

	return usingstr("%u.%u.%u.%u",ip>>24,(ip>>16)&0xff,(ip>>8)&0xff,ip&0xff);
}

static cstr extract_ip4 (cstr s)
{
	// search for, decode and return ip address:

	for(;;)
	{
		while (*s && !is_dec_digit(*s)) s++;	// search start of decimal number
		if (*s == 0) return nullptr;			// end of text: ip4 not found

		cptr ipstr = s;
		uint dots = 0;
		for(;;)
		{
			uint n = 0;
			while (is_dec_digit(*s)) n = n*10 + uint(*s++) - '0';		// parse decimal number
			if (n>255) break;					// too big

			if (*s!='.' && dots==3) return substr(ipstr,s);		// OK
			if (*s=='.' && dots<3) { dots++; s++; continue; }	// more dot-separated numbers to got
			break;                              // format error
		}

		while(*s=='.' || is_dec_digit(*s)) s++; // skip whatever looked similar to an ip address
	}
}

static cstr get_my_ip (cstr url)
{
	// get my ip from the supplied url
	// returns nullptr on failure
	// returns ip4 address if OK

	str s = get_url(url);
	if (s == nullptr) return nullptr;	// request failed

	cstr ip4 = extract_ip4(s);
	if (verbose && ip4 == nullptr)
	{
		logline("%s: ip4 address not found in response:", url);
		logline("--> %s", escapedstr(leftstr(s,500)));
	}
	return ip4;
}

static cstr get_my_ip()
{
	// get my ip from any host
	// returns ip4 address

	for(;;)
	{
		time_t t0 = now<time_t>();

		for (uint i=0; i < getmyiphosts.count(); i++)
		{
			cstr myip = get_my_ip(getmyiphosts[i]);
			if (myip) return myip;
		}
		logline("no answer from any ip server. network down?");

		// wait 60 seconds:
		// we already had to wait for tcp timeouts
		// but if network down then timeout may be 0!
		time_t dur = now<time_t>() - t0;
		if (60-dur > 0) sleep(uint(60-dur));
	}
}

static bool update_my_ip (cstr newip)
{
	// update my ip:
	// in:         ip in host native byte order
	// return:     true=ok // "good 1.2.3.4" or "nochg 1.2.3.4" or "nochg"
	// or error:   badsys badagent badauth !donator notfqdn nohost !yours numhost abuse dnserr 911

	if (startswith(newip,"10.") || startswith(newip,"127.") || startswith(newip,"0") || startswith(newip,"192."))
		throw AnyError("bad ip: %s", newip);	// configuration error

	str hostname = curl_escape(mydomain,0);
	cstr url = catstr(updatehost, "?hostname=", hostname, "&myip=", newip);
	curl_free(hostname); hostname=nullptr;

	str result = get_url(url,yes/*auth*/);
	if (!result || !*result) return no; // failed

	// "good 1.2.3.4" oder "nochg 1.2.3.4" oder "nochg"
	// oder: badsys badagent badauth !donator notfqdn nohost !yours numhost abuse dnserr 911

	tolower(result);
	bool ok = startswith(result,"good") || startswith(result,"nochg");
	if (verbose || !ok) logline("--> %s", result);
	return ok;
}

__attribute__((noreturn))
static void dyndns_updater()
{
	logline("%s running.",APPL_NAME);

	for(;;)
	{
		TempMemPool z;
		try
		{
			logline("ping self ...");
			while (ping_self_ok(pingselfurl)) { sleep(10); }

			if (ping_self_ok("http://127.0.0.1/"))
			{
				logline("webserver unreachable");
				logline("get_my_ip:");
				cstr newip4 = get_my_ip();
				logline("--> ip = %s", newip4);
				logline("update_my_ip:");
				update_my_ip(newip4);
			}
			else
			{
				logline("webserver not running");
			}
		}
		catch (std::exception& e)
		{
			logline("exception: %s", e.what());
		}
		logline("sleeping (300s) ...");
		sleep(300);
	}
}

static void drop_suid()
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

static void daemonize()
{
	int err = daemon(0,0);
	assert(err==0||err==-1);
	if (err) throw strerror(errno);
	// now forked and pwd="/" and stdin/out/err=/dev/null and no controlling terminal
	// and session leader and group leader and and and ...
	log2console = no;
}

static void parse_config (cstr configfile)
{
	FD fd(configfile);

	for (;;)
	{
		cstr s = fd.read_str();
		if (!s) break;					// end of file
		while(is_space(*s)) s++;
		if (*s=='#') continue;			// comment
		if (*s==0) continue;			// empty line

		cptr sep = strchr(s,':');
		if (!sep) throw AnyError("colon missing: %s", s);

		cstr key = lowerstr(substr(s,sep));

		cstr val = croppedstr(sep+1);
		if (*val==0) continue;			// error
		if (*val=='"') val = unquotedstr(val);

		if (eq(key,"domain"))	{ mydomain = val; continue; }
		if (eq(key,"dyndns"))	{ updatehost = val; continue; }
		if (eq(key,"username"))	{ username = val; continue; }
		if (eq(key,"password")) { password = val; continue; }

		if (eq(key,"verbose"))	{ verbose    = !strchr("0nN",*val); continue; }
		if (eq(key,"daemon"))	{ foreground =  strchr("0nN",*val); continue; }
		if (eq(key,"ip4"))		{ update_ip4 = !strchr("0nN",*val); continue; }
		if (eq(key,"ip6"))		{ update_ip6 = !strchr("0nN",*val); continue; }

		if (eq(key,"pingself")) { pingselfurl = val; continue; }
		if (eq(key,"getmyip"))	{ getmyiphosts.appendifnew(val); continue; }
		if (eq(key,"logdir"))	{ logdir = val; continue; }

		throw AnyError("unknown option: %s", key);
	}

	if (!mydomain) throw AnyError("'domain' missing");
	if (!updatehost) throw AnyError("'dyndns' missing");
	if (!username) throw AnyError("'username' missing");
	if (!password) throw AnyError("'password' missing");

	if (!pingselfurl) pingselfurl = catstr(mydomain,"/");

	getmyiphosts.appendifnew("checkip.dyn.com");
	getmyiphosts.appendifnew("checkip.dyndns.org");
}

int main (int argc, cstr argv[])
{
	try
	{
		APPL_NAME = basename_from_path(argv[0]);
		logdir = catstr("/var/log/", APPL_NAME);

		if (argc > 2 || (argc == 2 && *argv[1] == '-')) // -h or --help or unknown option:
		{
			printf("dyndns_updater (c) 2015-2021 kio@little-bat.de\n"
				   "  https://github.com/Megatokio/dyndns_daemon\n"
				   "  usage: dyndns_updater [configfile]\n");
			return 1;
		}

		cstr config = argc==2 ? argv[1] : quick_fullpath("~/.dyndns.config");
		parse_config(config);

		if (!foreground)
		{
			daemonize();
			openLogfile(logdir, LOGROTATION, MAXLOGFILES, yes);
		}
		drop_suid();
		curl_global_init(CURL_GLOBAL_ALL);
		dyndns_updater();
		//curl_global_cleanup();
	}
	catch (cstr& e)          { abort("%s: unexpected error: %s",APPL_NAME,e); }
	catch (AnyError& e) 	 { abort("%s: unexpected error: %s",APPL_NAME,e.what()); }
	catch (std::exception& e){ abort("%s: unexpected exception: %s",APPL_NAME,e.what()); }
}





























































