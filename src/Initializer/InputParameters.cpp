#include "InputParameters.hpp"

#include "InputAux.hpp"
#include "utils/logger.h"
#include "utils/stringutils.h"

#include "Geometry/MeshReader.h"
#include "SourceTerm/Manager.h"
#include "Checkpoint/Backend.h"
#include "time_stepping/LtsWeights/WeightsFactory.h"
#include "Parallel/MPI.h"

#include <yaml-cpp/yaml.h>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <vector>

using namespace seissol::initializer::parameters;

// converts a string to lower case, and trims it.
static void sanitize(std::string& input) {
  utils::StringUtils::trim(input);
  utils::StringUtils::toLower(input);
}

// A small helper class which reads a YAML node dictionary. It keeps track of all items that have
// been read and reports all values which are not used or not used anymore.
// TODO(David): maybe make the reader more tree-like (i.e. keep a central set on which nodes have
// been visited), and output all non-understood values at the end and not between sections
class ParameterReader {
  public:
  ParameterReader(const YAML::Node& node, bool empty) : node(node), empty(empty) {}

  template <typename T>
  T readWithDefault(const std::string& field, const T& defaultValue) {
    T value = defaultValue;
    if (hasField(field)) {
      value = readUnsafe<T>(field);
    } else {
      logDebug(seissol::MPI::mpi.rank())
          << "The field" << field << "was not specified, using fallback.";
    }
    return value;
  }

  // TODO(David): long-term (if we don't switch to another format first), merge readWithDefaultEnum
  // with readWithDefaultStringEnum, i.e. allow both numerical and textual values for an enum (can
  // we maybe auto-generate a parser from an enum definition?)
  template <typename T>
  T readWithDefaultEnum(const std::string& field,
                        const T& defaultValue,
                        const std::unordered_set<T>& validValues) {
    int value = readWithDefault(field, static_cast<int>(defaultValue));
    if (validValues.find(static_cast<T>(value)) == validValues.end()) {
      logError() << "The field" << field << "had an invalid enum value:" << value;
    }
    return static_cast<T>(value);
  }

  template <typename T>
  T readWithDefaultStringEnum(const std::string& field,
                              const std::string& defaultValue,
                              const std::unordered_map<std::string, T>& validValues) {
    std::string value = readWithDefault(field, defaultValue); // TODO(David): sanitize string
    sanitize(value);
    if (validValues.find(value) == validValues.end()) {
      logError() << "The field" << field << "had an invalid enum value:" << value;
    }
    return validValues.at(value);
  }

  template <typename T>
  T readOrFail(const std::string& field, const std::string& failMessage) {
    if (hasField(field)) {
      return readUnsafe<T>(field);
    } else {
      logError() << "The field" << field << "was not found, but it is required.";
      return T(); // unreachable. TODO(David): use compiler hint instead
    }
  }

  void warnDeprecatedSingle(const std::string& field) {
    if (hasField(field)) {
      visited.emplace(field);
      logInfo(seissol::MPI::mpi.rank())
          << "The field" << field
          << "is no longer in use. You may safely remove it from your parameters file.";
    }
  }

  void warnDeprecated(const std::vector<std::string>& fields) {
    for (const auto& field : fields) {
      warnDeprecatedSingle(field);
    }
  }

  void warnUnknown() {
    for (const auto& subnodes : node) {
      auto field = subnodes.first.as<std::string>();
      if (visited.find(field) == visited.end()) {
        logWarning(seissol::MPI::mpi.rank()) << "The field" << field << "is not known to SeisSol.";
      }
    }
  }

  void markUnused(const std::string& field) {
    logDebug(seissol::MPI::mpi.rank()) << "The field" << field << "is ignored (if it is found).";
    visited.emplace(field);
  }

