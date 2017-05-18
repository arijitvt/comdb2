/*
   Copyright 2015 Bloomberg Finance L.P.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

/*
 * Advanced/Smart/Too-clever-for-its-own-good logging module for comdb2.
 *
 * The aims here are:-
 * - minimal impact on speed when logging is all turned off.
 * - ability to log specific events (e.g. requests from certain sourcs,
 *   certain types of requests, requests that fail in certain ways etc)
 * - ability to log to act.log or a file.
 * - unified interface for sql and tagged requests.
 * - free beer.
 *
 * To make this as fast as possible and accessible in deeply nested routines
 * each thread has a request logging object associated with it whose memory
 * is recycled between requests.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <bdb_api.h>

#include <str0.h>
#include <rtcpu.h>
#include <list.h>
#include <segstr.h>
#include <plbitlib.h>
#include <lockmacro.h>
#include <memory_sync.h>
#include <sysarena.h>

#include <epochlib.h>

#include "comdb2.h"
#include "sqloffload.h"
#include "osqlblockproc.h"
#include <cdb2_constants.h>

#include "nodemap.h"
#include "intern_strings.h"
#include "util.h"
#include "logmsg.h"

enum logevent_type {
    EVENT_PUSH_PREFIX,
    EVENT_POP_PREFIX,
    EVENT_POP_PREFIX_ALL,
    EVENT_PRINT
};

enum { MAXSTMT = 31, NUMSTMTS = 16 };

struct logevent {
    struct logevent *next;
    enum logevent_type type;
};

struct push_prefix_event {
    struct logevent hdr;
    int length; /* -ve if not known, in which case text must be \0
                   terminated */
    const char *text;
};

struct pop_prefix_event {
    struct logevent hdr;
};

struct print_event {
    struct logevent hdr;
    unsigned event_flag;
    int length; /* -ve if not known, in which case text must be \0
                   terminated */
    const char *text;
};

enum { MAX_PREFIXES = 16 };

struct prefix_type {
    char prefix[256];
    int pos;
    int stack[MAX_PREFIXES];
    int stack_pos;
};

struct tablelist {
    struct tablelist *next;
    int count;
    char name[1];
};

struct reqlogger {
    /* get all our memory for logevent structs from the arena */
    struct arena *arena;

    char origin[128];

    /* Everything from here onwards is transient and can be reset
     * with a bzero. */
    int start_transient;

    /* flags that can be set as we progress */
    unsigned reqflags;

    int in_request;
    const char *request_type;
    unsigned event_mask;
    unsigned dump_mask;
    unsigned mask; /* bitwise or of the above two masks */
    int startms;

    struct prefix_type prefix;
    char dumpline[1024];
    int dumplinepos;

    /* list of tables touched by this request */
    int tracking_tables;
    struct tablelist *tables;

    /* our opcode - OP_SQL for sql */
    int opcode;

    struct ireq *iq;

    /* singly linked list of all the stuff we've logged */
    struct logevent *events;
    struct logevent *last_event;

    /* the sql statement */
    const char *stmt;
    char *tags;
    void *tagbuf;
    void *nullbits;

    int sqlrows;
    double sqlcost;

    int rc;
    int durationms;
    int vreplays;
    int queuetimems;
    char fingerprint[16];
};

/* a rage of values to look for */
struct range {
    int from;
    int to;
};

struct dblrange {
    double from;
    double to;
};

/* a list of integers to look for (or exclude) */
enum { LIST_MAX = 32 };
struct list {
    unsigned num;
    unsigned inv; /* flag - if set allow values not in list */
    int list[LIST_MAX];
};

struct output {
    LINKC_T(struct output) linkv;

    int refcount;
    int fd;

    int use_time_prefix;
    int lasttime;
    char timeprefix[17]; /* "dd/mm hh:mm:ss: " */

    /* serialise output so that we don't get */
    pthread_mutex_t mutex;

    char filename[1];
};

/* A set of conditions that must be met to log a request, what we want to
 * log and where we should log it. */
struct logrule {

    /* A name for this rule */
    char name[32];

    int active;

    /* Part 1: conditions.  These are all and conditions (all must be met) */

    int count; /* how many to log.  delete rule after this many */

    struct range duration;
    struct range retries;
    struct range vreplays;
    struct dblrange sql_cost;
    struct range sql_rows;

    struct list rc_list;
    struct list opcode_list;

    char tablename[MAXTABLELEN + 1];

    char stmt[MAXSTMT + 1];

    /* Part 2: what to log */

    unsigned event_mask;

    /* Part 3: where to log it */

    struct output *out;

    /* Keep the rules in a linked list */
    LINKC_T(struct logrule) linkv;
};

/* per client request stats */

/* we normalise our request rate report to this period - we have a bucket for
 * each second. */
enum { NUM_BUCKETS = 10 };
struct nodestats {
    struct nodestats *next;

    char *host;

    /* raw counters, totals (updated locklessly by multiple threads) */
    struct rawnodestats rawtotals;

    /* previous totals for last time quanta */
    struct rawnodestats prevtotals;

    /* keep a diff of reqs/second for th last few seconds so we can
     * caculate a smoothis reqs/second.  this may not get updated regularaly
     * so we record epochms times. */
    unsigned cur_bucket;
    struct rawnodestats raw_buckets[NUM_BUCKETS];
    int bucket_spanms[NUM_BUCKETS];
};

struct summary_nodestats {
    char *host;

    unsigned finds;
    unsigned rngexts;
    unsigned writes;
    unsigned other_fstsnds;

    unsigned adds;
    unsigned upds;
    unsigned dels;
    unsigned bsql;     /* block sql */
    unsigned recom;    /* recom sql */
    unsigned snapisol; /* snapisol sql */
    unsigned serial;   /* serial sql */

    unsigned sql_queries;
    unsigned sql_steps;
    unsigned sql_rows;
};

extern int gbl_time_fdb;

/*
** ugh - constants are variable
** comdb2 assumption #define MAXNODES 32768
** rtcpu.h - #define MAXMACHINES 2048
** intgatewayconstants.h - #define MAXMACHINES 4096
*/

static unsigned num_nodes = 0;
static struct nodestats *nodestats_first = NULL;
static struct nodestats *nodestats_array[MAXNODES] = {NULL};
static pthread_mutex_t nodestats_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t nodestats_calc_lk = PTHREAD_MUTEX_INITIALIZER;

/* The normal case is for there to be no rules, just a long request threshold
 * which takes some default action on long requests.  If you want anything
 * different then you add rules and you have to lock around the list. */
static int long_request_ms = 2000;
static struct output *long_request_out = NULL;
static struct output *bad_cstr_out = NULL;
static pthread_mutex_t rules_mutex;
static LISTC_T(struct logrule) rules;
static LISTC_T(struct output) outputs;

static int long_request_count = 0;
static int last_long_request_epoch = 0;
static int longest_long_request_ms = 0;
static int shortest_long_request_ms = -1;

static struct output *default_out;

static int diffstat_thresh = 60;  /* every minute */
static struct output *stat_request_out = NULL;

/* These global lockless variables define what we will log for all requests
 * just in case the request meets all the criteria to trigger a rule. */
static int master_event_mask = 0;
static int master_all_requests = 0;
static struct list master_opcode_list = {0};
static struct list master_opcode_inv_list = {0};
static int master_table_rules = 0;
static char master_stmts[NUMSTMTS][MAXSTMT + 1];
static int master_num_stmts = 0;

static int reqltruncate = 1;

/* sometimes you have to debug the debugger */
static int verbose = 0;

/* stolen from sltdbt.c */
static int long_reqs = 0;
static int norm_reqs = 0;

/* for the sqldbgtrace message trap */
int sqldbgflag = 0;

static void log_all_events(struct reqlogger *logger, struct output *out);

void sltdbt_get_stats(int *n_reqs, int *l_reqs)
{
    *n_reqs = norm_reqs;
    *l_reqs = long_reqs;
    norm_reqs = long_reqs = 0;
}

/* maintain a logging trace prefix with a stack structure */
static void prefix_init(struct prefix_type *p)
{
    p->pos = 0;
    p->stack_pos = 0;
    p->prefix[0] = '\0';
}

static void prefix_push(struct prefix_type *p, const char *prefix, int len)
{
    if (p->stack_pos < MAX_PREFIXES) {
        p->stack[p->stack_pos] = p->pos;
        if (len + p->pos >= sizeof(p->prefix)) {
            len = (sizeof(p->prefix) - 1) - p->pos;
        }
        memcpy(p->prefix + p->pos, prefix, len);
        p->pos += len;
        p->prefix[p->pos] = '\0';
    }
    p->stack_pos++;
}

static void prefix_pop(struct prefix_type *p)
{
    p->stack_pos--;
    if (p->stack_pos < 0) {
        p->stack_pos = 0;
        p->pos = 0;
        logmsg(LOGMSG_ERROR, "%s: stack pos went -ve!\n", __func__);
    } else if (p->stack_pos < MAX_PREFIXES) {
        p->pos = p->stack[p->stack_pos];
    }
    p->prefix[p->pos] = '\0';
}

static void prefix_pop_all(struct prefix_type *p)
{
    p->stack_pos = 0;
    p->pos = 0;
    p->prefix[p->pos] = '\0';
}

/* This should be called under lock unless the output is the default
 * output (act.log). */
