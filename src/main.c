#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>


#include <libcec/cecc.h>
#include <libcec/cectypes.h>
#include "ceccloader.h"

#include <jansson.h>
#include <mosquitto.h>

#define MQTT_HOST ""
#define MQTT_PORT 1883
#define MQTT_TOPIC "media_player/living_room/tv"

typedef struct tv_state {
    cec_power_status power_status;
    uint8_t hdmi_input;
} TvState;


static bool terminate  = false;
static bool debug = false;
static struct mosquitto* mosq = NULL;

static void cb_cec_command_received(void *lib, const cec_command* command);
static void cb_cec_log_message(void *lib, const cec_log_message* message);
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
        printf("TV State: (%s [HDMI %d]) -> (%s [HDMI %d])\n",
               oldpower, old.hdmi_input,
               newpower, tv_state.hdmi_input
        );

        json_t *json = json_object();
        json_object_set_new(json, "power_state", json_string(newpower));
        json_object_set_new(json, "hdmi_input", json_integer(tv_state.hdmi_input));
        char *json_string = json_dumps(json, JSON_INDENT(2));

        if (mosq)
        {
            int rc = mosquitto_publish(mosq, NULL, MQTT_TOPIC, strlen(json_string), json_string, 0, true);
            // TODO reconnect on failure?
        }
        
        printf("%s\n", json_string);
        free (json_string);
        json_object_clear(json);
        json_decref(json);
    }
}

static void cb_cec_log_message(void *p, const cec_log_message* message)
{
    (void)p;
    printf("%s\n", message->message);
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
    // TODO
    terminate = true;
}



int main(int argc, char *argv[])
{
    // TODO command line arguments for adapter and MQTT
    (void)argc;
    (void)argv;
    if (signal(SIGINT, sighandler) == SIG_ERR)
    {
        printf("can't register sighandler\n");
        return -1;
    }

    // init mosquitto
    mosquitto_lib_init();

    // Create mosquitto instance -- must be ready before we load libcec
    mosq = mosquitto_new(NULL, true, NULL);
    //mosquitto_threaded_set(mosq, true);

    if (mosquitto_connect(mosq, MQTT_HOST, MQTT_PORT, 0) != MOSQ_ERR_SUCCESS)
    {
        fprintf(stderr, "Could not connect to %s\n", MQTT_HOST);
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

    return 0;
}
