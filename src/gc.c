/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Garbage collection routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "gc.h"
#include "shmem.h"
#include "timers.h"
#include "config/config.h"
#include "overTime.h"
#include "database/common.h"
#include "log.h"
// global variable killed
#include "signals.h"
// data getter functions
#include "datastructure.h"
// delete_old_queries_from_db()
#include "database/query-table.h"
// logg_rate_limit_message()
#include "database/message-table.h"
// get_nprocs()
#include <sys/sysinfo.h>
// get_path_usage()
#include "files.h"
// void calc_cpu_usage()
#include "daemon.h"
// create_inotify_watcher()
#include "config/inotify.h"

// Resource checking interval
// default: 300 seconds
#define RCinterval 300

bool doGC = false;

// Subtract rate-limitation count from individual client counters
// As long as client->rate_limit is still larger than the allowed
// maximum count, the rate-limitation will just continue
static void reset_rate_limiting(void)
{
	for(int clientID = 0; clientID < counters->clients; clientID++)
	{
		clientsData *client = getClient(clientID, true);
		if(!client)
			continue;

		// Check if we are currently rate-limiting this client
		if(client->flags.rate_limited)
		{
			const char *clientIP = getstr(client->ippos);

			// Check if we want to continue rate limiting
			if(client->rate_limit > config.dns.rateLimit.count.v.ui)
			{
				log_info("Still rate-limiting %s as it made additional %u queries", clientIP, client->rate_limit);
			}
			// or if rate-limiting ends for this client now
			else
			{
				log_info("Ending rate-limitation of %s", clientIP);
				         client->flags.rate_limited = false;
			}
		}

		// Reset counter
		client->rate_limit = 0;
	}
}

static time_t lastRateLimitCleaner = 0;
// Returns how many more seconds until the current rate-limiting interval is over
time_t get_rate_limit_turnaround(const unsigned int rate_limit_count)
{
	const unsigned int how_often = rate_limit_count/config.dns.rateLimit.count.v.ui;
	return (time_t)config.dns.rateLimit.interval.v.ui*how_often - (time(NULL) - lastRateLimitCleaner);
}

static int check_space(const char *file, unsigned int LastUsage)
{
	if(config.misc.check.disk.v.ui == 0)
		return 0;

	unsigned int perc = 0;
	char buffer[64] = { 0 };
	// Warn if space usage at the device holding the corresponding file
	// exceeds the configured threshold and current usage is higher than
	// usage in the last run (to prevent log spam)
	perc = get_path_usage(file, buffer);
	log_debug(DEBUG_GC, "Checking free space at %s: %u%% %s %u%%", file, perc,
	          perc > config.misc.check.disk.v.ui ? ">" : "<=",
	          config.misc.check.disk.v.ui);
	if(perc > config.misc.check.disk.v.ui && perc > LastUsage && perc <= 100.0)
		log_resource_shortage(-1.0, 0, -1, perc, file, buffer);

	return perc;
}

static void check_load(void)
{
	if(!config.misc.check.load.v.b)
		return;

	// Get CPU load averages
	double load[3];
	if (getloadavg(load, 3) == -1)
		return;

	// Get number of CPU cores
	const int nprocs = get_nprocs();

	// Warn if 15 minute average of load exceeds number of available
	// processors
	if(load[2] > nprocs)
		log_resource_shortage(load[2], nprocs, -1, -1, NULL, NULL);
}

