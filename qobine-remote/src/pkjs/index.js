/* global Pebble, XMLHttpRequest */

"use strict";

var DEFAULT_SERVER_URL =
  "https://qobine-disconnect.iot-lab.dk";

var DEFAULT_SERVER_SECRET = "";
var DEFAULT_DEVICE_ID = "pebble-watch";

var SERVER_URL =
  localStorage.getItem("serverUrl") ||
  DEFAULT_SERVER_URL;

var SERVER_SECRET =
  localStorage.getItem("serverSecret") ||
  DEFAULT_SERVER_SECRET;

var DEVICE_ID =
  localStorage.getItem("deviceId") ||
  DEFAULT_DEVICE_ID;

var CONFIG_URL =  "https://sofusa.github.io/qobine-pebble/pebble-config.html";

/*
 * Polling configuration
 */
var STATE_POLL_INTERVAL_MS = 2000;
var REQUEST_TIMEOUT_MS = 10000;

/*
 * Commands from the watch
 */
var COMMAND_PREVIOUS = 1;
var COMMAND_PLAY_PAUSE = 2;
var COMMAND_NEXT = 3;

/*
 * Connection states sent to the watch
 */
var CONNECTION_DISCONNECTED = 0;
var CONNECTION_CONNECTED = 1;
var CONNECTION_CONNECTING = 2;

/*
 * These values assume that ControlCommand serializes as a JSON string.
 *
 * Change the values here if the Rust variants have different names.
 */
var CONTROL_PREVIOUS_BODY = JSON.stringify("Previous");
var CONTROL_PLAY_BODY = JSON.stringify("Play");
var CONTROL_PAUSE_BODY = JSON.stringify("Pause");
var CONTROL_NEXT_BODY = JSON.stringify("Next");

var statePollTimer = null;
var stateRequest = null;
var isConnected = false;

var playerState = {
  title: "",
  artist: "",
  album: "",
  positionSeconds: 0,
  durationSeconds: 0,
  isPlaying: false
};

Pebble.addEventListener("ready", function() {
  console.log("Music Remote PebbleKit JS ready");

  sendConnectionState(CONNECTION_CONNECTING);
  startStatePolling();
});

Pebble.addEventListener("appmessage", function(event) {
  var payload = event && event.payload;

  if (!payload || payload.COMMAND === undefined) {
    return;
  }

  handleWatchCommand(Number(payload.COMMAND));
});

Pebble.addEventListener(
  "showConfiguration",
  function() {
    var settings = {
      serverUrl: SERVER_URL,
      serverSecret: SERVER_SECRET,
      deviceId: DEVICE_ID
    };

    Pebble.openURL(
      CONFIG_URL +
      "?settings=" +
      encodeURIComponent(JSON.stringify(settings))
    );
  }
);

Pebble.addEventListener(
  "webviewclosed",
  function(event) {
    if (!event || !event.response) {
      return;
    }

    try {
      var settings = JSON.parse(
        decodeURIComponent(event.response)
      );

      SERVER_URL = normalizeServerUrl(
        settings.serverUrl
      );

      SERVER_SECRET = String(
        settings.serverSecret || ""
      );

      DEVICE_ID = String(
        settings.deviceId || DEFAULT_DEVICE_ID
      );

      localStorage.setItem(
        "serverUrl",
        SERVER_URL
      );

      localStorage.setItem(
        "serverSecret",
        SERVER_SECRET
      );

      localStorage.setItem(
        "deviceId",
        DEVICE_ID
      );

      console.log("Settings saved");

      if (stateRequest !== null) {
        stateRequest.abort();
        stateRequest = null;
      }

      isConnected = false;
      startStatePolling();
    } catch (error) {
      console.log(
        "Unable to read settings: " +
        error.message
      );
    }
  }
);

function normalizeServerUrl(value) {
  var url = String(value || "").trim();

  while (
    url.length > 0 &&
    url.charAt(url.length - 1) === "/"
  ) {
    url = url.substring(0, url.length - 1);
  }

  return url;
}

function startStatePolling() {
  stopStatePolling();

  fetchState();

  statePollTimer = setInterval(function() {
    fetchState();
  }, STATE_POLL_INTERVAL_MS);
}

function stopStatePolling() {
  if (statePollTimer !== null) {
    clearInterval(statePollTimer);
    statePollTimer = null;
  }
}

