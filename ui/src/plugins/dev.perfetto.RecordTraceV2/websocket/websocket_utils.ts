// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// The messages read by the adb server have their length prepended in hex.
// This method adds the length at the beginning of the message.
// Example: 'host:track-devices' -> '0012host:track-devices'
// go/codesearch/aosp-android11/system/core/adb/SERVICES.TXT
export function prefixWithHexLen(cmd: string) {
  const hdr = cmd.length.toString(16).padStart(4, '0');
  return hdr + cmd;
}

export function websocketInstructions(os?: 'ANDROID') {
  os;
  return (
    'Instructions:\n' +
    '\n' +
    'On this PC run:\n' +
    '\n' +
    'ssh -L8037:localhost:8037 steamos@frame.local\n' +
    '\n' +
    'Then on your Steam Frame run:\n' +
    '\n' +
    'sudo steamos-readonly disable\n' +
    'sudo pacman --noconfirm -S perfetto perfetto-android\n' +
    '\n' +
    'Keep that window open while recording.'
  );
}

export function disposeWebsocket(ws: WebSocket) {
  ws.onclose = null;
  ws.onerror = null;
  ws.onmessage = null;
  ws.onopen = null;
  try {
    ws.close();
  } catch {}
}
