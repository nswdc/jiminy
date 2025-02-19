#include <cmath>
#include <ctime>
#include <unordered_set>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <fstream>
#include <streambuf>

#include "pinocchio/parsers/urdf.hpp"
#include "pinocchio/spatial/inertia.hpp"                    // `pinocchio::Inertia`
#include "pinocchio/spatial/force.hpp"                      // `pinocchio::Force`
#include "pinocchio/spatial/se3.hpp"                        // `pinocchio::SE3`
#include "pinocchio/spatial/explog.hpp"                     // `pinocchio::exp3`, `pinocchio::log3`
#include "pinocchio/spatial/explog-quaternion.hpp"          // `pinocchio::quaternion::log3`
#include "pinocchio/multibody/visitor.hpp"                  // `pinocchio::fusion::JointUnaryVisitorBase`
#include "pinocchio/multibody/joint/joint-model-base.hpp"   // `pinocchio::JointModelBase`
#include "pinocchio/algorithm/center-of-mass.hpp"           // `pinocchio::getComFromCrba`
#include "pinocchio/algorithm/frames.hpp"                   // `pinocchio::getFrameVelocity`
#include "pinocchio/algorithm/jacobian.hpp"                 // `pinocchio::getJointJacobian`
#include "pinocchio/algorithm/energy.hpp"                   // `pinocchio::computePotentialEnergy`
#include "pinocchio/algorithm/joint-configuration.hpp"      // `pinocchio::normalize`
#include "pinocchio/algorithm/geometry.hpp"                 // `pinocchio::computeCollisions`

#include "H5Cpp.h"
#include "json/json.h"

#include "jiminy/core/io/FileDevice.h"
#include "jiminy/core/io/Serialization.h"
#include "jiminy/core/telemetry/TelemetryData.h"
#include "jiminy/core/telemetry/TelemetryRecorder.h"
#include "jiminy/core/robot/PinocchioOverloadAlgorithms.h"
#include "jiminy/core/robot/AbstractMotor.h"
#include "jiminy/core/robot/AbstractSensor.h"
#include "jiminy/core/constraints/AbstractConstraint.h"
#include "jiminy/core/constraints/JointConstraint.h"
#include "jiminy/core/constraints/FixedFrameConstraint.h"
#include "jiminy/core/robot/Robot.h"
#include "jiminy/core/control/AbstractController.h"
#include "jiminy/core/control/ControllerFunctor.h"
#include "jiminy/core/solver/ConstraintSolvers.h"
#include "jiminy/core/stepper/AbstractStepper.h"
#include "jiminy/core/stepper/EulerExplicitStepper.h"
#include "jiminy/core/stepper/RungeKuttaDOPRIStepper.h"
#include "jiminy/core/stepper/RungeKutta4Stepper.h"
#include "jiminy/core/engine/EngineMultiRobot.h"
#include "jiminy/core/utilities/Pinocchio.h"
#include "jiminy/core/utilities/Random.h"
#include "jiminy/core/utilities/Json.h"
#include "jiminy/core/utilities/Helpers.h"
#include "jiminy/core/Constants.h"


namespace jiminy
{
    EngineMultiRobot::EngineMultiRobot(void):
    engineOptions_(nullptr),
    systems_(),
    isTelemetryConfigured_(false),
    isSimulationRunning_(false),
    engineOptionsHolder_(),
    timer_(std::make_unique<Timer>()),
    contactModel_(contactModel_t::NONE),
    telemetrySender_(),
    telemetryData_(nullptr),
    telemetryRecorder_(nullptr),
    stepper_(),
    stepperUpdatePeriod_(INF),
    stepperState_(),
    systemsDataHolder_(),
    forcesCoupling_(),
    contactForcesPrev_(),
    fPrev_(),
    aPrev_(),
    logData_(nullptr)
    {
        // Initialize the configuration options to the default.
        setOptions(getDefaultEngineOptions());

        // Initialize the global telemetry data holder
        telemetryData_ = std::make_shared<TelemetryData>();
        telemetryData_->reset();

        // Initialize the global telemetry recorder
        telemetryRecorder_ = std::make_unique<TelemetryRecorder>();

        // Initialize the engine-specific telemetry sender
        telemetrySender_.configureObject(telemetryData_, ENGINE_TELEMETRY_NAMESPACE);
    }

    EngineMultiRobot::~EngineMultiRobot(void) = default;  // Cannot be default in the header since some types are incomplete at this point

    hresult_t EngineMultiRobot::addSystem(std::string const & systemName,
                                          std::shared_ptr<Robot> robot,
                                          std::shared_ptr<AbstractController> controller,
                                          callbackFunctor_t callbackFct)
    {
        // Make sure that no simulation is running
        if (isSimulationRunning_)
        {
            PRINT_ERROR("A simulation is already running. Stop it before adding a new system.");
            return hresult_t::ERROR_GENERIC;
        }

        if (!robot)
        {
            PRINT_ERROR("Robot unspecified.");
            return hresult_t::ERROR_INIT_FAILED;
        }

        if (!robot->getIsInitialized())
        {
            PRINT_ERROR("Robot not initialized.");
            return hresult_t::ERROR_INIT_FAILED;
        }

        if (!controller)
        {
            PRINT_ERROR("Controller unspecified.");
            return hresult_t::ERROR_INIT_FAILED;
        }

        if (!controller->getIsInitialized())
        {
            PRINT_ERROR("Controller not initialized.");
            return hresult_t::ERROR_INIT_FAILED;
        }

        auto robot_controller = controller->robot_.lock();
        if (!robot_controller)
        {
            PRINT_ERROR("Controller's robot expired or unset.");
            return hresult_t::ERROR_INIT_FAILED;
        }

        if (robot != robot_controller)
        {
            PRINT_ERROR("Controller not initialized for specified robot.");
            return hresult_t::ERROR_INIT_FAILED;
        }

        // TODO: Check that the callback function is working as expected
        systems_.emplace_back(systemName,
                              robot,
                              controller,
                              std::move(callbackFct));
        systemsDataHolder_.resize(systems_.size());

        return hresult_t::SUCCESS;
    }

    hresult_t EngineMultiRobot::addSystem(std::string const & systemName,
                                          std::shared_ptr<Robot> robot,
                                          callbackFunctor_t callbackFct)
    {
        // Make sure an actual robot is specified
        if (!robot)
        {
            PRINT_ERROR("Robot unspecified.");
            return hresult_t::ERROR_INIT_FAILED;
        }

        // Make sure the robot is properly initialized
        if (!robot->getIsInitialized())
        {
            PRINT_ERROR("Robot not initialized.");
            return hresult_t::ERROR_INIT_FAILED;
        }

        // When using several robots the robots names are specified
        // as a circumfix in the log, for consistency they must all
        // have a name
        if (systems_.size() && systemName == "")
        {
            PRINT_ERROR("All systems but the first one must have a name.");
            return hresult_t::ERROR_GENERIC;
        }

        // Check if a system with the same name already exists
        auto systemIt = std::find_if(systems_.begin(), systems_.end(),
                                     [&systemName](auto const & sys)
                                     {
                                         return (sys.name == systemName);
                                     });
        if (systemIt != systems_.end())
        {
            PRINT_ERROR("A system with this name has already been added to the engine.");
            return hresult_t::ERROR_BAD_INPUT;
        }

        // Make sure none of the existing system is referring to the same robot
        systemIt = std::find_if(systems_.begin(), systems_.end(),
                                [&robot](auto const & sys)
                                {
                                    return (sys.robot == robot);
                                });
        if (systemIt != systems_.end())
        {
            PRINT_ERROR("The system '", systemIt->name, "' is already referring to this robot.");
            return hresult_t::ERROR_BAD_INPUT;
        }

        // Create and initialize a controller doing nothing
        auto bypassFunctor = [](float64_t        const & /* t */,
                                vectorN_t        const & /* q */,
                                vectorN_t        const & /* v */,
                                sensorsDataMap_t const & /* sensorsData */,
                                vectorN_t              & /* out */) {};
        auto controller = std::make_shared<ControllerFunctor<
            decltype(bypassFunctor), decltype(bypassFunctor)> >(bypassFunctor, bypassFunctor);
        controller->initialize(robot);

        return addSystem(systemName, robot, controller, std::move(callbackFct));
    }

    hresult_t EngineMultiRobot::removeSystem(std::string const & systemName)
    {
        hresult_t returnCode = hresult_t::SUCCESS;

        // Make sure that no simulation is running
        if (isSimulationRunning_)
        {
            PRINT_ERROR("A simulation is already running. Stop it before removing a system.");
            returnCode = hresult_t::ERROR_GENERIC;
        }

        if (returnCode == hresult_t::SUCCESS)
        {
            /* Remove every coupling forces involving the system.
               Note that it is already checking that the system exists. */
            returnCode = removeForcesCoupling(systemName);
        }

        if (returnCode == hresult_t::SUCCESS)
        {
            // Get the system index
            int32_t systemIdx;
            getSystemIdx(systemName, systemIdx);  // It cannot fail at this point

            // Update the systems' indices for the remaining coupling forces
            for (auto & force : forcesCoupling_)
            {
                if (force.systemIdx1 > systemIdx)
                {
                    force.systemIdx1--;
                }
                if (force.systemIdx2 > systemIdx)
                {
                    force.systemIdx2--;
                }
            }

            // Remove the system from the list
            systems_.erase(systems_.begin() + systemIdx);
            systemsDataHolder_.erase(systemsDataHolder_.begin() + systemIdx);
        }

        return returnCode;
    }

    hresult_t EngineMultiRobot::setController(std::string const & systemName,
                                              std::shared_ptr<AbstractController> controller)
    {
        hresult_t returnCode = hresult_t::SUCCESS;

        // Make sure that no simulation is running
        if (isSimulationRunning_)
        {
            PRINT_ERROR("A simulation is already running. Stop it before setting a new controller for a system.");
            returnCode = hresult_t::ERROR_GENERIC;
        }

        // Make sure that the controller is initialized
        if (returnCode == hresult_t::SUCCESS)
        {
            if (!controller->getIsInitialized())
            {
                PRINT_ERROR("Controller not initialized.");
                returnCode = hresult_t::ERROR_INIT_FAILED;
            }
        }

        auto robot_controller = controller->robot_.lock();
        if (returnCode == hresult_t::SUCCESS)
        {
            if (!robot_controller)
            {
                PRINT_ERROR("Controller's robot expired or unset.");
                returnCode = hresult_t::ERROR_INIT_FAILED;
            }
        }

        // Make sure that the system for which to set the controller exists
        systemHolder_t * system;
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = getSystem(systemName, system);
        }

        if (returnCode == hresult_t::SUCCESS)
        {
            if (system->robot != robot_controller)
            {
                PRINT_ERROR("Controller not initialized for robot associated with specified system.");
                returnCode = hresult_t::ERROR_INIT_FAILED;
            }
        }

        // Set the controller
        if (returnCode == hresult_t::SUCCESS)
        {
            system->controller = controller;
        }

