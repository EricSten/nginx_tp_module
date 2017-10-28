# nginx HTTP module with Thread Pools and Tasks

### Purpose

Example of nginx HTTP module using thread pools.


Hopefully this will be useful to someone who is writing an nginx HTTP module and needs to move their blocking operations to an asynchronous work item (a.k.a. a task queued to a thread pool).


### Notes

The tricky bit was figuring out how to resume processing of the HTTP request after the task completed.  The nginx documentation on [thread pools](http://nginx.org/en/docs/dev/development_guide.html#threads) didn't really cover that topic.  And the documentation on [HTTP phases](http://nginx.org/en/docs/dev/development_guide.html#http_phases) erroneously listed `ngx_http_core_run_phases()` as the function to call to resume processing, which didn't work when I tried it.  After digging through the nginx sources, I found the correct function was `ngx_http_handler()`.


This module was compiled and tested against nginx 1.12.0.


### Requirements

This module requres the `--with-threads` option for `./configure` for compilation.


You'll need to update the config file to match your build environment.


This module assumes the existance of a named thread pool, `ericsten`.  To get this module to run, you'll need to add a `thread_pool` directive to your nginx.conf file, e.g.:

```
    thread_pool ericsten threads=32 max_queue=65536;
```

I leave it as an excercise for you, the reader, to figure out how many threads you need for your particular background operation(s).

### License

[Apache License 2.0](https://github.com/EricSten/nginx_tp_module/blob/master/LICENSE.txt)