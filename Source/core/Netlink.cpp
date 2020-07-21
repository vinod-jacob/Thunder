 /*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2020 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#include <linux/rtnetlink.h>

#include "Netlink.h"
#include "Sync.h"

// #define DEBUG_FRAMES 1

namespace WPEFramework {

namespace Core {

#ifdef DEBUG_FRAMES
    static void DumpFrame(const TCHAR prefix[], const uint8_t frame[], const uint16_t length)
    {
        fprintf(stderr, "\n%s:", prefix);
        for (int teller = (8 * 3); teller < length; teller++) {
            if (teller % 8 == 0) {
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "0x%02X ", frame[teller]);
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
#endif

    /* static */ uint32_t Netlink::_sequenceId = 0;

    uint16_t Netlink::Serialize(uint8_t stream[], const uint16_t length) const
    {
        uint16_t result = 0;

        if (sizeof(struct nlmsghdr) < length) {

            memset(stream, 0, sizeof(struct nlmsghdr));

            struct nlmsghdr* message = reinterpret_cast<struct nlmsghdr*>(stream);

            _mySequence = Core::InterlockedIncrement(_sequenceId);

            size_t nlmsg_len = NLMSG_LENGTH(Write(static_cast<uint8_t*>(NLMSG_DATA(message)), length - sizeof(struct nlmsghdr)));

            message->nlmsg_len = nlmsg_len;
            message->nlmsg_type = Type();
            message->nlmsg_flags = Flags();
            message->nlmsg_seq = _mySequence;
            message->nlmsg_pid = 0; /* send to the kernel */

            result = nlmsg_len;
        }

        return (result);
    }

    uint16_t Netlink::Deserialize(const uint8_t stream[], const uint16_t streamLength)
    {
        uint16_t bytesRead = 0;
        Frames frame(stream, streamLength);
        
        if (frame.Next() == true) {

            _type = frame.Type();
            _flags = frame.Flags();
            ASSERT(_mySequence == frame.Sequence());

            // TODO: move to frame code
            _isMultimessage = _flags & NLM_F_MULTI;

            bytesRead += Read(
                frame.Payload<uint8_t>(), 
                frame.PayloadSize()
            );

            if (bytesRead != 0) {
                // Reading part of the message would be unexpected
                ASSERT(bytesRead == frame.PayloadSize());

                bytesRead += frame.HeaderSize();
                
                // Header + payload should equal frame size
                ASSERT(bytesRead == frame.RawSize());
            } 
        }

        return bytesRead;
    }

    uint32_t SocketNetlink::Send(const Core::Netlink& outbound, const uint32_t waitTime)
    {
        uint32_t result = Core::ERROR_BAD_REQUEST;

        _adminLock.Lock();

        _pending.emplace_back(
            new MessageSync(outbound)
        );

        auto myEntry = _pending.back();

        _adminLock.Unlock();

        Core::SocketDatagram::Trigger();

        if (myEntry->Wait(waitTime) == false) {
            result = Core::ERROR_RPC_CALL_FAILED;
        } else {
            result = Core::ERROR_NONE;
        }

        return (result);
    }

    uint32_t SocketNetlink::Exchange(const Core::Netlink& outbound, Core::Netlink& inbound, const uint32_t waitTime)
    {
        uint32_t result = Core::ERROR_BAD_REQUEST;

        _adminLock.Lock();

        _pending.emplace_back(
            new MessageSync(outbound, inbound)
        );

        auto& myEntry = _pending.back();

        _adminLock.Unlock();

        Core::SocketDatagram::Trigger();

        if (myEntry->Wait(waitTime) == false) {
            result = Core::ERROR_RPC_CALL_FAILED;
        } else {
            result = Core::ERROR_NONE;
        }

        return (result);
    }

    // Methods to extract and insert data into the socket buffers
    /* virtual */ uint16_t SocketNetlink::SendData(uint8_t* dataFrame, const uint16_t maxSendSize)
    {
        uint16_t result = 0;
        
        if (_pending.size() > 0) {

            _adminLock.Lock();

            // Skip all already send items
            PendingList::iterator index(_pending.begin());
            while ((index != _pending.end()) && ((*index)->IsSend() == true)) {
                index++;
            }

            if (index != _pending.end()) {
                result = (*index)->Serialize(dataFrame, maxSendSize);

                // If it's one way message, we are done with it :)
                if ((*index)->NeedResponse() == false) {
                    _pending.erase(index);
                }
            }

            _adminLock.Unlock();

#ifdef DEBUG_FRAMES
            DumpFrame("SEND", dataFrame, result);
#endif

            result = NLMSG_ALIGN(result);
        }
        return (result);
    }

    /* virtual */ uint16_t SocketNetlink::ReceiveData(uint8_t* dataFrame, const uint16_t receivedSize)
    {

#ifdef DEBUG_FRAMES
        DumpFrame("RECEIVED", dataFrame, result);
#endif

        Netlink::Frames frames(dataFrame, receivedSize);
        while (frames.Next() == true) {
            _adminLock.Lock();

            // Check if this is a response to something pending..
            PendingList::iterator index(_pending.begin());
            while ((index != _pending.end()) && ((*index)->Sequence() != frames.Sequence())) {
                index++;
            }

            uint32_t read = 0;
            if (index != _pending.end()) {
                // We found request waiting for this response!
                _adminLock.Unlock();
                read = (*index)->Deserialize(frames.RawData(), frames.RawSize());
                _adminLock.Lock();

                if ((*index)->IsProcessed()) {
                    _pending.erase(index);
                }
                _adminLock.Unlock();
            } else {
                // This message was sent without a request from our side!
                _adminLock.Unlock();
                read = this->Deserialize(frames.RawData(), frames.RawSize());
            }

            if (read == 0) {
                TRACE_L1("Failed at parsing Netlink message of type %d. Droping this frame...", frames.Type()); 
            } else {
                // We should always either read whole frame or return 0. This should never happen...
                ASSERT(read == frames.RawSize());
            }
        } 

        return receivedSize;
    }

    // Signal a state change, Opened, Closed or Accepted
    /* virtual */ void SocketNetlink::StateChange()
    {
    }
}
} // namespace WPEFramework::Core
