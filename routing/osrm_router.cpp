#include "osrm_router.hpp"
#include "route.hpp"
#include "vehicle_model.hpp"

#include "../geometry/distance.hpp"

#include "../indexer/mercator.hpp"
#include "../indexer/index.hpp"

#include "../platform/platform.hpp"

#include "../base/logging.hpp"

#include "../3party/osrm/osrm-backend/DataStructures/SearchEngineData.h"
#include "../3party/osrm/osrm-backend/Descriptors/DescriptionFactory.h"
#include "../3party/osrm/osrm-backend/RoutingAlgorithms/ShortestPathRouting.h"


namespace routing
{

size_t const MAX_NODE_CANDIDATES = 10;

namespace
{

class Point2PhantomNode : private noncopyable
{
  m2::PointD m_point;
  OsrmFtSegMapping const & m_mapping;

  struct Candidate
  {
    double m_dist;
    uint32_t m_segIdx;
    uint32_t m_fid;
    m2::PointD m_point;

    Candidate() : m_dist(numeric_limits<double>::max()), m_fid(OsrmFtSegMapping::FtSeg::INVALID_FID) {}
  };

  size_t m_ptIdx;
  buffer_vector<Candidate, 128> m_candidates[2];
  uint32_t m_mwmId;

public:
  Point2PhantomNode(OsrmFtSegMapping const & mapping)
    : m_mapping(mapping), m_ptIdx(0), m_mwmId(numeric_limits<uint32_t>::max())
  {
  }

  void SetPoint(m2::PointD const & pt, size_t idx)
  {
    ASSERT_LESS(idx, 2, ());
    m_point = pt;
    m_ptIdx = idx;
  }

  bool HasCandidates(uint32_t idx) const
  {
    ASSERT_LESS(idx, 2, ());
    return !m_candidates[idx].empty();
  }

  void operator() (FeatureType const & ft)
  {
    static CarModel const carModel;
    if (ft.GetFeatureType() != feature::GEOM_LINE || !carModel.IsRoad(ft))
      return;

    Candidate res;

    ft.ParseGeometry(FeatureType::BEST_GEOMETRY);

    size_t const count = ft.GetPointsCount();
    ASSERT_GREATER(count, 1, ());
    for (size_t i = 1; i < count; ++i)
    {
      /// @todo Probably, we need to get exact projection distance in meters.
      m2::ProjectionToSection<m2::PointD> segProj;
      segProj.SetBounds(ft.GetPoint(i - 1), ft.GetPoint(i));

      m2::PointD const pt = segProj(m_point);
      double const d = m_point.SquareLength(pt);
      if (d < res.m_dist)
      {
        res.m_dist = d;
        res.m_fid = ft.GetID().m_offset;
        res.m_segIdx = i - 1;
        res.m_point = pt;

        if (m_mwmId == numeric_limits<uint32_t>::max())
          m_mwmId = ft.GetID().m_mwm;
        ASSERT_EQUAL(m_mwmId, ft.GetID().m_mwm, ());
      }
    }

    if (res.m_fid != OsrmFtSegMapping::FtSeg::INVALID_FID)
      m_candidates[m_ptIdx].push_back(res);
  }

