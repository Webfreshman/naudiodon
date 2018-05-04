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
#include "AudioAsio.h"
#include "Persist.h"
#include "Params.h"
#include "ChunkQueue.h"
#include "AudioChunk.h"
#include <mutex>
#include <condition_variable>
#include <map>
#include <portaudio.h>
#include <pa_asio.h>

using namespace v8;

namespace streampunk {

static std::map<char*, std::shared_ptr<Memory> > outstandingAllocs;
static void freeAllocCb(char* data, void* hint) {
  std::map<char*, std::shared_ptr<Memory> >::iterator it = outstandingAllocs.find(data);
  if (it != outstandingAllocs.end())
    outstandingAllocs.erase(it);
}

class AsioContext {
public:
  typedef enum {
    INPUT,
    OUTPUT
  } AsioDirection;

  typedef std::vector<std::string> ChannelList;

  AsioContext(std::shared_ptr<AudioOptions> inAudioOptions,
                std::shared_ptr<AudioOptions> outAudioOptions, 
                PaStreamCallback *cb)
    : mActive(false)
    , mInAudioOptions(inAudioOptions)
    , mOutAudioOptions(outAudioOptions)
    , mInChunkQueue(mInAudioOptions ? mInAudioOptions->maxQueue() : 0)
    , mOutChunkQueue(mOutAudioOptions ? mOutAudioOptions->maxQueue() : 0)
    , mOutCurOffset(0)
    , mOutFinished(false)
    , mStream(NULL)
    , mSampleRate(-1.0)
    , mStreamCb(cb) {

    PaError errCode = Pa_Initialize();
    if (errCode != paNoError) {
      std::string err = std::string("Could not initialize PortAudio: ") + Pa_GetErrorText(errCode);
      Nan::ThrowError(err.c_str());
      return;
    }

    if ( !mInAudioOptions && !mOutAudioOptions ) {
      Nan::ThrowError("Input and/or output options must be specified");
      return;
    }
  }

  ~AsioContext() {
    stop();
    Pa_Terminate();
  }