  ParameterReader readSubNode(const std::string& subnodeName) {
    visited.emplace(subnodeName);
    logDebug(seissol::MPI::mpi.rank()) << "Entering section" << subnodeName;
    if (hasField(subnodeName)) {
      return ParameterReader(node[subnodeName], false);
    } else {
      logDebug(seissol::MPI::mpi.rank())
          << "Section" << subnodeName
          << "not found in the given parameter file. Using an empty reader.";
      return ParameterReader(node[subnodeName], true);
    }
  }

  bool hasField(const std::string& field) { return !empty && node[field]; }

  private:
  template <typename T>
  T readUnsafe(const std::string& field) {
    visited.emplace(field);
    logDebug(seissol::MPI::mpi.rank()) << "The field" << field << "was read.";
    try {
      // booleans are stored as integers
      if constexpr (std::is_same<T, bool>::value) {
        return node[field].as<int>() > 0;
      } else {
        return node[field].as<T>();
      }
    } catch (std::exception& e) {
      logError() << "Error while reading field" << field << ":" << e.what();
      return T(); // unreachable. TODO(David): use compiler hint instead
    }
  }

  bool empty;
  YAML::Node node; // apparently the YAML nodes use a reference semantic. Hence, we do it like this.
  std::unordered_set<std::string> visited;
};

static void readModel(ParameterReader& baseReader, SeisSolParameters& seissolParams) {
  auto reader = baseReader.readSubNode("equations");

  seissolParams.model.configFileName = reader.readWithDefault("configfilename", std::string(""));
  // the config file supersedes the material file
  if (seissolParams.model.configFileName == "") {
    seissolParams.model.materialFileName = reader.readOrFail<std::string>(
        "materialfilename", "No material nor configuration file given.");
  } else {
    reader.markUnused("materialfilename");
  }
  seissolParams.model.boundaryFileName =
      reader.readWithDefault("boundaryfilename", std::string(""));
  seissolParams.model.hasBoundaryFile = seissolParams.model.boundaryFileName != "";

  seissolParams.model.gravitationalAcceleration =
      reader.readWithDefault("gravitationalacceleration", 9.81);

  seissolParams.model.plasticity = reader.readWithDefault("plasticity", false);
  seissolParams.model.tv = reader.readWithDefault("tv", 0.1);
  seissolParams.model.useCellHomogenizedMaterial =
      reader.readWithDefault("usecellhomogenizedmaterial", true);

#if NUMBER_OF_RELAXATION_MECHANISMS > 0
  seissolParams.model.freqCentral = reader.readOrFail<double>(
      "freqcentral", "equations.freqcentral is needed for the attenuation fitting.");
  seissolParams.model.freqRatio = reader.readOrFail<double>(
      "freqratio", "equations.freqratio is needed for the attenuation fitting.");
#else
  reader.markUnused("freqcentral");
  reader.markUnused("freqratio");
#endif

  reader.warnDeprecated({"adjoint", "adjfilename", "anisotropy"});
  reader.warnUnknown();
}

static void readBoundaries(ParameterReader& baseReader, SeisSolParameters& seissolParams) {
  auto reader = baseReader.readSubNode("boundaries");
  seissolParams.dynamicRupture.hasFault = reader.readWithDefault("bc_dr", false);

  // TODO(David): ? port DR reading here, maybe.

  reader.warnDeprecated({"bc_fs", "bc_nc", "bc_if", "bc_of", "bc_pe"});
  reader.warnUnknown();
}

