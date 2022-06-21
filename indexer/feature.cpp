#include "indexer/feature.hpp"

#include "indexer/classificator.hpp"
#include "indexer/feature_algo.hpp"
#include "indexer/feature_impl.hpp"
#include "indexer/feature_utils.hpp"
#include "indexer/feature_visibility.hpp"
#include "indexer/map_object.hpp"
#include "indexer/scales.hpp"
#include "indexer/shared_load_info.hpp"

#include "platform/preferred_languages.hpp"

#include "geometry/parametrized_segment.hpp"
#include "geometry/robust_orientation.hpp"

#include "coding/byte_stream.hpp"
#include "coding/dd_vector.hpp"

#include "base/assert.hpp"
#include "base/logging.hpp"
#include "base/range_iterator.hpp"
#include "base/stl_helpers.hpp"

#include <algorithm>
#include <exception>
#include <limits>

#include "defines.hpp"

using namespace feature;
using namespace std;

namespace
{
uint32_t constexpr kInvalidOffset = numeric_limits<uint32_t>::max();

// Get the index for geometry serialization.
// @param[in]  scale:
// -1 : index for the best geometry
// -2 : index for the worst geometry
// default : needed geometry
int GetScaleIndex(SharedLoadInfo const & loadInfo, int scale)
{
  int const count = loadInfo.GetScalesCount();

  // In case of WorldCoasts we should get correct last geometry.
  int const lastScale = loadInfo.GetLastScale();
  if (scale > lastScale)
    scale = lastScale;

  switch (scale)
  {
  case FeatureType::WORST_GEOMETRY: return 0;
  case FeatureType::BEST_GEOMETRY: return count - 1;
  default:
    for (int i = 0; i < count; ++i)
    {
      if (scale <= loadInfo.GetScale(i))
        return i;
    }
    return -1;
  }
}

int GetScaleIndex(SharedLoadInfo const & loadInfo, int scale,
                  FeatureType::GeometryOffsets const & offsets)
{
  int ind = -1;
  int const count = static_cast<int>(offsets.size());

  // In case of WorldCoasts we should get correct last geometry.
  int const lastScale = loadInfo.GetLastScale();
  if (scale > lastScale)
    scale = lastScale;

  switch (scale)
  {
  case FeatureType::BEST_GEOMETRY:
    // Choose the best existing geometry for the last visible scale.
    ind = count - 1;
    while (ind >= 0 && offsets[ind] == kInvalidOffset)
      --ind;
    break;

  case FeatureType::WORST_GEOMETRY:
    // Choose the worst existing geometry for the first visible scale.
    ind = 0;
    while (ind < count && offsets[ind] == kInvalidOffset)
      ++ind;
    break;

  default:
  {
    int const n = loadInfo.GetScalesCount();
    for (int i = 0; i < n; ++i)
    {
      if (scale <= loadInfo.GetScale(i))
        return (offsets[i] != kInvalidOffset ? i : -1);
    }
  }
  }

  if (ind >= 0 && ind < count)
    return ind;

  ASSERT(false, ("Feature should have any geometry ..."));
  return -1;
}

uint32_t CalcOffset(ArrayByteSource const & source, uint8_t const * start)
{
  ASSERT_GREATER_OR_EQUAL(source.PtrUint8(), start, ());
  return static_cast<uint32_t>(distance(start, source.PtrUint8()));
}

uint8_t Header(vector<uint8_t> const & data)
{
 CHECK(!data.empty(), ());
 return data[0];
}

void ReadOffsets(SharedLoadInfo const & loadInfo, ArrayByteSource & src, uint8_t mask,
                 FeatureType::GeometryOffsets & offsets)
{
  ASSERT(offsets.empty(), ());
  ASSERT_GREATER(mask, 0, ());

  offsets.resize(loadInfo.GetScalesCount(), kInvalidOffset);
  size_t ind = 0;

  while (mask > 0)
  {
    if (mask & 0x01)
      offsets[ind] = ReadVarUint<uint32_t>(src);

    ++ind;
    mask = mask >> 1;
  }
}

class BitSource
{
  uint8_t const * m_ptr;
  uint8_t m_pos;

public:
  explicit BitSource(uint8_t const * p) : m_ptr(p), m_pos(0) {}