function fetchState() {
  /*
   * Avoid overlapping state requests.
   */
  if (stateRequest !== null) {
    return;
  }

  var url =
    SERVER_URL +
    "/state?secret=" +
    encodeURIComponent(SERVER_SECRET);

  var request = new XMLHttpRequest();
  stateRequest = request;

  request.open("GET", url, true);
  request.timeout = REQUEST_TIMEOUT_MS;

  request.onreadystatechange = function() {
    if (request.readyState !== 4) {
      return;
    }

    if (stateRequest !== request) {
      return;
    }

    stateRequest = null;

    if (request.status >= 200 && request.status < 300) {
      handleStateResponse(request.responseText);
      return;
    }

    console.log(
      "State request failed, status=" + request.status
    );

    markDisconnected();
  };

  request.onerror = function() {
    if (stateRequest !== request) {
      return;
    }

    stateRequest = null;

    console.log("State request failed");
    markDisconnected();
  };

  request.ontimeout = function() {
    if (stateRequest !== request) {
      return;
    }

    stateRequest = null;

    console.log("State request timed out");
    markDisconnected();
  };

  request.send(null);
}

function handleStateResponse(responseText) {
  try {
    var state = JSON.parse(responseText);

    markConnected();
    applyState(state);
  } catch (error) {
    console.log(
      "Unable to parse state response: " + error.message
    );
  }
}

function applyState(state) {
  if (!state) {
    return;
  }

  if (state.tracklist !== undefined) {
    applyTracklist(state.tracklist);
  }

  if (state.playback_status !== undefined) {
    applyPlaybackStatus(state.playback_status);
  }

  if (state.position !== undefined) {
    playerState.positionSeconds =
      durationToSeconds(state.position);
  }

  clampPosition();
  sendFullUpdate();
}

function applyTracklist(tracklist) {
  var track = findCurrentTrack(tracklist);

  if (!track) {
    /*
     * Preserve the previous metadata if the queue temporarily has no
     * track marked as Playing.
     */
    return;
  }

  playerState.title = stringValue(
    track.title,
    "Unknown title"
  );

  playerState.artist = stringValue(
    track.artist_name,
    "Unknown artist"
  );

  playerState.album = stringValue(
    track.album_title,
    "Unknown album"
  );

  playerState.durationSeconds = positiveInteger(
    track.duration_seconds
  );
}

function findCurrentTrack(tracklist) {
  if (!tracklist || !Array.isArray(tracklist.queue)) {
    return null;
  }

  for (var i = 0; i < tracklist.queue.length; i++) {
    var item = tracklist.queue[i];

    if (
      item &&
      item.track &&
      normalizeEnumValue(item.track.status) === "playing"
    ) {
      return item.track;
    }
  }

  return null;
}

function applyPlaybackStatus(status) {
  playerState.isPlaying =
    normalizeEnumValue(status) === "playing";
}

function handleWatchCommand(command) {
  switch (command) {
    case COMMAND_PREVIOUS:
      sendControlCommand(CONTROL_PREVIOUS_BODY);
      break;

    case COMMAND_PLAY_PAUSE:
      if (playerState.isPlaying) {
        sendControlCommand(CONTROL_PAUSE_BODY);
      } else {
        sendControlCommand(CONTROL_PLAY_BODY);
      }

      break;

    case COMMAND_NEXT:
      sendControlCommand(CONTROL_NEXT_BODY);
      break;

    default:
      console.log("Unknown watch command: " + command);
      break;
  }
}

function sendControlCommand(body) {
  var url =
    SERVER_URL +
    "/control?secret=" +
    encodeURIComponent(SERVER_SECRET) +
    "&device_id=" +
    encodeURIComponent(DEVICE_ID);

  var request = new XMLHttpRequest();

  request.open("POST", url, true);
  request.timeout = REQUEST_TIMEOUT_MS;

  request.setRequestHeader(
    "Content-Type",
    "application/json"
  );

  request.onreadystatechange = function() {
    if (request.readyState !== 4) {
      return;
    }

    if (request.status >= 200 && request.status < 300) {
      console.log("Control command sent");

      /*
       * Fetch updated state shortly after the command instead of waiting
       * for the next regular polling interval.
       */
      scheduleStateRefresh(200);
      return;
    }

    console.log(
      "Control command failed, status=" + request.status
    );

    if (request.status === 0) {
      markDisconnected();
    }
  };

  request.onerror = function() {
    console.log("Control request failed");
    markDisconnected();
  };

  request.ontimeout = function() {
    console.log("Control request timed out");
    markDisconnected();
  };

  request.send(body);
}

function scheduleStateRefresh(delayMs) {
  setTimeout(function() {
    fetchState();
  }, delayMs);
}

function sendFullUpdate() {
  clampPosition();

  var message = {
    TITLE: truncateUtf8(playerState.title, 47),
    ARTIST: truncateUtf8(playerState.artist, 39),
    ALBUM: truncateUtf8(playerState.album, 39),
    POSITION_SECONDS: playerState.positionSeconds,
    DURATION_SECONDS: playerState.durationSeconds,
    IS_PLAYING: playerState.isPlaying ? 1 : 0,
    CONNECTION_STATE: isConnected ?
      CONNECTION_CONNECTED :
      CONNECTION_DISCONNECTED
  };

  Pebble.sendAppMessage(
    message,
    function() {
      console.log("Player state sent to watch");
    },
    function(error) {
      console.log(
        "Failed to send player state: " +
        JSON.stringify(error)
      );
    }
  );
}

