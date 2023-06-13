#include "PixelblazeCommon.h"
#include "PixelblazeClient.h"
#include "PixelblazeHandlers.h"

#include <ArduinoJson.h>
#include <WebSocketClient.h>

PixelblazeClient::PixelblazeClient(
        WebSocketClient &_wsClient,
        PixelblazeBuffer &_binaryBuffer,
        PixelblazeUnrequestedHandler &_unrequestedHandler,
        ClientConfig &_clientConfig) :
        wsClient(_wsClient), binaryBuffer(_binaryBuffer),
        unrequestedHandler(_unrequestedHandler), clientConfig(_clientConfig),
        json(DynamicJsonDocument(clientConfig.jsonBufferBytes)) {

    byteBuffer = new uint8_t[clientConfig.binaryBufferBytes];
    previewFrameBuffer = new uint8_t[clientConfig.framePreviewBufferBytes];
    textReadBuffer = new char[clientConfig.textReadBufferBytes];
    controls = new Control[clientConfig.controlLimit];
    peers = new Peer[clientConfig.peerLimit];
    replyQueue = new ReplyHandler *[clientConfig.replyQueueSize];
    sequencerState.controls = new Control[clientConfig.controlLimit];
    playlist.items = new PlaylistItem[clientConfig.playlistLimit];
    playlistUpdate.items = new PlaylistItem[clientConfig.playlistLimit];
}

PixelblazeClient::~PixelblazeClient() {
    while (queueLength() > 0) {
        delete replyQueue[queueFront];
        queueFront = (queueFront + 1) % clientConfig.replyQueueSize;
    }

    delete[] byteBuffer;
    delete[] previewFrameBuffer;
    delete[] textReadBuffer;
    delete[] controls;
    delete[] peers;
    delete[] replyQueue;
    delete[] playlist.items;
    delete[] playlistUpdate.items;
}

bool PixelblazeClient::connected() {
    return wsClient.connected();
}

bool PixelblazeClient::connectionMaintenance() {
    if (connected()) {
        return true;
    }

    //TODO: retries n' stuff
    return wsClient.begin("/") == 0;
}

bool PixelblazeClient::getPatterns(AllPatternsReplyHandler &replyHandler) {
    auto *myHandler = new AllPatternsReplyHandler(replyHandler);
    myHandler->requestTsMs = millis();
    myHandler->satisfied = false;

    if (!enqueueReply(myHandler)) {
        delete myHandler;
        return false;
    }

    json.clear();
    json["listPrograms"] = true;
    sendJson(json);
    return true;
}

bool PixelblazeClient::getPlaylist(PlaylistReplyHandler &replyHandler, String &playlistName) {
    auto *myHandler = new PlaylistReplyHandler(replyHandler);
    myHandler->requestTsMs = millis();
    myHandler->satisfied = false;

    if (!enqueueReply(myHandler)) {
        delete myHandler;
        return false;
    }

    json.clear();
    json["getPlaylist"] = playlistName;
    sendJson(json);
    return true;
}

class PlaylistIndexExtractor : public PlaylistReplyHandler {
public:
    explicit PlaylistIndexExtractor(PlaylistIndexHandler &_indexHandler)
            : PlaylistReplyHandler() {
        indexHandler = new PlaylistIndexHandler(_indexHandler);
    };

    ~PlaylistIndexExtractor() override = default;

    void handle(Playlist &playlist) override {
        indexHandler->handle(playlist.position);
        delete indexHandler; //If not done here the wrapped handler would get freed when getPlaylist() returns
    }

private:
    PlaylistIndexHandler *indexHandler{};
};

bool PixelblazeClient::getPlaylistIndex(PlaylistIndexHandler &replyHandler) {
    PlaylistIndexExtractor extractor = PlaylistIndexExtractor(replyHandler);
    return getPlaylist(extractor);
}

void PixelblazeClient::setPlaylistIndex(int idx) {
    json.clear();
    JsonObject pl = json.createNestedObject("playlist");
    pl["position"] = idx;
    sendJson(json);
}

void PixelblazeClient::nextPattern() {
    json.clear();
    json["nextProgram"] = true;
    sendJson(json);
}

class PrevPlaylistReplyHandler : public PlaylistReplyHandler {
public:
    explicit PrevPlaylistReplyHandler(PixelblazeClient *_client)
            : PlaylistReplyHandler() {
        client = _client;
    }

    void handle(Playlist &playlist) override {
        if (playlist.position == 0) {
            client->setPlaylistIndex(playlist.numItems - 1);
        } else {
            client->setPlaylistIndex(playlist.position - 1);
        }
    }

private:
    PixelblazeClient *client;
};

