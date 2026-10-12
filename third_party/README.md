# third_party

第三方依赖放置目录。推荐用系统包管理器，其次源码放这里并 `add_subdirectory`。

## DCMTK（DICOM 编解码）

```bash
# Ubuntu / Debian
sudo apt install libdcmtk-dev

# macOS
brew install dcmtk

# 源码构建（可选）
git clone https://github.com/DCMTK/dcmtk.git
cd dcmtk && mkdir build && cd build
cmake .. -DDCMTK_ENABLE_STL=ON -DBUILD_SHARED_LIBS=OFF
make -j && sudo make install
```

未安装时可用 `-DDWS_HAVE_DCMTK=OFF` 关闭，先跑通网络层（DicomFile 退化为 stub）。

## SQLite3

```bash
sudo apt install libsqlite3-dev     # 或 brew install sqlite
```

未安装时 MetadataDao 退化为 no-op（可先用 `-DDWS_BUILD_TESTS=OFF` 跳过 DAO 测试）。

## GoogleTest

CMake 会自动 `FetchContent` 拉取 v1.14.0；无需手动下载。

## 参考项目（学习架构，勿直接抄代码）

- Orthanc — C++ PACS 服务器，DICOMweb 实现参考
- DCM4CHEE — Java 版 DICOMweb 接口设计参考
- DICOM 标准 PS3.18 — STOW/QIDO/WADO 规范
