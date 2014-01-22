/*
 * Copyright © 2013 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Daniel d'Andrada <daniel.dandrada@canonical.com>
 */
#include <ubuntu/hardware/gps.h>

#include <pthread.h>
#include <stdio.h>

// android stuff
#include <hardware/gps.h>
#include <hardware_legacy/power.h>

#define WAKE_LOCK_NAME  "U_HARDWARE_GPS"

struct UHardwareGps_
{
    UHardwareGps_(UHardwareGpsParams* params);
    ~UHardwareGps_();

    bool init();

    bool start();
    bool stop();
    void inject_time(int64_t time, int64_t timeReference, int uncertainty);
    void inject_location(double latitude, double longitude, float accuracy);
    void delete_aiding_data(UHardwareGpsAidingData flags);

    void set_server_for_type(UHardwareGpsAGpsType type, const char* hostname, uint16_t port);
    void set_reference_location(UHardwareGpsAGpsRefLocation* location, size_t size_of_struct);

    void notify_connection_is_open(const char* apn);
    void notify_connection_is_closed();
    void notify_connection_not_available();

    bool set_position_mode(uint32_t mode, uint32_t recurrence, uint32_t min_interval,
                           uint32_t preferred_accuracy, uint32_t preferred_time);
    void inject_xtra_data(char* data, int length);

    const GpsInterface* gps_interface;
    const GpsXtraInterface* gps_xtra_interface;
    const AGpsInterface* agps_interface;
    const GpsNiInterface* gps_ni_interface;
    const GpsDebugInterface* gps_debug_interface;
    const AGpsRilInterface* agps_ril_interface;

    UHardwareGpsLocationCallback location_cb;
    UHardwareGpsStatusCallback status_cb;
    UHardwareGpsSvStatusCallback sv_status_cb;
    UHardwareGpsNmeaCallback nmea_cb;
    UHardwareGpsSetCapabilities set_capabilities_cb;
    UHardwareGpsRequestUtcTime request_utc_time_cb;

    UHardwareGpsXtraDownloadRequest xtra_download_request_cb;

    UHardwareGpsAGpsStatusCallback agps_status_cb;

    UHardwareGpsNiNotifyCallback gps_ni_notify_cb;

    UHardwareGpsAGpsRilRequestSetId request_setid_cb;
    UHardwareGpsAGpsRilRequestRefLoc request_refloc_cb;

    void* context;
};

namespace
{
UHardwareGps hybris_gps_instance = NULL;
}

static void location_callback(GpsLocation* location)
{
    printf("%s \n", __PRETTY_FUNCTION__);
    if (!hybris_gps_instance)
        return;

    hybris_gps_instance->location_cb(
        reinterpret_cast<UHardwareGpsLocation*>(location),
        hybris_gps_instance->context);
}

static void status_callback(GpsStatus* status)
{
    printf("%s \n", __PRETTY_FUNCTION__);
    if (!hybris_gps_instance)
        return;

    hybris_gps_instance->status_cb(status->status, hybris_gps_instance->context);
}

static void sv_status_callback(GpsSvStatus* sv_status)
{
    printf("%s \n", __PRETTY_FUNCTION__);
    if (!hybris_gps_instance)
        return;

    hybris_gps_instance->sv_status_cb(
            reinterpret_cast<UHardwareGpsSvStatus*>(sv_status),
            hybris_gps_instance->context);
}

static void nmea_callback(GpsUtcTime timestamp, const char* nmea, int length)
{
    printf("%s \n", __PRETTY_FUNCTION__);
    if (!hybris_gps_instance)
        return;

    hybris_gps_instance->nmea_cb(timestamp, nmea, length, hybris_gps_instance->context);
}

static void set_capabilities_callback(uint32_t capabilities)
{
    printf("%s \n", __PRETTY_FUNCTION__);
    if (!hybris_gps_instance)
        return;

    hybris_gps_instance->set_capabilities_cb(capabilities, hybris_gps_instance->context);
}

static void acquire_wakelock_callback()
{
    printf("%s \n", __PRETTY_FUNCTION__);
    acquire_wake_lock(PARTIAL_WAKE_LOCK, WAKE_LOCK_NAME);
}

