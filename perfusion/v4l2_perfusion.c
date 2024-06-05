/* -*- c-file-style: "linux" -*- */
/*
 * v4l2_perfusion  --  obs 灌流服务
 *
 * Copyright (C) 2020-2023 IOhannes m zmoelnig (zmoelnig@iem.at)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <linux/memfd.h>
#include <cutils/sockets.h>
#include <pthread.h>
#include <android/log.h>

#include <sys/types.h>
#include <sys/stat.h>

#define LOG_TAG "obs"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)


#define FRAME_WIDTH 1920
#define FRAME_HEIGHT 1080
#define FPS 30
#define FRAME_BYTES 3 * FRAME_HEIGHT * FRAME_WIDTH / 2


#define DEFAULT_FILE_NAME "/data/local/tmp/df_frame"

#define SOCKET_PATH "obs"

#define CMD_MEMFD "memfd"

#define LOG_DEBUG 1

char *default_frame_buf = NULL;

char *prog;

int camera_fd[5] = {-1, -1, -1, -1, -1};  // 5个视频设备的文件描述符

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;  // 定义一个全局的互斥锁

pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
int ready = 0;

// 全局变量，用于在两个线程之间共享文件描述符
int global_memfd_fd = -1;

unsigned int last_frame_index = 0;
unsigned int lost_frame_count = 0;
#define LOST_MAX_FRAME_COUNT 30*5 // 连续5秒丢帧就直接使用默认图 (ANR一般5秒)

//void fail(char *msg)
//{
//    fprintf(stderr, "%s: %s\n", prog, msg);
//}



/**
 * 打开视频设备
 * @param device    视频设备的路径
 * @return  返回视频设备的文件描述符
 */
int open_video(char *device)
{
    struct v4l2_format v;


    int dev_fd = open(device, O_RDWR);
    if (dev_fd == -1) {
        LOGE("cannot open video device");
    }
    v.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(dev_fd, VIDIOC_G_FMT, &v) == -1) {
        LOGE("cannot setup video device");
    }
    v.fmt.pix.width = FRAME_WIDTH;
    v.fmt.pix.height = FRAME_HEIGHT;
    v.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    v.fmt.pix.sizeimage = FRAME_BYTES;
    v.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(dev_fd, VIDIOC_S_FMT, &v) == -1) {
        LOGE("cannot setup video device");
    }

    return dev_fd;
}

int fd = -1;

/**
 * 将帧写入视频设备中
 * @param buffer
 */
void write_frames(char *buffer) {
    // 遍历 camera_fd ，如果大于等于0，则调用 write 函数写入帧数据


//    if (fd == -1) {
//        fd = open_video("/dev/video30");
//    }
//
//    write(fd, buffer, FRAME_BYTES);

    pthread_mutex_lock(&lock);
    // 打印日志
    for (int i = 0; i < 5; i++) {
        // 打印出 camera_fd[i] 的值
        if (camera_fd[i] >= 0) {
            // 打印日志

            // 打印日志
            LOGI("v4l2_perfusion:  write frame to video device %d\n", i);

            write(camera_fd[i], buffer, FRAME_BYTES);
        }
    }
    pthread_mutex_unlock(&lock);
}


void write_default_frame() {
    if (default_frame_buf == NULL) {
        // 打开yuv文件
        FILE * yuv_file = fopen (DEFAULT_FILE_NAME,"rb");

        if (yuv_file != NULL) {
            default_frame_buf = malloc(FRAME_BYTES);
            if (default_frame_buf != NULL) {
                fread(default_frame_buf, 1, FRAME_BYTES, yuv_file);
            } else {
                LOGE("cannot malloc frame");
            }
        } else {

            // 日志带上 DEFAULT_FILE_NAME
            LOGE("can not open yuv file %s", DEFAULT_FILE_NAME);


//            LOGE("can not open yuv file");
        }
    }

    if (default_frame_buf != NULL) {
        write_frames(default_frame_buf);
    }
}


