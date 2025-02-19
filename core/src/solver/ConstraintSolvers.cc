#include "pinocchio/multibody/model.hpp"     // `pinocchio::Model`
#include "pinocchio/multibody/data.hpp"      // `pinocchio::Data`
#include "pinocchio/algorithm/cholesky.hpp"  // `pinocchio::cholesky::`

#include "jiminy/core/robot/PinocchioOverloadAlgorithms.h"
#include "jiminy/core/constraints/AbstractConstraint.h"
#include "jiminy/core/utilities/Random.h"
#include "jiminy/core/utilities/Helpers.h"
#include "jiminy/core/Constants.h"

#include "jiminy/core/solver/ConstraintSolvers.h"


namespace jiminy
{
    PGSSolver::PGSSolver(pinocchio::Model const * model,
                         pinocchio::Data * data,
                         constraintsHolder_t * constraintsHolder,
                         float64_t const & friction,
                         float64_t const & torsion,
                         float64_t const & tolAbs,
                         float64_t const & tolRel,
                         uint32_t const & maxIter) :
    model_(model),
    data_(data),
    maxIter_(maxIter),
    tolAbs_(tolAbs),
    tolRel_(tolRel),
    J_(),
    gamma_(),
    lambda_(),
    constraintsData_(),
    b_(),
    y_(),
    yPrev_()
    {
        Eigen::Index constraintsRowsMax = 0U;
        constraintsHolder->foreach(
            [&](
                std::shared_ptr<AbstractConstraintBase> const & constraint,
                constraintsHolderType_t const & holderType)
            {
                // Define constraint blocks
                Eigen::Index const constraintDim = static_cast<Eigen::Index>(constraint->getDim());
                ConstraintBlock block {};
                ConstraintData constraintData {};
                switch (holderType)
                {
                case constraintsHolderType_t::BOUNDS_JOINTS:
                    // The joint is blocked in only one direction
                    block.lo = 0;
                    block.hi = INF;
                    block.fIdx[0] = 0;
                    block.fSize = 1;
                    constraintData.blocks[0] = block;
                    constraintData.nBlocks = 1;
                    break;
                case constraintsHolderType_t::CONTACT_FRAMES:
                case constraintsHolderType_t::COLLISION_BODIES:
                    // Non-penetration normal force
                    block.lo = 0;
                    block.hi = INF;
                    block.fIdx[0] = 2;
                    block.fSize = 1;
                    constraintData.blocks[0] = block;

                    // Torsional friction around normal axis
                    block.lo = qNAN;
                    block.hi = torsion;
                    block.isZero = (torsion < EPS);
                    block.fIdx[0] = 3;
                    block.fIdx[1] = 2;
                    block.fSize = 2;
                    constraintData.blocks[1] = block;

                    // Friction cone in tangential plane
                    block.lo = qNAN;
                    block.hi = friction;
                    block.isZero = (friction < EPS);
                    block.fIdx[0] = 0;
                    block.fIdx[1] = 1;
                    block.fIdx[2] = 2;
                    block.fSize = 3;
                    constraintData.blocks[2] = block;

                    constraintData.nBlocks = 3;
                    break;
                case constraintsHolderType_t::USER:
                default:
                    break;
                }
                constraintData.dim = constraintDim;
                constraintData.constraint = constraint.get();
                constraintsData_.emplace_back(std::move(constraintData));
                constraintsRowsMax += constraintDim;
            });

        // Resize buffers
        J_.resize(constraintsRowsMax, model_->nv);
        gamma_.resize(constraintsRowsMax);
        lambda_.resize(constraintsRowsMax);
        b_.resize(constraintsRowsMax);
        y_.resize(constraintsRowsMax);
        yPrev_.resize(constraintsRowsMax);
    }

