/* Exercise the real LwM2M callbacks and Observe controller with a fake board. */
#include "halow_sensors.h"
#include "sensors_hw.h"
#include "halow_device.h"
#include "device_platform.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static int64_t now = 100000;
static unsigned int observed, mask_calls, imu_reads, voltage_reads, changes;
static uint16_t changed_resource;
static int imu_error, voltage_error;
static double supply = 3.75;
static int reboot_count, last_factory_reset, error_code;
static double sample[3] = {1, 2, 3};
int64_t k_uptime_get(void) { return now; }
int board_imu_observe(unsigned int mask) { observed = mask; mask_calls++; return 0; }
int board_imu_read(unsigned int sensor, double axes[3])
{
    assert(sensor == EZ_IMU_ACCEL || sensor == EZ_IMU_GYRO);
    imu_reads++;
    if (imu_error) return imu_error;
    memcpy(axes, sample, sizeof(sample)); return 0;
}
int board_voltage_read(double *volts)
{
    voltage_reads++; if (voltage_error) return voltage_error; *volts = supply; return 0;
}
const char *board_device_model(void) { return "EZ-DEV54-GPS"; }
const char *board_device_serial(void) { return "factory-test-serial"; }
const char *board_device_software(void) { return "Zephyr-test"; }
uint32_t board_memory_free_kb(void) { return 55; }
uint32_t board_memory_total_kb(void) { return 262; }
int board_device_reboot(bool factory) { reboot_count++; last_factory_reset = factory; return 0; }
int board_device_error(void) { return error_code; }
void board_device_reset_errors(void) { error_code = 0; }
void lwm2m_data_encode_int(int64_t value, lwm2m_data_t *d) { d->type=LWM2M_TYPE_INTEGER;d->value.asInteger=value; }
int lwm2m_data_decode_int(const lwm2m_data_t *d, int64_t *value)
{
    if (d->type!=LWM2M_TYPE_INTEGER) return 0; *value=d->value.asInteger;return 1;
}
void lwm2m_data_encode_instances(lwm2m_data_t *children, size_t count, lwm2m_data_t *d)
{
    d->type=LWM2M_TYPE_MULTIPLE_RESOURCE;d->value.asChildren.array=children;d->value.asChildren.count=count;
}
static void free_data(int count, lwm2m_data_t *data)
{
    for(int i=0;i<count;i++) {
        if(data[i].type==LWM2M_TYPE_STRING) free(data[i].value.asBuffer.buffer);
        if(data[i].type==LWM2M_TYPE_MULTIPLE_RESOURCE) free_data(data[i].value.asChildren.count,data[i].value.asChildren.array);
    }
    free(data);
}
lwm2m_data_t *lwm2m_data_new(int n) { return calloc(n, sizeof(lwm2m_data_t)); }
void lwm2m_data_encode_float(double value, lwm2m_data_t *d) { d->type = LWM2M_TYPE_FLOAT; d->value.asFloat = value; }
void lwm2m_data_encode_string(const char *value, lwm2m_data_t *d)
{
    d->type = LWM2M_TYPE_STRING; d->value.asBuffer.length = strlen(value);
    d->value.asBuffer.buffer = (uint8_t *)strdup(value);
}
void lwm2m_resource_value_changed(lwm2m_context_t *ctx, lwm2m_uri_t *uri)
{
    assert(ctx && uri->instanceId == 0 && (uri->objectId==3 || (uri->resourceId >= 5702 && uri->resourceId <= 5704)));
    changes++; changed_resource = uri->resourceId;
}
static uint8_t read_one(lwm2m_context_t *ctx, lwm2m_object_t *object, uint16_t instance,
                        uint16_t resource, double *value)
{
    int count = 1; lwm2m_data_t *data = lwm2m_data_new(1); data->id = resource;
    uint8_t status = object->readFunc(ctx, instance, &count, &data, object);
    if (status == COAP_205_CONTENT && data->type == LWM2M_TYPE_FLOAT && value) *value = data->value.asFloat;
    if (status==COAP_205_CONTENT && value) {
        if (data->type==LWM2M_TYPE_INTEGER) *value=data->value.asInteger;
        if (data->type==LWM2M_TYPE_MULTIPLE_RESOURCE) {
            assert(data->value.asChildren.count==1 && data->value.asChildren.array[0].id==0);
            *value=data->value.asChildren.array[0].value.asInteger;
        }
    }
    free_data(count,data); return status;
}
static void setup(lwm2m_context_t *ctx, lwm2m_observed_t *obs, lwm2m_watcher_t *watcher,
                  lwm2m_server_t *server, uint16_t object, uint16_t resource)
{
    memset(ctx, 0, sizeof(*ctx)); memset(obs, 0, sizeof(*obs));
    memset(watcher, 0, sizeof(*watcher)); memset(server, 0, sizeof(*server));
    LWM2M_URI_RESET(&obs->uri); obs->uri.objectId = object; obs->uri.instanceId = 0;
    obs->uri.resourceId = resource; obs->watcherList = watcher; ctx->observedList = obs;
    watcher->active = true; watcher->server = server;
    server->status = STATE_REGISTERED; server->sessionH = (void *)1;
}
int main(int argc, char **argv)
{
    assert(argc == 2); int test = atoi(argv[1]); double value;
    lwm2m_context_t ctx; lwm2m_observed_t obs; lwm2m_watcher_t watcher; lwm2m_server_t server;
    setup(&ctx, &obs, &watcher, &server, 3313, 5702);
    if (test == 1) {
        ctx.observedList = NULL;
        assert(read_one(&ctx, &halow_accelerometer, 0, 5702, &value) == COAP_205_CONTENT && value == 1);
        assert(imu_reads == 1 && mask_calls == 0 && halow_sensors_step(&ctx) == -1);
    } else if (test == 2) {
        assert(read_one(&ctx, &halow_accelerometer, 0, 5701, NULL) == COAP_205_CONTENT);
        assert(read_one(&ctx, &halow_accelerometer, 1, 5702, NULL) == COAP_404_NOT_FOUND);
        assert(read_one(&ctx, &halow_accelerometer, 0, 9999, NULL) == COAP_404_NOT_FOUND);
        int count = 0; lwm2m_data_t *data = NULL;
        assert(halow_gyrometer.discoverFunc(&ctx, 0, &count, &data, &halow_gyrometer) == COAP_205_CONTENT);
        assert(count == 4); free(data);
        assert(imu_reads == 0 && voltage_reads == 0);
    } else if (test == 3) {
        watcher.active = false;
        assert(halow_sensors_step(&ctx) == -1 && mask_calls == 0);
        halow_sensors_internal_read(true);
        assert(read_one(&ctx, &halow_accelerometer, 0, 5702, NULL) == COAP_503_SERVICE_UNAVAILABLE);
        assert(imu_reads == 0);
    } else if (test == 4) {
        assert(halow_sensors_step(&ctx) == 1000 && observed == EZ_IMU_ACCEL && imu_reads == 1 && changes == 3);
        assert(halow_sensors_step(&ctx) == 1000 && imu_reads == 1);
        now += 1000; (void)halow_sensors_step(&ctx); assert(imu_reads == 2 && changes == 3);
        sample[1] += 1; now += 1000; (void)halow_sensors_step(&ctx);
        assert(changes == 4 && changed_resource == 5703);
        watcher.active = false; assert(halow_sensors_step(&ctx) == -1 && observed == 0);
        now += 100000; assert(halow_sensors_step(&ctx) == -1 && imu_reads == 3);
    } else if (test == 5) {
        lwm2m_observed_t gyro = obs; lwm2m_watcher_t gw = watcher;
        gyro.uri.objectId = 3334; gyro.watcherList = &gw; obs.next = &gyro;
        (void)halow_sensors_step(&ctx); assert(observed == 3 && imu_reads == 2);
        watcher.active = false; (void)halow_sensors_step(&ctx); assert(observed == EZ_IMU_GYRO);
        gw.active = false; assert(halow_sensors_step(&ctx) == -1 && observed == 0);
    } else if (test == 6) {
        (void)halow_sensors_step(&ctx); server.status = STATE_DEREGISTERED;
        assert(halow_sensors_step(&ctx) == -1 && observed == 0);
        server.status = STATE_REGISTERED; (void)halow_sensors_step(&ctx);
        assert(halow_sensors_step(NULL) == -1 && observed == 0);
    } else if (test == 7) {
        assert(read_one(&ctx, &halow_accelerometer, 0, 5702, NULL) == COAP_205_CONTENT);
        imu_error = -5;
        assert(read_one(&ctx, &halow_accelerometer, 0, 5702, NULL) == COAP_503_SERVICE_UNAVAILABLE);
        halow_sensors_internal_read(true);
        assert(read_one(&ctx, &halow_accelerometer, 0, 5702, NULL) == COAP_503_SERVICE_UNAVAILABLE);
        assert(imu_reads == 2);
    } else if (test == 8) {
        assert(read_one(&ctx, &halow_device, 0, 7, &value) == COAP_205_CONTENT && value == 3750);
        assert(read_one(&ctx, &halow_device, 0, 7, &value) == COAP_205_CONTENT && voltage_reads == 2);
        halow_sensors_internal_read(true);
        assert(read_one(&ctx, &halow_device, 0, 7, &value) == COAP_205_CONTENT && voltage_reads == 2);
        halow_sensors_internal_read(false); voltage_error = -5;
        assert(read_one(&ctx, &halow_device, 0, 7, NULL) == COAP_503_SERVICE_UNAVAILABLE);
    } else if (test == 9) {
        obs.uri.resourceId = 5701; assert(halow_sensors_step(&ctx) == -1 && imu_reads == 0 && mask_calls == 0);
    } else if (test == 10) {
        LWM2M_URI_RESET(&obs.uri); obs.uri.objectId = 3313;
        (void)halow_sensors_step(&ctx); assert(observed == EZ_IMU_ACCEL);
        watcher.active = false; (void)halow_sensors_step(&ctx); assert(observed == 0);
    } else if (test == 11) {
        const double voltages[]={2.8,3.0,3.6,4.2,4.5,5.1,6.0};
        const int percentages[]={0,0,50,100,100,100,100};
        for(unsigned i=0;i<sizeof(percentages)/sizeof(percentages[0]);i++) {
            supply=voltages[i];
            assert(read_one(&ctx,&halow_device,0,9,&value)==COAP_205_CONTENT && value==percentages[i]);
        }
    } else if (test == 12) {
        assert(read_one(&ctx,&halow_device,0,6,&value)==COAP_205_CONTENT && value==1);
        assert(read_one(&ctx,&halow_device,0,10,&value)==COAP_205_CONTENT && value==55);
        assert(read_one(&ctx,&halow_device,0,21,&value)==COAP_205_CONTENT && value==262);
        assert(read_one(&ctx,&halow_device,0,20,&value)==COAP_205_CONTENT && value==6);
        assert(read_one(&ctx,&halow_device,0,8,NULL)==COAP_404_NOT_FOUND);
        assert(read_one(&ctx,&halow_device,0,22,NULL)==COAP_404_NOT_FOUND);
        assert(read_one(&ctx,&halow_device,0,4,NULL)==COAP_405_METHOD_NOT_ALLOWED);
        assert(voltage_reads==0 && imu_reads==0);
    } else if (test == 13) {
        assert(read_one(&ctx,&halow_device,0,13,NULL)==COAP_503_SERVICE_UNAVAILABLE);
        lwm2m_data_t data={.id=13}; lwm2m_data_encode_int(1700000000,&data);
        assert(halow_device.writeFunc(&ctx,0,1,&data,&halow_device,LWM2M_WRITE_PARTIAL_UPDATE)==COAP_204_CHANGED);
        now+=2500;
        assert(read_one(&ctx,&halow_device,0,13,&value)==COAP_205_CONTENT && value==1700000002);
        lwm2m_data_t write[2]={{.id=13},{.id=14,.type=LWM2M_TYPE_STRING}};
        lwm2m_data_encode_int(1800000000,&write[0]);
        write[1].value.asBuffer.buffer=(uint8_t *)"+25:00";write[1].value.asBuffer.length=6;
        assert(halow_device.writeFunc(&ctx,0,2,write,&halow_device,LWM2M_WRITE_PARTIAL_UPDATE)==COAP_400_BAD_REQUEST);
        assert(read_one(&ctx,&halow_device,0,13,&value)==COAP_205_CONTENT && value==1700000002);
        write[1].value.asBuffer.buffer=(uint8_t *)"+01:00";
        assert(halow_device.writeFunc(&ctx,0,1,&write[1],&halow_device,LWM2M_WRITE_PARTIAL_UPDATE)==COAP_204_CHANGED);
    } else if (test == 14) {
        assert(halow_device.executeFunc(&ctx,0,4,NULL,0,&halow_device)==COAP_204_CHANGED);
        assert(reboot_count==1 && !last_factory_reset);
        assert(halow_device.executeFunc(&ctx,0,5,NULL,0,&halow_device)==COAP_204_CHANGED);
        assert(reboot_count==2 && last_factory_reset);
        error_code=8;
        assert(read_one(&ctx,&halow_device,0,11,&value)==COAP_205_CONTENT && value==8);
        assert(halow_device.executeFunc(&ctx,0,12,NULL,0,&halow_device)==COAP_204_CHANGED);
        assert(read_one(&ctx,&halow_device,0,11,&value)==COAP_205_CONTENT && value==0);
        assert(halow_device.executeFunc(&ctx,0,4,(uint8_t *)"x",1,&halow_device)==COAP_400_BAD_REQUEST);
    } else if (test == 15) {
        int count=0;lwm2m_data_t *data=NULL;
        assert(halow_device.readFunc(&ctx,0,&count,&data,&halow_device)==COAP_205_CONTENT);
        assert(voltage_reads==1);bool have_voltage=false,have_percent=false,have_serial=false,have_model=false;
        for(int i=0;i<count;i++) {
            have_voltage|=data[i].id==7;have_percent|=data[i].id==9;
            if(data[i].id==1) {have_model=true;assert(data[i].value.asBuffer.length==strlen("EZ-DEV54-GPS") && !memcmp(data[i].value.asBuffer.buffer,"EZ-DEV54-GPS",strlen("EZ-DEV54-GPS")));}
            if(data[i].id==2) {have_serial=true;assert(data[i].value.asBuffer.length==strlen("factory-test-serial"));}
            assert(data[i].id!=8 && data[i].id!=13 && data[i].id!=22);
        }
        assert(have_voltage && have_percent && have_serial && have_model);free_data(count,data);
    } else if (test == 16) {
        setup(&ctx,&obs,&watcher,&server,3,13);
        assert(halow_device_step(&ctx)==-1); /* UTC not initialized */
        halow_device_sync_clock(&ctx,1700000000000LL); changes=0;
        watcher.active=false; assert(halow_device_step(&ctx)==-1);
        watcher.active=true; assert(halow_device_step(&ctx)==1000 && changes==0);
        now+=500; assert(halow_device_step(&ctx)==500 && changes==0);
        now+=500; assert(halow_device_step(&ctx)==1000 && changes==1 && changed_resource==13);
        assert(read_one(&ctx,&halow_device,0,13,&value)==COAP_205_CONTENT && value==1700000001);
        watcher.active=false; assert(halow_device_step(&ctx)==-1);
        now+=9000; assert(halow_device_step(&ctx)==-1 && changes==1);
        /* Local UTC still ticks without an observer or any server response. */
        assert(read_one(&ctx,&halow_device,0,13,&value)==COAP_205_CONTENT && value==1700000010);
        watcher.active=true; obs.uri.resourceId=7; assert(halow_device_step(&ctx)==-1);
        LWM2M_URI_RESET(&obs.uri);obs.uri.objectId=3;
        assert(halow_device_step(&ctx)==1000);server.status=STATE_DEREGISTERED;
        assert(halow_device_step(&ctx)==-1 && voltage_reads==0 && imu_reads==0);
    } else return 2;
    puts("OK"); return 0;
}

const char *hardware_type_name(void) { return "HaLow camera with IMU"; }
