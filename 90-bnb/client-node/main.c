/*
 * Copyright (C) 2013 Freie Universität Berlin
 *
 * This file is subject to the terms and conditions of the GNU Lesser General
 * Public License. See the file LICENSE in the top level directory for more
 * details.
 */

/**
 * @ingroup examples
 * @{
 *
 * @file
 * @brief CCN Lite interactive shell example application
 *
 * @author Christian Mehlis <mehlis@inf.fu-berlin.de>
 *
 * @}
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "msg.h"
#include "thread.h"
#include "posix_io.h"
#include "shell.h"
#include "board_uart0.h"
#include "transceiver.h"
#include "vtimer.h"
#include "ps.h"
#include "ltc4150.h"

#define ENABLE_DEBUG (1)
#include "debug.h"

#include "ccn_lite/ccnl-riot.h"
#include "ccn_lite/util/ccnl-riot-client.h"

#include "../events.h"

#define RIOT_CCN_APPSERVER (1)
#define RIOT_CCN_TESTS (0)

long long relay_stack[KERNEL_CONF_STACKSIZE_MAIN];
long long blinker_stack[KERNEL_CONF_STACKSIZE_DEFAULT];

#if RIOT_CCN_APPSERVER
long long appserver_stack[KERNEL_CONF_STACKSIZE_MAIN];
#endif
int relay_pid, appserver_pid;

#define SHELL_MSG_BUFFER_SIZE (64)
msg_t msg_buffer_shell[SHELL_MSG_BUFFER_SIZE];

shell_t shell;

unsigned char big_buf[3 * 1024];
char small_buf[PAYLOAD_SIZE];

state_t state = IDLE;

#if RIOT_CCN_APPSERVER

static void riot_ccn_appserver(int argc, char **argv)
{
    (void) argc; /* the function takes no arguments */
    (void) argv;

    if (appserver_pid) {
        /* already running */
        return;
    }

    appserver_pid = thread_create(
            appserver_stack, sizeof(appserver_stack),
            PRIORITY_MAIN - 1, CREATE_STACKTEST,
            ccnl_riot_appserver_start, (void *) relay_pid, "appserver");
    DEBUG("ccn-lite appserver on thread_id %d...\n", appserver_pid);
}
#endif

void interest(const char *argv);

static void riot_ccn_express_interest(int argc, char **argv)
{
    static const char *default_interest = "/ccnx/0.7.1/doc/technical/CanonicalOrder.txt";

    if (argc < 2) {
        interest(default_interest);
    }
    else {
        interest(argv[1]);
    }
}

void interest(const char *argv)
{
    strncpy(small_buf, argv, strlen(argv)); // null terminated
    DEBUG("in='%s'\n", small_buf);

    /* for demo cases */
    vtimer_usleep(300 * 1000);

    int content_len = ccnl_riot_client_get(relay_pid, small_buf, (char *) big_buf); // small_buf=name to request

    if (content_len == 0) {
        puts("riot_get returned 0 bytes...aborting!");
        return;
    }

    puts("####################################################");
    big_buf[content_len] = '\0';
    printf("data='%s'\n", big_buf);
    puts("####################################################");
    puts("done");
    state = READY;
}

static void riot_ccn_register_prefix(int argc, char **argv)
{
    if (argc < 4) {
        puts("enter: prefix </path/to/abc> <type> <faceid>");
        return;
    }

    strncpy(small_buf, argv[1], 100);
    DEBUG("prefix='%s'\n", small_buf);

    char *type = argv[2];
    char *faceid = argv[3]; // 0=trans;1=msg

    int content_len = ccnl_riot_client_publish(relay_pid, small_buf, faceid, type, big_buf);

    DEBUG("shell received: '%s'\n", big_buf);
    DEBUG("received %d bytes.\n", content_len);
    puts("done");
}

static void riot_ccn_relay_config(int argc, char **argv)
{
    if (!relay_pid) {
        puts("ccnl stack not running");
        return;
    }

    if (argc < 2) {
        printf("%s: <max_cache_entries>\n", argv[0]);
        return;
    }

    msg_t m;
    m.content.value = atoi(argv[1]);
    m.type = CCNL_RIOT_CONFIG_CACHE;
    msg_send(&m, relay_pid);
}

