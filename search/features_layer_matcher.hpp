#pragma once

#include "search/cancel_exception.hpp"
#include "search/cbv.hpp"
#include "search/features_layer.hpp"
#include "search/house_numbers_matcher.hpp"
#include "search/model.hpp"
#include "search/mwm_context.hpp"
#include "search/point_rect_matcher.hpp"
#include "search/projection_on_street.hpp"
#include "search/reverse_geocoder.hpp"
#include "search/stats_cache.hpp"
#include "search/street_vicinity_loader.hpp"

#include "indexer/feature.hpp"
#include "indexer/feature_algo.hpp"
#include "indexer/feature_impl.hpp"
#include "indexer/features_vector.hpp"
#include "indexer/ftypes_matcher.hpp"
#include "indexer/mwm_set.hpp"

#include "geometry/mercator.hpp"
#include "geometry/point2d.hpp"
#include "geometry/rect2d.hpp"

#include "base/cancellable.hpp"
#include "base/logging.hpp"
#include "base/macros.hpp"
#include "base/stl_helpers.hpp"
#include "base/string_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

class DataSource;

namespace search
{
// This class performs pairwise intersection between two layers of
// features, where the first (child) layer is geographically smaller
// than the second (parent) one.  It emits all pairs
// (feature-from-child-layer, feature-from-parent-layer) of matching
// features, where feature-from-child-layer belongs-to
// feature-from-parent-layer.  Belongs-to is a partial relation on
// features, and has different meaning for different search classes:
//
// * BUILDING belongs-to STREET iff the building is located on the street;
// * BUILDING belongs-to CITY iff the building is located in the city;
// * POI belongs-to BUILDING iff the poi is (roughly) located near or inside the building;
// * STREET belongs-to CITY iff the street is (roughly) located in the city;
// * etc.
//
// NOTE: this class *IS NOT* thread-safe.
class FeaturesLayerMatcher
{
public:
  static uint32_t const kInvalidId = std::numeric_limits<uint32_t>::max();
  static int constexpr kBuildingRadiusMeters = 50;
  static int constexpr kStreetRadiusMeters = 100;

  FeaturesLayerMatcher(DataSource const & dataSource, base::Cancellable const & cancellable);
  void SetContext(MwmContext * context);
  void SetPostcodes(CBV const * postcodes);

  template <typename Fn>
  void Match(FeaturesLayer const & child, FeaturesLayer const & parent, Fn && fn)
  {
    if (child.m_type >= parent.m_type)
      return;
    switch (parent.m_type)
    {
    case Model::TYPE_POI:
    case Model::TYPE_CITY:
    case Model::TYPE_VILLAGE:
    case Model::TYPE_STATE:
    case Model::TYPE_COUNTRY:
    case Model::TYPE_UNCLASSIFIED:
    case Model::TYPE_COUNT:
      ASSERT(false, ("Invalid parent layer type:", parent.m_type));
      break;
    case Model::TYPE_BUILDING:
      ASSERT_EQUAL(child.m_type, Model::TYPE_POI, ());
      MatchPOIsWithBuildings(child, parent, std::forward<Fn>(fn));
      break;
    case Model::TYPE_STREET:
      ASSERT(child.m_type == Model::TYPE_POI || child.m_type == Model::TYPE_BUILDING,
             ("Invalid child layer type:", child.m_type));
      if (child.m_type == Model::TYPE_POI)
        MatchPOIsWithStreets(child, parent, std::forward<Fn>(fn));
      else
        MatchBuildingsWithStreets(child, parent, std::forward<Fn>(fn));
      break;
    }
  }

