#include "jiminy/core/robot/Model.h"
#include "jiminy/core/constraints/AbstractConstraint.h"
#include "jiminy/core/constraints/JointConstraint.h"
#include "jiminy/core/constraints/FixedFrameConstraint.h"
#include "jiminy/core/constraints/DistanceConstraint.h"
#include "jiminy/core/constraints/SphereConstraint.h"
#include "jiminy/core/constraints/WheelConstraint.h"

#include "pinocchio/bindings/python/fwd.hpp"

#include "jiminy/python/Utilities.h"
#include "jiminy/python/Functors.h"
#include "jiminy/python/Constraints.h"


namespace jiminy
{
    // ***************************** PyConstraintVisitor ***********************************

    // Using an intermediary class is a trick to enable defining bp::base<...> in conjunction with bp::wrapper<...>
    class AbstractConstraintImpl: public AbstractConstraintTpl<AbstractConstraintImpl> {};

    // Explicit template specialization must appear in exactly the same namespace than its template declaration
    template<>
    std::string const AbstractConstraintTpl<AbstractConstraintImpl>::type_("UserConstraint");

namespace python
{
    namespace bp = boost::python;

    class AbstractConstraintWrapper: public AbstractConstraintImpl, public bp::wrapper<AbstractConstraintImpl>
    {
    public:
        hresult_t reset(vectorN_t const & q,
                        vectorN_t const & v)
        {
            bp::override func = this->get_override("reset");
            if (func)
            {
                func(FctPyWrapperArgToPython(q),
                     FctPyWrapperArgToPython(v));
            }
            return hresult_t::SUCCESS;
        }

        hresult_t computeJacobianAndDrift(vectorN_t const & q,
                                          vectorN_t const & v)
        {
            bp::override func = this->get_override("compute_jacobian_and_drift");
            if (func)
            {
                func(FctPyWrapperArgToPython(q),
                     FctPyWrapperArgToPython(v));
            }
            return hresult_t::SUCCESS;
        }
    };