//This is ugly ATM because we don't cache anything. Leaving it void instead of bool because it won't surprise
//me if a future library update adds a standalone prev command
void PixelblazeClient::prevPattern() {
    PrevPlaylistReplyHandler prevHandler = PrevPlaylistReplyHandler(this);
    getPlaylist(prevHandler);
}

void PixelblazeClient::playSequence() {
    json.clear();
    json["runSequencer"] = true;
    sendJson(json);
}

void PixelblazeClient::pauseSequence() {
    json.clear();
    json["runSequencer"] = false;
    sendJson(json);
}

void PixelblazeClient::setSequencerMode(int sequencerMode) {
    json.clear();
    json["sequencerMode"] = sequencerMode;
    sendJson(json);
}

bool PixelblazeClient::getPeers(PeersReplyHandler &replyHandler) {
    auto *myHandler = new PeersReplyHandler(replyHandler);
    myHandler->requestTsMs = millis();
    myHandler->satisfied = false;

    if (!enqueueReply(myHandler)) {
        delete myHandler;
        return false;
    }

    json.clear();
    json["getPeers"] = 1;
    sendJson(json);
    return true;
}

void PixelblazeClient::setCurrentPatternControls(Control *controls, int numControls, bool saveToFlash) {
    json.clear();
    JsonObject controlsObj = json.createNestedObject("setControls");
    for (int idx = 0; idx < numControls; idx++) {
        controlsObj[controls[idx].name] = controls[idx].value;
    }

    json["save"] = saveToFlash;
    sendJson(json);
}

void PixelblazeClient::setCurrentPatternControl(String &controlName, float value, bool saveToFlash) {
    json.clear();
    JsonObject controls = json.createNestedObject("setControls");
    controls[controlName] = value;
    json["save"] = saveToFlash;
    sendJson(json);
}

void PixelblazeClient::setBrightness(float brightness, bool saveToFlash) {
    json.clear();
    json["brightness"] = constrain(brightness, 0, 1);
    json["save"] = saveToFlash;
    sendJson(json);
}

bool PixelblazeClient::getPatternControls(String &patternId, PatternControlReplyHandler &replyHandler) {
    auto *myHandler = new PatternControlReplyHandler(replyHandler);
    myHandler->requestTsMs = millis();
    myHandler->satisfied = false;

    if (!enqueueReply(myHandler)) {
        delete myHandler;
        return false;
    }

    json.clear();
    json["getControls"] = patternId;
    sendJson(json);

    return true;
}

class CurrentControlsReplyExtractor : public SequencerReplyHandler {
public:
    explicit CurrentControlsReplyExtractor(PatternControlReplyHandler *_wrapped)
            : SequencerReplyHandler() {
        wrapped = _wrapped;
    }

    ~CurrentControlsReplyExtractor() override = default;

    void handle(SequencerState &sequencerState) override {
        wrapped->handle(sequencerState.controls, sequencerState.controlCount);
        delete wrapped; //If not done here the wrapped handler would get freed when getCurrentPatternControls() returns
    }

private:
    PatternControlReplyHandler *wrapped;
};

bool PixelblazeClient::getCurrentPatternControls(PatternControlReplyHandler &replyHandler) {
    auto *myHandler = new PatternControlReplyHandler(replyHandler);
    auto extractor = CurrentControlsReplyExtractor(myHandler);

    NoopSettingsReplyHandler noopSettingsHandler = NoopSettingsReplyHandler();
    NoopExpanderConfigReplyHandler noopExpanderConfig = NoopExpanderConfigReplyHandler();
    return getSettings(noopSettingsHandler, extractor, noopExpanderConfig);
}

bool PixelblazeClient::getPreviewImage(String &patternId, PreviewImageReplyHandler &replyHandler) {
    auto *myHandler = new PreviewImageReplyHandler(replyHandler);
    myHandler->requestTsMs = millis();
    myHandler->satisfied = false;

    if (!enqueueReply(myHandler)) {
        delete myHandler;
        return false;
    }

    json.clear();
    json["getPreviewImg"] = patternId;
    sendJson(json);
    return true;
}

void PixelblazeClient::setBrightnessLimit(int value, bool saveToFlash) {
    json.clear();
    json["maxBrightness"] = constrain(value, 0, 100);
    json["save"] = saveToFlash;
    sendJson(json);
}

void PixelblazeClient::setPixelCount(uint32_t pixels, bool saveToFlash) {
    json.clear();
    json["pixelCount"] = pixels;
    json["save"] = saveToFlash;
    sendJson(json);
}

