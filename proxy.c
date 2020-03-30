#include <assert.h>
#include <stdio.h>
#include <libgen.h>
#include <stdint.h>

#include <string.h>
#include <errno.h>

#include <unistd.h>

#include <vmci_sockets.h>

#include "sock-portable.h"

BOOL DGRAM = FALSE;
int trust_cid = 0;

char *program_name;

void print_usage_exit(void)
{
    fprintf(stderr, "usage: %s [-d] -l VSOCKET-PORT -t IP:PORT [-s SOURCE-CID]\n", program_name);
    fprintf(stderr, "  -d: use DATAGRAM mode instead of STREAM mode\n");
    fprintf(stderr, "  -l: listen vSocket PORT\n");
    fprintf(stderr, "  -t: tunnel to IP:PORT\n");
    fprintf(stderr, "  -s: trust only SOURCE-CID\n");
    exit(2);
}

enum {
//TODO: handle buffer override on recv
    BUFSIZE = 0xffff,
    CONNECTION_BACKLOG = 10,
};

int main(int argc, char **argv)
{
    unsigned int server_cid = 0, server_port = 0;
    char to_server[256] = "";
    unsigned int to_port = 0;
    int listen_fd, client_fd, nfds;
    int to_fd;

    uint64_t buf_size, t;
    socklen_t size, their_addr_size;
    struct sockaddr_vm my_addr = {0}, their_addr;
    struct sockaddr_in to_addr = {0};

    int vmci_address_family;
    fd_set read_fds;
    uint8_t buf[BUFSIZE];
    char *p;

    program_name = basename(argv[0]);

    int option;
    while((option = getopt(argc, argv, ":dl:t:s:h")) != -1){ 
        switch(option){
        case 'd':
            DGRAM = TRUE;
            break;
        case 's':
            trust_cid = (unsigned int)strtoul(optarg, &p, 10);
            break;
        case 'l':
            server_port = (unsigned int)strtoul(optarg, &p, 10);
            break;
        case 't':
            p = strtok(optarg, ":");
            strcpy(to_server, p);

            p = strtok(NULL, ":");
            to_port = strtoul(p, NULL, 10);
            break;
        case 'h':
        case '?':
            print_usage_exit();
            break;
        }
    }

    if (!server_port || !to_port) 
        print_usage_exit();

    fprintf(stderr, "Tunneling from vSocket port %u to %s:%u\n", server_port, to_server, to_port);

    socket_startup();

    if ((vmci_address_family = VMCISock_GetAFValue()) < 0) {
        fprintf(stderr, "VMCISock_GetAFValue failed: %d. You probably need root privileges\n", vmci_address_family);
        goto cleanup;
    }

    int socket_type;
    socket_type = DGRAM ? SOCK_DGRAM : SOCK_STREAM;
    int socket_proto = DGRAM ? IPPROTO_UDP : IPPROTO_TCP;

    if ((listen_fd = socket(vmci_address_family, socket_type, 0)) == -1) {
        perror("vmci socket");
        goto cleanup;
    }

    if ((to_fd = socket(AF_INET, socket_type, socket_proto)) == -1) {
        perror("ip socket");
        goto cleanup;
    }

    if (!DGRAM) { 
        /*
        * SO_VMCI_BUFFER_SIZE – Default size of communicating buffers; 65536 bytes if not set.
        * SO_VMCI_BUFFER_MIN_SIZE – Minimum size of communicating buffers; defaults to 128 bytes.
        * SO_VMCI_BUFFER_MAX_SIZE – Maximum size of communicating buffers; defaults to 262144 bytes.
        */

        buf_size = 32768;
        /* reduce buffer to above size and check */
        if (setsockopt(listen_fd, vmci_address_family, SO_VMCI_BUFFER_SIZE, (void *)&buf_size, sizeof(buf_size)) == -1) {
            perror("vmci setsockopt");
            goto close;
        }

        size = sizeof(t);
        if (getsockopt(listen_fd, vmci_address_family, SO_VMCI_BUFFER_SIZE, (void *)&t, &size) == -1) {
            perror("vmci getsockopt");
            goto close;
        }
        if (t != buf_size) {
            fprintf(stderr, "vmci SO_VMCI_BUFFER_SIZE not set to size requested.\n");
            goto close;
        }
    }

    my_addr.svm_family = vmci_address_family;
    my_addr.svm_cid = VMADDR_CID_ANY;
    my_addr.svm_port = server_port;
    if (bind(listen_fd, (struct sockaddr *) &my_addr, sizeof(my_addr)) == -1) {
        perror("vmci bind");
        goto close;
    }

    if ((server_cid = VMCISock_GetLocalCID()) == (unsigned int)-1) {
        fprintf(stderr, "VMCISock_GetLocalCID failed\n");
    } else {
        fprintf(stderr, "local cid: %u\n", server_cid);
    }

    size = sizeof(my_addr);
    if (getsockname(listen_fd, (struct sockaddr *)&my_addr, &size) == -1) {
        perror("vmci getsockname");
        goto close;
    }
//     fprintf(stderr, "listening %u:%u at %s mode\n", my_addr.svm_cid, my_addr.svm_port, DGRAM ? "DATAGRAM" : "STREAM");
    fprintf(stderr, "listening %u:%u at %s mode\n", server_cid, my_addr.svm_port, DGRAM ? "DATAGRAM" : "STREAM");

// disable buffering
//     setvbuf ( stdout , NULL , _IONBF , 1024 );

    to_addr.sin_family = AF_INET;
    to_addr.sin_addr.s_addr = inet_addr(to_server); 
    to_addr.sin_port = htons(to_port);
        
    for (;;) {
        int numbytes;
        their_addr_size = sizeof(their_addr);

        if (!DGRAM) {
        // STREAM mode
            if (listen(listen_fd, CONNECTION_BACKLOG) == -1) {
                perror("vmci listen");
                goto close;
            }

            their_addr_size = sizeof(their_addr);
            if ((client_fd = accept(listen_fd, (struct sockaddr *) &their_addr, &their_addr_size)) == -1) {
                perror("vmci accept");
                goto close;
            }
            fprintf(stderr, "client connected\n");

            FD_ZERO(&read_fds);
            FD_SET(client_fd, &read_fds);
            nfds = client_fd + 1;
            if (select(nfds, &read_fds, NULL, NULL, NULL) == -1) {
                perror("vmci select");
                goto close;
            }
            if (!FD_ISSET(client_fd, &read_fds)) {
                fprintf(stderr, "FD_ISSET failed\n");
                goto close;
            }

            numbytes = recv(client_fd, (void*)buf, sizeof(buf), 0);

            if (numbytes < 0) {
                fprintf(stderr, "recv failed: %s\n", strerror(errno));
            }

            close(client_fd);
        } else {
        // DATAGRAM mode
            numbytes = recvfrom(listen_fd, (void*)buf, sizeof(buf), 0, (struct sockaddr *) &their_addr, &their_addr_size);
            if (numbytes < 0) {
                fprintf(stderr, "recvfrom failed: %s\n", strerror(errno));
            }
        }

        if (numbytes < 0) 
            continue;

        fprintf(stderr, "received %u bytes from %u:%u\n", numbytes, their_addr.svm_cid, their_addr.svm_port);
        
        if (trust_cid && (trust_cid != their_addr.svm_cid)) {
            fprintf(stderr, "untrusted CID (%u != %u), skip data\n", trust_cid, their_addr.svm_cid);
            continue;
        }

        fwrite(buf, 1, numbytes, stdout);
        fflush(stdout);

        fprintf(stderr, "connecting to %s:%u\n", to_server, to_port);
        if (connect(to_fd, (struct sockaddr *) &to_addr, sizeof(to_addr)) == -1) {
            switch (WSAGetLastError()) {
                case WSAEALREADY:
//                     fprintf(stderr, "connect WSAEALREADY\n");
                    break;
                case WSAEISCONN:
//                     fprintf(stderr, "connect WSAEALREADY\n");
                    break;
                default:
                    perror("ip connect");
                    goto close;
            }
        }

        int to_numbytes;
        to_numbytes = send(to_fd, (char*) buf, numbytes, 0);

        if (to_numbytes == -1) {
            perror("ip send");
            goto close;
        }

        fprintf(stderr, "sended %u bytes to %s:%u\n", to_numbytes, to_server, to_port);

        if (to_numbytes != numbytes) {
            fprintf(stderr, "lost in transit, received %u bytes, sended %u bytes", numbytes, to_numbytes);
            goto close;
        }
    }
close:
    socket_close(listen_fd);
    socket_close(to_fd);
cleanup:
    socket_cleanup();
    return 0;
}
