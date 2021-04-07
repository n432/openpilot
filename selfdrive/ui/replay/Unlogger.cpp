#include <string>
#include <vector>
#include <capnp/dynamic.h>
#include <capnp/schema.h>

// include the dynamic struct
#include "cereal/gen/cpp/log.capnp.c++"
#include "cereal/gen/cpp/car.capnp.c++"
#include "cereal/gen/cpp/legacy.capnp.c++"
#include "cereal/services.h"

#include "Unlogger.hpp"

#include <stdint.h>
#include <time.h>

#include "common/timing.h"

Unlogger::Unlogger(Events *events_, QReadWriteLock* events_lock_, QMap<int, FrameReader*> *frs_, int seek)
  : events(events_), events_lock(events_lock_), frs(frs_) {
  ctx = Context::create();

  seek_request = seek*1e9;

  QStringList block = QString(getenv("BLOCK")).split(",");
  qDebug() << "blocklist" << block;

  QStringList allow = QString(getenv("ALLOW")).split(",");
  qDebug() << "allowlist" << allow;

  for (const auto& it : services) {
    std::string name = it.name;
    if (allow[0].size() > 0 && !allow.contains(name.c_str())) {
      qDebug() << "not allowing" << name.c_str();
      continue;
    }

    if (block.contains(name.c_str())) {
      qDebug() << "blocking" << name.c_str();
      continue;
    }

    PubSocket *sock = PubSocket::create(ctx, name);
    if (sock == NULL) {
      qDebug() << "FAILED" << name.c_str();
      continue;
    }

    qDebug() << name.c_str();

    for (auto field: capnp::Schema::from<cereal::Event>().getFields()) {
      std::string tname = field.getProto().getName();

      if (tname == name) {
        // TODO: I couldn't figure out how to get the which, only the index, hence this hack
        int type = field.getIndex();
        if (type > 67) type--; // valid
        type--; // logMonoTime

        //qDebug() << "here" << tname.c_str() << type << cereal::Event::CONTROLS_STATE;
        socks.insert(type, sock);
      }
    }
  }

  cl_device_id device_id = cl_get_device_id(CL_DEVICE_TYPE_DEFAULT);
  cl_context context = CL_CHECK_ERR(clCreateContext(NULL, 1, &device_id, NULL, NULL, &err));

  vipc_server = new VisionIpcServer("camerad", device_id, context);
  vipc_server->create_buffers(VisionStreamType::VISION_STREAM_RGB_BACK, 4, true, 1164, 874);
}

void Unlogger::process() {

	vipc_server->start_listener();

  qDebug() << "hello from unlogger thread";
  while (events->size() == 0) {
    qDebug() << "waiting for events";
    QThread::sleep(1);
  }
  qDebug() << "got events";

  // TODO: hack
  if (seek_request != 0) {
    seek_request += events->begin().key();
    while (events->lowerBound(seek_request) == events->end()) {
      qDebug() << "waiting for desired time";
      QThread::sleep(1);
    }
  }

  QElapsedTimer timer;
  timer.start();

  uint64_t last_elapsed = 0;

  // loops
  while (1) {
    uint64_t t0 = (events->begin()+1).key();
    uint64_t t0r = timer.nsecsElapsed();
    qDebug() << "unlogging at" << t0;

    auto eit = events->lowerBound(t0);
    while (eit != events->end()) {
      while (paused) {
        QThread::usleep(1000);
        t0 = eit->getLogMonoTime();
        t0r = timer.nsecsElapsed();
      }

      if (seek_request != 0) {
        t0 = seek_request;
        qDebug() << "seeking to" << t0;
        t0r = timer.nsecsElapsed();
        eit = events->lowerBound(t0);
        seek_request = 0;
        if (eit == events->end()) {
          qWarning() << "seek off end";
          break;
        }
      }

      if (abs(((long long)tc-(long long)last_elapsed)) > 50e6) {
        //qDebug() << "elapsed";
        emit elapsed();
        last_elapsed = tc;
      }

      auto e = *eit;
      auto type = e.which();
      uint64_t tm = e.getLogMonoTime();
      auto it = socks.find(type);
      tc = tm;
      if (it != socks.end()) {
        long etime = tm-t0;
        long rtime = timer.nsecsElapsed() - t0r;
        long us_behind = ((etime-rtime)*1e-3)+0.5;
        if (us_behind > 0) {
          if (us_behind > 1e6) {
            qWarning() << "OVER ONE SECOND BEHIND, HACKING" << us_behind;
            us_behind = 0;
            t0 = tm;
            t0r = timer.nsecsElapsed();
          }
          QThread::usleep(us_behind);
          //qDebug() << "sleeping" << us_behind << etime << timer.nsecsElapsed();
        }

        capnp::MallocMessageBuilder msg;
        msg.setRoot(e);

        auto ee = msg.getRoot<cereal::Event>();
        ee.setLogMonoTime(nanos_since_boot());

        if (e.which() == cereal::Event::ROAD_CAMERA_STATE) {
          auto fr = msg.getRoot<cereal::Event>().getRoadCameraState();

          // TODO: better way?
          auto it = eidx.find(fr.getFrameId());
          if (it != eidx.end()) {
            auto pp = *it;
            //qDebug() << fr.getRoadCameraStateId() << pp;

            if (frs->find(pp.first) != frs->end()) {
              auto frm = (*frs)[pp.first];
              auto data = frm->get(pp.second);

							VisionBuf *buf = vipc_server->get_buffer(VisionStreamType::VISION_STREAM_RGB_BACK);
							memcpy(buf->addr, data, frm->getRGBSize());
							VisionIpcBufExtra extra = {};

							vipc_server->send(buf, &extra, false);
            }
          }
        }

        auto words = capnp::messageToFlatArray(msg);
        auto bytes = words.asBytes();

        // TODO: Can PubSocket take a const char?
        (*it)->send((char*)bytes.begin(), bytes.size());
      }
      ++eit;
    }
  }
}