uint64_t last_end_copy_time = 0;// 上次完成frame处理的时间戳
uint32_t last_sleep_time = 0;// 上次sleep时间
uint32_t last_sleep_lost_time = 0;// 上次执行 sleep 函数本身耗费的时间 （不算传入sleep的时间）


/**
 * 从共享内存中拷贝帧
 * @param setup     视频设备
 * @param dev_fd    共享内存的文件描述符
 */
void copy_frames()
{
    // 使用mmap函数来映射memfd到你的进程的地址空间
    void *remote_data = MAP_FAILED;

    int old_memfd = -1;

    while (1) {
        struct timeval tv;
        // 这里获取处理frame之前的时间戳
        gettimeofday(&tv, NULL);
        uint64_t start_copy_time = tv.tv_sec * 1000000 + tv.tv_usec;

        pthread_mutex_lock(&lock);
        if (global_memfd_fd > 0) {
            // 当文件描述符发生变化时，解除映射
            if (old_memfd != global_memfd_fd && remote_data != MAP_FAILED) {
                munmap(remote_data, sizeof(unsigned int) + FRAME_BYTES);
                remote_data = MAP_FAILED;
            }
            if (remote_data == MAP_FAILED) {
                remote_data = mmap(NULL, sizeof(unsigned int) + FRAME_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, global_memfd_fd, 0);

                last_frame_index = 0;
            }
        } else {
            if (remote_data != MAP_FAILED) {
#if LOG_DEBUG
                LOGI("v4l2_perfusion:  unmap memfd\n");
#endif
                munmap(remote_data, sizeof(unsigned int) + FRAME_BYTES);
                remote_data = MAP_FAILED;
            }
        }
        old_memfd = global_memfd_fd;

        pthread_mutex_unlock(&lock);

        if (remote_data == MAP_FAILED) {
            // 写入固定一张图到视频设备中
#if LOG_DEBUG
            LOGI("v4l2_perfusion:  default frame\n");
#endif
            write_default_frame();
        } else {
            // 将导播台传过来的数据写入到视频设备中

            // 从 remote_data 取出 无符号int类型的数据
            unsigned int *frame_count = (unsigned int *)remote_data;
            if (*frame_count == 0) {
                // 写入固定一张图到视频设备中
#if LOG_DEBUG
                LOGI("v4l2_perfusion: unavaild frame default frame\n");
#endif
                write_default_frame();
            } else {
                // 从 remote_data 取出 帧数据
                char *frame_buf = (char *)(remote_data) + sizeof(unsigned int);
                // 将 frame_count 的值打印出来
#if LOG_DEBUG
                LOGI("v4l2_perfusion:  frame_count: %d\n", *frame_count);
#endif

                int d_value = *frame_count - last_frame_index;

                if (d_value >= 1) {
                    // 导播台正常写入数据， 这里往视频设备中写入帧数据
                    write_frames(frame_buf);

                    last_frame_index = *frame_count;
                    lost_frame_count = 0;
#if LOG_DEBUG
                    LOGI("v4l2_perfusion:  normal frame\n");
#endif
                } else if (lost_frame_count < LOST_MAX_FRAME_COUNT) {
                    // 导播台写入数据出现了丢帧，这里往视频设备中写入上一帧的数据，持续30*10帧
//                    write(dev_fd, frame_buf, FRAME_BYTES);
                    write_frames(frame_buf);
#if LOG_DEBUG
                    LOGI("v4l2_perfusion:  repetition frame\n");
#endif
                    lost_frame_count++;
                } else {
                    // 丢帧太多，默认导播台挂掉，使用默认图，并解除内存映射
#if LOG_DEBUG
                    LOGI("v4l2_perfusion:  lost frame too much\n");
#endif
                    write_default_frame();
                }
            }
        }


        // last_end_copy_time 和 start_copy_time 这两个时间期间经历了 sleep 函数，只需要将这段时间减去 阻塞的时间 last_sleep_time，就是上次执行 sleep 函数本身的耗费时间
        if (last_end_copy_time > 0) {
            last_sleep_lost_time = start_copy_time - last_end_copy_time - last_sleep_time;
        }

        // 这里获取完成处理frame的时间戳
        gettimeofday(&tv, NULL);
        uint64_t end_copy_time = tv.tv_sec * 1000000 + tv.tv_usec;
        last_end_copy_time = end_copy_time;

        // 处理帧数据的耗费的时间
        uint64_t copy_frames_time = end_copy_time - start_copy_time;
        if (copy_frames_time > UINT32_MAX) {
            copy_frames_time = UINT32_MAX;
        }

        // 目标：每秒执行 FPS 次循环，则每个循环只能耗时 1.0f / FPS * 1000000.0f
        // 每个循环耗时分为三部分：处理帧数据的时间 + 调用 sleep 函数本身耗费时间 + sleep 函数阻塞的时间
        // sleep 函数阻塞的时间(sleep_time) = 总耗时(1.0f / FPS * 1000000.0f) - 处理帧数据的时间(copy_frames_time) - 调用 sleep 函数本身耗费时间(last_sleep_lost_time)
        uint32_t sleep_time = copy_frames_time + last_sleep_lost_time > 1.0f / FPS * 1000000.0f ? 0 : 1.0f / FPS * 1000000.0f - copy_frames_time - last_sleep_lost_time;

//#if LOG_DEBUG
//        LOGI("copy_frames_time: %lu\n", copy_frames_time);
//        LOGI("last_sleep_lost_time: %d\n", last_sleep_lost_time);
//        LOGI("sleep_time: %d\n", sleep_time);
//#endif

        last_sleep_time = sleep_time;
        usleep(sleep_time);


    }

    // 解除映射
    if (remote_data != MAP_FAILED) {
        munmap(remote_data, sizeof(unsigned int) + FRAME_BYTES);
    }
}

