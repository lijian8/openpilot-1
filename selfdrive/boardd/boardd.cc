#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <assert.h>
#include <pthread.h>

#include <zmq.h>
#include <libusb.h>

#include <capnp/serialize.h>
#include "cereal/gen/cpp/log.capnp.h"

#include "common/timing.h"

int do_exit = 0;

libusb_context *ctx = NULL;
libusb_device_handle *dev_handle;
pthread_mutex_t usb_lock;

// double the FIFO size
#define RECV_SIZE (0x1000)
#define TIMEOUT 0

#define DEBUG_BOARDD
#ifdef DEBUG_BOARDD
#define DPRINTF(fmt, ...) printf("boardd: " fmt, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...)
#endif

bool usb_connect() {
  int err;

  dev_handle = libusb_open_device_with_vid_pid(ctx, 0xbbaa, 0xddcc);
  if (dev_handle == NULL) { return false; }

  err = libusb_set_configuration(dev_handle, 1);
  if (err != 0) { return false; }

  err = libusb_claim_interface(dev_handle, 0);
  if (err != 0) { return false; }

  return true;
}


void handle_usb_issue(int err, const char func[]) {
  DPRINTF("usb error %d \"%s\" in %s\n", err, libusb_strerror((enum libusb_error)err), func);
  if (err == -4) {
    while (!usb_connect()) { DPRINTF("attempting to connect\n"); usleep(100*1000); }
  }
  // TODO: check other errors, is simply retrying okay?
}

void can_recv(void *s) {
  int err;
  uint32_t data[RECV_SIZE/4];
  int recv;
  uint32_t f1, f2;

  // do recv
  pthread_mutex_lock(&usb_lock);
 
  do {
    err = libusb_bulk_transfer(dev_handle, 0x81, (uint8_t*)data, RECV_SIZE, &recv, TIMEOUT);
    if (err != 0) { handle_usb_issue(err, __func__); }
    if (err == -8) { DPRINTF("overflow got 0x%x\n", recv); };

    // timeout is okay to exit, recv still happened
    if (err == -7) { break; }
  } while(err != 0);

  pthread_mutex_unlock(&usb_lock);

  // return if length is 0
  if (recv <= 0) {
    return;
  }

  // create message
  capnp::MallocMessageBuilder msg;
  cereal::Event::Builder event = msg.initRoot<cereal::Event>();
  event.setLogMonoTime(nanos_since_boot());

  auto canData = event.initCan(recv/0x10);

  // populate message
  for (int i = 0; i<(recv/0x10); i++) {
    if (data[i*4] & 4) {
      // extended
      canData[i].setAddress(data[i*4] >> 3);
      //printf("got extended: %x\n", data[i*4] >> 3);
    } else {
      // normal
      canData[i].setAddress(data[i*4] >> 21);
    }
    canData[i].setBusTime(data[i*4+1] >> 16);
    int len = data[i*4+1]&0xF;
    canData[i].setDat(kj::arrayPtr((uint8_t*)&data[i*4+2], len));
    canData[i].setSrc((data[i*4+1] >> 4) & 3);
  }

  // send to can
  auto words = capnp::messageToFlatArray(msg);
  auto bytes = words.asBytes();
  zmq_send(s, bytes.begin(), bytes.size(), 0); 
}

void can_health(void *s) {
  int cnt;

  // copied from board/main.c
  struct health {
    uint32_t voltage;
    uint32_t current;
    uint8_t started;
    uint8_t controls_allowed;
    uint8_t gas_interceptor_detected;
  } health;

  // recv from board
  pthread_mutex_lock(&usb_lock);

  do {
    cnt = libusb_control_transfer(dev_handle, 0xc0, 0xd2, 0, 0, (unsigned char*)&health, sizeof(health), TIMEOUT);
    if (cnt != sizeof(health)) { handle_usb_issue(cnt, __func__); }
  } while(cnt != sizeof(health));

  pthread_mutex_unlock(&usb_lock);

  // create message
  capnp::MallocMessageBuilder msg;
  cereal::Event::Builder event = msg.initRoot<cereal::Event>();
  event.setLogMonoTime(nanos_since_boot());
  auto healthData = event.initHealth();

  // set fields
  healthData.setVoltage(health.voltage);
  healthData.setCurrent(health.current);
  healthData.setStarted(health.started);
  healthData.setControlsAllowed(health.controls_allowed);
  healthData.setGasInterceptorDetected(health.gas_interceptor_detected);

  // send to health
  auto words = capnp::messageToFlatArray(msg);
  auto bytes = words.asBytes();
  zmq_send(s, bytes.begin(), bytes.size(), 0); 
}