static void riot_ccn_transceiver_start(int relay_pid)
{
    transceiver_init(TRANSCEIVER);
    int transceiver_pid = transceiver_start();
    DEBUG("transceiver on thread_id %d...\n", transceiver_pid);

    /* register for transceiver events */
    uint8_t reg = transceiver_register(TRANSCEIVER, relay_pid);
    if (reg != 1) {
        DEBUG("transceiver register failed\n");
    }

    /* set channel to CCNL_CHAN */
    msg_t mesg;
    transceiver_command_t tcmd;
    int32_t c = 10;
    tcmd.transceivers = TRANSCEIVER;
    tcmd.data = &c;
    mesg.content.ptr = (char *) &tcmd;
    mesg.type = SET_CHANNEL;
    msg_send_receive(&mesg, &mesg, transceiver_pid);
    if (c == -1) {
        puts("[transceiver] Error setting/getting channel");
    }
    else {
        printf("[transceiver] Got channel: %" PRIi32 "\n", c);
    }
}

static void riot_ccn_relay_start(void)
{
    if (relay_pid) {
        DEBUG("ccn-lite relay on thread_id %d...please stop it first!\n", relay_pid);
        /* already running */
        return;
    }

    relay_pid = thread_create(
            relay_stack, sizeof(relay_stack),
            PRIORITY_MAIN - 2, CREATE_STACKTEST,
            ccnl_riot_relay_start, NULL, "relay");
    DEBUG("ccn-lite relay on thread_id %d...\n", relay_pid);

    riot_ccn_transceiver_start(relay_pid);
}

static void *blinker_thread(void *u)
{
    (void) u;
    unsigned i = 0;
    while (1) {
        if (state == IDLE) {
            LED_RED_OFF;
            LED_GREEN_ON;
            vtimer_usleep(100 * 1000);
            LED_GREEN_OFF;
            vtimer_usleep(100 * 1000);
        }
        else if (state == WAITING) {
            LED_RED_ON;
            LED_GREEN_OFF;
            vtimer_usleep(100 * 1000);
            LED_RED_OFF;
            vtimer_usleep(100 * 1000);
        }
        /* got response */
        else {
            LED_RED_OFF;
            LED_GREEN_ON;
            vtimer_usleep(600 * 1000);
            LED_GREEN_OFF;
            vtimer_usleep(600 * 1000);
            if (i++ > 10) {
                i = 0;
                state = IDLE;
            }
        }
    }

    return NULL;
}

static void riot_ccn_relay_stop(int argc, char **argv)
{
    (void) argc; /* the function takes no arguments */
    (void) argv;

    msg_t m;
    m.content.value = 0;
    m.type = CCNL_RIOT_HALT;
    msg_send(&m, relay_pid);

    /* mark relay as not running */
    relay_pid = 0;
}

#if RIOT_CCN_TESTS
static void riot_ccn_pit_test(int argc, char **argv)
{
    (void) argc; /* the function takes no arguments */
    (void) argv;

    char name[] = "/riot/test";

    char *prefix[CCNL_MAX_NAME_COMP];
    char *cp = strtok(name, "/");
    int i = 0;

    while (i < (CCNL_MAX_NAME_COMP - 1) && cp) {
        prefix[i++] = cp;
        cp = strtok(NULL, "/");
    }

    //prefix[i] = 0; //segment to request
    prefix[i + 1] = 0;

    msg_t m;
    riot_ccnl_msg_t rmsg;
    char segment_string[16]; //max=999\0
    timex_t now;

    int segment;

    for (segment = 0; segment < 200; segment++) {
        memset(segment_string, 0, 16);
        snprintf(segment_string, 16, "%d", segment);
        prefix[i] = segment_string;
        unsigned int interest_nonce = genrand_uint32();
        int interest_len = mkInterest(prefix, &interest_nonce, (unsigned char *) small_buf);

        rmsg.payload = &small_buf;
        rmsg.size = interest_len;
        m.content.ptr = (char *) &rmsg;
        m.type = CCNL_RIOT_MSG;

        msg_send(&m, relay_pid);

        if ((segment % 50) == 0) {
            vtimer_now(&now);
            printf("done: %d - %ld.%ld\n", segment, now.tv_sec, now.tv_usec);
        }
    }

    printf("done: tried to send %d interests\n", segment);
}

