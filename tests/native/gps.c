/* Actual GPS object and NMEA parser, fake UART/power/clock boundary. */
#include "halow_gps.h"
#include "gps_nmea.h"
#include "gps_diagnostics.h"
#include "gps_hw.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t now;
static bool powered;
static int starts, stops, start_error, read_error, stop_error;
static char input[2048];
static size_t offset, input_size;
int64_t k_uptime_get(void) { return now; }
int gps_hw_init(void) { assert(!powered); return 0; }
int gps_hw_start(void) { ++starts; powered = true; return start_error; }
int gps_hw_stop(void) { ++stops; powered = false; offset = input_size = 0; return stop_error; }
int gps_hw_read(uint8_t *data, size_t length) {
    if (read_error) return read_error;
    size_t n = input_size - offset; if (n > length) n = length;
    memcpy(data, input + offset, n); offset += n; return (int)n;
}
lwm2m_data_t *lwm2m_data_new(int n) { return calloc(n, sizeof(lwm2m_data_t)); }
void lwm2m_data_encode_int(int64_t value, lwm2m_data_t *d) { d->type=LWM2M_TYPE_INTEGER;d->value.asInteger=value; }
void lwm2m_data_encode_bool(bool value, lwm2m_data_t *d) { d->type=LWM2M_TYPE_BOOLEAN;d->value.asBoolean=value; }
void lwm2m_data_encode_float(double value, lwm2m_data_t *d) { d->type=LWM2M_TYPE_FLOAT;d->value.asFloat=value; }
int lwm2m_data_decode_int(const lwm2m_data_t *d,int64_t *value) {
    if(d->type!=LWM2M_TYPE_INTEGER)return 0;*value=d->value.asInteger;return 1;
}
void lwm2m_resource_value_changed(lwm2m_context_t *ctx,lwm2m_uri_t *uri) {
    assert(ctx && (uri->objectId==6 || uri->objectId==31028));
    /* A completed fix is readable even while first-fix tracking stays powered. */
}
static void sentence(char *out,size_t capacity,const char *body) {
    unsigned int sum=0;for(const char *p=body;*p;++p)sum^=(unsigned char)*p;
    snprintf(out,capacity,"$%s*%02X\r\n",body,sum);
}
static void feed(const char *body) { sentence(input,sizeof input,body);offset=0;input_size=strlen(input); }
static const char *rmc="GNRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W,A";
static uint8_t execute(lwm2m_context_t *ctx) {return halow_gps_control.executeFunc(ctx,0,0,NULL,0,&halow_gps_control);}
static int64_t control(lwm2m_context_t *ctx,int rid) {
    int count=1;lwm2m_data_t *d=lwm2m_data_new(1);d->id=rid;
    assert(halow_gps_control.readFunc(ctx,0,&count,&d,&halow_gps_control)==COAP_205_CONTENT);
    int64_t value=rid==6?d->value.asBoolean:d->value.asInteger;free(d);return value;
}
static uint8_t timeout(lwm2m_context_t *ctx,int64_t seconds) {
    lwm2m_data_t d={.id=1};lwm2m_data_encode_int(seconds,&d);
    return halow_gps_control.writeFunc(ctx,0,1,&d,&halow_gps_control,LWM2M_WRITE_REPLACE_RESOURCES);
}
static bool parse(const char *body,struct gps_fix *fix) {char line[256];sentence(line,sizeof line,body);return gps_nmea_fix(line,fix);}
int main(int argc, char **argv) {
    struct gps_fix fix;
    assert(parse(rmc,&fix));
    assert(fabs(fix.latitude-48.1173)<1e-8 && fabs(fix.longitude-11.5166666667)<1e-8);
    assert(fix.timestamp==764426119);
    assert(parse("GPRMC,000000,A,0000.000,N,00000.000,E,0,0,010100,,,A",&fix));
    assert(fix.latitude==0 && fix.longitude==0 && fix.timestamp==946684800);
    assert(parse("GPRMC,235959.50,A,9000.000,S,18000.000,W,0,0,290200,,,A",&fix));
    assert(fix.latitude==-90 && fix.longitude==-180);
    assert(!parse("GPRMC,000000,V,4807.038,N,01131.000,E,0,0,010100,,,A",&fix));
    assert(!parse("GPRMC,000000,A,4860.000,N,01131.000,E,0,0,010100,,,A",&fix));
    assert(!parse("GPRMC,000000,A,9000.001,N,01131.000,E,0,0,010100,,,A",&fix));
    assert(!parse("GPRMC,000000,A,4807.038,X,01131.000,E,0,0,010100,,,A",&fix));
    assert(!parse("GPRMC,000000,A,4807.038,N,18100.000,E,0,0,010100,,,A",&fix));
    assert(!parse("GPRMC,000000,A,4807.038,N,01131.000,E,0,0,290201,,,A",&fix));
    assert(!parse("GPRMC,240000,A,4807.038,N,01131.000,E,0,0,010100,,,A",&fix));
    assert(!parse("GPRMC,000000,A,4807.,N,01131.000,E,0,0,010100,,,A",&fix));
    assert(!gps_nmea_fix("$GPRMC*00junk",&fix));
    lwm2m_context_t ctx={0};
    assert(halow_gps_init()==0 && !powered);
    if (argc > 1) {
        /* A failed attempt must not consume the first successful fix hold. */
        assert(timeout(&ctx,10)==COAP_204_CHANGED);
        assert(execute(&ctx)==COAP_204_CHANGED); halow_gps_step(&ctx,true);
        now=10000; assert(halow_gps_step(&ctx,true)==-1 && !powered);
        assert(execute(&ctx)==COAP_204_CHANGED); halow_gps_step(&ctx,true);
        feed(rmc); assert(halow_gps_step(&ctx,true)==60000 && powered);
        if (!strcmp(argv[1], "hold-cancel")) {
            assert(halow_gps_step(NULL,false)==-1 && !powered);
            assert(control(&ctx,2)==2 && control(&ctx,3)==0 && control(&ctx,6)==1);
        } else {
            read_error=-EIO;
            assert(halow_gps_step(&ctx,true)==-1 && !powered && control(&ctx,2)==4);
            read_error=0;
        }
        assert(execute(&ctx)==COAP_204_CHANGED); halow_gps_step(&ctx,true);
        feed(rmc); assert(halow_gps_step(&ctx,true)==-1 && !powered);
        puts("Failed first acquisition retains hold; hold cancellation/error cleans up without repeating hold");
        return 0;
    }
    assert(control(&ctx,2)==0 && control(&ctx,4)==-1 && control(&ctx,6)==0 && starts==0);
    int count=0;lwm2m_data_t *data=NULL;
    assert(halow_gps_location.readFunc(&ctx,0,&count,&data,&halow_gps_location)==COAP_503_SERVICE_UNAVAILABLE);free(data);
    assert(timeout(&ctx,9)==COAP_400_BAD_REQUEST && timeout(&ctx,601)==COAP_400_BAD_REQUEST);
    assert(timeout(&ctx,10)==COAP_204_CHANGED);
    assert(execute(&ctx)==COAP_204_CHANGED && !powered);
    assert(execute(&ctx)==COAP_503_SERVICE_UNAVAILABLE && timeout(&ctx,20)==COAP_503_SERVICE_UNAVAILABLE);
    assert(halow_gps_step(&ctx,true)==10000 && powered && starts==1);
    feed("GPRMC,000000,V,4807.038,N,01131.000,E,0,0,010100,,,A");
    assert(halow_gps_step(&ctx,true)==10000 && powered);
    feed(rmc); input[input_size-4]^=1; /* Corrupt checksum; no false fix. */
    assert(halow_gps_step(&ctx,true)==10000 && powered);
    feed(rmc);assert(halow_gps_step(&ctx,true)==60000 && powered);
    assert(control(&ctx,2)==2 && control(&ctx,3)==0 && control(&ctx,5)==1 && control(&ctx,6)==1);
    assert(gps_acquisition_diagnostics.lines == 3 && gps_acquisition_diagnostics.rmc_lines == 3);
    assert(gps_acquisition_diagnostics.active_rmc_lines == 2 && gps_acquisition_diagnostics.rejected_rmc_lines == 2);
    assert(gps_acquisition_diagnostics.fixed_ms == 0 && gps_acquisition_diagnostics.finished_ms == UINT32_MAX);
    assert(gps_acquisition_diagnostics.state == 2 && gps_acquisition_diagnostics.result == 0);
    count=0;data=NULL;
    assert(halow_gps_location.readFunc(&ctx,0,&count,&data,&halow_gps_location)==COAP_205_CONTENT && count==3);
    assert(fabs(data[0].value.asFloat-48.1173)<1e-8 && data[2].value.asInteger==764426119);free(data);
    now=12000;assert(control(&ctx,4)==12 && starts==1); /* Reads never restart GPS. */
    feed(rmc); assert(halow_gps_step(&ctx,true)==48000 && powered);
    assert(control(&ctx,5)==1 && offset==input_size); /* Drain, without inventing acquisitions. */
    assert(execute(&ctx)==COAP_204_CHANGED); /* Reuse powered receiver; do not extend hold. */
    feed(rmc); assert(halow_gps_step(&ctx,true)==48000 && starts==1 && powered);
    assert(control(&ctx,5)==2 && control(&ctx,4)==0);
    now=59999; assert(halow_gps_step(&ctx,true)==1 && powered);
    now=60000; assert(halow_gps_step(&ctx,true)==-1 && !powered);
    assert(control(&ctx,2)==2 && gps_acquisition_diagnostics.finished_ms==60000);
    assert(execute(&ctx)==COAP_204_CHANGED); halow_gps_step(&ctx,true);
    feed(rmc); assert(halow_gps_step(&ctx,true)==-1 && !powered && starts==2);
    assert(control(&ctx,5)==3); /* Later fixes power down immediately. */
    assert(execute(&ctx)==COAP_204_CHANGED);assert(halow_gps_step(&ctx,true)==10000);
    now+=10000;assert(halow_gps_step(&ctx,true)==-1 && !powered);
    assert(control(&ctx,2)==3 && control(&ctx,3)==-ETIMEDOUT && control(&ctx,6)==1);
    assert(execute(&ctx)==COAP_204_CHANGED);halow_gps_step(&ctx,true);
    assert(halow_gps_step(&ctx,false)==-1 && !powered && control(&ctx,3)==-ECANCELED);
    assert(execute(&ctx)==COAP_204_CHANGED);start_error=-EIO;
    assert(halow_gps_step(&ctx,true)==-1 && !powered && control(&ctx,2)==4);
    start_error=0;assert(execute(&ctx)==COAP_204_CHANGED);read_error=-EIO;
    assert(halow_gps_step(&ctx,true)==-1 && !powered);read_error=0;
    assert(execute(&ctx)==COAP_204_CHANGED);halow_gps_step(&ctx,true);
    assert(halow_gps_step(NULL,false)==-1 && !powered); /* Context loss cancels. */
    assert(stops>=starts && control(&ctx,5)==3);
    puts("GPS checksum/coordinates/UTC, cached resources, deferred power, timeout and WNM/context cancellation passed");
}
