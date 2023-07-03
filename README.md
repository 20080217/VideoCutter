# VideoCutter
A video cutter that can cut videos into frames.
You need libjpeg and ffmpeg to build and run it.
(My English is poor,if you cannot understand what i'm saying,please use translater.)


Chinese:视频切割器
一个可以将视频切割成帧的项目，使用c++编写，你需要libjpeg库和ffmpeg库才能用，当然，release里有编译好的exe可以直接使用
切割后的帧将被储存到运行目录下的cut_down文件夹中
默认的jpg质量为90，可以根据需求自行调整
本项目使用了多线程，可以榨干你的cpu性能，并且使用了异步i/o来优化性能
我只是个初中生，写的东西很垃圾，有问题请提交issues
