# enostorg — Network Storage Server

**enostorg** 是一个基于 **Drogon** C++ Web 框架的网络存储服务器，采用**两级存储架构**：SQLite 记录文件的元数据和块链结构，实际数据以 `.dat` 文件形式存放在文件系统中。

---

## 目录

- [构建](#构建)
- [配置](#配置)
- [架构](#架构)
- [API 参考](#api-参考)
  - [/api/files — 文件元数据](#apifiles--文件元数据)
  - [/api/blocks — 数据块元数据](#apiblocks--数据块元数据)
  - [/api/files/blocks — 文件-块关联](#apifilesblocks--文件-块关联)
  - [/api/objects — 对象存取（带数据读写）](#apiobjects--对象存取带数据读写)
- [分块策略](#分块策略)
- [数据流示例](#数据流示例)

---

## 构建

### 前置条件

- **CMake** ≥ 3.16
- **Visual Studio 2022**（Windows）或 **GCC** / **Clang**（Linux/macOS）
- 编译器需支持 **C++23**（MSVC v19.44+ / GCC 14+ / Clang 18+）

### 编译

```bash
cd enostorg
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

CMake 会自动使用 `FetchContent` 下载依赖：

| 依赖 | 来源 | 用途 |
|------|------|------|
| Drogon 1.9.9 | GitHub | HTTP 框架、路由、JSON 序列化 |
| SQLite 3.49 | sqlite.org | 元数据存储（文件 + 块记录） |
| zlib 1.3.1 | GitHub | Drogon 依赖 |
| jsoncpp 1.9.6 | GitHub | JSON 解析/序列化 |

### 运行

```bash
cd Release
enostorg.exe
```

默认监听 `http://0.0.0.0:8080`。

---

## 配置

启动前可在工作目录放置 `config.ini`（不提供则使用默认值）。

```ini
[server]
listen = 0.0.0.0           # 监听地址
port = 8080                # 监听端口
max_connections = 0        # 最大连接数（0 = 不限制）
request_timeout = 30       # 请求超时（秒）

[logging]
level = info               # 日志级别：trace/debug/info/warn/error/fatal
file = enostorg.log        # 日志文件路径（留空仅输出到终端）
console = true             # 是否输出到终端

[database]
path = storage.db          # SQLite 数据库文件路径
wal_mode = true            # 是否启用 WAL 模式
busy_timeout_ms = 5000     # SQLite 忙等待超时（毫秒）

[storage]
blocks_dir = blocks        # 数据块文件存放目录（相对于工作目录）

[chunking]
strategy = variable        # 分块策略：fixed（固定大小）| variable（内容定义）
fixed_size = 262144        # 固定分块大小（字节），仅 strategy=fixed 时生效
min_chunk_size = 65536     # 可变分块最小大小（字节）
max_chunk_size = 1048576   # 可变分块最大大小（字节）
rolling_hash_window = 48   # 滚动哈希滑动窗口大小（字节）
rolling_hash_mask_bits = 12 # 掩码位数（约每 2^bits 字节一个边界）

[backup]
strategy = mirror            # 备份策略：none（不备份）| mirror（副本环）
replicas = 1                 # 每个主块的副本数（需 ≥ replicas+1 个可用磁盘）
```

### diskinfo.ini（磁盘元数据）

每个物理/虚拟磁盘对应 `blocks_dir` 下的一个子文件夹，其中放置 `diskinfo.ini` 标识该磁盘可用。

```ini
[disk]
name = disk01               # 文件夹名（自动匹配）
label = Fast SSD 1TB        # 可读描述
capacity = 1000000000000    # 总容量（字节）
speed_rating = 8            # 速率等级 1-10
weight = auto               # 分配权重：auto（=容量×速率）| 数值 | 0（只读）
```

- 文件夹下无 `diskinfo.ini` → 跳过，磁盘不可用
- `weight = 0` 或 `capacity = 0` → 只读，不接收新数据

### 磁盘发现与权重分配

启动时扫描 `blocks_dir` 下所有子文件夹，识别有效磁盘。写入时按权重分配：

```
权重 = (weight==auto) ? capacity * speed_rating : weight
选中概率 P_i = W_i / ΣW_j
```

例如 3 盘配置：
| 磁盘 | 容量 | 速率 | 权重 | 选中概率 |
|------|------|------|------|----------|
| disk01 | 1GB | 8 | 8,192 | 11.5% |
| disk02 | 10GB | 3 | 30,720 | 43.2% |
| disk03 | 5GB | 6 | 30,720 | 43.2% |

---

## 架构

### 两级存储

```
┌──────────────────────────────────────────┐
│              /api/objects                 │
│    POST / GET / PATCH / PUT / DELETE      │
└──────────┬───────────────────┬───────────┘
           │                   │
           ▼                   ▼
┌──────────────────┐  ┌──────────────────────────────┐
│   SQLite 元数据    │  │    多磁盘数据文件              │
│                  │  │                              │
│  files 表        │  │  blocks_dir/                 │
│  ├─ file_path   │  │  ├── disk01/                 │
│  ├─ size        │  │  │   ├── block_xxx.dat (主块)  │
│  ├─ start_block │  │  │   └── block_yyy.dat        │
│  └─ ...         │  │  ├── disk02/                 │
│                  │  │  │   ├── block_xxx.dat (副本) │
│  blocks 表       │  │  │   └── block_zzz.dat        │
│  ├─ block_path  │  │  └── disk03/                 │
│  ├─ next_block  │  │      └── ...                  │
│  ├─ spare_block │  │                              │
│  ├─ block_size  │  │  每个块文件 = 原始二进制数据     │
│  └─ sha256      │  │                              │
└──────────────────┘  └──────────────────────────────┘
```

- **SQLite 只存元数据**：不包含 `BLOB` 列，数据分布在多个磁盘文件夹下
- **block_path** 格式：`<disk_name>/block_<ts>_<n>.dat`
- **块链 + 备份环**：`next_block` 串联主块，`spare_block` 串联每个主块的副本环

### 文件分块

大文件写入时根据 `[chunking]` 配置自动分块：
- `fixed`：按固定大小切分
- `variable`：基于滚动哈希（Rabin fingerprint）寻找内容定义的边界

每个数据块生成一个独立的 `.dat` 文件，并在 `blocks` 表中创建一条元数据记录。

---

## API 参考

### /api/files — 文件元数据

**GET** `/api/files` — 列出所有文件
```
curl http://localhost:8080/api/files
```
响应：`[{file_entry}, ...]`

**GET** `/api/files?path=...` — 获取单个文件元数据
```
curl http://localhost:8080/api/files?path=/test/hello.txt
```
响应：`{file_entry}`

**POST** `/api/files` — 创建文件元数据记录（body 为 JSON）
```json
{"file_path":"/test/doc.txt","size":0,"description":"a test file"}
```

**PUT** `/api/files?path=...` — 更新文件元数据（body 为 JSON，仅提供要修改的字段）

**DELETE** `/api/files?path=...` — 删除文件元数据及关联的所有块

### /api/blocks — 数据块元数据

**GET** 无直接列表接口（通过 `/api/files/blocks?path=...` 获取）

**POST** `/api/blocks` — 创建块元数据（body 为 JSON）
```json
{"block_path":"blocks/block_xxx.dat","sha256":"abc...","block_size":65536}
```

**PUT** `/api/blocks/update?block=N` — 更新块元数据

**DELETE** `/api/blocks/delete?block=N` — 删除块（同时删除对应的磁盘 .dat 文件）

**PATCH** `/api/blocks/bad?block=N` — 标记块为损坏

**PATCH** `/api/blocks/spare?block=N&spare=M` — 设置备用块

### /api/files/blocks — 文件-块关联

**GET** `/api/files/blocks?path=...` — 获取一个文件的所有块

**POST** `/api/files/blocks?path=...&block=N` — 将块追加到文件的块链尾部

### /api/objects — 对象存取（带数据读写）

这是最常用的高级接口，内部自动完成分块、文件读写和元数据管理。

#### POST — 创建对象

```
curl -X POST "http://localhost:8080/api/objects?path=/test/hello.txt" \
  --data-binary "Hello, world!"
```

- 响应：`201 Created`，返回 `FileEntry` JSON
- 行为：将 body 数据分块后写入磁盘文件，创建 files + blocks 元数据

#### GET — 读取对象

```
curl "http://localhost:8080/api/objects?path=/test/hello.txt"
```

- 响应：`200 OK`，body 为原始二进制数据
- 行为：遍历块链，依次读取磁盘文件，拼接后返回

#### GET + Range — 部分读取

```
curl -H "Range: bytes=0-6" "http://localhost:8080/api/objects?path=/test/hello.txt"
```

- 响应：`206 Partial Content`，带 `Content-Range: bytes 0-6/24` 头
- 行为：只读取涉及范围内的块文件和对应偏移，不发完整数据

#### PATCH — 追加数据（无 offset）

```
curl -X PATCH "http://localhost:8080/api/objects?path=/test/hello.txt" \
  --data-binary " appended!"
```

- 行为：将新数据分块后追加到文件块链尾部，更新 `size` 和 `modify_time`

#### PATCH + offset — 指定偏移修改

```
curl -X PATCH "http://localhost:8080/api/objects?path=/test/hello.txt&offset=7" \
  --data-binary "World"
```

- 行为：读取全部数据 → 在内存中修改 → 删除旧块 → 重新分块写入
- 注意：大文件此操作代价较高，会完全重写所有块文件

#### PUT — 重命名

```
curl -X PUT "http://localhost:8080/api/objects?path=/test/hello.txt" \
  -H "Content-Type: application/json" \
  -d '{"new_path":"/test/renamed.txt"}'
```

- 行为：仅更新 SQLite `files.file_path`，不移动磁盘文件

#### DELETE — 删除对象

```
curl -X DELETE "http://localhost:8080/api/objects?path=/test/hello.txt"
```

- 响应：`200 OK`
- 行为：删除所有关联的 `.dat` 块文件 → 清理 `blocks` 表 → 删除 `files` 记录

---

## 分块策略

### 固定分块 (`fixed`)

数据按 `fixed_size` 字节切分，最后一块可能小于该值。

```
[数据: 700 KB]
┌──────────────┬──────────────┬──────────┐
│ 256 KB       │ 256 KB       │ 188 KB   │
└──────────────┴──────────────┴──────────┘
```

### 可变分块 (`variable`)

基于**多项式滚动哈希（Rabin fingerprint）**寻找内容定义的边界：

```
hash = (hash - oldest_byte * BASE^(w-1)) * BASE + new_byte
```

当 `hash & mask == 0` 且块大小 ≥ `min_chunk_size` 时产生边界。块大小不会超过 `max_chunk_size`。

```
[数据: 700 KB]
┌──────────┬────────────┬──────────────┬────────────┐
│ 68 KB    │ 73 KB      │ 87 KB        │ ...        │
└──────────┴────────────┴──────────────┴────────────┘
```

**优势**：相同内容的边界稳定，即使数据发生偏移（插入/删除），已存在的块边界不受影响，适合增量备份和去重。

### 备份环

当 `[backup]` 配置为 `strategy=mirror` 时，`/api/objects` 写入的每个主块会同时在另外的磁盘上创建副本，形成 `spare_block` 环。

```
主链（next_block）:
  M1.next_block ──→ M2.next_block ──→ M3 ──→ -1
  R1a.next_block ──→ M2  (副本 next_block 同主块)
  R1b.next_block ──→ M2  (副本 next_block 同主块)

副本环（spare_block）:
  M1.spare_block ──→ R1a.spare_block ──→ R1b.spare_block ──→ M1 (闭合)
  M2.spare_block ──→ R2a ──→ R2b ──→ M2
  ...
```

- 每个环内的所有块（主块 + 副本）**不能在同一磁盘上**
- 写入 N 个副本需至少 N+1 个可用磁盘，不足则写入中止
- `GET` 返回原始数据（自动读取主块）；读取失败时可沿 `spare_block` 环查找可用副本
- `DELETE` 自动遍历环清理所有副本文件和元数据

---

## 数据流示例

### 完整操作流程

```bash
# 1. 创建一个文件对象（写入 24 字节 + 自动分块）
curl -X POST "http://localhost:8080/api/objects?path=/photo/beach.jpg" \
  --data-binary @beach.jpg

# 2. 查看元数据
curl "http://localhost:8080/api/files?path=/photo/beach.jpg"

# 3. 查看分块情况
curl "http://localhost:8080/api/files/blocks?path=/photo/beach.jpg"

# 4. 读取前 1KB
curl -H "Range: bytes=0-1023" "http://localhost:8080/api/objects?path=/photo/beach.jpg" \
  -o first_kb.bin

# 5. 追加数据
curl -X PATCH "http://localhost:8080/api/objects?path=/photo/beach.jpg" \
  --data-binary @metadata.bin

# 6. 重命名
curl -X PUT "http://localhost:8080/api/objects?path=/photo/beach.jpg" \
  -H "Content-Type: application/json" \
  -d '{"new_path":"/photo/sunset.jpg"}'

# 7. 删除
curl -X DELETE "http://localhost:8080/api/objects?path=/photo/sunset.jpg"
```

### 磁盘布局示例

```
enostorg/
├── enostorg.exe
├── config.ini
├── storage.db           ← SQLite: files + blocks 表
└── blocks/              ← [storage].blocks_dir
    ├── disk01/          ← 第一块磁盘（高速）
    │   ├── diskinfo.ini
    │   └── block_1784365043466_1.dat   ← 主块
    ├── disk02/          ← 第二块磁盘（大容量）
    │   ├── diskinfo.ini
    │   └── block_1784365043467_2.dat   ← 副本 (spare_block ring)
    └── disk03/          ← 第三块磁盘
        ├── diskinfo.ini
        └── ...
```