    struct PyConstraintVisitor
        : public bp::def_visitor<PyConstraintVisitor>
    {
    public:
        template<class PyClass>
        void visit(PyClass & cl) const
        {
            cl
                .ADD_PROPERTY_GET_WITH_POLICY("type",
                                              &AbstractConstraintBase::getType,
                                              bp::return_value_policy<bp::copy_const_reference>())
                .ADD_PROPERTY_GET_SET_WITH_POLICY("is_enabled",
                                                  &AbstractConstraintBase::getIsEnabled,
                                                  bp::return_value_policy<bp::copy_const_reference>(),
                                                  &PyConstraintVisitor::setIsEnable)
                .ADD_PROPERTY_GET_SET("baumgarte_freq",
                                      &AbstractConstraintBase::getBaumgarteFreq,
                                      &AbstractConstraintBase::setBaumgarteFreq)
                .ADD_PROPERTY_GET_WITH_POLICY("jacobian",
                                              &AbstractConstraintBase::getJacobian,
                                              bp::return_value_policy<result_converter<false> >())
                .ADD_PROPERTY_GET_WITH_POLICY("drift",
                                              &AbstractConstraintBase::getDrift,
                                              bp::return_value_policy<result_converter<false> >())
                .DEF_READONLY("lambda_c", &AbstractConstraintBase::lambda_)
                ;
        }

        static std::shared_ptr<FixedFrameConstraint> fixedFrameConstraintFactory(std::string const & frameName,
                                                                                 bp::object const & maskFixedPy)
        {
            Eigen::Matrix<bool_t, 6, 1> maskFixed;
            if (maskFixedPy.is_none())
            {
                maskFixed = Eigen::Matrix<bool_t, 6, 1>::Constant(true);
            }
            else
            {
                bp::extract<bp::list> maskFixedPyExtract(maskFixedPy);
                assert(maskFixedPyExtract.check() && "'maskFixedPy' must be a list.");
                bp::list maskFixedListPy = maskFixedPyExtract();
                assert(bp::len(maskFixedListPy) == 6 && "'maskFixedPy' must have length 6.");
                for (uint32_t i = 0; i < 6; ++i)
                {
                    bp::extract<bool_t> maskFixedListPyExtract(maskFixedPy[i]);
                    assert(maskFixedListPyExtract.check() && "'maskFixedPy' elements must be bool.");
                    maskFixed[i] = maskFixedListPyExtract();
                }
            }
            return std::make_shared<FixedFrameConstraint>(frameName, maskFixed);
        }

        static void setIsEnable(AbstractConstraintBase & self,
                                bool_t const & value)
        {
            if (value)
            {
                self.enable();
            }
            else
            {
                self.disable();
            }
        }

    public:
        ///////////////////////////////////////////////////////////////////////////////
        /// \brief Expose.
        ///////////////////////////////////////////////////////////////////////////////
        static void expose()
        {
            bp::class_<AbstractConstraintBase,
                       std::shared_ptr<AbstractConstraintBase>,
                       boost::noncopyable>("AbstractConstraint", bp::no_init)
                .def(PyConstraintVisitor())
                .def("reset", &AbstractConstraintBase::reset,
                              (bp::arg("self"), "q", "v"))
                .def("compute_jacobian_and_drift", &AbstractConstraintBase::computeJacobianAndDrift,
                                                   (bp::arg("self"), "q", "v"));

            bp::class_<AbstractConstraintWrapper, bp::bases<AbstractConstraintBase>,
                       std::shared_ptr<AbstractConstraintWrapper>,
                       boost::noncopyable>("BaseConstraint")
                .def_readonly("type", &AbstractConstraintWrapper::type_)
                .def("reset", bp::pure_virtual(&AbstractConstraintBase::reset))
                .def("compute_jacobian_and_drift", bp::pure_virtual(&AbstractConstraintBase::computeJacobianAndDrift));

            bp::class_<JointConstraint, bp::bases<AbstractConstraintBase>,
                       std::shared_ptr<JointConstraint>,
                       boost::noncopyable>("JointConstraint",
                       bp::init<std::string const &>(bp::args("self", "joint_name")))
                .def_readonly("type", &JointConstraint::type_)
                .ADD_PROPERTY_GET_WITH_POLICY("joint_name",
                                              &JointConstraint::getJointName,
                                              bp::return_value_policy<bp::copy_const_reference>())
                .ADD_PROPERTY_GET_WITH_POLICY("joint_idx",
                                              &JointConstraint::getJointIdx,
                                              bp::return_value_policy<bp::copy_const_reference>())
                .ADD_PROPERTY_GET_SET_WITH_POLICY("reference_configuration",
                                                  &JointConstraint::getReferenceConfiguration,
                                                  bp::return_value_policy<result_converter<false> >(),
                                                  &JointConstraint::setReferenceConfiguration)
                .ADD_PROPERTY_GET_SET_WITH_POLICY("is_enabled",
                                                  &JointConstraint::getRotationDir,
                                                  bp::return_value_policy<bp::copy_const_reference>(),
                                                  &JointConstraint::setRotationDir);

            bp::class_<FixedFrameConstraint, bp::bases<AbstractConstraintBase>,
                       std::shared_ptr<FixedFrameConstraint>,
                       boost::noncopyable>("FixedFrameConstraint", bp::no_init)
                .def("__init__", bp::make_constructor(&PyConstraintVisitor::fixedFrameConstraintFactory,
                                 bp::default_call_policies(), (bp::arg("frame_name"),
                                                               bp::arg("mask_fixed")=bp::object())))
                .def_readonly("type", &FixedFrameConstraint::type_)
                .ADD_PROPERTY_GET_WITH_POLICY("frame_name",
                                              &FixedFrameConstraint::getFrameName,
                                              bp::return_value_policy<bp::copy_const_reference>())
                .ADD_PROPERTY_GET_WITH_POLICY("frame_idx",
                                              &FixedFrameConstraint::getFrameIdx,
                                              bp::return_value_policy<bp::copy_const_reference>())
                .ADD_PROPERTY_GET_WITH_POLICY("dofs_fixed",
                                              &FixedFrameConstraint::getDofsFixed,
                                              bp::return_value_policy<bp::return_by_value>())
                .ADD_PROPERTY_GET_SET_WITH_POLICY("reference_transform",
                                                  &FixedFrameConstraint::getReferenceTransform,
                                                  bp::return_value_policy<result_converter<false> >(),
                                                  &FixedFrameConstraint::setReferenceTransform)
                .ADD_PROPERTY_GET_WITH_POLICY("local_rotation",
                                              &FixedFrameConstraint::getLocalFrame,
                                              bp::return_value_policy<result_converter<false> >())
                .def("set_normal", &FixedFrameConstraint::setNormal);

            bp::class_<DistanceConstraint, bp::bases<AbstractConstraintBase>,
                       std::shared_ptr<DistanceConstraint>,
                       boost::noncopyable>("DistanceConstraint",
                       bp::init<std::string const &, std::string const &>(
                       bp::args("self", "first_frame_name", "second_frame_name")))
                .def_readonly("type", &DistanceConstraint::type_)
                .ADD_PROPERTY_GET_WITH_POLICY("frames_names",
                                              &DistanceConstraint::getFramesNames,
                                              bp::return_value_policy<result_converter<true> >())
                .ADD_PROPERTY_GET_WITH_POLICY("frames_idx",
                                              &DistanceConstraint::getFramesIdx,
                                              bp::return_value_policy<result_converter<true> >())
                .ADD_PROPERTY_GET_SET_WITH_POLICY("reference_distance",
                                                  &DistanceConstraint::getReferenceDistance,
                                                  bp::return_value_policy<bp::copy_const_reference>(),
                                                  &DistanceConstraint::setReferenceDistance);

            bp::class_<SphereConstraint, bp::bases<AbstractConstraintBase>,
                       std::shared_ptr<SphereConstraint>,
                       boost::noncopyable>("SphereConstraint",
                       bp::init<std::string const &, float64_t const &>(
                       bp::args("self", "frame_name", "radius")))
                .def_readonly("type", &SphereConstraint::type_)
                .ADD_PROPERTY_GET_WITH_POLICY("frame_name",
                                              &SphereConstraint::getFrameName,
                                              bp::return_value_policy<bp::copy_const_reference>())
                .ADD_PROPERTY_GET_WITH_POLICY("frame_idx",
                                              &SphereConstraint::getFrameIdx,
                                              bp::return_value_policy<bp::copy_const_reference>())
                .ADD_PROPERTY_GET_SET_WITH_POLICY("reference_transform",
                                                  &SphereConstraint::getReferenceTransform,
                                                  bp::return_value_policy<result_converter<false> >(),
                                                  &SphereConstraint::setReferenceTransform);

            bp::class_<WheelConstraint, bp::bases<AbstractConstraintBase>,
                       std::shared_ptr<WheelConstraint>,
                       boost::noncopyable>("WheelConstraint",
                       bp::init<std::string const &, float64_t const &, vector3_t const &, vector3_t const &>(
                       bp::args("self", "frame_name", "radius", "ground_normal", "wheel_axis")))
                .def_readonly("type", &WheelConstraint::type_)
                .ADD_PROPERTY_GET_WITH_POLICY("frame_name",
                                              &WheelConstraint::getFrameName,
                                              bp::return_value_policy<bp::copy_const_reference>())
                .ADD_PROPERTY_GET_WITH_POLICY("frame_idx",
                                              &WheelConstraint::getFrameIdx,
                                              bp::return_value_policy<bp::copy_const_reference>())
                .ADD_PROPERTY_GET_SET_WITH_POLICY("reference_transform",
                                                  &WheelConstraint::getReferenceTransform,
                                                  bp::return_value_policy<result_converter<false> >(),
                                                  &WheelConstraint::setReferenceTransform);
        }
    };