static void readMesh(ParameterReader& baseReader, SeisSolParameters& seissolParams) {
  auto reader = baseReader.readSubNode("meshnml");

  seissolParams.mesh.meshFileName =
      reader.readOrFail<std::string>("meshfile", "No mesh file given.");
  seissolParams.mesh.partitioningLib =
      reader.readWithDefault("partitioninglib", std::string("Default"));
  seissolParams.mesh.meshFormat = reader.readWithDefaultStringEnum<seissol::geometry::MeshFormat>(
      "meshgenerator",
      "puml",
      {{"netcdf", seissol::geometry::MeshFormat::Netcdf},
       {"puml", seissol::geometry::MeshFormat::PUML}});

  seissolParams.mesh.displacement =
      reader.readWithDefault("displacement", std::array<double, 3>{0, 0, 0});
  auto scalingX = reader.readWithDefault("scalingmatrixx", std::array<double, 3>{1, 0, 0});
  auto scalingY = reader.readWithDefault("scalingmatrixy", std::array<double, 3>{0, 1, 0});
  auto scalingZ = reader.readWithDefault("scalingmatrixz", std::array<double, 3>{0, 0, 1});
  seissolParams.mesh.scaling = {scalingX, scalingY, scalingZ};

  seissolParams.timeStepping.vertexWeight.weightElement =
      reader.readWithDefault("vertexweightelement", 100);
  seissolParams.timeStepping.vertexWeight.weightDynamicRupture =
      reader.readWithDefault("vertexweightdynamicrupture", 100);
  seissolParams.timeStepping.vertexWeight.weightFreeSurfaceWithGravity =
      reader.readWithDefault("vertexweightfreesurfacewithgravity", 100);

  reader.warnDeprecated({"periodic", "periodic_direction"});
  reader.warnUnknown();
}

static void readTimeStepping(ParameterReader& baseReader, SeisSolParameters& seissolParams) {
  auto reader = baseReader.readSubNode("discretization");

  seissolParams.timeStepping.cfl = reader.readWithDefault("cfl", 0.5);
  seissolParams.timeStepping.maxTimestepWidth = reader.readWithDefault("fixtimestep", 5000.0);
  seissolParams.timeStepping.lts.rate = reader.readWithDefault("clusteredlts", 2u);
  seissolParams.timeStepping.lts.weighttype = reader.readWithDefaultEnum(
      "ltsweighttypeid",
      seissol::initializers::time_stepping::LtsWeightsTypes::ExponentialWeights,
      {
          seissol::initializers::time_stepping::LtsWeightsTypes::ExponentialWeights,
          seissol::initializers::time_stepping::LtsWeightsTypes::ExponentialBalancedWeights,
          seissol::initializers::time_stepping::LtsWeightsTypes::EncodedBalancedWeights,
      });

  // TODO(David): integrate LTS parameters here
  reader.markUnused("ltswigglefactormin");
  reader.markUnused("ltswigglefactorstepsize");
  reader.markUnused("ltswigglefactorenforcemaximumdifference");
  reader.markUnused("ltsmaxnumberofclusters");
  reader.markUnused("ltsautomergeclusters");
  reader.markUnused("ltsallowedrelativeperformancelossautomerge");
  reader.markUnused("ltsautomergecostbaseline");

  reader.warnDeprecated({"ckmethod",
                         "dgfineout1d",
                         "fluxmethod",
                         "iterationcriterion",
                         "npoly",
                         "npolyrec",
                         "limitersecurityfactor",
                         "order",
                         "material",
                         "npolymap"});
  reader.warnUnknown();
}

static void readInitialization(ParameterReader& baseReader, SeisSolParameters& seissolParams) {
  auto reader = baseReader.readSubNode("inicondition");

  seissolParams.initialization.type = reader.readWithDefaultStringEnum<InitializationType>(
      "cictype",
      "zero",
      {
          {"zero", InitializationType::Zero},
          {"planarwave", InitializationType::Planarwave},
          {"superimposedplanarwave", InitializationType::SuperimposedPlanarwave},
          {"travelling", InitializationType::Travelling},
          {"scholte", InitializationType::Scholte},
          {"snell", InitializationType::Snell},
          {"ocean_0", InitializationType::Ocean0},
          {"ocean_1", InitializationType::Ocean1},
          {"ocean_2", InitializationType::Ocean2},
      });
  seissolParams.initialization.origin = reader.readWithDefault("origin", std::array<double, 3>{0});
  seissolParams.initialization.kVec = reader.readWithDefault("kvec", std::array<double, 3>{0});
  seissolParams.initialization.ampField =
      reader.readWithDefault("ampfield", std::array<double, NUMBER_OF_QUANTITIES>{0});

  reader.warnUnknown();
}

