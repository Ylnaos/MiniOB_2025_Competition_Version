#!/bin/bash

# MiniOB 自动化部署脚本
# 支持本地编译和Docker部署两种模式

set -e

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 检测CPU核心数
CPU_CORES=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# 脚本所在目录
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# 默认配置
BUILD_TYPE="release"
DEPLOY_MODE="local"
USE_DOCKER=false
CLEAN_BUILD=false
RUN_TESTS=false
AUTO_START=false

# 输出函数
print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 显示帮助信息
show_help() {
    cat << EOF
MiniOB 自动化部署脚本

用法: ./deploy.sh [选项]

选项:
    -h, --help          显示帮助信息
    -t, --type TYPE     编译类型 (debug/release, 默认: release)
    -m, --mode MODE     部署模式 (local/docker, 默认: local)
    -c, --clean         清理后重新编译
    -T, --test          编译后运行测试
    -s, --start         自动启动服务
    -j, --jobs N        并行编译任务数 (默认: $CPU_CORES)

示例:
    ./deploy.sh                     # 本地Release编译
    ./deploy.sh -t debug            # 本地Debug编译
    ./deploy.sh -m docker            # Docker部署
    ./deploy.sh -c -T -s            # 清理编译、运行测试并启动服务
    ./deploy.sh -m docker -s         # Docker部署并启动服务

Docker相关命令:
    docker-compose -f docker/docker-compose.yml up -d     # 启动容器
    docker-compose -f docker/docker-compose.yml down      # 停止容器
    docker-compose -f docker/docker-compose.yml logs -f   # 查看日志
    docker exec -it miniob-dev /bin/zsh                   # 进入容器
EOF
}