    BOOST_PYTHON_VISITOR_EXPOSE(Constraint)

    // ***************************** PyConstraintsHolderVisitor ***********************************


    struct PyConstraintsHolderVisitor
        : public bp::def_visitor<PyConstraintsHolderVisitor>
    {
    public:
        ///////////////////////////////////////////////////////////////////////////////
        /// \brief Expose C++ API through the visitor.
        ///////////////////////////////////////////////////////////////////////////////
        template<class PyClass>
        void visit(PyClass & cl) const
        {
            cl
                .ADD_PROPERTY_GET("bounds_joints", &PyConstraintsHolderVisitor::getBoundJoints)
                .ADD_PROPERTY_GET("contact_frames", &PyConstraintsHolderVisitor::getContactFrames)
                .ADD_PROPERTY_GET("collision_bodies", &PyConstraintsHolderVisitor::getCollisionBodies)
                .ADD_PROPERTY_GET("registered", &PyConstraintsHolderVisitor::getRegistered)
                ;
        }

        static bp::dict getBoundJoints(constraintsHolder_t & self)
        {
            bp::dict boundJoints;
            for (auto & constraintItem : self.boundJoints)
            {
                boundJoints[constraintItem.first] = constraintItem.second;
            }
            return boundJoints;
        }

        static bp::dict getContactFrames(constraintsHolder_t & self)
        {
            bp::dict contactFrames;
            for (auto & constraintItem : self.contactFrames)
            {
                contactFrames[constraintItem.first] = constraintItem.second;
            }
            return contactFrames;
        }

        static bp::list getCollisionBodies(constraintsHolder_t & self)
        {
            bp::list collisionBodies;
            for (auto & constraintsMap : self.collisionBodies)
            {
                bp::dict constraintsMapPy;
                for (auto & constraintItem : constraintsMap)
                {
                    constraintsMapPy[constraintItem.first] = constraintItem.second;
                }
                collisionBodies.append(constraintsMapPy);
            }
            return collisionBodies;
        }

        static bp::dict getRegistered(constraintsHolder_t & self)
        {
            bp::dict registered;
            for (auto & constraintItem : self.registered)
            {
                registered[constraintItem.first] = constraintItem.second;
            }
            return registered;
        }

        ///////////////////////////////////////////////////////////////////////////////
        /// \brief Expose.
        ///////////////////////////////////////////////////////////////////////////////
        static void expose()
        {
            bp::class_<constraintsHolder_t,
                       std::shared_ptr<constraintsHolder_t>,
                       boost::noncopyable>("ConstraintsHolder", bp::no_init)
                .def(PyConstraintsHolderVisitor());
        }
    };

    BOOST_PYTHON_VISITOR_EXPOSE(ConstraintsHolder)

}  // End of namespace python.
}  // End of namespace jiminy.