static void readOutput(ParameterReader& baseReader, SeisSolParameters& seissolParams) {
  auto reader = baseReader.readSubNode("output");

  constexpr double veryLongTime = 1.0e100;

  auto warnIntervalAndDisable =
      [](bool& enabled, double interval, const std::string& valName, const std::string& intName) {
        if (enabled && interval <= 0) {
          auto intPhrase = valName + " = 0";
          logInfo(seissol::MPI::mpi.rank())
              << "In your parameter file, you have specified a non-positive interval for" << intName
              << ". This mechanism is deprecated and may be removed in a future version of "
                 "SeisSol. Consider disabling the whole module by setting"
              << valName << "to 0 instead by adding" << intPhrase
              << "to the \"output\" section of your parameter file instead.";

          // still, replicate the old behavior.
          enabled = false;
        }
      };

  // general params
  seissolParams.output.format = reader.readWithDefaultEnum<OutputFormat>(
      "format", OutputFormat::None, {OutputFormat::None, OutputFormat::Xdmf});
  seissolParams.output.prefix =
      reader.readOrFail<std::string>("outputfile", "Output file prefix not defined.");
  seissolParams.output.xdmfWriterBackend =
      reader.readWithDefaultStringEnum<xdmfwriter::BackendType>(
          "xdmfwriterbackend",
          "posix",
          {
              {"posix", xdmfwriter::BackendType::POSIX},
#ifdef USE_HDF
              {"hdf5", xdmfwriter::BackendType::H5},
#endif
          });

  // checkpointing
  seissolParams.output.checkpointParameters.enabled = reader.readWithDefault("checkpoint", true);
  seissolParams.output.checkpointParameters.backend =
      reader.readWithDefaultStringEnum<seissol::checkpoint::Backend>(
          "checkpointbackend",
          "none",
          {{"none", seissol::checkpoint::Backend::DISABLED},
           {"posix", seissol::checkpoint::Backend::POSIX},
           {"hdf5", seissol::checkpoint::Backend::HDF5},
           {"mpio", seissol::checkpoint::Backend::MPIO},
           {"mpio_async", seissol::checkpoint::Backend::MPIO_ASYNC},
           {"sionlib", seissol::checkpoint::Backend::SIONLIB}});
  seissolParams.output.checkpointParameters.interval =
      reader.readWithDefault("checkpointinterval", 0.0);

  warnIntervalAndDisable(seissolParams.output.checkpointParameters.enabled,
                         seissolParams.output.checkpointParameters.interval,
                         "checkpoint",
                         "checkpointinterval");

  if (seissolParams.output.checkpointParameters.enabled) {
    seissolParams.output.checkpointParameters.fileName =
        reader.readOrFail<std::string>("checkpointfile", "No checkpoint filename given.");
  } else {
    reader.markUnused("checkpointfile");
  }

  // output: wavefield
  // (these variables are usually not prefixed with "wavefield" or the likes)

  // bounds
  auto bounds = reader.readWithDefault("outputregionbounds", std::array<double, 6>{0});
  seissolParams.output.waveFieldParameters.bounds.boundsX.lower = bounds[0];
  seissolParams.output.waveFieldParameters.bounds.boundsX.upper = bounds[1];
  seissolParams.output.waveFieldParameters.bounds.boundsY.lower = bounds[2];
  seissolParams.output.waveFieldParameters.bounds.boundsY.upper = bounds[3];
  seissolParams.output.waveFieldParameters.bounds.boundsZ.lower = bounds[4];
  seissolParams.output.waveFieldParameters.bounds.boundsZ.upper = bounds[5];
  seissolParams.output.waveFieldParameters.bounds.enabled =
      !(bounds[0] == 0 && bounds[1] == 0 && bounds[2] == 0 && bounds[3] == 0 && bounds[4] == 0 &&
        bounds[5] == 0);

  seissolParams.output.waveFieldParameters.enabled =
      reader.readWithDefault("wavefieldoutput", true);
  seissolParams.output.waveFieldParameters.interval =
      reader.readWithDefault("timeinterval", veryLongTime);
  seissolParams.output.waveFieldParameters.refinement =
      reader.readWithDefaultEnum<OutputRefinement>("refinement",
                                                   OutputRefinement::NoRefine,
                                                   {OutputRefinement::NoRefine,
                                                    OutputRefinement::Refine4,
                                                    OutputRefinement::Refine8,
                                                    OutputRefinement::Refine32});

  warnIntervalAndDisable(seissolParams.output.waveFieldParameters.enabled,
                         seissolParams.output.waveFieldParameters.interval,
                         "wavefieldoutput",
                         "timeinterval");

  if (seissolParams.output.waveFieldParameters.enabled &&
      seissolParams.output.format == OutputFormat::None) {
    logInfo(seissol::MPI::mpi.rank())
        << "Disabling the wavefield output by setting \"outputformat = 10\" is deprecated "
           "and may be removed in a future version of SeisSol. Consider using the parameter "
           "\"wavefieldoutput\" instead. To disable wavefield output, add \"wavefieldoutput "
           "= 0\" to the \"output\" section of your parameters file.";

    seissolParams.output.waveFieldParameters.enabled = false;
  }

  auto groupsVector = reader.readWithDefault("outputgroups", std::vector<int>());
  seissolParams.output.waveFieldParameters.groups =
      std::unordered_set<int>(groupsVector.begin(), groupsVector.end());

  // output mask
  auto iOutputMask = reader.readOrFail<std::string>("ioutputmask", "No output mask given.");
  seissol::initializers::convertStringToMask(iOutputMask,
                                             seissolParams.output.waveFieldParameters.outputMask);

  auto iPlasticityMask = reader.readWithDefault("iplasticitymask", std::string("0 0 0 0 0 0 1"));
  seissol::initializers::convertStringToMask(
      iPlasticityMask, seissolParams.output.waveFieldParameters.plasticityMask);

  auto integrationMask =
      reader.readWithDefault("integrationmask", std::string("0 0 0 0 0 0 0 0 0"));
  seissol::initializers::convertStringToMask(
      integrationMask, seissolParams.output.waveFieldParameters.integrationMask);

  // output: surface
  seissolParams.output.freeSurfaceParameters.enabled =
      reader.readWithDefault("surfaceoutput", false);
  seissolParams.output.freeSurfaceParameters.interval =
      reader.readWithDefault("surfaceoutputinterval", veryLongTime);
  seissolParams.output.freeSurfaceParameters.refinement =
      reader.readWithDefault("surfaceoutputrefinement", 0u);

  warnIntervalAndDisable(seissolParams.output.freeSurfaceParameters.enabled,
                         seissolParams.output.freeSurfaceParameters.interval,
                         "surfaceoutput",
                         "surfaceoutputinterval");

  // output: energy
  seissolParams.output.energyParameters.enabled = reader.readWithDefault("energyoutput", false);
  seissolParams.output.energyParameters.interval =
      reader.readWithDefault("energyoutputinterval", veryLongTime);
  seissolParams.output.energyParameters.terminalOutput =
      reader.readWithDefault("energyterminaloutput", false);
  seissolParams.output.energyParameters.computeVolumeEnergiesEveryOutput =
      reader.readWithDefault("computevolumeenergieseveryoutput", 1);

  warnIntervalAndDisable(seissolParams.output.energyParameters.enabled,
                         seissolParams.output.energyParameters.interval,
                         "energyoutput",
                         "energyoutputinterval");

  // output: refinement
  seissolParams.output.receiverParameters.enabled = reader.readWithDefault("receiveroutput", true);
  seissolParams.output.receiverParameters.interval =
      reader.readWithDefault("receiveroutputinterval", veryLongTime);
  seissolParams.output.receiverParameters.computeRotation =
      reader.readWithDefault("receivercomputerotation", false);
  seissolParams.output.receiverParameters.fileName =
      reader.readWithDefault("rfilename", std::string(""));
  seissolParams.output.receiverParameters.samplingInterval = reader.readWithDefault("pickdt", 0.0);

  warnIntervalAndDisable(seissolParams.output.receiverParameters.enabled,
                         seissolParams.output.receiverParameters.interval,
                         "receiveroutput",
                         "receiveroutputinterval");

  // output: fault
  seissolParams.output.faultOutput = reader.readWithDefault("faultoutputflag", false);

  // output: loop statistics
  seissolParams.output.loopStatisticsNetcdfOutput =
      reader.readWithDefault("loopstatisticsnetcdfoutput", false);

  reader.warnDeprecated({"rotation",
                         "interval",
                         "nrecordpoints",
                         "printintervalcriterion",
                         "pickdttype",
                         "ioutputmaskmaterial"});
  reader.warnUnknown();
}