static void release_wakelock_callback()
{
    printf("%s \n", __PRETTY_FUNCTION__);
    release_wake_lock(WAKE_LOCK_NAME);
}

static void request_utc_time_callback()
{
    printf("%s \n", __PRETTY_FUNCTION__);
    if (!hybris_gps_instance)
        return;

    hybris_gps_instance->request_utc_time_cb(hybris_gps_instance->context);
}


typedef struct 
{
    void (*func)(void *);
    void *arg;
} FuncAndArg;

static void * thread_start_wrapper(void* arg)
{
    printf("%s \n", __PRETTY_FUNCTION__);
    FuncAndArg *func_and_arg = reinterpret_cast<FuncAndArg*>(arg);
    func_and_arg->func(func_and_arg->arg);
    delete func_and_arg;
    return NULL;
}

static pthread_t create_thread_callback(const char* name, void (*start)(void *), void* arg)
{
    printf("%s: %s \n", __PRETTY_FUNCTION__, name);
    pthread_t thread;

    FuncAndArg *func_and_arg = new FuncAndArg;
    func_and_arg->func = start;
    func_and_arg->arg = arg;

    pthread_create(&thread, NULL, thread_start_wrapper, func_and_arg);
    return thread;
}

GpsCallbacks gps_callbacks =
{
    sizeof(GpsCallbacks),
    location_callback,
    status_callback,
    sv_status_callback,
    nmea_callback,
    set_capabilities_callback,
    acquire_wakelock_callback,
    release_wakelock_callback,
    create_thread_callback,
    request_utc_time_callback,
};

static void xtra_download_request_callback()
{
    printf("%s \n", __PRETTY_FUNCTION__);
    if (hybris_gps_instance)
        hybris_gps_instance->xtra_download_request_cb(hybris_gps_instance->context);
}

GpsXtraCallbacks gps_xtra_callbacks =
{
    xtra_download_request_callback,
    create_thread_callback,
};

static void agps_status_cb(AGpsStatus* agps_status)
{
    printf("%s \n", __PRETTY_FUNCTION__);
    if (!hybris_gps_instance)
        return;

    /*
    uint32_t ipaddr;
    // ipaddr field was not included in original AGpsStatus
    if (agps_status->size >= sizeof(AGpsStatus))
        ipaddr = agps_status->ipaddr;
    else
        ipaddr = 0xFFFFFFFF;
    */

    hybris_gps_instance->agps_status_cb(
        reinterpret_cast<UHardwareGpsAGpsStatus*>(agps_status), hybris_gps_instance->context);
}

AGpsCallbacks agps_callbacks =
{
    agps_status_cb,
    create_thread_callback,
};

static void gps_ni_notify_cb(GpsNiNotification *notification)
{
    printf("%s \n", __PRETTY_FUNCTION__);
    if (hybris_gps_instance)
        hybris_gps_instance->gps_ni_notify_cb(
            reinterpret_cast<UHardwareGpsNiNotification*>(notification),
            hybris_gps_instance->context);
}

GpsNiCallbacks gps_ni_callbacks =
{
    gps_ni_notify_cb,
    create_thread_callback,
};

static void agps_request_set_id(uint32_t flags)
{
    printf("%s \n", __PRETTY_FUNCTION__);
    if (hybris_gps_instance)
        hybris_gps_instance->request_setid_cb(flags, hybris_gps_instance->context);
}

static void agps_request_ref_location(uint32_t flags)
{
    printf("%s \n", __PRETTY_FUNCTION__);
    if (hybris_gps_instance)
        hybris_gps_instance->request_refloc_cb(flags, hybris_gps_instance->context);
}

AGpsRilCallbacks agps_ril_callbacks =
{
    agps_request_set_id,
    agps_request_ref_location,
    create_thread_callback,
};


