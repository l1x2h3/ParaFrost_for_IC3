#!/bin/bash
# test_gpu_preprocessing.sh

echo "=== GPU预处理功能测试 ==="

# 检查GPU环境
echo "1. 检查GPU环境..."
if command -v nvidia-smi &> /dev/null; then
    echo "✓ NVIDIA GPU detected:"
    nvidia-smi --query-gpu=name,memory.total --format=csv,noheader,nounits
else
    echo "✗ NVIDIA GPU not found"
fi

# 检查CUDA toolkit
echo -e "\n2. 检查CUDA toolkit..."
if command -v nvcc &> /dev/null; then
    echo "✓ CUDA toolkit found:"
    nvcc --version | grep "release"
else
    echo "✗ CUDA toolkit not found (需要安装CUDA toolkit)"
fi

# 检查ParaFROST版本
echo -e "\n3. 检查ParaFROST构建..."
if [ -f "build/gpu/bin/parafrost" ]; then
    echo "✓ ParaFROST GPU版本可用"
    GPU_AVAILABLE=true
elif [ -f "build/cpu/bin/parafrost" ]; then
    echo "⚠ ParaFROST只有CPU版本 (运行测试但无GPU加速)"
    GPU_AVAILABLE=false
else
    echo "✗ ParaFROST未构建"
    exit 1
fi

# 创建测试文件
echo -e "\n4. 创建测试AIGER文件..."
cat > gpu_test.aig << 'AIG_EOF'
aag 0 0 0 1 0
0
AIG_EOF
echo "✓ 测试文件创建完成"

# 运行IC3算法测试
echo -e "\n5. 运行IC3预处理测试..."
cd geyser/build

timeout 30 ./run-geyser -e=pdr ../gpu_test.aig > gpu_test_output.log 2>&1

if [ $? -eq 0 ]; then
    echo "✓ IC3运行成功"
    
    # 检查预处理日志
    if grep -q "DEBUG: Starting GPU preprocessing" gpu_test_output.log; then
        echo "✓ 预处理功能已启用"
    else
        echo "✗ 预处理功能未启用"
    fi
    
    if grep -q "DEBUG: Found.*ParaFROST" gpu_test_output.log; then
        echo "✓ ParaFROST集成成功"
    else
        echo "✗ ParaFROST集成失败"
    fi
    
    if grep -q "Finished" gpu_test_output.log; then
        echo "✓ IC3算法完成"
    else
        echo "✗ IC3算法异常"
    fi
    
    echo -e "\n=== 详细日志 ==="
    cat gpu_test_output.log
    
else
    echo "✗ IC3运行失败"
    cat gpu_test_output.log
fi

echo -e "\n=== 测试总结 ==="
if [ "$GPU_AVAILABLE" = true ]; then
    echo "GPU环境完整，预处理可实现并行化"
else
    echo "GPU环境不完整，建议安装CUDA toolkit以启用并行预处理"
fi
