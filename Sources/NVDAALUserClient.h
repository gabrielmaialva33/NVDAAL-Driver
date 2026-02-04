/*
 * NVDAALUserClient.h - IOUserClient for NVDAAL Driver
 *
 * Handles communication between user-space (firmware loader) and kernel driver.
 * securely maps firmware memory and triggers GSP boot.
 */

#ifndef NVDAAL_USER_CLIENT_H
#define NVDAAL_USER_CLIENT_H

#include <IOKit/IOUserClient.h>
#include "NVDAAL.h"

class NVDAALUserClient : public IOUserClient {
    OSDeclareDefaultStructors(NVDAALUserClient);

private:
    NVDAAL *provider;
    task_t clientTask;

public:
    // Lifecycle
    virtual bool initWithTask(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual IOReturn clientClose(void) override;

    // Dispatcher
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                                    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) override;

    // Methods
    IOReturn methodLoadFirmware(IOExternalMethodArguments *args);
    IOReturn methodAllocVram(IOExternalMethodArguments *args);
    IOReturn methodSubmitCommand(IOExternalMethodArguments *args);
    IOReturn methodWaitSemaphore(IOExternalMethodArguments *args);
    IOReturn methodLoadBooterLoad(IOExternalMethodArguments *args);
    IOReturn methodLoadVbios(IOExternalMethodArguments *args);
    IOReturn methodLoadBootloader(IOExternalMethodArguments *args);
    IOReturn methodGetStatus(IOExternalMethodArguments *args);
    IOReturn methodExecuteFwsec(IOExternalMethodArguments *args);
};

// Method Selectors
enum {
    kNVDAALMethodLoadFirmware = 0,
    kNVDAALMethodAllocVram,
    kNVDAALMethodSubmitCommand,
    kNVDAALMethodWaitSemaphore,
    kNVDAALMethodLoadBooterLoad,
    kNVDAALMethodLoadVbios,
    kNVDAALMethodLoadBootloader,
    kNVDAALMethodGetStatus,
    kNVDAALMethodExecuteFwsec,
    kNVDAALMethodCount
};

#endif // NVDAAL_USER_CLIENT_H