void runGC(const time_t now, time_t *lastGCrun, const bool flush)
{
	doGC = false;
	// Update lastGCrun timer
	if(lastGCrun != NULL)
		*lastGCrun = now - GCdelay - (now - GCdelay)%GCinterval;

	// Lock FTL's data structure, since it is likely that it will be changed here
	// Requests should not be processed/answered when data is about to change
	if(!flush)
		lock_shm();

	// Get minimum timestamp to keep
	time_t mintime = now;
	if(!flush)
	{
		// Normal GC run
		mintime -= GCdelay + config.webserver.api.maxHistory.v.ui;

		// Align the start time of this GC run to the GCinterval. This will also align with the
		// oldest overTime interval after GC is done.
		mintime -= mintime % GCinterval;
	}

	if(config.debug.gc.v.b)
	{
		timer_start(GC_TIMER);
		char timestring[TIMESTR_SIZE] = "";
		get_timestr(timestring, mintime, false, false);
		log_debug(DEBUG_GC, "GC starting, mintime: %s (%lu), counters->queries = %d",
		          timestring, (unsigned long)mintime, counters->queries);
	}

	// Process all queries
	int removed = 0;
	for(long int i=0; i < counters->queries; i++)
	{
		queriesData* query = getQuery(i, true);
		if(query == NULL)
			continue;

		// Test if this query is too new
		if(query->timestamp > mintime)
			break;

		// Adjust client counter (total and overTime)
		clientsData* client = getClient(query->clientID, true);
		const int timeidx = getOverTimeID(query->timestamp);
		overTime[timeidx].total--;
		if(client != NULL)
			change_clientcount(client, -1, 0, timeidx, -1);

		// Adjust domain counter (no overTime information)
		domainsData* domain = getDomain(query->domainID, true);
		if(domain != NULL)
			domain->count--;

		// Get upstream pointer

		// Change other counters according to status of this query
		switch(query->status)
		{
			case QUERY_UNKNOWN:
				// Unknown (?)
				break;
			case QUERY_FORWARDED: // (fall through)
			case QUERY_RETRIED: // (fall through)
			case QUERY_RETRIED_DNSSEC:
				// Forwarded to an upstream DNS server
				break;
			case QUERY_CACHE:
			case QUERY_CACHE_STALE:
				// Answered from local cache _or_ local config
				break;
			case QUERY_GRAVITY: // Blocked by Pi-hole's blocking lists (fall through)
			case QUERY_DENYLIST: // Exact blocked (fall through)
			case QUERY_REGEX: // Regex blocked (fall through)
			case QUERY_EXTERNAL_BLOCKED_IP: // Blocked by upstream provider (fall through)
			case QUERY_EXTERNAL_BLOCKED_NXRA: // Blocked by upstream provider (fall through)
			case QUERY_EXTERNAL_BLOCKED_NULL: // Blocked by upstream provider (fall through)
			case QUERY_GRAVITY_CNAME: // Gravity domain in CNAME chain (fall through)
			case QUERY_REGEX_CNAME: // Regex denied domain in CNAME chain (fall through)
			case QUERY_DENYLIST_CNAME: // Exactly denied domain in CNAME chain (fall through)
			case QUERY_DBBUSY: // Blocked because gravity database was busy
			case QUERY_SPECIAL_DOMAIN: // Blocked by special domain handling
				overTime[timeidx].blocked--;
				if(domain != NULL)
					domain->blockedcount--;
				if(client != NULL)
					change_clientcount(client, 0, -1, -1, 0);
				break;
			case QUERY_IN_PROGRESS: // Don't have to do anything here
			case QUERY_STATUS_MAX: // fall through
			default:
				/* That cannot happen */
				break;
		}

		// Update reply counters
		counters->reply[query->reply]--;

		// Update type counters
		counters->querytype[query->type]--;

		// Subtract UNKNOWN from the counters before
		// setting the status if different.
		// Minus one here and plus one below = net zero
		if(query->status != QUERY_UNKNOWN)
			counters->status[QUERY_UNKNOWN]--;

		// Set query again to UNKNOWN to reset the counters
		query_set_status(query, QUERY_UNKNOWN);

		// Count removed queries
		removed++;
	}

	// Remove query from queries table (temp), we can release the lock for this
	// action to prevent blocking the DNS service too long
	if(!flush)
		unlock_shm();
	delete_old_queries_from_db(true, mintime);
	if(!flush)
		lock_shm();

	// Only perform memory operations when we actually removed queries
	if(removed > 0)
	{
		// Move memory forward to keep only what we want
		// Note: for overlapping memory blocks, memmove() is a safer approach than memcpy()
		// Example: (I = now invalid, X = still valid queries, F = free space)
		//   Before: IIIIIIXXXXFF
		//   After:  XXXXFFFFFFFF
		queriesData *dest = getQuery(0, true);
		// Note: we use "removed - 1" here because the ID of the last query is "counters->queries - 1"
		queriesData *src = getQuery(removed - 1, true);
		if(dest && src)
			memmove(dest, src, (counters->queries - removed)*sizeof(queriesData));

		// Update queries counter
		counters->queries -= removed;

		// ensure remaining memory is zeroed out (marked as "F" in the above example)
		queriesData *tail = getQuery(counters->queries, true);
		if(tail)
			memset(tail, 0, (counters->queries_MAX - counters->queries)*sizeof(queriesData));
	}

	// Determine if overTime memory needs to get moved
	moveOverTimeMemory(mintime);

	log_debug(DEBUG_GC, "GC removed %i queries (took %.2f ms)", removed, timer_elapsed_msec(GC_TIMER));

	// Release thread lock
	if(!flush)
		unlock_shm();

	// After storing data in the database for the next time,
	// we should scan for old entries, which will then be deleted
	// to free up pages in the database and prevent it from growing
	// ever larger and larger
	DBdeleteoldqueries = true;
}

