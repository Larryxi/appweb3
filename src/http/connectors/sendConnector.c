/*
 *  sendConnector.c -- Send file connector. 
 *
 *  The Sendfile connector supports the optimized transmission of whole static files. It uses operating system 
 *  sendfile APIs to eliminate reading the document into user space and multiple socket writes. The send connector 
 *  is not a general purpose connector. It cannot handle dynamic data or ranged requests. It does support chunked requests.
 *
 *  Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

#if BLD_FEATURE_SEND

/**************************** Forward Declarations ****************************/

static void addPacketForSend(MaQueue *q, MaPacket *packet);
static void adjustSendVec(MaQueue *q, int written);
static int  buildSendVec(MaQueue *q);
static void freeSentPackets(MaQueue *q, int written);

/*********************************** Code *************************************/
/*
 *  Invoked to initialize the send connector for a request
 */
static void sendOpen(MaQueue *q)
{
    MaConn          *conn;
    MaResponse      *resp;

    conn = q->conn;
    resp = conn->response;

    /*
     *  To write an entire file, reset the maximum and packet size to the maximum response body size (LimitResponseBody)
     */
    q->max = conn->http->limits.maxResponseBody;
    q->packetSize = conn->http->limits.maxResponseBody;

    if (!conn->requestFailed && !(resp->flags & MA_RESP_NO_BODY)) {
        resp->file = mprOpen(q, resp->filename, O_RDONLY | O_BINARY, 0);
        if (resp->file == 0) {
            maFailRequest(conn, MPR_HTTP_CODE_NOT_FOUND, "Can't open document: %s", resp->filename);
        }
    }
}


/*
 *  Outgoing data service routine. May be called multiple times.
 */
static void sendOutgoingService(MaQueue *q)
{
    MaConn      *conn;
    MaResponse  *resp;
    int         written, ioCount, errCode;

    conn = q->conn;
    resp = conn->response;

    /*
     *  Loop doing non-blocking I/O until blocked or all the packets received are written.
     */
    while (1) {
        /*
         *  Rebuild the iovector only when the past vector has been completely written. Simplifies the logic quite a bit.
         */
        written = 0;
        if (q->ioIndex == 0 && buildSendVec(q) <= 0) {
            break;
        }
        /*
         *  Write the vector and file data. Exclude the file entry in the io vector.
         */
        ioCount = q->ioIndex - q->ioFileEntry;
        mprAssert(ioCount >= 0);
        written = (int) mprSendFileToSocket(conn->sock, resp->file, resp->pos, q->ioCount, q->iovec, ioCount, NULL, 0);
        mprLog(q, 5, "Send connector written %d", written);
        if (written < 0) {
            errCode = mprGetError();
            if (errCode == EAGAIN || errCode == EWOULDBLOCK) {
                break;
            }
            if (errCode == EPIPE || errCode == ECONNRESET) {
                maDisconnectConn(conn);
            } else {
                mprLog(conn, 7, "mprSendFileToSocket failed, errCode %d", errCode);
                maDisconnectConn(conn);
            }
            freeSentPackets(q, MAXINT);
            break;

        } else if (written == 0) {
            /* Socket is full. Wait for an I/O event */
            maRequestWriteBlocked(conn);
            break;

        } else if (written > 0) {
            resp->bytesWritten += written;
            freeSentPackets(q, written);
            adjustSendVec(q, written);
        }
    }
    if (q->ioCount == 0 && q->flags & MA_QUEUE_EOF) {
        maCompleteRequest(conn);
    }
}


/*
 *  Build the IO vector. This connector uses the send file API which permits multiple IO blocks to be written with 
 *  file data. This is used to write transfer the headers and chunk encoding boundaries. Return the count of bytes to 
 *  be written. Return -1 for EOF.
 */
static int buildSendVec(MaQueue *q)
{
    MaConn      *conn;
    MaResponse  *resp;
    MaPacket    *packet;

    conn = q->conn;
    resp = conn->response;

    mprAssert(q->ioIndex == 0);
    q->ioCount = 0;
    q->ioFileEntry = 0;

    /*
     *  Examine each packet and accumulate as many packets into the I/O vector as possible. Can only have one data packet at
     *  a time due to the limitations of the sendfile API (on Linux). And the data packet must be after all the 
     *  vector entries. Leave the packets on the queue for now, they are removed after the IO is complete for the 
     *  entire packet.
     */
    for (packet = q->first; packet && !q->ioFileEntry; packet = packet->next) {
        if (packet->flags & MA_PACKET_HEADER) {
            maFillHeaders(conn, packet);
            q->count += maGetPacketLength(packet);

        } else if (maGetPacketLength(packet) == 0) {
            /*
             *  This is the end of file packet. If chunking, we must still add this to the vector as we need to emit 
             *  a trailing chunk termination line.
             */
            q->flags |= MA_QUEUE_EOF;
            if (packet->prefix == NULL) {
                break;
            }

        } else if (resp->flags & MA_RESP_NO_BODY) {
            maCleanQueue(q);
            continue;
        }
        if (q->ioIndex >= (MA_MAX_IOVEC - 2)) {
            break;
        }
        addPacketForSend(q, packet);
    }
    return q->ioCount;
}


/*
 *  Add one entry to the io vector
 */
static void addToSendVector(MaQueue *q, char *ptr, int bytes)
{
    mprAssert(bytes > 0);

    q->iovec[q->ioIndex].start = ptr;
    q->iovec[q->ioIndex].len = bytes;
    q->ioCount += bytes;
    q->ioIndex++;
}


/*
 *  Add a packet to the io vector. Return the number of bytes added to the vector.
 */