# 检查依赖
check_dependencies() {
    print_info "检查系统依赖..."

    local missing_deps=()

    # 检查基本工具
    command -v git >/dev/null 2>&1 || missing_deps+=("git")
    command -v cmake >/dev/null 2>&1 || missing_deps+=("cmake")
    command -v make >/dev/null 2>&1 || missing_deps+=("make")

    if [ "$DEPLOY_MODE" == "docker" ]; then
        command -v docker >/dev/null 2>&1 || missing_deps+=("docker")
        command -v docker-compose >/dev/null 2>&1 || missing_deps+=("docker-compose")
    fi

    if [ ${#missing_deps[@]} -gt 0 ]; then
        print_error "缺少以下依赖: ${missing_deps[*]}"
        print_info "请先安装缺失的依赖"
        return 1
    fi

    print_info "依赖检查通过"
    return 0
}

# 初始化第三方依赖
init_dependencies() {
    print_info "初始化第三方依赖..."

    if [ ! -d "deps/3rd/usr/local" ]; then
        print_info "首次编译，正在下载和编译依赖库..."
        ./build.sh init
        if [ $? -ne 0 ]; then
            print_error "依赖初始化失败"
            return 1
        fi
    else
        print_info "依赖已存在，跳过初始化"
    fi

    return 0
}

# 清理编译目录
clean_build_dir() {
    print_info "清理编译目录..."
    ./build.sh clean
}

# 本地编译
build_local() {
    print_info "开始本地编译 (模式: $BUILD_TYPE, 并行度: $CPU_CORES)..."

    # 初始化依赖
    init_dependencies || return 1

    # 清理
    if [ "$CLEAN_BUILD" = true ]; then
        clean_build_dir
    fi

    # 编译
    ./build.sh $BUILD_TYPE --make -j$CPU_CORES

    if [ $? -eq 0 ]; then
        print_info "编译成功!"
        print_info "二进制文件位置: $(pwd)/build_$BUILD_TYPE/bin/"
    else
        print_error "编译失败"
        return 1
    fi

    return 0
}

# Docker部署
deploy_docker() {
    print_info "开始Docker部署..."

    # 检查Docker服务
    if ! docker info >/dev/null 2>&1; then
        print_error "Docker服务未运行，请先启动Docker"
        return 1
    fi

    # 构建镜像
    print_info "构建Docker镜像..."
    cd docker

    # 使用BuildKit构建
    export DOCKER_BUILDKIT=1
    docker-compose build --parallel

    if [ $? -ne 0 ]; then
        print_error "Docker镜像构建失败"
        return 1
    fi

    print_info "Docker镜像构建成功"

    # 启动容器
    if [ "$AUTO_START" = true ]; then
        print_info "启动Docker容器..."
        docker-compose up -d

        if [ $? -eq 0 ]; then
            print_info "Docker容器启动成功"
            print_info "SSH端口: 10000"
            print_info "连接命令: ssh root@localhost -p 10000"
        else
            print_error "Docker容器启动失败"
            return 1
        fi
    else
        print_info "使用以下命令启动容器:"
        print_info "  cd docker && docker-compose up -d"
    fi

    cd ..
    return 0
}

# 运行测试
run_tests() {
    print_info "运行测试..."

    if [ ! -d "build_$BUILD_TYPE/bin" ]; then
        print_error "请先编译项目"
        return 1
    fi

    cd test
    python3 miniob_test.py --test-case-dir=case --test-case-score=case.score \
        --test-result-dir=result --test-result-tmp-dir=result_tmp \
        --miniob-dir=../build_$BUILD_TYPE --config=mysql.conf

    local test_result=$?
    cd ..

    if [ $test_result -eq 0 ]; then
        print_info "测试通过"
    else
        print_warn "部分测试失败，请查看详细日志"
    fi

    return $test_result
}

# 启动服务
start_service() {
    print_info "启动MiniOB服务..."

    if [ "$DEPLOY_MODE" == "local" ]; then
        if [ ! -f "build_$BUILD_TYPE/bin/observer" ]; then
            print_error "observer程序不存在，请先编译"
            return 1
        fi

        # 创建数据目录
        mkdir -p miniob_data

        # 启动observer
        print_info "启动observer..."
        cd miniob_data
        ../build_$BUILD_TYPE/bin/observer -f ../etc/observer.ini &
        local pid=$!
        cd ..

        sleep 2
        if ps -p $pid > /dev/null; then
            print_info "MiniOB服务启动成功 (PID: $pid)"
            print_info "连接命令: ./build_$BUILD_TYPE/bin/obclient"
        else
            print_error "MiniOB服务启动失败"
            return 1
        fi
    fi

    return 0
}

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        -t|--type)
            BUILD_TYPE="$2"
            shift 2
            ;;
        -m|--mode)
            DEPLOY_MODE="$2"
            shift 2
            ;;
        -c|--clean)
            CLEAN_BUILD=true
            shift
            ;;
        -T|--test)
            RUN_TESTS=true
            shift
            ;;
        -s|--start)
            AUTO_START=true
            shift
            ;;
        -j|--jobs)
            CPU_CORES="$2"
            shift 2
            ;;
        *)
            print_error "未知选项: $1"
            show_help
            exit 1
            ;;
    esac
done

# 主流程
main() {
    print_info "MiniOB 自动化部署开始"
    print_info "配置: 模式=$DEPLOY_MODE, 类型=$BUILD_TYPE, 核心数=$CPU_CORES"

    # 检查依赖
    check_dependencies || exit 1

    # 根据部署模式执行
    if [ "$DEPLOY_MODE" == "docker" ]; then
        deploy_docker || exit 1
    else
        build_local || exit 1

        # 运行测试
        if [ "$RUN_TESTS" = true ]; then
            run_tests
        fi

        # 启动服务
        if [ "$AUTO_START" = true ]; then
            start_service || exit 1
        fi
    fi

    print_info "部署完成!"

    # 显示后续步骤
    if [ "$DEPLOY_MODE" == "local" ]; then
        echo ""
        print_info "后续步骤:"
        print_info "1. 启动服务: cd miniob_data && ../build_$BUILD_TYPE/bin/observer -f ../etc/observer.ini"
        print_info "2. 连接客户端: ./build_$BUILD_TYPE/bin/obclient"
        print_info "3. 运行测试: cd test && python3 miniob_test.py"
    else
        echo ""
        print_info "后续步骤:"
        print_info "1. 查看容器: docker ps"
        print_info "2. 进入容器: docker exec -it miniob-dev /bin/zsh"
        print_info "3. 查看日志: docker-compose -f docker/docker-compose.yml logs -f"
        print_info "4. 停止容器: docker-compose -f docker/docker-compose.yml down"
    fi
}

# 执行主流程
main