  uint8_t Read(uint8_t count)
  {
    ASSERT_LESS(count, 9, ());

    uint8_t v = *m_ptr;
    v >>= m_pos;
    v &= ((1 << count) - 1);

    m_pos += count;
    if (m_pos >= 8)
    {
      ASSERT_EQUAL(m_pos, 8, ());
      ++m_ptr;
      m_pos = 0;
    }

    return v;
  }

  uint8_t const * RoundPtr()
  {
    if (m_pos > 0)
    {
      ++m_ptr;
      m_pos = 0;
    }
    return m_ptr;
  }
};

template <class TSource>
uint8_t ReadByte(TSource & src)
{
  return ReadPrimitiveFromSource<uint8_t>(src);
}
}  // namespace

FeatureType::FeatureType(SharedLoadInfo const * loadInfo, vector<uint8_t> && buffer,
                         indexer::MetadataDeserializer * metadataDeserializer)
  : m_loadInfo(loadInfo)
  , m_data(std::move(buffer))
  , m_metadataDeserializer(metadataDeserializer)
{
  CHECK(m_loadInfo && m_metadataDeserializer, ());

  m_header = Header(m_data);
}

FeatureType::FeatureType(osm::MapObject const & emo)
{
  HeaderGeomType headerGeomType = HeaderGeomType::Point;
  m_limitRect.MakeEmpty();

  switch (emo.GetGeomType())
  {
  case feature::GeomType::Undefined:
    // It is not possible because of FeatureType::GetGeomType() never returns GeomType::Undefined.
    UNREACHABLE();
  case feature::GeomType::Point:
    headerGeomType = HeaderGeomType::Point;
    m_center = emo.GetMercator();
    m_limitRect.Add(m_center);
    break;
  case feature::GeomType::Line:
    headerGeomType = HeaderGeomType::Line;
    m_points = Points(emo.GetPoints().begin(), emo.GetPoints().end());
    for (auto const & p : m_points)
      m_limitRect.Add(p);
    break;
  case feature::GeomType::Area:
    headerGeomType = HeaderGeomType::Area;
    m_triangles = Points(emo.GetTriangesAsPoints().begin(), emo.GetTriangesAsPoints().end());
    for (auto const & p : m_triangles)
      m_limitRect.Add(p);
    break;
  }

  m_parsed.m_points = m_parsed.m_triangles = true;

  m_params.name = emo.GetNameMultilang();
  string const & house = emo.GetHouseNumber();
  if (house.empty())
    m_params.house.Clear();
  else
    m_params.house.Set(house);
  m_parsed.m_common = true;

  m_metadata = emo.GetMetadata();
  m_parsed.m_metadata = true;
  m_parsed.m_metaIds = true;

  CHECK_LESS_OR_EQUAL(emo.GetTypes().Size(), feature::kMaxTypesCount, ());
  copy(emo.GetTypes().begin(), emo.GetTypes().end(), m_types.begin());

  m_parsed.m_types = true;
  m_header = CalculateHeader(emo.GetTypes().Size(), headerGeomType, m_params);
  m_parsed.m_header2 = true;

  m_id = emo.GetID();
}

feature::GeomType FeatureType::GetGeomType() const
{
  // FeatureType::FeatureType(osm::MapObject const & emo) expects
  // that GeomType::Undefined is never returned.
  auto const headerGeomType = static_cast<HeaderGeomType>(m_header & HEADER_MASK_GEOMTYPE);
  switch (headerGeomType)
  {
  case HeaderGeomType::Line: return GeomType::Line;
  case HeaderGeomType::Area: return GeomType::Area;
  default: return GeomType::Point;
  }
}