    void PGSSolver::ProjectedGaussSeidelIter(matrixN_t const & A,
                                             vectorN_t::SegmentReturnType const & b,
                                             vectorN_t::SegmentReturnType & x)
    {
        // First, loop over all unbounded constraints
        for (ConstraintData const & constraintData : constraintsData_)
        {
            // Bypass inactive and bounded constraints
            if (constraintData.isInactive || constraintData.nBlocks != 0)
            {
                continue;
            }

            // Loop over all coefficients individually
            Eigen::Index i = constraintData.startIdx;
            Eigen::Index const endIdx = i + constraintData.dim;
            for (; i < endIdx ; ++i)
            {
                y_[i] = b[i] - A.col(i).dot(x);
                x[i] += y_[i] / A(i, i);
            }
        }

        /* Second, loop over all bounds constraints.
           Update breadth-first to converge faster. The deeper is it the more
           coefficients at the very end. Note that the maximum number of blocks
           per constraint is 3. */
        for (std::size_t i = 0; i < 3 ; ++i)
        {
            for (ConstraintData const & constraintData : constraintsData_)
            {
                // Bypass inactive or unbounded constraints or no block left
                if (constraintData.isInactive || constraintData.nBlocks <= i)
                {
                    continue;
                }

                // Extract block data
                ConstraintBlock const & block = constraintData.blocks[i];
                Eigen::Index const * fIdx = block.fIdx;
                std::uint_fast8_t const & fSize = block.fSize;
                Eigen::Index const & o = constraintData.startIdx;
                Eigen::Index const i0 = o + fIdx[0];
                float64_t const & hi = block.hi;
                float64_t const & lo = block.lo;
                float64_t & e = x[i0];

                // Bypass zero-ed coefficients
                if (block.isZero)
                {
                    // Specialization for speed-up
                    e *= 0;
                    for (std::uint_fast8_t j = 1; j < fSize - 1; ++j)
                    {
                        x[o + fIdx[j]] *= 0;
                    }
                    continue;
                }

                // Update several coefficients at once with the same step
                float64_t A_max = A(i0, i0);
                y_[i0] = b[i0] - A.col(i0).dot(x);
                for (std::uint_fast8_t j = 1; j < fSize - 1; ++j)
                {
                    Eigen::Index const k = o + fIdx[j];
                    y_[k] = b[k] - A.col(k).dot(x);
                    float64_t const & A_kk = A(k, k);
                    if (A_kk > A_max)
                    {
                        A_max = A_kk;
                    }
                }
                e += y_[i0] / A_max;
                for (std::uint_fast8_t j = 1; j < fSize - 1; ++j)
                {
                    Eigen::Index const k = o + fIdx[j];
                    x[k] += y_[k] / A_max;
                }

                // Project the coefficient between lower and upper bounds
                auto xConst = x.segment(o, constraintData.dim);
                if (fSize == 1)
                {
                    e = std::clamp(e, lo, hi);
                }
                else
                {
                    float64_t const thr = hi * xConst[fIdx[fSize - 1]];
                    if (fSize == 2)
                    {
                        // Specialization for speedup and numerical stability
                        e = std::clamp(e, -thr, thr);
                    }
                    else
                    {
                        // Generic case
                        float64_t squaredNorm = e * e;
                        for (std::uint_fast8_t j = 1; j < fSize - 1; ++j)
                        {
                            float64_t const & f = xConst[fIdx[j]];
                            squaredNorm += f * f;
                        }
                        if (squaredNorm > thr * thr)
                        {
                            float64_t const scale = thr / std::sqrt(squaredNorm);
                            e *= scale;
                            for (std::uint_fast8_t j = 1; j < fSize - 1; ++j)
                            {
                                xConst[fIdx[j]] *= scale;
                            }
                        }
                    }
                }
            }
        }
    }

    bool_t PGSSolver::ProjectedGaussSeidelSolver(matrixN_t const & A,
                                                 vectorN_t::SegmentReturnType const & b,
                                                 vectorN_t::SegmentReturnType & x)
    {
        /* For some reason, it is impossible to get a better accuracy than 1e-5
           for the absolute tolerance, even if unconstrained. It seems to be
           related to compunding of errors, maybe due to the recursive computations. */

        assert(b.size() > 0 && "The number of inequality constraints must be larger than 0.");

        // Reset the residuals
        y_.setZero();

        // Perform multiple PGS loop until convergence or max iter reached
        for (uint32_t iter = 0; iter < maxIter_; ++iter)
        {
            // Backup previous residuals
            yPrev_ = y_;

            // Do a single iteration
            ProjectedGaussSeidelIter(A, b, x);

            // Check if terminate conditions are satisfied
            float64_t const tol = tolAbs_ + tolRel_ * y_.cwiseAbs().maxCoeff();
            if (((y_ - yPrev_).array().abs() < tol).all())
            {
                return true;
            }
        }

        // Impossible to converge
        return false;
    }

