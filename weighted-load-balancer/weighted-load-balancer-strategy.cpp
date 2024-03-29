/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2014,  Regents of the University of California,
 *                      Arizona Board of Regents,
 *                      Colorado State University,
 *                      University Pierre & Marie Curie, Sorbonne University,
 *                      Washington University in St. Louis,
 *                      Beijing Institute of Technology,
 *                      The University of Memphis
 *
 * This file is part of NFD (Named Data Networking Forwarding Daemon).
 * See AUTHORS.md for complete list of NFD authors and contributors.
 *
 * NFD is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * NFD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * NFD, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <random>
#include <algorithm>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/identity.hpp>
#include <boost/multi_index/mem_fun.hpp>

#include <boost/chrono/system_clocks.hpp>

#include <ndn-cxx/util/time.hpp>

#include "weighted-load-balancer-strategy.hpp"

#include "core/logger.hpp"
#include "table/measurements-entry.hpp"

using namespace ndn::time;
using namespace boost::multi_index;

namespace nfd {
namespace fw {

class MyPitInfo;
class MyMeasurementInfo;
class WeightedFace;

class WeightedFace
{
public:

  WeightedFace(shared_ptr<Face> face_,
               const milliseconds& delay = milliseconds(0))
    : face(face_)
    , lastDelay(delay)
  {
    calculateWeight();
  }

  bool
  operator<(const WeightedFace& other) const
  {
    if (lastDelay == other.lastDelay)
      return face->getId() < other.face->getId();

    return lastDelay < other.lastDelay;
  }

  FaceId
  getId() const
  {
    return face->getId();
  }

  static void
  modifyWeightedFaceDelay(WeightedFace& weightedFace,
                          const ndn::time::milliseconds& delay)
  {
    weightedFace.lastDelay = delay;
    weightedFace.calculateWeight();
  }

  void
  calculateWeight()
  {
    weight = (1.0 * (milliseconds::max() - lastDelay)) / milliseconds::max();
  }

  shared_ptr<Face> face;
  ndn::time::milliseconds lastDelay;
  double weight;
};

///////////////////////
// PIT entry storage //
///////////////////////

class MyPitInfo : public StrategyInfo
{
public:
  MyPitInfo()
    : creationTime(system_clock::now())
  {}

  static int constexpr
  getTypeId() { return 9970; }

  system_clock::TimePoint creationTime;
};

///////////////////////////////
// Measurement entry storage //
///////////////////////////////

class MyMeasurementInfo : public StrategyInfo
{
public:

  MyMeasurementInfo() : weightedFaces(new WeightedFaceSet) {}
  void
  updateFaceDelay(const Face& face, const milliseconds& delay);

  void
  updateStoredNextHops(const fib::NextHopList& nexthops);

  static int constexpr
  getTypeId() { return 9971; }

public:

  struct ByDelay {};
  struct ByFaceId {};

  typedef multi_index_container<
    WeightedFace,
    indexed_by<
      ordered_unique<
        tag<ByDelay>,
        identity<WeightedFace>
        >,
      hashed_unique<
        tag<ByFaceId>,
        const_mem_fun<WeightedFace, FaceId, &WeightedFace::getId>
        >
      >
    > WeightedFaceSet;

  typedef WeightedFaceSet::index<ByDelay>::type WeightedFaceSetByDelay;
  typedef WeightedFaceSet::index<ByFaceId>::type WeightedFaceSetByFaceId;

