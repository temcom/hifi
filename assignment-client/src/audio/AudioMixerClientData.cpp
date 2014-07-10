//
//  AudioMixerClientData.cpp
//  assignment-client/src/audio
//
//  Created by Stephen Birarda on 10/18/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <QDebug>

#include <PacketHeaders.h>
#include <UUID.h>

#include "InjectedAudioRingBuffer.h"

#include "AudioMixer.h"
#include "AudioMixerClientData.h"
#include "MovingMinMaxAvg.h"

AudioMixerClientData::AudioMixerClientData() :
    _ringBuffers(),
    _outgoingMixedAudioSequenceNumber(0),
    _incomingAvatarAudioSequenceNumberStats()
{
    
}

AudioMixerClientData::~AudioMixerClientData() {
    for (int i = 0; i < _ringBuffers.size(); i++) {
        // delete this attached PositionalAudioRingBuffer
        delete _ringBuffers[i];
    }
}

AvatarAudioRingBuffer* AudioMixerClientData::getAvatarAudioRingBuffer() const {
    for (int i = 0; i < _ringBuffers.size(); i++) {
        if (_ringBuffers[i]->getType() == PositionalAudioRingBuffer::Microphone) {
            return (AvatarAudioRingBuffer*) _ringBuffers[i];
        }
    }

    // no AvatarAudioRingBuffer found - return NULL
    return NULL;
}

int AudioMixerClientData::parseData(const QByteArray& packet) {

    // parse sequence number for this packet
    int numBytesPacketHeader = numBytesForPacketHeader(packet);
    const char* sequenceAt = packet.constData() + numBytesPacketHeader;
    quint16 sequence = *(reinterpret_cast<const quint16*>(sequenceAt));

    PacketType packetType = packetTypeForPacket(packet);
    if (packetType == PacketTypeMicrophoneAudioWithEcho
        || packetType == PacketTypeMicrophoneAudioNoEcho
        || packetType == PacketTypeSilentAudioFrame) {

        _incomingAvatarAudioSequenceNumberStats.sequenceNumberReceived(sequence);

        // grab the AvatarAudioRingBuffer from the vector (or create it if it doesn't exist)
        AvatarAudioRingBuffer* avatarRingBuffer = getAvatarAudioRingBuffer();
        
        // read the first byte after the header to see if this is a stereo or mono buffer
        quint8 channelFlag = packet.at(numBytesForPacketHeader(packet) + sizeof(quint16));
        bool isStereo = channelFlag == 1;
        
        if (avatarRingBuffer && avatarRingBuffer->isStereo() != isStereo) {
            // there's a mismatch in the buffer channels for the incoming and current buffer
            // so delete our current buffer and create a new one
            _ringBuffers.removeOne(avatarRingBuffer);
            avatarRingBuffer->deleteLater();
            avatarRingBuffer = NULL;
        }

        if (!avatarRingBuffer) {
            // we don't have an AvatarAudioRingBuffer yet, so add it
            avatarRingBuffer = new AvatarAudioRingBuffer(isStereo, AudioMixer::getUseDynamicJitterBuffers());
            _ringBuffers.push_back(avatarRingBuffer);
        }

        // ask the AvatarAudioRingBuffer instance to parse the data
        avatarRingBuffer->parseData(packet);
    } else {
        // this is injected audio

        // grab the stream identifier for this injected audio
        QUuid streamIdentifier = QUuid::fromRfc4122(packet.mid(numBytesForPacketHeader(packet) + sizeof(quint16), NUM_BYTES_RFC4122_UUID));

        _incomingInjectedAudioSequenceNumberStatsMap[streamIdentifier].sequenceNumberReceived(sequence);

        InjectedAudioRingBuffer* matchingInjectedRingBuffer = NULL;

        for (int i = 0; i < _ringBuffers.size(); i++) {
            if (_ringBuffers[i]->getType() == PositionalAudioRingBuffer::Injector
                && ((InjectedAudioRingBuffer*) _ringBuffers[i])->getStreamIdentifier() == streamIdentifier) {
                matchingInjectedRingBuffer = (InjectedAudioRingBuffer*) _ringBuffers[i];
            }
        }

        if (!matchingInjectedRingBuffer) {
            // we don't have a matching injected audio ring buffer, so add it
            matchingInjectedRingBuffer = new InjectedAudioRingBuffer(streamIdentifier, AudioMixer::getUseDynamicJitterBuffers());
            _ringBuffers.push_back(matchingInjectedRingBuffer);
        }

        matchingInjectedRingBuffer->parseData(packet);
    }

    return 0;
}