void FeatureType::ParseTypes()
{
  if (m_parsed.m_types)
    return;

  auto const typesOffset = sizeof(m_header);
  Classificator & c = classif();
  ArrayByteSource source(m_data.data() + typesOffset);

  size_t const count = GetTypesCount();
  for (size_t i = 0; i < count; ++i)
  {
    uint32_t index = ReadVarUint<uint32_t>(source);
    uint32_t const type = c.GetTypeForIndex(index);
    if (type > 0)
      m_types[i] = type;
    else
    {
      // Possible for newer MWMs with added types.
      LOG(LWARNING, ("Incorrect type index for feature. FeatureID:", m_id, ". Incorrect index:", index,
                   ". Loaded feature types:", m_types, ". Total count of types:", count));

      m_types[i] = c.GetStubType();
    }
  }

  m_offsets.m_common = CalcOffset(source, m_data.data());
  m_parsed.m_types = true;
}

void FeatureType::ParseCommon()
{
  if (m_parsed.m_common)
    return;

  CHECK(m_loadInfo, ());
  ParseTypes();

  ArrayByteSource source(m_data.data() + m_offsets.m_common);
  uint8_t const h = Header(m_data);
  m_params.Read(source, h);

  if (GetGeomType() == GeomType::Point)
  {
    m_center = serial::LoadPoint(source, m_loadInfo->GetDefGeometryCodingParams());
    m_limitRect.Add(m_center);
  }

  m_offsets.m_header2 = CalcOffset(source, m_data.data());
  m_parsed.m_common = true;
}

m2::PointD FeatureType::GetCenter()
{
  ASSERT_EQUAL(GetGeomType(), feature::GeomType::Point, ());
  ParseCommon();
  return m_center;
}

int8_t FeatureType::GetLayer()
{
  if ((m_header & feature::HEADER_MASK_HAS_LAYER) == 0)
    return 0;

  ParseCommon();
  return m_params.layer;
}

void FeatureType::ParseHeader2()
{
  if (m_parsed.m_header2)
    return;

  CHECK(m_loadInfo, ());
  ParseCommon();

  uint8_t ptsCount = 0, ptsMask = 0, trgCount = 0, trgMask = 0;
  BitSource bitSource(m_data.data() + m_offsets.m_header2);
  auto const headerGeomType = static_cast<HeaderGeomType>(Header(m_data) & HEADER_MASK_GEOMTYPE);

  if (headerGeomType == HeaderGeomType::Line)
  {
    ptsCount = bitSource.Read(4);
    if (ptsCount == 0)
      // A mask of outer geometry present.
      ptsMask = bitSource.Read(4);
    else
      ASSERT_GREATER(ptsCount, 1, ());
  }
  else if (headerGeomType == HeaderGeomType::Area)
  {
    trgCount = bitSource.Read(4);
    if (trgCount == 0)
      trgMask = bitSource.Read(4);
  }

  ArrayByteSource src(bitSource.RoundPtr());
  serial::GeometryCodingParams const & cp = m_loadInfo->GetDefGeometryCodingParams();

  if (headerGeomType == HeaderGeomType::Line)
  {
    if (ptsCount > 0)
    {
      // Inner geometry.
      int const count = ((ptsCount - 2) + 4 - 1) / 4;
      ASSERT_LESS(count, 4, ());

      for (int i = 0; i < count; ++i)
      {
        uint32_t mask = ReadByte(src);
        m_ptsSimpMask += (mask << (i << 3));
      }

      auto const * start = src.PtrUint8();
      src = ArrayByteSource(serial::LoadInnerPath(start, ptsCount, cp, m_points));
      m_innerStats.m_points = static_cast<uint32_t>(src.PtrUint8() - start);
    }
    else
    {
      // Outer geometry: first point is stored in the header (coding params).
      m_points.emplace_back(serial::LoadPoint(src, cp));
      ReadOffsets(*m_loadInfo, src, ptsMask, m_offsets.m_pts);
    }
  }
  else if (headerGeomType == HeaderGeomType::Area)
  {
    if (trgCount > 0)
    {
      trgCount += 2;

      auto const * start = src.PtrUint8();
      src = ArrayByteSource(serial::LoadInnerTriangles(start, trgCount, cp, m_triangles));
      m_innerStats.m_strips = CalcOffset(src, start);
    }
    else
    {
      ReadOffsets(*m_loadInfo, src, trgMask, m_offsets.m_trg);
    }
  }
  m_innerStats.m_size = CalcOffset(src, m_data.data());
  m_parsed.m_header2 = true;
}

