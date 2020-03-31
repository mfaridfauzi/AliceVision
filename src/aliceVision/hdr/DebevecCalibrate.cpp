// This file is part of the AliceVision project.
// Copyright (c) 2019 AliceVision contributors.
// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "DebevecCalibrate.hpp"
#include <iostream>
#include <fstream>
#include <cassert>
#include <aliceVision/alicevision_omp.hpp>
#include <aliceVision/system/Logger.hpp>
#include <aliceVision/image/all.hpp>
#include <aliceVision/image/io.hpp>
#include <OpenImageIO/imagebufalgo.h>
#include "sampling.hpp"


namespace aliceVision {
namespace hdr {

using T = Eigen::Triplet<double>;

bool DebevecCalibrate::process(const std::vector< std::vector<std::string>> & imagePathsGroups,
                               const std::size_t channelQuantization,
                               const std::vector<std::vector<float> > &times,
                               int nbPoints,
                               int calibrationDownscale,
                               bool fisheye,
                               const rgbCurve &weight,
                               float lambda,
                               rgbCurve &response)
{
  // Always 3 channels for the input images
  static const std::size_t channelsCount = 3;

  /*Extract samples*/
  ALICEVISION_LOG_DEBUG("Extract color samples");
  std::vector<std::vector<ImageSamples>> samples;
  extractSamples(samples, imagePathsGroups, times, nbPoints, calibrationDownscale, fisheye);
  
  /*
  Count really extracted amount of points
  (observed in multiple brackets)
  */
  size_t countPoints = 0;
  size_t countMeasures = 0;
  std::vector<size_t> countPointPerGroup;
  for (size_t groupId = 0; groupId < samples.size(); groupId++) {
    
    size_t count = 0;
    std::vector<ImageSamples> & group = samples[groupId];  
    if (group.size() > 0) {
      count = group[0].colors.size();
    }

    countPointPerGroup.push_back(count);
    countPoints += count;
    countMeasures += count * group.size();
  }

  // Initialize response
  response = rgbCurve(channelQuantization);

  // Store intermediate data for all three channels
  Vec b_array[channelsCount];
  std::vector<T> tripletList_array[channelsCount];

  // Initialize intermediate buffers
  for(unsigned int channel=0; channel < channelsCount; ++channel)
  {
    Vec & b = b_array[channel];
    b = Vec::Zero(countMeasures + channelQuantization + 1);
    std::vector<T> & tripletList = tripletList_array[channel];
  }

  size_t count = 0;
  size_t previousSamplesCount = 0;
  for (size_t groupId = 0; groupId < samples.size(); groupId++) {
    
    std::vector<ImageSamples> & group = samples[groupId];
          
    for (size_t bracketId = 0; bracketId < group.size() - 1; bracketId++) {
            
      ImageSamples & bracket_cur = group[bracketId];
      
      for (size_t sampleId = 0; sampleId < bracket_cur.colors.size(); sampleId++) {
        
        for (int channel = 0; channel < channelsCount; channel++) {
          float sample = bracket_cur.colors[sampleId](channel);
          
          float w_ij = weight(sample, channel);
          const float time = std::log(bracket_cur.exposure);
          std::size_t index = std::round(sample * (channelQuantization - 1));

          tripletList_array[channel].push_back(T(count, index, w_ij));
          tripletList_array[channel].push_back(T(count, channelQuantization + previousSamplesCount + sampleId, -w_ij));
          b_array[channel][count] = w_ij * time;
        }

        count++;
      }
    }

    previousSamplesCount += countPointPerGroup[groupId];
  }       

  /**
   * Fix scale
   * Enforce f(0.5) = 0.0
   */
  for (int channel = 0; channel < channelsCount; channel++)
  {
    tripletList_array[channel].push_back(T(count, std::floor(channelQuantization/2), 1.f));
  }
  count += 1;

  
  /* Make sure the discrete response curve has a minimal second derivative */
  for (std::size_t k = 0; k < channelQuantization - 2; k++)
  {
    for (int channel = 0; channel < channelsCount; channel++)
    {
      /*
      Simple derivatives of second derivative wrt to the k+1 element
      f''(x) = f(x + 1) - 2 * f(x) + f(x - 1)
      */
      float w = weight.getValue(k + 1, channel);
      
      tripletList_array[channel].push_back(T(count, k, lambda * w));
      tripletList_array[channel].push_back(T(count, k + 1, - 2.f * lambda * w));
      tripletList_array[channel].push_back(T(count, k + 2, lambda * w));
    }

    count++;
  }


  for (int channel = 0; channel < channelsCount; channel ++)
  {
    sMat A(count, channelQuantization + countPoints);
    
    A.setFromTriplets(tripletList_array[channel].begin(), tripletList_array[channel].end());
    b_array[channel].conservativeResize(count);

    // solve the system using SVD decomposition
    A.makeCompressed();
    Eigen::SparseQR<sMat, Eigen::COLAMDOrdering<int>> solver;
    solver.compute(A);

    // Check solver failure
    if (solver.info() != Eigen::Success)
    {
      return false;
    }

    Vec x = solver.solve(b_array[channel]);

    // Check solver failure
    if(solver.info() != Eigen::Success)
    {
      return false;
    }

    // Copy the result to the response curve
    for(std::size_t k = 0; k < channelQuantization; ++k)
    {
      response.setValue(k, channel, x(k));
    }
  }

  return true;
}

} // namespace hdr
} // namespace aliceVision