  void start() {
    // Start stream if not already actively streaming
    if ( !isActive() ) {
      int32_t deviceID;
      uint32_t sampleFormat;
      double sampleRate;
      uint32_t framesPerBuffer;
      const PaDeviceInfo* deviceInfo;
      PaAsioStreamInfo outAsioStreamInfo;
      PaAsioStreamInfo inAsioStreamInfo;
      PaError errCode(paNoError);
      PaStreamParameters inParams;
      PaStreamParameters outParams;

      if ( mInAudioOptions ) {
        memset(&inParams, 0, sizeof(PaStreamParameters));
        memset(&inAsioStreamInfo, 0, sizeof(inAsioStreamInfo));
        inAsioStreamInfo.size = sizeof(inAsioStreamInfo);
        inAsioStreamInfo.hostApiType = paASIO;
        inAsioStreamInfo.version = 1;
        inAsioStreamInfo.flags = paAsioUseChannelSelectors;
        inAsioStreamInfo.channelSelectors = mInAudioOptions->channelSelectors().data();

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
        deviceInfo = Pa_GetDeviceInfo(inParams.device);
        if ( paASIO != Pa_GetHostApiInfo(deviceInfo->hostApi)->type ) {
          Nan::ThrowError("Not an ASIO device");
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
        inParams.hostApiSpecificStreamInfo = &inAsioStreamInfo;

        sampleRate = ( mSampleRate < 0.0) ? (double)mInAudioOptions->sampleRate() : mSampleRate;
        framesPerBuffer = paFramesPerBufferUnspecified;

        #ifdef __arm__
        framesPerBuffer = 256;
        inParams.suggestedLatency = Pa_GetDeviceInfo(inParams.device)->defaultHighInputLatency;
        #endif
      }

      if ( mOutAudioOptions ) {
        memset(&outParams, 0, sizeof(PaStreamParameters));
        memset(&outAsioStreamInfo, 0, sizeof(outAsioStreamInfo));
        outAsioStreamInfo.size = sizeof(outAsioStreamInfo);
        outAsioStreamInfo.hostApiType = paASIO;
        outAsioStreamInfo.version = 1;
        outAsioStreamInfo.flags = paAsioUseChannelSelectors;
        outAsioStreamInfo.channelSelectors = mOutAudioOptions->channelSelectors().data();

        deviceID = (int32_t)mOutAudioOptions->deviceID();
        if ((deviceID >= 0) && (deviceID < Pa_GetDeviceCount()))
          outParams.device = (PaDeviceIndex)deviceID;
        else
          outParams.device = Pa_GetDefaultOutputDevice();
        if (outParams.device == paNoDevice) {
          Nan::ThrowError("No default output device");
          return;
        }
        deviceInfo = Pa_GetDeviceInfo(outParams.device);
        if ( paASIO != Pa_GetHostApiInfo(deviceInfo->hostApi)->type ) {
          Nan::ThrowError("Not an ASIO device");
          return;
        }

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
        outParams.hostApiSpecificStreamInfo = &outAsioStreamInfo;

        sampleRate = (mSampleRate < 0.0) ? (double)mOutAudioOptions->sampleRate() : sampleRate;
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

  PaError getAvailableBufferSizes
    (
    AsioDirection direction,
    long *minBufferSizeFrames,
    long *maxBufferSizeFrames,
    long *preferredBufferSizeFrames,
    long *granularity
    )
  {
    PaError status(paDeviceUnavailable);
    std::lock_guard<std::mutex> lk(m);
    if ( (direction == OUTPUT) && mOutAudioOptions ) {
      status = PaAsio_GetAvailableBufferSizes((int32_t)mOutAudioOptions->deviceID(),
                                      minBufferSizeFrames,
                                      maxBufferSizeFrames,
                                      preferredBufferSizeFrames,
                                      granularity);
    }
    else if ( (direction == INPUT) && mInAudioOptions ) {
      status = PaAsio_GetAvailableBufferSizes((int32_t)mInAudioOptions->deviceID(),
                                      minBufferSizeFrames,
                                      maxBufferSizeFrames,
                                      preferredBufferSizeFrames,
                                      granularity);
    }
    return status;
  }


  /** Display the ASIO control panel for the specified device.

    @param device The global index of the device whose control panel is to be displayed.
    @param systemSpecific On Windows, the calling application's main window handle,
    on Macintosh this value should be zero.
  */
  PaError showControlPanel
    (
    AsioDirection direction,
    void*         systemSpecific
    ) 
  {
    PaError status(paDeviceUnavailable);
    std::lock_guard<std::mutex> lk(m);
    if ( (direction == OUTPUT) && mOutAudioOptions ) {
      status = PaAsio_ShowControlPanel((int32_t)mOutAudioOptions->deviceID(),
                                      systemSpecific);
    }
    else if ( (direction == INPUT) && mInAudioOptions ) {
      status = PaAsio_ShowControlPanel((int32_t)mInAudioOptions->deviceID(),
                                      systemSpecific);
    }
    return status;
  }


  PaError getInputChannelNames
    (
    int           channelIndex,
    ChannelList&  channelList
    )
  {
    PaError status(paDeviceUnavailable);
    const char* name;
    std::lock_guard<std::mutex> lk(m);
    if ( mInAudioOptions ) {
      channelList.clear();
      if ( channelIndex >= 0 ) {
        status = PaAsio_GetInputChannelName((int32_t)mInAudioOptions->deviceID(),
                                            channelIndex, &name);
        if ( paNoError == status ) {
          channelList.push_back(name);
        }
      }
      else {
        const PaDeviceInfo* devInfo;
        devInfo = Pa_GetDeviceInfo((int32_t)mInAudioOptions->deviceID());
        if ( devInfo ) {
          for ( int idx = 0; idx < devInfo->maxInputChannels; ++idx ) {
            status = PaAsio_GetInputChannelName((int32_t)mInAudioOptions->deviceID(), idx, &name);
            if ( paNoError == status ) {
              channelList.push_back(name);
            }
            else {
              break;
            }
          }
        }
      }
    }
    return status;
  }


  PaError getOutputChannelNames
    (
    int           channelIndex,
    ChannelList&  channelList
    )
  {
    PaError status(paDeviceUnavailable);
    const char* name;
    std::lock_guard<std::mutex> lk(m);
    if ( mOutAudioOptions ) {
      channelList.clear();
      if ( channelIndex >= 0 ) {
        status = PaAsio_GetOutputChannelName((int32_t)mOutAudioOptions->deviceID(),
                                            channelIndex, &name);
        if ( paNoError == status ) {
          channelList.push_back(name);
        }
      }
      else {
        const PaDeviceInfo* devInfo;
        devInfo = Pa_GetDeviceInfo((int32_t)mOutAudioOptions->deviceID());
        if ( devInfo ) {
          for ( int idx = 0; idx < devInfo->maxOutputChannels; ++idx ) {
            status = PaAsio_GetOutputChannelName((int32_t)mOutAudioOptions->deviceID(), idx, &name);
            if ( paNoError == status ) {
              channelList.push_back(name);
            }
            else {
              break;
            }
          }
        }
      }
    }
    return status;
  }


  /** Set the sample rate of an open paASIO stream.
   
  @param stream The stream to operate on.
  @param sampleRate The new sample rate. 

  Note that this function may fail if the stream is already running and the 
  ASIO driver does not support switching the sample rate of a running stream.

  Returns paIncompatibleStreamHostApi if stream is not a paASIO stream.
  */
  PaError setStreamSampleRate( double sampleRate ) {
    PaError status(paStreamIsNotStopped);
    if ( !isActive() ) {
      mSampleRate = sampleRate;
      status = paNoError;
    }

    return status;
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

    bool active = isActive();
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
  double mSampleRate;
  PaStreamCallback* mStreamCb;

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

int AsioIoCallback(const void *input, void *output, unsigned long frameCount,
               const PaStreamCallbackTimeInfo *timeInfo,
               PaStreamCallbackFlags statusFlags, void *userData) {
  int outputRetCode(paComplete);
  int inputRetCode(paComplete);
  AsioContext *context = (AsioContext *)userData;
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

class InputAsioWorker : public Nan::AsyncWorker {
  public:
    InputAsioWorker(std::shared_ptr<AsioContext> AsioContext, Nan::Callback *callback)
      : AsyncWorker(callback), mAsioContext(AsioContext)
    { }
    ~InputAsioWorker() {}

    void Execute() {
      mInChunk = mAsioContext->readChunk();
    }

    void HandleOKCallback () {
      Nan::HandleScope scope;

      std::string errStr;
      if (mAsioContext->getErrStr(errStr)) {
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
    std::shared_ptr<AsioContext> mAsioContext;
    std::shared_ptr<Memory> mInChunk;
};

class OutputAsioWorker : public Nan::AsyncWorker {
  public:
    OutputAsioWorker(std::shared_ptr<AsioContext> AsioContext, Nan::Callback *callback, std::shared_ptr<AudioChunk> audioChunk)
      : AsyncWorker(callback), mAsioContext(AsioContext), mAudioChunk(audioChunk) 
    { }
    ~OutputAsioWorker() {}

    void Execute() {
      mAsioContext->addChunk(mAudioChunk);
    }

    void HandleOKCallback () {
      Nan::HandleScope scope;
      std::string errStr;
      if (mAsioContext->getErrStr(errStr)) {
        Local<Value> argv[] = { Nan::Error(errStr.c_str()) };
        callback->Call(1, argv, async_resource);
      } else {
        callback->Call(0, NULL, async_resource);
      }
    }

  private:
    std::shared_ptr<AsioContext> mAsioContext;
    std::shared_ptr<AudioChunk> mAudioChunk;
};

class QuitAsioWorker : public Nan::AsyncWorker {
  public:
    QuitAsioWorker(std::shared_ptr<AsioContext> AsioContext, Nan::Callback *callback)
      : AsyncWorker(callback), mAsioContext(AsioContext)
    { }
    ~QuitAsioWorker() {}

    void Execute() {
      mAsioContext->quit();
    }

    void HandleOKCallback () {
      Nan::HandleScope scope;
      mAsioContext->stop();
      callback->Call(0, NULL, async_resource);
    }

  private:
    std::shared_ptr<AsioContext> mAsioContext;
};

AudioAsio::AudioAsio(Local<Object> options) {
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

  mAsioContext = std::make_shared<AsioContext>(inOpts, outOpts, AsioIoCallback);
}
AudioAsio::~AudioAsio() {}

void AudioAsio::doStart() { mAsioContext->start(); }

NAN_METHOD(AudioAsio::Start) {
  AudioAsio* obj = Nan::ObjectWrap::Unwrap<AudioAsio>(info.Holder());
  obj->doStart();
  info.GetReturnValue().SetUndefined();
}

NAN_METHOD(AudioAsio::Read) {
  if (info.Length() != 2)
    return Nan::ThrowError("AudioAsio Read expects 2 arguments");
  if (!info[0]->IsNumber())
    return Nan::ThrowError("AudioAsio Read requires a valid advisory size as the first parameter");
  if (!info[1]->IsFunction())
    return Nan::ThrowError("AudioAsio Read requires a valid callback as the second parameter");

  Local<Function> callback = Local<Function>::Cast(info[1]);
  AudioAsio* obj = Nan::ObjectWrap::Unwrap<AudioAsio>(info.Holder());

  if ( !obj->getContext()->isInput() ) {
    return Nan::ThrowError("AudioAsio is not configured for input");
  }

  AsyncQueueWorker(new InputAsioWorker(obj->getContext(), new Nan::Callback(callback)));
  info.GetReturnValue().SetUndefined();
}

NAN_METHOD(AudioAsio::Write) {
  if (info.Length() != 2)
    return Nan::ThrowError("AudioAsio Write expects 2 arguments");
  if (!info[0]->IsObject())
    return Nan::ThrowError("AudioAsio Write requires a valid chunk buffer as the first parameter");
  if (!info[1]->IsFunction())
    return Nan::ThrowError("AudioAsio Write requires a valid callback as the second parameter");

  Local<Object> chunkObj = Local<Object>::Cast(info[0]);
  Local<Function> callback = Local<Function>::Cast(info[1]);
  AudioAsio* obj = Nan::ObjectWrap::Unwrap<AudioAsio>(info.Holder());

  if ( !obj->getContext()->isOutput() ) {
    return Nan::ThrowError("AudioAsio is not configured for output");
  }

  AsyncQueueWorker(new OutputAsioWorker(obj->getContext(), new Nan::Callback(callback), std::make_shared<AudioChunk>(chunkObj)));
  info.GetReturnValue().SetUndefined();
}

NAN_METHOD(AudioAsio::Quit) {
  if (info.Length() != 1)
    return Nan::ThrowError("AudioAsio Quit expects 1 argument");
  if (!info[0]->IsFunction())
    return Nan::ThrowError("AudioAsio Quit requires a valid callback as the parameter");

  Local<Function> callback = Local<Function>::Cast(info[0]);
  AudioAsio* obj = Nan::ObjectWrap::Unwrap<AudioAsio>(info.Holder());

  AsyncQueueWorker(new QuitAsioWorker(obj->getContext(), new Nan::Callback(callback)));
  info.GetReturnValue().SetUndefined();
}

NAN_METHOD(AudioAsio::GetAvailableBufferSizes) {
  AudioAsio* obj = Nan::ObjectWrap::Unwrap<AudioAsio>(info.Holder());
  long minBufferSizeFrames;
  long maxBufferSizeFrames;
  long preferredBufferSizeFrames;
  long granularity;
  v8::Local<v8::Object> result = Nan::New<v8::Object>();

  PaError status = obj->getContext()->getAvailableBufferSizes(
                                      AsioContext::AsioDirection::INPUT,
                                      &minBufferSizeFrames,
                                      &maxBufferSizeFrames,
                                      &preferredBufferSizeFrames,
                                      &granularity);
  if ( status == paNoError ) {
    v8::Local<v8::Object> input = Nan::New<v8::Object>();
    Nan::DefineOwnProperty(result, Nan::New<v8::String>("input").ToLocalChecked(), input);
    Nan::DefineOwnProperty(input, Nan::New<v8::String>("minBufferSizeFrames").ToLocalChecked(),
              Nan::New<v8::Int32>(minBufferSizeFrames));
    Nan::DefineOwnProperty(input, Nan::New<v8::String>("maxBufferSizeFrames").ToLocalChecked(),
              Nan::New<v8::Int32>(maxBufferSizeFrames));
    Nan::DefineOwnProperty(input, Nan::New<v8::String>("preferredBufferSizeFrames").ToLocalChecked(),
              Nan::New<v8::Int32>(preferredBufferSizeFrames));
    Nan::DefineOwnProperty(input, Nan::New<v8::String>("granularity").ToLocalChecked(),
              Nan::New<v8::Int32>(granularity));
  }

  status = obj->getContext()->getAvailableBufferSizes(
                                      AsioContext::AsioDirection::OUTPUT,
                                      &minBufferSizeFrames,
                                      &maxBufferSizeFrames,
                                      &preferredBufferSizeFrames,
                                      &granularity);
  if ( status == paNoError ) {
    v8::Local<v8::Object> output = Nan::New<v8::Object>();
    Nan::DefineOwnProperty(result, Nan::New<v8::String>("output").ToLocalChecked(), output);
    Nan::DefineOwnProperty(output, Nan::New<v8::String>("minBufferSizeFrames").ToLocalChecked(),
              Nan::New<v8::Int32>(minBufferSizeFrames));
    Nan::DefineOwnProperty(output, Nan::New<v8::String>("maxBufferSizeFrames").ToLocalChecked(),
              Nan::New<v8::Int32>(maxBufferSizeFrames));
    Nan::DefineOwnProperty(output, Nan::New<v8::String>("preferredBufferSizeFrames").ToLocalChecked(),
              Nan::New<v8::Int32>(preferredBufferSizeFrames));
    Nan::DefineOwnProperty(output, Nan::New<v8::String>("granularity").ToLocalChecked(),
              Nan::New<v8::Int32>(granularity));
  }

  info.GetReturnValue().Set(result);
}

NAN_METHOD(AudioAsio::ShowControlPanel) {
  AudioAsio* obj = Nan::ObjectWrap::Unwrap<AudioAsio>(info.Holder());
  AsioContext::AsioDirection dir;

  if ( info.Length() < 1 ) {
    Nan::ThrowError("Direction ('input'/'output') must be given");
    return;
  }

  if ( !info[0]->IsString() ) {
    Nan::ThrowError("First argument must be string");
    return;
  }

  Nan::Utf8String dirStr(info[0]);

  if ( 0 == strncmp(*dirStr, "input", dirStr.length()) ) {
    dir = AsioContext::AsioDirection::INPUT; 
  }
  else if ( 0 == strncmp(*dirStr, "output", dirStr.length()) ) {
    dir = AsioContext::AsioDirection::OUTPUT;
  }
  else {
    Nan::ThrowError("Unknown direction string");
    return;
  }

  PaError status = obj->getContext()->showControlPanel(dir, NULL);
  if ( paNoError != status ) {
    Nan::ThrowError(Pa_GetErrorText(status));
    return;
  }
  info.GetReturnValue().SetUndefined();
}

NAN_METHOD(AudioAsio::GetInputChannelNames) {
  AudioAsio* obj = Nan::ObjectWrap::Unwrap<AudioAsio>(info.Holder());
  PaDeviceIndex devIdx(-1);
  AsioContext::ChannelList chanList;
  PaError status;
  v8::Local<v8::Array> jsChanList;
  v8::Local<v8::Value> retValue = Nan::Undefined();

  if ( (info.Length() > 0) && (info[0]->IsInt32()) ) {
    devIdx = Nan::To<int32_t>(info[0]).FromJust();
  }

  status = obj->getContext()->getInputChannelNames(devIdx, chanList);
  if ( status != paNoError ) {
    Nan::ThrowError(Pa_GetErrorText(status));
    return;
  }

  if ( devIdx >= 0 ) {
    retValue = Nan::New(chanList[0]).ToLocalChecked();
  }
  else {
    jsChanList = Nan::New<v8::Array>(chanList.size());
    for ( int chanIdx = 0; chanIdx < chanList.size(); ++chanIdx ) {
      jsChanList->Set(chanIdx, Nan::New(chanList[chanIdx]).ToLocalChecked());
    }
    retValue = jsChanList;
  }
  info.GetReturnValue().Set(retValue);
}

NAN_METHOD(AudioAsio::GetOutputChannelNames) {
  AudioAsio* obj = Nan::ObjectWrap::Unwrap<AudioAsio>(info.Holder());
  PaDeviceIndex devIdx(-1);
  AsioContext::ChannelList chanList;
  PaError status;
  v8::Local<v8::Array> jsChanList;
  v8::Local<v8::Value> retValue = Nan::Undefined();

  if ( (info.Length() > 0) && (info[0]->IsInt32()) ) {
    devIdx = Nan::To<int32_t>(info[0]).FromJust();
  }

  status = obj->getContext()->getOutputChannelNames(devIdx, chanList);
  if ( status != paNoError ) {
    Nan::ThrowError(Pa_GetErrorText(status));
    return;
  }

  if ( devIdx >= 0 ) {
    retValue = Nan::New(chanList[0]).ToLocalChecked();
  }
  else {
    jsChanList = Nan::New<v8::Array>(chanList.size());
    for ( int chanIdx = 0; chanIdx < chanList.size(); ++chanIdx ) {
      jsChanList->Set(chanIdx, Nan::New(chanList[chanIdx]).ToLocalChecked());
    }
    retValue = jsChanList;
  }
  info.GetReturnValue().Set(retValue);
}

NAN_METHOD(AudioAsio::SetStreamSampleRate) {
  AudioAsio* obj = Nan::ObjectWrap::Unwrap<AudioAsio>(info.Holder());
  
  if ( info.Length() == 0 ) {
    Nan::ThrowError("Requires sample rate argument");
    return;
  }

  if ( !info[0]->IsNumber() ) {
    Nan::ThrowError("Sample rate must be a number");
    return;
  }

  PaError status = obj->getContext()->setStreamSampleRate(Nan::To<double>(info[0]).FromJust());
  if ( status != paNoError ) {
    Nan::ThrowError(Pa_GetErrorText(status));
    return;
  }
  info.GetReturnValue().SetUndefined();
}

NAN_MODULE_INIT(AudioAsio::Init) {
  Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>(New);
  tpl->SetClassName(Nan::New("AudioAsio").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  SetPrototypeMethod(tpl, "start", Start);
  SetPrototypeMethod(tpl, "read", Read);
  SetPrototypeMethod(tpl, "write", Write);
  SetPrototypeMethod(tpl, "quit", Quit);
  SetPrototypeMethod(tpl, "getAvailableBufferSizes", GetAvailableBufferSizes);
  SetPrototypeMethod(tpl, "showControlPanel", ShowControlPanel);
  SetPrototypeMethod(tpl, "getInputChannelNames", GetInputChannelNames);
  SetPrototypeMethod(tpl, "getOutputChannelNames", GetOutputChannelNames);
  SetPrototypeMethod(tpl, "setStreamSampleRate", SetStreamSampleRate);

  constructor().Reset(Nan::GetFunction(tpl).ToLocalChecked());
  Nan::Set(target, Nan::New("AudioAsio").ToLocalChecked(),
    Nan::GetFunction(tpl).ToLocalChecked());
}

} // namespace streampunk
