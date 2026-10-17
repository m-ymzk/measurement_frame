#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <linux/if.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef IF_NAMESIZE
#define IF_NAMESIZE IFNAMSIZ
#endif

#define ETHERTYPE_MFF 0x3434U
#define VERSION_MFF 0x0002U
#define DEVICE_ID_DEFAULT 0x00000001U
#define INFO_NUM_DEFAULT 1U
#define SRC_MAC_FIXED 0x111122223333ULL

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -i <interface> -d <dst-mac> [-s <num-shot>] [-c <code-distance>] [-n <info-num>] [-v]\n"
            "  -i  interface name\n"
            "  -d  destination MAC address (xx:xx:xx:xx:xx:xx)\n"
            "  -s  number of shots to send (default: 1)\n"
            "  -c  code distance (default: 3)\n"
            "  -n  number of info bits (default: 1)\n"
            "  -v  print per-shot latency summary\n",
            prog);
}

static void parse_mac(const char *mac_str, uint8_t out[6])
{
    unsigned int v[6];
    int got = sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
                     &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    if (got != 6)
    {
        fprintf(stderr, "invalid MAC address: %s\n", mac_str);
        exit(1);
    }
    for (int i = 0; i < 6; ++i)
    {
        out[i] = (uint8_t)v[i];
    }
}

static void get_interface_mac(const char *ifname, uint8_t out[6])
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        perror("socket");
        exit(1);
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);

    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0)
    {
        perror("ioctl SIOCGIFHWADDR");
        close(fd);
        exit(1);
    }
    close(fd);

    memcpy(out, ifr.ifr_hwaddr.sa_data, 6);
}

static int read_file_all(const char *path, uint8_t **buf, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        perror("fopen");
        return -1;
    }

    if (fseek(fp, 0, SEEK_END) != 0)
    {
        perror("fseek");
        fclose(fp);
        return -1;
    }

    long size = ftell(fp);
    if (size < 0)
    {
        perror("ftell");
        fclose(fp);
        return -1;
    }
    rewind(fp);

    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data && size != 0)
    {
        perror("malloc");
        fclose(fp);
        return -1;
    }

    size_t got = fread(data, 1, (size_t)size, fp);
    if (got != (size_t)size)
    {
        fprintf(stderr, "failed to read entire file: %s\n", path);
        free(data);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    *buf = data;
    *len = (size_t)size;
    return 0;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec) * 1ULL;
}

static uint64_t host_to_be64(uint64_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(x);
#else
    return x;
#endif
}