  void MakeResult(OsrmRouter::FeatureGraphNodeVecT & res, size_t maxCount, uint32_t & mwmId)
  {
    mwmId = m_mwmId;
    if (mwmId == numeric_limits<uint32_t>::max())
      return;

    vector<OsrmFtSegMapping::FtSeg> segments;
    segments.resize(maxCount * 2);
    for (size_t i = 0; i < 2; ++i)
    {
      sort(m_candidates[i].begin(), m_candidates[i].end(), [] (Candidate const & r1, Candidate const & r2)
      {
        return (r1.m_dist < r2.m_dist);
      });

      size_t const n = min(m_candidates[i].size(), maxCount);
      for (size_t j = 0; j < n; ++j)
      {
        OsrmFtSegMapping::FtSeg & seg = segments[i * maxCount + j];
        Candidate const & c = m_candidates[i][j];

        seg.m_fid = c.m_fid;
        seg.m_pointStart = c.m_segIdx;
        seg.m_pointEnd = c.m_segIdx + 1;
      }
    }

    OsrmFtSegMapping::OsrmNodesT nodes;
    vector<OsrmFtSegMapping::FtSeg> scopy(segments);
    m_mapping.GetOsrmNodes(scopy, nodes);

    res.clear();
    res.resize(maxCount * 2);

    for (size_t i = 0; i < 2; ++i)
      for (size_t j = 0; j < maxCount; ++j)
      {
        size_t const idx = i * maxCount + j;

        auto it = nodes.find(segments[idx].Store());
        if (it != nodes.end())
        {
          OsrmRouter::FeatureGraphNode & node = res[idx];

          node.m_node.forward_node_id = it->second.first;
          node.m_node.reverse_node_id = it->second.second;
          node.m_seg = segments[idx];
          node.m_segPt = m_candidates[i][j].m_point;
        }
      }
  }
};

} // namespace


OsrmRouter::OsrmRouter(Index const * index, CountryFileFnT const & fn)
  : m_countryFn(fn), m_pIndex(index)
{
}

string OsrmRouter::GetName() const
{
  return "mapsme";
}

void OsrmRouter::SetFinalPoint(m2::PointD const & finalPt)
{
  m_finalPt = finalPt;
}

void OsrmRouter::CalculateRoute(m2::PointD const & startingPt, ReadyCallback const & callback)
{
  Route route(GetName());
  ResultCode code;

  try
  {
    code = CalculateRouteImpl(startingPt, m_finalPt, route);
    switch (code)
    {
    case StartPointNotFound:
      LOG(LWARNING, ("Can't find start point node"));
      break;
    case EndPointNotFound:
      LOG(LWARNING, ("Can't find end point node"));
      break;
    case PointsInDifferentMWM:
      LOG(LWARNING, ("Points are in different MWMs"));
      break;
    case RouteNotFound:
      LOG(LWARNING, ("Route not found"));
      break;

    default:
      break;
    }
  }
  catch (Reader::Exception const & e)
  {
    LOG(LERROR, ("Routing index absent or incorrect. Error while loading routing index:", e.Msg()));
    code = InternalError;
  }

  callback(route, code);
}

OsrmRouter::ResultCode OsrmRouter::CalculateRouteImpl(m2::PointD const & startPt, m2::PointD const & finalPt, Route & route)
{
  string const fName = m_countryFn(startPt);
  if (fName != m_countryFn(finalPt))
    return PointsInDifferentMWM;

  string const fPath = GetPlatform().WritablePathForFile(fName + DATA_FILE_EXTENSION + ROUTING_FILE_EXTENSION);
  if (NeedReload(fPath))
  {
    LOG(LDEBUG, ("Load routing index for file:", fPath));

    // Clear data while m_container is valid.
    m_dataFacade.Clear();
    m_mapping.Clear();

    m_container.Open(fPath);

    m_mapping.Load(m_container);
  }

  SearchEngineData engineData;
  ShortestPathRouting<DataFacadeT> pathFinder(&m_dataFacade, engineData);
  RawRouteData rawRoute;

  FeatureGraphNodeVecT graphNodes;
  uint32_t mwmId = -1;

  ResultCode code = FindPhantomNodes(fName + ".mwm", startPt, finalPt, graphNodes, MAX_NODE_CANDIDATES, mwmId);
  if (code != NoError)
    return code;

  m_mapping.Clear();
  m_dataFacade.Load(m_container);

  auto checkRouteExist = [](RawRouteData const & r)
  {
    return !(INVALID_EDGE_WEIGHT == r.shortest_path_length ||
            r.segment_end_coordinates.empty() ||
            r.source_traversed_in_reverse.empty());
  };

  ASSERT_EQUAL(graphNodes.size(), MAX_NODE_CANDIDATES * 2, ());

  size_t ni = 0, nj = 0;
  while (ni < MAX_NODE_CANDIDATES && nj < MAX_NODE_CANDIDATES)
  {
    PhantomNodes nodes;
    nodes.source_phantom = graphNodes[ni].m_node;
    nodes.target_phantom = graphNodes[nj + MAX_NODE_CANDIDATES].m_node;

    rawRoute = RawRouteData();

    if ((nodes.source_phantom.forward_node_id != INVALID_NODE_ID || nodes.source_phantom.reverse_node_id != INVALID_NODE_ID) &&
        (nodes.target_phantom.forward_node_id != INVALID_NODE_ID || nodes.target_phantom.reverse_node_id != INVALID_NODE_ID))
    {
      rawRoute.segment_end_coordinates.push_back(nodes);

      pathFinder({nodes}, {}, rawRoute);
    }

    if (!checkRouteExist(rawRoute))
    {
      ++ni;
      if (ni == MAX_NODE_CANDIDATES)
      {
        ++nj;
        ni = 0;
      }
    }
    else
      break;
  }

  m_dataFacade.Clear();
  m_mapping.Load(m_container);

  if (!checkRouteExist(rawRoute))
    return RouteNotFound;

  ASSERT_LESS(ni, MAX_NODE_CANDIDATES, ());
  ASSERT_LESS(nj, MAX_NODE_CANDIDATES, ());


  // restore route
  typedef OsrmFtSegMapping::FtSeg SegT;

  FeatureGraphNode const & sNode = graphNodes[ni];
  FeatureGraphNode const & eNode = graphNodes[nj + MAX_NODE_CANDIDATES];

  SegT const & segBegin = sNode.m_seg;
  SegT const & segEnd = eNode.m_seg;

  ASSERT(segBegin.IsValid(), ());
  ASSERT(segEnd.IsValid(), ());

  vector<m2::PointD> points;
  for (auto i : osrm::irange<std::size_t>(0, rawRoute.unpacked_path_segments.size()))
  {
    // Get all the coordinates for the computed route
    size_t const n = rawRoute.unpacked_path_segments[i].size();
    for (size_t j = 0; j < n; ++j)
    {
      PathData const & path_data = rawRoute.unpacked_path_segments[i][j];

      buffer_vector<SegT, 8> buffer;
      m_mapping.ForEachFtSeg(path_data.node, MakeBackInsertFunctor(buffer));

      auto correctFn = [&buffer] (SegT const & seg, size_t & ind)
      {
        auto const it = find_if(buffer.begin(), buffer.end(), [&seg] (OsrmFtSegMapping::FtSeg const & s)
        {
          return s.IsIntersect(seg);
        });

        ASSERT(it != buffer.end(), ());
        ind = distance(buffer.begin(), it);
      };

      //m_mapping.DumpSegmentByNode(path_data.node);

      size_t startK = 0, endK = buffer.size();
      if (j == 0)
        correctFn(segBegin, startK);
      else if (j == n - 1)
      {
        correctFn(segEnd, endK);
        ++endK;
      }

      for (size_t k = startK; k < endK; ++k)
      {
        SegT const & seg = buffer[k];

        FeatureType ft;
        Index::FeaturesLoaderGuard loader(*m_pIndex, mwmId);
        loader.GetFeature(seg.m_fid, ft);
        ft.ParseGeometry(FeatureType::BEST_GEOMETRY);

        auto startIdx = seg.m_pointStart;
        auto endIdx = seg.m_pointEnd;

        if (j == 0 && k == startK)
          startIdx = (seg.m_pointEnd > seg.m_pointStart) ? segBegin.m_pointStart : segBegin.m_pointEnd;
        else if (j == n - 1 && k == endK - 1)
          endIdx = (seg.m_pointEnd > seg.m_pointStart) ? segEnd.m_pointEnd : segEnd.m_pointStart;

        if (seg.m_pointEnd > seg.m_pointStart)
        {
          for (auto idx = startIdx; idx <= endIdx; ++idx)
            points.push_back(ft.GetPoint(idx));
        }
        else
        {
          for (auto idx = startIdx; idx > endIdx; --idx)
            points.push_back(ft.GetPoint(idx));
          points.push_back(ft.GetPoint(endIdx));
        }
      }
    }
  }

  points.front() = sNode.m_segPt;
  points.back() = eNode.m_segPt;

  route.SetGeometry(points.begin(), points.end());

  return NoError;
}

IRouter::ResultCode OsrmRouter::FindPhantomNodes(string const & fPath, m2::PointD const & startPt, m2::PointD const & finalPt,
                                                 FeatureGraphNodeVecT & res, size_t maxCount, uint32_t & mwmId)
{
  Point2PhantomNode getter(m_mapping);

  auto processPt = [&](m2::PointD const & p, size_t idx)
  {
    getter.SetPoint(p, idx);
    /// @todo Review radius of rect and read index scale.
    m_pIndex->ForEachInRectForMWM(getter, MercatorBounds::RectByCenterXYAndSizeInMeters(p, 1000.0), 17, fPath);
  };

  processPt(startPt, 0);
  if (!getter.HasCandidates(0))
    return StartPointNotFound;

  processPt(finalPt, 1);
  if (!getter.HasCandidates(1))
    return EndPointNotFound;

  getter.MakeResult(res, maxCount, mwmId);
  return NoError;
}

bool OsrmRouter::NeedReload(string const & fPath) const
{
  return (m_container.GetName() != fPath);
}

}
