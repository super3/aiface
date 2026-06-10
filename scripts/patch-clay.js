// pebble-clay 1.0.4 predates the flint/gabbro platforms. The SDK unzips the
// package's dist.zip and requires a directory per target platform, so add
// clones inside the zip: flint (Core 2 Duo) mirrors diorite, gabbro (Round 2)
// mirrors chalk. Runs as postinstall, so a fresh npm install re-patches.
var fs = require('fs');
var path = require('path');
var execSync = require('child_process').execSync;

var root = path.join(__dirname, '..', 'node_modules', 'pebble-clay');
var zipPath = path.join(root, 'dist.zip');
var clones = { flint: 'diorite', gabbro: 'chalk' };

function copyDir(src, dst) {
  if (!fs.existsSync(src) || fs.existsSync(dst)) return;
  fs.mkdirSync(dst, { recursive: true });
  fs.readdirSync(src).forEach(function(name) {
    var s = path.join(src, name);
    var d = path.join(dst, name);
    if (fs.statSync(s).isDirectory()) {
      copyDir(s, d);
    } else {
      fs.copyFileSync(s, d);
    }
  });
}

function patchTree(base) {
  Object.keys(clones).forEach(function(target) {
    ['binaries', path.join('include', 'pebble-clay')].forEach(function(sub) {
      copyDir(path.join(base, sub, clones[target]), path.join(base, sub, target));
    });
  });
}

if (!fs.existsSync(zipPath)) {
  console.log('pebble-clay dist.zip not found; skipping patch');
  process.exit(0);
}

var listing = execSync('unzip -l ' + JSON.stringify(zipPath)).toString();
if (listing.indexOf('binaries/flint/') === -1) {
  var tmp = path.join(root, '.dist-patch');
  fs.rmSync(tmp, { recursive: true, force: true });
  execSync('unzip -q ' + JSON.stringify(zipPath) + ' -d ' + JSON.stringify(tmp));
  patchTree(tmp);
  fs.rmSync(zipPath);
  // -z preserves the "LibraryPackage" archive comment the SDK looks for
  execSync('cd ' + JSON.stringify(tmp) + ' && zip -q -r ' + JSON.stringify(zipPath) +
           ' . && echo LibraryPackage | zip -q -z ' + JSON.stringify(zipPath));
  fs.rmSync(tmp, { recursive: true, force: true });
}

// Also patch an already-extracted dist/, if a previous build created one
var dist = path.join(root, 'dist');
if (fs.existsSync(dist)) {
  patchTree(dist);
}

console.log('pebble-clay patched for:', Object.keys(clones).join(', '));
