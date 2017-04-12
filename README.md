## [如何使用gdb调试v8](https://github.com/earlymeme/v8/issues/2)
1. gdbinit
2. gdb-v8-support.py

## [如何使用lldb调试v8](https://github.com/earlymeme/v8/issues/1)
1. lldbinit
2. lldb_commands.py

## 分支
annotated分支,用于加注解,debug代码
master分支用于merge from upsteam/master

---

forked from https://github.com/v8/v8

V8 JavaScript Engine
=============

V8 is Google's open source JavaScript engine.

V8 implements ECMAScript as specified in ECMA-262.

V8 is written in C++ and is used in Google Chrome, the open source
browser from Google.

V8 can run standalone, or can be embedded into any C++ application.

V8 Project page: https://github.com/v8/v8/wiki


Getting the Code
=============

Checkout [depot tools](http://www.chromium.org/developers/how-tos/install-depot-tools), and run

        fetch v8

This will checkout V8 into the directory `v8` and fetch all of its dependencies.
To stay up to date, run

        git pull origin
        gclient sync

For fetching all branches, add the following into your remote
configuration in `.git/config`:

        fetch = +refs/branch-heads/*:refs/remotes/branch-heads/*
        fetch = +refs/tags/*:refs/tags/*


Contributing
=============

Please follow the instructions mentioned on the
[V8 wiki](https://github.com/v8/v8/wiki/Contributing).
