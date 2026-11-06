# Linux C Web 服务器

基于 Linux TCP Socket 和 `epoll` 的单进程事件驱动 HTTP 服务，提供静态页面、注册登录和 SQLite 传感器数据管理接口。传感器读数是用于演示 CRUD 的数据库数据，不连接真实硬件。

## 构建与运行

Ubuntu/Debian 系统先安装编译器以及 SQLite、OpenSSL 的开发库：

```sh
sudo apt update
sudo apt install build-essential libsqlite3-dev libssl-dev
```

其中，`libsqlite3-dev` 提供 `sqlite3.h`，`libssl-dev` 提供 `openssl/evp.h`。

安装完成后，在 Linux 上构建并运行：

```sh
cd src
make
./web
```

浏览器打开 `http://127.0.0.1:8080`。服务器首次启动会在当前目录创建 `dictionary.db`，并写入三条示例传感器记录。静态文件路径相对 `src` 目录，因此请从该目录启动。

注册账号后登录。传感器 API 需要登录会话：

- `GET /api/sensors?q=实验室` 查询，可按名称或位置筛选。
- `POST /api/sensors` 新增，表单字段为 `name`、`location`、`temperature`、`humidity`。
- `PUT /api/sensors?id=1` 更新同样的字段。
- `DELETE /api/sensors?id=1` 删除。
- `POST /api/auth/register` 和 `POST /api/auth/login` 接收 `username`、`password` 表单字段；会话通过 HttpOnly Cookie 维护。

账号密码以每用户随机盐值加 SHA-256 摘要保存。该实现用于课程演示，不应直接用于公网生产环境；传感器记录当前对所有已登录账号共享。

## 等待模型对比

在 Linux 上运行 `make benchmark && ./benchmark`，可测量 `select`、`poll` 和 `epoll` 等待 timerfd 事件时的墙钟时间和进程 CPU 时间。服务本身在没有事件时阻塞于 `epoll_wait`，不会忙轮询。`select` 和 `poll` 同样可以阻塞；`epoll` 的主要扩展优势是在大量文件描述符中关注就绪事件，少量连接时不保证它一定比另外两种更快。
