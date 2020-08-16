#pragma once


// Application name
extern const char* APPL_NAME; // "dyndns_updater"

// Logfile settings:
#define LOGROTATION		MONTHLY
#define MAXLOGFILES		12

// we use log.cpp:
#define LOGFILE
#define LOGFILE_BASE_DIRECTORY	"/var/log/"
#define LOGFILE_AUX_DIRECTORY	"/var/log/"		// don't silently fallback to /tmp/

// use TempMem and Logger for single-threaded Application:
#define NO_THREADS