static void readAbortCriteria(ParameterReader& baseReader, SeisSolParameters& seissolParams) {
  auto reader = baseReader.readSubNode("abortcriteria");

  seissolParams.end.endTime = reader.readWithDefault("endtime", 15.0);

  reader.warnDeprecated(
      {"maxiterations", "maxtolerance", "maxtolcriterion", "walltime_h", "delay_h"});
  reader.warnUnknown();
}

static void readSource(ParameterReader& baseReader, SeisSolParameters& seissolParams) {
  auto reader = baseReader.readSubNode("sourcetype");

  seissolParams.source.type =
      reader.readWithDefaultEnum("type",
                                 seissol::sourceterm::SourceType::None,
                                 {seissol::sourceterm::SourceType::None,
                                  seissol::sourceterm::SourceType::FsrmSource,
                                  seissol::sourceterm::SourceType::NrfSource});
  if (seissolParams.source.type != seissol::sourceterm::SourceType::None) {
    seissolParams.source.fileName =
        reader.readOrFail<std::string>("filename", "No source file specified.");
  } else {
    reader.markUnused("filename");
  }

  reader.warnDeprecated({"rtype", "ndirac", "npulsesource", "nricker"});
  reader.warnUnknown();
}

void SeisSolParameters::readParameters(const YAML::Node& baseNode) {
  logInfo(seissol::MPI::mpi.rank()) << "Reading SeisSol parameter file...";

  ParameterReader baseReader(baseNode, false);

  readModel(baseReader, *this);
  readBoundaries(baseReader, *this);
  readMesh(baseReader, *this);
  readTimeStepping(baseReader, *this);
  readInitialization(baseReader, *this);
  readOutput(baseReader, *this);
  readSource(baseReader, *this);
  readAbortCriteria(baseReader, *this);

  // TODO(David): remove once DR parameter reading is integrated here
  baseReader.markUnused("dynamicrupture");
  baseReader.markUnused("elementwise");
  baseReader.markUnused("pickpoint");

  baseReader.warnDeprecated({"rffile",
                             "inflowbound",
                             "inflowboundpwfile",
                             "inflowbounduin",
                             "source110",
                             "source15",
                             "source1618",
                             "source17",
                             "source19",
                             "spongelayer",
                             "sponges",
                             "analysis",
                             "analysisfields",
                             "debugging"});
  baseReader.warnUnknown();

  logInfo(seissol::MPI::mpi.rank()) << "SeisSol parameter file read successfully.";
}
