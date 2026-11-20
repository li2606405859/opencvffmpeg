#define __STDC_CONSTANT_MACROS
#define __STDC_FORMAT_MACROS
#include <stdio.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
}
int main() {
    av_register_all();
    avfilter_register_all();
    const AVFilter* f;
    
    // 试试系统 ffmpeg 显示的名字
    const char* names[] = {
        "abuffer", "abuffersrc", "buffersrc", 
        "abuffersink", "buffersink", "atempo",
        NULL
    };
    for (int i = 0; names[i]; i++) {
        f = avfilter_get_by_name(names[i]);
        printf("%-20s: %s\n", names[i], f ? "FOUND" : "NOT FOUND");
    }
    return 0;
}