void AudioMixerClientData::checkBuffersBeforeFrameSend(AABox* checkSourceZone, AABox* listenerZone) {
    for (int i = 0; i < _ringBuffers.size(); i++) {
        if (_ringBuffers[i]->shouldBeAddedToMix()) {
            // this is a ring buffer that is ready to go
            // set its flag so we know to push its buffer when all is said and done
            _ringBuffers[i]->setWillBeAddedToMix(true);
            
            // calculate the average loudness for the next NETWORK_BUFFER_LENGTH_SAMPLES_PER_CHANNEL
            // that would be mixed in
            _ringBuffers[i]->updateNextOutputTrailingLoudness();
            
            if (checkSourceZone && checkSourceZone->contains(_ringBuffers[i]->getPosition())) {
                _ringBuffers[i]->setListenerUnattenuatedZone(listenerZone);
            } else {
                _ringBuffers[i]->setListenerUnattenuatedZone(NULL);
            }
        }
    }
}

void AudioMixerClientData::pushBuffersAfterFrameSend() {

    QList<PositionalAudioRingBuffer*>::iterator i = _ringBuffers.begin();
    while (i != _ringBuffers.end()) {
        // this was a used buffer, push the output pointer forwards
        PositionalAudioRingBuffer* audioBuffer = *i;

        const int INJECTOR_CONSECUTIVE_NOT_MIXED_THRESHOLD = 100;

        if (audioBuffer->willBeAddedToMix()) {
            audioBuffer->shiftReadPosition(audioBuffer->getSamplesPerFrame());
            audioBuffer->setWillBeAddedToMix(false);
        } else if (audioBuffer->getType() == PositionalAudioRingBuffer::Injector
                   && audioBuffer->hasStarted() && audioBuffer->isStarved()
                   && audioBuffer->getConsecutiveNotMixedCount() > INJECTOR_CONSECUTIVE_NOT_MIXED_THRESHOLD) {
            // this is an empty audio buffer that has starved, safe to delete
            // also delete its sequence number stats
            QUuid streamIdentifier = ((InjectedAudioRingBuffer*)audioBuffer)->getStreamIdentifier();
            _incomingInjectedAudioSequenceNumberStatsMap.remove(streamIdentifier);
            delete audioBuffer;
            i = _ringBuffers.erase(i);
            continue;
        }
        i++;
    }
}

