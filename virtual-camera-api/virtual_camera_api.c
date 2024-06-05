//
// Created by liang on 2024/2/18.
//

#include <stdio.h>

#include <jni.h>
#include <android/log.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/memfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>

#define LOG_DEBUG 1

#define SOCKET_PATH "obs"

#define TEST_FILE_NAME "sdcard/earth_15.yuv"
#define FRAME_WIDTH 1920
#define FRAME_HEIGHT 1080
#define FPS 30
#define FRAME_BYTES 3 * FRAME_HEIGHT * FRAME_WIDTH / 2

#define CMD_MEMFD "memfd"

#define JAVA_CLASS "com/xiaowa/live/rendersdk/core/vitualcamera/VirtualCameraUtil"

unsigned int buffer_index = 1;// 通过共享内存传给obs的 帧下标 （重点：默认是1，因为obs端会通过 0 来判断当前共享内存中是否有有效帧数据）

int g_memfd = -1;// 共享内存的文件描述符

#define LOG_TAG "rexso_jni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)


pthread_mutex_t g_memfd_mutex = PTHREAD_MUTEX_INITIALIZER;

void fail(char *msg)
{
    LOGE("%s", msg);
}


int create_memfd(size_t size) {
    // 创建一个匿名的内存文件
    int fd = memfd_create("camera_memfd", MFD_CLOEXEC);
    if (fd == -1) {
        LOGE("memfd_create error");
        return -1;
    }

    // 设置文件的大小
    if (ftruncate(fd, size) == -1) {
        // 打印错误日志
        LOGE("meminfo ftruncate error");
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
        LOGE("socket create error");
        return;
    }

    // 设置 socket 为非阻塞模式
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/dev/socket/obs", sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        LOGE("socket connect error");
        close(sock);
        return;
    }

    // 发送共享内存的文件描述符
    struct msghdr msg = {0};
    struct iovec io = {.iov_base = "memfd", .iov_len = 5};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;

    if (memfd >= 0) {
        struct cmsghdr *cmsg;
        char buf[CMSG_SPACE(sizeof(int))];
        memset(buf, '\0', sizeof(buf));

        msg.msg_control = buf;
        msg.msg_controllen = sizeof(buf);

        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));

        *((int *) CMSG_DATA(cmsg)) = memfd;
    }

    // 使用 select 来检查 socket 是否准备好写入
    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(sock, &writefds);

    struct timeval timeout;
    timeout.tv_sec = 3;  // 设置超时为 3 秒
    timeout.tv_usec = 0;

    int ret = select(sock + 1, NULL, &writefds, NULL, &timeout);
    if (ret > 0) {
        // socket 已经准备好写入
        if (sendmsg(sock, &msg, 0) < 0) {
            LOGE("socket sendmsg error");
            close(sock);
            return;
        }
    } else if (ret == 0) {
        // select 超时
        LOGE("socket select timeout");
    } else {
        // select 出错
        // 打印 LOGE()
        LOGE("socket select error");
    }

    close(sock);

}


/**
 * 往共享内存中写入buffer
 * @param memfd   文件描述符
 */
void write_buffer(int memfd, char *frame, size_t len)
{
    // 加入帧下标
    write(memfd, &buffer_index, sizeof(unsigned int));
    // 将 frame 写入到共享内存中
    write(memfd, frame, len);
    // 将文件描述符的位置设置到文件的开始，否则 obs 无法读取到有效数据
    lseek(memfd, 0, SEEK_SET);

    buffer_index++;
}


void initFd(JNIEnv *env, jclass cls) {
    // 打印日志

#if LOG_DEBUG
    LOGI("initFd\n");
#endif

    pthread_mutex_lock(&g_memfd_mutex);
    // 共享内存里 数据：帧下标 + 帧数据
    g_memfd = create_memfd(sizeof(unsigned int) + FRAME_BYTES);
    if (g_memfd < 0) {
        LOGE("create memfd failed");
    } else {
        send_memfd(g_memfd);
    }
    pthread_mutex_unlock(&g_memfd_mutex);
}

void closeFd(JNIEnv *env, jclass cls) {
#if LOG_DEBUG
    LOGI("closeFd\n");
#endif
    // 在这里实现closeFd的功能
    pthread_mutex_lock(&g_memfd_mutex);
    if (g_memfd >= 0) {
        close(g_memfd);
        g_memfd = -1; // 将 g_memfd 重置为无效值
    }
    send_memfd(g_memfd);
    pthread_mutex_unlock(&g_memfd_mutex);
}

void initFdPath(JNIEnv *env, jclass cls, jstring str) {
    const char *path = (*env)->GetStringUTFChars(env, str, 0);
    // 在这里实现initFdPath的功能

    (*env)->ReleaseStringUTFChars(env, str, path);
}

void writeOneFrame(JNIEnv *env, jclass cls, jbyteArray bArr) {
    jbyte *frame = (*env)->GetByteArrayElements(env, bArr, NULL);
//    jsize len = (*env)->GetArrayLength(env, bArr);



    // 在这里实现writeOneFrame的功能
    (*env)->ReleaseByteArrayElements(env, bArr, frame, 0);
}

void writeFrame(JNIEnv *env, jclass cls, jbyteArray bArr) {
    jbyte *frame = (*env)->GetByteArrayElements(env, bArr, NULL);
    jsize len = (*env)->GetArrayLength(env, bArr);

#if LOG_DEBUG
    LOGI("writeFrame, len: %d\n", len);
#endif


    // 这里判断 数据长度是否是一帧的长度
    if (len != FRAME_BYTES) {
        LOGE("frame length is not equal to %d, len: %d", FRAME_BYTES, len);
        return;
    }
    pthread_mutex_lock(&g_memfd_mutex);
    if (g_memfd >= 0) {
        write_buffer(g_memfd, frame, len);
    }
#if LOG_DEBUG
    else {
        LOGI("memfd is invalid\n");
    }
#endif
    pthread_mutex_unlock(&g_memfd_mutex);


    // 在这里实现writeFrame的功能
    (*env)->ReleaseByteArrayElements(env, bArr, frame, 0);
}

static const JNINativeMethod methods[] = {
        {"initFd", "()V", (void *)initFd},
        {"closeFd", "()V", (void *)closeFd},
        {"initFdPath", "(Ljava/lang/String;)V", (void *)initFdPath},
        {"writeOneFrame", "([B)V", (void *)writeOneFrame},
        {"writeFrame", "([B)V", (void *)writeFrame},
};


JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        return -1;
    }

    jclass cls = (*env)->FindClass(env, JAVA_CLASS);
    if (cls == NULL) {
        return -1;
    }

    if ((*env)->RegisterNatives(env, cls, methods, sizeof(methods) / sizeof(methods[0])) < 0) {
        return -1;
    }
    // 如果还有其他上层java代码使用，在这里再次调用RegisterNatives

    return JNI_VERSION_1_6;
}