void FeatureType::ResetGeometry()
{
  // Do not reset geometry for features created from MapObjects.
  if (!m_loadInfo)
    return;

  m_points.clear();
  m_triangles.clear();

  if (GetGeomType() != GeomType::Point)
    m_limitRect = m2::RectD();

  m_parsed.m_header2 = m_parsed.m_points = m_parsed.m_triangles = false;
  m_offsets.m_pts.clear();
  m_offsets.m_trg.clear();
  m_ptsSimpMask = 0;
}

uint32_t FeatureType::ParseGeometry(int scale)
{
  uint32_t sz = 0;
  if (!m_parsed.m_points)
  {
    CHECK(m_loadInfo, ());
    ParseHeader2();

    auto const headerGeomType = static_cast<HeaderGeomType>(Header(m_data) & HEADER_MASK_GEOMTYPE);
    if (headerGeomType == HeaderGeomType::Line)
    {
      size_t const count = m_points.size();
      if (count < 2)
      {
        ASSERT_EQUAL(count, 1, ());

        // Outer geometry.
        int ind = GetScaleIndex(*m_loadInfo, scale, m_offsets.m_pts);
        // If there is no geometry for the requested scale, fallback to a closest available one.
        // TODO: add enable/disable flag and always keep disabled for the world map.
        if (ind == -1)
          ind = GetScaleIndex(*m_loadInfo, FeatureType::WORST_GEOMETRY, m_offsets.m_pts);
        if (ind != -1)
        {
          ReaderSource<FilesContainerR::TReader> src(m_loadInfo->GetGeometryReader(ind));
          src.Skip(m_offsets.m_pts[ind]);

          serial::GeometryCodingParams cp = m_loadInfo->GetGeometryCodingParams(ind);
          cp.SetBasePoint(m_points[0]);
          serial::LoadOuterPath(src, cp, m_points);

          sz = static_cast<uint32_t>(src.Pos() - m_offsets.m_pts[ind]);
        }
      }
      else
      {
        // Filter inner geometry.

        FeatureType::Points points;
        points.reserve(count);

        int const scaleIndex = GetScaleIndex(*m_loadInfo, scale);
        ASSERT_LESS(scaleIndex, m_loadInfo->GetScalesCount(), ());

        points.emplace_back(m_points.front());
        int minScale = m_loadInfo->GetScalesCount() - 1;
        int pointScale = 0;
        for (size_t i = 1; i + 1 < count; ++i)
        {
          // Check for point visibility in needed scaleIndex.
          pointScale = static_cast<int>((m_ptsSimpMask >> (2 * (i - 1))) & 0x3);
          if (pointScale <= scaleIndex)
            points.emplace_back(m_points[i]);
          else if (points.size() == 1 && minScale > pointScale)
            minScale = pointScale;
        }
        // Fallback to a closest available geometry.
        if (points.size() == 1)
        {
          for (size_t i = 1; i + 1 < count; ++i)
          {
            if (static_cast<int>((m_ptsSimpMask >> (2 * (i - 1))) & 0x3) == minScale)
              points.emplace_back(m_points[i]);
          }
        }
        points.emplace_back(m_points.back());

        m_points.swap(points);
      }

      CalcRect(m_points, m_limitRect);
    }
    m_parsed.m_points = true;
  }
  return sz;
}

uint32_t FeatureType::ParseTriangles(int scale)
{
  uint32_t sz = 0;
  if (!m_parsed.m_triangles)
  {
    CHECK(m_loadInfo, ());
    ParseHeader2();

    auto const headerGeomType = static_cast<HeaderGeomType>(Header(m_data) & HEADER_MASK_GEOMTYPE);
    if (headerGeomType == HeaderGeomType::Area)
    {
      if (m_triangles.empty())
      {
        auto const ind = GetScaleIndex(*m_loadInfo, scale, m_offsets.m_trg);
        if (ind != -1)
        {
          ReaderSource<FilesContainerR::TReader> src(m_loadInfo->GetTrianglesReader(ind));
          src.Skip(m_offsets.m_trg[ind]);
          serial::LoadOuterTriangles(src, m_loadInfo->GetGeometryCodingParams(ind), m_triangles);

          sz = static_cast<uint32_t>(src.Pos() - m_offsets.m_trg[ind]);
        }
      }

      CalcRect(m_triangles, m_limitRect);
    }
    m_parsed.m_triangles = true;
  }
  return sz;
}

