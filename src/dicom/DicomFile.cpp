#include "dicom/DicomFile.h"

#include "util/Logger.h"

#include <algorithm>
#include <fstream>
#include <vector>

#if defined(DWS_HAVE_DCMTK)
#include <dcmtk/dcmdata/dctk.h>
#endif

namespace dws {

bool DicomFile::IsValid(const std::string& path) {
#if defined(DWS_HAVE_DCMTK)
  DcmFileFormat ff;
  return ff.loadFile(path.c_str()).good();
#else
  // 无 DCMTK 时的最小校验：检查偏移 128 处的 DICM magic
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(128);
  char magic[4] = {0};
  in.read(magic, 4);
  return in.gcount() == 4 && magic[0] == 'D' && magic[1] == 'I' && magic[2] == 'C' &&
         magic[3] == 'M';
#endif
}

std::optional<DicomTags> DicomFile::ReadTags(const std::string& path, std::string* err) {
#if defined(DWS_HAVE_DCMTK)
  DcmFileFormat ff;
  if (!ff.loadFile(path.c_str()).good()) {
    if (err) *err = "loadFile failed";
    return std::nullopt;
  }
  DcmDataset* ds = ff.getDataset();
  DicomTags t;
  OFString s;
  if (ds->findAndGetOFString(DCM_StudyInstanceUID, s).good()) t.study_instance_uid = s.c_str();
  if (ds->findAndGetOFString(DCM_SeriesInstanceUID, s).good()) t.series_instance_uid = s.c_str();
  if (ds->findAndGetOFString(DCM_SOPInstanceUID, s).good()) t.sop_instance_uid = s.c_str();
  if (ds->findAndGetOFString(DCM_PatientID, s).good()) t.patient_id = s.c_str();
  if (ds->findAndGetOFString(DCM_PatientName, s).good()) t.patient_name = s.c_str();
  if (ds->findAndGetOFString(DCM_StudyDate, s).good()) t.study_date = s.c_str();
  if (ds->findAndGetOFString(DCM_Modality, s).good()) t.modality = s.c_str();
  Uint16 u = 0;
  if (ds->findAndGetUint16(DCM_Rows, u).good()) t.rows = u;
  if (ds->findAndGetUint16(DCM_Columns, u).good()) t.columns = u;
  if (ds->findAndGetOFString(DCM_TransferSyntaxUID, s).good()) t.transfer_syntax = s.c_str();
  return t;
#else
  (void)path;
  if (err) *err = "built without DCMTK (DWS_HAVE_DCMTK=0)";
  LOG_WARN("ReadTags: %s", err->c_str());
  return std::nullopt;
#endif
}

std::optional<std::vector<char>> DicomFile::ReadFrame(const std::string& path, int frame_index,
                                                      std::string* err) {
#if defined(DWS_HAVE_DCMTK)
  DcmFileFormat ff;
  if (!ff.loadFile(path.c_str()).good()) {
    if (err) *err = "loadFile failed";
    return std::nullopt;
  }
  DcmDataset* ds = ff.getDataset();
  Uint16 bits = 0, samples = 1;
  ds->findAndGetUint16(DCM_BitsAllocated, bits);
  ds->findAndGetUint16(DCM_SamplesPerPixel, samples);
  const Uint16* pix = nullptr;
  unsigned long cnt = 0;
  if (ds->findAndGetUint16Array(DCM_PixelData, pix, &cnt).bad()) {
    if (err) *err = "no pixel data";
    return std::nullopt;
  }
  const size_t bytes_per_frame = (cnt * sizeof(Uint16)) / std::max(1, frame_index + 1);
  const size_t offset = bytes_per_frame * static_cast<size_t>(frame_index);
  std::vector<char> out(reinterpret_cast<const char*>(pix) + offset,
                        reinterpret_cast<const char*>(pix) + offset + bytes_per_frame);
  return out;
#else
  (void)path; (void)frame_index;
  if (err) *err = "built without DCMTK";
  return std::nullopt;
#endif
}

}  // namespace dws
