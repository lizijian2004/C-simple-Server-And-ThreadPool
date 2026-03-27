本项目使用原生C++实现的简单服务器, 是一个新手的练手项目, 使用EPOLL处理了 linux 平台的网络并发, 使用自定义线程池完成了并发处理, 数据库和HTTP请求将在未来添加, 为了项目测试和构建的方便本项目使用了CMake
自定义线程池使用了模板函数进行类型擦除, 使其可以处理任何一个函数, 使用方法如下
pool.AddTask(func,args...); 或 pool.AddTask([this,arg](){func(arg);});
服务器开始运行只需要
server.Start(thread_num);
都使用了析构函数进行资源保底回收
对于不会使用CMake的同学可以通过在本项目的根目录下于终端中
cmake -S . -B build
cmake --build build
构建出来的执行程序就会在build文件夹(不存在cmake会自动创建)下, 执行文件名是server