//uint64_t last_copy_time = 0;
//uint64_t sleep_lost_time = 0;

///**
// * 从共享内存中拷贝帧
// * @param setup     视频设备
// * @param dev_fd    共享内存的文件描述符
// */
//void copy_frames()
//{
//    // 使用mmap函数来映射memfd到你的进程的地址空间
//    void *remote_data = MAP_FAILED;
//
//    int old_memfd = -1;
//
//    while (1) {
//        struct timeval tv;
//
//        // 这里获取while循环开始的时间戳
//        gettimeofday(&tv, NULL);
//
//        uint64_t d_time = 0;
//        uint64_t current_time = tv.tv_sec * 1000000 + tv.tv_usec;
//        if (last_copy_time > 0 && current_time > last_copy_time) {
//            d_time = tv.tv_sec * 1000000 + tv.tv_usec - last_copy_time;
//        }
//
//        if (d_time > UINT32_MAX) {
//            d_time = UINT32_MAX;
//        }
//
//        uint32_t sleep_time = 0;
//        sleep_time = d_time + sleep_lost_time > 1.0f / FPS * 1000000.0f ? 0 : 1.0f / FPS * 1000000.0f - d_time - sleep_lost_time;
//
//#if LOG_DEBUG
//        LOGI("d_time: %lu\n", d_time);
//        LOGI("sleep_time: %d\n", sleep_time);
//#endif
//
//        usleep(sleep_time);
//
//        // 这里获取开始处理buffer的时间戳
//        gettimeofday(&tv, NULL);
//
//        last_copy_time = tv.tv_sec * 1000000 + tv.tv_usec;
//
//        uint64_t wait_time = last_copy_time - current_time;
//
//        sleep_lost_time = wait_time - sleep_time;
//
//#if LOG_DEBUG
//        LOGI("wait_time: %lu\n", wait_time);
//#endif
//
//        // 打印时间戳
//#if LOG_DEBUG
//        LOGI("v4l2_perfusion:  time: %ld\n", tv.tv_usec);
//#endif
//
//        pthread_mutex_lock(&lock);
//        if (global_memfd_fd > 0) {
//            // 当文件描述符发生变化时，解除映射
//            if (old_memfd != global_memfd_fd && remote_data != MAP_FAILED) {
//                munmap(remote_data, sizeof(unsigned int) + FRAME_BYTES);
//                remote_data = MAP_FAILED;
//            }
//            if (remote_data == MAP_FAILED) {
//                remote_data = mmap(NULL, sizeof(unsigned int) + FRAME_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, global_memfd_fd, 0);
//
//                last_frame_index = 0;
//            }
//        } else {
//            if (remote_data != MAP_FAILED) {
//#if LOG_DEBUG
//                LOGI("v4l2_perfusion:  unmap memfd\n");
//#endif
//                munmap(remote_data, sizeof(unsigned int) + FRAME_BYTES);
//                remote_data = MAP_FAILED;
//            }
//        }
//        old_memfd = global_memfd_fd;
//
//        pthread_mutex_unlock(&lock);
//
//        if (remote_data == MAP_FAILED) {
//            // 写入固定一张图到视频设备中
//#if LOG_DEBUG
//            LOGI("v4l2_perfusion:  default frame\n");
//#endif
//            write_default_frame();
//        } else {
//            // 将导播台传过来的数据写入到视频设备中
//
//            // 从 remote_data 取出 无符号int类型的数据
//            unsigned int *frame_count = (unsigned int *)remote_data;
//            if (*frame_count == 0) {
//                // 写入固定一张图到视频设备中
//#if LOG_DEBUG
//                LOGI("v4l2_perfusion: unavaild frame default frame\n");
//#endif
//                write_default_frame();
//                continue;
//            }
//
//            // 从 remote_data 取出 帧数据
//            char *frame_buf = (char *)(remote_data) + sizeof(unsigned int);
//            // 将 frame_count 的值打印出来
//#if LOG_DEBUG
//            LOGI("v4l2_perfusion:  frame_count: %d\n", *frame_count);
//#endif
//
//            int d_value = *frame_count - last_frame_index;
//
//            if (d_value >= 1) {
//                // 导播台正常写入数据， 这里往视频设备中写入帧数据
//                write_frames(frame_buf);
//
//                last_frame_index = *frame_count;
//                lost_frame_count = 0;
//#if LOG_DEBUG
//                LOGI("v4l2_perfusion:  normal frame\n");
//#endif
//            } else if (lost_frame_count < LOST_MAX_FRAME_COUNT) {
//                // 导播台写入数据出现了丢帧，这里往视频设备中写入上一帧的数据，持续30*10帧
////                    write(dev_fd, frame_buf, FRAME_BYTES);
//                write_frames(frame_buf);
//#if LOG_DEBUG
//                LOGI("v4l2_perfusion:  repetition frame\n");
//#endif
//                lost_frame_count++;
//            } else {
//                // 丢帧太多，默认导播台挂掉，使用默认图，并解除内存映射
//#if LOG_DEBUG
//                LOGI("v4l2_perfusion:  lost frame too much\n");
//#endif
//                write_default_frame();
//            }
//
//        }
//    }
//
//    // 解除映射
//    if (remote_data != MAP_FAILED) {
//        munmap(remote_data, sizeof(unsigned int) + FRAME_BYTES);
//    }
//}

