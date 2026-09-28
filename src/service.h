#ifndef SERVICE_H
#define SERVICE_H

#include <windows.h>
#include <stdbool.h>

#define SVC_NAME  "LANTimeSync"
#define SVC_NAME_W L"LANTimeSync"
#define SVC_DISPLAY "LAN Time Sync Client"

typedef enum {
    SVC_NOT_INSTALLED = 0,
    SVC_STOPPED,
    SVC_RUNNING,
    SVC_START_PENDING,
    SVC_STOP_PENDING,
    SVC_UNKNOWN,
} svc_state_t;

/* register the service's entry points and block in the SCM dispatcher.
   Call from main() when running as a service. Returns false if not started
   by SCM (e.g. launched manually). */
bool service_run(void);

/* --- install/uninstall helpers, callable from command line (elevated) --- */
bool service_install(const char* exe_path, bool auto_start);
bool service_uninstall(void);

/* --- SCM query / control (callable from GUI, no elevation required for query) --- */
svc_state_t service_query_state(void);
const char* service_state_name(svc_state_t s);
bool service_start_svc(void);
bool service_stop_svc(void);
bool service_is_elevated(void);

/* set by the service control handler; the loop checks this */
void  service_set_stopping(void);
bool  service_is_stopping(void);

#endif /* SERVICE_H */