void FeatureType::ParseMetadata()
{
  if (m_parsed.m_metadata)
    return;

  CHECK(m_loadInfo, ());
  try
  {
    UNUSED_VALUE(m_metadataDeserializer->Get(m_id.m_index, m_metadata));
  }
  catch (Reader::OpenException const &)
  {
    LOG(LERROR, ("Error reading metadata", m_id));
  }

  m_parsed.m_metadata = true;
}

void FeatureType::ParseMetaIds()
{
  if (m_parsed.m_metaIds)
    return;

  CHECK(m_loadInfo, ());
  try
  {
    UNUSED_VALUE(m_metadataDeserializer->GetIds(m_id.m_index, m_metaIds));
  }
  catch (Reader::OpenException const &)
  {
    LOG(LERROR, ("Error reading metadata", m_id));
  }

  m_parsed.m_metaIds = true;
}

StringUtf8Multilang const & FeatureType::GetNames()
{
  ParseCommon();
  return m_params.name;
}

namespace
{
  template <class TCont>
  void Points2String(string & s, TCont const & points)
  {
    for (size_t i = 0; i < points.size(); ++i)
      s += DebugPrint(points[i]) + " ";
  }
}

string FeatureType::DebugString(int scale)
{
  ParseCommon();

  Classificator const & c = classif();

  string res = "Types";
  uint32_t const count = GetTypesCount();
  for (size_t i = 0; i < count; ++i)
    res += (" : " + c.GetReadableObjectName(m_types[i]));
  res += "\n";

  auto const paramsStr = m_params.DebugString();
  if (!paramsStr.empty())
  {
    res += paramsStr;
    res += "\n";
  }

  ParseGeometryAndTriangles(scale);
  m2::PointD keyPoint;
  switch (GetGeomType())
  {
  case GeomType::Point:
    keyPoint = m_center;
    break;

  case GeomType::Line:
    if (m_points.empty())
    {
      ASSERT(scale != FeatureType::WORST_GEOMETRY && scale != FeatureType::BEST_GEOMETRY, (scale));
      return res;
    }
    keyPoint = m_points.front();
    break;

  case GeomType::Area:
    if (m_triangles.empty())
    {
      ASSERT(scale != FeatureType::WORST_GEOMETRY && scale != FeatureType::BEST_GEOMETRY, (scale));
      return res;
    }

    ASSERT_GREATER(m_triangles.size(), 2, ());
    keyPoint = (m_triangles[0] + m_triangles[1] + m_triangles[2]) / 3.0;
    break;

  case GeomType::Undefined:
    ASSERT(false, ());
    break;
  }

  // Print coordinates in (lat,lon) for better investigation capabilities.
  res += "Key point: " + DebugPrint(keyPoint) + "; " + DebugPrint(mercator::ToLatLon(keyPoint));
  return res;
}

m2::RectD FeatureType::GetLimitRect(int scale)
{
  ParseGeometryAndTriangles(scale);

  if (m_triangles.empty() && m_points.empty() && (GetGeomType() != GeomType::Point))
  {
    // This function is called during indexing, when we need
    // to check visibility according to feature sizes.
    // So, if no geometry for this scale, assume that rect has zero dimensions.
    m_limitRect = m2::RectD(0, 0, 0, 0);
  }

  return m_limitRect;
}

bool FeatureType::IsEmptyGeometry(int scale)
{
  ParseGeometryAndTriangles(scale);

  switch (GetGeomType())
  {
  case GeomType::Area: return m_triangles.empty();
  case GeomType::Line: return m_points.empty();
  default: return false;
  }
}

size_t FeatureType::GetPointsCount() const
{
  ASSERT(m_parsed.m_points, ());
  return m_points.size();
}