static void riot_ccn_fib_test(int argc, char **argv)
{
    (void) argc; /* the function takes no arguments */
    (void) argv;

    char type[] = "newTRANSface";
    char faceid[] = "42";

    riot_new_face(relay_pid, type, faceid, big_buf);

    timex_t now;
    int i = -1;

    do {
        i++;
        snprintf(small_buf, sizeof(small_buf), "/riot/test/fib/%d/", i);
        riot_register_prefix(relay_pid, small_buf, faceid, big_buf);

        if (i % 50 == 0) {
            vtimer_now(&now);
            printf("done: %d - %ld.%ld\n", i, now.tv_sec, now.tv_usec);
        }
    }
    while (0 == strcmp((const char *) big_buf, "prefixreg cmd worked"));

    DEBUG("%d: '%s'\n", i, big_buf);
    printf("done: %d\n", i - 1);
}
#endif

static void riot_ccn_populate(int argc, char **argv)
{
    (void) argc; /* the function takes no arguments */
    (void) argv;

    msg_t m;
    m.content.value = 0;
    m.type = CCNL_RIOT_POPULATE;
    msg_send(&m, relay_pid);
}

static void riot_ccn_stat(int argc, char **argv)
{
    (void) argc; /* the function takes no arguments */
    (void) argv;

    msg_t m;
    m.content.value = 0;
    m.type = CCNL_RIOT_PRINT_STAT;
    msg_send(&m, relay_pid);
}

static void _ignore(radio_address_t a);
transceiver_command_t tcmd;

void ignore(int argc, char **argv)
{
    if (transceiver_pid < 0) {
        puts("Transceiver not runnning.");
        return;
    }
    if (argc == 2) {
        _ignore(atoi(argv[1]));
    }
    else {
        printf("Usage: %s <addr>\n", argv[0]);
    }
}

static void _ignore(radio_address_t a)
{
    msg_t mesg;
    mesg.type = DBG_IGN;
    mesg.content.ptr = (char *) &tcmd;

    tcmd.transceivers = TRANSCEIVER_CC1100;
    tcmd.data = &a;

    printf("sending to transceiver (%u): %u\n", transceiver_pid, (*(uint8_t *)tcmd.data));
    msg_send(&mesg, transceiver_pid);
}

static const shell_command_t sc[] = {
    { "haltccn", "stops ccn relay", riot_ccn_relay_stop },
    { "interest", "express an interest", riot_ccn_express_interest },
    { "populate", "populate the cache of the relay with data", riot_ccn_populate },
    { "prefix", "registers a prefix to a face", riot_ccn_register_prefix },
    { "stat", "prints out forwarding statistics", riot_ccn_stat },
    { "config", "changes the runtime config of the ccn lite relay", riot_ccn_relay_config },
#if RIOT_CCN_APPSERVER
    { "appserver", "starts an application server to reply to interests", riot_ccn_appserver },
#endif
#if RIOT_CCN_TESTS
    { "pittest", "starts a test for the size and speed of pit operations", riot_ccn_pit_test },
    { "fibtest", "starts a test for the size and speed of fib operations", riot_ccn_fib_test },
#endif
    { "ign", "ignore node", ignore},
    { NULL, NULL, NULL }
};

/* checked for type safety */
static void set_address(radio_address_t a)
{
    msg_t mesg;
    transceiver_command_t tcmd;

    tcmd.transceivers = TRANSCEIVER_DEFAULT;
    tcmd.data = &a;
    mesg.content.ptr = (char *) &tcmd;

    printf("[transceiver] trying to set address %" PRIu16 "\n", a);
    mesg.type = SET_ADDRESS;

    msg_send_receive(&mesg, &mesg, transceiver_pid);
    printf("[transceiver] got address: %" PRIu16 "\n", a);
}

int main(void)
{
    puts("CCN!");

    if (msg_init_queue(msg_buffer_shell, SHELL_MSG_BUFFER_SIZE) != 0) {
        DEBUG("msg init queue failed...abording\n");
        return -1;
    }

    sense_init();
    riot_ccn_relay_start();
    set_address(3);
    _ignore(1);
    
    thread_create(blinker_stack, sizeof(blinker_stack),
            PRIORITY_MAIN - 1, CREATE_STACKTEST,
            blinker_thread, NULL, "blinker");

    puts("starting shell...");
    puts("  posix open");
    posix_open(uart0_handler_pid, 0);
    puts("  shell init");
    shell_init(&shell, sc, UART0_BUFSIZE, uart0_readc, uart0_putc);
    puts("  shell run");
    shell_run(&shell);

    return 0;
}
