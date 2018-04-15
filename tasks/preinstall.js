'use strict';

const https = require('https');
const http = require('http');
const urlLib = require('url');
const zlib = require('zlib');
const fs = require('fs');
const path = require('path');
const execSync = require('child_process').execSync;

const asioSdkUrl = 'https://www.steinberg.net/sdk_downloads/asiosdk2.3.zip';
const asioSdkDir = path.normalize(path.join(__dirname, '..', 'repos','ASIOSDK2.3'));
const reposPath = path.normalize(path.join(__dirname, '..', 'repos'));
const zipPath = path.normalize(path.join(reposPath, path.basename(asioSdkUrl)));
const protocol = { 'http:': http, 'https:': https };
const topLevel = path.normalize(path.join(__dirname, '..'));

// Install the dependencies need for unzipping an archive
execSync('npm install -D extract-zip', { cwd: topLevel });
const extract = require('extract-zip');

function request(url, callback) {
  let urlObj = urlLib.parse(url);
  protocol[urlObj.protocol].get(url, (response) => {
    if ((response.statusCode == 302) || (response.statusCode == 301)) {
      request(response.headers.location, callback);
    } else {
      callback(null, response);
    };
  }).on("error", (err) => {
    callback(err);
  });
};


if ( !fs.existsSync(asioSdkDir) ) {
  request(asioSdkUrl, (err, res) => {
    var error;
    if ( err ) {
      error = err;
    }
    else if (res.statusCode !== 200) {
      error = new Error('Request Failed.\n' +
                        `Status Code: ${res.statusCode}`);
    }
    if (error) {
      console.error(error.message);
      // consume response data to free up memory
      res.resume();
      return;
    }

    let zipFile = fs.createWriteStream(zipPath, { encoding:'binary'});
    res.setEncoding("binary");
    res.once('error', err => {
      console.error(err);
    });
    res.on("data", data => {
      zipFile.write(data);
    });
    res.on("end", () => {
      zipFile.end(() => {
        extract(zipPath, { dir: reposPath }, (err) => {
          if ( err ) {
            console.error(`${err}`);
          }
          else {
            console.log(`${path.basename(asioSdkUrl)} unzipped`);
          }
          // Remove the zipped ASIO SDK
          fs.unlinkSync(zipPath);
        });
      });
    });
  });
}
