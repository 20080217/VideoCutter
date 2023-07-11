extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <jpeglib.h>
#include <direct.h> 
}

#include <stdio.h>
#include <functional>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <iostream>
#include <string>
#include <windows.h>
//线程池
class ThreadPool {
public:
    ThreadPool(size_t threads) : stop(false) {
        for (size_t i = 0; i < threads; ++i)
            workers.emplace_back(
                [this] {
                    for (;;) {
                        std::function<void()> task;
                        {
                            std::unique_lock<std::mutex> lock(this->queue_mutex);
                            this->condition.wait(lock,
                                [this] { return this->stop || !this->tasks.empty(); });
                            if (this->stop && this->tasks.empty())
                                return;
                            task = std::move(this->tasks.front());
                            this->tasks.pop();
                        }
                        task();
                    }
                }
        );
    }

    template<class F>
    void enqueue(F&& f) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.emplace(std::forward<F>(f));
        }
        condition.notify_one();
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread& worker : workers)
            worker.join();
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};
void saveJpeg(AVFrame* pFrame, int width, int height, int iFrame) {
    FILE* outfile;
    char filename[32];
    sprintf_s(filename, sizeof(filename), "cut_done/frame%d.jpg", iFrame);

    errno_t err = fopen_s(&outfile, filename, "wb");
    if (err != 0 || outfile == NULL) {
        fprintf(stderr, "打开输出文件失败\n");
        exit(1);
    }
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;

    JSAMPROW row_pointer[1];
    int row_stride;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, outfile);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 90, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    row_stride = width * 3;

    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = &pFrame->data[0][cinfo.next_scanline * pFrame->linesize[0]];
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    fclose(outfile);
    jpeg_destroy_compress(&cinfo);
}
void print_progress(double percentage) {
    const int bar_width = 70;
    std::cout << "[";
    int pos = bar_width * percentage;
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] " << int(percentage * 100.0) << " %\r";
    std::cout.flush();
}
int main(int argc, char* argv[]) {
    std::wstring path = L".\\ffmpeg\\";
    SetDllDirectory(path.c_str());
    // 任务队列
    std::queue<std::function<void()>> tasks;
    // 互斥锁
    std::mutex tasks_mutex;
    // 条件变量
    std::condition_variable tasks_cond;
    // 停止标志
    bool stop = false;
    // 写入线程
    std::thread writer_thread([&]() {
        while (true) {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(tasks_mutex);

                // 等待直到有任务可用或者收到停止信号
                tasks_cond.wait(lock, [&] { return stop || !tasks.empty(); });

                // 如果收到停止信号并且没有任务可执行，退出循环
                if (stop && tasks.empty())
                    return;

                // 取出任务
                task = std::move(tasks.front());
                tasks.pop();
            }

            // 执行任务
            task();
        }
        });
    unsigned int n = std::thread::hardware_concurrency();
    ThreadPool pool(n);
    if (_mkdir("cut_done") == -1) {
        errno_t err;
        _get_errno(&err);
        if (err != EEXIST) {
            fprintf(stderr, "无法创建目录\n");
            return -1;
        }
    }
    AVFormatContext* pFormatCtx = nullptr;
    int videoStream;
    AVCodecContext* pCodecCtx = nullptr;
    const AVCodec* pCodec = nullptr;
    AVFrame* pFrame = nullptr;
    AVFrame* pFrameRGB = nullptr;
    AVPacket packet;
    int frameFinished;
    int numBytes;
    uint8_t* buffer = nullptr;

    AVDictionary* optionsDict = nullptr;
    SwsContext* sws_ctx = nullptr;
    std::string videoFilePath;
    if (argc < 2) {
        std::cout << "请提供一个视频文件路径: ";
        std::getline(std::cin, videoFilePath);
        if (videoFilePath.empty()) {
            std::cout << "未提供视频文件路径，程序将退出。\n";
            return -1;
        }
    }
    else {
        videoFilePath = argv[1];
    }

    avformat_network_init();

    // 打开视频文件
    if (avformat_open_input(&pFormatCtx, videoFilePath.c_str(), nullptr, nullptr) != 0) {
        std::cout << "无法打开文件\n";
        system("pause");
        return -1; // 无法打开文件
    }
    // 获取开始时间
    auto start = std::chrono::high_resolution_clock::now();
    // 获取流信息
    if (avformat_find_stream_info(pFormatCtx, nullptr) < 0) {
        return -1; // 无法找到流信息
    }

    // 找到第一个视频流
    videoStream = -1;
    for (unsigned int i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = i;
            break;
        }
    }
    if (videoStream == -1) {
        return -1; // 没有找到视频流
    }

    // 获取视频流的解码器上下文指针
    pCodecCtx = avcodec_alloc_context3(nullptr);
    avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[videoStream]->codecpar);

    // 查找视频流的解码器
    pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
    if (pCodec == nullptr) {
        fprintf(stderr, "不支持的解码器！\n");
        return -1; // 未找到解码器
    }

    // 打开解码器
    if (avcodec_open2(pCodecCtx, pCodec, &optionsDict) < 0) {
        return -1; // 无法打开解码器
    }
    fprintf(stderr, "处理中,所有文件将存于.\\cut_done目录下\n");
    // 分配视频帧内存
    pFrame = av_frame_alloc();

    // 分配一个 AVFrame 结构体
    pFrameRGB = av_frame_alloc();
    if (pFrameRGB == nullptr) {
        // 如果分配失败，释放已经分配的内存
        av_frame_free(&pFrame);
        return -1;
    }

    // 确定所需缓冲区的大小并分配缓冲区
    numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, pCodecCtx->width,
        pCodecCtx->height, 1);
    buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));

    av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, buffer, AV_PIX_FMT_RGB24,
        pCodecCtx->width, pCodecCtx->height, 1);

    // 初始化用于软件缩放的 SWS 上下文
    sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
        pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    int frameNumber = 0;
    int total_frames = pFormatCtx->streams[videoStream]->nb_frames;
    int processed_frames = 0;

    while (av_read_frame(pFormatCtx, &packet) >= 0) {
        // 判断是否为视频流的数据包
        if (packet.stream_index == videoStream) {
            // 解码视频帧
            avcodec_send_packet(pCodecCtx, &packet);
            if (avcodec_receive_frame(pCodecCtx, pFrame) == 0) {  // 使用 pFrame.get()
                frameFinished = 1;
            }
            else {
                frameFinished = 0;
            }

            // 是否解码到视频帧
            if (frameFinished) {
                // 获取当前帧的时间戳
                int64_t pts = pFrame->pts;

                // 转换时间戳为秒
                double time = pts * av_q2d(pFormatCtx->streams[videoStream]->time_base);

                // 获取当前时间
                auto now = std::chrono::high_resolution_clock::now();

                // 计算从开始到现在经过的时间
                double elapsed = std::chrono::duration<double>(now - start).count();

                // 如果当前帧的显示时间还没有到，就等待
                if (time > elapsed) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(int((time - elapsed) * 1000)));
                }

                // 将图像从原始格式转换为 RGB 格式
                sws_scale(sws_ctx, (uint8_t const* const*)pFrame->data,
                    pFrame->linesize, 0, pCodecCtx->height,
                    pFrameRGB->data, pFrameRGB->linesize);

                // 将帧保存到磁盘
                pool.enqueue([=] { saveJpeg(pFrameRGB, pCodecCtx->width, pCodecCtx->height, frameNumber); });
                frameNumber++;

            }
        }
        av_packet_unref(&packet);
    }


    // 关闭解码器
    avcodec_close(pCodecCtx);

    // 关闭视频文件
    avformat_close_input(&pFormatCtx);
    // 设置停止标志
    stop = true;

    // 通知写入线程
    tasks_cond.notify_one();

    // 等待写入线程结束
    writer_thread.join();
    fprintf(stderr, "处理完毕\n");
    system("pause");
    return 0;
}
