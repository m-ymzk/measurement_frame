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

static uint64_t now_ns_100(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec) / 100ULL;
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

    int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock < 0)
    {
        perror("socket");
        free(file_data);
        return 1;
    }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = if_nametoindex(ifname);
    sll.sll_halen = ETH_ALEN;
    memcpy(sll.sll_addr, dst_mac, ETH_ALEN);

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

    for (int shot = 0; shot < num_shot; ++shot)
    {
        uint64_t start_ns100 = now_ns_100();
        size_t offset = (size_t)((shot * total_payload_size) % file_len);
        size_t frame_len = header_len;

        memset(frame, 0, sizeof(frame));
        memcpy(frame, dst_mac, 6);
        memcpy(frame + 6, src_mac, 6);
        *(uint16_t *)(frame + 12) = htons((uint16_t)ETHERTYPE_MFF);
        *(uint16_t *)(frame + 14) = htons((uint16_t)VERSION_MFF);
        *(uint64_t *)(frame + 16) = host_to_be64(start_ns100);
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
        }

        ssize_t sent = sendto(sock, frame, frame_len, 0, (struct sockaddr *)&sll, sizeof(sll));
        if (sent < 0)
        {
            perror("sendto");
            close(sock);
            free(file_data);
            return 1;
        }

        if (verbose)
        {
            uint64_t end_ns100 = now_ns_100();
            uint64_t latency = end_ns100 - start_ns100;
            if (latency_count < sizeof(latencies) / sizeof(latencies[0]))
            {
                latencies[latency_count++] = latency;
            }
        }
    }

    if (verbose && latency_count > 0)
    {
        uint64_t sum = 0;
        uint64_t maxv = 0;
        uint64_t minv = ~0ULL;
        for (size_t i = 0; i < latency_count; ++i)
        {
            uint64_t v = latencies[i];
            sum += v;
            if (v > maxv)
                maxv = v;
            if (v < minv)
                minv = v;
        }
        double avg = (double)sum / (double)latency_count;
        double var = 0.0;
        for (size_t i = 0; i < latency_count; ++i)
        {
            double d = (double)latencies[i] - avg;
            var += d * d;
        }
        var /= (double)latency_count;
        double sigma = sqrt(var);
        printf("Average latency: %.3f us, sigma %.3f us\n", avg / 10.0, sigma / 10.0);
        printf("Max latency: %.3f us\n", (double)maxv / 10.0);
        printf("Min latency: %.3f us\n", (double)minv / 10.0);
    }

    close(sock);
    free(file_data);
    return 0;
}