  void OnQueryFinished();

private:
  template <typename Fn>
  void MatchPOIsWithBuildings(FeaturesLayer const & child, FeaturesLayer const & parent, Fn && fn)
  {
    // Following code initially loads centers of POIs and then, for
    // each building, tries to find all POIs located at distance less
    // than kBuildingRadiusMeters.

    ASSERT_EQUAL(child.m_type, Model::TYPE_POI, ());
    ASSERT_EQUAL(parent.m_type, Model::TYPE_BUILDING, ());

    auto const & pois = *child.m_sortedFeatures;
    auto const & buildings = *parent.m_sortedFeatures;

    BailIfCancelled(m_cancellable);

    std::vector<PointRectMatcher::PointIdPair> poiCenters;
    poiCenters.reserve(pois.size());

    for (size_t i = 0; i < pois.size(); ++i)
    {
      if (auto poiFt = GetByIndex(pois[i]))
      {
        poiCenters.emplace_back(feature::GetCenter(*poiFt, FeatureType::WORST_GEOMETRY),
                                i /* id */);
      }
    }

    std::vector<PointRectMatcher::RectIdPair> buildingRects;
    buildingRects.reserve(buildings.size());
    for (size_t i = 0; i < buildings.size(); ++i)
    {
      auto buildingFt = GetByIndex(buildings[i]);
      if (!buildingFt)
        continue;

      if (buildingFt->GetFeatureType() == feature::GEOM_POINT)
      {
        auto const center = feature::GetCenter(*buildingFt, FeatureType::WORST_GEOMETRY);
        buildingRects.emplace_back(
            MercatorBounds::RectByCenterXYAndSizeInMeters(center, kBuildingRadiusMeters),
            i /* id */);
      }
      else
      {
        buildingRects.emplace_back(buildingFt->GetLimitRect(FeatureType::WORST_GEOMETRY),
                                   i /* id */);
      }
    }

    PointRectMatcher::Match(poiCenters, buildingRects, PointRectMatcher::RequestType::Any,
                            [&](size_t poiId, size_t buildingId) {
                              ASSERT_LESS(poiId, pois.size(), ());
                              ASSERT_LESS(buildingId, buildings.size(), ());
                              fn(pois[poiId], buildings[buildingId]);
                            });

    if (!parent.m_hasDelayedFeatures)
      return;

    // |buildings| doesn't contain buildings matching by house number,
    // so following code reads buildings in POIs vicinities and checks
    // house numbers.
    std::vector<house_numbers::Token> queryParse;
    ParseQuery(parent.m_subQuery, parent.m_lastTokenIsPrefix, queryParse);
    if (queryParse.empty())
      return;

    for (size_t i = 0; i < pois.size(); ++i)
    {
      m_context->ForEachFeature(
          MercatorBounds::RectByCenterXYAndSizeInMeters(poiCenters[i].m_point, kBuildingRadiusMeters),
          [&](FeatureType & ft) {
            if (m_postcodes && !m_postcodes->HasBit(ft.GetID().m_index))
              return;
            if (house_numbers::HouseNumbersMatch(strings::MakeUniString(ft.GetHouseNumber()),
                                                 queryParse))
            {
              double const distanceM =
                  MercatorBounds::DistanceOnEarth(feature::GetCenter(ft), poiCenters[i].m_point);
              if (distanceM < kBuildingRadiusMeters)
                fn(pois[i], ft.GetID().m_index);
            }
          });
    }
  }

  template <typename Fn>
  void MatchPOIsWithStreets(FeaturesLayer const & child, FeaturesLayer const & parent, Fn && fn)
  {
    BailIfCancelled(m_cancellable);

    ASSERT_EQUAL(child.m_type, Model::TYPE_POI, ());
    ASSERT_EQUAL(parent.m_type, Model::TYPE_STREET, ());

    auto const & pois = *child.m_sortedFeatures;
    auto const & streets = *parent.m_sortedFeatures;

    std::vector<PointRectMatcher::PointIdPair> poiCenters;
    poiCenters.reserve(pois.size());

    for (size_t i = 0; i < pois.size(); ++i)
    {
      if (auto poiFt = GetByIndex(pois[i]))
      {
        poiCenters.emplace_back(feature::GetCenter(*poiFt, FeatureType::WORST_GEOMETRY),
                                i /* id */);
      }
    }

    std::vector<PointRectMatcher::RectIdPair> streetRects;
    streetRects.reserve(streets.size());

    std::vector<ProjectionOnStreetCalculator> streetProjectors;
    streetProjectors.reserve(streets.size());

    for (size_t i = 0; i < streets.size(); ++i)
    {
      auto streetFt = GetByIndex(streets[i]);
      if (!streetFt)
        continue;

      streetFt->ParseGeometry(FeatureType::WORST_GEOMETRY);

      m2::RectD inflationRect;
      // Any point is good enough here, and feature::GetCenter would re-read the geometry.
      if (streetFt->GetPointsCount() > 0)
      {
        inflationRect = MercatorBounds::RectByCenterXYAndSizeInMeters(streetFt->GetPoint(0),
                                                                      0.5 * kStreetRadiusMeters);
      }

      for (size_t j = 0; j + 1 < streetFt->GetPointsCount(); ++j)
      {
        auto const & p1 = streetFt->GetPoint(j);
        auto const & p2 = streetFt->GetPoint(j + 1);
        m2::RectD rect(p1, p2);
        rect.Inflate(inflationRect.SizeX(), inflationRect.SizeY());
        streetRects.emplace_back(rect, i /* id */);
      }

      std::vector<m2::PointD> streetPoints;
      streetPoints.reserve(streetFt->GetPointsCount());
      for (size_t j = 0; j < streetFt->GetPointsCount(); ++j)
        streetPoints.emplace_back(streetFt->GetPoint(j));
      streetProjectors.emplace_back(streetPoints);
    }

    BailIfCancelled(m_cancellable);
    PointRectMatcher::Match(poiCenters, streetRects, PointRectMatcher::RequestType::All,
                            [&](size_t poiId, size_t streetId) {
                              ASSERT_LESS(poiId, pois.size(), ());
                              ASSERT_LESS(streetId, streets.size(), ());
                              auto const & poiCenter = poiCenters[poiId].m_point;
                              ProjectionOnStreet proj;
                              if (streetProjectors[streetId].GetProjection(poiCenter, proj) &&
                                  proj.m_distMeters < kStreetRadiusMeters)
                              {
                                fn(pois[poiId], streets[streetId]);
                              }
                            });
  }