int main(int argc, char **argv)
{
    char ifname[IF_NAMESIZE] = {0};
    char dst_mac_str[32] = {0};
    int num_shot = 1;
    int code_distance = 3;
    int info_num = INFO_NUM_DEFAULT;
    int verbose = 0;

    int ch;
    while ((ch = getopt(argc, argv, "i:d:s:c:n:v")) != -1)
    {
        switch (ch)
        {
        case 'i':
            snprintf(ifname, sizeof(ifname), "%s", optarg);
            break;
        case 'd':
            snprintf(dst_mac_str, sizeof(dst_mac_str), "%s", optarg);
            break;
        case 's':
            num_shot = atoi(optarg);
            break;
        case 'c':
            code_distance = atoi(optarg);
            break;
        case 'n':
            info_num = atoi(optarg);
            break;
        case 'v':
            verbose = 1;
            break;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (ifname[0] == '\0' || dst_mac_str[0] == '\0')
    {
        print_usage(argv[0]);
        return 1;
    }

    if (num_shot <= 0)
    {
        fprintf(stderr, "num-shot must be positive\n");
        return 1;
    }
    if (code_distance <= 0)
    {
        fprintf(stderr, "code-distance must be positive\n");
        return 1;
    }
    if (info_num <= 0)
    {
        fprintf(stderr, "info-num must be positive\n");
        return 1;
    }

    uint8_t dst_mac[6];
    uint8_t src_mac[6];
    parse_mac(dst_mac_str, dst_mac);
    get_interface_mac(ifname, src_mac);

    int num_qubits = 2 * (code_distance * code_distance) - 1;
    size_t payload_bytes = (size_t)((num_qubits + 7) / 8);
    size_t total_payload_size = payload_bytes * (size_t)info_num;

    uint8_t *file_data = NULL;
    size_t file_len = 0;
    if (read_file_all("rand_record.bin", &file_data, &file_len) != 0)
    {
        return 1;
    }
    if (file_len == 0)
    {
        fprintf(stderr, "rand_record.bin is empty\n");
        free(file_data);
        return 1;
    }
    if (file_len < total_payload_size)
    {
        fprintf(stderr,
                "rand_record.bin is too short: need %zu bytes, got %zu bytes\n",
                total_payload_size, file_len);
        free(file_data);
        return 1;
    }

    int sock = socket(AF_PACKET, SOCK_RAW, htons(ETHERTYPE_MFF));
    if (sock < 0)
    {
        perror("socket");
        free(file_data);
        return 1;
    }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = 0;
    sll.sll_ifindex = if_nametoindex(ifname);
    if (sll.sll_ifindex == 0)
    {
        fprintf(stderr, "interface not found: %s\n", ifname);
        close(sock);
        free(file_data);
        return 1;
    }
    if (bind(sock, (struct sockaddr *)&sll, sizeof(sll)) < 0)
    {
        perror("bind");
        close(sock);
        free(file_data);
        return 1;
    }

    uint8_t frame[1518];
    size_t header_len = 6 + 6 + 2 + 2 + 8 + 4 + 4 + 4;
    size_t max_frame_len = header_len + total_payload_size;
    if (max_frame_len > sizeof(frame))
    {
        fprintf(stderr, "frame too large for local buffer: %zu > %zu\n", max_frame_len, sizeof(frame));
        close(sock);
        free(file_data);
        return 1;
    }

    uint64_t latencies[1024];
    size_t latency_count = 0;

    uint64_t last_send_time = now_ns();

    for (int shot = 0; shot < num_shot; ++shot)
    {
        uint64_t start_ns = now_ns();
        size_t offset = (size_t)((shot * total_payload_size) % file_len);
        size_t frame_len = header_len;

        memcpy(frame, dst_mac, 6);
        memcpy(frame + 6, src_mac, 6);
        *(uint16_t *)(frame + 12) = htons((uint16_t)ETHERTYPE_MFF);
        *(uint16_t *)(frame + 14) = htons((uint16_t)VERSION_MFF);
        *(uint64_t *)(frame + 16) = host_to_be64(start_ns);
        *(uint32_t *)(frame + 24) = htonl((uint32_t)DEVICE_ID_DEFAULT);
        *(uint32_t *)(frame + 28) = htonl((uint32_t)num_qubits);
        *(uint32_t *)(frame + 32) = htonl((uint32_t)info_num);

        size_t remain = total_payload_size;
        size_t p = 0;
        while (remain > 0)
        {
            size_t chunk = remain;
            size_t src_left = file_len - offset;
            if (chunk > src_left)
            {
                chunk = src_left;
            }
            memcpy(frame + header_len + p, file_data + offset, chunk);
            offset += chunk;
            p += chunk;
            remain -= chunk;
            if (offset >= file_len)
            {
                offset = 0;
            }
        }
        frame_len = header_len + total_payload_size;

        if (frame_len < 60)
        {
            frame_len = 60;
            memset(frame + header_len + total_payload_size, 0,
                   frame_len - header_len - total_payload_size);
        }

        {
            uint64_t elapsed_ns = now_ns() - last_send_time;
            if (elapsed_ns < 1000)
            {
                printf("# shot %d: elapsed time %llu ns, waiting for 1000 ns\n", shot, (unsigned long long)elapsed_ns);
            }
            while (now_ns() - last_send_time < 1000)
            {
            }
        }
        last_send_time = now_ns();

        ssize_t sent = send(sock, frame, frame_len, 0);
        if (sent < 0)
        {
            perror("sendto");
            close(sock);
            free(file_data);
            return 1;
        }

        if (verbose)
        {
            uint64_t end_ns = now_ns();
            uint64_t latency = end_ns - start_ns;
            uint64_t send_time = end_ns - last_send_time;
            if (latency_count < sizeof(latencies) / sizeof(latencies[0]))
            {
                latencies[latency_count++] = latency;
            }
            printf("# shot %d: latency %llu ns, send time %llu ns\n", shot, (unsigned long long)latency, (unsigned long long)send_time);
        }
    }

    if (verbose && latency_count > 2)
    {
        uint64_t sum = 0;
        uint64_t maxv = 0;
        uint64_t minv = ~0ULL;
        for (size_t i = 2; i < latency_count; ++i)
        {
            uint64_t v = latencies[i];
            sum += v;
            if (v > maxv)
                maxv = v;
            if (v < minv)
                minv = v;
        }
        double avg = (double)sum / (double)(latency_count - 2);
        double var = 0.0;
        for (size_t i = 2; i < latency_count; ++i)
        {
            double d = (double)latencies[i] - avg;
            var += d * d;
        }
        var /= (double)(latency_count - 2);
        double sigma = sqrt(var);
        printf("Average latency: %.1f +- %.1f ns\n", avg, sigma);
        printf("Max latency: %.1f ns\n", (double)maxv);
        printf("Min latency: %.1f ns\n", (double)minv);
    }

    close(sock);
    free(file_data);
    return 0;
}
