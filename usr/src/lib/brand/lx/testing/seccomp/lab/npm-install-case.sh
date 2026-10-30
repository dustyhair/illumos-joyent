#!/bin/sh
set -eu
work=${1:?missing private install directory}
node --version
npm --version
node -e 'const os=require("node:os"); console.log("NODE_RUNTIME="+JSON.stringify({platform:process.platform,arch:process.arch,uid:process.getuid(),release:os.release()})); if(process.platform!=="linux"||process.arch!=="x64")process.exit(1)'
npm install --offline --prefix "$work/install" --no-audit --no-fund \
    @openai/codex@0.166.0
version=$("$work/install/node_modules/.bin/codex" --version)
printf '%s\n' "$version"
test "$version" = 'codex-cli 0.166.0'
node - "$work" <<'JS'
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const root = path.join(process.argv[2], 'install/node_modules');
const main = JSON.parse(fs.readFileSync(path.join(root, '@openai/codex/package.json')));
const platform = path.join(root, '@openai/codex-linux-x64');
const binary = path.join(platform, 'vendor/x86_64-unknown-linux-musl/bin/codex');
const sha256 = crypto.createHash('sha256').update(fs.readFileSync(binary)).digest('hex');
if (main.version !== '0.166.0' ||
    sha256 !== 'b8a11fedf24b2dfb59a70c9e52b030f9a508dcd49cd2dd0d41956f4c785156b8')
    throw new Error('installed Codex does not match the verified release');
console.log('NPM_INSTALLED_CODEX=' + JSON.stringify({version:main.version,sha256}));
JS
# Use the freshly installed npm launcher for the real command/PTY smoke.
/test/zone_codex_audit "$work/install/node_modules/.bin/codex"
printf '%s\n' NPM_INSTALL_CASE_PASS