bool PixelblazeClient::getSettings(
        SettingsReplyHandler &settingsHandler,
        SequencerReplyHandler &seqHandler,
        ExpanderConfigReplyHandler &expanderHandler,
        int watchResponses) {

    unsigned long timeMs = millis();

    auto mySettingsHandler = new SettingsReplyHandler(settingsHandler);
    mySettingsHandler->requestTsMs = timeMs;
    mySettingsHandler->satisfied = false;
    if (!(watchResponses & WATCH_CONFIG_REQ)) {
        mySettingsHandler->satisfied = true;
    }

    auto *mySeqHandler = new SequencerReplyHandler(seqHandler);
    mySeqHandler->requestTsMs = timeMs;
    mySeqHandler->satisfied = false;
    if (!(watchResponses & WATCH_SEQ_REQ)) {
        mySeqHandler->satisfied = true;
    }

    auto *myExpanderHandler = new ExpanderConfigReplyHandler(expanderHandler);
    myExpanderHandler->requestTsMs = timeMs;
    myExpanderHandler->satisfied = false;
    if (!(watchResponses & WATCH_EXPANDER_REQ)) {
        myExpanderHandler->satisfied = true;
    }

    if (!enqueueReplies(3, mySettingsHandler, mySeqHandler, myExpanderHandler)) {
        delete mySettingsHandler;
        delete mySeqHandler;
        delete myExpanderHandler;
        return false;
    }

    json.clear();
    json["getConfig"] = true;
    sendJson(json);
    return true;
}

bool PixelblazeClient::ping(PingReplyHandler &replyHandler) {
    auto *myHandler = new PingReplyHandler(replyHandler);
    myHandler->requestTsMs = millis();
    myHandler->satisfied = false;

    if (!enqueueReply(myHandler)) {
        delete myHandler;
        return false;
    }

    json.clear();
    json["ping"] = true;
    sendJson(json);
    return true;
}

void PixelblazeClient::sendFramePreviews(bool sendEm) {
    json.clear();
    json["sendUpdates"] = sendEm;
    sendJson(json);
}

bool PixelblazeClient::getRawBinary(RawBinaryHandler &replyHandler, JsonDocument &request) {
    auto *myHandler = new RawBinaryHandler(replyHandler);
    myHandler->requestTsMs = millis();
    myHandler->satisfied = false;

    if (!enqueueReply(myHandler)) {
        delete myHandler;
        return false;
    }

    sendJson(request);
    return true;
}

bool PixelblazeClient::getRawText(RawTextHandler &replyHandler, JsonDocument &request) {
    auto *myHandler = new RawTextHandler(replyHandler);
    myHandler->requestTsMs = millis();
    myHandler->satisfied = false;

    if (!enqueueReply(myHandler)) {
        delete myHandler;
        return false;
    }

    sendJson(request);
    return true;
}

void PixelblazeClient::weedExpiredReplies() {
    uint32_t currentTimeMs = millis();
    while (queueLength() > 0) {
        if (replyQueue[queueFront]->isSatisfied() ||
            replyQueue[queueFront]->requestTsMs + clientConfig.maxResponseWaitMs < currentTimeMs) {
            queueFront = (queueFront + 1) % clientConfig.replyQueueSize;
        } else {
            return;
        }
    }
}

void PixelblazeClient::seekingTextHasText() {
    DeserializationError deErr = deserializeJson(json, wsClient.readString());
    if (deErr) {
        Serial.print("Message deserialization error: ");
        Serial.println(deErr.f_str());
    } else {
        if (replyQueue[queueFront]->jsonMatches(json)) {
            dispatchTextReply(replyQueue[queueFront]);
            dequeueReply();
        } else {
            handleUnrequestedJson();
        }
    }
}

void PixelblazeClient::seekingTextHasBinary() {
    handleUnrequestedBinary(wsClient.read());
}