void can_send(void *s) {
  int err;

  // recv from sendcan
  zmq_msg_t msg;
  zmq_msg_init(&msg);
  err = zmq_msg_recv(&msg, s, 0);
  assert(err >= 0);

  // format for board
  auto amsg = kj::arrayPtr((const capnp::word*)zmq_msg_data(&msg), zmq_msg_size(&msg));
  capnp::FlatArrayMessageReader cmsg(amsg);
  cereal::Event::Reader event = cmsg.getRoot<cereal::Event>();
  int msg_count = event.getCan().size();

  uint32_t *send = (uint32_t*)malloc(msg_count*0x10);
  memset(send, 0, msg_count*0x10);

  for (int i = 0; i < msg_count; i++) {
    auto cmsg = event.getCan()[i];
    if (cmsg.getAddress() >= 0x800) {
      // extended
      send[i*4] = (cmsg.getAddress() << 3) | 5;
    } else {
      // normal
      send[i*4] = (cmsg.getAddress() << 21) | 1;
    }
    assert(cmsg.getDat().size() <= 8);
    send[i*4+1] = cmsg.getDat().size() | (cmsg.getSrc() << 4);
    memcpy(&send[i*4+2], cmsg.getDat().begin(), cmsg.getDat().size());
  }

  //DPRINTF("got send message: %d\n", msg_count);

  // release msg
  zmq_msg_close(&msg);

  // send to board
  int sent;
  pthread_mutex_lock(&usb_lock);

  do {
    err = libusb_bulk_transfer(dev_handle, 3, (uint8_t*)send, msg_count*0x10, &sent, TIMEOUT);
    if (err != 0 || msg_count*0x10 != sent) { handle_usb_issue(err, __func__); }
  } while(err != 0);

  pthread_mutex_unlock(&usb_lock);

  // done
  free(send);
}


// **** threads ****

void *can_send_thread(void *crap) {
  DPRINTF("start send thread\n");

  // sendcan = 8017
  void *context = zmq_ctx_new();
  void *subscriber = zmq_socket(context, ZMQ_SUB);
  zmq_setsockopt(subscriber, ZMQ_SUBSCRIBE, "", 0);
  zmq_connect(subscriber, "tcp://127.0.0.1:8017");

  // run as fast as messages come in
  while (!do_exit) {
    can_send(subscriber);
  }
  return NULL;
}

void *can_recv_thread(void *crap) {
  DPRINTF("start recv thread\n");

  // can = 8006
  void *context = zmq_ctx_new();
  void *publisher = zmq_socket(context, ZMQ_PUB);
  zmq_bind(publisher, "tcp://*:8006");

  // run at ~200hz
  while (!do_exit) {
    can_recv(publisher);
    // 5ms
    usleep(5*1000);
  }
  return NULL;
}

void *can_health_thread(void *crap) {
  DPRINTF("start health thread\n");

  // health = 8011
  void *context = zmq_ctx_new();
  void *publisher = zmq_socket(context, ZMQ_PUB);
  zmq_bind(publisher, "tcp://*:8011");

  // run at 1hz
  while (!do_exit) {
    can_health(publisher);
    usleep(1000*1000);
  }
  return NULL;
}

int main() {
  int err;
  printf("boardd: starting boardd\n");

  // set process priority
  err = setpriority(PRIO_PROCESS, 0, -4);
  printf("boardd: setpriority returns %d\n", err);

  // connect to the board
  err = libusb_init(&ctx);
  assert(err == 0);
  libusb_set_debug(ctx, 3);

  // TODO: duplicate code from error handling
  while (!usb_connect()) { DPRINTF("attempting to connect\n"); usleep(100*1000); }

  /*int config;
  err = libusb_get_configuration(dev_handle, &config);
  assert(err == 0);
  DPRINTF("configuration is %d\n", config);*/

  /*err = libusb_set_interface_alt_setting(dev_handle, 0, 0);
  assert(err == 0);*/

  // create threads

  pthread_t can_health_thread_handle;
  err = pthread_create(&can_health_thread_handle, NULL,
                       can_health_thread, NULL);
  assert(err == 0);

  pthread_t can_send_thread_handle;
  err = pthread_create(&can_send_thread_handle, NULL,
                       can_send_thread, NULL);
  assert(err == 0);

  pthread_t can_recv_thread_handle;
  err = pthread_create(&can_recv_thread_handle, NULL,
                       can_recv_thread, NULL);
  assert(err == 0);

  // join threads

  err = pthread_join(can_recv_thread_handle, NULL);
  assert(err == 0);

  err = pthread_join(can_send_thread_handle, NULL);
  assert(err == 0);

  err = pthread_join(can_health_thread_handle, NULL);
  assert(err == 0);

  // destruct libusb

  libusb_close(dev_handle);
  libusb_exit(ctx);
}

