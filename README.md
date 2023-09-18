## C++ Simple ParallelFor Implementation

### 简介

C++ 实现了一个简单的、可嵌套执行的`parallel_for` 方法。

实现方式参考了Unreal Engine的`ParallelFor`，是它的简化版、学习版。

测试的项目是[Ray Tracing in One Weekend Demo](https://github.com/RayTracing/raytracing.github.io)，利用实现后的`parallel_for`方法加速图片的渲染。

### 实现内容

```
using parallel_for_fn = std::function<void(int)>;

void parallel_for(int first, int last, int step, parallel_for_fn func)
{
    // 原始代码是一个串行执行的循环代码如下：
    //
    // for(int index = first; index < last; index += step)
    //     func(index);
    //
    // 利用parallel_for(first, last, step, func)改写后，func之间可以多线程并行执行
}
```

注意：`parallel_for`的所有并行`func`执行完之前，调用`parallel_for`的线程不应该继续执行。

```
void foo() {
	// 串行
	....
   
   	// 并行parallel_for
    parallel_for(0, 5, 1, [](int i){
        printf("inside for loop, i = %d\n", i);
    });

	// 串行
    printf("parallel_for done\n");	// 在打印出五次“inside for loop"之前，这句话不应执行
}
```

### 讲解

核心代码只在`parallel_for.h`文件中，其他文件都是Ray Tracing项目的内容。

分为了4个类，parallel_for_data/parallel_for_executor/task_t/scheduler；

- scheduler是单例调度器。
  - 构造函数中创建std::thread作为worker，worker在没有task的时候会等待std::condition_variable直到有task才执行，避免空转；
  -  task列表需要保证线程安全，使用了std::queue搭配std::unique_lock简单实现，性能较差；
- task_t表示单个任务，是scheduler调度、worker执行的最小粒度单位；
  - 具体执行的内容由成员变量runnable决定，runnable由用户自己构造，时间有限，只实现了parallel_for_executor这一种runnable；
- parallel_for_executor是task_t真正执行时运行的代码；
  - parallel_for_executor实例之间由不同worker并行，parallel_for_executor里面的内容是串行执行，目前是遍历执行若干次的callbody；
- parallel_for_data是单次调用parallel_for()时，创建出来的若干task共享的数据，类似context上下文的功能；

---

parallel_for(...)是入口，该函数会根据worker数量计算出创建多少个task；

再根据总计需要执行的callbody（即func）次数，简单计算出把整个大任务拆分成多少个batch，每个batch执行多少次callbody；

调用parallel_for线程负责创建task以及其他需要的数据，并且传给scheduler调度器处理；

同时当前线程会至少执行1个task，执行完毕后，会根据结果，直接返回，或挂起等待所有task结束的信号再返回（此时并行结束），避免忙等待；

在parallel_for嵌套调用的过程中，外循环的parallel_for会创建外循环task，每个外循环task执行过程也会创建内循环task；

worker执行单个外循环task时，会等待它创建的所有内循环task执行完毕后，才返回（内循环并行结束）。

### 性能测试

开启O2优化，未开启ENABLE_PARALLEL_FOR，执行37.92s；

开启O2优化，只开启ENABLE_PARALLEL_FOR，执行4.58s；

开启O2优化，开启ENABLE_PARALLEL_FOR和ENABLE_NESTING_PARALLEL_FOR，执行4.43s；