void PixelblazeClient::seekingBinaryHasBinary() {
    auto *binaryHandler = (BinaryReplyHandler *) replyQueue[queueFront];
    int frameType = wsClient.read();
    if (frameType < 0) {
        Serial.println("Empty binary body received");
    } else if (binaryReadType < 0) {
        //We've read nothing so far, blank slate
        if (frameType == binaryHandler->binType) {
            int frameFlag = wsClient.read();
            if (frameFlag & FRAME_FIRST & FRAME_LAST) {
                //Lone message
                if (readBinaryToStream(binaryHandler->bufferId, false)) {
                    dispatchBinaryReply(replyQueue[queueFront]);
                }
                if (replyQueue[queueFront]->shouldDeleteBuffer()) {
                    binaryBuffer.deleteStreamResults(binaryHandler->bufferId);
                }
                dequeueReply();
            } else if (frameFlag & FRAME_FIRST) {
                if (!readBinaryToStream(binaryHandler->bufferId, false)) {
                    binaryBuffer.deleteStreamResults(binaryHandler->bufferId);
                    dequeueReply();
                    return;
                }
                binaryReadType = frameType;
            } else {
                //Frame was middle, last, or 0, none of which should happen
                Serial.print("Got unexpected frameFlag: ");
                Serial.print(frameFlag);
                Serial.print("For frameType: ");
                Serial.println(frameType);
            }
        } else {
            handleUnrequestedBinary(frameType);
        }
    } else if (frameType == binaryReadType) {
        //We're mid read and the latest is compatible
        int frameFlag = wsClient.read();
        if (frameFlag & FRAME_LAST) {
            if (readBinaryToStream(binaryHandler->bufferId, true)) {
                dispatchBinaryReply(replyQueue[queueFront]);
            }

            if (replyQueue[queueFront]->shouldDeleteBuffer()) {
                binaryBuffer.deleteStreamResults(binaryHandler->bufferId);
            }
            dequeueReply();
        } else if (frameFlag & FRAME_MIDDLE) {
            if (!readBinaryToStream(binaryHandler->bufferId, true)) {
                binaryBuffer.deleteStreamResults(binaryHandler->bufferId);
                dequeueReply();
                return;
            }
        } else {
            //Frame was first or 0, neither of which should happen
            Serial.print("Got unexpected frameFlag: ");
            Serial.print(frameFlag);
            Serial.print("For frameType: ");
            Serial.println(frameType);
        }
    } else {
        //We're mid read and just got an incompatible frame
        if (!handleUnrequestedBinary(frameType)) {
            Serial.print("Expected frameType: ");
            Serial.print(binaryReadType);
            Serial.print(" but got: ");
            Serial.println(frameType);

            //Scrap the current read, if the finisher never comes it would drop requested events until weeded
            binaryBuffer.deleteStreamResults(binaryHandler->bufferId);
            dequeueReply();
            binaryReadType = -1;
        }
    }
}

void PixelblazeClient::seekingBinaryHasText() {
    DeserializationError deErr = deserializeJson(json, wsClient.readString());
    if (deErr) {
        Serial.print("Message deserialization error: ");
        Serial.println(deErr.f_str());
    } else {
        handleUnrequestedJson();
    }
}


void PixelblazeClient::checkForInbound() {
    weedExpiredReplies();
    uint32_t startTime = millis();

    int read = wsClient.parseMessage();
    while (read > 0 && startTime + clientConfig.maxInboundCheckMs > millis()) {
        int format = wsClient.messageType();
        while (queueLength() > 0 && replyQueue[queueFront]->isSatisfied()) {
            dequeueReply();
        }

        if (queueLength() == 0) {
            //Nothing expected, dispatch everything through unrequested functions
            if (format == FORMAT_TEXT) {
                DeserializationError deErr = deserializeJson(json, wsClient.readString());
                if (deErr) {
                    Serial.print("Message deserialization error: ");
                    Serial.println(deErr.f_str());
                } else {
                    handleUnrequestedJson();
                }
            } else if (format == FORMAT_BINARY && wsClient.available() > 0) {
                handleUnrequestedBinary(wsClient.read());
            } else {
                Serial.print("Unexpected reply format: ");
                Serial.println(format);
            }
        } else {
            int soughtFormat = replyQueue[queueFront]->format;

            int repliesExamined = 0;
            while (repliesExamined <= queueLength()
                   && soughtFormat == FORMAT_BINARY
                   && ((BinaryReplyHandler *) replyQueue[queueFront])->binType == BIN_TYPE_EXPANDER_CONFIG
                   && (format != FORMAT_BINARY || wsClient.peek() != BIN_TYPE_EXPANDER_CONFIG)) {
                //Expander configs can be non-optionally fetched by getConfig, and may never come if no expander is installed.
                //If the head of the queue is seeking them and the current message isn't one, bump it to the back of the queue.
                //This does require special handling in handleUnrequestedBinary(). If it's the only thing in the queue we'll be
                //thrashing it a bit, but that should be fine.

                ReplyHandler *expanderHandler = replyQueue[queueFront];
                replyQueue[queueFront] = nullptr;
                queueFront = (queueFront + 1) % clientConfig.replyQueueSize;
                enqueueReply(expanderHandler);

                soughtFormat = replyQueue[queueFront]->format;
                repliesExamined++;
            }

            if (soughtFormat != FORMAT_TEXT && soughtFormat != FORMAT_BINARY) {
                Serial.print("Unexpected sought format: ");
                Serial.println(soughtFormat);
                dequeueReply();
            } else if (format == FORMAT_TEXT) {
                if (soughtFormat == FORMAT_TEXT) {
                    seekingTextHasText();
                } else {
                    seekingBinaryHasText();
                }
            } else if (format == FORMAT_BINARY) {
                if (soughtFormat == FORMAT_TEXT) {
                    seekingTextHasBinary();
                } else {
                    seekingBinaryHasBinary();
                }
            } else {
                Serial.print("Unexpected reply format: ");
                Serial.println(format);
                dequeueReply();
            }
        }

        read = wsClient.parseMessage();
    }
}