UHardwareGps_::UHardwareGps_(UHardwareGpsParams* params)
    : gps_interface(NULL),
      gps_xtra_interface(NULL),
      agps_interface(NULL),
      gps_ni_interface(NULL),
      gps_debug_interface(NULL),
      agps_ril_interface(NULL),
      location_cb(params->location_cb),
      status_cb(params->status_cb),
      sv_status_cb(params->sv_status_cb),
      nmea_cb(params->nmea_cb),
      set_capabilities_cb(params->set_capabilities_cb),
      request_utc_time_cb(params->request_utc_time_cb),
      xtra_download_request_cb(params->xtra_download_request_cb),
      agps_status_cb(params->agps_status_cb),
      gps_ni_notify_cb(params->gps_ni_notify_cb),
      request_setid_cb(params->request_setid_cb),
      request_refloc_cb(params->request_refloc_cb),
      context(params->context)
{
    int err;
    hw_module_t* module;

    err = hw_get_module(GPS_HARDWARE_MODULE_ID, (hw_module_t const**)&module);
    if (err == 0)
    {
        hw_device_t* device;
        err = module->methods->open(module, GPS_HARDWARE_MODULE_ID, &device);
        if (err == 0)
        {
            gps_device_t* gps_device = (gps_device_t *)device;
            gps_interface = gps_device->get_gps_interface(gps_device);
        }
    }
    if (gps_interface)
    {
        gps_xtra_interface =
            (const GpsXtraInterface*)gps_interface->get_extension(GPS_XTRA_INTERFACE);
        agps_interface =
            (const AGpsInterface*)gps_interface->get_extension(AGPS_INTERFACE);
        gps_ni_interface =
            (const GpsNiInterface*)gps_interface->get_extension(GPS_NI_INTERFACE);
        gps_debug_interface =
            (const GpsDebugInterface*)gps_interface->get_extension(GPS_DEBUG_INTERFACE);
        agps_ril_interface =
            (const AGpsRilInterface*)gps_interface->get_extension(AGPS_RIL_INTERFACE);
    }
}

UHardwareGps_::~UHardwareGps_()
{
    if (gps_interface)
        gps_interface->cleanup();
}

bool UHardwareGps_::init()
{
    // fail if the main interface fails to initialize
    if (!gps_interface || gps_interface->init(&gps_callbacks) != 0)
        return false;

    // if XTRA initialization fails we will disable it by gps_Xtra_interface to null,
    // but continue to allow the rest of the GPS interface to work.
    if (gps_xtra_interface && gps_xtra_interface->init(&gps_xtra_callbacks) != 0)
        gps_xtra_interface = NULL;
    if (agps_interface)
        agps_interface->init(&agps_callbacks);
    if (gps_ni_interface)
        gps_ni_interface->init(&gps_ni_callbacks);
    if (agps_ril_interface)
        agps_ril_interface->init(&agps_ril_callbacks);

    return true;
}

bool UHardwareGps_::start()
{
    if (gps_interface)
        return (gps_interface->start() == 0);
    else
        return false;
}

bool UHardwareGps_::stop()
{
    if (gps_interface)
        return (gps_interface->stop() == 0);
    else
        return false;
}

void UHardwareGps_::inject_time(int64_t time, int64_t time_reference, int uncertainty)
{
    if (gps_interface)
        gps_interface->inject_time(time, time_reference, uncertainty);
}

void UHardwareGps_::inject_location(double latitude, double longitude, float accuracy)
{
    printf("%s: %f %f %f \n", __PRETTY_FUNCTION__, latitude, longitude, accuracy);
    if (gps_interface && gps_interface->inject_location)
        gps_interface->inject_location(latitude, longitude, accuracy);
}

void UHardwareGps_::delete_aiding_data(uint16_t flags)
{
    if (gps_interface)
        gps_interface->delete_aiding_data(flags);
}

void UHardwareGps_::set_server_for_type(UHardwareGpsAGpsType type, const char* hostname, uint16_t port)
{
    printf("%s \n", __PRETTY_FUNCTION__);
    if (agps_interface)
        agps_interface->set_server(type, hostname, port);
}

void UHardwareGps_::set_reference_location(UHardwareGpsAGpsRefLocation* location, size_t size_of_struct)
{
    AGpsRefLocation ref_loc;
    ref_loc.type = location->type;
    ref_loc.u.cellID.type = location->u.cellID.type;
    ref_loc.u.cellID.mcc = location->u.cellID.mcc;
    ref_loc.u.cellID.mnc = location->u.cellID.mnc;
    ref_loc.u.cellID.lac = location->u.cellID.lac;
    ref_loc.u.cellID.cid = location->u.cellID.cid;

    if (agps_ril_interface)
        agps_ril_interface->set_ref_location(&ref_loc, sizeof(ref_loc));
}