AudioStreamStats AudioMixerClientData::getAudioStreamStatsOfStream(const PositionalAudioRingBuffer* ringBuffer) const {
    
    AudioStreamStats streamStats;
    const SequenceNumberStats* streamSequenceNumberStats;

    streamStats._streamType = ringBuffer->getType();
    if (streamStats._streamType == PositionalAudioRingBuffer::Injector) {
        streamStats._streamIdentifier = ((InjectedAudioRingBuffer*)ringBuffer)->getStreamIdentifier();
        streamSequenceNumberStats = &_incomingInjectedAudioSequenceNumberStatsMap[streamStats._streamIdentifier];
    } else {
        streamSequenceNumberStats = &_incomingAvatarAudioSequenceNumberStats;
    }
    
    const MovingMinMaxAvg<quint64>& timeGapStats = ringBuffer->getInterframeTimeGapStatsForStatsPacket();
    streamStats._timeGapMin = timeGapStats.getMin();
    streamStats._timeGapMax = timeGapStats.getMax();
    streamStats._timeGapAverage = timeGapStats.getAverage();
    streamStats._timeGapMovingMin = timeGapStats.getWindowMin();
    streamStats._timeGapMovingMax = timeGapStats.getWindowMax();
    streamStats._timeGapMovingAverage = timeGapStats.getWindowAverage();

    streamStats._ringBufferFramesAvailable = ringBuffer->framesAvailable();
    streamStats._ringBufferCurrentJitterBufferFrames = ringBuffer->getCurrentJitterBufferFrames();
    streamStats._ringBufferDesiredJitterBufferFrames = ringBuffer->getDesiredJitterBufferFrames();
    streamStats._ringBufferStarveCount = ringBuffer->getStarveCount();
    streamStats._ringBufferConsecutiveNotMixedCount = ringBuffer->getConsecutiveNotMixedCount();
    streamStats._ringBufferOverflowCount = ringBuffer->getOverflowCount();
    streamStats._ringBufferSilentFramesDropped = ringBuffer->getSilentFramesDropped();
    
    streamStats._packetStreamStats._numReceived = streamSequenceNumberStats->getNumReceived();
    streamStats._packetStreamStats._numUnreasonable = streamSequenceNumberStats->getNumUnreasonable();
    streamStats._packetStreamStats._numEarly = streamSequenceNumberStats->getNumEarly();
    streamStats._packetStreamStats._numLate = streamSequenceNumberStats->getNumLate();
    streamStats._packetStreamStats._numLost = streamSequenceNumberStats->getNumLost();
    streamStats._packetStreamStats._numRecovered = streamSequenceNumberStats->getNumRecovered();
    streamStats._packetStreamStats._numDuplicate = streamSequenceNumberStats->getNumDuplicate();

    return streamStats;
}

void AudioMixerClientData::sendAudioStreamStatsPackets(const SharedNodePointer& destinationNode) const {
    
    char packet[MAX_PACKET_SIZE];
    NodeList* nodeList = NodeList::getInstance();

    // The append flag is a boolean value that will be packed right after the header.  The first packet sent 
    // inside this method will have 0 for this flag, while every subsequent packet will have 1 for this flag.
    // The sole purpose of this flag is so the client can clear its map of injected audio stream stats when
    // it receives a packet with an appendFlag of 0. This prevents the buildup of dead audio stream stats in the client.
    quint8 appendFlag = 0;

    // pack header
    int numBytesPacketHeader = populatePacketHeader(packet, PacketTypeAudioStreamStats);
    char* headerEndAt = packet + numBytesPacketHeader;

    // calculate how many stream stat structs we can fit in each packet
    const int numStreamStatsRoomFor = (MAX_PACKET_SIZE - numBytesPacketHeader - sizeof(quint8) - sizeof(quint16)) / sizeof(AudioStreamStats);

    // pack and send stream stats packets until all ring buffers' stats are sent
    int numStreamStatsRemaining = _ringBuffers.size();
    QList<PositionalAudioRingBuffer*>::ConstIterator ringBuffersIterator = _ringBuffers.constBegin();
    while (numStreamStatsRemaining > 0) {

        char* dataAt = headerEndAt;

        // pack the append flag
        memcpy(dataAt, &appendFlag, sizeof(quint8));
        appendFlag = 1;
        dataAt += sizeof(quint8);

        // calculate and pack the number of stream stats to follow
        quint16 numStreamStatsToPack = std::min(numStreamStatsRemaining, numStreamStatsRoomFor);
        memcpy(dataAt, &numStreamStatsToPack, sizeof(quint16));
        dataAt += sizeof(quint16);

        // pack the calculated number of stream stats
        for (int i = 0; i < numStreamStatsToPack; i++) {
            AudioStreamStats streamStats = getAudioStreamStatsOfStream(*ringBuffersIterator);
            memcpy(dataAt, &streamStats, sizeof(AudioStreamStats));
            dataAt += sizeof(AudioStreamStats);

            ringBuffersIterator++;
        }
        numStreamStatsRemaining -= numStreamStatsToPack;

        // send the current packet
        nodeList->writeDatagram(packet, dataAt - packet, destinationNode);
    }
}

