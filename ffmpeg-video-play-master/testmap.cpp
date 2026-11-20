#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace std;
int main() {
    int* ptr = new(nothrow) int[10000000];
    if (ptr == nullptr)
    {
        cerr << "内存分配失败！" << endl;
    }
    else
    {
        // 使用ptr
        delete[] ptr;
    }
    return 0;
}