bool PixelblazeClient::readBinaryToStream(String &bufferId, bool append) {
    CloseableStream* stream = binaryBuffer.makeWriteStream(bufferId, append);
    if (!stream) {
        Serial.println("Couldn't open write stream, attempting to garbage collect");
        binaryBuffer.garbageCollect();
        stream = binaryBuffer.makeWriteStream(bufferId, append);
    }

    if (!stream) {
        Serial.print("Failed to get write stream for: ");
        Serial.println(bufferId);
        return false;
    }

    int available = wsClient.available();
    while (available > 0) {
        int bytesRead = wsClient.read(byteBuffer, min(clientConfig.binaryBufferBytes, available));
        size_t written = stream->write(byteBuffer, bytesRead);
        if (bytesRead != written) {
            Serial.print("Partial write on stream for bufferId: ");
            Serial.println(bufferId);
            return false;
        }

        available -= bytesRead;
    }

    stream->close();
    return true;
}

void PixelblazeClient::dispatchTextReply(ReplyHandler *genHandler) {
    ReplyHandler *handler = genHandler;
    if (genHandler->replyType == HANDLER_SYNC) {
        auto *syncHandler = (SyncHandler *) genHandler;
        syncHandler->finish();
        handler = syncHandler->getWrapped();
    }

    switch (handler->replyType) {
        case HANDLER_RAW_TEXT: {
            auto *rawTextHandler = (RawTextHandler *) handler;
            rawTextHandler->handle(json);
            break;
        }
        case HANDLER_PLAYLIST: {
            auto *playlistHandler = (PlaylistReplyHandler *) handler;
            JsonObject playlistObj = json["playlist"];
            playlist.id = playlistObj["id"].as<String>();
            playlist.position = playlistObj["position"];
            playlist.currentDurationMs = playlistObj["ms"];
            playlist.remainingCurrentMs = playlistObj["remainingMs"];

            JsonArray items = playlistObj["items"];
            int itemIdx = 0;
            for (JsonVariant v: items) {
                JsonObject itemObj = v.as<JsonObject>();
                playlist.items[itemIdx].id = itemObj["id"].as<String>();
                playlist.items[itemIdx].durationMs = itemObj["ms"];
                itemIdx++;
                if (itemIdx >= clientConfig.playlistLimit) {
                    Serial.print("Got too many patterns on playlist to store: ");
                    Serial.print(items.size());
                    break;
                }
            }
            playlist.numItems = itemIdx;

            playlistHandler->handle(playlist);
            break;
        }
        case HANDLER_PEERS: {
            auto *peerHandler = (PeersReplyHandler *) handler;
            //TODO, and remember to bounds check against clientConfig.peerLimit
            peerHandler->handle(peers, peerCount);
            break;
        }
        case HANDLER_SETTINGS: {
            auto *settingsHandler = (SettingsReplyHandler *) handler;
            settings.name = json["name"].as<String>();
            settings.brandName = json["brandName"].as<String>();
            settings.pixelCount = json["pixelCount"];
            settings.brightness = json["brightness"];
            settings.maxBrightness = json["maxBrightness"];
            settings.colorOrder = json["colorOrder"].as<String>();
            settings.dataSpeed = json["dataSpeed"];
            settings.ledType = json["ledType"];
            settings.sequenceTimerMs = json["sequenceTimer"];
            settings.transitionDurationMs = json["transitionDuration"];
            settings.sequencerMode = json["sequencerMode"];
            settings.runSequencer = json["runSequencer"];
            settings.simpleUiMode = json["simpleUiMode"];
            settings.learningUiMode = json["learningUiMode"];
            settings.discoveryEnabled = json["discoveryEnable"];
            settings.timezone = json["timezone"].as<String>();
            settings.autoOffEnable = json["autoOffEnable"];
            settings.autoOffStart = json["autoOffStart"].as<String>();
            settings.autoOffEnd = json["autoOffEnd"].as<String>();
            settings.cpuSpeedMhz = json["cpuSpeed"];
            settings.networkPowerSave = json["networkPowerSave"];
            settings.mapperFit = json["mapperFit"];
            settings.leaderId = json["leaderId"];
            settings.nodeId = json["nodeId"];
            settings.soundSrc = json["soundSrc"];
            settings.accelSrc = json["accelSrc"];
            settings.lightSrc = json["lightSrc"];
            settings.exp = json["exp"];
            settings.version = json["ver"].as<String>();
            settings.chipId = json["chipId"];

            settingsHandler->handle(settings);
            break;
        }
        case HANDLER_SEQ: {
            auto *seqHandler = (SequencerReplyHandler *) handler;
            parseSequencerState();
            seqHandler->handle(sequencerState);
            break;
        }
        case HANDLER_PING: {
            auto *pingHandler = (PingReplyHandler *) handler;
            pingHandler->handle(millis() - pingHandler->requestTsMs);
            break;
        }
        case HANDLER_PATTERN_CONTROLS: {
            break;
        }
        default: {
            Serial.print("Got unexpected text reply type: ");
            Serial.println(handler->replyType);
        }

    }
}

