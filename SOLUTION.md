# Geyser 测试自动化脚本 - 问题解决总结

## 问题描述
运行 `python3 test_automation.py` 时，所有测试结果显示为 `UNKNOWN`，而非实际的 CORRECT/INCORRECT 结果。

## 根本原因
Geyser 模型检查器的输出格式与脚本的预期不匹配：

- Geyser **不输出** 明确的 "CORRECT" 或 "INCORRECT" 字样
- 而是通过 **DEBUG 信息 + 反例数据** 来表达结果：
  - **CORRECT** = 只有 DEBUG 信息，没有反例数据
  - **INCORRECT** = DEBUG 信息 + 反例数据（以数字开头，后跟 "b0"、位向量等）

## Geyser 输出格式示例

### INCORRECT 的输出：
```
DEBUG: Starting GPU preprocessing
DEBUG: init_clauses size: 0
DEBUG: trans_clauses size: 32
...
1                          ← 反例状态索引
b0                         ← 位向量标识
11111111111111111111111111 ← 位向量数据
.                          ← 终止符
```

### CORRECT 的输出：
```
DEBUG: Starting GPU preprocessing
DEBUG: init_clauses size: 0
DEBUG: trans_clauses size: 32
...
(只有 DEBUG 消息，没有反例数据）
```

## 脚本修复

### 改进的结果解析逻辑：

```python
# 过滤掉 DEBUG 行
non_debug_lines = [line for line in stdout.split('\n') 
                   if not line.startswith('DEBUG:')]
non_debug_output = '\n'.join(non_debug_lines).strip()

# 检查是否存在反例
has_counterexample = False
if non_debug_output:
    lines = non_debug_output.split('\n')
    for line in lines:
        if line and (line[0].isdigit() or line.startswith('b')):
            has_counterexample = True
            break

if has_counterexample:
    result.result = "INCORRECT"
else:
    result.result = "CORRECT"
```

## 测试结果

现在脚本能够正确识别：

```
Testing accumulator_32bit.aig... ✗ INCORRECT
Testing gpu_test.aig... ✗ INCORRECT
```

## 使用方法

```bash
# 完整测试
python3 test_automation.py --timeout 60 --output geyser_report.txt --engine pdr

# 调试单个文件
python3 test_automation.py --debug-file accumulator_32bit.aig --engine pdr

# 不同引擎测试
python3 test_automation.py --engine car --timeout 60 --output car_report.txt
python3 test_automation.py --engine bmc --timeout 60 --output bmc_report.txt
```

## 关键改进

1. ✅ 正确识别 CORRECT vs INCORRECT
2. ✅ 支持多个 Geyser 引擎（pdr, car, icar, bmc, bcar）
3. ✅ 详细的错误捕获和调试模式
4. ✅ 生成 CSV 数据文件便于分析
5. ✅ 超时和错误处理

## 报告文件

运行完成后会生成：
- `geyser_report.txt` - 文本格式的详细报告
- `geyser_report_data.csv` - CSV 格式下的可用于进一步分析