void UHardwareGps_::notify_connection_is_open(const char* apn)
{
    if (agps_interface)
        agps_interface->data_conn_open(apn);
}

void UHardwareGps_::notify_connection_is_closed()
{
    if (agps_interface)
        agps_interface->data_conn_closed();
}

void UHardwareGps_::notify_connection_not_available()
{
    if (agps_interface)
        agps_interface->data_conn_failed();
}

bool UHardwareGps_::set_position_mode(uint32_t mode, uint32_t recurrence, uint32_t min_interval,
                                    uint32_t preferred_accuracy, uint32_t preferred_time)
{
    printf("%s \n", __PRETTY_FUNCTION__);

    if (gps_interface)
        return (gps_interface->set_position_mode(mode, recurrence, min_interval,
                                                 preferred_accuracy, preferred_time) == 0);
    else
        return false;
}

void UHardwareGps_::inject_xtra_data(char* data, int length)
{
    if (gps_xtra_interface)
        gps_xtra_interface->inject_xtra_data(data, length);
}

/////////////////////////////////////////////////////////////////////
// Implementation of the C API

UHardwareGps u_hardware_gps_new(UHardwareGpsParams* params)
{
    if (hybris_gps_instance != NULL)
        return NULL;

    UHardwareGps u_hardware_gps = new UHardwareGps_(params);
    hybris_gps_instance = u_hardware_gps;

    if (!u_hardware_gps->init())
    {
        delete u_hardware_gps;
        u_hardware_gps = NULL;
    }

    return u_hardware_gps;
}

void u_hardware_gps_delete(UHardwareGps handle)
{
    delete handle;
    if (handle == hybris_gps_instance)
        hybris_gps_instance = NULL;
}

bool u_hardware_gps_start(UHardwareGps self)
{
    return self->start();
}

bool u_hardware_gps_stop(UHardwareGps self)
{
    return self->stop();
}

void u_hardware_gps_inject_time(UHardwareGps self, int64_t time, int64_t time_reference,
                            int uncertainty)
{
    self->inject_time(time, time_reference, uncertainty);
}

void u_hardware_gps_inject_location(UHardwareGps self, double latitude, double longitude,
                                float accuracy)
{
    printf("%s: %f %f %f \n", __PRETTY_FUNCTION__, latitude, longitude, accuracy);
    self->inject_location(latitude, longitude, accuracy);
}

void u_hardware_gps_delete_aiding_data(UHardwareGps self, UHardwareGpsAidingData flags)
{
    self->delete_aiding_data(flags);
}

void u_hardware_gps_agps_set_server_for_type(
        UHardwareGps self,
        UHardwareGpsAGpsType type,
        const char* hostname,
        uint16_t port)
{
    self->set_server_for_type(type, hostname, port);
}

void u_hardware_gps_agps_set_reference_location(
    UHardwareGps self,
    UHardwareGpsAGpsRefLocation *location,
    size_t size_of_struct)
{
    self->set_reference_location(location, size_of_struct);
}

void u_hardware_gps_agps_notify_connection_is_open(
    UHardwareGps self,
    const char *apn)
{
    self->notify_connection_is_open(apn);
}

void u_hardware_gps_agps_notify_connection_is_closed(UHardwareGps self)
{
    self->notify_connection_is_closed();
}

void u_hardware_gps_agps_notify_connection_not_available(UHardwareGps self)
{
    self->notify_connection_not_available();
}


bool u_hardware_gps_set_position_mode(UHardwareGps self, uint32_t mode, uint32_t recurrence,
                                  uint32_t min_interval, uint32_t preferred_accuracy,
                                  uint32_t preferred_time)
{
    return self->set_position_mode(mode, recurrence, min_interval, preferred_accuracy,
                                   preferred_time);
}

void u_hardware_gps_inject_xtra_data(UHardwareGps self, char* data, int length)
{
    self->inject_xtra_data(data, length);
}
