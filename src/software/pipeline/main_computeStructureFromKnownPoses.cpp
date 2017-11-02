// This file is part of the AliceVision project.
// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include <aliceVision/config.hpp>
#include <aliceVision/sfm/sfm.hpp>
#include <aliceVision/sfm/pipeline/regionsIO.hpp>
#include <aliceVision/matching/IndMatch.hpp>
#include <aliceVision/system/Timer.hpp>
#include <aliceVision/system/Logger.hpp>
#include <aliceVision/system/cmdline.hpp>

#include <dependencies/stlplus3/filesystemSimplified/file_system.hpp>

#include <boost/program_options.hpp>

using namespace aliceVision;
using namespace aliceVision::sfm;
using namespace std;
namespace po = boost::program_options;

/// Compute the structure of a scene according existing camera poses.
int main(int argc, char **argv)
{
  // command-line parameters

  std::string verboseLevel = system::EVerboseLevel_enumToString(system::Logger::getDefaultVerboseLevel());
  std::string sfmDataFilename;
  std::string outSfMDataFilename;
  std::string featuresDirectory;

  // user optional parameters

  std::string describerTypesName = feature::EImageDescriberType_enumToString(feature::EImageDescriberType::SIFT);
  std::string matchesDirectory;
  std::string matchesGeometricModel = "f";

  po::options_description allParams("AliceVision ComputeStructureFromKnownPoses");

  po::options_description requiredParams("Required parameters");
  requiredParams.add_options()
    ("input,i", po::value<std::string>(&sfmDataFilename)->required(),
      "SfMData file.")
    ("featuresDirectory,f", po::value<std::string>(&featuresDirectory)->required(),
      "Path to a directory containing the extracted features.")
    ("output,o", po::value<std::string>(&outSfMDataFilename)->required(),
      "Output path for the features and descriptors files (*.feat, *.desc).");

  po::options_description optionalParams("Optional parameters");
  optionalParams.add_options()
    ("describerTypes,d", po::value<std::string>(&describerTypesName)->default_value(describerTypesName),
      feature::EImageDescriberType_informations().c_str())
    ("matchesDirectory,m", po::value<std::string>(&matchesDirectory)->default_value(matchesDirectory),
      "Path to a directory containing the matches.")
    ("matchesGeometricModel,g", po::value<std::string>(&matchesGeometricModel)->default_value(matchesGeometricModel),
      "Matches geometric Model :\n"
      "* f: fundamental matrix\n"
      "* e: essential matrix\n"
      "* h: homography matrix");

  po::options_description logParams("Log parameters");
  logParams.add_options()
    ("verboseLevel,v", po::value<std::string>(&verboseLevel)->default_value(verboseLevel),
      "verbosity level (fatal, error, warning, info, debug, trace).");

  allParams.add(requiredParams).add(optionalParams).add(logParams);

  po::variables_map vm;
  try
  {
    po::store(po::parse_command_line(argc, argv, allParams), vm);

    if(vm.count("help") || (argc == 1))
    {
      ALICEVISION_COUT(allParams);
      return EXIT_SUCCESS;
    }
    po::notify(vm);
  }
  catch(boost::program_options::required_option& e)
  {
    ALICEVISION_CERR("ERROR: " << e.what());
    ALICEVISION_COUT("Usage:\n\n" << allParams);
    return EXIT_FAILURE;
  }
  catch(boost::program_options::error& e)
  {
    ALICEVISION_CERR("ERROR: " << e.what());
    ALICEVISION_COUT("Usage:\n\n" << allParams);
    return EXIT_FAILURE;
  }

  ALICEVISION_COUT("Program called with the following parameters:");
  ALICEVISION_COUT(vm);


  // set verbose level
  system::Logger::get()->setLogLevel(verboseLevel);
  
  // Load input SfMData scene
  SfMData sfm_data;
  if (!Load(sfm_data, sfmDataFilename, ESfMData(VIEWS|INTRINSICS|EXTRINSICS))) {
    std::cerr << std::endl
      << "The input SfMData file \""<< sfmDataFilename << "\" cannot be read." << std::endl;
    return EXIT_FAILURE;
  }

  // Init the regions_type from the image describer file (used for image regions extraction)
  using namespace aliceVision::feature;
  
  // Get imageDescriberMethodType
  std::vector<EImageDescriberType> describerMethodTypes = EImageDescriberType_stringToEnums(describerTypesName);

  // Prepare the Regions provider
  RegionsPerView regionsPerView;
  if(!sfm::loadRegionsPerView(regionsPerView, sfm_data, featuresDirectory, describerMethodTypes))
  {
    std::cerr << std::endl
      << "Invalid regions." << std::endl;
    return EXIT_FAILURE;
  }

  //--
  //- Pair selection method:
  //  - geometry guided -> camera frustum intersection,
  //  - putative matches guided (photometric matches)
  //     (keep pairs that have valid Intrinsic & Pose ids).
  //--
  PairSet pairs;
  if (matchesDirectory.empty())
  {
    // No image pair provided, so we use cameras frustum intersection.
    // Build the list of connected images pairs from frustum intersections
    pairs = FrustumFilter(sfm_data).getFrustumIntersectionPairs();
  }
  else
  {
    // Load pre-computed matches
    matching::PairwiseMatches matches;
    if (!matching::Load(matches, sfm_data.GetViewsKeys(), matchesDirectory, describerMethodTypes, matchesGeometricModel))
    {
      std::cerr<< "Unable to read the matches file." << std::endl;
      return EXIT_FAILURE;
    }
    pairs = matching::getImagePairs(matches);
    // Keep only Pairs that belong to valid view indexes.
    const std::set<IndexT> valid_viewIdx = sfm_data.getValidViews();
    pairs = Pair_filter(pairs, valid_viewIdx);
  }

  aliceVision::system::Timer timer;

  // Clear previous 3D landmarks
  sfm_data.structure.clear();

  //------------------------------------------
  // Compute Structure from known camera poses
  //------------------------------------------
  StructureEstimationFromKnownPoses structure_estimator;
  structure_estimator.match(sfm_data, pairs, regionsPerView);

  // Unload descriptors before triangulation
  regionsPerView.clearDescriptors();

  // Filter matches
  structure_estimator.filter(sfm_data, pairs, regionsPerView);
  // Create 3D landmarks
  structure_estimator.triangulate(sfm_data, regionsPerView);

  RemoveOutliers_AngleError(sfm_data, 2.0);

  std::cout
    << "\nStructure estimation took (s): " << timer.elapsed() << "." << std::endl
    << "#landmark found: " << sfm_data.GetLandmarks().size() << std::endl;

  if (stlplus::extension_part(outSfMDataFilename) != "ply")
  {
    Save(sfm_data,
      stlplus::create_filespec(
        stlplus::folder_part(outSfMDataFilename),
        stlplus::basename_part(outSfMDataFilename), "ply"),
      ESfMData(ALL));
  }

  if (Save(sfm_data, outSfMDataFilename, ESfMData(ALL)))
    return EXIT_SUCCESS;
  
  std::cout << "Error while saving the sfm data!" << std::endl;
  return EXIT_FAILURE;
}
