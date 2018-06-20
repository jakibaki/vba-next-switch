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
        //printf("Couldn't get lightsensor service :/\n");
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
    int v = getLightSensorPercentage() / 10;
    int value = 0;
    switch (v)
    {
    case 0:
        value = 0x06;
        break;
    case 1:
        value = 0x06;
        break;
    case 2:
        value = 0x0E;
        break;
    case 3:
        value = 0x18;
        break;
    case 4:
        value = 0x20;
        break;
    case 5:
        value = 0x28;
        break;
    case 6:
        value = 0x38;
        break;
    case 7:
        value = 0x48;
        break;
    case 8:
        value = 0x60;
        break;
    case 9:
        value = 0x78;
        break;
    case 10:
        value = 0x98;
        break;
    default:
        value = 0x98;
        break;
    }

    return 0xE8 - value;
}