void PixelblazeClient::parseSequencerState() {
    JsonObject activeProgram = json["activeProgram"];
    sequencerState.name = activeProgram["name"].as<String>();
    sequencerState.activeProgramId = activeProgram["activeProgramId"].as<String>();

    JsonObject controlsObj = activeProgram["controls"];
    int controlIdx = 0;
    for (JsonPair kv: controlsObj) {
        sequencerState.controls[controlIdx].name = kv.key().c_str();
        sequencerState.controls[controlIdx].value = kv.value();
        controlIdx++;
        if (controlIdx >= clientConfig.controlLimit) {
            Serial.print("Got more controls than could be saved: ");
            Serial.println(controlsObj.size());
            break;
        }
    }
    sequencerState.controlCount = controlIdx;

    sequencerState.sequencerMode = json["sequencerMode"];
    sequencerState.runSequencer = json["runSequencer"];

    JsonObject playlistObj = json["playlist"];
    sequencerState.playlistPos = playlistObj["position"];
    sequencerState.playlistId = playlistObj["id"].as<String>();
    sequencerState.ttlMs = playlistObj["ms"];
    sequencerState.remainingMs = playlistObj["remainingMs"];
}

void PixelblazeClient::dispatchBinaryReply(ReplyHandler *handler) {
    BinaryReplyHandler *binHandler;
    if (handler->replyType == HANDLER_SYNC) {
        auto *syncHandler = (SyncHandler *) handler;
        syncHandler->finish();
        binHandler = (BinaryReplyHandler *) syncHandler->wrappedHandler;
    } else {
        binHandler = (BinaryReplyHandler *) handler;
    }

    auto stream = binaryBuffer.makeReadStream(binHandler->bufferId);
    if (!stream) {
        Serial.print("Couldn't open read string for bufferId: ");
        Serial.println(binHandler->bufferId);
        return;
    }

    switch (binHandler->replyType) {
        case HANDLER_RAW_BINARY: {
            auto rawHandler = (RawBinaryHandler *) binHandler;
            rawHandler->handle(stream);
            break;
        }
        case HANDLER_ALL_PATTERNS: {
            auto allPatternsHandler = (AllPatternsReplyHandler *) binHandler;
            auto iterator = AllPatternIterator(stream, textReadBuffer, clientConfig.textReadBufferBytes);
            allPatternsHandler->handle(iterator);
            break;
        }
        case HANDLER_PREVIEW_IMG: {
            auto previewImageHandler = (PreviewImageReplyHandler *) binHandler;
            size_t buffIdx = 0;
            int peek = stream->peek();
            while (peek >= 0 && peek < 0xFF && buffIdx < clientConfig.textReadBufferBytes) {
                stream->read();
                textReadBuffer[buffIdx] = (char) peek;
                buffIdx++;
                peek = stream->peek();
            }

            textReadBuffer[buffIdx] = '\0';
            if (buffIdx == clientConfig.textReadBufferBytes && peek != 0xFF) {
                while (peek >= 0 && peek != 0xFF) {
                    peek = stream->read();
                }
            } else if (peek == 0xFF) {
                stream->read();
            }

            String id = textReadBuffer;
            previewImageHandler->handle(id, stream);
            break;
        }
        case HANDLER_EXPANDER_CONF: {
            //TODO
            break;
        }
        default: {
            Serial.print("Got unexpected binary reply type: ");
            Serial.println(binHandler->replyType);
        }
    }

    stream->close();
    delete stream;
}