void notify_camera_status() {
    pthread_mutex_lock(&mutex);
    ready = 1;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);
}


// 线程函数，用于等待socket客户端的连接并接收socket消息
void *socket_receive_thread(void *arg) {
    int sock, conn;
    char buf[64];
    struct iovec io = { .iov_base = buf, .iov_len = sizeof(buf) };
    struct msghdr msg = {0};
    struct cmsghdr *cmsg;
    char cbuf[CMSG_SPACE(sizeof(int))];

    while (1) {
        // 创建UNIX域套接字服务器
        sock = socket_local_server(SOCKET_PATH, ANDROID_SOCKET_NAMESPACE_RESERVED, SOCK_STREAM);
        if (sock < 0) {
            LOGE("socket_local_server ERROR");
            sleep(3);
        } else {
            break;
        }
    }

    // 玛德，这里的给0666权限，不然通过init进程调起obs进程时，/dev/socket/obs 权限为 0700，导致Hal Camera 无法连接socket, 真JB坑
    int result = chmod("/dev/socket/obs", S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
    if (result == 0) {
        // 权限修改成功
#if LOG_DEBUG
        LOGI("v4l2_perfusion:  chmod success\n");
#endif
    } else {
        // 权限修改失败
        LOGE("v4l2_perfusion:  chmod fail\n");
    }


    fd_set readfds;
    struct timeval tv;
    int retval;
    // 等待A进程的连接
    while (1) {
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        // 设置超时时间为1秒
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        retval = select(sock + 1, &readfds, NULL, NULL, &tv);
        if (retval == -1) {
#if LOG_DEBUG
            LOGI("v4l2_perfusion:  select no client connect\n");
#endif
            continue;
        } else if (retval > 0) {
            // 有连接请求

#if LOG_DEBUG
            LOGI("v4l2_perfusion:  receive connect\n");
#endif

            // 等待A进程的连接
            conn = accept(sock, NULL, NULL);
            if (conn < 0) {
#if LOG_DEBUG
                LOGI("v4l2_perfusion:  accept fail\n");
#endif
                continue;
            }
            // 打印socket连接成功的日志
#if LOG_DEBUG
            LOGI("v4l2_perfusion:  connect success\n");
#endif

            // 清空 buf，防止出现上次读取的脏数据
            memset(buf, 0, sizeof(buf));

            // 接收 socket 信息
            msg.msg_iov = &io;
            msg.msg_iovlen = 1;
            msg.msg_control = cbuf;
            msg.msg_controllen = sizeof(cbuf);
            if (recvmsg(conn, &msg, 0) < 0) {
                perror("recvmsg");
                continue;
            }

            buf[63] = '\0';  // 确保字符串以 null 结尾

#if LOG_DEBUG
            LOGI("Received content: %s\n", buf);
#endif

            if (strcmp(buf, CMD_MEMFD) == 0) {
                cmsg = CMSG_FIRSTHDR(&msg);
                if (cmsg == NULL || cmsg->cmsg_type != SCM_RIGHTS) {
                    LOGI("No SCM_RIGHTS control message received\n");
                    pthread_mutex_lock(&lock);
                    global_memfd_fd = -1;
                    pthread_mutex_unlock(&lock);
                } else {
                    // 接收到的文件描述符
                    pthread_mutex_lock(&lock);
                    global_memfd_fd = *((int *) CMSG_DATA(cmsg));
                    // 将 global_memfd_fd 打印出来
#if LOG_DEBUG
                    LOGI("v4l2_perfusion:  global_memfd_fd: %d\n", global_memfd_fd);
#endif
                    pthread_mutex_unlock(&lock);
                }


                // 激活 socket_send_thread 线程，发送camera状态
                // todo 这里发送socket消息，不会成功，因为导播台 socket server还没开启，暂时注释掉
//                notify_camera_status();

            } else if (strncmp(buf, "start", 5) == 0) {
                int index = buf[5] - '0';  // 获取数字部分
                if (index >= 0 && index < 5) {
                    pthread_mutex_lock(&lock);  // 加锁
                    if (camera_fd[index] < 0) {
                        char device[20];
                        sprintf(device, "/dev/video%d", 30 + index);
                        camera_fd[index] = open_video(device);
                        // 打印日志
                        LOGI("v4l2_perfusion:  open video device %s\n", device);
                    }
                    pthread_mutex_unlock(&lock);  // 解锁

                    notify_camera_status();
                }
            } else if (strncmp(buf, "stop", 4) == 0) {
                int index = buf[4] - '0';  // 获取数字部分
                if (index >= 0 && index < 5) {
                    pthread_mutex_lock(&lock);
                    if (camera_fd[index] >= 0) {
                        close(camera_fd[index]);
                        camera_fd[index] = -1;
                        // 打印日志
                        LOGI("v4l2_perfusion:  close video device %d\n", index);
                    }
                    pthread_mutex_unlock(&lock);

                    notify_camera_status();
                }
            }

            close(conn);
        } else {
            LOGI("v4l2_perfusion:  select no client connect 222\n");
        }
        // 没有连接请求，继续等待
    }

    close(sock);

    return NULL;
}

/**
 * 线程函数，用于发送socket消息
 * @param arg
 * @return
 */
void *socket_send_thread(void *arg) {
    while (1) {
        pthread_mutex_lock(&mutex);
        while (!ready)  // Loop to avoid spurious wakeups
            pthread_cond_wait(&cond, &mutex);
        ready = 0;  // Reset the condition

        char msg[] = "idle";

        pthread_mutex_lock(&lock);
        for (int i = 0; i < sizeof(camera_fd) / sizeof(camera_fd[0]); ++i) {
            if (camera_fd[i] >= 0) {
                strcpy(msg, "busy");
                break;
            }
        }
        pthread_mutex_unlock(&lock);

        // Create a socket and send a message
        int sock = socket_local_client("obs_reply", ANDROID_SOCKET_NAMESPACE_ABSTRACT, SOCK_STREAM);
#if LOG_DEBUG
        LOGI("v4l2_perfusion: socket status: %d\n", sock);
#endif
        if (sock >= 0) {
            // 将socket设置为非阻塞模式
            int flags = fcntl(sock, F_GETFL, 0);
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);


            // 使用select来检查socket是否准备好写入
            fd_set writefds;
            struct timeval tv;
            FD_ZERO(&writefds);
            FD_SET(sock, &writefds);
            tv.tv_sec = 0;
            tv.tv_usec = 0;  // 设置超时时间为0，使得select立即返回
            if (select(sock + 1, NULL, &writefds, NULL, &tv) > 0) {
#if LOG_DEBUG
                LOGI("v4l2_perfusion:  send socket message: %s\n", msg);
#endif
                write(sock, msg, strlen(msg));
            }

            close(sock);
        }

        pthread_mutex_unlock(&mutex);
    }
    return NULL;
}