static void addPacketForSend(MaQueue *q, MaPacket *packet)
{
    MaResponse  *resp;
    MaConn      *conn;
    MprIOVec    *iovec;
    int         mask;

    conn = q->conn;
    resp = conn->response;
    iovec = q->iovec;
    
    mprAssert(q->count >= 0);
    mprAssert(q->ioIndex < (MA_MAX_IOVEC - 2));

    if (packet->prefix) {
        addToSendVector(q, mprGetBufStart(packet->prefix), mprGetBufLength(packet->prefix));
    }
    if (maGetPacketLength(packet) > 0) {
        /*
         *  Header packets have actual content. File data packets are virtual and only have a count.
         */
        if (packet->content) {
            addToSendVector(q, mprGetBufStart(packet->content), mprGetBufLength(packet->content));

        } else {
            addToSendVector(q, 0, maGetPacketLength(packet));
            mprAssert(q->ioFileEntry == 0);
            q->ioFileEntry = 1;
            q->ioFileOffset += maGetPacketLength(packet);
        }
    }
    mask = (packet->flags & MA_PACKET_HEADER) ? MA_TRACE_HEADERS : MA_TRACE_BODY;
    if (maShouldTrace(conn, mask)) {
        maTraceContent(conn, packet, 0, resp->bytesWritten, mask);
    }
}


/*
 *  Clear entries from the IO vector that have actually been transmitted. This supports partial writes due to the socket
 *  being full. Don't come here if we've seen all the packets and all the data has been completely written. ie. small files
 *  don't come here.
 */
static void freeSentPackets(MaQueue *q, int bytes)
{
    MaPacket    *packet;
    int         len;

    mprAssert(q->first);
    mprAssert(q->count >= 0);
    mprAssert(bytes >= 0);

    while ((packet = q->first) != 0) {
        if (packet->prefix) {
            len = mprGetBufLength(packet->prefix);
            len = min(len, bytes);
            mprAdjustBufStart(packet->prefix, len);
            bytes -= len;
            /* Prefixes dont' count in the q->count. No need to adjust */
            if (mprGetBufLength(packet->prefix) == 0) {
                packet->prefix = 0;
            }
        }
        if ((len = maGetPacketLength(packet)) > 0) {
            len = min(len, bytes);
            if (packet->content) {
                mprAdjustBufStart(packet->content, len);
            } else {
                packet->entityLength -= len;
            }
            bytes -= len;
            q->count -= len;
            mprAssert(q->count >= 0);
        }
        if (maGetPacketLength(packet) == 0) {
            if ((packet = maGet(q)) != 0) {
                maFreePacket(q, packet);
            }
        }
        mprAssert(bytes >= 0);
        if (bytes == 0) {
            break;
        }
    }
}


/*
 *  Clear entries from the IO vector that have actually been transmitted. This supports partial writes due to the socket
 *  being full. Don't come here if we've seen all the packets and all the data has been completely written. ie. small files
 *  don't come here.
 */
static void adjustSendVec(MaQueue *q, int written)
{
    MprIOVec    *iovec;
    MaResponse  *resp;
    int         i, j, len;

    resp = q->conn->response;

    /*
     *  Cleanup the IO vector
     */
    if (written == q->ioCount) {
        /*
         *  Entire vector written. Just reset.
         */
        q->ioIndex = 0;
        q->ioCount = 0;
        resp->pos = q->ioFileOffset;

    } else {
        /*
         *  Partial write of an vector entry. Need to copy down the unwritten vector entries.
         */
        q->ioCount -= written;
        mprAssert(q->ioCount >= 0);
        iovec = q->iovec;
        for (i = 0; i < q->ioIndex; i++) {
            len = (int) iovec[i].len;
            if (iovec[i].start) {
                if (written < len) {
                    iovec[i].start += written;
                    iovec[i].len -= written;
                    break;
                } else {
                    written -= len;
                }
            } else {
                /*
                 *  File data has a null start ptr
                 */
                resp->pos += written;
                q->ioIndex = 0;
                q->ioCount = 0;
                return;
            }
        }

        /*
         *  Compact
         */
        for (j = 0; i < q->ioIndex; ) {
            iovec[j++] = iovec[i++];
        }
        q->ioIndex = j;
    }
}


int maOpenSendConnector(MaHttp *http)
{
    MaStage     *stage;

    stage = maCreateConnector(http, "sendConnector", MA_STAGE_ALL);
    if (stage == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    stage->open = sendOpen;
    stage->outgoingService = sendOutgoingService; 
    http->sendConnector = stage;
    return 0;
}


#else
void __dummySendConnector() {}
#endif /* BLD_FEATURE_SEND */

/*
 *  @copy   default
 *  
 *  Copyright (c) Embedthis Software LLC, 2003-2011. All Rights Reserved.
 *  Copyright (c) Michael O'Brien, 1993-2011. All Rights Reserved.
 *  
 *  This software is distributed under commercial and open source licenses.
 *  You may use the GPL open source license described below or you may acquire 
 *  a commercial license from Embedthis Software. You agree to be fully bound 
 *  by the terms of either license. Consult the LICENSE.TXT distributed with 
 *  this software for full details.
 *  
 *  This software is open source; you can redistribute it and/or modify it 
 *  under the terms of the GNU General Public License as published by the 
 *  Free Software Foundation; either version 2 of the License, or (at your 
 *  option) any later version. See the GNU General Public License for more 
 *  details at: http://www.embedthis.com/downloads/gplLicense.html
 *  
 *  This program is distributed WITHOUT ANY WARRANTY; without even the 
 *  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
 *  
 *  This GPL license does NOT permit incorporating this software into 
 *  proprietary programs. If you are unable to comply with the GPL, you must
 *  acquire a commercial license to use this software. Commercial licenses 
 *  for this software and support services are available from Embedthis 
 *  Software at http://www.embedthis.com 
 *  
 *  Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */
