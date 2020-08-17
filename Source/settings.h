#pragma once


// Application name
extern const char* APPL_NAME; // "dyndns_updater"

// use log.cpp:
#define LOGFILE
// Logfile settings:
#define LOGROTATION		MONTHLY
#define MAXLOGFILES		12

// use TempMem and Logger for single-threaded Application:
#define NO_THREADS