String PixelblazeClient::humanizeVarName(String &camelCaseVar, int maxWords) {
    camelCaseVar = camelCaseVar.substring(0);
    if (camelCaseVar.length() == 0) {
        return "";
    } else if (camelCaseVar.equals("slider")) {
        return "Slider";
    }

    size_t startIdx = 0;
    if (camelCaseVar.startsWith("slider")) {
        startIdx = 6;
    } else {
        camelCaseVar.setCharAt(0, (char) toupper(camelCaseVar.charAt(0)));
    }

    size_t wordStarts[maxWords];
    size_t numStarts = 0;
    for (size_t idx = startIdx; idx < camelCaseVar.length(); idx++) {
        if (isupper(camelCaseVar.charAt(idx))) {
            wordStarts[numStarts] = idx;
            numStarts++;
            if (numStarts >= maxWords) {
                break;
            }
        }
    }

    String result = "";
    for (size_t idx = 0; idx < numStarts - 1; idx++) {
        result += camelCaseVar.substring(wordStarts[idx], wordStarts[idx + 1]) + " ";
    }
    result += camelCaseVar.substring(wordStarts[numStarts - 1]);

    return result;
}

void PixelblazeClient::sendJson(JsonDocument &doc) {
    wsClient.beginMessage(FORMAT_TEXT);
    serializeJson(doc, wsClient);
    wsClient.endMessage();
}

void PixelblazeClient::handleUnrequestedJson() {
    if (json.containsKey("fps")) {
        statsEvent.fps = json["fps"];
        statsEvent.vmerr = json["vmerr"];
        statsEvent.vmerrpc = json["vmerrpc"];
        statsEvent.memBytes = json["mem"];
        statsEvent.expansions = json["exp"];
        statsEvent.renderType = json["renderType"];
        statsEvent.uptimeMs = json["uptime"];
        statsEvent.storageBytesUsed = json["storageUsed"];
        statsEvent.storageBytesSize = json["storageSize"];
        statsEvent.rr0 = json["rr0"];
        statsEvent.rr1 = json["rr1"];
        statsEvent.rebootCounter = json["rebootCounter"];

        unrequestedHandler.handleStats(statsEvent);
    } else if (json.containsKey("activeProgram")) {
        //This is also sent as part of the response to getConfig
        parseSequencerState();
        unrequestedHandler.handlePatternChange(sequencerState);
    } else if (json.containsKey("playlist")) {
        //TODO
        //unrequestedHandler.handlePlaylistChange()
    }
}

bool PixelblazeClient::handleUnrequestedBinary(int frameType) {
    if (frameType == BIN_TYPE_PREVIEW_FRAME) {
        //Should always be 300 bytes, but just in case...
        int frameSize = wsClient.read(previewFrameBuffer,
                                      min(wsClient.available(), clientConfig.framePreviewBufferBytes));
        unrequestedHandler.handlePreviewFrame(previewFrameBuffer, frameSize);
        return true;
    } else if (frameType == BIN_TYPE_EXPANDER_CONFIG) {
        // Expander configs can come in out of order, check if one has been requested
        size_t queuePos = queueFront;
        while (queuePos != queueBack) {
            if (replyQueue[queuePos]->format == FORMAT_BINARY &&
                ((BinaryReplyHandler *) replyQueue[queuePos])->binType == BIN_TYPE_EXPANDER_CONFIG) {
                dispatchBinaryReply(replyQueue[queuePos]);
                replyQueue[queuePos]->satisfied = true;
                break;
            }
            queuePos = (queuePos + 1) % clientConfig.replyQueueSize;
        }
        return true;
    }

    return false;
}

size_t PixelblazeClient::queueLength() {
    if (queueFront > queueBack) {
        //Queue shape: |***B...F**|
        return queueBack + (clientConfig.replyQueueSize - queueFront);
    } else {
        //Queue shape: |.F*****B..| or empty with them ==
        return queueBack - queueFront;
    }
}

bool PixelblazeClient::enqueueReply(ReplyHandler *replyHandler) {
    return enqueueReplies(1, replyHandler);
}