QString AudioMixerClientData::getAudioStreamStatsString() const {
    QString result;
    AvatarAudioRingBuffer* avatarRingBuffer = getAvatarAudioRingBuffer();
    if (avatarRingBuffer) {
        AudioStreamStats streamStats = getAudioStreamStatsOfStream(avatarRingBuffer);
        result += "mic.desired:" + QString::number(streamStats._ringBufferDesiredJitterBufferFrames)
            + " current:" + QString::number(streamStats._ringBufferCurrentJitterBufferFrames)
            + " available:" + QString::number(streamStats._ringBufferFramesAvailable)
            + " starves:" + QString::number(streamStats._ringBufferStarveCount)
            + " not mixed:" + QString::number(streamStats._ringBufferConsecutiveNotMixedCount)
            + " overflows:" + QString::number(streamStats._ringBufferOverflowCount)
            + " silents dropped:" + QString::number(streamStats._ringBufferSilentFramesDropped)
            + " early:" + QString::number(streamStats._packetStreamStats._numEarly)
            + " late:" + QString::number(streamStats._packetStreamStats._numLate)
            + " lost:" + QString::number(streamStats._packetStreamStats._numLost)
            + " min gap:" + QString::number(streamStats._timeGapMin)
            + " max gap:" + QString::number(streamStats._timeGapMax)
            + " avg gap:" + QString::number(streamStats._timeGapAverage, 'g', 2)
            + " min 30s gap:" + QString::number(streamStats._timeGapMovingMin)
            + " max 30s gap:" + QString::number(streamStats._timeGapMovingMax)
            + " avg 30s gap:" + QString::number(streamStats._timeGapMovingAverage, 'g', 2);
    } else {
        result = "mic unknown";
    }
    
    for (int i = 0; i < _ringBuffers.size(); i++) {
        if (_ringBuffers[i]->getType() == PositionalAudioRingBuffer::Injector) {
            AudioStreamStats streamStats = getAudioStreamStatsOfStream(_ringBuffers[i]);
            result += "mic.desired:" + QString::number(streamStats._ringBufferDesiredJitterBufferFrames)
                + " current:" + QString::number(streamStats._ringBufferCurrentJitterBufferFrames)
                + " available:" + QString::number(streamStats._ringBufferFramesAvailable)
                + " starves:" + QString::number(streamStats._ringBufferStarveCount)
                + " not mixed:" + QString::number(streamStats._ringBufferConsecutiveNotMixedCount)
                + " overflows:" + QString::number(streamStats._ringBufferOverflowCount)
                + " silents dropped:" + QString::number(streamStats._ringBufferSilentFramesDropped)
                + " early:" + QString::number(streamStats._packetStreamStats._numEarly)
                + " late:" + QString::number(streamStats._packetStreamStats._numLate)
                + " lost:" + QString::number(streamStats._packetStreamStats._numLost)
                + " min gap:" + QString::number(streamStats._timeGapMin)
                + " max gap:" + QString::number(streamStats._timeGapMax)
                + " avg gap:" + QString::number(streamStats._timeGapAverage, 'g', 2)
                + " min 30s gap:" + QString::number(streamStats._timeGapMovingMin)
                + " max 30s gap:" + QString::number(streamStats._timeGapMovingMax)
                + " avg 30s gap:" + QString::number(streamStats._timeGapMovingAverage, 'g', 2);
        }
    }
    return result;
}