  unique_ptr<WeightedFaceSet> weightedFaces;

private:
  NFD_LOG_INCLASS_DECLARE();
};

NFD_LOG_INCLASS_DEFINE(MyMeasurementInfo, "MyMeasurementInfo");

/////////////////////////////
// Strategy Implementation //
/////////////////////////////

NFD_LOG_INIT("WeightedLoadBalancerStrategy");

const Name WeightedLoadBalancerStrategy::STRATEGY_NAME("ndn:/localhost/nfd/strategy/weighted-load-balancer");
NFD_REGISTER_STRATEGY(WeightedLoadBalancerStrategy);

WeightedLoadBalancerStrategy::WeightedLoadBalancerStrategy(Forwarder& forwarder,
                                                           const Name& name)
  : Strategy(forwarder, name)
{
}

WeightedLoadBalancerStrategy::~WeightedLoadBalancerStrategy()
{
}

void
WeightedLoadBalancerStrategy::afterReceiveInterest(const Face& inFace,
                                                   const Interest& interest,
                                                   shared_ptr<fib::Entry> fibEntry,
                                                   shared_ptr<pit::Entry> pitEntry)
{
  NFD_LOG_TRACE("Received Interest: " << interest.getName());

  const auto suppression = m_retxSuppression.decide(inFace, interest, *pitEntry);

  NFD_LOG_DEBUG("retx decision: " << suppression);

  if (suppression == RetxSuppression::FORWARD ||
      suppression == RetxSuppression::SUPPRESS)
    {
      demoteFace(pitEntry);
    }

  // create timer information and attach to PIT entry
  auto pitEntryInfo = myGetOrCreateMyPitInfo(pitEntry);
  auto measurementsEntryInfo = myGetOrCreateMyMeasurementInfo(fibEntry);

  // reconcile differences between incoming nexthops and those stored
  // on our custom measurement entry info
  measurementsEntryInfo->updateStoredNextHops(fibEntry->getNextHops());

  auto selectedFace = selectOutgoingFace(inFace,
                                         interest,
                                         measurementsEntryInfo,
                                         pitEntry);

  if (selectedFace == nullptr)
    {
      rejectPendingInterest(pitEntry);
      return;
    }

  sendInterest(pitEntry, selectedFace);
}


void
WeightedLoadBalancerStrategy::beforeSatisfyInterest(shared_ptr<pit::Entry> pitEntry,
                                                    const Face& inFace,
                                                    const Data& data)
{
  NFD_LOG_TRACE("Received Data: " << data.getName());
  auto pitInfo = pitEntry->getStrategyInfo<MyPitInfo>();

  // No start time available, cannot compute delay for this retrieval
  if (pitInfo == nullptr)
    {
      NFD_LOG_TRACE("No start time available for Data " << data.getName());
      return;
    }

  const milliseconds delay =
    duration_cast<milliseconds>(system_clock::now() - pitInfo->creationTime);

  NFD_LOG_TRACE("Computed delay of: " << system_clock::now() << " - " << pitInfo->creationTime << " = " << delay);

  auto& accessor = getMeasurements();

  // Update Face delay measurements and entry lifetimes owned
  // by this strategy while walking up the NameTree
  auto measurementsEntry = accessor.get(*pitEntry);
  if (measurementsEntry != nullptr)
    {
      NFD_LOG_TRACE("accessor returned measurements entry " << measurementsEntry->getName()
                   << " for " << pitEntry->getName());
    }
  else
    {
      NFD_LOG_WARN ("accessor returned invalid measurements entry for " << pitEntry->getName());
    }

  while (measurementsEntry != nullptr)
    {
      auto measurementsEntryInfo = measurementsEntry->getStrategyInfo<MyMeasurementInfo>();

      if (measurementsEntryInfo != nullptr)
        {
          accessor.extendLifetime(*measurementsEntry, seconds(16));
          measurementsEntryInfo->updateFaceDelay(inFace, delay);
        }

      measurementsEntry = accessor.getParent(*measurementsEntry);
    }
}


void
WeightedLoadBalancerStrategy::beforeExpirePendingInterest(shared_ptr<pit::Entry> pitEntry)
{
  demoteFace(pitEntry);
}


/////////////////////////////
// Strategy Helper Methods //
/////////////////////////////

static inline bool
isEligibleFace(const shared_ptr<pit::Entry>& pitEntry,
               const Face& downstream,
               const Face& upstream)
{
  return downstream.getId() != upstream.getId() &&
    !pitEntry->violatesScope(upstream);
}

shared_ptr<Face>
WeightedLoadBalancerStrategy::selectOutgoingFace(const Face& inFace,
                                                 const Interest& interest,
                                                 shared_ptr<MyMeasurementInfo>& measurementsEntryInfo,
                                                 shared_ptr<pit::Entry>& pitEntry)
{
  auto& facesById =
    measurementsEntryInfo->weightedFaces->get<MyMeasurementInfo::ByFaceId>();

  std::vector<uint64_t> faceIds;
  std::vector<double> weights;

  faceIds.reserve(facesById.size() + 1);
  weights.reserve(facesById.size());

  for (auto faceWeight : facesById)
    {
      faceIds.push_back(faceWeight.face->getId());
      weights.push_back(faceWeight.weight);
    }

  faceIds.push_back(INVALID_FACEID);

  std::piecewise_constant_distribution<> dist(faceIds.begin(),
                                              faceIds.end(),
                                              weights.begin());

  const uint64_t selection = dist(m_randomGenerator);

  NFD_LOG_DEBUG("selected value: " << selection);

  auto faceEntry = facesById.begin();
  uint64_t firstMatchIndex = INVALID_FACEID;
  auto firstMatchFaceEntry = facesById.end();
  for (uint64_t i = 0; i < facesById.size() - 1; i++)
    {
      if (faceIds[i] <= selection && selection < faceIds[i + 1])
        {
          if (isEligibleFace(pitEntry, inFace, *faceEntry->face))
            {
              NFD_LOG_DEBUG("selected FaceID: " << faceEntry->face->getId());
              return faceEntry->face->shared_from_this();
            }
          else if (i < firstMatchIndex)
            {
              firstMatchIndex = i;
              firstMatchFaceEntry = faceEntry;
            }
        }
      ++faceEntry;
    }

  if (faceEntry != facesById.end() &&
      isEligibleFace(pitEntry, inFace, *faceEntry->face))
    {
      NFD_LOG_DEBUG("selected FaceID: " << faceEntry->face->getId());
      return faceEntry->face->shared_from_this();
    }


  // wrap around and try faces up to, but not including, first match
  faceEntry = facesById.begin();
  const auto limit = std::min(firstMatchIndex, static_cast<uint64_t>(facesById.size()));
  for (uint64_t i = 0; i < limit; i++)
    {
      if (isEligibleFace(pitEntry, inFace, *faceEntry->face))
        {
          NFD_LOG_DEBUG("selected FaceID: " << faceEntry->face->getId());
          return faceEntry->face->shared_from_this();
        }
      ++faceEntry;
    }


  NFD_LOG_WARN("no face selected for forwarding");
  return nullptr;
}


shared_ptr<MyPitInfo>
WeightedLoadBalancerStrategy::myGetOrCreateMyPitInfo(const shared_ptr<pit::Entry>& entry)
{
  auto pitEntryInfo = entry->getStrategyInfo<MyPitInfo>();

  if (pitEntryInfo == nullptr)
    {
      pitEntryInfo = make_shared<MyPitInfo>();
      entry->setStrategyInfo(pitEntryInfo);
    }

  return pitEntryInfo;
}

shared_ptr<MyMeasurementInfo>
WeightedLoadBalancerStrategy::myGetOrCreateMyMeasurementInfo(const shared_ptr<fib::Entry>& entry)
{
  BOOST_ASSERT(entry != nullptr);

  //this could return null?
  auto measurementsEntry = getMeasurements().get(*entry);

  BOOST_ASSERT(measurementsEntry != nullptr);

  auto measurementsEntryInfo = measurementsEntry->getStrategyInfo<MyMeasurementInfo>();

  if (measurementsEntryInfo == nullptr)
    {
      measurementsEntryInfo = make_shared<MyMeasurementInfo>();
      measurementsEntry->setStrategyInfo(measurementsEntryInfo);
    }

  return measurementsEntryInfo;
}

void
WeightedLoadBalancerStrategy::demoteFace(shared_ptr<pit::Entry> pitEntry)
{
  MeasurementsAccessor& accessor = this->getMeasurements();
  auto measurementsEntry = accessor.get(*pitEntry);

  while (measurementsEntry != nullptr)
    {
      auto measurementsEntryInfo =
        measurementsEntry->getStrategyInfo<MyMeasurementInfo>();

      if (measurementsEntryInfo != nullptr)
        {
          accessor.extendLifetime(*measurementsEntry, seconds(16));
          for (auto& entry : pitEntry->getOutRecords())
            {
              measurementsEntryInfo->updateFaceDelay(*(entry.getFace()),
                                                     milliseconds::max());
            }
        }

      measurementsEntry = accessor.getParent(*measurementsEntry);
    }
}

///////////////////////////////////////
// MyMeasurementInfo Implementations //
///////////////////////////////////////

void
MyMeasurementInfo::updateFaceDelay(const Face& face, const milliseconds& delay)
{
  auto& facesById = weightedFaces->get<MyMeasurementInfo::ByFaceId>();
  auto faceEntry = facesById.find(face.getId());

  if (faceEntry != facesById.end())
    {
      // NFD_LOG_TRACE("Recording delay of " << delay.count()
      //               << "ms (diff: " << (delay - faceEntry->lastDelay).count()
      //               << "ms) for FaceId: " << inFace.getId());

      auto oldWeight = faceEntry->weight;
      auto result = facesById.modify(faceEntry,
                                     bind(&WeightedFace::modifyWeightedFaceDelay,
                                          _1,
                                          boost::cref(delay)));

      NFD_LOG_DEBUG("updated weight: " << oldWeight << " -> " << faceEntry->weight
                    << " modify: " << result << "\n");
    }
}

void
MyMeasurementInfo::updateStoredNextHops(const fib::NextHopList& nexthops)
{
  auto updatedFaceSet = new MyMeasurementInfo::WeightedFaceSet;
  auto& facesById = weightedFaces->get<MyMeasurementInfo::ByFaceId>();
  auto& updatedFacesById = updatedFaceSet->get<MyMeasurementInfo::ByFaceId>();

  for (auto& hop : nexthops)
    {
      BOOST_ASSERT(hop.getFace() != nullptr);
      auto& face = hop.getFace();

      auto weightedIt = facesById.find(face->getId());
      if (weightedIt == facesById.end())
        {
          updatedFacesById.insert(WeightedFace(face));
        }
      else
        {
          updatedFacesById.insert(*weightedIt);
        }
    }

  weightedFaces.reset(updatedFaceSet);
}

} // namespace fw
} // namespace nfd
