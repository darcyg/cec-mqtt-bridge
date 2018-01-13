/*
  Copyright (c) 2018, Garrett L. Ward
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

  * Neither the name of picture_sort nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <ctype.h>

#include <libcec/cecc.h>
#include <libcec/cectypes.h>
#include "ceccloader.h"

#include <jansson.h>
#include <mosquitto.h>

static int use_tls = 0;
static char *mqtt_broker = NULL;
static char *mqtt_topic = NULL;
static uint16_t mqtt_port = 1883;

// Option Parsing
typedef struct _option
{
    struct option opt;
    const char *description;
    const char *help;
} Option;

static Option options[] =
{
    {{"mqtt-broker", required_argument, 0, 'b'}, "MQTT Broker IP", "The IP address of the MQTT broker to publish to"},
    {{"mqtt-port", required_argument, 0, 'p'}, "MQTT Broker Port", "The port of the MQTT broker to publish to. Defaults to 1883"},
    {{"mqtt-use-tls", no_argument, &use_tls, 1}, "Use TLS for MQTT connection", "CURRENTLY UNSUPPORTED. If specified, TLS will be used to connect to the MQTT broker"},
    {{"mqtt-topic", required_argument, 0, 't'}, "MQTT Topic", "The MQTT topic to publish TV state information to"},
    {{"debug", no_argument, 0, 'd'}, "Enable additional debug messages"},
};
#define OPTION_COUNT (sizeof(options)/sizeof(options[0]))

#define MAX_HELP_LINE_LENGTH (40)
static char help_buf[MAX_HELP_LINE_LENGTH + 1];

static void usage(void);

typedef struct tv_state {
    cec_power_status power_status;
    uint8_t hdmi_input;
} TvState;


static bool terminate  = false;
static bool debug = false;
static struct mosquitto* mosq = NULL;

static void cb_cec_command_received(void *lib, const cec_command* command);
static bool tv_state_equal(TvState *a, TvState *b);

static char devices[16][16] = {
    "TV",
    "PI",
    "NONE",
    "NONE",
    "Apple TV",
    "NONE",
    "NONE",
    "NONE",
    "Chromecast",
    "NONE",
    "NONE",
    "NONE",
    "NONE",
    "NONE",
    "NONE",
    "BROADCAST"
};

static void sighandler(int signal);


// Set up callbacks
static ICECCallbacks        callbacks = {
    .logMessage           = NULL,
    .keyPress             = NULL,
    .commandReceived      = cb_cec_command_received,
    .configurationChanged = NULL,
    .alert                = NULL,
    .menuStateChanged     = NULL,
    .sourceActivated      = NULL
};

static TvState tv_state = {
    .power_status = CEC_POWER_STATUS_UNKNOWN,
    .hdmi_input = 0
};

static void cb_cec_command_received(void *p, const cec_command* command)
{
    (void)p;
    char buffer[24];

    TvState old = tv_state;

    if (debug)
    {
        libcec_opcode_to_string(command->opcode, buffer, sizeof(buffer));
        printf("%s -> %s: %s ( ", devices[command->initiator], devices[command->destination], buffer);
        if (command->parameters.size > 0)
        {
            for (int i = 0; i < command->parameters.size; i++)
            {
                printf("%02x ", command->parameters.data[i]);
            }
        }
        printf(")\n");
    }
    

    if (command->initiator == CECDEVICE_TV)
    {
        // Check for the following TV message:
        // 1. Report power status.
        // 2. Standby
        // 3. Vendor code with ID? (TBD)
        switch (command->opcode)
        {
        case CEC_OPCODE_REPORT_POWER_STATUS:
            tv_state.power_status = command->parameters.data[0];
            break;
        case CEC_OPCODE_VENDOR_COMMAND_WITH_ID:
        case CEC_OPCODE_STANDBY:
            // Bravia TV broadcasts a vendor command on any
            // on->standby transition
            tv_state.power_status = CEC_POWER_STATUS_STANDBY;
            break;
        case CEC_OPCODE_REPORT_PHYSICAL_ADDRESS:
            // Bravia TV broadcasts its physical address on any
            // standby->on transition
            tv_state.power_status = CEC_POWER_STATUS_ON;
            break;
        case CEC_OPCODE_SET_STREAM_PATH: // from tv to bcast
            tv_state.hdmi_input = command->parameters.data[0] >> 4;
            break;
        default:
            break;
        }
    }
    else if (command->destination == CECDEVICE_BROADCAST)
    {
        switch (command->opcode)
        {
        case CEC_OPCODE_ACTIVE_SOURCE: // from src to bcast
            tv_state.hdmi_input = command->parameters.data[0] >> 4;
            break;
        default:
            break;
        }
    }

    if (!tv_state_equal(&old, &tv_state))
    {
        char oldpower[24];
        char newpower[24];
        libcec_power_status_to_string(old.power_status, oldpower, sizeof(oldpower));
        libcec_power_status_to_string(tv_state.power_status, newpower, sizeof(newpower));
        if (debug)
        {
            printf("TV State: (%s [HDMI %d]) -> (%s [HDMI %d])\n",
                   oldpower, old.hdmi_input,
                   newpower, tv_state.hdmi_input
            );
        }

        json_t *json = json_object();
        json_object_set_new(json, "power_state", json_string(newpower));
        json_object_set_new(json, "hdmi_input", json_integer(tv_state.hdmi_input));
        char *json_string = json_dumps(json, JSON_INDENT(2));

        if (mosq)
        {
            int rc = mosquitto_publish(mosq, NULL, mqtt_topic, strlen(json_string), json_string, 0, true);
            // TODO reconnect on failure?
            if (rc != MOSQ_ERR_SUCCESS)
            {
                // Set terminate and let the process die and maybe be
                // restarted
                terminate = true;
            }
        }

        if (debug)
        {
            printf("%s\n", json_string);
        }
        free (json_string);
        json_object_clear(json);
        json_decref(json);
    }
}

static bool tv_state_equal(TvState *a, TvState *b)
{
    // if non-NULL and reference equal, true
    if (a != NULL && a == b)
    {
        return true;
    }
    // If both NULL or a OR b is NULL, false
    if (!a || !b)
    {
        return false;
    }
    
    return a->power_status == b->power_status
        && a->hdmi_input == b->hdmi_input;
}

static void sighandler(int signal)
{
    (void)signal;
    // TODO
    terminate = true;
}

static void usage(void)
{
    printf("Usage: cec-mqtt-bridge <arguments>\n");
    printf("Sniff a CEC bus and send power state events to an MQTT broker\n");
    printf("\n");

    for (size_t i = 0; i < OPTION_COUNT; i++)
    {
        Option *option = &options[i];

        if (isalnum((int)option->opt.val)) {
            printf("  -%c", option->opt.val);
            if (option->opt.name) {
                printf(", ");
            } else {
                printf("  ");
            }
        } else {
            printf("      ");
        }
        if (option->opt.name) {
            size_t opt_len = strlen(option->opt.name);
            if (option->opt.has_arg == no_argument) {
                printf("--%-20s", option->opt.name);
            } else if (option->opt.has_arg == optional_argument) {
                printf("--%s[=VAL]%*s", option->opt.name, 20 - (int)opt_len - 6, " ");
            } else {
                printf("--%s=VAL%*s", option->opt.name, 20 - (int)opt_len - 4, " ");
            }
        } else {
            printf("%-22s", " ");
        }
        if (option->description) {
            printf("  %s", option->description);
        }
        if (option->help) {
            size_t help_len = strlen(option->help);
            size_t ofs = 0;
            while (ofs < help_len) {
                size_t i = ofs + MAX_HELP_LINE_LENGTH;
                if (i > help_len) {
                    i = help_len;
                } else {
                    while(!isspace(option->help[i])) {
                        i--;
                    }
                }
                strncpy(help_buf, option->help + ofs, i - ofs);
                help_buf[i - ofs] = '\0';
                printf("\n%32s%s", " ", help_buf);
                ofs = i + 1;
            }

        }
        printf("\n");
    }
}

int main(int argc, char *argv[])
{
    // build a long_option array
    size_t long_options_size = sizeof(struct option) * (OPTION_COUNT + 1);
    struct option *long_options = malloc(long_options_size);
    memset(long_options, 0, long_options_size);

    for (size_t i = 0; i < OPTION_COUNT; i++)
    {
        long_options[i] = options[i].opt;
    }
    
    // TODO command line arguments for adapter and MQTT
    int c;
    int opt_index = 0;
    while (true)
    {
        c = getopt_long(argc, argv, "b:p:t:d", long_options, &opt_index);
        if (c == -1)
        {
            break;
        }
        switch (c) {
        case 'b':
        {
            size_t len = strlen(optarg);
            mqtt_broker = malloc(sizeof(char) * (len + 1));
            if (mqtt_broker == NULL)
            {
                fprintf(stderr, "Failed to allocate %zu bytes for mqtt broker string\n", len);
                exit(EXIT_FAILURE);
            }
            strncpy(mqtt_broker, optarg, len);
            break;
        }
        case 'p':
        {
            uint32_t mqtt_port_arg = strtoul(optarg, NULL, 10);
            if (mqtt_port_arg <= 1024 || mqtt_port_arg > 65535)
            {
                fprintf(stderr, "Invalid value %u for mqtt-port; must be between 1024 and 65535\n", mqtt_port_arg);
                return EXIT_FAILURE;
            }
            mqtt_port = mqtt_port_arg;
            break;
        }
        case 't':
        {
            size_t len = strlen(optarg);
            mqtt_topic = malloc(sizeof(char) * (len + 1));
            if (mqtt_topic == NULL)
            {
                fprintf(stderr, "Failed to allocate %zu bytes for mqtt topic string\n", len);
                return EXIT_FAILURE;
            }
            strncpy(mqtt_topic, optarg, len);
            break;
        }
        case 'd':
        {
            debug = true;
            break;
        }
        case 'h':
        case '?':
        default:
            usage();
            exit (EXIT_FAILURE);
        }
    }

    free(long_options);

    if (mqtt_topic == NULL)
    {
        fprintf(stderr, "Must specify a value for -t/--mqtt-topic\n\n");
        usage();
        exit(EXIT_FAILURE);
    }
    
    if (mqtt_broker == NULL)
    {
        fprintf(stderr, "Must specify a value for -b/--mqtt-broker\n\n");
        usage();
        exit(EXIT_FAILURE);
    }

      
    if (signal(SIGINT, sighandler) == SIG_ERR)
    {
        fprintf(stderr, "can't register sighandler\n");
        return -1;
    }

    // init mosquitto
    mosquitto_lib_init();

    // Create mosquitto instance -- must be ready before we load libcec
    mosq = mosquitto_new(NULL, true, NULL);
    //mosquitto_threaded_set(mosq, true);

    if (mosquitto_connect(mosq, mqtt_broker, mqtt_port, 0) != MOSQ_ERR_SUCCESS)
    {
        fprintf(stderr, "Could not connect to %s:%u\n", mqtt_broker, mqtt_port);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        exit(EXIT_FAILURE);
    }
      
    libcec_configuration config;
    libcec_interface_t   iface;

    // configure client
    libcecc_reset_configuration(&config);
    config.clientVersion = LIBCEC_VERSION_CURRENT;
    config.bActivateSource = 0;
    config.callbacks = &callbacks;
    config.deviceTypes.types[0] = CEC_DEVICE_TYPE_RECORDING_DEVICE;
    snprintf(config.strDeviceName, sizeof(config.strDeviceName), "cec-mqtt");

    int rc;
    if ((rc = libcecc_initialise(&config, &iface, NULL)) != 1)
    {
        fprintf(stderr, "Could not initialize libcec: %d\n", rc);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        exit(EXIT_FAILURE);
    }

    // Assume RPI for now
    if (!iface.open(iface.connection, "RPI", 5000))
    {
        fprintf(stderr, "Unable to open device on port RPI\n");
        libcecc_destroy(&iface);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        exit(EXIT_FAILURE);
    }

    
    

//    printf("Opened RPI device\n");

    while (!terminate)
    {
        // Nothing to do
        sleep(1);
    }
    

    libcecc_destroy(&iface);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    free(mqtt_broker);
    free(mqtt_topic);

    return 0;
}
