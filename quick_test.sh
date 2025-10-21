#!/bin/bash

# 简单测试TOKENIZE函数
cd /root/miniob
echo "SELECT TOKENIZE('test', 'jieba');" | timeout 5s ./build/bin/observer -f /root/miniob/db/sys 2>/dev/null