// 线程函数，用于从共享内存中读取帧数据并将其写入到视频设备中
void *perfusion_thread(void *arg) {

    // 从共享内存memfd_fd中读取帧数据，然后写入到视频设备中
    copy_frames();

    return NULL;
}



int main(int argc, char **argv) {
    prog = argv[0];

    pthread_t thread1, thread2, thread3;

    // 创建线程1，用于等待socket客户端的连接并接收socket消息
    if (pthread_create(&thread1, NULL, socket_receive_thread, NULL) != 0) {
        perror("pthread_create");
        return 1;
    }

    // 创建第二个线程，用于从共享内存中读取帧数据并将其写入到视频设备中
    if (pthread_create(&thread2, NULL, perfusion_thread, NULL) != 0) {
        perror("pthread_create");
        return 1;
    }

    if (pthread_create(&thread3, NULL, socket_send_thread, NULL) != 0) {
        perror("pthread_create");
        return 1;
    }

    // 等待第一个线程结束
    if (pthread_join(thread1, NULL) != 0) {
        perror("pthread_join");
        return 1;
    }

    // 等待第二个线程结束
    if (pthread_join(thread2, NULL) != 0) {
        perror("pthread_join");
        return 1;
    }

    // 等待第三个线程结束
    if (pthread_join(thread3, NULL) != 0) {
        perror("pthread_join");
        return 1;
    }

    return 0;
}