# key_raw 解析逻辑修复说明

## 🔴 **原始问题**

### **问题描述**
`key_raw` 应该**包含终止符**，但原始代码的逻辑导致 `key_raw` **不包含终止符**。

### **原始代码的错误逻辑**

#### **UTF-16 情况**
```c
// ❌ 错误逻辑
if (header->is_utf16) {
    while (offset + 1 < block_size) {
        if (block_data[offset] == 0 && block_data[offset + 1] == 0) {
            break;  // offset 停在 \0\0 的第一个字节
        }
        offset += 2;
    }
}

size_t key_raw_len = offset - key_start;  // ❌ 不包含 \0\0
offset += terminator_size;  // 然后再跳过终止符
```

**问题**：
- 找到 `\0\0` 后，`break` 导致 `offset` 指向终止符的第一个字节
- `key_raw_len = offset - key_start` **不包含** `\0\0`
- 然后 `offset += 2` 跳过终止符

#### **非 UTF-16 情况**
```c
// ❌ 错误逻辑
else {
    while (offset < block_size && block_data[offset] != '\0') {
        offset++;  // offset 停在 \0 的位置
    }
}

size_t key_raw_len = offset - key_start;  // ❌ 不包含 \0
offset += terminator_size;  // 然后再跳过终止符
```

**问题**：
- 找到 `\0` 后，`offset` 指向 `\0`
- `key_raw_len = offset - key_start` **不包含** `\0`
- 然后 `offset += 1` 跳过终止符

---

## ✅ **修复后的正确逻辑**

### **关键改进**

1. **在找到终止符后立即递进 `offset`**
2. **`key_raw_len` 自然包含终止符**
3. **不需要再次跳过终止符**

### **修复后的代码**

#### **UTF-16 情况**
```c
// ✅ 正确逻辑
if (header->is_utf16) {
    while (offset + 1 < block_size) {
        if (block_data[offset] == 0 && block_data[offset + 1] == 0) {
            offset += 2;  // ✅ 立即移动到 \0\0 之后
            break;
        }
        offset += 2;
    }
}

size_t key_raw_len = offset - key_start;  // ✅ 包含 \0\0
// 不需要再次跳过终止符
```

**改进**：
- 找到 `\0\0` 后，立即 `offset += 2`
- `key_raw_len = offset - key_start` **包含** `\0\0`
- 逻辑清晰，不需要额外操作

#### **非 UTF-16 情况**
```c
// ✅ 正确逻辑
else {
    while (offset < block_size) {
        if (block_data[offset] == '\0') {
            offset += 1;  // ✅ 立即移动到 \0 之后
            break;
        }
        offset++;
    }
}

size_t key_raw_len = offset - key_start;  // ✅ 包含 \0
// 不需要再次跳过终止符
```

**改进**：
- 找到 `\0` 后，立即 `offset += 1`
- `key_raw_len = offset - key_start` **包含** `\0`
- 逻辑清晰，不需要额外操作

---

## 📊 **对比示例**

### **场景：UTF-8 字符串 "hello\0"**

#### **原始逻辑（错误）**
```
block_data: h e l l o \0 n e x t
offset:     0 1 2 3 4 5

1. key_start = 0
2. 扫描找到 \0 (offset = 5)
3. break (offset 停在 5)
4. key_raw_len = 5 - 0 = 5  ❌ 不包含 \0
5. key_raw = "hello" (5 字节)
6. offset += 1 (offset = 6)
```

#### **修复后逻辑（正确）**
```
block_data: h e l l o \0 n e x t
offset:     0 1 2 3 4 5 6

1. key_start = 0
2. 扫描找到 \0 (offset = 5)
3. offset += 1 (offset = 6) ✅
4. break
5. key_raw_len = 6 - 0 = 6  ✅ 包含 \0
6. key_raw = "hello\0" (6 字节)
7. 继续下一个 entry
```

### **场景：UTF-16 字符串 "你好\0\0"**

假设 UTF-16LE 编码：
- "你" = `0x60 0x4F`
- "好" = `0x7D 0x59`
- 终止符 = `0x00 0x00`

#### **原始逻辑（错误）**
```
block_data: 60 4F 7D 59 00 00 n e x t
offset:     0  1  2  3  4  5

1. key_start = 0
2. 扫描找到 \0\0 (offset = 4)
3. break (offset 停在 4)
4. key_raw_len = 4 - 0 = 4  ❌ 不包含 \0\0
5. key_raw = [60 4F 7D 59] (4 字节)
6. offset += 2 (offset = 6)
```

#### **修复后逻辑（正确）**
```
block_data: 60 4F 7D 59 00 00 n e x t
offset:     0  1  2  3  4  5  6

1. key_start = 0
2. 扫描找到 \0\0 (offset = 4)
3. offset += 2 (offset = 6) ✅
4. break
5. key_raw_len = 6 - 0 = 6  ✅ 包含 \0\0
6. key_raw = [60 4F 7D 59 00 00] (6 字节)
7. 继续下一个 entry
```

---

## 🎯 **关键改进点**

| 方面 | 原始逻辑 | 修复后逻辑 |
|------|----------|-----------|
| **UTF-16 找到终止符后** | `break` (停在 `\0\0` 的第一个字节) | `offset += 2; break;` (移到 `\0\0` 之后) |
| **非 UTF-16 找到终止符后** | 循环条件退出 (停在 `\0`) | `offset += 1; break;` (移到 `\0` 之后) |
| **key_raw_len** | 不包含终止符 ❌ | 包含终止符 ✅ |
| **额外操作** | 需要 `offset += terminator_size` | 不需要（已在循环中处理） |
| **逻辑一致性** | UTF-16 和非 UTF-16 不一致 | 完全一致 ✅ |

---

## ✅ **验证**

### **传递给 `convert_encoding_to_utf8` 的长度**

```c
// key_raw_len 包含终止符
size_t key_data_len = key_raw_len - terminator_size;  // ✅ 减去终止符

// 传递不含终止符的长度给转换函数
key_index->key = convert_encoding_to_utf8(
    key_index->key_raw,  // 原始数据（包含终止符）
    key_data_len,        // 数据长度（不含终止符）
    header->encoding
);
```

**原因**：
- `convert_encoding_to_utf8` 会自己添加 `\0` 结尾
- 所以传递的长度应该是**不含终止符**的数据长度

---

## 🎉 **总结**

### **修复的关键**

1. ✅ **统一递进逻辑**：UTF-16 和非 UTF-16 都在找到终止符后立即递进
2. ✅ **包含终止符**：`key_raw_len` 自然包含终止符
3. ✅ **简化代码**：不需要 `offset += terminator_size` 这个额外步骤
4. ✅ **逻辑清晰**：一次性完成，避免分两步处理

### **修复效果**

- ✅ `key_raw` 正确包含终止符
- ✅ UTF-16 和非 UTF-16 逻辑一致
- ✅ 代码更简洁易懂
- ✅ 避免了潜在的 bug

**感谢你指出这个问题！现在逻辑完全正确了！** 🚀