    bool_t PGSSolver::SolveBoxedForwardDynamics(float64_t const & inv_damping,
                                                bool_t const & ignoreBounds)
    {
        // Update constraints start indices, jacobian, drift and multipliers
        Eigen::Index constraintRows = 0U;
        for (auto & constraintData : constraintsData_)
        {
            AbstractConstraintBase * constraint = constraintData.constraint;
            constraintData.isInactive = !constraint->getIsEnabled();
            if (constraintData.isInactive)
            {
                continue;
            }
            Eigen::Index const constraintDim = constraintData.dim;
            J_.middleRows(constraintRows, constraintDim) = constraint->getJacobian();
            gamma_.segment(constraintRows, constraintDim) = constraint->getDrift();
            lambda_.segment(constraintRows, constraintDim) = constraint->lambda_;
            constraintData.startIdx = constraintRows;
            constraintRows += constraintDim;
        };

        // Extract active rows
        auto J = J_.topRows(constraintRows);
        auto lambda = lambda_.head(constraintRows);
        auto gamma = gamma_.head(constraintRows);
        auto b = b_.head(constraintRows);

        // Check if problem is bounded
        bool_t isUnbounded = std::all_of(
            constraintsData_.cbegin(), constraintsData_.cend(),
            [](ConstraintData const & constraintData){
                return constraintData.isInactive || constraintData.nBlocks == 0;
            });

        /* Compute JMinvJt, including cholesky decomposition of inertia matrix.
           Abort computation if the inertia matrix is not positive definite,
           which is never supposed to happen in theory but in practice it is
           not sure because of compounding of errors. */
        hresult_t returnCode = pinocchio_overload::computeJMinvJt(*model_, *data_, J);
        if (returnCode != hresult_t::SUCCESS)
        {
            data_->ddq.setConstant(qNAN);
            return false;
        }
        matrixN_t & A = data_->JMinvJt;

        /* Add regularization term in case A is not invertible.
           Note that Mujoco defines an impedance function that depends on
           the distance instead of a constant value to model soft contacts.
           See: - http://mujoco.org/book/modeling.html#CSolver
                - http://mujoco.org/book/computation.html#soParameters  */
        A.diagonal() += clamp(
            A.diagonal() * inv_damping,
            PGS_MIN_REGULARIZER,
            INF);

        // Compute the dynamic drift (control - nle)
        data_->torque_residual = data_->u - data_->nle;
        pinocchio::cholesky::solve(*model_, *data_, data_->torque_residual);

        /* Compute b.
           - TODO: Leverage sparsity of J to avoid dense matrix multiplication */
        b = - gamma;
        b.noalias() -= J * data_->torque_residual;

        // Compute resulting forces solving forward dynamics
        bool_t isSuccess = false;
        if (ignoreBounds || isUnbounded)
        {
            /* There is no inequality constraint, so the problem can be
               solved exactly and efficiently using cholesky decomposition.

               The implementation of this particular case is based on
               `pinocchio::forwardDynamics methods` without modification.
               See https://github.com/stack-of-tasks/pinocchio/blob/master/src/algorithm/contact-dynamics.hxx */

            // Compute the Lagrange Multipliers
            lambda = pinocchio_overload::solveJMinvJtv(*data_, b, true);

            // Always successful
            isSuccess = true;
        }
        else
        {
            // Full matrix is needed to enable vectorization
            A.triangularView<Eigen::StrictlyUpper>() = A.transpose();

            // Run standard PGS algorithm
            isSuccess = ProjectedGaussSeidelSolver(A, b, lambda);
        }

        // Update lagrangian multipliers associated with the constraint
        constraintRows = 0U;
        for (auto const & constraintData : constraintsData_)
        {
            AbstractConstraintBase * constraint = constraintData.constraint;
            if (!constraint->getIsEnabled())
            {
                continue;
            }
            Eigen::Index const constraintDim = static_cast<Eigen::Index>(constraint->getDim());
            constraint->lambda_ = lambda_.segment(constraintRows, constraintDim);
            constraintRows += constraintDim;
        };

        // Compute resulting acceleration, no matter if computing forces was successful
        data_->ddq.noalias() = J.transpose() * lambda;
        pinocchio::cholesky::solve(*model_, *data_, data_->ddq);
        data_->ddq += data_->torque_residual;

        return isSuccess;
    }
}
