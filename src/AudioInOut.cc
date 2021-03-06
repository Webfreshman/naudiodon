/* Copyright 2017 Streampunk Media Ltd.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include <nan.h>
#include "AudioInOut.h"
#include "Persist.h"
#include "Params.h"
#include "ChunkQueue.h"
#include "AudioChunk.h"
#include <mutex>
#include <condition_variable>
#include <map>
#include <portaudio.h>

using namespace v8;

namespace streampunk {

static std::map<char*, std::shared_ptr<Memory> > outstandingAllocs;
static void freeAllocCb(char* data, void* hint) {
  std::map<char*, std::shared_ptr<Memory> >::iterator it = outstandingAllocs.find(data);
  if (it != outstandingAllocs.end())
    outstandingAllocs.erase(it);
}

class InOutContext {
public:
  InOutContext(std::shared_ptr<AudioOptions> inAudioOptions,
                std::shared_ptr<AudioOptions> outAudioOptions, 
                PaStreamCallback *cb)
    : mActive(false)
    , mInAudioOptions(inAudioOptions)
    , mOutAudioOptions(outAudioOptions)
    , mInChunkQueue(inAudioOptions ? inAudioOptions->maxQueue() : 0)
    , mOutChunkQueue(outAudioOptions ? outAudioOptions->maxQueue() : 0)
    , mOutCurOffset(0)
    , mOutFinished(false)
    , mStreamCb(cb) {

    PaError errCode = Pa_Initialize();
    if (errCode != paNoError) {
      std::string err = std::string("Could not initialize PortAudio: ") + Pa_GetErrorText(errCode);
      Nan::ThrowError(err.c_str());
      return;
    }

    if ( !mInAudioOptions && !mOutAudioOptions ) {
      Nan::ThrowError("Input and/or output options must be specified");
    }
  }

  ~InOutContext() {
    stop();
    Pa_Terminate();
  }

  void start() {
    if ( !isActive() ) {
      int32_t deviceID;
      uint32_t sampleFormat;
      double sampleRate;
      uint32_t framesPerBuffer;
      PaError errCode(paNoError);

      PaStreamParameters inParams;
      PaStreamParameters outParams;
      memset(&inParams, 0, sizeof(PaStreamParameters));
      memset(&outParams, 0, sizeof(PaStreamParameters));

      if ( mInAudioOptions ) {
        //printf("Input %s\n", mInAudioOptions->toString().c_str());
        deviceID = (int32_t)mInAudioOptions->deviceID();
        if ((deviceID >= 0) && (deviceID < Pa_GetDeviceCount()))
          inParams.device = (PaDeviceIndex)deviceID;
        else
          inParams.device = Pa_GetDefaultInputDevice();
        if (inParams.device == paNoDevice) {
          Nan::ThrowError("No default input device");
          return;
        }

        inParams.channelCount = mInAudioOptions->channelCount();
        if (inParams.channelCount > Pa_GetDeviceInfo(inParams.device)->maxInputChannels) {
          Nan::ThrowError("Channel count exceeds maximum number of input channels for device");
          return;
        }

        sampleFormat = mInAudioOptions->sampleFormat();
        switch(sampleFormat) {
        case 8: inParams.sampleFormat = paInt8; break;
        case 16: inParams.sampleFormat = paInt16; break;
        case 24: inParams.sampleFormat = paInt24; break;
        case 32: inParams.sampleFormat = paInt32; break;
        default:
          Nan::ThrowError("Invalid sampleFormat");
          return;
        }

        inParams.suggestedLatency = Pa_GetDeviceInfo(inParams.device)->defaultLowInputLatency;
        inParams.hostApiSpecificStreamInfo = NULL;

        sampleRate = (double)mInAudioOptions->sampleRate();
        framesPerBuffer = paFramesPerBufferUnspecified;

        #ifdef __arm__
        framesPerBuffer = 256;
        inParams.suggestedLatency = Pa_GetDeviceInfo(inParams.device)->defaultHighInputLatency;
        #endif
      }

      if ( mOutAudioOptions ) {
        deviceID = (int32_t)mOutAudioOptions->deviceID();
        if ((deviceID >= 0) && (deviceID < Pa_GetDeviceCount()))
          outParams.device = (PaDeviceIndex)deviceID;
        else
          outParams.device = Pa_GetDefaultOutputDevice();
        if (outParams.device == paNoDevice) {
          Nan::ThrowError("No default output device");
          return;
        }
        //printf("Output device name is %s\n", Pa_GetDeviceInfo(outParams.device)->name);

        outParams.channelCount = mOutAudioOptions->channelCount();
        if (outParams.channelCount > Pa_GetDeviceInfo(outParams.device)->maxOutputChannels) {
          Nan::ThrowError("Channel count exceeds maximum number of output channels for device");
          return;
        }

        sampleFormat = mOutAudioOptions->sampleFormat();
        switch(sampleFormat) {
        case 8: outParams.sampleFormat = paInt8; break;
        case 16: outParams.sampleFormat = paInt16; break;
        case 24: outParams.sampleFormat = paInt24; break;
        case 32: outParams.sampleFormat = paInt32; break;
        default:
          Nan::ThrowError("Invalid sampleFormat");
          return;
        }

        outParams.suggestedLatency = Pa_GetDeviceInfo(outParams.device)->defaultLowOutputLatency;
        outParams.hostApiSpecificStreamInfo = NULL;

        sampleRate = (double)mOutAudioOptions->sampleRate();
        framesPerBuffer = paFramesPerBufferUnspecified;

        #ifdef __arm__
        framesPerBuffer = 256;
        outParams.suggestedLatency = Pa_GetDeviceInfo(outParams.device)->defaultHighOutputLatency;
        #endif
      }

      if ( mInAudioOptions && mOutAudioOptions ) {
        if ( mInAudioOptions->sampleRate() != mOutAudioOptions->sampleRate() ) {
          Nan::ThrowError("Input/output sample rates must match");
          return;
        }
      }
    
      errCode = Pa_OpenStream(&mStream,
                              mInAudioOptions ? &inParams : NULL, 
                              mOutAudioOptions ? &outParams : NULL,
                              sampleRate,
                              framesPerBuffer,
                              paNoFlag,
                              mStreamCb,
                              this);
      if (errCode != paNoError) {
        std::string err = std::string("Could not open stream: ") + Pa_GetErrorText(errCode);
        Nan::ThrowError(err.c_str());
        return;
      }

      errCode = Pa_StartStream(mStream);
      if ( errCode != paNoError ) {
        Pa_CloseStream(&mStream);
        std::string err = std::string("Could not start input/output stream: ") + Pa_GetErrorText(errCode);
        Nan::ThrowError(err.c_str());
        return;
      }

      // Mark the stream as now active
      setActive(true);
    }
  }

  void stop() {
    if ( isActive() ) {
      PaError errCode = Pa_StopStream(mStream);
      if ( errCode == paNoError ) {
        Pa_CloseStream(mStream);
        mStream = NULL;
        setActive(false);
      }
    }
  }

  std::shared_ptr<Memory> readChunk() {
    return mInChunkQueue.dequeue();
  }

  bool readBuffer(const void *srcBuf, uint32_t frameCount) {
    if ( !mInAudioOptions ) {
      Nan::ThrowError("Input configuration was not defined");
      return false;
    }
    bool active = isActive();
    if ( active ) {
      const uint8_t *src = (uint8_t *)srcBuf;
      uint32_t bytesAvailable = frameCount * mInAudioOptions->channelCount() * mInAudioOptions->sampleFormat() / 8;
      std::shared_ptr<Memory> dstBuf = Memory::makeNew(bytesAvailable);
      memcpy(dstBuf->buf(), src, bytesAvailable);
      mInChunkQueue.enqueue(dstBuf);
    }
    return active;
  }

  void addChunk(std::shared_ptr<AudioChunk> audioChunk) {
    mOutChunkQueue.enqueue(audioChunk);
  }

  bool fillBuffer(void *buf, uint32_t frameCount) {
    if ( !mOutAudioOptions ) {
      Nan::ThrowError("Output configuration was not defined");
      return false;
    }

    uint8_t *dst = (uint8_t *)buf;
    uint32_t bytesRemaining = frameCount * mOutAudioOptions->channelCount() * mOutAudioOptions->sampleFormat() / 8;

    uint32_t active = isActive();
    if (!active && (0 == mOutChunkQueue.size()) && 
        (!mOutCurChunk || (mOutCurChunk && (bytesRemaining >= mOutCurChunk->chunk()->numBytes() - mOutCurOffset)))) {
      if (mOutCurChunk) {
        uint32_t bytesCopied = doCopy(mOutCurChunk->chunk(), dst, bytesRemaining);
        uint32_t missingBytes = bytesRemaining - bytesCopied;
        if (missingBytes > 0) {
          //printf("Finishing - %d bytes not available for the last output buffer\n", missingBytes);
          memset(dst + bytesCopied, 0, missingBytes);
        }
      }
      std::lock_guard<std::mutex> lk(m);
      mOutFinished = true;
      cv.notify_one();
    } else {
      while (bytesRemaining) {
        if (!(mOutCurChunk && (mOutCurOffset < mOutCurChunk->chunk()->numBytes()))) {
          mOutCurChunk = mOutChunkQueue.dequeue();
          mOutCurOffset = 0;
        }
        if (mOutCurChunk) {
          uint32_t bytesCopied = doCopy(mOutCurChunk->chunk(), dst, bytesRemaining);
          bytesRemaining -= bytesCopied;
          dst += bytesCopied;
          mOutCurOffset += bytesCopied;
        } else { // Deal with termination case of ChunkQueue being kicked and returning null chunk
          std::lock_guard<std::mutex> lk(m);
          mOutFinished = true;
          cv.notify_one();
          break;
        }
      }
    }

    return !mOutFinished;
  }

  void checkStatus(uint32_t statusFlags) {
    if (statusFlags) {
      std::string err = std::string("portAudio status - ");
      if (statusFlags & paInputUnderflow)
        err += "input underflow ";
      if (statusFlags & paInputOverflow)
        err += "input overflow ";
      if (statusFlags & paOutputUnderflow)
        err += "output underflow ";
      if (statusFlags & paOutputOverflow)
        err += "output overflow ";
      if (statusFlags & paPrimingOutput)
        err += "priming output ";
      std::lock_guard<std::mutex> lk(m);
      mErrStr = err;
    }
  }

  bool getErrStr(std::string& errStr) {
    std::lock_guard<std::mutex> lk(m);
    errStr = mErrStr;
    mErrStr = std::string();
    return errStr != std::string();
  }

  void quit() {
    std::unique_lock<std::mutex> lk(m);
    mActive = false;
    if ( isInput() ) {
      mInChunkQueue.quit();
    }

    if ( isOutput() && !mOutFinished ) {
      mOutChunkQueue.quit();
      while(!mOutFinished)
        cv.wait(lk);  
    }
  }

  bool isInput() {
    return mInAudioOptions ? true : false;
  }

  bool isOutput() {
    return mOutAudioOptions ? true : false;
  }

private:
  bool mActive;
  std::shared_ptr<AudioOptions> mInAudioOptions;
  std::shared_ptr<AudioOptions> mOutAudioOptions;
  ChunkQueue<std::shared_ptr<Memory> > mInChunkQueue;
  ChunkQueue<std::shared_ptr<AudioChunk> > mOutChunkQueue;
  std::shared_ptr<AudioChunk> mOutCurChunk;
  uint32_t mOutCurOffset;
  bool mOutFinished;
  PaStream* mStream;
  std::string mErrStr;
  mutable std::mutex m;
  std::condition_variable cv;
  PaStreamCallback * mStreamCb;

  bool isActive() const {
    std::unique_lock<std::mutex> lk(m);
    return mActive;
  }

  void setActive(bool option) {
    std::unique_lock<std::mutex> lk(m);
    mActive = option;
    return;
  }

  uint32_t doCopy(std::shared_ptr<Memory> chunk, void *dst, uint32_t numBytes) {
    uint32_t curChunkBytes = chunk->numBytes() - mOutCurOffset;
    uint32_t thisChunkBytes = std::min<uint32_t>(curChunkBytes, numBytes);
    memcpy(dst, chunk->buf() + mOutCurOffset, thisChunkBytes);
    return thisChunkBytes;
  }

};

int InOutCallback(const void *input, void *output, unsigned long frameCount,
               const PaStreamCallbackTimeInfo *timeInfo,
               PaStreamCallbackFlags statusFlags, void *userData) {
  int outputRetCode(paComplete);
  int inputRetCode(paComplete);
  InOutContext *context = (InOutContext *)userData;
  context->checkStatus(statusFlags);
  if ( context->isInput() ) {
    inputRetCode = context->readBuffer(input, frameCount) ? paContinue : paComplete;
  }

  if ( context->isOutput() ) {
    outputRetCode = context->fillBuffer(output, frameCount) ? paContinue : paComplete;
  }

  int finalRetCode = ((inputRetCode == paComplete) && (outputRetCode == paComplete)) ? paComplete : paContinue;

  return finalRetCode;
}

class InputIoWorker : public Nan::AsyncWorker {
  public:
    InputIoWorker(std::shared_ptr<InOutContext> InOutContext, Nan::Callback *callback)
      : AsyncWorker(callback), mInOutContext(InOutContext)
    { }
    ~InputIoWorker() {}

    void Execute() {
      mInChunk = mInOutContext->readChunk();
    }

    void HandleOKCallback () {
      Nan::HandleScope scope;

      std::string errStr;
      if (mInOutContext->getErrStr(errStr)) {
        Local<Value> argv[] = { Nan::Error(errStr.c_str()) };
        callback->Call(1, argv, async_resource);
      }

      if (mInChunk) {
        outstandingAllocs.insert(make_pair((char*)mInChunk->buf(), mInChunk));
        Nan::MaybeLocal<Object> maybeBuf = Nan::NewBuffer((char*)mInChunk->buf(), mInChunk->numBytes(), freeAllocCb, 0);
        Local<Value> argv[] = { Nan::Null(), maybeBuf.ToLocalChecked() };
        callback->Call(2, argv, async_resource);
      } else {
        Local<Value> argv[] = { Nan::Null(), Nan::Null() };
        callback->Call(2, argv, async_resource);
      }
    }

  private:
    std::shared_ptr<InOutContext> mInOutContext;
    std::shared_ptr<Memory> mInChunk;
};

class OutputIoWorker : public Nan::AsyncWorker {
  public:
    OutputIoWorker(std::shared_ptr<InOutContext> InOutContext, Nan::Callback *callback, std::shared_ptr<AudioChunk> audioChunk)
      : AsyncWorker(callback), mInOutContext(InOutContext), mAudioChunk(audioChunk) 
    { }
    ~OutputIoWorker() {}

    void Execute() {
      mInOutContext->addChunk(mAudioChunk);
    }

    void HandleOKCallback () {
      Nan::HandleScope scope;
      std::string errStr;
      if (mInOutContext->getErrStr(errStr)) {
        Local<Value> argv[] = { Nan::Error(errStr.c_str()) };
        callback->Call(1, argv, async_resource);
      } else {
        callback->Call(0, NULL, async_resource);
      }
    }

  private:
    std::shared_ptr<InOutContext> mInOutContext;
    std::shared_ptr<AudioChunk> mAudioChunk;
};

class QuitIoWorker : public Nan::AsyncWorker {
  public:
    QuitIoWorker(std::shared_ptr<InOutContext> InOutContext, Nan::Callback *callback)
      : AsyncWorker(callback), mInOutContext(InOutContext)
    { }
    ~QuitIoWorker() {}

    void Execute() {
      mInOutContext->quit();
    }

    void HandleOKCallback () {
      Nan::HandleScope scope;
      mInOutContext->stop();
      callback->Call(0, NULL, async_resource);
    }

  private:
    std::shared_ptr<InOutContext> mInOutContext;
};

AudioInOut::AudioInOut(Local<Object> options) {
  std::shared_ptr<AudioOptions> inOpts;
  std::shared_ptr<AudioOptions> outOpts;

  Local<String> inProp = Nan::New("inOptions").ToLocalChecked();
  Local<String> outProp = Nan::New("outOptions").ToLocalChecked();
  Nan::MaybeLocal<Value> inValue = Nan::Get(options, inProp);
  Nan::MaybeLocal<Value> outValue = Nan::Get(options, outProp);

  if ( !inValue.IsEmpty() && inValue.ToLocalChecked()->IsObject() ) {
    inOpts = std::make_shared<AudioOptions>(inValue.ToLocalChecked()->ToObject());
  }

  if ( !outValue.IsEmpty() && outValue.ToLocalChecked()->IsObject() ) {
    outOpts = std::make_shared<AudioOptions>(outValue.ToLocalChecked()->ToObject());
  }
  mInOutContext = std::make_shared<InOutContext>(inOpts, outOpts, InOutCallback);
}
AudioInOut::~AudioInOut() {}

void AudioInOut::doStart() { mInOutContext->start(); }

NAN_METHOD(AudioInOut::Start) {
  AudioInOut* obj = Nan::ObjectWrap::Unwrap<AudioInOut>(info.Holder());
  obj->doStart();
  info.GetReturnValue().SetUndefined();
}

NAN_METHOD(AudioInOut::Read) {
  if (info.Length() != 2)
    return Nan::ThrowError("AudioInOut Read expects 2 arguments");
  if (!info[0]->IsNumber())
    return Nan::ThrowError("AudioInOut Read requires a valid advisory size as the first parameter");
  if (!info[1]->IsFunction())
    return Nan::ThrowError("AudioInOut Read requires a valid callback as the second parameter");

  // uint32_t sizeAdv = Nan::To<uint32_t>(info[0]).FromJust();
  Local<Function> callback = Local<Function>::Cast(info[1]);
  AudioInOut* obj = Nan::ObjectWrap::Unwrap<AudioInOut>(info.Holder());

  if ( !obj->getContext()->isInput() ) {
    return Nan::ThrowError("AudioInOut is not configured for input");
  }

  AsyncQueueWorker(new InputIoWorker(obj->getContext(), new Nan::Callback(callback)));
  info.GetReturnValue().SetUndefined();
}

NAN_METHOD(AudioInOut::Write) {
  if (info.Length() != 2)
    return Nan::ThrowError("AudioInOut Write expects 2 arguments");
  if (!info[0]->IsObject())
    return Nan::ThrowError("AudioInOut Write requires a valid chunk buffer as the first parameter");
  if (!info[1]->IsFunction())
    return Nan::ThrowError("AudioInOut Write requires a valid callback as the second parameter");

  Local<Object> chunkObj = Local<Object>::Cast(info[0]);
  Local<Function> callback = Local<Function>::Cast(info[1]);
  AudioInOut* obj = Nan::ObjectWrap::Unwrap<AudioInOut>(info.Holder());

  if ( !obj->getContext()->isOutput() ) {
    return Nan::ThrowError("AudioInOut is not configured for output");
  }

  AsyncQueueWorker(new OutputIoWorker(obj->getContext(), new Nan::Callback(callback), std::make_shared<AudioChunk>(chunkObj)));
  info.GetReturnValue().SetUndefined();
}

NAN_METHOD(AudioInOut::Quit) {
  if (info.Length() != 1)
    return Nan::ThrowError("AudioInOut Quit expects 1 argument");
  if (!info[0]->IsFunction())
    return Nan::ThrowError("AudioInOut Quit requires a valid callback as the parameter");

  Local<Function> callback = Local<Function>::Cast(info[0]);
  AudioInOut* obj = Nan::ObjectWrap::Unwrap<AudioInOut>(info.Holder());

  AsyncQueueWorker(new QuitIoWorker(obj->getContext(), new Nan::Callback(callback)));
  info.GetReturnValue().SetUndefined();
}

NAN_MODULE_INIT(AudioInOut::Init) {
  Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>(New);
  tpl->SetClassName(Nan::New("AudioInOut").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  SetPrototypeMethod(tpl, "start", Start);
  SetPrototypeMethod(tpl, "read", Read);
  SetPrototypeMethod(tpl, "write", Write);
  SetPrototypeMethod(tpl, "quit", Quit);

  constructor().Reset(Nan::GetFunction(tpl).ToLocalChecked());
  Nan::Set(target, Nan::New("AudioInOut").ToLocalChecked(),
    Nan::GetFunction(tpl).ToLocalChecked());
}

} // namespace streampunk
