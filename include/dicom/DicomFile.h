#pragma once
// DICOM 文件读写封装。未安装 DCMTK 时（DWS_HAVE_DCMTK 未定义）退化为 stub，便于先跑通网络层。
#include <optional>
#include <string>
#include <vector>

namespace dws {

struct DicomTags {
  std::string study_instance_uid;
  std::string series_instance_uid;
  std::string sop_instance_uid;
  std::string patient_id;
  std::string patient_name;
  std::string study_date;
  std::string modality;
  int series_number = 0;
  int rows = 0;
  int columns = 0;
  std::string transfer_syntax;
};

class DicomFile {
 public:
  // 读取文件头与关键标签；不加载像素数据（延迟到 frame 请求）
  static std::optional<DicomTags> ReadTags(const std::string& path, std::string* err);

  // 校验是否为合法 DICOM（校验 DICM magic + 传输语法）
  static bool IsValid(const std::string& path);

  // 提取指定帧的原始像素数据（WADO-RS frames 用）
  static std::optional<std::vector<char>> ReadFrame(const std::string& path, int frame_index,
                                                    std::string* err);
};

}  // namespace dws
