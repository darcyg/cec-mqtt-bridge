#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>


#include <libcec/cecc.h>
#include <libcec/cectypes.h>
#include "ceccloader.h"


static bool terminate  = false;


static void cb_cec_configuration_changed(void *lib, const libcec_configuration* configuration);
static void cb_cec_command_received(void *lib, const cec_command* command);
static void cb_cec_log_message(void *lib, const cec_log_message* message);

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
static ICECCallbacks        g_callbacks = {
    .logMessage           = NULL,
    .keyPress             = NULL,
    .commandReceived      = cb_cec_command_received,
    .configurationChanged = cb_cec_configuration_changed,
    .alert                = NULL,
    .menuStateChanged     = NULL,
    .sourceActivated      = NULL
};



static void cb_cec_configuration_changed(void *lib, const libcec_configuration* configuration)
{
    printf("%s\n", configuration->strDeviceName);
}

static void cb_cec_command_received(void *lib, const cec_command* command)
{
    char buffer[24];

    if (command->initiator == CECDEVICE_TV)
    {
        // Check for the following TV message:
        // 1. Report power status.
        // 2. Standby
        // 3. Vendor code with ID? (TBD)
        switch (command->opcode)
        {
        case CEC_OPCODE_REPORT_POWER_STATUS:
            libcec_power_status_to_string(command->parameters.data[0], buffer, sizeof(buffer));
            printf("TV State: %s\n", buffer);
            break;
        case CEC_OPCODE_VENDOR_COMMAND_WITH_ID:
        case CEC_OPCODE_STANDBY:
            printf("TV State: standby\n");
            break;
        case CEC_OPCODE_REPORT_PHYSICAL_ADDRESS:
            printf("TV State: on\n");
            // printf("TV State: ??? (");
            // for (int i = 0; i < command->parameters.size; i++)
            // {
            //     printf("%02x ", command->parameters.data[i]);
            // }
            // printf(")\n");
            break;
        default:
            break;
        }
    }
}

static void cb_cec_log_message(void *lib, const cec_log_message* message)
{
    printf("%s\n", message->message);
}


static void sighandler(int signal)
{
    // TODO
    terminate = true;
}



int main(int argc, char *argv[])
{
    if (signal(SIGINT, sighandler) == SIG_ERR)
    {
        printf("can't register sighandler\n");
        return -1;
    }
      
    libcec_configuration config;
    libcec_interface_t   iface;

    // configure client
    libcecc_reset_configuration(&config);
    config.clientVersion = LIBCEC_VERSION_CURRENT;
    config.bActivateSource = 0;
    config.callbacks = &g_callbacks;
    config.deviceTypes.types[0] = CEC_DEVICE_TYPE_RECORDING_DEVICE;
    snprintf(config.strDeviceName, sizeof(config.strDeviceName), "cec-mqtt");

    int rc;
    if ((rc = libcecc_initialise(&config, &iface, NULL)) != 1)
    {
        fprintf(stderr, "Could not initialize libcec: %d\n", rc);
        exit(EXIT_FAILURE);
    }

    // Assume RPI for now

    if (!iface.open(iface.connection, "RPI", 5000))
    {
        fprintf(stderr, "Unable to open device on port RPI\n");
        libcecc_destroy(&iface);
        exit(EXIT_FAILURE);
    }


//    printf("Opened RPI device\n");

    while (!terminate)
    {
        sleep(1);
    }
    

    libcecc_destroy(&iface);
    return 0;
}
