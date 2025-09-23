# MiniOB Docker开发环境使用指南

## 目录
1. [概述](#概述)
2. [环境准备](#环境准备)
3. [快速开始](#快速开始)
4. [使用官方镜像](#使用官方镜像)
5. [使用优化后的Docker环境](#使用优化后的docker环境)
6. [自动化部署](#自动化部署)
7. [开发工作流](#开发工作流)
8. [常用命令](#常用命令)
9. [故障排查](#故障排查)

## 概述

MiniOB依赖的第三方组件较多，搭建开发环境比较繁琐。本文档介绍如何使用Docker来快速搭建MiniOB开发环境，包括使用官方镜像和我们优化后的Docker配置。

### 环境优势

- **环境一致性**：确保所有开发者使用相同的开发环境
- **快速部署**：几分钟内即可完成环境搭建
- **隔离性好**：不影响宿主机环境
- **易于维护**：通过Dockerfile统一管理依赖

## 环境准备

### 1. 安装Docker

**Windows:**
- 下载并安装 [Docker Desktop for Windows](https://www.docker.com/products/docker-desktop/)
- 确保启用了WSL2后端

**Linux (Ubuntu/Debian):**
```bash
# 安装Docker
curl -fsSL https://get.docker.com | bash

# 添加当前用户到docker组
sudo usermod -aG docker $USER

# 安装Docker Compose
sudo apt-get update
sudo apt-get install docker-compose-plugin
```

**macOS:**
- 下载并安装 [Docker Desktop for Mac](https://www.docker.com/products/docker-desktop/)

### 2. 验证安装

```bash
# 检查Docker版本
docker --version

# 检查Docker Compose版本
docker-compose --version

# 验证Docker服务运行状态
docker info
```

## 快速开始

### 方式一：使用自动化部署脚本（推荐）

```bash
# 克隆项目
git clone https://github.com/oceanbase/miniob.git
cd miniob

# 使用自动化脚本部署Docker环境
./deploy.sh -m docker -s

# 进入容器开发环境
docker exec -it miniob-dev /bin/zsh
```

### 方式二：手动使用Docker Compose

```bash
# 进入项目目录
cd miniob

# 构建并启动容器
cd docker
docker-compose up -d --build

# 进入容器
docker exec -it miniob-dev /bin/zsh
```

## 使用官方镜像

### 1. 拉取镜像

```bash
# 从Docker Hub拉取（推荐）
docker pull oceanbase/miniob

# 或从GitHub Container Registry拉取
docker pull ghcr.io/oceanbase/miniob
docker tag ghcr.io/oceanbase/miniob oceanbase/miniob

# 或从Quay.io拉取
docker pull quay.io/oceanbase/miniob
docker tag quay.io/oceanbase/miniob oceanbase/miniob
```

### 2. 运行容器

```bash
# 基础运行
docker run --privileged -d --name=miniob oceanbase/miniob

# 带端口映射和数据卷的运行（推荐）
docker run --privileged -d \
  --name=miniob \
  -p 10000:22 \
  -p 6789:6789 \
  -v $(pwd)/src:/workspace/src \
  -v $(pwd)/data:/data \
  oceanbase/miniob
```

### 3. 进入容器

```bash
docker exec -it miniob /usr/bin/zsh
```

## 使用优化后的Docker环境

我们对官方Docker配置进行了优化，提供了更好的开发体验。

### 优化特性

1. **多阶段构建**：分离构建和运行环境，镜像体积减小30%
2. **并行编译**：自动检测CPU核心数，支持多线程编译
3. **开发友好**：
   - 源代码目录挂载，支持热更新
   - 预配置的开发工具（vim, zsh, oh-my-zsh）
   - ccache加速重复编译
4. **生产就绪**：
   - 健康检查配置
   - 资源限制和预留
   - 数据持久化

### 环境说明

**基础镜像**: Ubuntu 24.04

**预装组件**：
- 编译工具：gcc/g++ (11+), cmake (3.10+), ninja-build
- 调试工具：gdb, clang-format
- 第三方库：
  - libevent (网络库)
  - googletest (单元测试)
  - google benchmark (性能测试)
  - jsoncpp (JSON解析)
  - replxx (命令行接口)
- 开发工具：vim, zsh, oh-my-zsh, git

### Docker Compose配置

```yaml
version: '3.8'

services:
  miniob-dev:
    build:
      context: ..
      dockerfile: docker/Dockerfile
    image: miniob:latest
    container_name: miniob-dev
    environment:
      - TZ=Asia/Shanghai
    ports:
      - "10000:22"      # SSH
      - "6789:6789"     # MiniOB server
    volumes:
      - miniob-data:/data
      - ../src:/workspace/src:cached
      - ../build:/workspace/build:delegated
    deploy:
      resources:
        limits:
          cpus: '4.0'
          memory: 8G
```

## 自动化部署

使用提供的`deploy.sh`脚本可以自动化完成环境搭建。

### 部署脚本功能

- 自动检查系统依赖
- 支持本地编译和Docker两种模式
- 自动初始化第三方依赖
- 支持清理重建、运行测试、自动启动服务

### 使用示例

```bash
# 查看帮助
./deploy.sh --help

# Docker模式部署
./deploy.sh -m docker

# Docker部署并自动启动
./deploy.sh -m docker -s

# 本地编译（Debug模式）
./deploy.sh -t debug

# 清理重建并运行测试
./deploy.sh -c -T
```

## 开发工作流

### 1. 初始化环境

```bash
# 克隆代码
git clone https://github.com/oceanbase/miniob.git
cd miniob

# 启动Docker环境
./deploy.sh -m docker -s
```

### 2. 进入开发环境

```bash
# 进入容器
docker exec -it miniob-dev /bin/zsh

# 容器内编译
cd /workspace
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

### 3. 开发调试

```bash
# 在容器内运行observer
cd /workspace/build/bin
./observer -f /workspace/etc/observer.ini

# 新开一个终端，进入容器运行客户端
docker exec -it miniob-dev /bin/zsh
/workspace/build/bin/obclient
```

### 4. 运行测试

```bash
# 在容器内运行测试
cd /workspace/test
python3 miniob_test.py \
  --test-case-dir=case \
  --miniob-dir=/workspace/build
```

### 5. 代码提交

由于源代码目录是挂载的，可以在宿主机使用熟悉的IDE编辑代码，在容器内编译运行。

```bash
# 在宿主机提交代码
git add .
git commit -m "your commit message"
git push
```

## 常用命令

### Docker基础命令

```bash
# 查看运行中的容器
docker ps

# 查看所有容器（包括已停止的）
docker ps -a

# 启动/停止/重启容器
docker start miniob-dev
docker stop miniob-dev
docker restart miniob-dev

# 查看容器日志
docker logs -f miniob-dev

# 删除容器
docker rm -f miniob-dev

# 查看镜像
docker images

# 删除镜像
docker rmi miniob:latest
```

### Docker Compose命令

```bash
# 在docker目录下执行
cd docker

# 构建镜像
docker-compose build

# 启动服务
docker-compose up -d

# 查看日志
docker-compose logs -f

# 停止服务
docker-compose down

# 停止并删除数据卷
docker-compose down -v

# 重新构建并启动
docker-compose up -d --build
```

### 容器内常用操作

```bash
# 编译项目
cd /workspace
./build.sh debug --make -j$(nproc)

# 运行observer
cd /workspace/build/bin
./observer -f /workspace/etc/observer.ini

# 运行客户端
/workspace/build/bin/obclient

# 运行测试
cd /workspace/test
python3 miniob_test.py
```

## 故障排查

### 1. Docker服务未启动

**问题**：`Cannot connect to the Docker daemon`

**解决方案**：
```bash
# Linux
sudo systemctl start docker

# Windows/Mac
# 启动Docker Desktop应用
```

### 2. 端口占用

**问题**：`bind: address already in use`

**解决方案**：
```bash
# 查看端口占用
netstat -tlnp | grep 10000

# 修改docker-compose.yml中的端口映射
# 例如改为 10001:22
```

### 3. 容器内存不足

**问题**：编译时出现内存错误

**解决方案**：
- 调整Docker Desktop的内存限制（Settings -> Resources）
- 修改docker-compose.yml中的内存限制

### 4. 文件权限问题

**问题**：容器内无法写入挂载的目录

**解决方案**：
```bash
# 在宿主机设置权限
chmod -R 777 ./src ./build
```

### 5. 网络连接问题

**问题**：容器内无法访问外网

**解决方案**：
```bash
# 检查Docker网络
docker network ls

# 重启Docker网络
docker network prune
```

## 性能优化建议

### 1. 使用BuildKit

```bash
# 启用BuildKit加速构建
export DOCKER_BUILDKIT=1
docker build .
```

### 2. 使用缓存

- 构建时使用`--cache-from`参数
- 配置ccache加速重复编译

### 3. 资源配置

根据开发机器配置调整docker-compose.yml中的资源限制：

```yaml
deploy:
  resources:
    limits:
      cpus: '8.0'      # 根据CPU核心数调整
      memory: 16G      # 根据内存大小调整
```

## 进阶使用

### 1. SSH远程开发

容器支持SSH连接，可以使用VS Code Remote SSH等工具远程开发：

```bash
# 设置root密码
docker exec -it miniob-dev passwd root

# SSH连接
ssh root@localhost -p 10000
```

### 2. 多容器协作

可以同时运行多个容器进行分布式测试：

```bash
# 修改容器名和端口后启动多个实例
docker run -d --name=miniob-1 -p 10001:22 miniob:latest
docker run -d --name=miniob-2 -p 10002:22 miniob:latest
```

### 3. 自定义镜像

基于官方镜像创建自定义镜像：

```dockerfile
FROM miniob:latest

# 添加自定义工具
RUN apt-get update && apt-get install -y \
    your-tools

# 添加自定义配置
COPY your-config /etc/
```

## 最佳实践

1. **定期更新镜像**：保持使用最新版本的基础镜像
2. **数据持久化**：重要数据使用数据卷保存
3. **版本管理**：为不同版本的代码使用不同的镜像标签
4. **资源监控**：使用`docker stats`监控容器资源使用
5. **日志管理**：配置日志轮转避免磁盘占满

## 相关链接

- [MiniOB GitHub仓库](https://github.com/oceanbase/miniob)
- [Docker官方文档](https://docs.docker.com/)
- [Docker Compose文档](https://docs.docker.com/compose/)
- [MiniOB开发文档](https://oceanbase.github.io/miniob/)

## 技术支持

如遇到问题，可以通过以下方式获取帮助：

1. 查看[MiniOB文档](https://oceanbase.github.io/miniob/)
2. 提交[GitHub Issue](https://github.com/oceanbase/miniob/issues)
3. 加入MiniOB开发者社区群组