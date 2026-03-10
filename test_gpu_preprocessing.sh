#!/bin/bash
# test_gpu_preprocessing.sh

cd /root/ParaFrost_for_IC3

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
cat > gpu_test.aig << 'EOF'
aag 0 0 0 1 0
0