  template <typename Fn>
  void MatchBuildingsWithStreets(FeaturesLayer const & child, FeaturesLayer const & parent,
                                 Fn && fn)
  {
    ASSERT_EQUAL(child.m_type, Model::TYPE_BUILDING, ());
    ASSERT_EQUAL(parent.m_type, Model::TYPE_STREET, ());

    auto const & buildings = *child.m_sortedFeatures;
    auto const & streets = *parent.m_sortedFeatures;

    // When all buildings are in |buildings| and the number of
    // buildings less than the number of streets, it's probably faster
    // to check nearby streets for each building instead of street
    // vicinities loading.
    if (!child.m_hasDelayedFeatures && buildings.size() < streets.size())
    {
      for (uint32_t const houseId : buildings)
      {
        uint32_t const streetId = GetMatchingStreet(houseId);
        if (std::binary_search(streets.begin(), streets.end(), streetId))
          fn(houseId, streetId);
      }
      return;
    }

    std::vector<house_numbers::Token> queryParse;
    ParseQuery(child.m_subQuery, child.m_lastTokenIsPrefix, queryParse);

    uint32_t numFilterInvocations = 0;
    auto houseNumberFilter = [&](uint32_t id, std::unique_ptr<FeatureType> & feature,
                                 bool & loaded) -> bool {
      ++numFilterInvocations;
      if ((numFilterInvocations & 0xFF) == 0)
        BailIfCancelled(m_cancellable);

      if (std::binary_search(buildings.begin(), buildings.end(), id))
        return true;

      if (m_postcodes && !m_postcodes->HasBit(id))
        return false;

      if (!loaded)
      {
        feature = GetByIndex(id);
        loaded = feature != nullptr;
      }

      if (!loaded)
        return false;

      if (!child.m_hasDelayedFeatures)
        return false;

      strings::UniString const houseNumber(strings::MakeUniString(feature->GetHouseNumber()));
      return house_numbers::HouseNumbersMatch(houseNumber, queryParse);
    };

    std::unordered_map<uint32_t, bool> cache;
    auto cachingHouseNumberFilter = [&](uint32_t id, std::unique_ptr<FeatureType> & feature,
                                        bool & loaded) -> bool {
      auto const it = cache.find(id);
      if (it != cache.cend())
        return it->second;
      bool const result = houseNumberFilter(id, feature, loaded);
      cache[id] = result;
      return result;
    };

    ProjectionOnStreet proj;
    for (uint32_t streetId : streets)
    {
      BailIfCancelled(m_cancellable);
      StreetVicinityLoader::Street const & street = m_loader.GetStreet(streetId);
      if (street.IsEmpty())
        continue;

      for (uint32_t houseId : street.m_features)
      {
        std::unique_ptr<FeatureType> feature;
        bool loaded = false;
        if (!cachingHouseNumberFilter(houseId, feature, loaded))
          continue;

        if (!loaded)
          feature = GetByIndex(houseId);

        if (!feature)
          continue;

        if (GetMatchingStreet(*feature) == streetId)
          fn(houseId, streetId);
      }
    }
  }

  // Returns id of a street feature corresponding to a |houseId|/|houseFeature|, or
  // kInvalidId if there're not such street.
  uint32_t GetMatchingStreet(uint32_t houseId);
  uint32_t GetMatchingStreet(FeatureType & houseFeature);

  using Street = ReverseGeocoder::Street;
  using Streets = std::vector<Street>;

  Streets const & GetNearbyStreets(FeatureType & feature);

  std::unique_ptr<FeatureType> GetByIndex(uint32_t id) const
  {
    /// @todo Add Cache for feature id -> (point, name / house number).
    auto res = m_context->GetFeature(id);

    // It may happen to features deleted by the editor. We do not get them from EditableDataSource
    // but we still have ids of these features in the search index.
    if (!res)
      LOG(LWARNING, ("GetFeature() returned false.", id));
    return res;
  }

  MwmContext * m_context;

  CBV const * m_postcodes;

  ReverseGeocoder m_reverseGeocoder;

  // Cache of streets in a feature's vicinity. All lists in the cache
  // are ordered by distance from the corresponding feature.
  Cache<uint32_t, Streets> m_nearbyStreetsCache;

  // Cache of correct streets for buildings. Current search algorithm
  // supports only one street for a building, whereas buildings can be
  // located on multiple streets.
  Cache<uint32_t, uint32_t> m_matchingStreetsCache;

  StreetVicinityLoader m_loader;
  base::Cancellable const & m_cancellable;
};
}  // namespace search
