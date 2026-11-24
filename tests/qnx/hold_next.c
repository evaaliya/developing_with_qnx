/*
 * Test tool for the guest (F-033). Registers a name as the next node,
 * answers every message until the first URGENT, takes that URGENT and
 * then stops: no reply, no more receives, as a relay stopped by SIGSTOP
 * between MsgReceive and MsgReply. The sender of that URGENT waits for
 * a reply that never comes, and its send time limit can only ask this
 * process to unblock it (F-028). Every later sender waits to be
 * received and is freed by its own time limit.
 *   hold-next NAME
 */
#define _QNX_SOURCE

#include "signal_msg.h"
#include "wire.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>

int main(int argc, char **argv)
{
    name_attach_t *attach;
    static union {
        struct _pulse pulse;
        uint16_t type;
        struct signal_message message;
        struct signal_batch batch;
        struct signal_close close;
        struct signal_manifest manifest;
    } buffer;

    if (argc != 2 || argv[1][0] == '\0') {
        fprintf(stderr, "usage: hold-next NAME\n");
        return EXIT_FAILURE;
    }
    attach = name_attach(NULL, argv[1], 0);
    if (attach == NULL) {
        fprintf(stderr, "hold-next: could not register %s: %s\n", argv[1],
                strerror(errno));
        return EXIT_FAILURE;
    }
    printf("hold-next: registered %s\n", argv[1]);
    fflush(stdout);

    for (;;) {
        struct _msg_info info;
        rcvid_t rcvid;

        rcvid = MsgReceive(attach->chid, &buffer, sizeof buffer, &info);
        if (rcvid == -1) {
            fprintf(stderr, "hold-next: receive failed: %s\n", strerror(errno));
            return EXIT_FAILURE;
        }
        if (rcvid == 0) {
            if (buffer.pulse.code == _PULSE_CODE_DISCONNECT) {
                (void)ConnectDetach(buffer.pulse.scoid);
            }
            continue;
        }
        if (info.msglen < (ssize_t)sizeof(uint16_t) || buffer.type <= _IO_MAX) {
            (void)MsgError(rcvid, ENOSYS);
            continue;
        }
        if (buffer.type == SIGNAL_MSG_TYPE &&
            info.msglen >= (ssize_t)sizeof(struct signal_message) &&
            buffer.message.body.class_id == SIGNAL_CLASS_URGENT) {
            printf("hold-next: holding URGENT %" PRIu64
                   " without a reply; receiving nothing more\n",
                   buffer.message.body.number);
            fflush(stdout);
            for (;;) {
                pause();
            }
        }
        if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
            fprintf(stderr, "hold-next: reply failed: %s\n", strerror(errno));
        }
    }
}