bool PixelblazeClient::enqueueReplies(int num, ...) {
    if (num == 0) {
        Serial.println("Got empty enqueue request");
        return true;
    }

    //In order to drop handling parts of a response like from getSettings, sometimes we mark replies
    //satisfied before we even enqueue them. There are probably cleaner ways but this works.
    int toEnqueue = 0;
    va_list arguments;
    va_start(arguments, num);
    for (int idx = 0; idx < num; idx++) {
        ReplyHandler *handler = va_arg(arguments, ReplyHandler*);
        if (handler && !handler->isSatisfied()) {
            toEnqueue++;
        }
    }
    va_end(arguments);

    if (toEnqueue == 0) {
        return true;
    }

    //Verify that there's space
    if (clientConfig.replyQueueSize - queueLength() < toEnqueue) {
        //Last ditch compact and try again
        compactQueue();
        if (clientConfig.replyQueueSize - queueLength() < toEnqueue) {
            return false;
        }
    }

    va_start(arguments, num);
    for (int idx = 0; idx < num; idx++) {
        ReplyHandler *handler = va_arg(arguments, ReplyHandler*);
        if (handler && !handler->isSatisfied()) {
            replyQueue[queueBack] = handler;
            queueBack = (queueBack + 1) % clientConfig.replyQueueSize;
        } else if (handler) {
            handler->cleanup();
            delete handler;
        }
    }
    va_end(arguments);

    return true;
}

void PixelblazeClient::dequeueReply() {
    if (queueLength() == 0) {
        Serial.println("Dequeue called on empty queue");
        return;
    }

    replyQueue[queueFront]->cleanup();
    delete replyQueue[queueFront];
    replyQueue[queueFront] = nullptr;
    queueFront = (queueFront + 1) % clientConfig.replyQueueSize;
}

//Last ditch when an enqueue fails
void PixelblazeClient::compactQueue() {
    int toKeep = 0;
    unsigned long nowMs = millis();
    for (size_t idx = queueFront; idx != queueBack; idx = (idx + 1) % clientConfig.replyQueueSize) {
        if (!replyQueue[idx]->isSatisfied() && replyQueue[idx]->requestTsMs + clientConfig.maxResponseWaitMs > nowMs) {
            toKeep++;
        }
    }

    if (toKeep == 0) {
        for (size_t idx = 0; idx < clientConfig.replyQueueSize; idx++) {
            if (replyQueue[idx]) {
                replyQueue[idx]->cleanup();
                delete replyQueue[idx];
                replyQueue[idx] = nullptr;
            }
        }

        queueFront = 0;
        queueBack = 0;
    } else if (toKeep == clientConfig.replyQueueSize) {
        //Nothing to do
    } else {
        ReplyHandler *temp[toKeep];
        int tempIdx = 0;
        for (size_t idx = queueFront; idx != queueBack; idx = (idx + 1) % clientConfig.replyQueueSize) {
            if (!replyQueue[idx]->isSatisfied() &&
                replyQueue[idx]->requestTsMs + clientConfig.maxResponseWaitMs > nowMs) {
                temp[tempIdx] = replyQueue[idx];
                tempIdx++;
            } else {
                delete replyQueue[idx];
            }
            replyQueue[idx] = nullptr;
        }

        for (int idx = 0; idx < toKeep; idx++) {
            replyQueue[idx] = temp[idx];
        }

        queueFront = 0;
        queueBack = toKeep;
    }
}

bool AllPatternIterator::next(PatternIdentifiers &fillMe) {
    size_t buffIdx = 0;
    int read = stream->read();
    if (read < 0) {
        return false;
    }

    while (read >= 0 && read != '\t' && buffIdx < bufferLen) {
        readBuffer[buffIdx] = (char) read;
        buffIdx++;
        read = stream->read();
    }

    //Our length limit exceeded, unclear what id length limits are though 16 bytes seems standard
    if (buffIdx >= bufferLen) {
        while (read >= 0 && read != '\t') {
            read = stream->read();
        }
    }

    if (read < 0) {
        Serial.println("Got malformed all pattern response.");
        return false;
    }

    readBuffer[buffIdx] = '\0';
    fillMe.id = readBuffer;

    buffIdx = 0;
    while (read >= 0 && read != '\n' && buffIdx < bufferLen) {
        readBuffer[buffIdx] = (char) read;
        buffIdx++;
        read = stream->read();
    }

    //Our length limit exceeded, unclear what name length limits are
    if (buffIdx >= bufferLen) {
        while (read >= 0 && read != '\n') {
            read = stream->read();
        }
    }

    readBuffer[buffIdx] = '\0';
    fillMe.name = readBuffer;

    return true;
}