static void flushdump(struct reqlogger *logger, struct output *out)
{
    if (logger->dumplinepos > 0) {
        struct iovec iov[5];
        int niov = 0;
        int append_duration = 0;
        char durstr[16];
        if (!out) {
            out = default_out;
            append_duration = 1;
        }
        if (out->use_time_prefix && out != default_out) {
            int now = time_epoch();
            if (now != out->lasttime) {
                time_t timet = (time_t)now;
                struct tm tm;
                out->lasttime = now;
                localtime_r(&timet, &tm);
                snprintf(out->timeprefix, sizeof(out->timeprefix),
                         "%02d/%02d %02d:%02d:%02d: ", tm.tm_mon + 1,
                         tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
            }
            iov[niov].iov_base = out->timeprefix;
            iov[niov].iov_len = sizeof(out->timeprefix) - 1;
            niov++;
        }
        if (logger->prefix.pos > 0) {
            iov[niov].iov_base = logger->prefix.prefix;
            iov[niov].iov_len = logger->prefix.pos;
            niov++;
        }
        iov[niov].iov_base = logger->dumpline;
        iov[niov].iov_len = logger->dumplinepos;
        niov++;
        if (append_duration) {
            iov[niov].iov_base = durstr;
            iov[niov].iov_len = snprintf(durstr, sizeof(durstr), " TIME +%d",
                                         time_epochms() - logger->startms);
            niov++;
        }
        iov[niov].iov_base = "\n";
        iov[niov].iov_len = 1;
        niov++;
        if (out == default_out) {
            for (int i = 0; i < niov; i++)
                logmsg(LOGMSG_INFO, iov[i].iov_base);
        } else {
            int dum = writev(out->fd, iov, niov);
        }
        logger->dumplinepos = 0;
    }
}

static void dump(struct reqlogger *logger, struct output *out, const char *s,
                 int len)
{
    int ii;
    for (ii = 0; ii < len;) {
        if (logger->dumplinepos >= sizeof(logger->dumpline)) {
            flushdump(logger, out);
            continue;
        }
        if (s[ii] == '\n') {
            flushdump(logger, out);
        } else {
            logger->dumpline[(logger->dumplinepos)++] = s[ii];
        }
        ii++;
    }
}

/* if we provide here an output bigger than 256, we used to print
   the stack in the log file;
   in this new attempt, we're providing a slow path in which we
   malloc/free a buffer large enough to fit the size and preserve
   the output string (and its termination)
 */
static void dumpf(struct reqlogger *logger, struct output *out, const char *fmt,
                  ...)
{
    va_list args;
    char buf[256], *buf_slow = NULL;
    int len;

    va_start(args, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len >= sizeof(buf)) {
        buf_slow = malloc(len + 1); /* with the trailing 0 */
        if (!buf_slow) {
            /* no memory, or asking for too much;
               i am gonna assume we are providing a new line */
            buf[sizeof(buf) - 1] = '\n';
            len = sizeof(buf);
        } else {
            len = vsnprintf(buf_slow, len + 1, fmt, args);
        }
    }
    va_end(args);

    dump(logger, out, (buf_slow) ? buf_slow : buf,
         len); /* don't dump the terminating \0 */

    if (buf_slow)
        free(buf_slow);
}

static void init_range(struct range *range)
{
    range->from = -1;
    range->to = -1;
}

static void init_dblrange(struct dblrange *dblrange)
{
    dblrange->from = -1;
    dblrange->to = -1;
}

static int add_list(struct list *list, int value, unsigned inv)
{
    int ii;
    inv = inv ? ~0 : 0;
    if (inv != list->inv) {
        list->num = 0;
        list->inv = inv;
    }
    for (ii = 0; ii < list->num; ii++) {
        if (list->list[ii] == value) {
            return 0;
        }
    }
    if (list->num >= LIST_MAX) {
        return -1;
    }
    list->list[list->num] = value;
    list->num++;
    return 0;
}

/* see if value matches criteria of the list */
static int check_list(const struct list *list, int value)
{
    unsigned ii;
    if (list->num == 0) {
        /* empty list matches all values */
        return 1;
    }
    for (ii = 0; ii < list->num && ii < LIST_MAX; ii++) {
        if (list->list[ii] == value) {
            return !list->inv;
        }
    }
    return list->inv;
}

static void printlist(FILE *fh, const struct list *list,
                      const char *(*item2a)(int value))
{
    int ii;
    if (list->inv) {
        logmsgf(LOGMSG_USER, fh, "not in ");
    } else {
        logmsgf(LOGMSG_USER, fh, "in ");
    }
    for (ii = 0; ii < list->num; ii++) {
        if (ii > 0) {
            logmsgf(LOGMSG_USER, fh, ", ");
        }
        logmsgf(LOGMSG_USER, fh, "%d", list->list[ii]);
        if (item2a) {
            logmsgf(LOGMSG_USER, fh, " (%s)", item2a(list->list[ii]));
        }
    }
}

/* get and reference a file output */
static struct output *get_output_ll(const char *filename)
{
    struct output *out;
    int fd, len;
    LISTC_FOR_EACH(&outputs, out, linkv)
    {
        if (strcmp(out->filename, filename) == 0) {
            out->refcount++;
            return out;
        }
    }
    fd = open(filename, O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (fd == -1) {
        logmsg(LOGMSG_ERROR, "error opening '%s' for logging: %d %s\n", filename,
                errno, strerror(errno));
        default_out->refcount++;
        return default_out;
    }
    len = strlen(filename);
    out = calloc(offsetof(struct output, filename) + len + 1, 1);
    if (!out) {
        logmsg(LOGMSG_ERROR, "%s: out of memory\n", __func__);
        close(fd);
        default_out->refcount++;
        return default_out;
    }
    logmsg(LOGMSG_INFO, "opened request log file %s\n", filename);
    memcpy(out->filename, filename, len + 1);
    out->use_time_prefix = 1;
    out->refcount = 1;
    out->fd = fd;
    listc_atl(&outputs, out);
    pthread_mutex_init(&out->mutex, NULL);
    return out;
}

/* dereference a file output */
static void deref_output_ll(struct output *out)
{
    out->refcount--;
    if (out->refcount <= 0 && out->fd > 2) {
        close(out->fd);
        logmsg(LOGMSG_INFO, "closed request log file %s\n", out->filename);
        listc_rfl(&outputs, out);
        free(out);
    }
}

/* should be called while you hold the rules_mutex */
static struct logrule *new_rule_ll(const char *name)
{
    struct logrule *rule;

    rule = calloc(sizeof(struct logrule), 1);
    if (!rule) {
        logmsg(LOGMSG_ERROR, "%s: calloc failed\n", __func__);
        return NULL;
    }
    strncpy0(rule->name, name, sizeof(rule->name));

    init_range(&rule->duration);
    init_range(&rule->retries);
    init_dblrange(&rule->sql_cost);
    init_range(&rule->sql_rows);

    rule->out = default_out;
    rule->out->refcount++;

    listc_abl(&rules, rule);
    return rule;
}

static void del_rule_ll(struct logrule *rule)
{
    if (rule) {
        listc_rfl(&rules, rule);
        deref_output_ll(rule->out);
        free(rule);
    }
}

static const char *rangestr(const struct range *range, char *buf, size_t buflen)
{
    if (range->from >= 0 && range->to >= 0) {
        snprintf(buf, buflen, "%d..%d", range->from, range->to);
    } else if (range->from >= 0) {
        snprintf(buf, buflen, ">=%d", range->from);
    } else if (range->to >= 0) {
        snprintf(buf, buflen, "<=%d", range->to);
    } else {
        strncpy0(buf, "<no constraint>", buflen);
    }
    return buf;
}

static const char *dblrangestr(const struct dblrange *range, char *buf,
                               size_t buflen)
{
    if (range->from >= 0 && range->to >= 0) {
        snprintf(buf, buflen, "%f..%f", range->from, range->to);
    } else if (range->from >= 0) {
        snprintf(buf, buflen, ">=%f", range->from);
    } else if (range->to >= 0) {
        snprintf(buf, buflen, "<=%f", range->to);
    } else {
        strncpy0(buf, "<no constraint>", buflen);
    }
    return buf;
}

static void printrule(struct logrule *rule, FILE *fh, const char *p)
{
    char b[32];
    int rc, opcode;
    logmsgf(LOGMSG_USER, fh, "%sRULE '%s'", p, rule->name);
    if (!rule->active)
        logmsgf(LOGMSG_USER, fh, " (INACTIVE)");
    logmsgf(LOGMSG_USER, fh, "\n");
    if (rule->count)
        logmsgf(LOGMSG_USER, fh, "%s  Log next %d requests where:\n", p, rule->count);
    else
        logmsgf(LOGMSG_USER, fh, "%s  Log all requests where:\n", p);
    if (rule->duration.from >= 0 || rule->duration.to >= 0)
        logmsgf(LOGMSG_USER, fh, "%s    duration %s msec\n", p,
                rangestr(&rule->duration, b, sizeof(b)));
    if (rule->retries.from >= 0 || rule->retries.to >= 0)
        logmsgf(LOGMSG_USER, fh, "%s    retries %s\n", p,
                rangestr(&rule->retries, b, sizeof(b)));
    if (rule->vreplays.from >= 0 || rule->vreplays.to >= 0)
        logmsgf(LOGMSG_USER, fh, "%s    verify replays %s\n", p,
                rangestr(&rule->vreplays, b, sizeof(b)));
    if (rule->sql_cost.from >= 0 || rule->sql_cost.to >= 0)
        logmsgf(LOGMSG_USER, fh, "%s    SQL cost %s\n", p,
                dblrangestr(&rule->sql_cost, b, sizeof(b)));
    if (rule->sql_rows.from >= 0 || rule->sql_rows.to >= 0)
        logmsgf(LOGMSG_USER, fh, "%s    SQL rows %s\n", p,
                rangestr(&rule->sql_rows, b, sizeof(b)));
    if (rule->rc_list.num > 0) {
        logmsgf(LOGMSG_USER, fh, "%s    rcode is ", p);
        printlist(fh, &rule->rc_list, NULL);
        logmsgf(LOGMSG_USER, fh, "\n");
    }
    if (rule->opcode_list.num > 0) {
        logmsgf(LOGMSG_USER, fh, "%s    opcode is ", p);
        printlist(fh, &rule->opcode_list, req2a);
        logmsgf(LOGMSG_USER, fh, "\n");
    }
    if (rule->tablename[0]) {
        logmsgf(LOGMSG_USER, fh, "%s    touches table '%s'\n", p, rule->tablename);
    }
    if (rule->stmt[0]) {
        logmsgf(LOGMSG_USER, fh, "%s    sql statement like '%%%s%%'\n", p, rule->stmt);
    }
    if (rule->event_mask & REQL_TRACE)
        logmsgf(LOGMSG_USER, fh, "%s  Logging detailed trace\n", p);
    if (rule->event_mask & REQL_RESULTS)
        logmsgf(LOGMSG_USER, fh, "%s  Logging query results\n", p);
    logmsgf(LOGMSG_USER, fh, "%s  Log to %s\n", p, rule->out->filename);
}

/* scan all the rules and setup our master settings that define what we log
 * for each request.  We want to log as little as possible to be fast, but
 * we have to make sure that we log enough so that if a request matches some
 * of our criteria we can catch it. */
static void scanrules_ll(void)
{
    int ii, rc;
    int table_rules = 0;
    struct logrule *rule;
    unsigned event_mask = 0;
    int log_all_reqs = 0;
    master_all_requests = 1;
    master_num_stmts = 0;
    bzero(&master_opcode_list, sizeof(master_opcode_list));
    bzero(&master_opcode_inv_list, sizeof(master_opcode_inv_list));
    bzero(master_stmts, NUMSTMTS * (MAXSTMT + 1));
    LISTC_FOR_EACH(&rules, rule, linkv)
    {
        /* ignore inactive rules */
        if (!rule->active) {
            continue;
        }
        /* if the rule doesn't have any criteria that can be tested before the
         * request starts, then we definateky have to log for all requests */
        if (rule->opcode_list.num == 0 && rule->stmt[0] == 0) {
            log_all_reqs = 1;
        }
        /* if the rule has opcode criteria then build a list of opcodes to
         * allow through */
        for (ii = 0; ii < rule->opcode_list.num; ii++) {
            if (rule->opcode_list.inv) {
                rc = add_list(&master_opcode_inv_list,
                              rule->opcode_list.list[ii], 1);
            } else {
                rc = add_list(&master_opcode_list, rule->opcode_list.list[ii],
                              0);
            }
            if (rc != 0) {
                log_all_reqs = 1;
            }
        }
        /* if the rule has table name criteria then we must track tables used */
        if (rule->tablename[0]) {
            table_rules = 1;
        }
        /* if the rule has sql stmt criteria then add it to the list */
        if (rule->stmt[0]) {
            if (master_num_stmts == NUMSTMTS) {
                log_all_reqs = 1;
            } else {
                strncpy(master_stmts[master_num_stmts], rule->stmt, MAXSTMT);
                master_num_stmts++;
            }
        }
        event_mask |= rule->event_mask;
    }
    master_event_mask = event_mask;
    master_table_rules = table_rules;
    master_all_requests = log_all_reqs;
    if (verbose) {
        logmsg(LOGMSG_USER, "%s: master_event_mask=0x%x\n", __func__, master_event_mask);
        logmsg(LOGMSG_USER, "%s: master_table_rules=%d\n", __func__, master_table_rules);
        logmsg(LOGMSG_USER, "%s: master_all_requests=%d\n", __func__, master_all_requests);
        logmsg(LOGMSG_USER, "%s: master_opcode_inv_list: ", __func__);
        printlist(stdout, &master_opcode_inv_list, NULL);
        logmsg(LOGMSG_USER, "\n");
        logmsg(LOGMSG_USER, "%s: master_opcode_list: ", __func__);
        printlist(stdout, &master_opcode_list, NULL);
        logmsg(LOGMSG_USER, "\n");
        for (ii = 0; ii < master_num_stmts; ii++) {
           logmsg(LOGMSG_USER, "master_stmts[%d] = '%s'\n", ii, master_stmts[ii]);
        }
    }
}

int reqlog_init(const char *dbname)
{
    struct logrule *rule;
    struct output *out;
    char *filename;

    pthread_mutex_init(&rules_mutex, NULL);
    listc_init(&rules, offsetof(struct logrule, linkv));
    listc_init(&outputs, offsetof(struct output, linkv));

    out = calloc(sizeof(struct output) + strlen("<stdout>") + 1, 1);
    if (!out) {
        logmsg(LOGMSG_ERROR, "%s:calloc failed\n", __func__);
        return -1;
    }
    strcpy(out->filename, "<stdout>");
    out->fd = 2;
    out->refcount = 1;
    default_out = out;
    listc_atl(&outputs, out);
    pthread_mutex_init(&out->mutex, NULL);

    filename = comdb2_location("logs", "%s.longreqs", dbname);
    long_request_out = get_output_ll(filename);
    free(filename);

    filename = comdb2_location("logs", "%s.statreqs", dbname);
    stat_request_out = get_output_ll(filename);
    free(filename);

    scanrules_ll();
    return 0;
}

static const char *help_text[] = {
    "Request logging framework commands",
    "reql longrequest #           - set long request threshold in msec",
    "reql longsqlrequest #        - set long SQL request threshold in msec",
    "reql longreqfile <filename>  - set file to log long requests in",
    "reql diffstat #              - set diff stat threshold in sec",
    "reql truncate #              - set request truncation",
    "reql stat               - status, print rules",
    "reql [rulename] ...     - add/modify rules.  The default rule is '0'.",
    "                          Valid rule names begin with a digit or '.'.",
    "   General commands:", "       delete           - delete named rule",
    "       go               - start logging with rule",
    "       stop             - stop logging with this rule",
    "   Specify criteria:",
    "       opcode [!]#      - log regular requests with opcode [other than] #",
    "       rc [!]#          - log requests with rcode [other than] #",
    "       ms <range>       - log requests within a range of msecs",
    "       retries <range>  - log requests with that many retries",
    "       cost <range>     - log SQL requests with the given cost",
    "       rows <range>     - log SQL requests with the given row count",
    "       table <name>     - log requests that touch given table",
    "       stmt 'sql stmt'  - log requests where sql contains that text",
    "       vreplays <range> - log requests with given number of verify "
    "replays",
    "   Specify what to log:", "       trace            - log detailed trace",
    "       results          - log query results",
    "       cnt #            - log up to # before removing rule",
    "   Specify where to log:",
    "       file <filename>  - log to filename rather than stdout",
    "       stdout           - log to stdout",
    "<range> is a range specification.  valid range specifications are:-",
    "   #+                   - match any number >=#",
    "   #-                   - match any number <=#",
    "   #..#                 - match anything between the two numbers "
    "inclusive",
    "<filename> must be a filename or the keyword '<stdout>'", NULL};

void reqlog_help(void)
{
    int ii;
    for (ii = 0; help_text[ii]; ii++) {
        logmsg(LOGMSG_USER, "%s\n", help_text[ii]);
    }
}

/* parse a range specification */
static int parse_range_tok(struct range *range, char *tok, int ltok)
{
    if (ltok > 0) {
        if (tok[ltok - 1] == '-') {
            range->from = -1;
            range->to = toknum(tok, ltok - 1);
            return 0;
        } else if (tok[ltok - 1] == '+') {
            range->from = toknum(tok, ltok - 1);
            range->to = -1;
            return 0;
        } else {
            int ii;
            for (ii = 0; ii < ltok - 1; ii++) {
                if (tok[ii] == '.' && tok[ii + 1] == '.') {
                    int end;
                    for (end = ii + 2; end < ltok && tok[end] == '.'; end++)
                        ;
                    range->from = toknum(tok, ii);
                    range->to = toknum(tok + end, ltok - end);
                    return 0;
                }
            }
        }
    }
    logmsg(LOGMSG_ERROR, "bad range specification '%*.*s'\n", ltok, ltok, tok);
    return -1;
}

static int parse_dblrange_tok(struct dblrange *dblrange, char *tok, int ltok)
{
    struct range range;
    int rc;

    rc = parse_range_tok(&range, tok, ltok);
    if (rc)
        return rc;
    dblrange->from = range.from;
    dblrange->to = range.to;
    return 0;
}

static void tokquoted(char *line, int lline, int *st, char *buf, size_t bufsz)
{
    int stage = 0;
    char quote = 0;
    if (bufsz == 0) {
        return;
    }
    while (bufsz > 0 && *st < lline) {
        char ch = line[*st];
        switch (stage) {
        case 0:
            /* scan for start */
            if (ch == '\'' || ch == '"') {
                quote = ch;
                stage = 2;
                break;
            } else if (isspace(ch)) {
                break;
            }
            stage = 1;
        /* fall through; found first character of line */
        case 1:
            /* unquoted text, scan for next whitespace */
            if (isspace(ch)) {
                goto end;
            }
            *buf = ch;
            buf++;
            bufsz--;
            break;
        case 2:
            /* quoted text */
            if (ch == quote) {
                if ((*st) + 1 < lline && buf[(*st) + 1] == ch) {
                    (*st)++;
                } else {
                    (*st)++;
                    goto end;
                }
            }
            *buf = ch;
            buf++;
            bufsz--;
            break;
        }
        (*st)++;
    }
end:
    if (bufsz == 0) {
        bufsz--;
        buf--;
    }
    *buf = 0;
}

void reqlog_process_message(char *line, int st, int lline)
{
    char *tok;
    int ltok;
    tok = segtok(line, lline, &st, &ltok);
    if (tokcmp(tok, ltok, "longrequest") == 0) {
        tok = segtok(line, lline, &st, &ltok);
        long_request_ms = toknum(tok, ltok);
        logmsg(LOGMSG_USER, "Long request threshold now %d msec\n", long_request_ms);
    } else if (tokcmp(tok, ltok, "longsqlrequest") == 0) {
        tok = segtok(line, lline, &st, &ltok);
        gbl_sql_time_threshold = toknum(tok, ltok);
        logmsg(LOGMSG_USER, "Long SQL request threshold now %d msec\n",
               gbl_sql_time_threshold);
    } else if (tokcmp(tok, ltok, "longreqfile") == 0) {
        char filename[128];
        struct output *out;
        tok = segtok(line, lline, &st, &ltok);
        tokcpy0(tok, ltok, filename, sizeof(filename));
        out = get_output_ll(filename);
        if (out) {
            deref_output_ll(long_request_out);
            long_request_out = out;
        }
    } else if (tokcmp(tok, ltok, "diffstat") == 0) {
        tok = segtok(line, lline, &st, &ltok);
        if (ltok == 0) {
            reqlog_help();
        } else {
            reqlog_set_diffstat_thresh(toknum(tok, ltok));
        }
    } else if (tokcmp(tok, ltok, "truncate") == 0) {
        tok = segtok(line, lline, &st, &ltok);
        if (ltok == 0) {
            reqlog_help();
        } else {
            reqlog_set_truncate(toknum(tok, ltok));
        }
    } else if (tokcmp(tok, ltok, "stat") == 0) {
        reqlog_stat();
    } else if (tokcmp(tok, ltok, "help") == 0) {
        reqlog_help();
    } else if (tokcmp(tok, ltok, "vbon") == 0) {
        verbose = 1;
    } else if (tokcmp(tok, ltok, "vbof") == 0) {
        verbose = 0;
    } else if (ltok == 0) {
        logmsg(LOGMSG_ERROR, "huh?\n");
    } else {
        char rulename[32];
        struct logrule *rule;

        if (isdigit(tok[0]) || tok[0] == '.') {
            tokcpy0(tok, ltok, rulename, sizeof(rulename));
            tok = segtok(line, lline, &st, &ltok);
        } else {
            strncpy0(rulename, "0", sizeof(rulename));
        }
        if (verbose) {
            logmsg(LOGMSG_USER, "rulename='%s'\n", rulename);
        }

        pthread_mutex_lock(&rules_mutex);
        LISTC_FOR_EACH(&rules, rule, linkv)
        {
            if (strcmp(rulename, rule->name) == 0) {
                break;
            }
        }
        if (!rule) {
            rule = new_rule_ll(rulename);
            if (!rule) {
                logmsg(LOGMSG_ERROR, "error creating new rule %s\n", rulename);
                pthread_mutex_unlock(&rules_mutex);
                return;
            }
        }
        while (ltok > 0) {
            if (tokcmp(tok, ltok, "go") == 0) {
                rule->active = 1;
            } else if (tokcmp(tok, ltok, "stop") == 0) {
                rule->active = 0;
            } else if (tokcmp(tok, ltok, "delete") == 0) {
                del_rule_ll(rule);
                rule = NULL;
                logmsg(LOGMSG_USER, "Rule deleted\n");
                break;
            } else if (tokcmp(tok, ltok, "cnt") == 0) {
                tok = segtok(line, lline, &st, &ltok);
                rule->count = toknum(tok, ltok);
            } else if (tokcmp(tok, ltok, "file") == 0) {
                char filename[128];
                struct output *out;
                tok = segtok(line, lline, &st, &ltok);
                tokcpy0(tok, ltok, filename, sizeof(filename));
                out = get_output_ll(filename);
                if (out) {
                    deref_output_ll(rule->out);
                    rule->out = out;
                }
            } else if (tokcmp(tok, ltok, "stdout") == 0) {
                struct output *out;
                out = default_out;
                out->refcount++;
                deref_output_ll(rule->out);
                rule->out = out;
            } else if (tokcmp(tok, ltok, "ms") == 0) {
                tok = segtok(line, lline, &st, &ltok);
                parse_range_tok(&rule->duration, tok, ltok);
            } else if (tokcmp(tok, ltok, "retries") == 0) {
                tok = segtok(line, lline, &st, &ltok);
                parse_range_tok(&rule->retries, tok, ltok);
            } else if (tokcmp(tok, ltok, "vreplays") == 0) {
                tok = segtok(line, lline, &st, &ltok);
                parse_range_tok(&rule->vreplays, tok, ltok);
            } else if (tokcmp(tok, ltok, "cost") == 0) {
                tok = segtok(line, lline, &st, &ltok);
                parse_dblrange_tok(&rule->sql_cost, tok, ltok);
            } else if (tokcmp(tok, ltok, "rows") == 0) {
                tok = segtok(line, lline, &st, &ltok);
                parse_range_tok(&rule->sql_rows, tok, ltok);
            } else if (tokcmp(tok, ltok, "sql") == 0) {
                add_list(&rule->opcode_list, OP_SQL, 0);
            } else if (tokcmp(tok, ltok, "stmt") == 0) {
                tokquoted(line, lline, &st, rule->stmt, sizeof(rule->stmt));
            } else if (tokcmp(tok, ltok, "opcode") == 0) {
                int opcode;
                char opname[32];
                int inv = 0;
                tok = segtok(line, lline, &st, &ltok);
                if (ltok > 0 && tok[0] == '!') {
                    tok++;
                    ltok--;
                    inv = 1;
                }
                tokcpy0(tok, ltok, opname, sizeof(opname));
                opcode = a2req(opname);
                if (opcode >= 0 && opcode < MAXTYPCNT) {
                    add_list(&rule->opcode_list, opcode, inv);
                }
            } else if (tokcmp(tok, ltok, "rc") == 0) {
                int rc;
                int inv = 0;
                tok = segtok(line, lline, &st, &ltok);
                if (ltok > 0 && tok[0] == '!') {
                    tok++;
                    ltok--;
                    inv = 1;
                }
                rc = toknum(tok, ltok);
                add_list(&rule->rc_list, rc, inv);
            } else if (tokcmp(tok, ltok, "table") == 0) {
                tok = segtok(line, lline, &st, &ltok);
                tokcpy0(tok, ltok, rule->tablename, sizeof(rule->tablename));
            } else if (tokcmp(tok, ltok, "trace") == 0) {
                rule->event_mask |= REQL_TRACE;
            } else if (tokcmp(tok, ltok, "results") == 0) {
                rule->event_mask |= REQL_RESULTS;
            } else {
                logmsg(LOGMSG_ERROR, "unknown rule command <%*.*s>\n", ltok, ltok,
                        tok);
            }
            tok = segtok(line, lline, &st, &ltok);
        }
        if (rule) {
            printrule(rule, stdout, "");
        }
        scanrules_ll();
        pthread_mutex_unlock(&rules_mutex);
    }
}

void reqlog_stat(void)
{
    struct logrule *rule;
    struct output *out;
    logmsg(LOGMSG_USER, "Long request threshold : %d msec (%dmsec  for SQL)\n",
           long_request_ms, gbl_sql_time_threshold);
    logmsg(LOGMSG_USER, "Long request log file  : %s\n", long_request_out->filename);
    logmsg(LOGMSG_USER, "diffstat threshold     : %d s\n", diffstat_thresh);
    logmsg(LOGMSG_USER, "diffstat log file      : %s\n", stat_request_out->filename);
    logmsg(LOGMSG_USER, "request truncation     : %s\n",
           reqltruncate ? "enabled" : "disabled");
    logmsg(LOGMSG_USER, "SQL cost thresholds    :\n");
    logmsg(LOGMSG_USER, "   trace               : ");
    if (gbl_sql_cost_trace_threshold == -1)
        logmsg(LOGMSG_USER, "not set\n");
    else
        logmsg(LOGMSG_USER, "%f\n", gbl_sql_cost_trace_threshold);
    logmsg(LOGMSG_USER, "   warn                : ");
    if (gbl_sql_cost_warn_threshold == -1)
        logmsg(LOGMSG_USER, "not set\n");
    else
        logmsg(LOGMSG_USER, "%f\n", gbl_sql_cost_warn_threshold);
    logmsg(LOGMSG_USER, "   error               : ");
    if (gbl_sql_cost_error_threshold == -1)
        logmsg(LOGMSG_USER, "not set\n");
    else
        logmsg(LOGMSG_USER, "%f\n", gbl_sql_cost_error_threshold);
    pthread_mutex_lock(&rules_mutex);
    logmsg(LOGMSG_USER, "%d rules currently active\n", rules.count);
    LISTC_FOR_EACH(&rules, rule, linkv) { printrule(rule, stdout, ""); }
    LISTC_FOR_EACH(&outputs, out, linkv) {
        logmsg(LOGMSG_USER, "Output file open: %s\n", out->filename);
    }
    pthread_mutex_unlock(&rules_mutex);
}

struct reqlogger *reqlog_alloc(void)
{
    struct reqlogger *logger;
    logger = calloc(sizeof(struct reqlogger), 1);
    if (!logger) {
        logmsg(LOGMSG_ERROR, "%s: calloc failed\n", __func__);
        return NULL;
    }

    logger->arena = arena_new(malloc, free, 64 * 1024, 64 * 1024);
    if (!logger->arena) {
        logmsg(LOGMSG_ERROR, "%s: arena_new failed\n", __func__);
        free(logger);
        return NULL;
    }

    return logger;
}

void reqlog_free(struct reqlogger *reqlogger)
{
    if (reqlogger) {
        arena_destroy(reqlogger->arena);
        free(reqlogger);
    }
}

void reqlog_reset_logger(struct reqlogger *logger)
{
    if (logger) {
        arena_free_all(logger->arena);
        bzero(&logger->start_transient,
              sizeof(struct reqlogger) -
                  offsetof(struct reqlogger, start_transient));
    }
}

/* append an event onto the list of events for this logger */
static void reqlog_append_event(struct reqlogger *logger,
                                enum logevent_type type, void *voidevent)
{
    struct logevent *event = voidevent;
    event->type = type;
    event->next = NULL;
    if (logger->events) {
        logger->last_event->next = event;
        logger->last_event = event;
    } else {
        logger->events = logger->last_event = event;
    }
}

/* push an output trace prefix */
int reqlog_pushprefixv(struct reqlogger *logger, const char *fmt, va_list args)
{
    if (logger) {
        char buf[256];
        char *s = NULL;
        int len;
        va_list args_c;

        va_copy(args_c, args);
        len = vsnprintf(buf, sizeof(buf), fmt, args);

        if (len >= sizeof(buf) && reqltruncate == 0) {
            s = arena_alloc_align(logger->arena, len + 1,
                                  SYSARENA_1_BYTE_ALIGN);
            if (!s) {
                logmsg(LOGMSG_ERROR, "%s:arena_alloc(%d) failed\n", __func__,
                        len + 1);
                va_end(args_c);
                return -1;
            }
            len = vsnprintf(s, len + 1, fmt, args_c);
        } else if (len >= sizeof(buf)) {
            len = sizeof(buf) - 1;
        }
        va_end(args_c);

        if (logger->dump_mask) {
            flushdump(logger, NULL);
            prefix_push(&logger->prefix, s ? s : buf, len);
        }

        if (logger->event_mask) {
            struct push_prefix_event *event;

            if (!s) {
                s = arena_alloc_align(logger->arena, len + 1,
                                      SYSARENA_1_BYTE_ALIGN);
                if (!s) {
                    logmsg(LOGMSG_ERROR, "%s:arena_alloc(%d) failed\n", __func__,
                            len + 1);
                    return -1;
                }
                memcpy(s, buf, len + 1);
            }

            event =
                arena_alloc(logger->arena, sizeof(struct push_prefix_event));
            if (!event) {
                logmsg(LOGMSG_ERROR, "%s:arena_alloc failed\n", __func__);
                return -1;
            }
            event->length = len;
            event->text = s;
            reqlog_append_event(logger, EVENT_PUSH_PREFIX, event);
        }
    }
    return 0;
}

int reqlog_pushprefixf(struct reqlogger *logger, const char *fmt, ...)
{
    va_list args;
    int rc;

    va_start(args, fmt);
    rc = reqlog_pushprefixv(logger, fmt, args);
    va_end(args);
    return rc;
}

/* pop an output trace prefix */
int reqlog_popprefix(struct reqlogger *logger)
{
    if (logger) {
        if (logger->dump_mask) {
            flushdump(logger, NULL);
            prefix_pop(&logger->prefix);
        }

        if (logger->event_mask) {
            struct pop_prefix_event *event;
            event = arena_alloc(logger->arena, sizeof(struct pop_prefix_event));
            if (!event) {
                logmsg(LOGMSG_ERROR, "%s:arena_alloc failed\n", __func__);
                return -1;
            }
            reqlog_append_event(logger, EVENT_POP_PREFIX, event);
        }
    }
    return 0;
}

int reqlog_popallprefixes(struct reqlogger *logger)
{
    if (logger) {
        if (logger->dump_mask) {
            flushdump(logger, NULL);
            prefix_pop_all(&logger->prefix);
        }

        if (logger->event_mask) {
            struct pop_prefix_event *event;
            event = arena_alloc(logger->arena, sizeof(struct pop_prefix_event));
            if (!event) {
                logmsg(LOGMSG_ERROR, "%s:arena_alloc failed\n", __func__);
                return -1;
            }
            reqlog_append_event(logger, EVENT_POP_PREFIX_ALL, event);
        }
    }
    return 0;
}

static int reqlog_logv_int(struct reqlogger *logger, unsigned event_flag,
                           const char *fmt, va_list args)
{
    char buf[256];
    char *s = NULL;
    int len;
    va_list args_c;

    va_copy(args_c, args);
    len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len >= sizeof(buf) && reqltruncate == 0) {
        s = arena_alloc_align(logger->arena, len + 1, SYSARENA_1_BYTE_ALIGN);
        if (!s) {
            logmsg(LOGMSG_ERROR, "%s:arena_alloc(%d) failed\n", __func__, len + 1);
            va_end(args_c);
            return -1;
        }
        len = vsnprintf(s, len + 1, fmt, args_c);
    } else if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    va_end(args_c);

    if (logger->dump_mask & event_flag) {
        dump(logger, NULL, s ? s : buf, len);
    }

    if (logger->event_mask & event_flag) {
        struct print_event *event;

        if (!s) {
            s = arena_alloc_align(logger->arena, len + 1,
                                  SYSARENA_1_BYTE_ALIGN);
            if (!s) {
                logmsg(LOGMSG_ERROR, "%s:arena_alloc(%d) failed\n", __func__,
                        len + 1);
                return -1;
            }
            memcpy(s, buf, len + 1);
        }

        event = arena_alloc(logger->arena, sizeof(struct print_event));
        if (!event) {
            logmsg(LOGMSG_ERROR, "%s:arena_alloc failed\n", __func__);
            return -1;
        }
        event->event_flag = event_flag;
        event->length = len;
        event->text = s;
        reqlog_append_event(logger, EVENT_PRINT, event);
    }
    return 0;
}

int reqlog_logv(struct reqlogger *logger, unsigned event_flag, const char *fmt,
                va_list args)
{
    if (logger && (logger->mask & event_flag)) {
        return reqlog_logv_int(logger, event_flag, fmt, args);
    } else {
        return 0;
    }
}

/* Log a formatted string.  This incurs some copy overhead if logging is
 * turned on. */
int reqlog_logf(struct reqlogger *logger, unsigned event_flag, const char *fmt,
                ...)
{
    if (logger && (logger->mask & event_flag)) {
        va_list args;
        int rc;

        va_start(args, fmt);
        rc = reqlog_logv_int(logger, event_flag, fmt, args);
        va_end(args);
        return rc;
    }
    return 0;
}

/* Log a string literal.  This avoids copying the string. */
int reqlog_logl(struct reqlogger *logger, unsigned event_flag, const char *s)
{
    if (logger && (logger->mask & event_flag)) {
        if (logger->event_mask & event_flag) {
            struct print_event *event;
            event = arena_alloc(logger->arena, sizeof(struct print_event));
            if (!event) {
                logmsg(LOGMSG_ERROR, "%s:arena_alloc failed\n", __func__);
                return -1;
            }
            event->event_flag = event_flag;
            event->length = -1; /* to indicate length is unknown */
            event->text = s;
            reqlog_append_event(logger, EVENT_PRINT, event);
        }
        if (logger->dump_mask & event_flag) {
            dump(logger, NULL, s, strlen(s));
        }
    }
    return 0;
}

/* similar, but string length is supplied. */
int reqlog_logll(struct reqlogger *logger, unsigned event_flag, const char *s,
                 size_t len)
{
    if (logger && (logger->mask & event_flag)) {
        if (logger->event_mask & event_flag) {
            struct print_event *event;
            event = arena_alloc(logger->arena, sizeof(struct print_event));
            if (!event) {
                logmsg(LOGMSG_ERROR, "%s:arena_alloc failed\n", __func__);
                return -1;
            }
            event->event_flag = event_flag;
            event->length = len;
            event->text = s;
            reqlog_append_event(logger, EVENT_PRINT, event);
        }
        if (logger->dump_mask & event_flag) {
            dump(logger, NULL, s, len);
        }
    }
    return 0;
}

int reqlog_loghex(struct reqlogger *logger, unsigned event_flag, const void *d,
                  size_t len)
{
    if (logger && (logger->mask & event_flag)) {
        struct print_event *event;
        char *hexstr;
        char *hexpos;
        const unsigned char *dptr = d;
        size_t ii;
        static const char *hexchars = "0123456789abcdef";

        hexstr =
            arena_alloc_align(logger->arena, len * 2, SYSARENA_1_BYTE_ALIGN);
        if (!hexstr) {
            logmsg(LOGMSG_ERROR, "%s:arena_alloc failed\n", __func__);
            return -1;
        }

        if (logger->event_mask & event_flag) {
            event = arena_alloc(logger->arena, sizeof(struct print_event));
            if (!event) {
                logmsg(LOGMSG_ERROR, "%s:arena_alloc failed\n", __func__);
                return -1;
            }

            event->event_flag = event_flag;
            event->length = len * 2;
            event->text = hexstr;
            reqlog_append_event(logger, EVENT_PRINT, event);
        }

        hexpos = hexstr;
        for (ii = 0; ii < len; ii++) {
            *hexpos = hexchars[((*dptr) >> 4) & 0xf];
            hexpos++;
            *hexpos = hexchars[(*dptr) & 0xf];
            hexpos++;
            dptr++;
        }

        if (logger->dump_mask & event_flag) {
            dump(logger, NULL, hexstr, len * 2);
        }
    }
    return 0;
}

void reqlog_usetable(struct reqlogger *logger, const char *tablename)
{
    if (logger && logger->tracking_tables) {
        struct tablelist *table;
        int len;
        if (verbose) {
           logmsg(LOGMSG_USER, "%s: table %s\n", __func__, tablename);
        }
        for (table = logger->tables; table; table = table->next) {
            if (strcasecmp(table->name, tablename) == 0) {
                table->count++;
                return;
            }
        }
        len = strlen(tablename);
        table = arena_alloc(logger->arena,
                            offsetof(struct tablelist, name) + len + 1);
        if (!table) {
            logmsg(LOGMSG_ERROR, "%s: arena_alloc failed\n", __func__);
        } else {
            table->next = logger->tables;
            table->count = 1;
            memcpy(table->name, tablename, len + 1);
            logger->tables = table;
        }
    }
}

void reqlog_setflag(struct reqlogger *logger, unsigned flag)
{
    if (logger) {
        logger->reqflags |= flag;
    }
}

/* figure out what to log for this request */
static void reqlog_start_request(struct reqlogger *logger)
{
    struct logrule *rule;
    int gather;
    int ii;

    logger->tracking_tables = master_table_rules;

    if (logger->iq && logger->iq->debug) {
        logger->dump_mask = REQL_TRACE;
    }

    if (logger->opcode == OP_SQL && sqldbgflag) {
        logger->dump_mask = REQL_TRACE;
    }

    /* always gather info */
    logger->event_mask |= REQL_INFO;

    /* try to filter out this request based on opcode */
    gather = 0;
    if (master_all_requests) {
        gather = 1;
    } else {
        /* see if we match any of the criteria for a request that we have to
         * log. */
        if (master_opcode_list.num > 0 &&
            check_list(&master_opcode_list, logger->opcode)) {
            gather = 1;
        } else if (master_opcode_inv_list.num > 0 &&
                   check_list(&master_opcode_inv_list, logger->opcode)) {
            gather = 1;
        } else if (logger->stmt && master_num_stmts > 0) {
            for (ii = 0; ii < master_num_stmts && ii < NUMSTMTS; ii++) {
                if (strstr(logger->stmt, master_stmts[ii])) {
                    gather = 1;
                    break;
                }
            }
        }
    }
    if (gather) {
        logger->event_mask |= master_event_mask;
        if (logger->iq) {
            /* force legacy code to call reqprintf functions */
            logger->iq->debug = 1;
        }
    }

    logger->mask = logger->event_mask | logger->dump_mask;

    logger->in_request = 1;

    if (verbose) {
       logmsg(LOGMSG_USER, "gather=%d opcode=%d mask=0x%x\n", gather, logger->opcode,
               logger->mask);
    }
}

/* Set up the request logger for a new regular request with an ireq. */
void reqlog_new_request(struct ireq *iq)
{
    struct reqlogger *logger;

    logger = iq->reqlogger;
    if (!logger) {
        return;
    }

    reqlog_reset_logger(logger);
    logger->startms = iq->nowms;
    logger->iq = iq;
    logger->opcode = iq->opcode;
    if (iq->is_fromsocket) {
        logger->request_type = "socket/fstsnd request";
    } else {
        logger->request_type = "regular request";
    }
    reqlog_start_request(logger);
}

void reqlog_dump_tags(struct reqlogger *logger, char *tags, void *tagbuf,
                      int bufsz, void *nullbits, int numbits)
{

    /* don't do the work if we're not going to log this */
    if (!(logger && logger->mask & REQL_INFO))
        return;

    struct schema *s;
    s = new_dynamic_schema(NULL, tags, strlen(tags), 0);
    /* can't decode? */
    if (s == NULL)
        return;

    for (int fldnum = 0; fldnum < s->nmembers; fldnum++) {
        struct field *f;
        f = &s->member[fldnum];

        if (btst(nullbits, fldnum)) {
            reqlog_logf(logger, REQL_INFO, " %s null", f->name);
        } else {
            if (f->offset + f->len > bufsz)
                continue;

            switch (f->type) {
            case CLIENT_INT: {
                long long ival;
                if (f->len != 8)
                    continue;
                buf_get(&ival, 8, (uint8_t *)tagbuf + f->offset,
                        (uint8_t *)tagbuf + f->offset + f->len);
                reqlog_logf(logger, REQL_INFO, " %s int %lld", f->name, ival);
                break;
            }
            case CLIENT_REAL: {
                double dval;
                if (f->len != 8)
                    continue;
                buf_get(&dval, 8, (uint8_t *)tagbuf + f->offset,
                        (uint8_t *)tagbuf + f->offset + f->len);
                reqlog_logf(logger, REQL_INFO, " %s real %f", f->name, dval);
                break;
            }
            case CLIENT_DATETIME: {
                /* TODO */
                struct cdb2_client_datetime dt;
                if (f->len != sizeof(struct cdb2_client_datetime))
                    continue;
                memcpy(&dt, (uint8_t *)tagbuf + f->offset, f->len);
                dt.msec = ntohl(dt.msec);
                dt.tm.tm_sec = ntohl(dt.tm.tm_sec);
                dt.tm.tm_min = ntohl(dt.tm.tm_min);
                dt.tm.tm_hour = ntohl(dt.tm.tm_hour);
                dt.tm.tm_mday = ntohl(dt.tm.tm_mday);
                dt.tm.tm_mon = ntohl(dt.tm.tm_mon);
                dt.tm.tm_year = ntohl(dt.tm.tm_year);
                dt.tm.tm_wday = ntohl(dt.tm.tm_wday);
                dt.tm.tm_yday = ntohl(dt.tm.tm_yday);
                dt.tm.tm_isdst = ntohl(dt.tm.tm_isdst);
                reqlog_logf(logger, REQL_INFO,
                            " datetime %02d/%02d/%d %02d:%02d:%02d.%03d %s",
                            dt.tm.tm_mon + 1, dt.tm.tm_mday,
                            1900 + dt.tm.tm_year, dt.tm.tm_hour, dt.tm.tm_min,
                            dt.tm.tm_sec, dt.msec, dt.tzname);
                break;
            }
            case CLIENT_DATETIMEUS: {
                /* TODO */
                struct cdb2_client_datetimeus dt;
                if (f->len != sizeof(struct cdb2_client_datetimeus))
                    continue;
                memcpy(&dt, (uint8_t *)tagbuf + f->offset, f->len);
                dt.usec = ntohl(dt.usec);
                dt.tm.tm_sec = ntohl(dt.tm.tm_sec);
                dt.tm.tm_min = ntohl(dt.tm.tm_min);
                dt.tm.tm_hour = ntohl(dt.tm.tm_hour);
                dt.tm.tm_mday = ntohl(dt.tm.tm_mday);
                dt.tm.tm_mon = ntohl(dt.tm.tm_mon);
                dt.tm.tm_year = ntohl(dt.tm.tm_year);
                dt.tm.tm_wday = ntohl(dt.tm.tm_wday);
                dt.tm.tm_yday = ntohl(dt.tm.tm_yday);
                dt.tm.tm_isdst = ntohl(dt.tm.tm_isdst);
                reqlog_logf(logger, REQL_INFO,
                            " datetimeus %02d/%02d/%d %02d:%02d:%02d.%06d %s",
                            dt.tm.tm_mon + 1, dt.tm.tm_mday,
                            1900 + dt.tm.tm_year, dt.tm.tm_hour, dt.tm.tm_min,
                            dt.tm.tm_sec, dt.usec, dt.tzname);
                break;
            }
            case CLIENT_CSTR: {
                reqlog_logf(logger, REQL_INFO, " %s text \"%.*s\"", f->name,
                            f->len, (uint8_t *)tagbuf + f->offset);
                break;
            }
            case CLIENT_BLOB:
            case CLIENT_BYTEARRAY: {
                uint8_t *b = (uint8_t *)tagbuf + f->offset;
                static const char *hexchars = "0123456789abcdef";
                reqlog_logf(logger, REQL_INFO, " %s blob ", f->name);
                for (int i = 0; i < f->len; i++) {
                    reqlog_logf(logger, REQL_INFO, "%c%c",
                                hexchars[(b[i] & 0xf0) >> 4],
                                hexchars[b[i] & 0x0f]);
                }
                break;
            }
            }
        }
        reqlog_logf(logger, REQL_INFO, "\n");
    }
    free_tag_schema(s);
}

void reqlog_new_sql_request(struct reqlogger *logger, const char *sqlstmt,
                            char *tags, void *tagbuf, int tagbufsz,
                            void *nullbits, int numbits)
{
    if (!logger) {
        return;
    }
    logger->stmt = NULL;

    reqlog_reset_logger(logger);
    logger->request_type = "sql request";
    logger->opcode = OP_SQL;
    if (sqlstmt) {
        logger->stmt = arena_strdup(logger->arena, sqlstmt);
    }
    logger->startms = time_epochms();
    reqlog_start_request(logger);
    if (logger->stmt) {
        reqlog_logl(logger, REQL_INFO, logger->stmt);
    }
}

void reqlog_set_actual_sql(struct reqlogger *logger, char *sqlstmt)
{
    if (sqlstmt && logger->stmt == NULL) {
        logger->stmt = arena_strdup(logger->arena, sqlstmt);
    }
    if (logger->stmt) {
        reqlog_logl(logger, REQL_INFO, logger->stmt);
    }
}

void reqlog_diffstat_init(struct reqlogger *logger)
{
    if (!logger) {
        return;
    }

    reqlog_reset_logger(logger);
    logger->request_type = "stat dump";
    logger->opcode = OP_DEBUG;
    logger->mask = REQL_INFO;
    logger->event_mask = REQL_INFO;
}

/* Get the origin string for the request */
static const char *reqorigin(struct reqlogger *logger)
{
    if (logger->iq) {
        return getorigin(logger->iq);
    } else if (logger->origin[0]) {
        return logger->origin;
    } else {
        return "<unknown origin>";
    }
}

struct reqlog_print_callback_args {
    struct reqlogger *logger;
    struct output *out;
};

static int reqlog_print_callback(const char *s, void *context)
{
    struct reqlog_print_callback_args *args = context;
    dumpf(args->logger, args->out, "%s", s);
    return 0;
}

/* same as dump_client_query_stats, except print in readable text */
static void print_client_query_stats(struct reqlogger *logger,
                                     struct client_query_stats *st,
                                     struct output *out)
{
    for (int ii = 0; ii < st->n_components; ii++) {
        dumpf(logger, out, "    ");
        if (st->path_stats[ii].ix >= 0)
            dumpf(logger, out, "index %d on ", st->path_stats[ii].ix);
        dumpf(logger, out, "table %s ", st->path_stats[ii].table);
        dumpf(logger, out, "finds %d ", st->path_stats[ii].nfind);
        dumpf(logger, out, "next/prev %d ", st->path_stats[ii].nnext);
        if (st->path_stats[ii].nwrite)
            dumpf(logger, out, "nwrite %d ", st->path_stats[ii].nwrite);
        dumpf(logger, out, "\n");
    }
}

/* print the request header for the request. */
static void log_header_ll(struct reqlogger *logger, struct output *out,
                          int is_long)
{
    const struct bdb_thread_stats *thread_stats = bdb_get_thread_stats();
    struct reqlog_print_callback_args args;

    if (is_long) {
        dumpf(logger, out, "LONG REQUEST %d msec ", logger->durationms);
    } else {
        dumpf(logger, out, "%s %d msec ", logger->request_type,
              logger->durationms);
    }
    dumpf(logger, out, "from %s rc %d\n", reqorigin(logger), logger->rc);

    if (logger->iq) {
        struct ireq *iq = logger->iq;
        if (iq->reptimems > 0) {
            uint64_t rate = iq->txnsize / iq->reptimems;
            dumpf(logger, out,
                  "  Committed %llu log bytes in %d ms rep time (%llu "
                  "bytes/ms)\n",
                  iq->txnsize, iq->reptimems, rate);
        }

        dumpf(logger, out, "  nretries %d reply len %d\n", iq->retries,
              iq->p_buf_out - iq->p_buf_out_start);
    }

    args.logger = logger;
    args.out = out;
    bdb_print_stats(thread_stats, "  ", reqlog_print_callback, &args);

    if (is_long &&
        bdb_attr_get(thedb->bdb_attr, BDB_ATTR_SHOW_COST_IN_LONGREQ)) {
        struct client_query_stats *qstats = get_query_stats_from_thd();
        if (qstats)
            print_client_query_stats(logger, qstats, out);
    }
    log_all_events(logger, out);
}

static void log_header(struct reqlogger *logger, struct output *out,
                       int is_long)
{
    pthread_mutex_lock(&rules_mutex);
    pthread_mutex_lock(&long_request_out->mutex);
    log_header_ll(logger, out, is_long);
    pthread_mutex_unlock(&long_request_out->mutex);
    pthread_mutex_unlock(&rules_mutex);
}

static void log_all_events(struct reqlogger *logger, struct output *out)
{
    struct logevent *event;

    /* now scan for all tidbits of information about the request to publish */
    for (event = logger->events; event; event = event->next) {
        if (event->type == EVENT_PRINT) {
            struct print_event *pevent = (struct print_event *)event;
            if (pevent->event_flag & REQL_INFO) {
                if (pevent->length < 0) {
                    pevent->length = strlen(pevent->text);
                }
                if (logger->dumplinepos != 0 &&
                    pevent->length + logger->dumplinepos > 70) {
                    flushdump(logger, out);
                }
                if (logger->dumplinepos == 0) {
                    dump(logger, out, "  ", 2);
                } else {
                    dump(logger, out, ", ", 2);
                }
                dump(logger, out, pevent->text, pevent->length);
            }
        }
    }
    flushdump(logger, out);
}

static void log(struct reqlogger *logger, struct output *out,
                unsigned event_mask)
{
    struct logevent *event;

    pthread_mutex_lock(&out->mutex);
    prefix_init(&logger->prefix);
    log_header_ll(logger, out, 0);
    if (event_mask == 0) {
        pthread_mutex_unlock(&out->mutex);
        return;
    }
    /* print all events that this rule wanted to log */
    for (event = logger->events; event; event = event->next) {
        struct push_prefix_event *pushevent;
        struct print_event *pevent;
        switch (event->type) {
        case EVENT_PUSH_PREFIX:
            pushevent = (struct push_prefix_event *)event;
            if (pushevent->length < 0) {
                pushevent->length = strlen(pushevent->text);
            }
            prefix_push(&logger->prefix, pushevent->text, pushevent->length);
            break;

        case EVENT_POP_PREFIX:
            prefix_pop(&logger->prefix);
            break;

        case EVENT_POP_PREFIX_ALL:
            prefix_pop_all(&logger->prefix);
            break;

        case EVENT_PRINT:
            pevent = (struct print_event *)event;
            if (pevent->event_flag & event_mask) {
                if (pevent->length < 0) {
                    pevent->length = strlen(pevent->text);
                }
                dump(logger, out, pevent->text, pevent->length);
            }
            break;

        default:
            pthread_mutex_unlock(&out->mutex);
            logmsg(LOGMSG_ERROR, "%s: bad event type %d?!\n", __func__, event->type);
            return;
        }
    }
    flushdump(logger, out);
    logger->prefix.pos = 0;
    dump(logger, out, "----------", 10);
    flushdump(logger, out);
    pthread_mutex_unlock(&out->mutex);
}

static int inrange(const struct range *range, int value)
{
    if (range->from >= 0 && value < range->from) {
        return 0;
    } else if (range->to >= 0 && value > range->to) {
        return 0;
    } else {
        return 1;
    }
}

static int indblrange(const struct dblrange *range, double value)
{
    if (range->from >= 0 && value < range->from) {
        return 0;
    } else if (range->to >= 0 && value > range->to) {
        return 0;
    } else {
        return 1;
    }
}

void reqlog_set_cost(struct reqlogger *logger, double cost)
{
    if (logger)
        logger->sqlcost = cost;
}

void reqlog_set_rows(struct reqlogger *logger, int rows)
{
    if (logger)
        logger->sqlrows = rows;
}

int reqlog_current_ms(struct reqlogger *logger)
{
    return (time_epochms() - logger->startms);
}

/* End of a request. */
void reqlog_end_request(struct reqlogger *logger, int rc, const char *callfunc, int line)
{
    struct logruleuse {
        struct logruleuse *next;
        struct output *out;
        unsigned event_mask;
    };

    struct logrule *rule;
    struct logrule *tmprule;

    struct logruleuse *use_rules = NULL;
    struct logruleuse *use_rule;

    int long_request_thresh;

    if (!logger || !logger->in_request) {
        return;
    }

    if (logger->sqlrows > 0) {
        reqlog_logf(logger, REQL_INFO, "rowcount=%d", logger->sqlrows);
    }
    if (logger->sqlcost > 0) {
        reqlog_logf(logger, REQL_INFO, "cost=%f", logger->sqlcost);
    }
    if (logger->vreplays) {
        reqlog_logf(logger, REQL_INFO, "verify replays=%d", logger->vreplays);
    }

    if (gbl_fingerprint_queries) {
        static const char hex[] = "0123456789abcdef";
        unsigned char fingerprint_str[33];
        fingerprint_str[32] = 0;
        for (int i = 0; i < 16; i++) {
            fingerprint_str[i*2] = hex[(logger->fingerprint[i] & 0xf0) >> 4];
            fingerprint_str[i*2+1] = hex[logger->fingerprint[i] & 0x0f];
        }
        reqlog_logf(logger, REQL_INFO, "fingerprint %s", fingerprint_str);
    }

    logger->in_request = 0;

    flushdump(logger, NULL);

    logger->rc = rc;

    logger->durationms =
        (time_epochms() - logger->startms) + logger->queuetimems;

    /* now see if this matches any of our rules */
    if (rules.count != 0) {
        pthread_mutex_lock(&rules_mutex);
        LISTC_FOR_EACH_SAFE(&rules, rule, tmprule, linkv)
        {
            if (!rule->active) {
                continue;
            }

            if (logger->iq) {
                /* rules that apply only to regular style requests */
                if (!inrange(&rule->retries, logger->iq->retries)) {
                    continue;
                }
            }

            if (!inrange(&rule->duration, logger->durationms)) {
                continue;
            }

            if (!inrange(&rule->vreplays, logger->vreplays)) {
                continue;
            }

            if (!indblrange(&rule->sql_cost, logger->sqlcost)) {
                continue;
            }

            if (!inrange(&rule->sql_rows, logger->sqlrows)) {
                continue;
            }

            if (!check_list(&rule->opcode_list, logger->opcode)) {
                continue;
            }

            if (!check_list(&rule->rc_list, logger->rc)) {
                continue;
            }

            if (rule->stmt[0] &&
                (!logger->stmt || !strstr(logger->stmt, rule->stmt))) {
                continue;
            }

            if (rule->tablename[0]) {
                struct tablelist *table;
                for (table = logger->tables; table; table = table->next) {
                    if (strcasecmp(table->name, rule->tablename) == 0) {
                        break;
                    }
                }
                if (!table) {
                    continue;
                }
            }

            if (verbose) {
               logmsg(LOGMSG_USER, "matched rule %s event_mask 0x%x\n", rule->name,
                       rule->event_mask);
            }

            /* all conditions met (or no conditions); log this request
             * using this rule. */
            for (use_rule = use_rules; use_rule; use_rule = use_rule->next) {
                if (use_rule->out == rule->out) {
                    use_rule->event_mask |= rule->event_mask;
                    break;
                }
            }
            if (!use_rule) {
                use_rule =
                    arena_alloc(logger->arena, sizeof(struct logruleuse));
                if (!use_rule) {
                    logmsg(LOGMSG_ERROR, "%s:arena_alloc failed\n", __func__);
                } else {
                    use_rule->next = use_rules;
                    use_rule->event_mask = rule->event_mask;
                    use_rule->out = rule->out;
                    use_rules = use_rule;
                    use_rule->out->refcount++;
                }
            }

            if (rule->count > 0) {
                rule->count--;
                if (rule->count == 0) {
                    /* discard this rule */
                    logmsg(LOGMSG_USER, "Discarding logging rule '%s'\n", rule->name);
                    del_rule_ll(rule);
                }
            }
        }
        for (use_rule = use_rules; use_rule; use_rule = use_rule->next) {
            if (verbose) {
                logmsg(LOGMSG_USER, "print to %s with event_mask 0x%x\n",
                       use_rule->out->filename, use_rule->event_mask);
            }
            log(logger, use_rule->out, use_rule->event_mask);
            deref_output_ll(use_rule->out);
        }
        pthread_mutex_unlock(&rules_mutex);
    }

    /* check for bad cstrings */
    if (logger->reqflags & REQL_BAD_CSTR_FLAG) {
        logmsg(LOGMSG_WARN, "WARNING: THIS DATABASE IS RECEIVING NON NUL "
                        "TERMINATED CSTRINGS\n");
        log_header(logger, default_out, 0);
    }

    /* check for long requests */
    if (logger->opcode == OP_SQL && !logger->iq) {
        long_request_thresh = gbl_sql_time_threshold;
    } else {
        long_request_thresh = long_request_ms;
    }

    if (logger->durationms >= long_request_thresh) {

        log_header(logger, long_request_out, 1);
        long_reqs++;

        if (logger->durationms > longest_long_request_ms) {
            longest_long_request_ms = logger->durationms;
        }
        if (shortest_long_request_ms == -1 ||
            logger->durationms < shortest_long_request_ms) {
            shortest_long_request_ms = logger->durationms;
        }
        long_request_count++;
        if (last_long_request_epoch != time_epoch()) {
            last_long_request_epoch = time_epoch();

            if (long_request_out != default_out) {
                char *sqlinfo;

                if (logger->iq) {
                    sqlinfo = osql_get_tran_summary(logger->iq);
                } else {
                    sqlinfo = NULL;
                }
                if (sqlinfo) {
                    if (long_request_count == 1) {
                        logmsg(LOGMSG_USER, 
                                "LONG REQUEST %d MS logged in %s [%s]\n",
                                logger->durationms, long_request_out->filename,
                                sqlinfo);
                    } else {
                        logmsg(LOGMSG_USER, 
                                        "%d LONG REQUESTS %d MS - %d MS logged "
                                        "in %s [last %s]\n",
                                long_request_count, shortest_long_request_ms,
                                longest_long_request_ms,
                                long_request_out->filename, sqlinfo);
                    }
                    free(sqlinfo);
                } else {
                    if (long_request_count == 1) {
                        logmsg(LOGMSG_USER, "LONG REQUEST %d MS logged in %s\n",
                                logger->durationms, long_request_out->filename);
                    } else {
                        logmsg(LOGMSG_USER, 
                                "%d LONG REQUESTS %d MS - %d MS logged in %s\n",
                                long_request_count, shortest_long_request_ms,
                                longest_long_request_ms,
                                long_request_out->filename);
                    }
                }
            }
            long_request_count = 0;
            longest_long_request_ms = 0;
            shortest_long_request_ms = -1;
        }
    } else {
        norm_reqs++;
    }
#if 0
    if (logger->sqlcost > gbl_sql_cost_warn_threshold)
        fprintf(stderr, "WARNING: expensive SQL query\n");
    if (logger->sqlcost > gbl_sql_cost_trace_threshold || logger->sqlcost > gbl_sql_cost_warn_threshold) {
        log_header(logger, default_out, 0);
        dumpf(logger, default_out, "rows=%d cost=%f query: %s\n", logger->sqlrows, logger->sqlcost, logger->stmt);
    }
#endif

    if (logger->iq && logger->iq->blocksql_tran) {
        if (gbl_time_osql)
            osql_bplog_time_done(logger->iq);

        osql_bplog_free(logger->iq, 1, __func__, callfunc, line);
    }
    logger->tags = NULL;
    logger->tagbuf = NULL;
    logger->nullbits = NULL;
}

/* this is meant to be called by only 1 thread, will need locking if
 * more than one threads were to be involved */
void reqlog_diffstat_dump(struct reqlogger *logger)
{
    if (!logger) {
        return;
    }
    log_all_events(logger, stat_request_out);
    reqlog_diffstat_init(logger);
}

int reqlog_diffstat_thresh() { return diffstat_thresh; }

void reqlog_set_diffstat_thresh(int val)
{
    diffstat_thresh = val;
    logmsg(LOGMSG_USER, "diffstat threshold now %d s\n", diffstat_thresh);
    if (diffstat_thresh == 0) {
        logmsg(LOGMSG_USER, "diffstat thresh feature is disabled\n");
    }
}

int reqlog_truncate() { return reqltruncate; }

void reqlog_set_truncate(int val)
{
    reqltruncate = val;
    logmsg(LOGMSG_USER, "truncate %s\n", reqltruncate ? "enabled" : "disabled");
}

struct rawnodestats *get_raw_node_stats(char *host)
{
    struct nodestats *nodestats;
    struct rawnodestats *rawnodestats;

    host = intern(host);

    if (!nodestats_array[nodeix(host)]) {
        LOCK(&nodestats_lk)
        {
            if (!nodestats_array[nodeix(host)]) {
                nodestats = calloc(1, sizeof(struct nodestats));
                nodestats->next = nodestats_first;
                nodestats->host = host;
                MEMORY_SYNC;
                nodestats_first = nodestats;
                nodestats_array[nodeix(host)] = nodestats;
                num_nodes++;
            }
        }
        UNLOCK(&nodestats_lk);
    }

    return &nodestats_array[nodeix(host)]->rawtotals;
}

/* called (roughly) once a second to update our per node stats */
void process_nodestats(void)
{
    static int last_time_ms = 0;
    int span_ms;
    struct nodestats *nodestats;

    if (last_time_ms == 0)
        last_time_ms = time_epochms();
    span_ms = time_epochms() - last_time_ms;
    last_time_ms = time_epochms();

    LOCK(&nodestats_calc_lk)
    {
        for (nodestats = nodestats_first; nodestats;
             nodestats = nodestats->next) {
            unsigned next_bucket;
            unsigned ii;
            unsigned *nowptr;
            unsigned *prevptr;
            unsigned *bucketptr;
            struct rawnodestats *rawnodestats;

            if (!nodestats)
                continue;

            nowptr = (unsigned *)&nodestats->rawtotals;
            prevptr = (unsigned *)&nodestats->prevtotals;

            nodestats->bucket_spanms[nodestats->cur_bucket] = span_ms;
            bucketptr =
                (unsigned *)&nodestats->raw_buckets[nodestats->cur_bucket];

            for (ii = 0; ii < NUM_RAW_NODESTATS; ii++) {
                unsigned prev_value, diff;
                prev_value = nowptr[ii];
                diff = prev_value - prevptr[ii];
                prevptr[ii] = prev_value;
                bucketptr[ii] = diff;
            }

            next_bucket = nodestats->cur_bucket + 1;
            if (next_bucket >= NUM_BUCKETS)
                next_bucket = 0;
            nodestats->cur_bucket = next_bucket;
        }
    }
    UNLOCK(&nodestats_calc_lk);
}

static void snap_nodestats_ll(char *host, struct rawnodestats *snap,
                              int disp_rates)
{
    struct nodestats *nodestats;

    bzero(snap, sizeof(*snap));

    nodestats = nodestats_array[nodeix(host)];

    if (!nodestats) {
        return;
    } else if (disp_rates) {
        /* calculate rates for raw figures in n per second */
        unsigned bucket, ii;
        int timespanms = 0;
        unsigned *snapptr = (unsigned *)snap;
        unsigned *bktptr;
        for (bucket = 0; bucket < NUM_BUCKETS; bucket++) {
            timespanms += nodestats->bucket_spanms[bucket];
            bktptr = (unsigned *)&nodestats->raw_buckets[bucket];
            for (ii = 0; ii < NUM_RAW_NODESTATS; ii++) {
                snapptr[ii] += bktptr[ii];
            }
        }

        if (timespanms <= 0)
            timespanms = 1;
        for (ii = 0; ii < NUM_RAW_NODESTATS; ii++) {
            snapptr[ii] = 0.5 + (NUM_BUCKETS * 1000.00 *
                                 ((double)snapptr[ii] / (double)timespanms));
        }
    } else {
        memcpy(snap, &nodestats->prevtotals, sizeof(*snap));
    }
}

void nodestats_node_report(FILE *fh, const char *prefix, int disp_rates,
                           char *host)
{
    struct rawnodestats snap;
    int opcode;

    if (!prefix)
        prefix = "";

    LOCK(&nodestats_calc_lk) { snap_nodestats_ll(host, &snap, disp_rates); }
    UNLOCK(&nodestats_calc_lk);

    logmsgf(LOGMSG_USER, fh, "%sRAW STATISTICS FOR NODE %d\n", prefix, host);
    logmsgf(LOGMSG_USER, fh, "%s--- opcode counts for regular fstsnd requests\n", prefix);
    for (opcode = 0; opcode < MAXTYPCNT; opcode++) {
        if (snap.opcode_counts[opcode]) {
            logmsgf(LOGMSG_USER, fh, "%s%-20s  %u\n", prefix, req2a(opcode),
                    snap.opcode_counts[opcode]);
        }
    }
    logmsgf(LOGMSG_USER, fh, "%s--- block operation opcode counts (for transactions)\n",
            prefix);
    for (opcode = 0; opcode < BLOCK_MAXOPCODE; opcode++) {
        if (snap.blockop_counts[gbl_blockop_count_xrefs[opcode]]) {
            logmsgf(LOGMSG_USER, fh, "%s%-20s  %u\n", prefix, breq2a(opcode),
                    snap.blockop_counts[gbl_blockop_count_xrefs[opcode]]);
        }
    }
    logmsgf(LOGMSG_USER, fh, "%s--- SQL statistics\n", prefix);
    if (snap.sql_queries)
        logmsgf(LOGMSG_USER, fh, "%s%-20s  %u\n", prefix, "queries", snap.sql_queries);
    if (snap.sql_steps)
        logmsgf(LOGMSG_USER, fh, "%s%-20s  %u\n", prefix, "steps", snap.sql_steps);
    if (snap.sql_rows)
        logmsgf(LOGMSG_USER, fh, "%s%-20s  %u\n", prefix, "rows", snap.sql_rows);
}

void nodestats_report(FILE *fh, const char *prefix, int disp_rates)
{
    unsigned max_nodes = num_nodes;
    unsigned ii;
    struct summary_nodestats *summaries;
    struct nodestats *nodestats;
    struct rawnodestats snap;

    if (!prefix)
        prefix = "";

    if (disp_rates) {
        logmsgf(LOGMSG_USER, fh, "%sCURRENT REQUEST RATE OVER LAST %d SECONDS\n", prefix,
                NUM_BUCKETS);
    } else {
        logmsgf(LOGMSG_USER, fh, "%sTOTAL REQUESTS SUMMARY\n", prefix);
    }
    logmsgf(LOGMSG_USER, fh, "%snode | regular fstsnds                 |  blockops          "
                "                                     | sql\n",
            prefix);
    logmsgf(LOGMSG_USER, fh, "%s     |   finds rngexts  writes   other |    adds    upds    "
                "dels blk/sql   recom snapisl  serial | queries   steps    "
                "rows\n",
            prefix);

    if (max_nodes == 0)
        return;

    summaries = calloc(max_nodes, sizeof(struct summary_nodestats));
    if (!summaries) {
        logmsgf(LOGMSG_USER, stderr, "%s: out of memory %u nodes\n", __func__, max_nodes);
        return;
    }

    LOCK(&nodestats_calc_lk)
    {
        for (ii = 0, nodestats = nodestats_first; nodestats && ii < max_nodes;
             nodestats = nodestats->next, ii++) {

            unsigned opcode;

            snap_nodestats_ll(nodestats->host, &snap, disp_rates);

            summaries[ii].host = nodestats->host;

            summaries[ii].sql_queries = snap.sql_queries;
            summaries[ii].sql_steps = snap.sql_steps;
            summaries[ii].sql_rows = snap.sql_rows;

            for (opcode = 0; opcode < MAXTYPCNT; opcode++) {
                unsigned n = snap.opcode_counts[opcode];
                if (!n)
                    continue;
                switch (opcode) {
                case OP_FIND:
                case OP_NEXT:
                case OP_JSTNX:
                case OP_JSTFND:
                case OP_FNDRRN:
                case OP_PREV:
                case OP_JSTPREV:
                case OP_FIND2:
                case OP_NEXT2:
                case OP_PREV2:
                case OP_JFND2:
                case OP_JNXT2:
                case OP_JPRV2:
                case OP_FNDKLESS:
                case OP_JFNDKLESS:
                case OP_FNDNXTKLESS:
                case OP_FNDPRVKLESS:
                case OP_JFNDNXTKLESS:
                case OP_JFNDPRVKLESS:
                    summaries[ii].finds += n;
                    break;

                case OP_STORED:
                case OP_RNGEXT2:
                case OP_RNGEXTP2:
                case OP_RNGEXTTAG:
                case OP_RNGEXTTAGP:
                case OP_RNGEXTTAGTZ:
                case OP_RNGEXTTAGPTZ:
                case OP_NEWRNGEX:
                    summaries[ii].rngexts += n;
                    break;

                case OP_BLOCK:
                case OP_FWD_BLOCK:
                case OP_LONGBLOCK:
                case OP_FWD_LBLOCK:
                case OP_CLEARTABLE:
                case OP_FASTINIT:
                    summaries[ii].writes += n;
                    break;

                default:
                    summaries[ii].other_fstsnds += n;
                    break;
                }
            }

            for (opcode = 0; opcode < BLOCK_MAXOPCODE; opcode++) {
                unsigned n =
                    snap.blockop_counts[gbl_blockop_count_xrefs[opcode]];
                if (!n)
                    continue;
                switch (opcode) {
                case BLOCK2_ADDDTA:
                case BLOCK2_ADDKL:
                case BLOCK2_ADDKL_POS:
                case BLOCK_ADDSL:
                    summaries[ii].adds += n;
                    break;

                case BLOCK_UPVRRN:
                case BLOCK2_UPDATE:
                case BLOCK2_UPDKL:
                case BLOCK2_UPDKL_POS:
                    summaries[ii].upds += n;
                    break;

                case BLOCK_DELSEC:
                case BLOCK_DELNOD:
                case BLOCK2_DELDTA:
                case BLOCK2_DELKL:
                    summaries[ii].dels += n;
                    break;

                case BLOCK2_SQL:
                    summaries[ii].bsql += n;
                    break;

                case BLOCK2_RECOM:
                    summaries[ii].recom += n;

                case BLOCK2_SNAPISOL:
                    summaries[ii].snapisol += n;

                case BLOCK2_SERIAL:
                    summaries[ii].serial += n;
                }
            }
        }
        max_nodes = ii;
    }
    UNLOCK(&nodestats_calc_lk);

    for (ii = 0; ii < max_nodes; ii++) {
        logmsgf(LOGMSG_USER, fh, "%s%16s | %7u %7u %7u %7u | %7u %7u %7u %7u %7u %7u %7u | "
                    "%7u %7u %7u\n",
                prefix, summaries[ii].host, summaries[ii].finds,
                summaries[ii].rngexts, summaries[ii].writes,
                summaries[ii].other_fstsnds, summaries[ii].adds,
                summaries[ii].upds, summaries[ii].dels, summaries[ii].bsql,
                summaries[ii].recom, summaries[ii].snapisol,
                summaries[ii].serial, summaries[ii].sql_queries,
                summaries[ii].sql_steps, summaries[ii].sql_rows);
    }

    free(summaries);
}

void reqlog_set_origin(struct reqlogger *logger, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(logger->origin, sizeof(logger->origin) - 1, fmt, args);
    va_end(args);
    logger->origin[sizeof(logger->origin) - 1] = 0;
}

const char *reqlog_get_origin(struct reqlogger *logger)
{
    return logger->origin;
}

void reqlog_set_vreplays(struct reqlogger *logger, int replays)
{
    if (logger)
        logger->vreplays = replays;
}

void reqlog_set_queue_time(struct reqlogger *logger, int timems)
{
    if (logger)
        logger->queuetimems = timems;
}

void reqlog_set_fingerprint(struct reqlogger *logger, char fingerprint[16]) {
    if (logger)
        memcpy(logger->fingerprint, fingerprint, sizeof(logger->fingerprint));
}