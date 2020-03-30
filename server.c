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

char *program_name;

void print_usage_exit(void)
{
    fprintf(stderr, "usage: %s [-d] [PORT]\n", program_name);
    fprintf(stderr, "  -d: use DATAGRAM mode instead of STREAM mode\n");
    fprintf(stderr, "  no PORT means port auto selection\n");
    exit(2);
}

enum {
    BUFSIZE = 4096,
    CONNECTION_BACKLOG = 10,
};

int main(int argc, char **argv)
{
    unsigned int server_cid = 0, server_port = 0;
    int listen_fd, client_fd, nfds;
    uint64_t buf_size, t;
    socklen_t size, their_addr_size;
    struct sockaddr_vm my_addr = {0}, their_addr;
    int vmci_address_family;
    fd_set read_fds;
    uint8_t buf[BUFSIZE];
    char *p;

    program_name = basename(argv[0]);

    int option;
    while((option = getopt(argc, argv, ":dh")) != -1){ 
        switch(option){
        case 'd':
            DGRAM = TRUE;
            break;
        case 'h':
        case '?':
            print_usage_exit();
            break;
        }
    }

    for(; optind < argc; optind++){ //when some extra arguments are passed
        server_port = (unsigned int)strtoul(argv[optind], &p, 0);
    }
     
    if (server_port == 0) 
         server_port = VMADDR_PORT_ANY;

    socket_startup();

    if ((vmci_address_family = VMCISock_GetAFValue()) < 0) {
        fprintf(stderr, "VMCISock_GetAFValue failed: %d. You probably need root privileges\n", vmci_address_family);
        goto cleanup;
    }

    int socket_type;
    socket_type = DGRAM ? SOCK_DGRAM : SOCK_STREAM;

    if ((listen_fd = socket(vmci_address_family, socket_type, 0)) == -1) {
        perror("socket");
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
            perror("setsockopt");
            goto close;
        }

        size = sizeof(t);
        if (getsockopt(listen_fd, vmci_address_family, SO_VMCI_BUFFER_SIZE, (void *)&t, &size) == -1) {
            perror("getsockopt");
            goto close;
        }
        if (t != buf_size) {
            fprintf(stderr, "SO_VMCI_BUFFER_SIZE not set to size requested.\n");
            goto close;
        }
    }

    my_addr.svm_family = vmci_address_family;
    my_addr.svm_cid = VMADDR_CID_ANY;
    my_addr.svm_port = server_port;
    if (bind(listen_fd, (struct sockaddr *) &my_addr, sizeof(my_addr)) == -1) {
        perror("bind");
        goto close;
    }

    if ((server_cid = VMCISock_GetLocalCID()) == (unsigned int)-1) {
        fprintf(stderr, "VMCISock_GetLocalCID failed\n");
    } else {
        fprintf(stderr, "local cid: %u\n", server_cid);
    }

    size = sizeof(my_addr);
    if (getsockname(listen_fd, (struct sockaddr *)&my_addr, &size) == -1) {
        perror("getsockname");
        goto close;
    }
//     fprintf(stderr, "listening %u:%u at %s mode\n", my_addr.svm_cid, my_addr.svm_port, DGRAM ? "DATAGRAM" : "STREAM");
    fprintf(stderr, "listening %u:%u at %s mode\n", server_cid, my_addr.svm_port, DGRAM ? "DATAGRAM" : "STREAM");

// disable buffering
//     setvbuf ( stdout , NULL , _IONBF , 1024 );

    for (;;) {
        int numbytes;
        their_addr_size = sizeof(their_addr);

        if (!DGRAM) {
        // STREAM mode
            if (listen(listen_fd, CONNECTION_BACKLOG) == -1) {
                perror("listen");
                goto close;
            }

            their_addr_size = sizeof(their_addr);
            if ((client_fd = accept(listen_fd, (struct sockaddr *) &their_addr, &their_addr_size)) == -1) {
                perror("accept");
                goto close;
            }
            fprintf(stderr, "client connected\n");

            FD_ZERO(&read_fds);
            FD_SET(client_fd, &read_fds);
            nfds = client_fd + 1;
            if (select(nfds, &read_fds, NULL, NULL, NULL) == -1) {
                perror("select");
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

        fprintf(stderr, "received %u bytes from %u:%u\n", numbytes, their_addr.svm_cid, their_addr.svm_port);

        fwrite(buf, 1, numbytes, stdout);
        fflush(stdout);
    }
close:
    socket_close(listen_fd);
cleanup:
    socket_cleanup();
    return 0;
}
