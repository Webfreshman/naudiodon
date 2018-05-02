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

const util = require("util");
const EventEmitter = require("events");
const { Readable, Writable, Duplex } = require('stream');
const portAudioBindings = require("bindings")("naudiodon.node");
const os = require('os');

// var SegfaultHandler = require('segfault-handler');
// SegfaultHandler.registerHandler("crash.log");

exports.SampleFormat8Bit = 8;
exports.SampleFormat16Bit = 16;
exports.SampleFormat24Bit = 24;
exports.SampleFormat32Bit = 32;

exports.getDevices = portAudioBindings.getDevices;

function AudioInput(options) {
  if (!(this instanceof AudioInput))
    return new AudioInput(options);

  this.AudioInAddon = new portAudioBindings.AudioIn(options);
  Readable.call(this, {
    highWaterMark: 16384,
    objectMode: false,
    read: size => {
      this.AudioInAddon.read(size, (err, buf) => {
        if (err)
          this.emit('error', err); // causes Streampunk Microphone node to exit early...
        else
          this.push(buf);
      });
    }
  });

  this.start = () => this.AudioInAddon.start();
  this.quit = cb => {
    this.AudioInAddon.quit(() => {
      if (typeof cb === 'function')
        cb();
    });
  }
}
util.inherits(AudioInput, Readable);
exports.AudioInput = AudioInput;

function AudioOutput(options) {
  if (!(this instanceof AudioOutput))
    return new AudioOutput(options);

  let Active = true;
  this.AudioOutAddon = new portAudioBindings.AudioOut(options);
  Writable.call(this, {
    highWaterMark: 16384,
    decodeStrings: false,
    objectMode: false,
    write: (chunk, encoding, cb) => this.AudioOutAddon.write(chunk, cb)
  });

  this.start = () => this.AudioOutAddon.start();
  
  this.quit = cb => {
    if ( !Active ) {
      if ( typeof cb === 'function' ) {
        process.nextTick(cb);
      }
    }
    else {
      Active = false;
      this.AudioOutAddon.quit(() => {
        if (typeof cb === 'function')
          cb();
      });
    }
  }

  // Override the Writable implementation
  this.end = function(chunk, encoding, callback) {
    if ( !chunk ) {
      this.quit(() => {
        AudioOutput.super_.prototype.end.call(this, null, encoding, callback);
      });
    }
    else {
      this.AudioOutAddon.write(chunk, (err) => {
        if ( err ) {
          this.emit('error', err);
        }
        else {
          this.quit(() => {
            AudioOutput.super_.prototype.end.call(this, null, encoding, callback);
          });
        }
      });
    }
  }
}
util.inherits(AudioOutput, Writable);
exports.AudioOutput = AudioOutput;

function AudioInputOutput(options) {
  if (!(this instanceof AudioInputOutput))
    return new AudioInputOutput(options);

  let Active = true;
  this.AudioInOutAddon = new portAudioBindings.AudioInOut(options);
  Duplex.call(this, {
    highWaterMark: 16384,
    decodeStrings: false,
    objectMode: false,
    write: (chunk, encoding, cb) => this.AudioInOutAddon.write(chunk, cb),
    read: size => {
      this.AudioInOutAddon.read(size, (err, buf) => {
        if (err)
          this.emit('error', err);
        else
          this.push(buf);
      });
    }
  });

  this.start = () => this.AudioInOutAddon.start();

  this.quit = cb => {
    if ( !Active ) {
      if ( typeof cb === 'function' ) {
        process.nextTick(cb);
      }
    }
    else {
      Active = false;
      this.AudioInOutAddon.quit(() => {
        if (typeof cb === 'function')
          cb();
      });
    }
  }

  // Override the Duplex implementation
  this.end = function(chunk, encoding, callback) {
    if ( !chunk ) {
      this.quit(() => {
        AudioInputOutput.super_.prototype.end.call(this, null, encoding, callback);
      });
    }
    else {
      this.AudioInOutAddon.write(chunk, (err) => {
        if ( err ) {
          this.emit('error', err);
        }
        else {
          this.quit(() => {
            AudioInputOutput.super_.prototype.end.call(this, null, encoding, callback);
          });
        }
      });
    }
  }
}
util.inherits(AudioInputOutput, Duplex);
exports.AudioInputOutput = AudioInputOutput;

//
// ASIO integration currently only available on Win32 platforms
//
if ( os.platform() == 'win32' ) {

  function AudioAsio(options) {
    if (!(this instanceof AudioAsio))
      return new AudioAsio(options);

    let Active = true;
    this.AudioAsioAddon = new portAudioBindings.AudioAsio(options);
    Duplex.call(this, {
      highWaterMark: 16384,
      decodeStrings: false,
      objectMode: false,
      write: (chunk, encoding, cb) => this.AudioAsioAddon.write(chunk, cb),
      read: size => {
        this.AudioAsioAddon.read(size, (err, buf) => {
          if (err)
            this.emit('error', err);
          else
            this.push(buf);
        });
      }
    });

    this.start = () => this.AudioAsioAddon.start();
  
    this.quit = cb => {
      if ( !Active ) {
        if ( typeof cb === 'function' ) {
          process.nextTick(cb);
        }
      }
      else {
        Active = false;
        this.AudioAsioAddon.quit(() => {
          if (typeof cb === 'function')
            cb();
        });
      }
    }

    // Override the Duplex implementation
    this.end = function(chunk, encoding, callback) {
      if ( !chunk ) {
        this.quit(() => {
          AudioAsio.super_.prototype.end.call(this, null, encoding, callback);
        });
      }
      else {
        this.AudioAsioAddon.write(chunk, (err) => {
          if ( err ) {
            this.emit('error', err);
          }
          else {
            this.quit(() => {
              AudioAsio.super_.prototype.end.call(this, null, encoding, callback);
            });
          }
        });
      }
    }
  
    this.getAvailableBufferSizes = function() {
      return this.AudioAsioAddon.getAvailableBufferSizes();
    };

    this.showControlPanel = function(direction) {
      return this.AudioAsioAddon.showControlPanel(direction);
    };

    this.getInputChannelNames = function(channelIndex) {
      return this.AudioAsioAddon.getInputChannelNames(channelIndex);
    };

    this.getOutputChannelNames = function(channelIndex) {
      return this.AudioAsioAddon.getOutputChannelNames(channelIndex);
    };

    this.setStreamSampleRate = function(sampleRate) {
      return this.AudioAsioAddon.setStreamSampleRate(sampleRate);
    };
  }
  util.inherits(AudioAsio, Duplex);
  exports.AudioAsio = AudioAsio;
}