static bool check_files_on_same_device(const char *path1, const char *path2)
{
	struct stat s1, s2;
	if(stat(path1, &s1) != 0 || stat(path2, &s2) != 0)
	{
		log_warn("check_files_on_same_device(): stat() failed: %s", strerror(errno));
		return false;
	}

	return s1.st_dev == s2.st_dev;
}

void *GC_thread(void *val)
{
	// Set thread name
	thread_names[GC] = "housekeeper";
	thread_running[GC] = true;
	prctl(PR_SET_NAME, thread_names[GC], 0, 0, 0);

	// Remember when we last ran the actions
	time_t lastGCrun = time(NULL) - time(NULL)%GCinterval;
	lastRateLimitCleaner = time(NULL);
	time_t lastResourceCheck = 0;

	// Remember disk usage
	unsigned int LastLogStorageUsage = 0;
	unsigned int LastDBStorageUsage = 0;

	bool db_and_log_on_same_dev = false;
	db_and_log_on_same_dev = check_files_on_same_device(config.files.database.v.s, config.files.log.ftl.v.s);

	// Create inotify watcher for pihole.toml config file
	watch_config(true);

	// Run as long as this thread is not canceled
	while(!killed)
	{
		const time_t now = time(NULL);
		if(config.dns.rateLimit.interval.v.ui > 0 &&
		   (unsigned int)(now - lastRateLimitCleaner) >= config.dns.rateLimit.interval.v.ui)
		{
			lastRateLimitCleaner = now;
			lock_shm();
			reset_rate_limiting();
			unlock_shm();
		}

		// Intermediate cancellation-point
		if(killed)
			break;

		// Calculate average CPU usage
		// This is done every second to get averaged values
		calc_cpu_usage();

		// Check available resources
		if(now - lastResourceCheck >= RCinterval)
		{
			// Check load averages
			check_load();

			// Check disk space of database file
			LastDBStorageUsage = check_space(config.files.database.v.s, LastDBStorageUsage);

			// Check disk space of log file only if they are not on
			// the same file system
			if(!db_and_log_on_same_dev)
				LastLogStorageUsage = check_space(config.files.log.ftl.v.s, LastLogStorageUsage);

			lastResourceCheck = now;
		}

		// Intermediate cancellation-point
		if(killed)
			break;

		if(now - GCdelay - lastGCrun >= GCinterval || doGC)
			runGC(now, &lastGCrun, false);

		// Intermediate cancellation-point
		if(killed)
			break;

		// Check if pihole.toml has been modified
		if(check_inotify_event())
		{
			// Reload config
			log_info("Reloading config due to pihole.toml change");
			reread_config();
		}

		thread_sleepms(GC, 1000);
	}

	// Close inotify watcher
	watch_config(false);

	log_info("Terminating GC thread");
	thread_running[GC] = false;
	return NULL;
}
