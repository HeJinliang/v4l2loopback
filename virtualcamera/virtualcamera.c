//
// Created by liang on 2024/2/18.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <linux/memfd.h>
//#include <cutils/sockets.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <linux/videodev2.h>

#define SOCKET_PATH "obs"

#define TEST_FILE_NAME "sdcard/earth_15.yuv"
#define FRAME_WIDTH 1920
#define FRAME_HEIGHT 1080
#define FPS 30
#define FRAME_BYTES 3 * FRAME_HEIGHT * FRAME_WIDTH / 2

#define CMD_MEMFD "memfd"

char *prog;

unsigned int buffer_index = 0;

void fail(char *msg)
{
    fprintf(stderr, "%s: %s\n", prog, msg);
    exit(1);
}

long long last_write_time = 0;

/**
 * 读取帧
 * @param memfd   文件描述符
 */
void read_frames(int memfd)
{
    // 打开yuv文件
    FILE * yuv_file = fopen (TEST_FILE_NAME,"rb");
    if (yuv_file == NULL) {
        fail("can not open yuv file");
    }

    char *frame = malloc(FRAME_BYTES);

    if (frame == NULL) {
        fail("cannot malloc frame");
    }

    // 死循环，不断的读取yuv文件，然后写入到共享内存中
    while (1) {
        int read_size = fread(frame, 1, FRAME_BYTES, yuv_file);

        struct timeval tv;
        gettimeofday(&tv, NULL);
        long long microseconds = tv.tv_sec*1000000LL + tv.tv_usec; // calculate microseconds

        if (last_write_time != 0) {
            long long lost_time = microseconds - last_write_time;
            printf("microseconds_0: %lld    lost_time: %lld\n", microseconds, lost_time);

            if (lost_time > 0) {
                if (lost_time < 1.0f/FPS * 1000000.0f) {
                    usleep(1.0f/FPS * 1000000.0f - lost_time);
                }
            } else {
                usleep(1.0f/FPS * 1000000.0f);
            }
        }


        struct timeval tv_2;
        gettimeofday(&tv_2, NULL);
        long long microseconds_2 = tv_2.tv_sec*1000000LL + tv_2.tv_usec; // calculate microseconds

        last_write_time = microseconds_2;

        if (read_size == FRAME_BYTES) {
            // 加入帧下标
            write(memfd, &buffer_index, sizeof(unsigned int));
            // 将 frame 写入到共享内存中
            write(memfd, frame, FRAME_BYTES);
            // 将文件描述符的位置设置到文件的开始，否则 obs 无法读取到有效数据
            lseek(memfd, 0, SEEK_SET);

            buffer_index++;

        } else if (read_size == 0) {
            fclose(yuv_file);
            yuv_file = fopen (TEST_FILE_NAME,"rb");
        } else {
            free(frame);
            fail("invalid frame size or file ending");
        }
    }
    // 释放内存
    free(frame);
}



int create_memfd(size_t size) {
    // 创建一个匿名的内存文件
    int fd = memfd_create("camera_memfd", MFD_CLOEXEC);
    if (fd == -1) {
        perror("memfd_create");
        return -1;
    }

    // 设置文件的大小
    if (ftruncate(fd, size) == -1) {
        perror("ftruncate");
        close(fd);
        return -1;
    }

    return fd;
}


void send_memfd(int memfd) {
    int sock;
    struct sockaddr_un addr;

    // 创建UNIX域套接字
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/dev/socket/obs", sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return;
    }

    // 发送共享内存的文件描述符
    struct msghdr msg = {0};
    struct cmsghdr *cmsg;
    char buf[CMSG_SPACE(sizeof(int))];
    memset(buf, '\0', sizeof(buf));

    struct iovec io = {.iov_base = "memfd", .iov_len = 5};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);

    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));

    *((int *) CMSG_DATA(cmsg)) = memfd;

    if (sendmsg(sock, &msg, 0) < 0) {
        perror("sendmsg");
        close(sock);
        exit(1);
    }

    close(sock);

}

//void send_memfd(int memfd) {
//    int sock;
//
//    // 创建UNIX域套接字
//    sock = socket_local_client(SOCKET_PATH, ANDROID_SOCKET_NAMESPACE_RESERVED, SOCK_STREAM);
//    if (sock < 0) {
//        perror("socket_local_client");
//        return;
//    }
//
//    // 发送共享内存的文件描述符
//    struct msghdr msg = {0};
//
//    struct cmsghdr *cmsg;
//    // 定义一个空的char数组，用于存放辅助数据消息 (此处是共享内存的文件描述符)
//    char buf[CMSG_SPACE(sizeof(int))];
//    memset(buf, '\0', sizeof(buf));
//
//    struct iovec io = { .iov_base = CMD_MEMFD, .iov_len = strlen(CMD_MEMFD) };
//    msg.msg_iov = &io;
//    msg.msg_iovlen = 1;
//    msg.msg_control = buf;
//    msg.msg_controllen = sizeof(buf);
//
//    cmsg = CMSG_FIRSTHDR(&msg); // 获取msg结构体的第一个辅助数据消息的头部
//    cmsg->cmsg_level = SOL_SOCKET; // 级别：设置辅助数据消息的级别为SOL_SOCKET，
//    cmsg->cmsg_type = SCM_RIGHTS; // 类型：设置辅助数据消息的类型为SCM_RIGHTS，表示这个消息包含了要传递的文件描述符。
//    cmsg->cmsg_len = CMSG_LEN(sizeof(int)); // 长度
//
//    // 同一个共享内存的文件描述符在不同进程里有不同的int值，所以不能直接传递 memfd 值。这是通过使用类型为SCM_RIGHTS的辅助数据消息完成的， 可以在消息的数据部分包含一个或多个文件描述符。当接收方接收到这个消息时，它会得到一些新的文件描述符，这些文件描述符指向与发送方的文件描述符相同的文件。
//    *((int *) CMSG_DATA(cmsg)) = memfd; // 这里其实是将memfd写入到msg.msg_control中
//
//    if (sendmsg(sock, &msg, 0) < 0) {
//        perror("sendmsg");
//        exit(1);
//    }
//
//    close(sock);
//}

//void send_socket_msg(char *cmd) {
//    int sock;
//
//    sock = socket_local_client(SOCKET_PATH, ANDROID_SOCKET_NAMESPACE_RESERVED, SOCK_STREAM);
//    if (sock < 0) {
//        perror("socket_local_client");
//        return;
//    }
//
//    struct msghdr msg = {0};
//    struct iovec io;
//
//    io.iov_base = cmd;
//    io.iov_len = strlen(cmd);
//    msg.msg_iov = &io;
//    msg.msg_iovlen = 1;
//    if (sendmsg(sock, &msg, 0) < 0) {
//        perror("sendmsg");
//    }
//    // 打印出 cmd
//    printf("send_socket_msg: %s\n", cmd);
//
//    close(sock);
//}

int main(int argc, char **argv) {
    prog = argv[0];
    buffer_index = 0;

    // 共享内存里 数据：帧下标 + 帧数据
    int memfd = create_memfd(sizeof(unsigned int) + FRAME_BYTES);
    if (memfd < 0) {
        return 1;
    }

    // 通过socket将共享内存描述符传给obs
    send_memfd(memfd);

    // 调用 read_frames 函数将读取到的frame buffer通过共享内存 memfd 给到 obs
    read_frames(memfd);

//    send_socket_msg("start");





    close(memfd);

    return 0;
}