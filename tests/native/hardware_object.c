#include "halow_hardware.h"
#include "hardware_config.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static int mode, sets, clears, failure;
const char *ez_prov_device_name(void) {return "Test collar";}
enum hardware_mode hardware_mode_get(void) {return HARDWARE_BASE;}
int hardware_config_desired(void) {return mode;}
int hardware_config_factory(void) {return -1;}
int hardware_config_source(void) {return 2;}
int hardware_config_error(void) {return failure;}
bool hardware_config_pending(void) {return mode!=0;}
int hardware_config_set(int value) {++sets;if(failure)return failure;mode=value;return 0;}
int hardware_config_clear(void) {++clears;if(failure)return failure;mode=0;return 0;}
lwm2m_data_t *lwm2m_data_new(int n) {return calloc(n,sizeof(lwm2m_data_t));}
void lwm2m_data_encode_int(int64_t value,lwm2m_data_t *d) {d->type=LWM2M_TYPE_INTEGER;d->value.asInteger=value;}
void lwm2m_data_encode_bool(bool value,lwm2m_data_t *d) {d->type=LWM2M_TYPE_BOOLEAN;d->value.asBoolean=value;}
void lwm2m_data_encode_string(const char *value,lwm2m_data_t *d) {
 d->type=LWM2M_TYPE_STRING;d->value.asBuffer.length=strlen(value);
 d->value.asBuffer.buffer=malloc(strlen(value)+1);strcpy((char *)d->value.asBuffer.buffer,value);
}
int lwm2m_data_decode_int(const lwm2m_data_t *d,int64_t *v) {if(d->type!=LWM2M_TYPE_INTEGER)return 0;*v=d->value.asInteger;return 1;}
int main(void) {
 lwm2m_object_t *o=&halow_hardware;
 lwm2m_data_t d={.id=0,.type=LWM2M_TYPE_INTEGER,.value.asInteger=3};
 assert(o->writeFunc(NULL,0,1,&d,o,LWM2M_WRITE_REPLACE_RESOURCES)==COAP_204_CHANGED && mode==3);
 d.value.asInteger=4;assert(o->writeFunc(NULL,0,1,&d,o,LWM2M_WRITE_REPLACE_RESOURCES)==COAP_400_BAD_REQUEST && sets==1);
 d.id=1;assert(o->writeFunc(NULL,0,1,&d,o,LWM2M_WRITE_REPLACE_RESOURCES)==COAP_405_METHOD_NOT_ALLOWED);
 d.id=0;d.value.asInteger=1;failure=-5;
 assert(o->writeFunc(NULL,0,1,&d,o,LWM2M_WRITE_REPLACE_RESOURCES)==COAP_500_INTERNAL_SERVER_ERROR);
 assert(o->executeFunc(NULL,0,5,NULL,0,o)==COAP_500_INTERNAL_SERVER_ERROR);
 failure=0;assert(o->executeFunc(NULL,0,5,NULL,0,o)==COAP_204_CHANGED && mode==0 && clears==2);
 assert(o->executeFunc(NULL,0,5,NULL,1,o)==COAP_400_BAD_REQUEST);
 assert(o->executeFunc(NULL,1,5,NULL,0,o)==COAP_404_NOT_FOUND);
 int count=0;lwm2m_data_t *data=NULL;
 assert(o->readFunc(NULL,0,&count,&data,o)==COAP_205_CONTENT && count==7);
 assert(data[2].value.asInteger==-1 && data[4].type==LWM2M_TYPE_BOOLEAN);
 assert(data[6].id==7 && data[6].type==LWM2M_TYPE_STRING);
 assert(data[6].value.asBuffer.length==strlen("Test collar") && !memcmp(data[6].value.asBuffer.buffer,"Test collar",strlen("Test collar")));
 free(data[6].value.asBuffer.buffer);free(data);
 d.id=7;assert(o->writeFunc(NULL,0,1,&d,o,LWM2M_WRITE_REPLACE_RESOURCES)==COAP_405_METHOD_NOT_ALLOWED);
 count=0;data=NULL;assert(o->discoverFunc(NULL,0,&count,&data,o)==COAP_205_CONTENT && count==8);free(data);
 puts("Hardware LwM2M reads, writes, validation, errors and clear override passed");
 return 0;
}