m2::PointD const & FeatureType::GetPoint(size_t i) const
{
  ASSERT_LESS(i, m_points.size(), ());
  ASSERT(m_parsed.m_points, ());
  return m_points[i];
}

vector<m2::PointD> FeatureType::GetTrianglesAsPoints(int scale)
{
  ParseTriangles(scale);
  return {m_triangles.begin(), m_triangles.end()};
}

void FeatureType::ParseGeometryAndTriangles(int scale)
{
  ParseGeometry(scale);
  ParseTriangles(scale);
}

FeatureType::GeomStat FeatureType::GetGeometrySize(int scale)
{
  uint32_t sz = ParseGeometry(scale);
  if (sz == 0 && !m_points.empty())
    sz = m_innerStats.m_points;

  return GeomStat(sz, m_points.size());
}

FeatureType::GeomStat FeatureType::GetTrianglesSize(int scale)
{
  uint32_t sz = ParseTriangles(scale);
  if (sz == 0 && !m_triangles.empty())
    sz = m_innerStats.m_strips;

  return GeomStat(sz, m_triangles.size());
}

std::pair<std::string_view, std::string_view> FeatureType::GetPreferredNames()
{
  feature::NameParamsOut out;
  GetPreferredNames(false /* allowTranslit */, StringUtf8Multilang::GetLangIndex(languages::GetCurrentNorm()), out);
  return { out.primary, out.secondary };
}

void FeatureType::GetPreferredNames(bool allowTranslit, int8_t deviceLang, feature::NameParamsOut & out)
{
  if (!HasName())
    return;

  auto const & mwmInfo = m_id.m_mwmId.GetInfo();
  if (!mwmInfo)
    return;

  ParseCommon();

  feature::GetPreferredNames({ GetNames(), mwmInfo->GetRegionData(), deviceLang, allowTranslit }, out);
}

string_view FeatureType::GetReadableName()
{
  feature::NameParamsOut out;
  GetReadableName(false /* allowTranslit */, StringUtf8Multilang::GetLangIndex(languages::GetCurrentNorm()), out);
  return out.primary;
}

void FeatureType::GetReadableName(bool allowTranslit, int8_t deviceLang, feature::NameParamsOut & out)
{
  if (!HasName())
    return;

  auto const & mwmInfo = m_id.m_mwmId.GetInfo();
  if (!mwmInfo)
    return;

  ParseCommon();

  feature::GetReadableName({ GetNames(), mwmInfo->GetRegionData(), deviceLang, allowTranslit }, out);
}

string const & FeatureType::GetHouseNumber()
{
  ParseCommon();
  return m_params.house.Get();
}

string_view FeatureType::GetName(int8_t lang)
{
  if (!HasName())
    return {};

  ParseCommon();

  // We don't store empty names.
  string_view name;
  if (m_params.name.GetString(lang, name))
    ASSERT(!name.empty(), ());

  return name;
}

uint8_t FeatureType::GetRank()
{
  ParseCommon();
  return m_params.rank;
}

uint64_t FeatureType::GetPopulation() { return feature::RankToPopulation(GetRank()); }

string const & FeatureType::GetRoadNumber()
{
  ParseCommon();
  return m_params.ref;
}

feature::Metadata const & FeatureType::GetMetadata()
{
  ParseMetadata();
  return m_metadata;
}

std::string_view FeatureType::GetMetadata(feature::Metadata::EType type)
{
  ParseMetaIds();

  auto meta = m_metadata.Get(type);
  if (meta.empty())
  {
    auto const it = base::FindIf(m_metaIds, [&type](auto const & v) { return v.first == type; });
    if (it != m_metaIds.end())
      meta = m_metadata.Set(type, m_metadataDeserializer->GetMetaById(it->second));
  }
  return meta;
}

bool FeatureType::HasMetadata(feature::Metadata::EType type)
{
  ParseMetaIds();
  if (m_metadata.Has(type))
    return true;

  return base::FindIf(m_metaIds, [&type](auto const & v) { return v.first == type; }) !=
         m_metaIds.end();
}