        return returnCode;
    }

    hresult_t EngineMultiRobot::registerForceCoupling(std::string            const & systemName1,
                                                      std::string            const & systemName2,
                                                      std::string            const & frameName1,
                                                      std::string            const & frameName2,
                                                      forceCouplingFunctor_t         forceFct)
    {
        hresult_t returnCode = hresult_t::SUCCESS;

        // Make sure that no simulation is running
        if (isSimulationRunning_)
        {
            PRINT_ERROR("A simulation is already running. Stop it before adding coupling forces.");
            returnCode = hresult_t::ERROR_GENERIC;
        }

        // Get system and frame indices
        int32_t systemIdx1;
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = getSystemIdx(systemName1, systemIdx1);
        }

        int32_t systemIdx2;
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = getSystemIdx(systemName2, systemIdx2);
        }

        frameIndex_t frameIdx1;
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = getFrameIdx(systems_[systemIdx1].robot->pncModel_,
                                     frameName1,
                                     frameIdx1);
        }

        frameIndex_t frameIdx2;
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = getFrameIdx(systems_[systemIdx2].robot->pncModel_,
                                     frameName2,
                                     frameIdx2);
        }

        // Make sure it is not coupling the exact same frame
        if (returnCode == hresult_t::SUCCESS)
        {
            if (systemIdx1 == systemIdx2 && frameIdx1 == frameIdx2)
            {
                PRINT_ERROR("A coupling force requires different frames.");
                returnCode = hresult_t::ERROR_GENERIC;
            }
        }

        if (returnCode == hresult_t::SUCCESS)
        {
            forcesCoupling_.emplace_back(systemName1,
                                         systemIdx1,
                                         systemName2,
                                         systemIdx2,
                                         frameName1,
                                         frameIdx1,
                                         frameName2,
                                         frameIdx2,
                                         std::move(forceFct));
        }

        return returnCode;
    }

    hresult_t EngineMultiRobot::registerViscoelasticForceCoupling(std::string const & systemName1,
                                                                  std::string const & systemName2,
                                                                  std::string const & frameName1,
                                                                  std::string const & frameName2,
                                                                  vector6_t   const & stiffness,
                                                                  vector6_t   const & damping,
                                                                  float64_t   const & alpha)
    {
        hresult_t returnCode = hresult_t::SUCCESS;

        if ((stiffness.array() < 0.0).any() || (damping.array() < 0.0).any())
        {
            PRINT_ERROR("The stiffness and damping parameters must be positive.");
            returnCode = hresult_t::ERROR_GENERIC;
        }

        systemHolder_t * system1;
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = getSystem(systemName1, system1);
        }

        systemHolder_t * system2;
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = getSystem(systemName2, system2);
        }

        frameIndex_t frameIdx1, frameIdx2;
        if (returnCode == hresult_t::SUCCESS)
        {
            getFrameIdx(system1->robot->pncModel_, frameName1, frameIdx1);
            getFrameIdx(system2->robot->pncModel_, frameName2, frameIdx2);
        }

        if (returnCode == hresult_t::SUCCESS)
        {
            // Allocate memory
            float64_t angle{0.0};
            matrix3_t rot12, rotJLog12, rotJExp12, rotRef12, omega;
            vector3_t rotLog12, pos12, posLocal12, fLin, fAng;

            auto forceFct = [=] (
                float64_t const & /*t*/,
                vectorN_t const & /*q_1*/,
                vectorN_t const & /*v_1*/,
                vectorN_t const & /*q_2*/,
                vectorN_t const & /*v_2*/) mutable -> pinocchio::Force
            {
                /* One must keep track of system pointers and frames indices internally
                   and update them at reset since the systems may have changed between
                   simulations. Note that `isSimulationRunning_` is false when called
                   for the first time in `start` method before locking the robot, so it
                   is the right time to update those proxies. */
                if (!isSimulationRunning_)
                {
                    getSystem(systemName1, system1);
                    getFrameIdx(system1->robot->pncModel_, frameName1, frameIdx1);
                    getSystem(systemName2, system2);
                    getFrameIdx(system2->robot->pncModel_, frameName2, frameIdx2);
                }

                // Get the frames positions and velocities in world
                pinocchio::SE3 const & oMf1 = system1->robot->pncData_.oMf[frameIdx1];
                pinocchio::SE3 const & oMf2 = system2->robot->pncData_.oMf[frameIdx2];
                pinocchio::Motion const oVf1 = getFrameVelocity(system1->robot->pncModel_,
                                                                system1->robot->pncData_,
                                                                frameIdx1,
                                                                pinocchio::LOCAL_WORLD_ALIGNED);
                pinocchio::Motion const oVf2 = getFrameVelocity(system2->robot ->pncModel_,
                                                                system2->robot->pncData_,
                                                                frameIdx2,
                                                                pinocchio::LOCAL_WORLD_ALIGNED);

                // Compute intermediary quantities
                rot12.noalias() = oMf1.rotation().transpose() * oMf2.rotation();
                rotLog12 = pinocchio::log3(rot12, angle);
                assert((angle < 0.95 * M_PI) &&
                       "Relative angle between reference frames of viscoelastic coupling must be smaller than 0.95 * pi.");
                pinocchio::Jlog3(angle, rotLog12, rotJLog12);
                fAng = stiffness.tail<3>().array() * rotLog12.array();
                rotLog12 *= alpha;
                pinocchio::Jexp3(rotLog12, rotJExp12);
                rotRef12.noalias() = oMf1.rotation() * pinocchio::exp3(rotLog12);
                pos12 = oMf2.translation() - oMf1.translation();
                posLocal12.noalias() = rotRef12.transpose() * pos12;
                fLin = stiffness.head<3>().array() * posLocal12.array();
                omega.noalias() = alpha * rotJExp12 * rotJLog12;

                /* Compute the relative velocity. The application point is the "linear"
                   interpolation between the frames placement with alpha ratio. */
                pinocchio::Motion velLocal12(
                    rotRef12.transpose() * (
                        oVf2.linear() - oVf1.linear() + pos12.cross(
                            alpha * oVf1.angular() + (1.0 - alpha) * oVf2.angular())),
                    rotRef12.transpose() * (oVf2.angular() - oVf1.angular()));

                // Compute the coupling force acting on frame 2
                pinocchio::Force f;
                f.linear() = damping.head<3>().array() * velLocal12.linear().array();
                f.angular() = (1.0 - alpha) * f.linear().cross(posLocal12);
                f.angular().array() += damping.tail<3>().array() * velLocal12.angular().array();
                f.linear() += fLin;
                f.linear() = rotRef12 * f.linear();
                f.angular() = rotRef12 * f.angular();
                f.angular() -= oMf2.rotation() * omega.colwise().cross(posLocal12).transpose() * fLin;
                f.angular() += oMf1.rotation() * rotJLog12 * fAng;

                // Deduce the force acting on frame 1 from action-reaction law
                f.angular() += pos12.cross(f.linear());

                return f;
            };

            returnCode = registerForceCoupling(
                systemName1, systemName2, frameName1, frameName2, forceFct);
        }

        return returnCode;
    }

    hresult_t EngineMultiRobot::registerViscoelasticForceCoupling(std::string const & systemName,
                                                                  std::string const & frameName1,
                                                                  std::string const & frameName2,
                                                                  vector6_t   const & stiffness,
                                                                  vector6_t   const & damping,
                                                                  float64_t   const & alpha)
    {
        return registerViscoelasticForceCoupling(
            systemName, systemName, frameName1, frameName2, stiffness, damping, alpha);
    }

    hresult_t EngineMultiRobot::registerViscoelasticDirectionalForceCoupling(std::string const & systemName1,
                                                                             std::string const & systemName2,
                                                                             std::string const & frameName1,
                                                                             std::string const & frameName2,
                                                                             float64_t   const & stiffness,
                                                                             float64_t   const & damping,
                                                                             float64_t   const & restLength)
    {
        hresult_t returnCode = hresult_t::SUCCESS;

        if (stiffness < 0.0 || damping < 0.0)
        {
            PRINT_ERROR("The stiffness and damping parameters must be positive.");
            returnCode = hresult_t::ERROR_GENERIC;
        }

        systemHolder_t * system1;
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = getSystem(systemName1, system1);
        }

        frameIndex_t frameIdx1;
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = getFrameIdx(system1->robot->pncModel_, frameName1, frameIdx1);
        }

        systemHolder_t * system2;
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = getSystem(systemName2, system2);
        }

        frameIndex_t frameIdx2;
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = getFrameIdx(system2->robot->pncModel_, frameName2, frameIdx2);
        }

        if (returnCode == hresult_t::SUCCESS)
        {
            auto forceFct = [=] (
                float64_t const & /*t*/,
                vectorN_t const & /*q_1*/,
                vectorN_t const & /*v_1*/,
                vectorN_t const & /*q_2*/,
                vectorN_t const & /*v_2*/) mutable -> pinocchio::Force
            {
                /* One must keep track of system pointers and frames indices internally
                   and update them at reset since the systems may have changed between
                   simulations. Note that `isSimulationRunning_` is false when called
                   for the first time in `start` method before locking the robot, so it
                   is the right time to update those proxies. */
                if (!isSimulationRunning_)
                {
                    getSystem(systemName1, system1);
                    getFrameIdx(system1->robot->pncModel_, frameName1, frameIdx1);
                    getSystem(systemName2, system2);
                    getFrameIdx(system2->robot->pncModel_, frameName2, frameIdx2);
                }

                // Get the frames positions and velocities in world
                pinocchio::SE3 const & oMf1 = system1->robot->pncData_.oMf[frameIdx1];
                pinocchio::SE3 const & oMf2 = system2->robot->pncData_.oMf[frameIdx2];
                pinocchio::Motion const oVf1 = getFrameVelocity(system1->robot->pncModel_,
                                                                system1->robot->pncData_,
                                                                frameIdx1,
                                                                pinocchio::LOCAL_WORLD_ALIGNED);
                pinocchio::Motion const oVf2 = getFrameVelocity(system2->robot ->pncModel_,
                                                                system2->robot->pncData_,
                                                                frameIdx2,
                                                                pinocchio::LOCAL_WORLD_ALIGNED);

                // Compute the linear force coupling them
                vector3_t dir12 = oMf2.translation() - oMf1.translation();
                float64_t const length = dir12.norm();
                auto vel12 = oVf2.linear() - oVf1.linear();
                if (length > EPS)
                {
                    dir12 /= length;
                    auto vel12Proj = vel12.dot(dir12);
                    return {(stiffness * (length - restLength) + damping * vel12Proj) * dir12,
                            vector3_t::Zero()};
                }
                else
                {
                    /* The direction between frames is ill-defined, so applying
                       force in the direction of the velocity instead. */
                    return {damping * vel12, vector3_t::Zero()};
                }
                return pinocchio::Force::Zero();
            };

            returnCode = registerForceCoupling(
                systemName1, systemName2, frameName1, frameName2, forceFct);
        }

        return returnCode;
    }

    hresult_t EngineMultiRobot::registerViscoelasticDirectionalForceCoupling(std::string const & systemName,
                                                                             std::string const & frameName1,
                                                                             std::string const & frameName2,
                                                                             float64_t   const & stiffness,
                                                                             float64_t   const & damping,
                                                                             float64_t   const & restLength)
    {
        return registerViscoelasticDirectionalForceCoupling(
            systemName, systemName, frameName1, frameName2, stiffness, damping, restLength);
    }

    hresult_t EngineMultiRobot::removeForcesCoupling(std::string const & systemName1,
                                                     std::string const & systemName2)
    {
        hresult_t returnCode = hresult_t::SUCCESS;

        // Make sure that no simulation is running
        if (isSimulationRunning_)
        {
            PRINT_ERROR("A simulation is already running. Stop it before removing coupling forces.");
            returnCode = hresult_t::ERROR_GENERIC;
        }

        systemHolder_t * system1;
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = getSystem(systemName1, system1);
        }

        if (returnCode == hresult_t::SUCCESS)
        {
            systemHolder_t * system2;
            returnCode = getSystem(systemName2, system2);
        }

        if (returnCode == hresult_t::SUCCESS)
        {
            forcesCoupling_.erase(
                std::remove_if(forcesCoupling_.begin(), forcesCoupling_.end(),
                [&systemName1, &systemName2](auto const & force)
                {
                    return (force.systemName1 == systemName1 &&
                            force.systemName2 == systemName2);
                }),
                forcesCoupling_.end()
            );
        }

        return returnCode;
    }

    hresult_t EngineMultiRobot::removeForcesCoupling(std::string const & systemName)
    {
        hresult_t returnCode = hresult_t::SUCCESS;

        // Make sure that no simulation is running
        if (isSimulationRunning_)
        {
            PRINT_ERROR("A simulation is already running. Stop it before removing coupling forces.");
            returnCode = hresult_t::ERROR_GENERIC;
        }

        systemHolder_t * system;
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = getSystem(systemName, system);
        }

        if (returnCode == hresult_t::SUCCESS)
        {
            forcesCoupling_.erase(
                std::remove_if(forcesCoupling_.begin(), forcesCoupling_.end(),
                [&systemName](auto const & force)
                {
                    return (force.systemName1 == systemName ||
                            force.systemName2 == systemName);
                }),
                forcesCoupling_.end()
            );
        }

        return returnCode;
    }

    hresult_t EngineMultiRobot::removeForcesCoupling(void)
    {
        hresult_t returnCode = hresult_t::SUCCESS;

        // Make sure that no simulation is running
        if (isSimulationRunning_)
        {
            PRINT_ERROR("A simulation is already running. Stop it before removing coupling forces.");
            returnCode = hresult_t::ERROR_GENERIC;
        }

        forcesCoupling_.clear();

        return returnCode;
    }

    forceCouplingRegister_t const & EngineMultiRobot::getForcesCoupling(void) const
    {
        return forcesCoupling_;
    }

    hresult_t EngineMultiRobot::removeAllForces(void)
    {
        hresult_t returnCode = hresult_t::SUCCESS;
        returnCode = removeForcesCoupling();
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = removeForcesImpulse();
        }
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = removeForcesProfile();
        }
        return returnCode;
    }

    hresult_t EngineMultiRobot::configureTelemetry(void)
    {
        hresult_t returnCode = hresult_t::SUCCESS;

        if (systems_.empty())
        {
            PRINT_ERROR("No system added to the engine.");
            returnCode = hresult_t::ERROR_INIT_FAILED;
        }

        if (!isTelemetryConfigured_)
        {
            auto systemIt = systems_.begin();
            auto systemDataIt = systemsDataHolder_.begin();
            for ( ; systemIt != systems_.end(); ++systemIt, ++systemDataIt)
            {
                // Generate the log fieldnames
                systemDataIt->logFieldnamesPosition =
                    addCircumfix(systemIt->robot->getLogFieldnamesPosition(),
                                 systemIt->name, "", TELEMETRY_FIELDNAME_DELIMITER);
                systemDataIt->logFieldnamesVelocity =
                    addCircumfix(systemIt->robot->getLogFieldnamesVelocity(),
                                 systemIt->name, "", TELEMETRY_FIELDNAME_DELIMITER);
                systemDataIt->logFieldnamesAcceleration =
                    addCircumfix(systemIt->robot->getLogFieldnamesAcceleration(),
                                 systemIt->name, "", TELEMETRY_FIELDNAME_DELIMITER);
                systemDataIt->logFieldnamesForceExternal =
                    addCircumfix(systemIt->robot->getLogFieldnamesForceExternal(),
                                 systemIt->name, "", TELEMETRY_FIELDNAME_DELIMITER);
                systemDataIt->logFieldnamesCommand =
                    addCircumfix(systemIt->robot->getCommandFieldnames(),
                                 systemIt->name, "", TELEMETRY_FIELDNAME_DELIMITER);
                systemDataIt->logFieldnamesMotorEffort =
                    addCircumfix(systemIt->robot->getMotorEffortFieldnames(),
                                 systemIt->name, "", TELEMETRY_FIELDNAME_DELIMITER);
                systemDataIt->logFieldnameEnergy =
                    addCircumfix("energy",
                                 systemIt->name, "", TELEMETRY_FIELDNAME_DELIMITER);

                // Register variables to the telemetry senders
                if (returnCode == hresult_t::SUCCESS)
                {
                    if (engineOptions_->telemetry.enableConfiguration)
                    {
                        returnCode = telemetrySender_.registerVariable(
                            systemDataIt->logFieldnamesPosition,
                            systemDataIt->state.q);
                    }
                }
                if (returnCode == hresult_t::SUCCESS)
                {
                    if (engineOptions_->telemetry.enableVelocity)
                    {
                        returnCode = telemetrySender_.registerVariable(
                            systemDataIt->logFieldnamesVelocity,
                            systemDataIt->state.v);
                    }
                }
                if (returnCode == hresult_t::SUCCESS)
                {
                    if (engineOptions_->telemetry.enableAcceleration)
                    {
                        returnCode = telemetrySender_.registerVariable(
                            systemDataIt->logFieldnamesAcceleration,
                            systemDataIt->state.a);
                    }
                }
                if (engineOptions_->telemetry.enableForceExternal)
                {
                    for (std::size_t i = 1; i < systemDataIt->state.fExternal.size(); ++i)
                    {
                        auto const & fext = systemDataIt->state.fExternal[i].toVector();
                        for (uint8_t j = 0; j < 6U; ++j)
                        {
                            returnCode = telemetrySender_.registerVariable(
                                systemDataIt->logFieldnamesForceExternal[(i - 1) * 6U + j],
                                fext[j]);
                        }
                    }
                }
                if (returnCode == hresult_t::SUCCESS)
                {
                    if (engineOptions_->telemetry.enableCommand)
                    {
                        returnCode = telemetrySender_.registerVariable(
                            systemDataIt->logFieldnamesCommand,
                            systemDataIt->state.command);
                    }
                }
                if (returnCode == hresult_t::SUCCESS)
                {
                    if (engineOptions_->telemetry.enableMotorEffort)
                    {
                        returnCode = telemetrySender_.registerVariable(
                            systemDataIt->logFieldnamesMotorEffort,
                            systemDataIt->state.uMotor);
                    }
                }
                if (returnCode == hresult_t::SUCCESS)
                {
                    if (engineOptions_->telemetry.enableEnergy)
                    {
                        returnCode = telemetrySender_.registerVariable(
                            systemDataIt->logFieldnameEnergy, 0.0);
                    }
                }

                if (returnCode == hresult_t::SUCCESS)
                {
                    returnCode = systemIt->controller->configureTelemetry(
                        telemetryData_, systemIt->name);
                }
                if (returnCode == hresult_t::SUCCESS)
                {
                    returnCode = systemIt->robot->configureTelemetry(
                        telemetryData_, systemIt->name);
                }
            }
        }

        if (returnCode == hresult_t::SUCCESS)
        {
            isTelemetryConfigured_ = true;
        }

        return returnCode;
    }

    void EngineMultiRobot::updateTelemetry(void)
    {
        auto systemIt = systems_.begin();
        auto systemDataIt = systemsDataHolder_.begin();
        for ( ; systemIt != systems_.end(); ++systemIt, ++systemDataIt)
        {
            // Compute the total energy of the system
            float64_t energy = pinocchio_overload::computeKineticEnergy(
                systemIt->robot->pncModel_,
                systemIt->robot->pncData_,
                systemDataIt->state.q,
                systemDataIt->state.v);
            energy += pinocchio::computePotentialEnergy(
                systemIt->robot->pncModel_,
                systemIt->robot->pncData_);

            // Update telemetry values
            if (engineOptions_->telemetry.enableConfiguration)
            {
                telemetrySender_.updateValue(systemDataIt->logFieldnamesPosition,
                                             systemDataIt->state.q);
            }
            if (engineOptions_->telemetry.enableVelocity)
            {
                telemetrySender_.updateValue(systemDataIt->logFieldnamesVelocity,
                                             systemDataIt->state.v);
            }
            if (engineOptions_->telemetry.enableAcceleration)
            {
                telemetrySender_.updateValue(systemDataIt->logFieldnamesAcceleration,
                                             systemDataIt->state.a);
            }
            if (engineOptions_->telemetry.enableForceExternal)
            {
                for (std::size_t i = 1; i < systemDataIt->state.fExternal.size(); ++i)
                {
                    auto const & fext = systemDataIt->state.fExternal[i].toVector();
                    for (uint8_t j = 0; j < 6U; ++j)
                    {
                        telemetrySender_.updateValue(
                            systemDataIt->logFieldnamesForceExternal[(i - 1) * 6U + j],
                            fext[j]);
                    }
                }
            }
            if (engineOptions_->telemetry.enableCommand)
            {
                telemetrySender_.updateValue(systemDataIt->logFieldnamesCommand,
                                             systemDataIt->state.command);
            }
            if (engineOptions_->telemetry.enableMotorEffort)
            {
                telemetrySender_.updateValue(systemDataIt->logFieldnamesMotorEffort,
                                             systemDataIt->state.uMotor);
            }
            if (engineOptions_->telemetry.enableEnergy)
            {
                telemetrySender_.updateValue(systemDataIt->logFieldnameEnergy, energy);
            }

            systemIt->controller->updateTelemetry();
            systemIt->robot->updateTelemetry();
        }

        // Flush the telemetry internal state
        telemetryRecorder_->flushDataSnapshot(stepperState_.t);
    }

    void EngineMultiRobot::reset(bool_t const & resetRandomNumbers,
                                 bool_t const & removeAllForce)
    {
        // Make sure the simulation is properly stopped
        if (isSimulationRunning_)
        {
            stop();
        }

        // Clear log data buffer
        logData_ = nullptr;

        // Reset the dynamic force register if requested
        if (removeAllForce)
        {
            for (auto & systemData : systemsDataHolder_)
            {
                systemData.forcesImpulse.clear();
                systemData.forcesImpulseBreaks.clear();
                systemData.forcesImpulseActive.clear();
                systemData.forcesProfile.clear();
            }
            stepperUpdatePeriod_ = std::get<1>(isGcdIncluded(
                engineOptions_->stepper.sensorsUpdatePeriod,
                engineOptions_->stepper.controllerUpdatePeriod));
        }

        // Reset the random number generators
        if (resetRandomNumbers)
        {
            resetRandomGenerators(engineOptions_->stepper.randomSeed);
        }

        // Reset the internal state of the robot and controller
        for (auto & system : systems_)
        {
            system.robot->reset();
            system.controller->reset();
        }

        // Clear system state buffers, since the robot kinematic may change
        for (auto & systemData : systemsDataHolder_)
        {
            systemData.state.clear();
            systemData.statePrev.clear();
        }

        isTelemetryConfigured_ = false;
    }

    void computeExtraTerms(systemHolder_t           & system,
                           systemDataHolder_t const & /* systemData */)
    {
        /// This method is optimized to avoid redundant computations.
        /// See `pinocchio::computeAllTerms` for reference:
        ///
        /// Based on https://github.com/stack-of-tasks/pinocchio/blob/a1df23c2f183d84febdc2099e5fbfdbd1fc8018b/src/algorithm/compute-all-terms.hxx
        ///
        /// Copyright (c) 2014-2020, CNRS
        /// Copyright (c) 2018-2020, INRIA

        pinocchio::Model const & model = system.robot->pncModel_;
        pinocchio::Data & data = system.robot->pncData_;

        /* Update manually the subtree (apparent) inertia, since it is only
           computed by crba, which is doing more computation than necessary.
           It will be used here for computing the centroidal kinematics, and
           used later for joint bounds dynamics. Note that, by doing all the
           computations here instead of 'computeForwardKinematics', we are
           doing the assumption that it is varying slowly enough to consider
           it constant during one integration step. */
        if (!system.robot->hasConstraints())
        {
            for (int32_t i = 1; i < model.njoints; ++i)
            {
                data.Ycrb[i] = model.inertias[i];
            }
            for (int32_t i = model.njoints - 1; i > 0; --i)
            {
                jointIndex_t const & jointIdx = model.joints[i].id();
                jointIndex_t const & parentIdx = model.parents[jointIdx];
                if (parentIdx > 0)
                {
                    data.Ycrb[parentIdx] += data.liMi[jointIdx].act(data.Ycrb[jointIdx]);
                }
            }
        }

        /* Neither 'aba' nor 'forwardDynamics' are computing simultaneously the actual
           joint accelerations, joint forces and body forces, so it must be done separately:
           - 1st step: computing the accelerations based on ForwardKinematic algorithm
           - 2nd step: computing the forces based on rnea algorithm */
        pinocchio_overload::forwardKinematicsAcceleration(model, data, data.ddq);

        // Compute the spatial momenta and the sum of external forces acting on each body
        data.h[0].setZero();
        data.f[0].setZero();
        for (int32_t i = 1; i < model.njoints; ++i)
        {
            data.h[i] = model.inertias[i] * data.v[i];
            data.f[i] = model.inertias[i] * data.a[i] + data.v[i].cross(data.h[i]);
        }
        for (int32_t i = model.njoints - 1; i > 0; --i)
        {
            jointIndex_t const & parentIdx = model.parents[i];
            data.h[parentIdx] += data.liMi[i].act(data.h[i]);
            data.f[parentIdx] += data.liMi[i].act(data.f[i]);
        }

        /* Now that `data.Ycrb` and `data.h` are available, one can get directly
           the position and velocity of the center of mass of each subtrees. */
        for (int32_t i = 0; i < model.njoints; ++i)
        {
            data.com[i] = data.Ycrb[i].lever();
            data.vcom[i].noalias() = data.h[i].linear() / data.mass[i];
        }
        data.com[0] = data.liMi[1].act(data.com[1]);
        data.vcom[0].noalias() = data.h[0].linear() / data.mass[0];

        // Compute centrodial dynamics and its derivative
        data.hg = data.h[0];
        data.hg.angular() += data.hg.linear().cross(data.com[0]);
        data.dhg = data.f[0];
        data.dhg.angular() += data.dhg.linear().cross(data.com[0]);
    }

    void computeAllExtraTerms(std::vector<systemHolder_t>                & systems,
                              vector_aligned_t<systemDataHolder_t> const & systemsDataHolder)
    {
        auto systemIt = systems.begin();
        auto systemDataIt = systemsDataHolder.begin();
        for ( ; systemIt != systems.end(); ++systemIt, ++systemDataIt)
        {
            computeExtraTerms(*systemIt, *systemDataIt);
        }
    }

    void syncAccelerationsAndForces(systemHolder_t const & system,
                                    forceVector_t        & contactForces,
                                    forceVector_t        & f,
                                    motionVector_t       & a)
    {
        for (std::size_t i = 0; i < system.robot->getContactFramesNames().size(); ++i)
        {
            contactForces[i] = system.robot->contactForces_[i];
        }

        for (int32_t i = 0; i < system.robot->pncModel_.njoints; ++i)
        {
            f[i] = system.robot->pncData_.f[i];
            a[i] = system.robot->pncData_.a[i];
        }
    }

    void syncAllAccelerationsAndForces(std::vector<systemHolder_t> const & systems,
                                       vector_aligned_t<forceVector_t>   & contactForces,
                                       vector_aligned_t<forceVector_t>   & f,
                                       vector_aligned_t<motionVector_t>  & a)
    {
        std::vector<systemHolder_t>::const_iterator systemIt = systems.begin();
        auto contactForcesIt = contactForces.begin();
        auto fPrevIt = f.begin();
        auto aPrevIt = a.begin();
        for ( ; systemIt != systems.end();
             ++systemIt, ++contactForcesIt, ++fPrevIt, ++aPrevIt)
        {
            syncAccelerationsAndForces(*systemIt, *contactForcesIt, *fPrevIt, *aPrevIt);
        }
    }

    hresult_t EngineMultiRobot::start(std::map<std::string, vectorN_t> const & qInit,
                                      std::map<std::string, vectorN_t> const & vInit,
                                      std::optional<std::map<std::string, vectorN_t> > const & aInit)
    {
        hresult_t returnCode = hresult_t::SUCCESS;

        // Make sure that no simulation is running
        if (isSimulationRunning_)
        {
            PRINT_ERROR("A simulation is already running. Stop it before starting again.");
            return hresult_t::ERROR_GENERIC;
        }

        if (systems_.empty())
        {
            PRINT_ERROR("No system to simulate. Please add one before starting a simulation.");
            return hresult_t::ERROR_INIT_FAILED;
        }

        if (qInit.size() != systems_.size() || vInit.size() != systems_.size())
        {
            PRINT_ERROR("The number of initial configurations and velocities must match the number of systems.");
            return hresult_t::ERROR_BAD_INPUT;
        }

        // Check the dimension of the initial state associated with every system and order them
        std::vector<vectorN_t> qSplit;
        std::vector<vectorN_t> vSplit;
        qSplit.reserve(systems_.size());
        vSplit.reserve(systems_.size());
        for (auto const & system : systems_)
        {
            auto qInitIt = qInit.find(system.name);
            auto vInitIt = vInit.find(system.name);
            if (qInitIt == qInit.end() || vInitIt == vInit.end())
            {
                PRINT_ERROR("System '", system.name, "'does not have an initial configuration or velocity.");
                return hresult_t::ERROR_BAD_INPUT;
            }

            vectorN_t const & q = qInitIt->second;  // Make sure the initial state is normalized
            vectorN_t const & v = vInitIt->second;
            if (q.rows() != system.robot->nq() || v.rows() != system.robot->nv())
            {
                PRINT_ERROR("The dimension of the initial configuration or velocity is inconsistent "
                            "with model size for system '", system.name, "'.");
                return hresult_t::ERROR_BAD_INPUT;
            }

            bool_t isValid;
            isPositionValid(system.robot->pncModel_, q, isValid, std::numeric_limits<float32_t>::epsilon());  // It cannot throw an exception at this point
            if (!isValid)
            {
                PRINT_ERROR("The initial configuration is not consistent with the types of "
                            "joints of the model for system '", system.name, "'.");
                return hresult_t::ERROR_BAD_INPUT;
            }

            // Note that EPS allows to be very slightly out-of-bounds
            if ((system.robot->mdlOptions_->joints.enablePositionLimit &&
                 ((EPS < q.array() - system.robot->getPositionLimitMax().array()).any() ||
                  (EPS < system.robot->getPositionLimitMin().array() - q.array()).any())) ||
                (system.robot->mdlOptions_->joints.enableVelocityLimit &&
                 (EPS < v.array().abs() - system.robot->getVelocityLimit().array()).any()))
            {
                PRINT_ERROR("The initial configuration or velocity is out-of-bounds for system '", system.name, "'.");
                return hresult_t::ERROR_BAD_INPUT;
            }

            /* Make sure the configuration is normalized (as double), since normalization
               is checked using float accuracy rather than double to circumvent float/double
               casting than may occurs because of Python bindings. */
            vectorN_t qNormalized = q;
            pinocchio::normalize(system.robot->pncModel_, qNormalized);

            qSplit.emplace_back(qNormalized);
            vSplit.emplace_back(v);
        }

        std::vector<vectorN_t> aSplit;
        aSplit.reserve(systems_.size());
        if (aInit)
        {
            // Check the dimension of the initial acceleration associated with every system and order them
            if (aInit->size() != systems_.size())
            {
                PRINT_ERROR("If specified, the number of initial accelerations must match the number of systems.");
                return hresult_t::ERROR_BAD_INPUT;
            }

            for (auto const & system : systems_)
            {
                auto aInitIt = aInit->find(system.name);
                if (aInitIt == aInit->end())
                {
                    PRINT_ERROR("System '", system.name, "'does not have an initial acceleration.");
                    return hresult_t::ERROR_BAD_INPUT;
                }

                vectorN_t const & a = aInitIt->second;
                if (a.rows() != system.robot->nv())
                {
                    PRINT_ERROR("The dimension of the initial acceleration is inconsistent "
                                "with model size for system '", system.name, "'.");
                    return hresult_t::ERROR_BAD_INPUT;
                }

                aSplit.emplace_back(a);
            }
        }
        else
        {
            // Zero acceleration by default
            std::transform(vSplit.begin(), vSplit.end(),
                           std::back_inserter(aSplit),
                           [](auto const & v) -> vectorN_t
                           {
                               return vectorN_t::Zero(v.size());
                           });
        }

        for (auto & system : systems_)
        {
            for (auto const & sensorGroup : system.robot->getSensors())
            {
                for (auto const & sensor : sensorGroup.second)
                {
                    if (!sensor->getIsInitialized())
                    {
                        PRINT_ERROR("At least a sensor of a robot is not initialized.");
                        return hresult_t::ERROR_INIT_FAILED;
                    }
                }
            }

            for (auto const & motor : system.robot->getMotors())
            {
                if (!motor->getIsInitialized())
                {
                    PRINT_ERROR("At least a motor of a robot is not initialized.");
                    return hresult_t::ERROR_INIT_FAILED;
                }
            }
        }

        /* Call reset if the internal state of the engine is not clean.
           Not calling reset systematically is more flexible for the user.
           It gives the opportunity to customize the system by resetting
           first the engine manually and then to alter the internal state
           of the system before starting a simulation, e.g. to change the
           inertia of a specific body. */
        if (isTelemetryConfigured_)
        {
            reset(false, false);
        }

        // Reset the internal state of the robot and controller
        auto systemIt = systems_.begin();
        auto systemDataIt = systemsDataHolder_.begin();
        for ( ; systemIt != systems_.end(); ++systemIt, ++systemDataIt)
        {
            // Propagate the user-defined gravity at robot level
            systemIt->robot->pncModelOrig_.gravity = engineOptions_->world.gravity;
            systemIt->robot->pncModel_.gravity = engineOptions_->world.gravity;

            /* Reinitialize the system state buffers, since the robot kinematic may have changed.
               For example, it may happens if one activates or deactivates the flexibility between
               two successive simulations. */
            systemDataIt->state.initialize(*(systemIt->robot));
            systemDataIt->statePrev.initialize(*(systemIt->robot));
        }

        // Initialize the ode solver
        auto systemOde = [this](float64_t              const & t,
                                std::vector<vectorN_t> const & q,
                                std::vector<vectorN_t> const & v,
                                std::vector<vectorN_t>       & a) -> void
                         {
                             this->computeSystemsDynamics(t, q, v, a);
                         };
        std::vector<Robot const *> robots;
        robots.reserve(systems_.size());
        std::transform(systems_.begin(), systems_.end(),
                        std::back_inserter(robots),
                        [](auto const & sys) -> Robot const *
                        {
                            return sys.robot.get();
                        });
        if (engineOptions_->stepper.odeSolver == "runge_kutta_dopri5")
        {
            stepper_ = std::unique_ptr<AbstractStepper>(
                new RungeKuttaDOPRIStepper(systemOde,
                                           robots,
                                           engineOptions_->stepper.tolAbs,
                                           engineOptions_->stepper.tolRel));
        }
        else if (engineOptions_->stepper.odeSolver == "runge_kutta_4")
        {
            stepper_ = std::unique_ptr<AbstractStepper>(
                new RungeKutta4Stepper(systemOde, robots));
        }
        else if (engineOptions_->stepper.odeSolver == "euler_explicit")
        {
            stepper_ = std::unique_ptr<AbstractStepper>(
                new EulerExplicitStepper(systemOde, robots));
        }

        // Initialize the stepper state
        float64_t const t = 0.0;
        stepperState_.reset(SIMULATION_MIN_TIMESTEP, qSplit, vSplit, aSplit);

        // Initialize previous joints forces and accelerations
        contactForcesPrev_.clear();
        fPrev_.clear();
        aPrev_.clear();
        contactForcesPrev_.reserve(systems_.size());
        fPrev_.reserve(systems_.size());
        aPrev_.reserve(systems_.size());
        for (auto const & system : systems_)
        {
            contactForcesPrev_.push_back(system.robot->contactForces_);
            fPrev_.push_back(system.robot->pncData_.f);
            aPrev_.push_back(system.robot->pncData_.a);
        }

        // Synchronize the individual system states with the global stepper state
        syncSystemsStateWithStepper();

        // Update the frame indices associated with the coupling forces
        for (auto & force : forcesCoupling_)
        {
            getFrameIdx(systems_[force.systemIdx1].robot->pncModel_,
                        force.frameName1,
                        force.frameIdx1);
            getFrameIdx(systems_[force.systemIdx2].robot->pncModel_,
                        force.frameName2,
                        force.frameIdx2);
        }

        systemIt = systems_.begin();
        systemDataIt = systemsDataHolder_.begin();
        for ( ; systemIt != systems_.end(); ++systemIt, ++systemDataIt)
        {
            // Update the frame indices associated with the impulse forces and force profiles
            for (auto & force : systemDataIt->forcesProfile)
            {
                getFrameIdx(systemIt->robot->pncModel_,
                            force.frameName,
                            force.frameIdx);
            }
            for (auto & force : systemDataIt->forcesImpulse)
            {
                getFrameIdx(systemIt->robot->pncModel_,
                            force.frameName,
                            force.frameIdx);
            }

            // Initialize the impulse force breakpoint point iterator
            systemDataIt->forcesImpulseBreakNextIt = systemDataIt->forcesImpulseBreaks.begin();

            // Reset the active set of impulse forces
            std::fill(systemDataIt->forcesImpulseActive.begin(),
                      systemDataIt->forcesImpulseActive.end(),
                      false);

            // Activate every force impulse starting at t=0
            auto forcesImpulseActiveIt = systemDataIt->forcesImpulseActive.begin();
            auto forcesImpulseIt = systemDataIt->forcesImpulse.begin();
            for ( ; forcesImpulseIt != systemDataIt->forcesImpulse.end() ;
                ++forcesImpulseActiveIt, ++forcesImpulseIt)
            {
                if (forcesImpulseIt->t < STEPPER_MIN_TIMESTEP)
                {
                    *forcesImpulseActiveIt = true;
                }
            }

            // Compute the forward kinematics for each system
            vectorN_t const & q = systemDataIt->state.q;
            vectorN_t const & v = systemDataIt->state.v;
            vectorN_t const & a = systemDataIt->state.a;
            computeForwardKinematics(*systemIt, q, v, a);

            /* Backup constraint register for fast lookup.
               Internal constraints cannot be added/removed at this point. */
            systemDataIt->constraintsHolder = systemIt->robot->getConstraints();

            // Initialize contacts forces in local frame
            std::vector<frameIndex_t> const & contactFramesIdx = systemIt->robot->getContactFramesIdx();
            systemDataIt->contactFramesForces = forceVector_t(
                contactFramesIdx.size(), pinocchio::Force::Zero());
            std::vector<std::vector<pairIndex_t> > const & collisionPairsIdx =
                systemIt->robot->getCollisionPairsIdx();
            systemDataIt->collisionBodiesForces.clear();
            systemDataIt->collisionBodiesForces.reserve(collisionPairsIdx.size());
            for (std::size_t i = 0; i < collisionPairsIdx.size(); ++i)
            {
                systemDataIt->collisionBodiesForces.emplace_back(
                    collisionPairsIdx[i].size(), pinocchio::Force::Zero());
            }

            // Initialize some addition buffers used by impulse contact solver
            systemDataIt->jointJacobian.setZero(6, systemIt->robot->pncModel_.nv);

            // Reset the constraints
            returnCode = systemIt->robot->resetConstraints(q, v);

            /* Set Baumgarte stabilization natural frequency for contact constraints
               Enable all contact constraints by default, it will be disable automatically
               if not in contact. It is useful to start in post-hysteresis state to avoid
               discontinuities at init. */
            systemDataIt->constraintsHolder.foreach(
                [& contactModel = contactModel_,
                 & enablePositionLimit = systemIt->robot->mdlOptions_->joints.enablePositionLimit,
                 & freq = engineOptions_->contacts.stabilizationFreq](
                    std::shared_ptr<AbstractConstraintBase> const & constraint,
                    constraintsHolderType_t const & holderType)
                {
                    // Set baumgarte freq for all contact constraints
                    if (holderType != constraintsHolderType_t::USER)
                    {
                        constraint->setBaumgarteFreq(freq);  // It cannot fail
                    }

                    // Enable constraints by default
                    if (contactModel == contactModel_t::CONSTRAINT)
                    {
                        switch (holderType)
                        {
                        case constraintsHolderType_t::BOUNDS_JOINTS:
                            if (!enablePositionLimit)
                            {
                                return;
                            }
                            [[fallthrough]];
                        case constraintsHolderType_t::CONTACT_FRAMES:
                        case constraintsHolderType_t::COLLISION_BODIES:
                            constraint->enable();
                            [[fallthrough]];
                        case constraintsHolderType_t::USER:
                        default:
                            break;
                        }
                    }
                });

            if (contactModel_ == contactModel_t::SPRING_DAMPER)
            {
                // Make sure that the contact forces are bounded for spring-damper model.
                // TODO: One should rather use something like 10 * m * g instead of a fix threshold
                float64_t forceMax = 0.0;
                for (std::size_t i = 0; i < contactFramesIdx.size(); ++i)
                {
                    auto & constraint = systemDataIt->constraintsHolder.contactFrames[i].second;
                    pinocchio::Force & fextLocal = systemDataIt->contactFramesForces[i];
                    computeContactDynamicsAtFrame(
                        *systemIt, contactFramesIdx[i], constraint, fextLocal);
                    forceMax = std::max(forceMax, fextLocal.linear().norm());
                }

                for (std::size_t i = 0; i < collisionPairsIdx.size(); ++i)
                {
                    for (std::size_t j = 0; j < collisionPairsIdx[i].size(); ++j)
                    {
                        pairIndex_t const & collisionPairIdx = collisionPairsIdx[i][j];
                        auto & constraint = systemDataIt->constraintsHolder.collisionBodies[i][j].second;
                        pinocchio::Force & fextLocal = systemDataIt->collisionBodiesForces[i][j];
                        computeContactDynamicsAtBody(
                            *systemIt, collisionPairIdx, constraint, fextLocal);
                        forceMax = std::max(forceMax, fextLocal.linear().norm());
                    }
                }

                if (forceMax > 1e5)
                {
                    PRINT_ERROR("The initial force exceeds 1e5 for at least one contact point, "
                                "which is forbidden for the sake of numerical stability. Please "
                                "update the initial state.");
                    return hresult_t::ERROR_BAD_INPUT;
                }
            }
        }

        systemIt = systems_.begin();
        systemDataIt = systemsDataHolder_.begin();
        for ( ; systemIt != systems_.end(); ++systemIt, ++systemDataIt)
        {
            if (returnCode == hresult_t::SUCCESS)
            {
                // Lock the robot. At this point it is no longer possible to change the robot anymore.
                returnCode = systemIt->robot->getLock(systemDataIt->robotLock);
            }
        }

        if (returnCode == hresult_t::SUCCESS)
        {
            // Instantiate the desired LCP solver
            systemIt = systems_.begin();
            systemDataIt = systemsDataHolder_.begin();
            for ( ; systemIt != systems_.end(); ++systemIt, ++systemDataIt)
            {
                std::string const & constraintSolverType = engineOptions_->constraints.solver;
                switch (CONSTRAINT_SOLVERS_MAP.at(constraintSolverType))
                {
                case constraintSolver_t::PGS:
                    systemDataIt->constraintSolver = std::make_unique<PGSSolver>(
                        &systemIt->robot->pncModel_,
                        &systemIt->robot->pncData_,
                        &systemDataIt->constraintsHolder,
                        engineOptions_->contacts.friction,
                        engineOptions_->contacts.torsion,
                        engineOptions_->stepper.tolAbs,
                        engineOptions_->stepper.tolRel,
                        PGS_MAX_ITERATIONS);
                        break;
                case constraintSolver_t::NONE:
                default:
                    break;
                }
            }

            /* Compute the efforts, internal and external forces applied on every
               systems excluding user-specified internal dynamics if any. */
            computeAllTerms(t, qSplit, vSplit);

            // Backup all external forces and internal efforts excluding constraint forces
            vector_aligned_t<forceVector_t> fextNoConst;
            std::vector<vectorN_t> uInternalConst;
            fextNoConst.reserve(systems_.size());
            uInternalConst.reserve(systems_.size());
            for (auto const & systemData : systemsDataHolder_)
            {
                fextNoConst.push_back(systemData.state.fExternal);
                uInternalConst.push_back(systemData.state.uInternal);
            }

            /* Solve algebric coupling between accelerations, sensors and controllers,
               by iterating several times until it (hopefully) converges. */
            for (uint32_t i = 0; i < INIT_ITERATIONS; ++i)
            {
                systemIt = systems_.begin();
                systemDataIt = systemsDataHolder_.begin();
                auto fextNoConstIt = fextNoConst.begin();
                auto uInternalConstIt = uInternalConst.begin();
                for ( ; systemIt != systems_.end();
                    ++systemIt, ++systemDataIt, ++fextNoConstIt, ++uInternalConstIt)
                {
                    // Get some system state proxies
                    vectorN_t const & q = systemDataIt->state.q;
                    vectorN_t const & v = systemDataIt->state.v;
                    vectorN_t & a = systemDataIt->state.a;
                    vectorN_t & u = systemDataIt->state.u;
                    vectorN_t & command = systemDataIt->state.command;
                    vectorN_t & uMotor = systemDataIt->state.uMotor;
                    vectorN_t & uInternal = systemDataIt->state.uInternal;
                    vectorN_t & uCustom = systemDataIt->state.uCustom;
                    forceVector_t & fext = systemDataIt->state.fExternal;

                    // Reset the external forces and internal efforts
                    fext = *fextNoConstIt;
                    uInternal = *uInternalConstIt;

                    // Compute dynamics
                    a = computeAcceleration(*systemIt, *systemDataIt, q, v, u, fext, i == 0);

                    // Make sure there is no nan at this point
                    if ((a.array() != a.array()).any())
                    {
                        PRINT_ERROR("Impossible to compute the acceleration. Probably a "
                                    "subtree has zero inertia along an articulated axis.");
                        return hresult_t::ERROR_GENERIC;
                    }

                    // Compute the sensor data with the updated effort and acceleration
                    systemIt->robot->setSensorsData(t, q, v, a, uMotor, fext);

                    // Compute the actual motor effort
                    computeCommand(*systemIt, t, q, v, command);

                    // Compute the actual motor effort
                    systemIt->robot->computeMotorsEfforts(t, q, v, a, command);
                    uMotor = systemIt->robot->getMotorsEfforts();

                    // Compute the internal dynamics
                    uCustom.setZero();
                    systemIt->controller->internalDynamics(t, q, v, uCustom);

                    // Compute the total effort vector
                    u = uInternal + uCustom;
                    for (auto const & motor : systemIt->robot->getMotors())
                    {
                        std::size_t const & motorIdx = motor->getIdx();
                        int32_t const & motorVelocityIdx = motor->getJointVelocityIdx();
                        u[motorVelocityIdx] += uMotor[motorIdx];
                    }
                }
            }

            // Update sensor data one last time to take into account the actual acceleration
            systemIt = systems_.begin();
            systemDataIt = systemsDataHolder_.begin();
            for ( ; systemIt != systems_.end(); ++systemIt, ++systemDataIt)
            {
                vectorN_t const & q = systemDataIt->state.q;
                vectorN_t const & v = systemDataIt->state.v;
                vectorN_t const & a = systemDataIt->state.a;
                vectorN_t const & uMotor = systemDataIt->state.uMotor;
                forceVector_t const & fext = systemDataIt->state.fExternal;
                systemIt->robot->setSensorsData(t, q, v, a, uMotor, fext);
            }

            // Compute joints accelerations and forces
            computeAllExtraTerms(systems_, systemsDataHolder_);
            syncAllAccelerationsAndForces(systems_, contactForcesPrev_, fPrev_, aPrev_);

            // Synchronize the global stepper state with the individual system states
            syncStepperStateWithSystems();

            // Initialize the last system states
            for (auto & systemData : systemsDataHolder_)
            {
                systemData.statePrev = systemData.state;
            }

            // Lock the telemetry. At this point it is no longer possible to register new variables.
            configureTelemetry();

            // Log systems data
            for (auto const & system : systems_)
            {
                // Backup URDF file
                std::string const telemetryUrdfFile = addCircumfix(
                    "urdf_file", system.name, "", TELEMETRY_FIELDNAME_DELIMITER);
                std::string const & urdfFileString = system.robot->getUrdfAsString();
                telemetrySender_.registerConstant(telemetryUrdfFile, urdfFileString);

                // Backup 'has_freeflyer' option
                std::string const telemetrHasFreeflyer = addCircumfix(
                    "has_freeflyer", system.name, "", TELEMETRY_FIELDNAME_DELIMITER);
                telemetrySender_.registerConstant(
                    telemetrHasFreeflyer, std::to_string(system.robot->getHasFreeflyer()));

                // Backup mesh package lookup directories
                std::string const telemetryMeshPackageDirs = addCircumfix(
                    "mesh_package_dirs", system.name, "", TELEMETRY_FIELDNAME_DELIMITER);
                std::string meshPackageDirsString;
                std::stringstream meshPackageDirsStream;
                std::vector<std::string> const & meshPackageDirs = system.robot->getMeshPackageDirs();
                copy(meshPackageDirs.begin(), meshPackageDirs.end(),
                     std::ostream_iterator<std::string>(meshPackageDirsStream, ";"));
                if (meshPackageDirsStream.peek() != decltype(meshPackageDirsStream)::traits_type::eof())
                {
                    meshPackageDirsString = meshPackageDirsStream.str();
                    meshPackageDirsString.pop_back();
                }
                telemetrySender_.registerConstant(telemetryMeshPackageDirs, meshPackageDirsString);

                // Backup the true and theoretical Pinocchio::Model
                std::string key = addCircumfix(
                    "pinocchio_model", system.name, "", TELEMETRY_FIELDNAME_DELIMITER);
                std::string value = saveToBinary(system.robot->pncModel_);
                telemetrySender_.registerConstant(key, value);

                /* Backup the Pinocchio GeometryModel for collisions and visuals.
                   It may fail because of missing serialization methods for convex,
                   or because it cannot fit into memory (return code).
                   Persistent mode is automatically enforced if no URDF is associated
                   with the robot.*/
                if (engineOptions_->telemetry.isPersistent || urdfFileString.empty())
                {
                    try
                    {
                        key = addCircumfix(
                            "collision_model", system.name, "", TELEMETRY_FIELDNAME_DELIMITER);
                        value = saveToBinary(system.robot->collisionModel_);
                        telemetrySender_.registerConstant(key, value);

                        key = addCircumfix(
                            "visual_model", system.name, "", TELEMETRY_FIELDNAME_DELIMITER);
                        value = saveToBinary(system.robot->visualModel_);
                        telemetrySender_.registerConstant(key, value);
                    }
                    catch (std::exception const & e)
                    {
                        std::string msg = "Failed to log the collision and/or visual model.";
                        if (urdfFileString.empty())
                        {
                            msg += " It will be impossible to replay log files because no URDF file is available as fallback.";
                        }
                        msg += "\nRaised from exception: ";
                        PRINT_ERROR(msg, e.what());
                    }
                }
            }

            // Log all options
            configHolder_t allOptions;
            for (auto const & system : systems_)
            {
                std::string const telemetryRobotOptions = addCircumfix(
                    "system", system.name, "", TELEMETRY_FIELDNAME_DELIMITER);
                configHolder_t systemOptions;
                systemOptions["robot"] = system.robot->getOptions();
                systemOptions["controller"] = system.controller->getOptions();
                allOptions[telemetryRobotOptions] = systemOptions;
            }
            allOptions["engine"] = engineOptionsHolder_;
            Json::Value allOptionsJson = convertToJson(allOptions);
            Json::StreamWriterBuilder jsonWriter;
            jsonWriter["indentation"] = "";
            std::string const allOptionsString = Json::writeString(jsonWriter, allOptionsJson);
            telemetrySender_.registerConstant("options", allOptionsString);

            // Write the header: this locks the registration of new variables
            telemetryRecorder_->initialize(telemetryData_.get(), getTelemetryTimeUnit());

            // At this point, consider that the simulation is running
            isSimulationRunning_ = true;
        }

        return returnCode;
    }

    hresult_t EngineMultiRobot::simulate(float64_t                        const & tEnd,
                                         std::map<std::string, vectorN_t> const & qInit,
                                         std::map<std::string, vectorN_t> const & vInit,
                                         std::optional<std::map<std::string, vectorN_t> > const & aInit)
    {
        hresult_t returnCode = hresult_t::SUCCESS;

        if (systems_.empty())
        {
            PRINT_ERROR("No system to simulate. Please add one before starting a simulation.");
            returnCode = hresult_t::ERROR_INIT_FAILED;
        }

        if (tEnd < 5e-3)
        {
            PRINT_ERROR("The duration of the simulation cannot be shorter than 5ms.");
            returnCode = hresult_t::ERROR_BAD_INPUT;
        }

        if (returnCode == hresult_t::SUCCESS)
        {
            // Reset the robot, controller, and engine
            reset(true, false);

            // Start the simulation
            returnCode = start(qInit, vInit, aInit);
        }

        // Now that telemetry has been initialized, check simulation duration.
        if (tEnd > telemetryRecorder_->getMaximumLogTime())
        {
            PRINT_ERROR("Time overflow: with the current precision the maximum value that "
                        "can be logged is ", telemetryRecorder_->getMaximumLogTime(),
                        "s. Decrease logger precision to simulate for longer than that.");
            returnCode = hresult_t::ERROR_BAD_INPUT;
        }

        // Integration loop based on boost::numeric::odeint::detail::integrate_times
        while (returnCode == hresult_t::SUCCESS)
        {
            // Stop the simulation if the end time has been reached
            if (tEnd - stepperState_.t < SIMULATION_MIN_TIMESTEP)
            {
                if (engineOptions_->stepper.verbose)
                {
                    std::cout << "Simulation done: desired final time reached." << std::endl;
                }
                break;
            }

            // Stop the simulation if any of the callbacks return false
            bool_t isCallbackFalse = false;
            auto systemIt = systems_.begin();
            auto systemDataIt = systemsDataHolder_.begin();
            for ( ; systemIt != systems_.end(); ++systemIt, ++systemDataIt)
            {
                if (!systemIt->callbackFct(stepperState_.t,
                                           systemDataIt->state.q,
                                           systemDataIt->state.v))
                {
                    isCallbackFalse = true;
                    break;
                }
            }
            if (isCallbackFalse)
            {
                if (engineOptions_->stepper.verbose)
                {
                    std::cout << "Simulation done: callback returned false." << std::endl;
                }
                break;
            }

            // Stop the simulation if the max number of integration steps is reached
            if (0U < engineOptions_->stepper.iterMax
                && engineOptions_->stepper.iterMax <= stepperState_.iter)
            {
                if (engineOptions_->stepper.verbose)
                {
                    std::cout << "Simulation done: maximum number of integration steps exceeded." << std::endl;
                }
                break;
            }

            // Perform a single integration step up to tEnd, stopping at stepperUpdatePeriod_ to log
            float64_t stepSize;
            if (std::isfinite(stepperUpdatePeriod_))
            {
                stepSize = min(stepperUpdatePeriod_, tEnd - stepperState_.t);
            }
            else
            {
                stepSize = min(engineOptions_->stepper.dtMax, tEnd - stepperState_.t);
            }
            returnCode = step(stepSize);  // Automatic dt adjustment
        }

        // Stop the simulation. New variables can be registered again, and the lock on the robot is released
        stop();

        return returnCode;
    }

    hresult_t EngineMultiRobot::step(float64_t stepSize)
    {
        hresult_t returnCode = hresult_t::SUCCESS;

        // Check if the simulation has started
        if (!isSimulationRunning_)
        {
            PRINT_ERROR("No simulation running. Please start it before using step method.");
            return hresult_t::ERROR_GENERIC;
        }

        // Clear log data buffer
        logData_ = nullptr;

        // Check if there is something wrong with the integration
        auto qIt = stepperState_.qSplit.begin();
        auto vIt = stepperState_.vSplit.begin();
        auto aIt = stepperState_.aSplit.begin();
        for ( ; qIt != stepperState_.qSplit.end(); ++qIt, ++vIt, ++aIt)
        {
            if ((qIt->array() != qIt->array()).any() ||
                (vIt->array() != vIt->array()).any() ||
                (aIt->array() != aIt->array()).any()) // isnan if NOT equal to itself
            {
                PRINT_ERROR("The low-level ode solver failed. Consider increasing the stepper accuracy.");
                return hresult_t::ERROR_GENERIC;
            }
        }

        // Check if the desired step size is suitable
        if (stepSize > EPS && stepSize < SIMULATION_MIN_TIMESTEP)
        {
            PRINT_ERROR("The requested step size is out of bounds.");
            return hresult_t::ERROR_BAD_INPUT;
        }

        /* Set end time: The default step size is equal to the
           controller update period if discrete-time, otherwise
           it uses the sensor update period if discrete-time,
           otherwise it uses the user-defined parameter dtMax. */
        if (stepSize < EPS)
        {
            float64_t const & controllerUpdatePeriod = engineOptions_->stepper.controllerUpdatePeriod;
            if (controllerUpdatePeriod > EPS)
            {
                stepSize = controllerUpdatePeriod;
            }
            else
            {
                float64_t const & sensorsUpdatePeriod = engineOptions_->stepper.sensorsUpdatePeriod;
                if (sensorsUpdatePeriod > EPS)
                {
                    stepSize = sensorsUpdatePeriod;
                }
                else
                {
                    stepSize = engineOptions_->stepper.dtMax;
                }
            }
        }

        /* Check that end time is not too large for the current
           logging precision, otherwise abort integration. */
        if (stepperState_.t + stepSize > telemetryRecorder_->getMaximumLogTime())
        {
            PRINT_ERROR("Time overflow: with the current precision the maximum value that "
                        "can be logged is ", telemetryRecorder_->getMaximumLogTime(),
                        "s. Decrease logger precision to simulate for longer than that.");
            return hresult_t::ERROR_GENERIC;
        }

        /* Avoid compounding of error using Kahan algorithm. It
           consists in keeping track of the cumulative rounding error
           to add it back to the sum when it gets larger than the
           numerical precision, thus avoiding it to grows unbounded. */
        float64_t stepSize_true = stepSize - stepperState_.tError;
        float64_t tEnd = stepperState_.t + stepSize_true;
        stepperState_.tError = (tEnd - stepperState_.t) - stepSize_true;

        // Get references to some internal stepper buffers
        float64_t & t = stepperState_.t;
        float64_t & dt = stepperState_.dt;
        float64_t & dtLargest = stepperState_.dtLargest;
        std::vector<vectorN_t> & qSplit = stepperState_.qSplit;
        std::vector<vectorN_t> & vSplit = stepperState_.vSplit;
        std::vector<vectorN_t> & aSplit = stepperState_.aSplit;

        // Monitor iteration failure
        uint32_t successiveIterFailed = 0;
        bool_t isNan = false;

        /* Flag monitoring if the current time step depends of a breakpoint
           or the integration tolerance. It will be used by the restoration
           mechanism, if dt gets very small to reach a breakpoint, in order
           to avoid having to perform several steps to stabilize again the
           estimation of the optimal time step. */
        bool_t isBreakpointReached = false;

        /* Flag monitoring if the dynamics has changed because of impulse
           forces or the command (only in the case of discrete control).

           `tryStep(rhs, x, dxdt, t, dt)` method of error controlled boost
           steppers leverage the FSAL (first same as last) principle. It is
           implemented by considering at the value of (x, dxdt) in argument
           have been initialized by the user with the system dynamics at
           current time t. Thus, if the system dynamics is discontinuous,
           one has to manually integrate up to t-, then update dxdt to take
           into the acceleration at t+.

           Note that ONLY the acceleration part of dxdt must be updated since
           the  projection of the velocity on the state space is not supposed
           to have changed, and on top of that tPrev is invalid at this point
           because it has been updated just after the last successful step.

           Note that the estimated dt is no longer very meaningful since the
           dynamics has changed. Maybe dt should be reschedule... */
        bool_t hasDynamicsChanged = false;

        // Start the timer used for timeout handling
        timer_->tic();

        // Perform the integration. Do not simulate extremely small time steps.
        while ((tEnd - t >= STEPPER_MIN_TIMESTEP) && (returnCode == hresult_t::SUCCESS))
        {
            // Initialize next breakpoint time to the one recommended by the stepper
            float64_t tNext = t;

            // Update the active set and get the next breakpoint of impulse forces
            float64_t tForceImpulseNext = INF;
            for (auto & systemData : systemsDataHolder_)
            {
                /* Update the active set: activate an impulse force as soon as
                   the current time gets close enough of the application time,
                   and deactivate it once the following the same reasoning.

                   Note that breakpoints at the begining and the end of every
                   impulse force at already enforced, so that the forces
                   cannot get activated/desactivate too late. */
                auto forcesImpulseActiveIt = systemData.forcesImpulseActive.begin();
                auto forcesImpulseIt = systemData.forcesImpulse.begin();
                for ( ; forcesImpulseIt != systemData.forcesImpulse.end() ;
                    ++forcesImpulseActiveIt, ++forcesImpulseIt)
                {
                    float64_t const & tForceImpulse = forcesImpulseIt->t;
                    float64_t const & dtForceImpulse = forcesImpulseIt->dt;

                    if (t > tForceImpulse - STEPPER_MIN_TIMESTEP)
                    {
                        *forcesImpulseActiveIt = true;
                        hasDynamicsChanged = true;
                    }
                    if (t >= tForceImpulse + dtForceImpulse - STEPPER_MIN_TIMESTEP)
                    {
                        *forcesImpulseActiveIt = false;
                        hasDynamicsChanged = true;
                    }
                }

                // Update the breakpoint time iterator if necessary
                auto & tBreakNextIt = systemData.forcesImpulseBreakNextIt;
                if (tBreakNextIt != systemData.forcesImpulseBreaks.end())
                {
                    if (t >= *tBreakNextIt - STEPPER_MIN_TIMESTEP)
                    {
                        // The current breakpoint is behind in time. Switching to the next one.
                        ++tBreakNextIt;
                    }
                }

                // Get the next breakpoint time if any
                if (tBreakNextIt != systemData.forcesImpulseBreaks.end())
                {
                    tForceImpulseNext = min(tForceImpulseNext, *tBreakNextIt);
                }
            }

            // Update the external force profiles if necessary (only for finite update frequency)
            if (std::isfinite(stepperUpdatePeriod_))
            {
                auto systemIt = systems_.begin();
                auto systemDataIt = systemsDataHolder_.begin();
                for ( ; systemIt != systems_.end(); ++systemIt, ++systemDataIt)
                {
                    for (auto & forceProfile : systemDataIt->forcesProfile)
                    {
                        if (forceProfile.updatePeriod > EPS)
                        {
                            float64_t const & forceUpdatePeriod = forceProfile.updatePeriod;
                            float64_t dtNextForceUpdatePeriod = forceUpdatePeriod - std::fmod(t, forceUpdatePeriod);
                            if (dtNextForceUpdatePeriod < SIMULATION_MIN_TIMESTEP
                            || forceUpdatePeriod - dtNextForceUpdatePeriod < STEPPER_MIN_TIMESTEP)
                            {
                                vectorN_t const & q = systemDataIt->state.q;
                                vectorN_t const & v = systemDataIt->state.v;
                                forceProfile.forcePrev = forceProfile.forceFct(t, q, v);
                                hasDynamicsChanged = true;
                            }
                        }
                    }
                }
            }

            // Update the controller command if necessary (only for finite update frequency)
            if (std::isfinite(stepperUpdatePeriod_) && engineOptions_->stepper.controllerUpdatePeriod > EPS)
            {
                float64_t const & controllerUpdatePeriod = engineOptions_->stepper.controllerUpdatePeriod;
                float64_t dtNextControllerUpdatePeriod = controllerUpdatePeriod - std::fmod(t, controllerUpdatePeriod);
                if (dtNextControllerUpdatePeriod < SIMULATION_MIN_TIMESTEP
                || controllerUpdatePeriod - dtNextControllerUpdatePeriod < STEPPER_MIN_TIMESTEP)
                {
                    auto systemIt = systems_.begin();
                    auto systemDataIt = systemsDataHolder_.begin();
                    for ( ; systemIt != systems_.end(); ++systemIt, ++systemDataIt)
                    {
                        vectorN_t const & q = systemDataIt->state.q;
                        vectorN_t const & v = systemDataIt->state.v;
                        vectorN_t & command = systemDataIt->state.command;
                        computeCommand(*systemIt, t, q, v, command);
                    }
                    hasDynamicsChanged = true;
                }
            }

            /* Update telemetry if necessary.
               It monitors the current iteration number, the current time, and the
               systems state, command, and sensors data.
               Note that the acceleration is logged BEFORE updating the dynamics if the
               command has been updated. The acceleration is discontinuous so their is
               no way to log both the acceleration at the end of the previous step and
               at the beginning of the next. Logging the previous acceleration is more
               natural since it preserves the consistency between sensors data and
               robot state.
               */
            if (!std::isfinite(stepperUpdatePeriod_) || !engineOptions_->stepper.logInternalStepperSteps)
            {
                bool_t mustUpdateTelemetry = !std::isfinite(stepperUpdatePeriod_);
                if (!mustUpdateTelemetry)
                {
                    float64_t dtNextStepperUpdatePeriod = stepperUpdatePeriod_ - std::fmod(t, stepperUpdatePeriod_);
                    mustUpdateTelemetry = (dtNextStepperUpdatePeriod < SIMULATION_MIN_TIMESTEP
                    || stepperUpdatePeriod_ - dtNextStepperUpdatePeriod < STEPPER_MIN_TIMESTEP);
                }
                if (mustUpdateTelemetry)
                {
                    updateTelemetry();
                }
            }

            // Fix the FSAL issue if the dynamics has changed
            if (!std::isfinite(stepperUpdatePeriod_) && hasDynamicsChanged)
            {
                // TODO: only update quantities acting at force/acceleration-level
                computeSystemsDynamics(t, qSplit, vSplit, aSplit);
                computeAllExtraTerms(systems_, systemsDataHolder_);
                syncAllAccelerationsAndForces(systems_, contactForcesPrev_, fPrev_, aPrev_);
                syncSystemsStateWithStepper(true);
                hasDynamicsChanged = false;
            }

            if (std::isfinite(stepperUpdatePeriod_))
            {
                /* Get the time of the next breakpoint for the ODE solver:
                   a breakpoint occurs if we reached tEnd, if an external force
                   is applied, or if we need to update the sensors / controller. */
                float64_t dtNextGlobal;  // dt to apply for the next stepper step because of the various breakpoints
                float64_t const dtNextUpdatePeriod = stepperUpdatePeriod_ - std::fmod(t, stepperUpdatePeriod_);
                if (dtNextUpdatePeriod < SIMULATION_MIN_TIMESTEP)
                {
                    /* Step to reach next sensors/controller update is too short:
                       skip one controller update and jump to the next one.
                       Note that in this case, the sensors have already been
                       updated in anticipation in previous loop. */
                    dtNextGlobal = min(dtNextUpdatePeriod + stepperUpdatePeriod_,
                                       tForceImpulseNext - t);
                }
                else
                {
                    dtNextGlobal = min(dtNextUpdatePeriod, tForceImpulseNext - t);
                }

                /* Check if the next dt to about equal to the time difference
                   between the current time (it can only be smaller) and
                   enforce next dt to exactly match this value in such a case. */
                if (tEnd - t - STEPPER_MIN_TIMESTEP < dtNextGlobal)
                {
                    dtNextGlobal = tEnd - t;
                }

                // Update next dt
                tNext += dtNextGlobal;

                // Compute the next step using adaptive step method
                while (tNext - t > STEPPER_MIN_TIMESTEP)
                {
                    // Log every stepper state only if the user asked for
                    if (successiveIterFailed == 0 && engineOptions_->stepper.logInternalStepperSteps)
                    {
                        updateTelemetry();
                    }

                    // Fix the FSAL issue if the dynamics has changed
                    if (hasDynamicsChanged)
                    {
                        // TODO: only update quantities acting at force/acceleration-level
                        computeSystemsDynamics(t, qSplit, vSplit, aSplit);
                        computeAllExtraTerms(systems_, systemsDataHolder_);
                        syncAllAccelerationsAndForces(systems_, contactForcesPrev_, fPrev_, aPrev_);
                        syncSystemsStateWithStepper(true);
                        hasDynamicsChanged = false;
                    }

                    // Adjust stepsize to end up exactly at the next breakpoint
                    dt = min(dt, tNext - t);
                    if (dtLargest > SIMULATION_MIN_TIMESTEP)
                    {
                        if (tNext - (t + dt) < SIMULATION_MIN_TIMESTEP)
                        {
                            dt = tNext - t;
                        }
                    }
                    else
                    {
                        if (tNext - (t + dt) < STEPPER_MIN_TIMESTEP)
                        {
                            dt = tNext - t;
                        }
                    }

                    /* Trying to reach multiples of STEPPER_MIN_TIMESTEP whenever
                       possible. The idea here is to reach only multiples of 1us,
                       making logging easier, given that, 1us can be consider an
                       'infinitesimal' time in robotics. This arbitrary threshold
                       many not be suited for simulating different, faster
                       dynamics, that require sub-microsecond precision. */
                    if (dt > SIMULATION_MIN_TIMESTEP)
                    {
                        float64_t const dtResidual = std::fmod(dt, SIMULATION_MIN_TIMESTEP);
                        if (dtResidual > STEPPER_MIN_TIMESTEP
                            && dtResidual < SIMULATION_MIN_TIMESTEP - STEPPER_MIN_TIMESTEP
                            && dt - dtResidual > STEPPER_MIN_TIMESTEP)
                        {
                            dt -= dtResidual;
                        }
                    }

                    /* Break the loop if dt is getting too small.
                       Don't worry, an exception will be raised later. */
                    if (dt < STEPPER_MIN_TIMESTEP)
                    {
                        break;
                    }

                    /* Break the loop in case of timeout.
                       Don't worry, an exception will be raised later. */
                    timer_->toc();
                    if (EPS < engineOptions_->stepper.timeout
                        && engineOptions_->stepper.timeout < timer_->dt)
                    {
                        break;
                    }

                    // Break the loop in case of too many successive failed inner iteration
                    if (successiveIterFailed > engineOptions_->stepper.successiveIterFailedMax)
                    {
                        break;
                    }

                    /* A breakpoint has been reached dt has been decreased
                       wrt the largest possible dt within integration tol. */
                    isBreakpointReached = (dtLargest > dt);

                    // Set the timestep to be tried by the stepper
                    dtLargest = dt;

                    // Try doing one integration step
                    bool_t isStepSuccessful = stepper_->tryStep(qSplit, vSplit, aSplit, t, dtLargest);

                    /* Check if the integrator failed miserably even if successfully.
                       It would happen if integration failed because of nan and the
                       timestep is not adaptive. */
                    isNan = std::isnan(dtLargest);
                    if (isNan)
                    {
                        break;
                    }

                    // Update buffer if really successful
                    if (isStepSuccessful)
                    {
                        // Reset successive iteration failure counter
                        successiveIterFailed = 0;

                        /* Compute the actual joint acceleration and forces, based on
                           up-to-date pinocchio::Data. */
                        computeAllExtraTerms(systems_, systemsDataHolder_);

                        // Synchronize the individual system states
                        syncAllAccelerationsAndForces(systems_, contactForcesPrev_, fPrev_, aPrev_);
                        syncSystemsStateWithStepper();

                        // Increment the iteration counter only for successful steps
                        ++stepperState_.iter;

                        /* Restore the step size dt if it has been significantly
                           decreased to because of a breakpoint. It is set
                           equal to the last available largest dt to be known,
                           namely the second to last successfull step. */
                        if (isBreakpointReached)
                        {
                            /* Restore the step size if and only if:
                               - the next estimated largest step size is larger than
                                 the requested one for the current (successful) step.
                               - the next estimated largest step size is significantly
                                 smaller than the estimated largest step size for the
                                 previous step. */
                            float64_t dtRestoreThresholdAbs = stepperState_.dtLargestPrev *
                                engineOptions_->stepper.dtRestoreThresholdRel;
                            if (dt < dtLargest && dtLargest < dtRestoreThresholdAbs)
                            {
                                dtLargest = stepperState_.dtLargestPrev;
                            }
                        }

                        /* Backup the stepper and systems' state on success only:
                           - t at last successful iteration is used to compute dt,
                             which is project the accelation in the state space
                             instead of SO3^2.
                           - dtLargestPrev is used to restore the largest step
                             size in case of a breakpoint requiring lowering it.
                           - the acceleration and effort at the last successful
                             iteration is used to update the sensors' data in
                             case of continuous sensing. */
                        stepperState_.tPrev = t;
                        stepperState_.dtLargestPrev = dtLargest;
                        for (auto & systemData : systemsDataHolder_)
                        {
                            systemData.statePrev = systemData.state;
                        }
                    }
                    else
                    {
                        // Increment the failed iteration counters
                        ++successiveIterFailed;
                        ++stepperState_.iterFailed;
                    }

                    // Initialize the next dt
                    dt = min(dtLargest, engineOptions_->stepper.dtMax);
                }
            }
            else
            {
                /* Make sure it ends exactly at the tEnd, never exceeds
                   dtMax, and stop to apply impulse forces. */
                dt = min(dt,
                         tEnd - t,
                         tForceImpulseNext - t);

                /* A breakpoint has been reached, because dt has been decreased
                   wrt the largest possible dt within integration tol. */
                isBreakpointReached = (dtLargest > dt);

                // Compute the next step using adaptive step method
                bool_t isStepSuccessful = false;
                while (!isStepSuccessful)
                {
                    // Set the timestep to be tried by the stepper
                    dtLargest = dt;

                    // Break the loop in case of too many successive failed inner iteration
                    if (successiveIterFailed > engineOptions_->stepper.successiveIterFailedMax)
                    {
                        break;
                    }

                    // Try to do a step
                    isStepSuccessful = stepper_->tryStep(qSplit, vSplit, aSplit, t, dtLargest);

                    // Check if the integrator failed miserably even if successfully
                    isNan = std::isnan(dtLargest);
                    if (isNan)
                    {
                        break;
                    }

                    if (isStepSuccessful)
                    {
                        // Reset successive iteration failure counter
                        successiveIterFailed = 0;

                        /* Compute the actual joint acceleration and forces, based on
                           up-to-date pinocchio::Data. */
                        computeAllExtraTerms(systems_, systemsDataHolder_);

                        // Synchronize the individual system states
                        syncAllAccelerationsAndForces(systems_, contactForcesPrev_, fPrev_, aPrev_);
                        syncSystemsStateWithStepper();

                        // Increment the iteration counter
                        ++stepperState_.iter;

                        // Restore the step size if necessary
                        if (isBreakpointReached)
                        {
                            float64_t dtRestoreThresholdAbs = stepperState_.dtLargestPrev *
                                engineOptions_->stepper.dtRestoreThresholdRel;
                            if (dt < dtLargest && dtLargest < dtRestoreThresholdAbs)
                            {
                                dtLargest = stepperState_.dtLargestPrev;
                            }
                        }

                        // Backup the stepper and systems' state
                        stepperState_.tPrev = t;
                        stepperState_.dtLargestPrev = dtLargest;
                        for (auto & systemData : systemsDataHolder_)
                        {
                            systemData.statePrev = systemData.state;
                        }
                    }
                    else
                    {
                        // Increment the failed iteration counter
                        ++successiveIterFailed;
                        ++stepperState_.iterFailed;
                    }

                    // Initialize the next dt
                    dt = min(dtLargest, engineOptions_->stepper.dtMax);
                }
            }

            // Exception handling
            if (isNan)
            {
                PRINT_ERROR("Something is wrong with the physics. Aborting integration.");
                returnCode = hresult_t::ERROR_GENERIC;
            }
            if (successiveIterFailed > engineOptions_->stepper.successiveIterFailedMax)
            {
                PRINT_ERROR("Too many successive iteration failures. Probably something is "
                            "going wrong with the physics. Aborting integration.");
                returnCode = hresult_t::ERROR_GENERIC;
            }
            if (dt < STEPPER_MIN_TIMESTEP)
            {
                PRINT_ERROR("The internal time step is getting too small. Impossible to "
                            "integrate physics further in time.");
                returnCode = hresult_t::ERROR_GENERIC;
            }
            timer_->toc();
            if (EPS < engineOptions_->stepper.timeout
                && engineOptions_->stepper.timeout < timer_->dt)
            {
                PRINT_ERROR("Step computation timeout.");
                returnCode = hresult_t::ERROR_GENERIC;
            }

            // Update sensors data if necessary, namely if time-continuous or breakpoint
            if (returnCode == hresult_t::SUCCESS)
            {
                float64_t const & sensorsUpdatePeriod = engineOptions_->stepper.sensorsUpdatePeriod;
                bool_t mustUpdateSensors = sensorsUpdatePeriod < EPS;
                float64_t dtNextSensorsUpdatePeriod = sensorsUpdatePeriod - std::fmod(t, sensorsUpdatePeriod);
                if (!mustUpdateSensors)
                {
                    mustUpdateSensors = dtNextSensorsUpdatePeriod < SIMULATION_MIN_TIMESTEP
                                     || sensorsUpdatePeriod - dtNextSensorsUpdatePeriod < STEPPER_MIN_TIMESTEP;
                }
                if (mustUpdateSensors)
                {
                    auto systemIt = systems_.begin();
                    auto systemDataIt = systemsDataHolder_.begin();
                    for ( ; systemIt != systems_.end(); ++systemIt, ++systemDataIt)
                    {
                        vectorN_t const & q = systemDataIt->state.q;
                        vectorN_t const & v = systemDataIt->state.v;
                        vectorN_t const & a = systemDataIt->state.a;
                        vectorN_t const & uMotor = systemDataIt->state.uMotor;
                        forceVector_t const & fext = systemDataIt->state.fExternal;
                        systemIt->robot->setSensorsData(t, q, v, a, uMotor, fext);
                    }
                }
            }
        }

        /* Update the final time and dt to make sure it corresponds
           to the desired values and avoid compounding of error.
           Anyway the user asked for a step of exactly stepSize,
           so he is expecting this value to be reached. */
        if (returnCode == hresult_t::SUCCESS)
        {
            t = tEnd;
        }

        return returnCode;
    }

    void EngineMultiRobot::stop(void)
    {
        // Release the lock on the robots
        for (auto & systemData : systemsDataHolder_)
        {
            systemData.robotLock.reset(nullptr);
        }

        // Make sure that a simulation running
        if (!isSimulationRunning_)
        {
            return;
        }

        // Log current buffer content as final point of the log data
        updateTelemetry();

        // Clear log data buffer one last time, now that the final point has been added
        logData_ = nullptr;

        /* Reset the telemetry.
           Note that calling ``stop` or  `reset` does NOT clear
           the internal data buffer of telemetryRecorder_.
           Clearing is done at init time, so that it remains
           accessible until the next initialization. */
        telemetryRecorder_->reset();
        telemetryData_->reset();

        // Update some internal flags
        isSimulationRunning_ = false;
    }

    hresult_t EngineMultiRobot::registerForceImpulse(std::string      const & systemName,
                                                     std::string      const & frameName,
                                                     float64_t        const & t,
                                                     float64_t        const & dt,
                                                     pinocchio::Force const & F)
    {
        // Make sure that the forces do NOT overlap while taking into account dt.

        hresult_t returnCode = hresult_t::SUCCESS;

        if (isSimulationRunning_)
        {
            PRINT_ERROR("A simulation is running. Please stop it before registering new forces.");
            returnCode = hresult_t::ERROR_GENERIC;
        }

        if (dt < STEPPER_MIN_TIMESTEP)
        {
            PRINT_ERROR("The force duration cannot be smaller than ", STEPPER_MIN_TIMESTEP, ".");
            returnCode = hresult_t::ERROR_BAD_INPUT;
        }

        if (t < 0.0)
        {
            PRINT_ERROR("The force application time must be positive.");
            returnCode = hresult_t::ERROR_BAD_INPUT;
        }

        if (frameName == "universe")
        {
            PRINT_ERROR("Impossible to apply external forces to the universe itself!");
            returnCode = hresult_t::ERROR_GENERIC;
        }

        int32_t systemIdx;
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = getSystemIdx(systemName, systemIdx);
        }

        frameIndex_t frameIdx;
        if (returnCode == hresult_t::SUCCESS)
        {
            systemHolder_t const & system = systems_[systemIdx];
            returnCode = getFrameIdx(system.robot->pncModel_, frameName, frameIdx);
        }

        if (returnCode == hresult_t::SUCCESS)
        {
            systemDataHolder_t & systemData = systemsDataHolder_[systemIdx];
            systemData.forcesImpulse.emplace_back(frameName, frameIdx, t, dt, F);
            systemData.forcesImpulseBreaks.emplace(t);
            systemData.forcesImpulseBreaks.emplace(t + dt);
            systemData.forcesImpulseActive.emplace_back(false);
        }

        return hresult_t::SUCCESS;
    }

    template<typename ...Args>
    std::tuple<bool_t, float64_t> isGcdIncluded(vector_aligned_t<systemDataHolder_t> const & systemsDataHolder,
                                                Args... values)
    {
        if (systemsDataHolder.empty())
        {
            return isGcdIncluded(std::forward<Args>(values)...);
        }

        float64_t minValue = INF;
        auto lambda = [&minValue, &values...](systemDataHolder_t const & systemData)
        {
            bool_t isIncluded; float64_t value;
            std::tie(isIncluded, value) = isGcdIncluded(
                systemData.forcesProfile.begin(),
                systemData.forcesProfile.end(),
                [](forceProfile_t const & force) { return force.updatePeriod; },
                std::forward<Args>(values)...);
            minValue = minClipped(minValue, value);
            return isIncluded;
        };
        return {std::all_of(systemsDataHolder.begin(), systemsDataHolder.end(), lambda), minValue};
    }

    hresult_t EngineMultiRobot::registerForceProfile(std::string const & systemName,
                                                     std::string const & frameName,
                                                     forceProfileFunctor_t const & forceFct,
                                                     float64_t const & updatePeriod)
    {
        hresult_t returnCode = hresult_t::SUCCESS;

        if (isSimulationRunning_)
        {
            PRINT_ERROR("A simulation is running. Please stop it before registering new forces.");
            returnCode = hresult_t::ERROR_GENERIC;
        }

        int32_t systemIdx;
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = getSystemIdx(systemName, systemIdx);
        }

        if (frameName == "universe")
        {
            PRINT_ERROR("Impossible to apply external forces to the universe itself!");
            returnCode = hresult_t::ERROR_GENERIC;
        }

        frameIndex_t frameIdx;
        if (returnCode == hresult_t::SUCCESS)
        {
            systemHolder_t const & system = systems_[systemIdx];
            returnCode = getFrameIdx(system.robot->pncModel_, frameName, frameIdx);
        }

        // Make sure the update period is valid
        if (returnCode == hresult_t::SUCCESS)
        {
            if (EPS < updatePeriod && updatePeriod < SIMULATION_MIN_TIMESTEP)
            {
                PRINT_ERROR("Cannot regsiter external force profile with update period smaller than ",
                            SIMULATION_MIN_TIMESTEP, "s. Adjust period or switch to continuous mode "
                            "by setting period to zero.");
                returnCode = hresult_t::ERROR_BAD_INPUT;
            }
        }

        // Make sure the desired update period is a multiple of the stepper period
        bool_t isIncluded; float64_t minUpdatePeriod;
        std::tie(isIncluded, minUpdatePeriod) = isGcdIncluded(
            systemsDataHolder_, stepperUpdatePeriod_, updatePeriod);
        if (returnCode == hresult_t::SUCCESS)
        {
            if (!isIncluded)
            {
                PRINT_ERROR("In discrete mode, the update period of force profiles and the stepper "
                            "update period (min of controller and sensor update periods) must be "
                            "multiple of each other.");
                returnCode = hresult_t::ERROR_BAD_INPUT;
            }
        }

        if (returnCode == hresult_t::SUCCESS)
        {
            // Set breakpoint period during the integration loop
            stepperUpdatePeriod_ = minUpdatePeriod;

            // Add force profile to register
            systemDataHolder_t & systemData = systemsDataHolder_[systemIdx];
            systemData.forcesProfile.emplace_back(
                frameName, frameIdx, updatePeriod, forceFct);
        }

        return returnCode;
    }

    hresult_t EngineMultiRobot::removeForcesImpulse(std::string const & systemName)
    {
        hresult_t returnCode = hresult_t::SUCCESS;

        // Make sure that no simulation is running
        if (isSimulationRunning_)
        {
            PRINT_ERROR("A simulation is already running. Stop it before removing coupling forces.");
            returnCode = hresult_t::ERROR_GENERIC;
        }

        int32_t systemIdx;
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = getSystemIdx(systemName, systemIdx);
        }

        if (returnCode == hresult_t::SUCCESS)
        {
            systemDataHolder_t & systemData = systemsDataHolder_[systemIdx];
            systemData.forcesImpulse.clear();
        }

        return hresult_t::SUCCESS;
    }

    hresult_t EngineMultiRobot::removeForcesImpulse(void)
    {
        // Make sure that no simulation is running
        if (isSimulationRunning_)
        {
            PRINT_ERROR("A simulation is already running. Stop it before removing coupling forces.");
            return hresult_t::ERROR_GENERIC;
        }

        for (auto & systemData : systemsDataHolder_)
        {
            systemData.forcesImpulse.clear();
        }

        return hresult_t::SUCCESS;
    }

    hresult_t EngineMultiRobot::removeForcesProfile(std::string const & systemName)
    {
        hresult_t returnCode = hresult_t::SUCCESS;

        // Make sure that no simulation is running
        if (isSimulationRunning_)
        {
            PRINT_ERROR("A simulation is already running. Stop it before removing coupling forces.");
            return hresult_t::ERROR_GENERIC;
        }

        int32_t systemIdx;
        if (returnCode == hresult_t::SUCCESS)
        {
            returnCode = getSystemIdx(systemName, systemIdx);
        }

        if (returnCode == hresult_t::SUCCESS)
        {
            // Remove force profile from register
            systemDataHolder_t & systemData = systemsDataHolder_[systemIdx];
            systemData.forcesProfile.clear();

            // Set breakpoint period during the integration loop
            stepperUpdatePeriod_ = std::get<1>(isGcdIncluded(
                systemsDataHolder_,
                engineOptions_->stepper.sensorsUpdatePeriod,
                engineOptions_->stepper.controllerUpdatePeriod));
        }

        return hresult_t::SUCCESS;
    }

    hresult_t EngineMultiRobot::removeForcesProfile(void)
    {
        // Make sure that no simulation is running
        if (isSimulationRunning_)
        {
            PRINT_ERROR("A simulation is already running. Stop it before removing coupling forces.");
            return hresult_t::ERROR_GENERIC;
        }

        for (auto & systemData : systemsDataHolder_)
        {
            systemData.forcesProfile.clear();
        }

        return hresult_t::SUCCESS;
    }

    hresult_t EngineMultiRobot::getForcesImpulse(std::string const & systemName,
                                                 forceImpulseRegister_t const * & forcesImpulsePtr) const
    {
        static forceImpulseRegister_t forcesImpuseDummy;

        hresult_t returnCode = hresult_t::SUCCESS;

        forcesImpulsePtr = &forcesImpuseDummy;

        int32_t systemIdx;
        returnCode = getSystemIdx(systemName, systemIdx);

        if (returnCode == hresult_t::SUCCESS)
        {
            systemDataHolder_t const & systemData = systemsDataHolder_[systemIdx];
            forcesImpulsePtr = &systemData.forcesImpulse;
        }

        return returnCode;
    }

    hresult_t EngineMultiRobot::getForcesProfile(std::string const & systemName,
                                                 forceProfileRegister_t const * & forcesProfilePtr) const
    {
        static forceProfileRegister_t forcesRegisterDummy;

        hresult_t returnCode = hresult_t::SUCCESS;

        forcesProfilePtr = &forcesRegisterDummy;

        int32_t systemIdx;
        returnCode = getSystemIdx(systemName, systemIdx);

        if (returnCode == hresult_t::SUCCESS)
        {
            systemDataHolder_t const & systemData = systemsDataHolder_[systemIdx];
            forcesProfilePtr = &systemData.forcesProfile;
        }

        return returnCode;
    }

    configHolder_t EngineMultiRobot::getOptions(void) const
    {
        return engineOptionsHolder_;
    }

    hresult_t EngineMultiRobot::setOptions(configHolder_t const & engineOptions)
    {
        if (isSimulationRunning_)
        {
            PRINT_ERROR("A simulation is running. Please stop it before updating the options.");
            return hresult_t::ERROR_GENERIC;
        }

        // Make sure the dtMax is not out of range
        configHolder_t stepperOptions = boost::get<configHolder_t>(engineOptions.at("stepper"));
        float64_t const & dtMax = boost::get<float64_t>(stepperOptions.at("dtMax"));
        if (SIMULATION_MAX_TIMESTEP + EPS < dtMax || dtMax < SIMULATION_MIN_TIMESTEP)
        {
            PRINT_ERROR("'dtMax' option is out of range.");
            return hresult_t::ERROR_BAD_INPUT;
        }

        // Make sure successiveIterFailedMax is strictly positive
        uint32_t const & successiveIterFailedMax = boost::get<uint32_t>(stepperOptions.at("successiveIterFailedMax"));
        if (successiveIterFailedMax < 1)
        {
            PRINT_ERROR("'successiveIterFailedMax' must be strictly positive.");
            return hresult_t::ERROR_BAD_INPUT;
        }

        // Make sure the selected ode solver is available and instantiate it
        std::string const & odeSolver = boost::get<std::string>(stepperOptions.at("odeSolver"));
        if (STEPPERS.find(odeSolver) == STEPPERS.end())
        {
            PRINT_ERROR("The requested ODE solver is not available.");
            return hresult_t::ERROR_BAD_INPUT;
        }

        // Make sure the controller and sensor update periods are valid
        float64_t const & sensorsUpdatePeriod =
            boost::get<float64_t>(stepperOptions.at("sensorsUpdatePeriod"));
        float64_t const & controllerUpdatePeriod =
            boost::get<float64_t>(stepperOptions.at("controllerUpdatePeriod"));
        bool_t isIncluded; float64_t minUpdatePeriod;
        std::tie(isIncluded, minUpdatePeriod) = isGcdIncluded(
            systemsDataHolder_, controllerUpdatePeriod, sensorsUpdatePeriod);
        if ((EPS < sensorsUpdatePeriod && sensorsUpdatePeriod < SIMULATION_MIN_TIMESTEP)
        || (EPS < controllerUpdatePeriod && controllerUpdatePeriod < SIMULATION_MIN_TIMESTEP))
        {
            PRINT_ERROR("Cannot simulate a discrete system with update period smaller than ",
                        SIMULATION_MIN_TIMESTEP, "s. Adjust period or switch to continuous mode "
                        "by setting period to zero.");
            return hresult_t::ERROR_BAD_INPUT;
        }
        else if (!isIncluded)
        {
            PRINT_ERROR("In discrete mode, the controller and sensor update periods must be "
                        "multiple of each other.");
            return hresult_t::ERROR_BAD_INPUT;
        }

        // Make sure the contacts options are fine
        configHolder_t constraintsOptions = boost::get<configHolder_t>(engineOptions.at("constraints"));
        std::string const & constraintSolverType = boost::get<std::string>(constraintsOptions.at("solver"));
        auto const constraintSolverIt = CONSTRAINT_SOLVERS_MAP.find(constraintSolverType);
        if (constraintSolverIt == CONSTRAINT_SOLVERS_MAP.end())
        {
            PRINT_ERROR("The requested constraint solver is not available.");
            return hresult_t::ERROR_BAD_INPUT;
        }
        float64_t const & regularization =
            boost::get<float64_t>(constraintsOptions.at("regularization"));
        if (regularization < 0.0)
        {
            PRINT_ERROR("The constraints option 'regularization' must be positive.");
            return hresult_t::ERROR_BAD_INPUT;
        }

        // Make sure the contacts options are fine
        configHolder_t contactsOptions = boost::get<configHolder_t>(engineOptions.at("contacts"));
        std::string const & contactModel = boost::get<std::string>(contactsOptions.at("model"));
        auto const contactModelIt = CONTACT_MODELS_MAP.find(contactModel);
        if (contactModelIt == CONTACT_MODELS_MAP.end())
        {
            PRINT_ERROR("The requested contact model is not available.");
            return hresult_t::ERROR_BAD_INPUT;
        }
        float64_t const & contactsTransitionEps =
            boost::get<float64_t>(contactsOptions.at("transitionEps"));
        if (contactsTransitionEps < 0.0)
        {
            PRINT_ERROR("The contacts option 'transitionEps' must be positive.");
            return hresult_t::ERROR_BAD_INPUT;
        }
        float64_t const & transitionVelocity =
            boost::get<float64_t>(contactsOptions.at("transitionVelocity"));
        if (transitionVelocity < EPS)
        {
            PRINT_ERROR("The contacts option 'transitionVelocity' must be strictly positive.");
            return hresult_t::ERROR_BAD_INPUT;
        }
        float64_t const & stabilizationFreq =
            boost::get<float64_t>(contactsOptions.at("stabilizationFreq"));
        if (stabilizationFreq < 0.0)
        {
            PRINT_ERROR("The contacts option 'stabilizationFreq' must be positive.");
            return hresult_t::ERROR_BAD_INPUT;
        }

        // Make sure the user-defined gravity force has the right dimension
        configHolder_t worldOptions = boost::get<configHolder_t>(engineOptions.at("world"));
        vectorN_t gravity = boost::get<vectorN_t>(worldOptions.at("gravity"));
        if (gravity.size() != 6)
        {
            PRINT_ERROR("The size of the gravity force vector must be 6.");
            return hresult_t::ERROR_BAD_INPUT;
        }

        /* Reset random number generators if setOptions is called for the first time,
           or if the desired random seed has changed. */
        uint32_t randomSeed = boost::get<uint32_t>(stepperOptions.at("randomSeed"));
        if (!engineOptions_ || randomSeed != engineOptions_->stepper.randomSeed)
        {
            resetRandomGenerators(randomSeed);
        }

        // Update the internal options
        engineOptionsHolder_ = engineOptions;

        // Create a fast struct accessor
        engineOptions_ = std::make_unique<engineOptions_t const>(engineOptionsHolder_);

        // Backup contact model as enum for fast check
        contactModel_ = contactModelIt->second;

        // Set breakpoint period during the integration loop
        stepperUpdatePeriod_ = minUpdatePeriod;

        return hresult_t::SUCCESS;
    }

    std::vector<std::string> EngineMultiRobot::getSystemsNames(void) const
    {
        std::vector<std::string> systemsNames;
        systemsNames.reserve(systems_.size());
        std::transform(systems_.begin(), systems_.end(),
                       std::back_inserter(systemsNames),
                       [](auto const & sys) -> std::string
                       {
                           return sys.name;
                       });
        return systemsNames;
    }

    hresult_t EngineMultiRobot::getSystemIdx(std::string const & systemName,
                                             int32_t           & systemIdx) const
    {
        auto systemIt = std::find_if(systems_.begin(), systems_.end(),
                                     [&systemName](auto const & sys)
                                     {
                                         return (sys.name == systemName);
                                     });
        if (systemIt == systems_.end())
        {
            PRINT_ERROR("No system with this name has been added to the engine.");
            return hresult_t::ERROR_BAD_INPUT;
        }

        systemIdx = static_cast<int32_t>(std::distance(systems_.begin(), systemIt));

        return hresult_t::SUCCESS;
    }

    hresult_t EngineMultiRobot::getSystem(std::string const & systemName,
                                          systemHolder_t * & system)
    {
        static systemHolder_t systemEmpty;

        hresult_t returnCode = hresult_t::SUCCESS;

        auto systemIt = std::find_if(systems_.begin(), systems_.end(),
                                     [&systemName](auto const & sys)
                                     {
                                         return (sys.name == systemName);
                                     });
        if (systemIt == systems_.end())
        {
            PRINT_ERROR("No system with this name has been added to the engine.");
            returnCode = hresult_t::ERROR_BAD_INPUT;
        }

        if (returnCode == hresult_t::SUCCESS)
        {
            system = &(*systemIt);
            return returnCode;
        }

        system = &systemEmpty;

        return returnCode;
    }

    hresult_t EngineMultiRobot::getSystemState(std::string   const   & systemName,
                                               systemState_t const * & systemState) const
    {
        static systemState_t const systemStateEmpty;

        hresult_t returnCode = hresult_t::SUCCESS;

        int32_t systemIdx;
        returnCode = getSystemIdx(systemName, systemIdx);
        if (returnCode == hresult_t::SUCCESS)
        {
            systemState = &(systemsDataHolder_[systemIdx].state);
            return returnCode;
        }

        systemState = &systemStateEmpty;
        return returnCode;
    }

    stepperState_t const & EngineMultiRobot::getStepperState(void) const
    {
        return stepperState_;
    }

    bool_t const & EngineMultiRobot::getIsSimulationRunning(void) const
    {
        return isSimulationRunning_;
    }

    float64_t EngineMultiRobot::getMaxSimulationDuration(void)
    {
        return TelemetryRecorder::getMaximumLogTime(getTelemetryTimeUnit());
    }

    float64_t EngineMultiRobot::getTelemetryTimeUnit(void)
    {
        return STEPPER_MIN_TIMESTEP;
    }

    // ========================================================
    // =================== Stepper utilities ==================
    // ========================================================

    void EngineMultiRobot::syncStepperStateWithSystems(void)
    {
        auto qSplitIt = stepperState_.qSplit.begin();
        auto vSplitIt = stepperState_.vSplit.begin();
        auto aSplitIt = stepperState_.aSplit.begin();
        auto systemDataIt = systemsDataHolder_.begin();
        for ( ; systemDataIt != systemsDataHolder_.end();
             ++systemDataIt, ++qSplitIt, ++vSplitIt, ++aSplitIt)
        {
            *qSplitIt = systemDataIt->state.q;
            *vSplitIt = systemDataIt->state.v;
            *aSplitIt = systemDataIt->state.a;
        }
    }

    void EngineMultiRobot::syncSystemsStateWithStepper(bool_t const & sync_acceleration_only)
    {
        if (sync_acceleration_only)
        {
            auto aSplitIt = stepperState_.aSplit.begin();
            auto systemDataIt = systemsDataHolder_.begin();
            for ( ; systemDataIt != systemsDataHolder_.end();
                ++systemDataIt, ++aSplitIt)
            {
                systemDataIt->state.a = *aSplitIt;
            }
        }
        else
        {
            auto qSplitIt = stepperState_.qSplit.begin();
            auto vSplitIt = stepperState_.vSplit.begin();
            auto aSplitIt = stepperState_.aSplit.begin();
            auto systemDataIt = systemsDataHolder_.begin();
            for ( ; systemDataIt != systemsDataHolder_.end();
                ++systemDataIt, ++qSplitIt, ++vSplitIt, ++aSplitIt)
            {
                systemDataIt->state.q = *qSplitIt;
                systemDataIt->state.v = *vSplitIt;
                systemDataIt->state.a = *aSplitIt;
            }
        }
    }

    // ========================================================
    // ================ Core physics utilities ================
    // ========================================================


    void EngineMultiRobot::computeForwardKinematics(systemHolder_t       & system,
                                                    vectorN_t      const & q,
                                                    vectorN_t      const & v,
                                                    vectorN_t      const & a)
    {
        // Create proxies for convenience
        pinocchio::Model const & model = system.robot->pncModel_;
        pinocchio::Data & data = system.robot->pncData_;
        pinocchio::GeometryModel const & geomModel = system.robot->collisionModel_;
        pinocchio::GeometryData & geomData = system.robot->collisionData_;

        // Update forward kinematics
        pinocchio::forwardKinematics(model, data, q, v, a);

        // Update frame placements (avoiding redundant computations)
        for (int32_t i=1; i < model.nframes; ++i)
        {
            pinocchio::Frame const & frame = model.frames[i];
            jointIndex_t const & parent = frame.parent;
            switch (frame.type)
            {
            case pinocchio::FrameType::JOINT:
                /* If the frame is associated with an actual joint, no need to compute
                   anything new, since the frame transform is supposed to be identity. */
                data.oMf[i] = data.oMi[parent];
                break;
            case pinocchio::FrameType::BODY:
                if (model.frames[frame.previousFrame].type == pinocchio::FrameType::FIXED_JOINT)
                {
                    /* BODYs connected via FIXED_JOINT(s) have the same transform than the
                    joint itself, so no need to compute them twice.
                    Here we are doing the assumption that the previous frame transfrom has
                    already been updated since it is closer to root in kinematic tree. */
                    data.oMf[i] = data.oMf[frame.previousFrame];
                }
                else
                {
                    /* BODYs connected via JOINT(s) have the identity transform, so copying
                    parent joint transform should be fine. */
                    data.oMf[i] = data.oMi[parent];
                }
                break;
            case pinocchio::FrameType::FIXED_JOINT:
            case pinocchio::FrameType::SENSOR:
            case pinocchio::FrameType::OP_FRAME:
            default:
                // Nothing special, doing the actual computation
                data.oMf[i] = data.oMi[parent] * frame.placement;
            }
        }

        /* Update collision information selectively,
           ie only for geometries involved in at least one collision pair. */
        std::unordered_set<geomIndex_t> activeGeometriesIdx;
        for (auto const & pair : geomModel.collisionPairs)
        {
            activeGeometriesIdx.insert(pair.first);
            activeGeometriesIdx.insert(pair.second);
        }
        for (geomIndex_t const & i : activeGeometriesIdx)
        {
            jointIndex_t const & jointIdx = geomModel.geometryObjects[i].parentJoint;
            if (jointIdx > 0)
            {
                geomData.oMg[i] = data.oMi[jointIdx] * geomModel.geometryObjects[i].placement;
            }
            else
            {
                geomData.oMg[i] = geomModel.geometryObjects[i].placement;
            }
        }
        pinocchio::computeCollisions(geomModel, geomData, false);
    }

    void EngineMultiRobot::computeContactDynamicsAtBody(systemHolder_t const & system,
                                                        pairIndex_t const & collisionPairIdx,
                                                        std::shared_ptr<AbstractConstraintBase> & constraint,
                                                        pinocchio::Force & fextLocal) const
    {
        // TODO: It is assumed that the ground is flat. For now ground profile is not supported
        // with body collision. Nevertheless it should not be to hard to generated a collision
        // object simply by sampling points on the profile.

        // Get the frame and joint indices
        geomIndex_t const & geometryIdx = system.robot->collisionModel_.collisionPairs[collisionPairIdx].first;
        jointIndex_t const & parentJointIdx = system.robot->collisionModel_.geometryObjects[geometryIdx].parentJoint;

        // Extract collision and distance results
        hpp::fcl::CollisionResult const & collisionResult = system.robot->collisionData_.collisionResults[collisionPairIdx];

        fextLocal.setZero();

        // There is no way to get access to the distance from the ground at this point,
        // so it is not possible to disable the constraint only if depth > transitionEps.
        if (constraint)
        {
            constraint->disable();
        }

        for (uint32_t i = 0; i < collisionResult.numContacts(); ++i)
        {
            /* Extract the contact information.
               Note that there is always a single contact point while computing the collision
               between two shape objects, for instance convex geometry and box primitive. */
            auto const & contact = collisionResult.getContact(i);
            vector3_t nGround = contact.normal.normalized();        // Normal of the ground in world
            float64_t depth = contact.penetration_depth;          // Penetration depth (signed, so always negative)
            pinocchio::SE3 posContactInWorld = pinocchio::SE3::Identity();
            posContactInWorld.translation() = contact.pos;                  //  Point inside the ground #TODO double check that, it may be between both interfaces

            /* Make sure the collision computation didn't failed. If it happends the
               norm of the distance normal is not normalized (usually close to zero).
               If so, just assume there is no collision at all. */
            if (nGround.norm() < 1.0 - EPS)
            {
                continue;
            }

            // Make sure the normal is always pointing upward, and the penetration depth is negative
            if (nGround[2] < 0.0)
            {
                nGround *= -1.0;
            }
            if (depth > 0.0)
            {
                depth *= -1.0;
            }

            if (contactModel_ == contactModel_t::SPRING_DAMPER)
            {
                // Compute the linear velocity of the contact point in world frame
                pinocchio::Motion const & motionJointLocal = system.robot->pncData_.v[parentJointIdx];
                pinocchio::SE3 const & transformJointFrameInWorld = system.robot->pncData_.oMi[parentJointIdx];
                pinocchio::SE3 const transformJointFrameInContact = posContactInWorld.actInv(transformJointFrameInWorld);
                vector3_t const vContactInWorld = transformJointFrameInContact.act(motionJointLocal).linear();

                // Compute the ground reaction force at contact point in world frame
                pinocchio::Force const fextAtContactInGlobal = computeContactDynamics(
                    nGround, depth, vContactInWorld);

                // Move the force at parent frame location
                fextLocal += transformJointFrameInContact.actInv(fextAtContactInGlobal);
            }
            else
            {
                // Some geometry shapes are not supported for now, if so the constraint is not initialized
                if (constraint)
                {
                    // In case of slippage the contact point has actually moved and must be updated
                    constraint->enable();
                    auto & frameConstraint = static_cast<FixedFrameConstraint &>(*constraint.get());
                    frameIndex_t const & frameIdx = frameConstraint.getFrameIdx();
                    frameConstraint.setReferenceTransform({
                        system.robot->pncData_.oMf[frameIdx].rotation(),
                        system.robot->pncData_.oMf[frameIdx].translation() - depth * nGround
                    });
                    frameConstraint.setNormal(nGround);

                    // Only one contact constraint per collision body is supported for now
                    break;
                }
            }
        }
    }

    void EngineMultiRobot::computeContactDynamicsAtFrame(systemHolder_t const & system,
                                                         frameIndex_t const & frameIdx,
                                                         std::shared_ptr<AbstractConstraintBase> & constraint,
                                                         pinocchio::Force & fextLocal) const
    {
        /* Returns the external force in the contact frame.
           It must then be converted into a force onto the parent joint.
           /!\ Note that the contact dynamics depends only on kinematics data. /!\ */

        // Define proxies for convenience
        pinocchio::Model const & model = system.robot->pncModel_;
        pinocchio::Data const & data = system.robot->pncData_;

        // Get the pose of the frame wrt the world
        pinocchio::SE3 const & transformFrameInWorld = data.oMf[frameIdx];

        // Compute the ground normal and penetration depth at the contact point
        vector3_t const & posFrame = transformFrameInWorld.translation();
        auto ground = engineOptions_->world.groundProfile(posFrame);
        float64_t const & zGround = std::get<float64_t>(ground);
        vector3_t & nGround = std::get<vector3_t>(ground);
        nGround.normalize();  // Make sure the ground normal is normalized
        float64_t const depth = (posFrame[2] - zGround) * nGround[2];  // First-order projection (exact assuming no curvature)

        // Only compute the ground reaction force if the penetration depth is negative
        if (depth < 0.0)
        {
            // Apply the force at the origin of the parent joint frame
            if (contactModel_ == contactModel_t::SPRING_DAMPER)
            {
                // Compute the linear velocity of the contact point in world frame.
                vector3_t const motionFrameLocal = pinocchio::getFrameVelocity(
                    model, data, frameIdx).linear();
                matrix3_t const & rotFrame = transformFrameInWorld.rotation();
                vector3_t const vContactInWorld = rotFrame * motionFrameLocal;

                // Compute the ground reaction force in world frame (local world aligned)
                pinocchio::Force const fextAtContactInGlobal = computeContactDynamics(
                    nGround, depth, vContactInWorld);

                // Deduce the ground reaction force in joint frame
                fextLocal = convertForceGlobalFrameToJoint(
                    model, data, frameIdx, fextAtContactInGlobal);
            }
            else
            {
                // Enable fixed frame constraint
                constraint->enable();
            }
        }
        else
        {
            if (contactModel_ == contactModel_t::SPRING_DAMPER)
            {
                // Not in contact with the ground, thus no force applied
                fextLocal.setZero();
            }
            else if (depth > engineOptions_->contacts.transitionEps)
            {
                /* Disable fixed frame constraint only if the frame move higher
                   `transitionEps` to avoid sporadic contact detection. */
                constraint->disable();
            }
        }

        /* Move the reference position at the surface of the ground.
           Note that it is must done systematically as long as the constraint is enabled
           because in case of slippage the contact point has actually moved. */
        if (constraint->getIsEnabled())
        {
            auto & frameConstraint = static_cast<FixedFrameConstraint &>(*constraint.get());
            frameConstraint.setReferenceTransform({
                transformFrameInWorld.rotation(),
                posFrame - depth * nGround});
            frameConstraint.setNormal(nGround);
        }
    }

    pinocchio::Force EngineMultiRobot::computeContactDynamics(vector3_t const & nGround,
                                                              float64_t const & depth,
                                                              vector3_t const & vContactInWorld) const
    {
        // Initialize the contact force
        vector3_t fextInWorld;

        if (depth < 0.0)
        {
            // Extract some proxies
            contactOptions_t const & contactOptions_ = engineOptions_->contacts;

            // Compute the penetration speed
            float64_t const vDepth = vContactInWorld.dot(nGround);

            // Compute normal force
            float64_t const fextNormal = - std::min(contactOptions_.stiffness * depth +
                                                    contactOptions_.damping * vDepth, 0.0);
            fextInWorld.noalias() = fextNormal * nGround;

            // Compute friction forces
            vector3_t const vTangential = vContactInWorld - vDepth * nGround;
            float64_t const vRatio = std::min(vTangential.norm() / contactOptions_.transitionVelocity, 1.0);
            float64_t const fextTangential = contactOptions_.friction * vRatio * fextNormal;
            fextInWorld.noalias() -= fextTangential * vTangential;

            // Add blending factor
            if (contactOptions_.transitionEps > EPS)
            {
                float64_t const blendingFactor = - depth / contactOptions_.transitionEps;
                float64_t const blendingLaw = std::tanh(2.0 * blendingFactor);
                fextInWorld *= blendingLaw;
            }
        }
        else
        {
            fextInWorld.setZero();
        }

        return {fextInWorld, vector3_t::Zero()};
    }

    void EngineMultiRobot::computeCommand(systemHolder_t       & system,
                                          float64_t      const & t,
                                          vectorN_t      const & q,
                                          vectorN_t      const & v,
                                          vectorN_t            & command)
    {
        // Reinitialize the external forces
        command.setZero();

        // Command the command
        system.controller->computeCommand(t, q, v, command);
    }

    template<template<typename, int, int> class JointModel, typename Scalar, int Options, int axis>
    static std::enable_if_t<is_pinocchio_joint_revolute_v<JointModel<Scalar, Options, axis> >
                         || is_pinocchio_joint_revolute_unbounded_v<JointModel<Scalar, Options, axis> >, float64_t>
    getSubtreeInertiaProj(JointModel<Scalar, Options, axis> const & /* model */,
                          pinocchio::Inertia                const & Isubtree)
    {
        return Isubtree.inertia()(axis, axis);
    }

    template<typename JointModel>
    static std::enable_if_t<is_pinocchio_joint_revolute_unaligned_v<JointModel>
                         || is_pinocchio_joint_revolute_unbounded_unaligned_v<JointModel>, float64_t>
    getSubtreeInertiaProj(JointModel const & model,
                          pinocchio::Inertia const & Isubtree)
    {
        return model.axis.dot(Isubtree.inertia() * model.axis);
    }

    template<typename JointModel>
    static std::enable_if_t<is_pinocchio_joint_prismatic_v<JointModel>
                         || is_pinocchio_joint_prismatic_unaligned_v<JointModel>, float64_t>
    getSubtreeInertiaProj(JointModel const & /* model */,
                          pinocchio::Inertia const & Isubtree)
    {
        return Isubtree.mass();
    }

    struct computePositionLimitsForcesAlgo
    : public pinocchio::fusion::JointUnaryVisitorBase<computePositionLimitsForcesAlgo>
    {
        typedef boost::fusion::vector<pinocchio::Data const & /* pncData */,
                                      vectorN_t const & /* q */,
                                      vectorN_t const & /* v */,
                                      vectorN_t const & /* positionLimitMin */,
                                      vectorN_t const & /* positionLimitMax */,
                                      std::unique_ptr<EngineMultiRobot::engineOptions_t const> const & /* engineOptions */,
                                      contactModel_t const & /* contactModel */,
                                      std::shared_ptr<AbstractConstraintBase> & /* constraint */,
                                      vectorN_t & /* u */> ArgsType;

        template<typename JointModel>
        static std::enable_if_t<is_pinocchio_joint_revolute_v<JointModel>
                             || is_pinocchio_joint_revolute_unaligned_v<JointModel>
                             || is_pinocchio_joint_prismatic_v<JointModel>
                             || is_pinocchio_joint_prismatic_unaligned_v<JointModel>, void>
        algo(pinocchio::JointModelBase<JointModel> const & joint,
             pinocchio::Data const & pncData,
             vectorN_t const & q,
             vectorN_t const & v,
             vectorN_t const & positionLimitMin,
             vectorN_t const & positionLimitMax,
             std::unique_ptr<EngineMultiRobot::engineOptions_t const> const & engineOptions,
             contactModel_t const & contactModel,
             std::shared_ptr<AbstractConstraintBase> & constraint,
             vectorN_t & u)
        {
            // Define some proxies for convenience
            jointIndex_t const & jointIdx = joint.id();
            uint32_t const & positionIdx = joint.idx_q();
            uint32_t const & velocityIdx = joint.idx_v();
            float64_t const & qJoint = q[positionIdx];
            float64_t const & qJointMin = positionLimitMin[positionIdx];
            float64_t const & qJointMax = positionLimitMax[positionIdx];
            float64_t const & vJoint = v[velocityIdx];
            float64_t const & Ia = getSubtreeInertiaProj(
                joint.derived(), pncData.Ycrb[jointIdx]);
            float64_t const & stiffness = engineOptions->joints.boundStiffness;
            float64_t const & damping = engineOptions->joints.boundDamping;
            float64_t const & transitionEps = engineOptions->contacts.transitionEps;

            // Check if out-of-bounds
            if (contactModel == contactModel_t::SPRING_DAMPER)
            {
                // Compute the acceleration to apply to move out of the bound
                float64_t accelJoint = 0.0;
                if (qJoint > qJointMax)
                {
                    float64_t const qJointError = qJoint - qJointMax;
                    accelJoint = - std::max(stiffness * qJointError + damping * vJoint, 0.0);
                }
                else if (qJoint < qJointMin)
                {
                    float64_t const qJointError = qJoint - qJointMin;
                    accelJoint = - std::min(stiffness * qJointError + damping * vJoint, 0.0);
                }

                // Apply the resulting force
                u[velocityIdx] += Ia * accelJoint;
            }
            else
            {
                if (qJointMax < qJoint || qJoint < qJointMin)
                {
                    // Enable fixed joint constraint and reset it if it was disable
                    constraint->enable();
                    auto & jointConstraint = static_cast<JointConstraint &>(*constraint.get());
                    jointConstraint.setReferenceConfiguration(
                        Eigen::Matrix<float64_t, 1, 1>(std::clamp(qJoint, qJointMin, qJointMax)));
                    jointConstraint.setRotationDir(qJointMax < qJoint);
                }
                else if (qJointMin + transitionEps < qJoint && qJoint < qJointMax - transitionEps)
                {
                    // Disable fixed joint constraint
                    constraint->disable();
                }
            }
        }

        template<typename JointModel>
        static std::enable_if_t<is_pinocchio_joint_revolute_unbounded_v<JointModel>
                             || is_pinocchio_joint_revolute_unbounded_unaligned_v<JointModel>, void>
        algo(pinocchio::JointModelBase<JointModel> const & /* joint */,
             pinocchio::Data const & /* pncData */,
             vectorN_t const & /* q */,
             vectorN_t const & /* v */,
             vectorN_t const & /* positionLimitMin */,
             vectorN_t const & /* positionLimitMax */,
             std::unique_ptr<EngineMultiRobot::engineOptions_t const> const & /* engineOptions */,
             contactModel_t const & contactModel,
             std::shared_ptr<AbstractConstraintBase> & constraint,
             vectorN_t & /* u */)
        {
            if (contactModel == contactModel_t::CONSTRAINT)
            {
                // Disable fixed joint constraint
                constraint->disable();
            }
        }

        template<typename JointModel>
        static std::enable_if_t<is_pinocchio_joint_freeflyer_v<JointModel>
                             || is_pinocchio_joint_spherical_v<JointModel>
                             || is_pinocchio_joint_spherical_zyx_v<JointModel>
                             || is_pinocchio_joint_translation_v<JointModel>
                             || is_pinocchio_joint_planar_v<JointModel>
                             || is_pinocchio_joint_mimic_v<JointModel>
                             || is_pinocchio_joint_composite_v<JointModel>, void>
        algo(pinocchio::JointModelBase<JointModel> const & /* joint */,
             pinocchio::Data const & /* pncData */,
             vectorN_t const & /* q */,
             vectorN_t const & /* v */,
             vectorN_t const & /* positionLimitMin */,
             vectorN_t const & /* positionLimitMax */,
             std::unique_ptr<EngineMultiRobot::engineOptions_t const> const & /* engineOptions */,
             contactModel_t const & contactModel,
             std::shared_ptr<AbstractConstraintBase> & constraint,
             vectorN_t & /* u */)
        {
            PRINT_WARNING("No position bounds implemented for this type of joint.");
            if (contactModel == contactModel_t::CONSTRAINT)
            {
                // Disable fixed joint constraint
                constraint->disable();
            }
        }
    };

    struct computeVelocityLimitsForcesAlgo
    : public pinocchio::fusion::JointUnaryVisitorBase<computeVelocityLimitsForcesAlgo>
    {
        typedef boost::fusion::vector<pinocchio::Data const & /* pncData */,
                                      vectorN_t const & /* v */,
                                      vectorN_t const & /* velocityLimitMax */,
                                      std::unique_ptr<EngineMultiRobot::engineOptions_t const> const & /* engineOptions */,
                                      contactModel_t const & /* contactModel */,
                                      vectorN_t & /* u */> ArgsType;
        template<typename JointModel>
        static std::enable_if_t<is_pinocchio_joint_revolute_v<JointModel>
                             || is_pinocchio_joint_revolute_unaligned_v<JointModel>
                             || is_pinocchio_joint_revolute_unbounded_v<JointModel>
                             || is_pinocchio_joint_revolute_unbounded_unaligned_v<JointModel>
                             || is_pinocchio_joint_prismatic_v<JointModel>
                             || is_pinocchio_joint_prismatic_unaligned_v<JointModel>, void>
        algo(pinocchio::JointModelBase<JointModel> const & joint,
             pinocchio::Data const & pncData,
             vectorN_t const & v,
             vectorN_t const & velocityLimitMax,
             std::unique_ptr<EngineMultiRobot::engineOptions_t const> const & engineOptions,
             contactModel_t const & contactModel,
             vectorN_t & u)
        {
            // Define some proxies for convenience
            jointIndex_t const & jointIdx = joint.id();
            uint32_t const & velocityIdx = joint.idx_v();
            float64_t const & vJoint = v[velocityIdx];
            float64_t const & vJointMin = -velocityLimitMax[velocityIdx];
            float64_t const & vJointMax = velocityLimitMax[velocityIdx];
            float64_t const & Ia = getSubtreeInertiaProj(
                joint.derived(), pncData.Ycrb[jointIdx]);
            float64_t const & damping = engineOptions->joints.boundDamping;

            // Check if out-of-bounds
            if (contactModel == contactModel_t::SPRING_DAMPER)
            {
                // Compute joint velocity error
                float64_t vJointError = 0.0;
                if (vJoint > vJointMax)
                {
                    vJointError = vJoint - vJointMax;
                }
                else if (vJoint < vJointMin)
                {
                    vJointError = vJoint - vJointMin;
                }
                else
                {
                    return;
                }

                // Generate acceleration in the opposite direction if out-of-bounds
                float64_t const accelJoint = - 2.0 * damping * vJointError;

                // Apply the resulting force
                u[velocityIdx] += Ia * accelJoint;
            }
        }

        template<typename JointModel>
        static std::enable_if_t<is_pinocchio_joint_freeflyer_v<JointModel>
                             || is_pinocchio_joint_spherical_v<JointModel>
                             || is_pinocchio_joint_spherical_zyx_v<JointModel>
                             || is_pinocchio_joint_translation_v<JointModel>
                             || is_pinocchio_joint_planar_v<JointModel>
                             || is_pinocchio_joint_mimic_v<JointModel>
                             || is_pinocchio_joint_composite_v<JointModel>, void>
        algo(pinocchio::JointModelBase<JointModel> const & /* joint */,
             pinocchio::Data const & /* pncData */,
             vectorN_t const & /* v */,
             vectorN_t const & /* velocityLimitMax */,
             std::unique_ptr<EngineMultiRobot::engineOptions_t const> const & /* engineOptions */,
             contactModel_t const & /* contactModel */,
             vectorN_t & /* u */)
        {
            PRINT_WARNING("No velocity bounds implemented for this type of joint.");
        }
    };

    void EngineMultiRobot::computeInternalDynamics(systemHolder_t     const & system,
                                                   systemDataHolder_t       & systemData,
                                                   float64_t          const & /* t */,
                                                   vectorN_t          const & q,
                                                   vectorN_t          const & v,
                                                   vectorN_t                & uInternal) const
    {

        // Define some proxies
        pinocchio::Model const & pncModel = system.robot->pncModel_;
        pinocchio::Data const & pncData = system.robot->pncData_;

        // Enforce the position limit (rigid joints only)
        if (system.robot->mdlOptions_->joints.enablePositionLimit)
        {
            vectorN_t const & positionLimitMin = system.robot->getPositionLimitMin();
            vectorN_t const & positionLimitMax = system.robot->getPositionLimitMax();
            std::vector<jointIndex_t> const & rigidJointsIdx = system.robot->getRigidJointsModelIdx();
            for (std::size_t i = 0; i < rigidJointsIdx.size(); ++i)
            {
                auto & constraint = systemData.constraintsHolder.boundJoints[i].second;
                computePositionLimitsForcesAlgo::run(pncModel.joints[rigidJointsIdx[i]],
                    typename computePositionLimitsForcesAlgo::ArgsType(
                        pncData, q, v, positionLimitMin, positionLimitMax,
                        engineOptions_, contactModel_, constraint, uInternal));
            }
        }

        // Enforce the velocity limit (rigid joints only)
        if (system.robot->mdlOptions_->joints.enableVelocityLimit)
        {
            vectorN_t const & velocityLimitMax = system.robot->getVelocityLimit();
            for (jointIndex_t const & rigidIdx : system.robot->getRigidJointsModelIdx())
            {
                computeVelocityLimitsForcesAlgo::run(pncModel.joints[rigidIdx],
                    typename computeVelocityLimitsForcesAlgo::ArgsType(
                        pncData, v, velocityLimitMax, engineOptions_, contactModel_, uInternal));
            }
        }

        // Compute the flexibilities (only support joint_t::SPHERICAL so far)
        float64_t angle;
        matrix3_t rotJlog3;
        Robot::dynamicsOptions_t const & mdlDynOptions = system.robot->mdlOptions_->dynamics;
        std::vector<jointIndex_t> const & flexibilityIdx = system.robot->getFlexibleJointsModelIdx();
        for (std::size_t i = 0; i < flexibilityIdx.size(); ++i)
        {
            jointIndex_t const & jointIdx = flexibilityIdx[i];
            uint32_t const & positionIdx = pncModel.joints[jointIdx].idx_q();
            uint32_t const & velocityIdx = pncModel.joints[jointIdx].idx_v();
            vector3_t const & stiffness = mdlDynOptions.flexibilityConfig[i].stiffness;
            vector3_t const & damping = mdlDynOptions.flexibilityConfig[i].damping;

            Eigen::Map<const quaternion_t> const quat(q.segment<4>(positionIdx).data());
            vector3_t const angleAxis = pinocchio::quaternion::log3(quat, angle);
            assert((angle < 0.95 * M_PI) && "Flexible joint angle must be smaller than 0.95 * pi.");
            pinocchio::Jlog3(angle, angleAxis, rotJlog3);
            uInternal.segment<3>(velocityIdx) -=
                rotJlog3 * (stiffness.array() * angleAxis.array()).matrix();
            uInternal.segment<3>(velocityIdx).array() -=
                damping.array() * v.segment<3>(velocityIdx).array();
        }
    }

    void EngineMultiRobot::computeCollisionForces(systemHolder_t     const & system,
                                                  systemDataHolder_t       & systemData,
                                                  forceVector_t            & fext) const
    {
        // Compute the forces at contact points
        std::vector<frameIndex_t> const & contactFramesIdx = system.robot->getContactFramesIdx();
        for (std::size_t i = 0; i < contactFramesIdx.size(); ++i)
        {
            // Compute force at the given contact frame.
            frameIndex_t const & frameIdx = contactFramesIdx[i];
            auto & constraint = systemData.constraintsHolder.contactFrames[i].second;
            pinocchio::Force & fextLocal = systemData.contactFramesForces[i];
            computeContactDynamicsAtFrame(system, frameIdx, constraint, fextLocal);

            // Apply the force at the origin of the parent joint frame, in local joint frame
            jointIndex_t const & parentJointIdx = system.robot->pncModel_.frames[frameIdx].parent;
            fext[parentJointIdx] += fextLocal;

            // Convert contact force from the global frame to the local frame to store it in contactForces_
            pinocchio::SE3 const & transformContactInJoint = system.robot->pncModel_.frames[frameIdx].placement;
            system.robot->contactForces_[i] = transformContactInJoint.actInv(fextLocal);
        }

        // Compute the force at collision bodies
        std::vector<frameIndex_t> const & collisionBodiesIdx = system.robot->getCollisionBodiesIdx();
        std::vector<std::vector<pairIndex_t> > const & collisionPairsIdx = system.robot->getCollisionPairsIdx();
        for (std::size_t i = 0; i < collisionBodiesIdx.size(); ++i)
        {
            // Compute force at the given collision body.
            // It returns the force applied at the origin of the parent joint frame, in global frame
            frameIndex_t const & frameIdx = collisionBodiesIdx[i];
            jointIndex_t const & parentJointIdx = system.robot->pncModel_.frames[frameIdx].parent;
            for (std::size_t j = 0; j < collisionPairsIdx[i].size(); ++j)
            {
                pairIndex_t const & collisionPairIdx = collisionPairsIdx[i][j];
                auto & constraint = systemData.constraintsHolder.collisionBodies[i][j].second;
                pinocchio::Force & fextLocal = systemData.collisionBodiesForces[i][j];
                computeContactDynamicsAtBody(system, collisionPairIdx, constraint, fextLocal);

                // Apply the force at the origin of the parent joint frame, in local joint frame
                fext[parentJointIdx] += fextLocal;
            }
        }
    }

    void EngineMultiRobot::computeExternalForces(systemHolder_t     const & system,
                                                 systemDataHolder_t       & systemData,
                                                 float64_t          const & t,
                                                 vectorN_t          const & q,
                                                 vectorN_t          const & v,
                                                 forceVector_t            & fext)
    {
        // Add the effect of user-defined external impulse forces
        auto forcesImpulseActiveIt = systemData.forcesImpulseActive.begin();
        auto forcesImpulseIt = systemData.forcesImpulse.begin();
        for ( ; forcesImpulseIt != systemData.forcesImpulse.end() ;
             ++forcesImpulseActiveIt, ++forcesImpulseIt)
        {
            /* Do not check if the force is active at this point.
               This is managed at stepper level to get around the
               ambiguous t- versus t+. */
            if (*forcesImpulseActiveIt)
            {
                frameIndex_t const & frameIdx = forcesImpulseIt->frameIdx;
                jointIndex_t const & parentJointIdx = system.robot->pncModel_.frames[frameIdx].parent;
                pinocchio::Force const & F = forcesImpulseIt->F;

                fext[parentJointIdx] += convertForceGlobalFrameToJoint(
                    system.robot->pncModel_, system.robot->pncData_, frameIdx, F);
            }
        }

        // Add the effect of time-continuous external force profiles
        for (auto & forceProfile : systemData.forcesProfile)
        {
            frameIndex_t const & frameIdx = forceProfile.frameIdx;
            jointIndex_t const & parentJointIdx = system.robot->pncModel_.frames[frameIdx].parent;
            if (forceProfile.updatePeriod < EPS)
            {
                forceProfile.forcePrev = forceProfile.forceFct(t, q, v);
            }
            fext[parentJointIdx] += convertForceGlobalFrameToJoint(
                system.robot->pncModel_, system.robot->pncData_, frameIdx, forceProfile.forcePrev);
        }
    }

    void EngineMultiRobot::computeForcesCoupling(float64_t              const & t,
                                                 std::vector<vectorN_t> const & qSplit,
                                                 std::vector<vectorN_t> const & vSplit)
    {
        for (auto & forceCoupling : forcesCoupling_)
        {
            // Extract info about the first system involved
            int32_t const & systemIdx1 = forceCoupling.systemIdx1;
            systemHolder_t const & system1 = systems_[systemIdx1];
            vectorN_t const & q1 = qSplit[systemIdx1];
            vectorN_t const & v1 = vSplit[systemIdx1];
            frameIndex_t const & frameIdx1 = forceCoupling.frameIdx1;
            forceVector_t & fext1 = systemsDataHolder_[systemIdx1].state.fExternal;

            // Extract info about the second system involved
            int32_t const & systemIdx2 = forceCoupling.systemIdx2;
            systemHolder_t const & system2 = systems_[systemIdx2];
            vectorN_t const & q2 = qSplit[systemIdx2];
            vectorN_t const & v2 = vSplit[systemIdx2];
            frameIndex_t const & frameIdx2 = forceCoupling.frameIdx2;
            forceVector_t & fext2 = systemsDataHolder_[systemIdx2].state.fExternal;

            // Compute the coupling force
            pinocchio::Force force = forceCoupling.forceFct(t, q1, v1, q2, v2);
            jointIndex_t const & parentJointIdx1 = system1.robot->pncModel_.frames[frameIdx1].parent;
            fext1[parentJointIdx1] += convertForceGlobalFrameToJoint(
                system1.robot->pncModel_, system1.robot->pncData_, frameIdx1, force);

            // Move force from frame1 to frame2 to apply it to the second system
            force.toVector() *= -1;
            jointIndex_t const & parentJointIdx2 = system2.robot->pncModel_.frames[frameIdx2].parent;
            vector3_t const offset = system2.robot->pncData_.oMf[frameIdx2].translation() -
                                     system1.robot->pncData_.oMf[frameIdx1].translation();
            force.angular() -= offset.cross(force.linear());
            fext2[parentJointIdx2] += convertForceGlobalFrameToJoint(
                system2.robot->pncModel_, system2.robot->pncData_, frameIdx2, force);
        }
    }

    void EngineMultiRobot::computeAllTerms(float64_t              const & t,
                                           std::vector<vectorN_t> const & qSplit,
                                           std::vector<vectorN_t> const & vSplit)
    {
        // Reinitialize the external forces and internal efforts
        for (auto & systemData : systemsDataHolder_)
        {
            for (pinocchio::Force & fext_i : systemData.state.fExternal)
            {
                fext_i.setZero();
            }
            systemData.state.uInternal.setZero();
        }

        // Compute the internal forces
        computeForcesCoupling(t, qSplit, vSplit);

        // Compute each individual system dynamics
        auto systemIt = systems_.begin();
        auto systemDataIt = systemsDataHolder_.begin();
        auto qIt = qSplit.begin();
        auto vIt = vSplit.begin();
        for ( ; systemIt != systems_.end();
             ++systemIt, ++systemDataIt, ++qIt, ++vIt)
        {
            // Define some proxies
            forceVector_t & fext = systemDataIt->state.fExternal;
            vectorN_t & uInternal = systemDataIt->state.uInternal;

            /* Compute internal dynamics, namely the efforts in joint space associated
               with position/velocity bounds dynamics, and flexibility dynamics. */
            computeInternalDynamics(*systemIt, *systemDataIt, t, *qIt, *vIt, uInternal);

            /* Compute the collision forces and estimated time at which the contact state
               will changed (Take-off / Touch-down). */
            computeCollisionForces(*systemIt, *systemDataIt, fext);

            // Compute the external contact forces.
            computeExternalForces(*systemIt, *systemDataIt, t, *qIt, *vIt, fext);
        }
    }

    hresult_t EngineMultiRobot::computeSystemsDynamics(float64_t              const & t,
                                                       std::vector<vectorN_t> const & qSplit,
                                                       std::vector<vectorN_t> const & vSplit,
                                                       std::vector<vectorN_t>       & aSplit)
    {
        /* - Note that the position of the free flyer is in world frame,
             whereas the velocities and accelerations are relative to
             the parent body frame. */

        // Make sure that a simulation is running
        if (!isSimulationRunning_)
        {
            PRINT_ERROR("No simulation running. Please start it before calling this method.");
            return hresult_t::ERROR_INIT_FAILED;
        }

        // Make sure memory has been allocated for the output acceleration
        aSplit.resize(vSplit.size());

        // Update the kinematics of each system
        auto systemIt = systems_.begin();
        auto systemDataIt = systemsDataHolder_.begin();
        auto qIt = qSplit.begin();
        auto vIt = vSplit.begin();
        for ( ; systemIt != systems_.end();
             ++systemIt, ++systemDataIt, ++qIt, ++vIt)
        {
            vectorN_t const & aPrev = systemDataIt->statePrev.a;
            computeForwardKinematics(*systemIt, *qIt, *vIt, aPrev);
        }

        /* Compute internal and external forces and efforts applied on every systems,
           excluding user-specified internal dynamics if any.
           Note that one must call this method BEFORE updating the sensors
           since the force sensor measurements rely on robot_->contactForces_. */
        computeAllTerms(t, qSplit, vSplit);

        // Compute each individual system dynamics
        systemIt = systems_.begin();
        systemDataIt = systemsDataHolder_.begin();
        qIt = qSplit.begin();
        vIt = vSplit.begin();
        auto contactForcesPrevIt = contactForcesPrev_.begin();
        auto fPrevIt = fPrev_.begin();
        auto aPrevIt = aPrev_.begin();
        auto aIt = aSplit.begin();
        for ( ; systemIt != systems_.end();
             ++systemIt, ++systemDataIt, ++qIt, ++vIt, ++aIt, ++contactForcesPrevIt, ++fPrevIt, ++aPrevIt)
        {
            // Define some proxies
            vectorN_t & u = systemDataIt->state.u;
            vectorN_t & command = systemDataIt->state.command;
            vectorN_t & uMotor = systemDataIt->state.uMotor;
            vectorN_t & uInternal = systemDataIt->state.uInternal;
            vectorN_t & uCustom = systemDataIt->state.uCustom;
            forceVector_t & fext = systemDataIt->state.fExternal;
            vectorN_t const & aPrev = systemDataIt->statePrev.a;
            vectorN_t const & uMotorPrev = systemDataIt->statePrev.uMotor;
            forceVector_t const & fextPrev = systemDataIt->statePrev.fExternal;

            /* Update the sensor data if necessary (only for infinite update frequency).
               Note that it is impossible to have access to the current accelerations
               and efforts since they depend on the sensor values themselves. */
            if (engineOptions_->stepper.sensorsUpdatePeriod < EPS)
            {
                // Roll back to forces and accelerations computed at previous iteration
                contactForcesPrevIt->swap(systemIt->robot->contactForces_);
                fPrevIt->swap(systemIt->robot->pncData_.f);
                aPrevIt->swap(systemIt->robot->pncData_.a);

                // Update sensors based on previous accelerations and forces
                systemIt->robot->setSensorsData(t, *qIt, *vIt, aPrev, uMotorPrev, fextPrev);

                // Restore current forces and accelerations
                contactForcesPrevIt->swap(systemIt->robot->contactForces_);
                fPrevIt->swap(systemIt->robot->pncData_.f);
                aPrevIt->swap(systemIt->robot->pncData_.a);
            }

            /* Update the controller command if necessary (only for infinite update frequency).
               Make sure that the sensor state has been updated beforehand. */
            if (engineOptions_->stepper.controllerUpdatePeriod < EPS)
            {
                computeCommand(*systemIt, t, *qIt, *vIt, command);
            }

            /* Compute the actual motor effort.
               Note that it is impossible to have access to the current accelerations. */
            systemIt->robot->computeMotorsEfforts(t, *qIt, *vIt, aPrev, command);
            uMotor = systemIt->robot->getMotorsEfforts();

            /* Compute the user-defined internal dynamics.
               Make sure that the sensor state has been updated beforehand since
               the user-defined internal dynamics may rely on it. */
            uCustom.setZero();
            systemIt->controller->internalDynamics(t, *qIt, *vIt, uCustom);

            // Compute the total effort vector
            u = uInternal + uCustom;
            for (auto const & motor : systemIt->robot->getMotors())
            {
                std::size_t const & motorIdx = motor->getIdx();
                int32_t const & motorVelocityIdx = motor->getJointVelocityIdx();
                u[motorVelocityIdx] += uMotor[motorIdx];
            }

            // Compute the dynamics
            *aIt = computeAcceleration(*systemIt, *systemDataIt, *qIt, *vIt, u, fext);
        }

        return hresult_t::SUCCESS;
    }

    vectorN_t const & EngineMultiRobot::computeAcceleration(systemHolder_t & system,
                                                            systemDataHolder_t & systemData,
                                                            vectorN_t const & q,
                                                            vectorN_t const & v,
                                                            vectorN_t const & u,
                                                            forceVector_t & fext,
                                                            bool_t const & ignoreBounds)
    {
        pinocchio::Model const & model = system.robot->pncModel_;
        pinocchio::Data & data = system.robot->pncData_;

        if (system.robot->hasConstraints())
        {
            // Define some proxies for convenience
            matrix6N_t & jointJacobian = systemData.jointJacobian;

            // Compute kinematic constraints
            system.robot->computeConstraints(q, v);

            // Project external forces from cartesian space to joint space
            data.u = u;
            for (int32_t i = 1; i < model.njoints; ++i)
            {
                jointJacobian.setZero();
                pinocchio::getJointJacobian(model,
                                            data,
                                            i,
                                            pinocchio::LOCAL,
                                            jointJacobian);
                data.u.noalias() += jointJacobian.transpose() * fext[i].toVector();
            }

            // Compute non-linear effects
            pinocchio::nonLinearEffects(model, data, q, v);

            // Call forward dynamics
            systemData.constraintSolver->SolveBoxedForwardDynamics(
                engineOptions_->constraints.regularization, ignoreBounds);

            // Restore contact frame forces and bounds internal efforts
            systemData.constraintsHolder.foreach(
                constraintsHolderType_t::BOUNDS_JOINTS,
                [&u = systemData.state.u,
                 &uInternal = systemData.state.uInternal,
                 &joints = const_cast<pinocchio::Model::JointModelVector &>(model.joints)](
                    std::shared_ptr<AbstractConstraintBase> & constraint,
                    constraintsHolderType_t const & /* holderType */)
                {
                    if (!constraint->getIsEnabled())
                    {
                        return;
                    }

                    vectorN_t & uJoint = constraint->lambda_;
                    auto const & jointConstraint = static_cast<JointConstraint const &>(*constraint.get());
                    auto const & jointModel = joints[jointConstraint.getJointIdx()];
                    jointModel.jointVelocitySelector(uInternal) += uJoint;
                    jointModel.jointVelocitySelector(u) += uJoint;
                });

            auto constraintIt = systemData.constraintsHolder.contactFrames.begin();
            auto forceIt = system.robot->contactForces_.begin();
            for ( ; constraintIt != systemData.constraintsHolder.contactFrames.end() ;
                ++constraintIt, ++forceIt)
            {
                auto & constraint = *constraintIt->second.get();
                if (!constraint.getIsEnabled())
                {
                    continue;
                }
                auto const & frameConstraint = static_cast<FixedFrameConstraint const &>(constraint);

                // Extract force in local reference-frame-aligned from lagrangian multipliers
                pinocchio::Force fextInLocal(
                    frameConstraint.lambda_.head<3>(),
                    frameConstraint.lambda_[3] * vector3_t::UnitZ());

                // Compute force in local world aligned frame
                matrix3_t const & rotationLocal = frameConstraint.getLocalFrame();
                pinocchio::Force const fextInWorld({
                    rotationLocal * fextInLocal.linear(),
                    rotationLocal * fextInLocal.angular(),
                });

                // Convert the force from local world aligned frame to local frame
                frameIndex_t const & frameIdx = frameConstraint.getFrameIdx();
                auto rotationWorldInContact = data.oMf[frameIdx].rotation().transpose();
                forceIt->linear().noalias() = rotationWorldInContact * fextInWorld.linear();
                forceIt->angular().noalias() = rotationWorldInContact * fextInWorld.angular();

                // Convert the force from local world aligned to local parent joint
                jointIndex_t const & jointIdx = model.frames[frameIdx].parent;
                fext[jointIdx] += convertForceGlobalFrameToJoint(model, data, frameIdx, fextInWorld);
            }

            systemData.constraintsHolder.foreach(
                constraintsHolderType_t::COLLISION_BODIES,
                [&fext, &model, &data](
                    std::shared_ptr<AbstractConstraintBase> & constraint,
                    constraintsHolderType_t const & /* holderType */)
                {
                    if (!constraint->getIsEnabled())
                    {
                        return;
                    }
                    auto const & frameConstraint = static_cast<FixedFrameConstraint const &>(*constraint.get());

                    // Extract force in world frame from lagrangian multipliers
                    pinocchio::Force fextInLocal(
                        frameConstraint.lambda_.head<3>(),
                        frameConstraint.lambda_[3] * vector3_t::UnitZ());

                    // Compute force in world frame
                    matrix3_t const & rotationLocal = frameConstraint.getLocalFrame();
                    pinocchio::Force const fextInWorld({
                        rotationLocal * fextInLocal.linear(),
                        rotationLocal * fextInLocal.angular(),
                    });

                    // Convert the force from local world aligned to local parent joint
                    frameIndex_t const & frameIdx = frameConstraint.getFrameIdx();
                    jointIndex_t const & jointIdx = model.frames[frameIdx].parent;
                    fext[jointIdx] += convertForceGlobalFrameToJoint(model, data, frameIdx, fextInWorld);
                });

            return data.ddq;
        }
        else
        {
            // No kinematic constraint: run aba algorithm
            return pinocchio_overload::aba(model, data, q, v, u, fext);
        }
    }

    // ===================================================================
    // ================ Log reading and writing utilities ================
    // ===================================================================

    hresult_t EngineMultiRobot::getLog(std::shared_ptr<logData_t const> & logData)
    {
        hresult_t returnCode = hresult_t::SUCCESS;

        // Update internal log data buffer if uninitialized
        if (!logData_)
        {
            logData_ = std::make_shared<logData_t>();
            returnCode = telemetryRecorder_->getLog(*logData_);
        }

        // Return shared pointer to internal log data buffer
        logData = std::const_pointer_cast<logData_t const>(logData_);

        return returnCode;
    }

    hresult_t readLogHdf5(std::string const & filename,
                          logData_t         & logData)
    {
        // Clear log data if any
        logData = {};

        // Open HDF5 logfile
        std::unique_ptr<H5::H5File> file;
        try {
            /* Specifying `H5F_CLOSE_STRONG` must be specified to completely
               close the file (including all open objects) before returning.
               See: https://docs.hdfgroup.org/hdf5/v1_12/group___f_a_p_l.html#ga60e3567f677fd3ade75b909b636d7b9c
            */
            H5::FileAccPropList access_plist;
            access_plist.setFcloseDegree(H5F_CLOSE_STRONG);
            file = std::make_unique<H5::H5File>(
                filename, H5F_ACC_RDONLY, H5::FileCreatPropList::DEFAULT, access_plist);
        } catch (H5::FileIException const & open_file) {
            PRINT_ERROR("Impossible to open the log file. "
                        "Make sure it exists and you have reading permissions.");
            return hresult_t::ERROR_BAD_INPUT;
        }

        // Extract all constants. There is no ordering among them, unlike variables.
        H5::Group constantsGroup = file->openGroup("/constants");
        file->iterateElems("/constants", NULL, [](
            hid_t group, char const * name, void * op_data) -> herr_t
        {
            logData_t * logDataPtr = static_cast<logData_t *>(op_data);
            H5::Group _constantsGroup(group);
            H5::DataSet const constantDataSet = _constantsGroup.openDataSet(name);
            H5::DataSpace const constantSpace = H5::DataSpace(H5S_SCALAR);
            H5::StrType const constantDataType = constantDataSet.getStrType();
            hssize_t const numBytes = constantDataType.getSize();
            H5::StrType stringType(H5::PredType::C_S1, numBytes);
            stringType.setStrpad(H5T_str_t::H5T_STR_NULLPAD);
            std::string value(numBytes, '\0');
            constantDataSet.read(value.data(), stringType, constantSpace);
            logDataPtr->constants.emplace_back(name, std::move(value));
            return 0;
        }, static_cast<void *>(&logData));

        // Extract the timestamps
        H5::DataSet const globalTimeDataSet = file->openDataSet(GLOBAL_TIME);
        H5::DataSpace const timeSpace = globalTimeDataSet.getSpace();
		hssize_t numData = timeSpace.getSimpleExtentNpoints();
        logData.timestamps.resize(numData);
        globalTimeDataSet.read(logData.timestamps.data(), H5::PredType::NATIVE_INT64);

        // Add "unit" attribute to GLOBAL_TIME vector
        H5::Attribute const unitAttrib = globalTimeDataSet.openAttribute("unit");
        unitAttrib.read(H5::PredType::NATIVE_DOUBLE, &logData.timeUnit);

        // Get the (partitioned) number of variables
        H5::Group variablesGroup = file->openGroup("/variables");
        int64_t numInt = 0, numFloat = 0;
        std::pair<int64_t &, int64_t &> numVar {numInt, numFloat};
        H5Literate(variablesGroup.getId(), H5_INDEX_CRT_ORDER, H5_ITER_INC, NULL, [](
            hid_t group, char const * name, H5L_info_t const * /* oinfo */, void * op_data
            ) -> herr_t
        {
            auto [_numInt, _numFloat] =
                *static_cast<std::pair<int64_t &, int64_t &> *>(op_data);
            H5::Group fieldGroup = H5::Group(group).openGroup(name);
            H5::DataSet const valueDataset = fieldGroup.openDataSet("value");
            H5T_class_t const valueType = valueDataset.getTypeClass();
            if(valueType == H5T_FLOAT)
            {
                ++_numFloat;
            }
            else
            {
                ++_numInt;
            }
            return 0;
        }, static_cast<void *>(&numVar));

        // Pre-allocate memory
        logData.intData.resize(numInt, numData);
        logData.floatData.resize(numFloat, numData);
        Eigen::Matrix<int64_t, Eigen::Dynamic, 1> intVector(numData);
        Eigen::Matrix<float64_t, Eigen::Dynamic, 1> floatVector(numData);
        logData.fieldnames.reserve(1 + numInt + numFloat);
        logData.fieldnames.push_back(GLOBAL_TIME);

        // Read all variables while preserving ordering
        using opDataT = std::tuple<
            logData_t &,
            Eigen::Matrix<int64_t, Eigen::Dynamic, 1> &,
            Eigen::Matrix<float64_t, Eigen::Dynamic, 1> &>;
        opDataT opData {logData, intVector, floatVector};
        H5Literate(variablesGroup.getId(), H5_INDEX_CRT_ORDER, H5_ITER_INC, NULL, [](
            hid_t group, char const * name, H5L_info_t const * /* oinfo */, void * op_data
            ) -> herr_t
        {
            auto [_logData, _intVector, _floatVector] = *static_cast<opDataT *>(op_data);
            Eigen::Index const varIdx = _logData.fieldnames.size() - 1;
            int64_t const _numInt = _logData.intData.rows();
            H5::Group fieldGroup = H5::Group(group).openGroup(name);
            H5::DataSet const valueDataset = fieldGroup.openDataSet("value");
            if(varIdx < _numInt)
            {
                valueDataset.read(_intVector.data(), H5::PredType::NATIVE_INT64);
                _logData.intData.row(varIdx) = _intVector;
            }
            else
            {
                valueDataset.read(_floatVector.data(), H5::PredType::NATIVE_DOUBLE);
                _logData.floatData.row(varIdx - _numInt) = _floatVector;
            }
            _logData.fieldnames.push_back(name);
            return 0;
        }, static_cast<void *>(&opData));

        // Close file once done
        file->close();

        return hresult_t::SUCCESS;
    }

    hresult_t EngineMultiRobot::readLog(std::string const & filename,
                                        std::string const & format,
                                        logData_t         & logData)
    {
        if (format == "binary")
        {
            return TelemetryRecorder::readLog(filename, logData);
        }
        else if (format == "hdf5")
        {
            return readLogHdf5(filename, logData);
        }

        PRINT_ERROR("Format '", format, "' not recognized. It must be either 'binary' or 'hdf5'.");
        return hresult_t::ERROR_BAD_INPUT;
    }

    hresult_t writeLogHdf5(std::string                      const & filename,
                           std::shared_ptr<logData_t const> const & logData)
    {
        // Open HDF5 logfile
        std::unique_ptr<H5::H5File> file;
        try {
            H5::FileAccPropList access_plist;
            access_plist.setFcloseDegree(H5F_CLOSE_STRONG);
            file = std::make_unique<H5::H5File>(
                filename, H5F_ACC_TRUNC, H5::FileCreatPropList::DEFAULT, access_plist);
        } catch (H5::FileIException const & open_file) {
            PRINT_ERROR("Impossible to create the log file. "
                        "Make sure the root folder exists and you have writing permissions.");
            return hresult_t::ERROR_BAD_INPUT;
        }

        // Add "VERSION" attribute
        H5::DataSpace const versionSpace = H5::DataSpace(H5S_SCALAR);
        H5::Attribute const versionAttrib = file->createAttribute(
            "VERSION", H5::PredType::NATIVE_INT32, versionSpace);
        versionAttrib.write(H5::PredType::NATIVE_INT32, &logData->version);

        // Add "START_TIME" attribute
        int64_t time = std::time(nullptr);
        H5::DataSpace const startTimeSpace = H5::DataSpace(H5S_SCALAR);
        H5::Attribute const startTimeAttrib = file->createAttribute(
            "START_TIME", H5::PredType::NATIVE_INT64, startTimeSpace);
        startTimeAttrib.write(H5::PredType::NATIVE_INT64, &time);

        // Add GLOBAL_TIME vector
        hsize_t const timeDims[1] = {hsize_t(logData->timestamps.size())};
        H5::DataSpace const globalTimeSpace = H5::DataSpace(1, timeDims);
        H5::DataSet const globalTimeDataSet = file->createDataSet(
            GLOBAL_TIME, H5::PredType::NATIVE_INT64, globalTimeSpace);
        globalTimeDataSet.write(logData->timestamps.data(), H5::PredType::NATIVE_INT64);

        // Add "unit" attribute to GLOBAL_TIME vector
        H5::DataSpace const unitSpace = H5::DataSpace(H5S_SCALAR);
        H5::Attribute const unitAttrib = globalTimeDataSet.createAttribute(
            "unit", H5::PredType::NATIVE_DOUBLE, unitSpace);
        unitAttrib.write(H5::PredType::NATIVE_DOUBLE, &logData->timeUnit);

        // Add group "constants"
        H5::Group constantsGroup(file->createGroup("constants"));
        for (auto const & [key, value] : logData->constants)
        {
            // Define a dataset with a single string of fixed length
            H5::DataSpace const constantSpace = H5::DataSpace(H5S_SCALAR);
            H5::StrType stringType(H5::PredType::C_S1, std::max(value.size(), std::size_t(1)));

            // To tell parser continue reading if '\0' is encountered
            stringType.setStrpad(H5T_str_t::H5T_STR_NULLPAD);

            // Write the constant
            H5::DataSet constantDataSet = constantsGroup.createDataSet(
                key, stringType, constantSpace);
            constantDataSet.write(value, stringType);
        }

        // Temporary contiguous storage for variables
        Eigen::Matrix<int64_t, Eigen::Dynamic, 1> intVector;
        Eigen::Matrix<float64_t, Eigen::Dynamic, 1> floatVector;

        // Get the number of integer and float variables
        Eigen::Index const numInt = logData->intData.rows();
        Eigen::Index const numFloat = logData->floatData.rows();

        /* Add group "variables".
           C++ helper `file->createGroup("variables")` cannot be used
           because we want to preserve order. */
        hid_t group_creation_plist = H5Pcreate(H5P_GROUP_CREATE);
        H5Pset_link_creation_order(
            group_creation_plist, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED);
        hid_t group_id = H5Gcreate(
            file->getId(), "/variables/",
            H5P_DEFAULT, group_creation_plist, H5P_DEFAULT);
        H5::Group variablesGroup(group_id);

        // Store all integers
        for (Eigen::Index i = 0; i < numInt; ++i)
        {
            std::string const & key = logData->fieldnames[i];

            // Create group for field
            H5::Group fieldGroup = variablesGroup.createGroup(key);

            // Enable compression and shuffling
            H5::DSetCreatPropList plist;
            hsize_t chunkSize[1];
            chunkSize[0] = logData->timestamps.size();  // Read the whole vector at once.
            plist.setChunk(1, chunkSize);
            plist.setShuffle();
            plist.setDeflate(4);

            // Create time dataset using symbolic link
            fieldGroup.link(H5L_TYPE_HARD, "/" + GLOBAL_TIME, "time");

            // Create variable dataset
            H5::DataSpace valueSpace = H5::DataSpace(1, timeDims);
            H5::DataSet valueDataset = fieldGroup.createDataSet(
                "value", H5::PredType::NATIVE_INT64, valueSpace, plist);

            // Write values in one-shot for efficiency
            intVector = logData->intData.row(i);
            valueDataset.write(intVector.data(), H5::PredType::NATIVE_INT64);
        }

        // Store all floats
        for (Eigen::Index i = 0; i < numFloat; ++i)
        {
            std::string const & key = logData->fieldnames[i + 1 + numInt];

            // Create group for field
            H5::Group fieldGroup(variablesGroup.createGroup(key));

            // Enable compression and shuffling
            H5::DSetCreatPropList plist;
            hsize_t chunkSize[1];
            chunkSize[0] = logData->timestamps.size();  // Read the whole vector at once.
            plist.setChunk(1, chunkSize);
            plist.setShuffle();
            plist.setDeflate(4);

            // Create time dataset using symbolic link
            fieldGroup.link(H5L_TYPE_HARD, "/" + GLOBAL_TIME, "time");

            // Create variable dataset
            H5::DataSpace valueSpace = H5::DataSpace(1, timeDims);
            H5::DataSet valueDataset = fieldGroup.createDataSet(
                "value", H5::PredType::NATIVE_DOUBLE, valueSpace, plist);

            // Write values
            floatVector = logData->floatData.row(i);
            valueDataset.write(floatVector.data(), H5::PredType::NATIVE_DOUBLE);
        }

        // Close file once done
        file->close();

        return hresult_t::SUCCESS;
    }

    hresult_t EngineMultiRobot::writeLog(std::string const & filename,
                                         std::string const & format)
    {
        hresult_t returnCode = hresult_t::SUCCESS;

        // Make sure there is something to write
        if (!isTelemetryConfigured_)
        {
            PRINT_ERROR("Telemetry not configured. Please run a simulation before writing log.");
            returnCode = hresult_t::ERROR_BAD_INPUT;
        }

        // Pick the appropriate format
        if (returnCode == hresult_t::SUCCESS)
        {
            if (format == "binary")
            {
                returnCode = telemetryRecorder_->writeLog(filename);
            }
            else if (format == "hdf5")
            {
                // Extract log data
                std::shared_ptr<logData_t const> logData;
                returnCode = getLog(logData);

                // Make sure it is not empty
                if (returnCode == hresult_t::SUCCESS)
                {
                    if (logData->timestamps.size() == 0)
                    {
                        PRINT_ERROR("No data available. Please start a simulation before writing log.");
                        returnCode = hresult_t::ERROR_BAD_INPUT;
                    }
                }

                // Write log data
                if (returnCode == hresult_t::SUCCESS)
                {
                    returnCode = writeLogHdf5(filename, logData);
                }
            }
            else
            {
                PRINT_ERROR("Format '", format, "' not recognized. It must be either 'binary' or 'hdf5'.");
                returnCode = hresult_t::ERROR_BAD_INPUT;
            }
        }

        return returnCode;
    }
}
