#include "lightsensor.h"
#include <stdio.h>

static Service lblSvc;

Result lblInit(void)
{
    if (serviceIsActive(&lblSvc))
        return 0;

    Result rc;

    rc = smGetService(&lblSvc, "lbl");

    return rc;
}

int getLightSensorPercentage()
{
    lblInit();

    if (R_FAILED(lblInit()))
    {
        #ifdef NXLINK_STDIO
        printf("Couldn't get lightsensor service :/\n");
        #endif
        return 0;
    }

    IpcCommand c;
    ipcInitialize(&c);

    struct
    {
        u64 magic;
        u64 cmd_id;
    } * raw;

    raw = ipcPrepareHeader(&c, sizeof(*raw));
    raw->magic = SFCI_MAGIC;
    raw->cmd_id = 16;

    Result rc = serviceIpcDispatch(&lblSvc);

    if (R_SUCCEEDED(rc))
    {
        IpcParsedCommand r;
        ipcParse(&r);

        struct
        {
            u64 magic;
            u64 result;
            u64 light;
        } *resp = r.Raw;

        rc = resp->result;
        if (R_SUCCEEDED(rc))
        {
            // The value is either 0 or this **really** large number roughly between 450e16 and 500e16
            // Don't ask my why it is that way D:

            u64 rawlight = resp->light;

            if (rawlight < 450e16)
                return 0;
            int perc = (rawlight - 450e16) * 2 / 1e16;
            if (perc > 100)
                return 100;
            return perc;
        }
    }

    return 0;
}

uint8_t getSensorDarkness()
{
    // Returns the sensor darkness in the format that Boktai expects

    // 0xE8 is complete darkness
    // 
    int value = getLightSensorPercentage() * 0xA0 / 100;
    return 0xE8 - value;
}