function sendConnectionState(connectionState) {
  Pebble.sendAppMessage(
    {
      CONNECTION_STATE: connectionState
    },
    function() {
      console.log(
        "Connection state sent: " + connectionState
      );
    },
    function(error) {
      console.log(
        "Failed to send connection state: " +
        JSON.stringify(error)
      );
    }
  );
}

function markConnected() {
  if (isConnected) {
    return;
  }

  isConnected = true;

  console.log("Server connected");
  sendConnectionState(CONNECTION_CONNECTED);
}

function markDisconnected() {
  if (!isConnected) {
    return;
  }

  isConnected = false;

  console.log("Server disconnected");
  sendConnectionState(CONNECTION_DISCONNECTED);
}

function durationToSeconds(duration) {
  if (duration === undefined || duration === null) {
    return 0;
  }

  if (typeof duration === "number") {
    return positiveInteger(duration);
  }

  if (typeof duration === "string") {
    var parsed = Number(duration);

    if (!isNaN(parsed)) {
      return positiveInteger(parsed);
    }

    return 0;
  }

  if (typeof duration === "object") {
    /*
     * Rust Duration representation:
     *
     * {
     *   "secs": 123,
     *   "nanos": 0
     * }
     */
    if (duration.secs !== undefined) {
      return positiveInteger(duration.secs);
    }

    if (duration.seconds !== undefined) {
      return positiveInteger(duration.seconds);
    }

    if (duration.position_seconds !== undefined) {
      return positiveInteger(
        duration.position_seconds
      );
    }

    if (duration.milliseconds !== undefined) {
      return positiveInteger(
        Number(duration.milliseconds) / 1000
      );
    }

    if (duration.millis !== undefined) {
      return positiveInteger(
        Number(duration.millis) / 1000
      );
    }
  }

  return 0;
}

function normalizeEnumValue(value) {
  if (value === undefined || value === null) {
    return "";
  }

  if (typeof value === "string") {
    return value.toLowerCase();
  }

  if (typeof value === "object") {
    if (value.type !== undefined) {
      return normalizeEnumValue(value.type);
    }

    if (value.status !== undefined) {
      return normalizeEnumValue(value.status);
    }

    if (value.state !== undefined) {
      return normalizeEnumValue(value.state);
    }
  }

  return String(value).toLowerCase();
}

function clampPosition() {
  playerState.positionSeconds = positiveInteger(
    playerState.positionSeconds
  );

  playerState.durationSeconds = positiveInteger(
    playerState.durationSeconds
  );

  if (
    playerState.durationSeconds > 0 &&
    playerState.positionSeconds >
      playerState.durationSeconds
  ) {
    playerState.positionSeconds =
      playerState.durationSeconds;
  }
}

function positiveInteger(value) {
  var number = Number(value);

  if (!isFinite(number) || number < 0) {
    return 0;
  }

  return Math.floor(number);
}

function stringValue(value, fallback) {
  if (
    value === undefined ||
    value === null ||
    value === ""
  ) {
    return fallback;
  }

  return String(value);
}

/*
 * AppMessage string limits are byte-based. This truncates text without
 * splitting a UTF-8 character and reserves three bytes for "...".
 */
function truncateUtf8(value, maximumBytes) {
  var text = String(value || "");

  if (utf8Length(text) <= maximumBytes) {
    return text;
  }

  var suffix = "...";
  var result = "";

  for (var i = 0; i < text.length; i++) {
    var character = text.charAt(i);

    /*
     * Keep UTF-16 surrogate pairs together.
     */
    if (
      character.charCodeAt(0) >= 0xD800 &&
      character.charCodeAt(0) <= 0xDBFF &&
      i + 1 < text.length
    ) {
      character += text.charAt(i + 1);
    }

    if (
      utf8Length(result + character) +
        utf8Length(suffix) >
      maximumBytes
    ) {
      break;
    }

    result += character;

    if (character.length === 2) {
      i += 1;
    }
  }

  return result + suffix;
}

function utf8Length(value) {
  var length = 0;

  for (var i = 0; i < value.length; i++) {
    var code = value.charCodeAt(i);

    if (code < 0x80) {
      length += 1;
    } else if (code < 0x800) {
      length += 2;
    } else if (
      code >= 0xD800 &&
      code <= 0xDBFF &&
      i + 1 < value.length
    ) {
      length += 4;
      i += 1;
    } else {
      length += 3;
    }
  }

  return length;
}
