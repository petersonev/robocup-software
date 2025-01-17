#include "RRTPlanner.hpp"
#include "EscapeObstaclesPathPlanner.hpp"
#include <Constants.hpp>
#include <Utils.hpp>
#include <protobuf/LogFrame.pb.h>
#include "motion/TrapezoidalMotion.hpp"
#include "Util.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <algorithm>
#include <Eigen/Dense>

using namespace std;
using namespace Eigen;
using namespace Geometry2d;

namespace Planning {

RRTPlanner::RRTPlanner(int maxIterations) : _maxIterations(maxIterations) {}

bool RRTPlanner::shouldReplan(MotionInstant start, MotionInstant goal,
                              const MotionConstraints& motionConstraints,
                              const Geometry2d::ShapeSet* obstacles,
                              const Path* prevPath) const {
    if (SingleRobotPathPlanner::shouldReplan(start, motionConstraints,
                                             obstacles, prevPath)) {
        return true;
    }

    // if the destination of the current path is greater than X m away
    // from the target destination, we invalidate the path. This
    // situation could arise if the path destination changed.
    float goalPosDiff = (prevPath->end().motion.pos - goal.pos).mag();
    float goalVelDiff = (prevPath->end().motion.vel - goal.vel).mag();
    if (goalPosDiff > goalChangeThreshold() ||
        goalVelDiff > goalChangeThreshold()) {
        // FIXME: goalChangeThreshold shouldn't be used for velocities as it
        // is above
        return true;
    }

    return false;
}

std::unique_ptr<Path> RRTPlanner::run(
    MotionInstant start, const MotionCommand* cmd,
    const MotionConstraints& motionConstraints,
    const Geometry2d::ShapeSet* obstacles, std::unique_ptr<Path> prevPath) {
    // This planner only works with commands of type 'PathTarget'
    assert(cmd->getCommandType() == Planning::MotionCommand::PathTarget);
    Planning::PathTargetCommand target =
        *static_cast<const Planning::PathTargetCommand*>(cmd);

    MotionInstant goal = target.pathGoal;

    // Simple case: no path
    if (start.pos == goal.pos) {
        InterpolatedPath* path = new InterpolatedPath();
        path->setStartTime(RJ::timestamp());
        path->waypoints.emplace_back(
            MotionInstant(start.pos, Geometry2d::Point()), 0);
        return unique_ptr<Path>(path);
    }

    // Locate a goal point that is obstacle-free
    boost::optional<Geometry2d::Point> prevGoal;
    if (prevPath) prevGoal = prevPath->end().motion.pos;
    goal.pos = EscapeObstaclesPathPlanner::findNonBlockedGoal(
        goal.pos, prevGoal, *obstacles);

    // Replan if needed, otherwise return the previous path unmodified
    if (shouldReplan(start, goal, motionConstraints, obstacles,
                     prevPath.get())) {
        // Run bi-directional RRT to generate a path.
        auto points = runRRT(start, goal, motionConstraints, obstacles);

        // Optimize out uneccesary waypoints
        optimize(points, obstacles, motionConstraints, start.vel, goal.vel);

        // Check if Planning or optimization failed
        if (points.size() < 2) {
            debugLog("PathPlanning Failed");
            auto path = make_unique<InterpolatedPath>();
            path->waypoints.emplace_back(MotionInstant(start.pos, Point()), 0);
            path->waypoints.emplace_back(MotionInstant(start.pos, Point()), 0);
            return std::move(path);
        }

        // Generate and return a cubic bezier path using the waypoints
        return generateCubicBezier(points, *obstacles, motionConstraints,
                                   start.vel, goal.vel);
    } else {
        return prevPath;
    }
}

vector<Point> RRTPlanner::runRRT(MotionInstant start, MotionInstant goal,
                                 const MotionConstraints& motionConstraints,
                                 const Geometry2d::ShapeSet* obstacles) {
    unique_ptr<InterpolatedPath> path = make_unique<InterpolatedPath>();

    // Initialize two RRT trees
    FixedStepTree startTree;
    FixedStepTree goalTree;
    startTree.init(start.pos, obstacles);
    goalTree.init(goal.pos, obstacles);
    startTree.step = goalTree.step = .15f;

    // Run bi-directional RRT algorithm
    Tree* ta = &startTree;
    Tree* tb = &goalTree;
    for (unsigned int i = 0; i < _maxIterations; ++i) {
        Geometry2d::Point r = RandomFieldLocation();

        Tree::Point* newPoint = ta->extend(r);

        if (newPoint) {
            // try to connect the other tree to this point
            if (tb->connect(newPoint->pos)) {
                // trees connected
                // done with global path finding
                // the path is from start to goal
                // runRRT will handle the rest
                break;
            }
        }

        swap(ta, tb);
    }

    Tree::Point* p0 = startTree.last();
    Tree::Point* p1 = goalTree.last();

    vector<Point> points;
    // sanity check
    if (!p0 || !p1 || p0->pos != p1->pos) {
        return points;
    }

    // extract path from RRTs
    // add the start tree first...normal order (aka from root to p0)
    startTree.addPath(points, p0);
    // add the goal tree in reverse (aka p1 to root)
    goalTree.addPath(points, p1, true);

    return points;
}

void RRTPlanner::optimize(vector<Geometry2d::Point>& pts,
                          const Geometry2d::ShapeSet* obstacles,
                          const MotionConstraints& motionConstraints,
                          Geometry2d::Point vi, Geometry2d::Point vf) {
    unsigned int start = 0;

    if (pts.size() < 2) {
        return;
    }

    // The set of obstacles the starting point was inside of
    const auto startHitSet = obstacles->hitSet(pts[start]);
    int span = 2;
    while (span < pts.size()) {
        bool changed = false;
        for (int i = 0; i + span < pts.size(); i++) {
            bool transitionValid = true;
            const auto newHitSet =
                obstacles->hitSet(Geometry2d::Segment(pts[i], pts[i + span]));
            if (!newHitSet.empty()) {
                for (std::shared_ptr<Geometry2d::Shape> hit : newHitSet) {
                    if (startHitSet.find(hit) == startHitSet.end()) {
                        transitionValid = false;
                        break;
                    }
                }
            }

            if (transitionValid) {
                for (int x = 1; x < span; x++) {
                    pts.erase(pts.begin() + i + 1);
                }
                changed = true;
            }
        }

        if (!changed) span++;
    }

    return;
}
float getTime(vector<Point> path, int index,
              const MotionConstraints& motionConstraints, float startSpeed,
              float endSpeed) {
    float length = 0;
    float startLength = 0;
    for (int i = 1; i < path.size(); i++) {
        length += path[i - 1].distTo(path[i]);
        if (index == i) {
            startLength = length;
        }
    }
    return Trapezoidal::getTime(startLength, length, motionConstraints.maxSpeed,
                                motionConstraints.maxAcceleration, startSpeed,
                                endSpeed);
}
float getTime(InterpolatedPath& path, int index,
              const MotionConstraints& motionConstraints, float startSpeed,
              float endSpeed) {
    return Trapezoidal::getTime(
        path.length(0, index), path.length(), motionConstraints.maxSpeed,
        motionConstraints.maxAcceleration, startSpeed, endSpeed);
}

std::unique_ptr<InterpolatedPath> RRTPlanner::generatePath(
    const std::vector<Geometry2d::Point>& points,
    const Geometry2d::ShapeSet& obstacles,
    const MotionConstraints& motionConstraints, Geometry2d::Point vi,
    Geometry2d::Point vf) {
    return generateCubicBezier(points, obstacles, motionConstraints, vi, vf);
}

vector<CubicBezierControlPoints> RRTPlanner::generateNormalCubicBezierPath(
    const vector<Geometry2d::Point>& points,
    const MotionConstraints& motionConstraints, Geometry2d::Point vi,
    Geometry2d::Point vf) {
    size_t length = points.size();
    size_t curvesNum = length - 1;

    const float directionDistance = 0.3;
    vector<Point> startDirections;
    vector<Point> endDirections;

    const float pathWeight = 1.0;
    Point pathDirection = (points[1] - points[0]).normalized(pathWeight);
    startDirections.push_back(
        (vi + pathDirection)
            .normalized((points[1] - points[0]).mag() * directionDistance));
    for (int i = 1; i < curvesNum - 1; i++) {
        const Point difference = points[i + 1] - points[i - 1];
        endDirections.push_back(difference.normalized(
            (points[i] - points[i - 1]).mag() * directionDistance));
        startDirections.push_back(difference.normalized(
            (points[i] - points[i + 1]).mag() * directionDistance));
    }
    Point endPathDirection = (points[points.size() - 1] -
                              points[points.size() - 2]).normalized(pathWeight);
    endDirections.push_back((vf + endPathDirection)
                                .normalized((points[points.size() - 1] -
                                             points[points.size() - 2]).mag() *
                                            directionDistance));

    vector<CubicBezierControlPoints> path;

    for (int i = 0; i < curvesNum; i++) {
        Point p0 = points[i];
        Point p3 = points[i + 1];
        Point p1 = p0 + startDirections[i];
        Point p2 = p3 - endDirections[i];
        path.emplace_back(p0, p1, p2, p3);
    }
    return path;
}

vector<CubicBezierControlPoints> RRTPlanner::generateCubicBezierPath(
    const vector<Geometry2d::Point>& points,
    const MotionConstraints& motionConstraints, Geometry2d::Point vi,
    Geometry2d::Point vf, const boost::optional<vector<float>>& times) {
    size_t length = points.size();
    size_t curvesNum = length - 1;
    vector<double> pointsX(length);
    vector<double> pointsY(length);
    vector<double> ks(length - 1);
    vector<double> ks2(length - 1);

    for (int i = 0; i < length; i++) {
        pointsX[i] = points[i].x;
        pointsY[i] = points[i].y;
    }
    const float startSpeed = vi.mag();

    const float endSpeed = vf.mag();

    if (times) {
        assert(times->size() == points.size());
        for (int i = 0; i < curvesNum; i++) {
            ks[i] = 1.0 / (times->at(i + 1) - times->at(i));
            ks2[i] = ks[i] * ks[i];
            if (std::isnan(ks[i])) {
                debugThrow(
                    "Something went wrong. Points are too close to each other "
                    "probably");
                return vector<CubicBezierControlPoints>();
            }
        }
    } else {
        for (int i = 0; i < curvesNum; i++) {
            ks[i] = 1.0 / (getTime(points, i + 1, motionConstraints, startSpeed,
                                   endSpeed) -
                           getTime(points, i, motionConstraints, startSpeed,
                                   endSpeed));
            ks2[i] = ks[i] * ks[i];
            if (std::isnan(ks[i])) {
                debugThrow(
                    "Something went wrong. Points are too close to each other "
                    "probably");
                return vector<CubicBezierControlPoints>();
            }
        }
    }

    VectorXd solutionX =
        RRTPlanner::cubicBezierCalc(vi.x, vf.x, pointsX, ks, ks2);
    VectorXd solutionY =
        RRTPlanner::cubicBezierCalc(vi.y, vf.y, pointsY, ks, ks2);

    vector<CubicBezierControlPoints> path;

    for (int i = 0; i < curvesNum; i++) {
        Point p0 = points[i];
        Point p1 = Geometry2d::Point(solutionX(i * 2), solutionY(i * 2));
        Point p2 =
            Geometry2d::Point(solutionX(i * 2 + 1), solutionY(i * 2 + 1));
        Point p3 = points[i + 1];
        path.emplace_back(p0, p1, p2, p3);
    }
    return path;
}

float oneStepLimitAcceleration(float maxAceleration, float d1, float v1,
                               float c1, float d2, float v2, float c2) {
    float d = d2 - d1;
    float deltaSpeed = v2 - v1;
    if (deltaSpeed < 0) {
        return v2;
    }

    // Isolated maxSpeed based on Curvature should already be taken care of
    float c = max(c1, c2);
    float a = maxAceleration;

    // newPointsSpeed[i] = std::min(v2, std::sqrt(a * d * 2 + v1 * v1));
    // continue;
    // acceleration = (v2-v1)/t;
    // t = distance/((v1+v2)/2)
    // acceleration = (v2-v1)/(distance/((v1+v2)/2))
    // acceleration = (v2-v1)(v1+v2)/2)/distance
    // acceleration^2 = ((v2-v1)((v1+v2)/2)/(distance))^2 + (v^2*curvature)^2
    // a^2 = ((b-v)((v+b)/2)/(d))^2 + (b^2*c)^2
    // b = ±sqrt((v^2-2 sqrt(d^2 (4 a^2 c^2 d^2+a^2-c^2 v^4)))/(4 c^2 d^2+1))
    // and 4 c^2 d^2+1!=0 and d!=0
    // http://www.wolframalpha.com/input/?i=solve+for+b+where+a%5E2+%3D+%28%28b-v%29%28%28v%2Bb%29%2F2%29%2F%28d%29%29%5E2+%2B+%28b%5E2*c%29%5E2
    float vPossible1 = sqrt((v1 * v1 -
                             2 * sqrt(d * d * (4 * a * a * c * c * d * d +
                                               a * a - c * c * pow(v1, 4)))) /
                            (4 * c * c * d * d + 1));

    // b = ±sqrt((2 sqrt(d^2 (4 a^2 c^2 d^2+a^2-c^2 v^4))+v^2)/(4 c^2 d^2+1))
    // and 4 c^2 d^2+1!=0 and d!=0
    float vPossible2 = sqrt((2 * sqrt(d * d * (4 * a * a * c * c * d * d +
                                               a * a - c * c * pow(v1, 4))) +
                             v1 * v1) /
                            (4 * c * c * d * d + 1));

    float maxSpeed;
    if (isnan(vPossible1) && isnan(vPossible2)) {
        maxSpeed = std::sqrt(a * d * 2 + v1 * v1);
    } else {
        if (isnan(vPossible1)) {
            maxSpeed = vPossible2;
        } else if (isnan(vPossible2)) {
            maxSpeed = vPossible1;
        } else {
            maxSpeed = max(vPossible1, vPossible2);
        }
    }
    return std::min(v2, maxSpeed);
}

/**
 * Generates a Cubic Bezier Path based on Albert's random Bezier Velocity Path
 * Algorithm
 */
std::vector<InterpolatedPath::Entry> RRTPlanner::generateVelocityPath(
    const std::vector<CubicBezierControlPoints>& controlPoints,
    const MotionConstraints& motionConstraints, Geometry2d::Point vi,
    Geometry2d::Point vf, int interpolations) {
    // Interpolate Through Bezier Path
    vector<Point> newPoints, newPoints1stDerivative, newPoints2ndDerivative;
    vector<float> newPointsCurvature, newPointsDistance, newPointsSpeed;

    float totalDistance = 0;
    const float maxAceleration = motionConstraints.maxAcceleration;

    for (const CubicBezierControlPoints& controlPoint : controlPoints) {
        Point p0 = controlPoint.p0;
        Point p1 = controlPoint.p1;
        Point p2 = controlPoint.p2;
        Point p3 = controlPoint.p3;
        for (int j = 0; j < interpolations; j++) {
            float t = (((float)j / (float)(interpolations)));
            Geometry2d::Point pos =
                pow(1.0 - t, 3) * p0 + 3.0 * pow(1.0 - t, 2) * t * p1 +
                3 * (1.0 - t) * pow(t, 2) * p2 + pow(t, 3) * p3;

            // Derivitive 1
            // 3 k (-(A (-1 + k t)^2) + k t (2 C - 3 C k t + D k t) + B (1 - 4 k
            // t + 3 k^2 t^2))
            Geometry2d::Point d1 = 3 * pow(1 - t, 2) * (p1 - p0) +
                                   6 * (1 - t) * t * (p2 - p1) +
                                   3 * pow(t, 2) * (p3 - p2);

            // Derivitive 2
            // https://en.wikipedia.org/wiki/B%C3%A9zier_curve#Cubic_B.C3.A9zier_curves
            // B''(t) = 6(1-t)(P2 - 2*P1 + P0) + 6*t(P3 - 2 * P2 + P1)
            Geometry2d::Point d2 =
                6 * (1 - t) * (p2 - 2 * p1 + p0) + 6 * t * (p3 - 2 * p2 + p1);

            // https://en.wikipedia.org/wiki/Curvature#Local_expressions
            // K = |x'*y'' - y'*x''| / (x'^2 + y'^2)^(3/2)
            float curvature =
                std::abs(d1.x * d2.y - d1.y * d2.x) /
                std::pow(std::pow(d1.x, 2) + std::pow(d1.y, 2), 1.5);

            // Handle 0 velocity case
            if (isnan(curvature)) {
                curvature = 0;
            }

            assert(curvature >= 0);
            if (!newPoints.empty()) {
                float distance = pos.distTo(newPoints.back());
                totalDistance += distance;
            }
            newPointsDistance.push_back(totalDistance);

            newPoints.push_back(pos);
            newPoints1stDerivative.push_back(d1);
            newPoints2ndDerivative.push_back(d2);

            newPointsCurvature.push_back(curvature);

            // Isolated maxSpeed based on Curvature
            // Curvature = 1/Radius of Curvature
            // vmax = sqrt(acceleartion/abs(Curvature))

            float constantMaxSpeed = std::sqrt(maxAceleration / curvature);
            newPointsSpeed.push_back(constantMaxSpeed);
        }
    }
    // Get last point in Path
    CubicBezierControlPoints lastControlPoint = controlPoints.back();

    Point p0 = lastControlPoint.p0;
    Point p1 = lastControlPoint.p1;
    Point p2 = lastControlPoint.p2;
    Point p3 = lastControlPoint.p3;

    Point pos = p3;
    Point d1 = vf;
    Geometry2d::Point d2 = 6 * (1) * (p3 - 2 * p2 + p1);
    float curvature = std::abs(d1.x * d2.y - d1.y * d2.x) /
                      std::pow(std::pow(d1.x, 2) + std::pow(d1.y, 2), 1.5);

    // handle 0 velcoity case
    if (isnan(curvature)) {
        curvature = 0;
    }

    totalDistance += pos.distTo(newPoints.back());
    newPoints.push_back(pos);
    newPoints1stDerivative.push_back(vf);
    newPoints2ndDerivative.push_back(d2);
    newPointsCurvature.push_back(curvature);

    newPointsDistance.push_back(totalDistance);

    newPointsSpeed[0] = vi.mag();
    newPointsSpeed.push_back(vf.mag());

    // Velocity Profile Generation
    // Forward Smoothing
    const float size = newPoints.size();
    assert(size == newPoints.size());
    assert(size == newPoints1stDerivative.size());
    assert(size == newPoints2ndDerivative.size());
    assert(size == newPointsDistance.size());
    assert(size == newPointsSpeed.size());
    assert(size == newPointsCurvature.size());

    // Generate Velocity Profile from Interpolation of Bezier Path
    // Forward Constraints

    for (int i = 1; i < size; i++) {
        int i1 = i - 1;
        int i2 = i;
        newPointsSpeed[i2] = oneStepLimitAcceleration(
            maxAceleration, newPointsDistance[i1], newPointsSpeed[i1],
            newPointsCurvature[i1], newPointsDistance[i2], newPointsSpeed[i2],
            newPointsCurvature[i2]);
    }

    // Backwards Constraints
    for (int i = size - 2; i >= 0; i--) {
        int i1 = i + 1;
        int i2 = i;
        newPointsSpeed[i2] = oneStepLimitAcceleration(
            maxAceleration, newPointsDistance[i1], newPointsSpeed[i1],
            newPointsCurvature[i1], newPointsDistance[i2], newPointsSpeed[i2],
            newPointsCurvature[i2]);
    }

    float totalTime = 0;
    vector<InterpolatedPath::Entry> entries;

    for (int i = 0; i < size; i++) {
        float currentSpeed = newPointsSpeed[i];
        float distance = newPointsDistance[i];
        if (i != 0) {
            distance -= newPointsDistance[i - 1];
            float averageSpeed = (currentSpeed + newPointsSpeed[i - 1]) / 2.0;
            float deltaT = distance / averageSpeed;
            totalTime += deltaT;
        }

        Point point = newPoints[i];
        Point vel = newPoints1stDerivative[i].normalized();
        entries.emplace_back(MotionInstant(point, vel * currentSpeed),
                             totalTime);
    }
    return entries;
}

std::unique_ptr<Planning::InterpolatedPath> RRTPlanner::generateCubicBezier(
    const std::vector<Geometry2d::Point>& points,
    const Geometry2d::ShapeSet& obstacles,
    const MotionConstraints& motionConstraints, Geometry2d::Point vi,
    Geometry2d::Point vf) {
    const int interpolations = 40;

    size_t length = points.size();
    size_t curvesNum = length - 1;
    if (curvesNum <= 0) {
        debugThrow("The path doesn't have enough points");
        return nullptr;
    }

    vector<CubicBezierControlPoints> controlPoints =
        generateCubicBezierPath(points, motionConstraints, vi, vf);

    vector<InterpolatedPath::Entry> entries = generateVelocityPath(
        controlPoints, motionConstraints, vi, vf, interpolations);

    std::unique_ptr<InterpolatedPath> path = make_unique<InterpolatedPath>();
    path->waypoints = entries;
    path->setStartTime(RJ::timestamp());
    return path;
}

VectorXd RRTPlanner::cubicBezierCalc(double vi, double vf,
                                     vector<double>& points, vector<double>& ks,
                                     vector<double>& ks2) {
    int curvesNum = points.size() - 1;

    if (curvesNum == 1) {
        VectorXd vector(2);
        vector[0] = vi / (3.0 * ks[0]) + points[0];
        vector[1] = points[curvesNum] - vf / (3 * ks[curvesNum - 1]);
        return vector;
    } else {
        int matrixSize = curvesNum * 2;
        MatrixXd equations = MatrixXd::Zero(matrixSize, matrixSize);
        VectorXd answer(matrixSize);
        equations(0, 0) = 1;
        answer(0) = vi / (3.0 * ks[0]) + points[0];
        equations(1, matrixSize - 1) = 1;
        answer(1) = points[curvesNum] - vf / (3 * ks[curvesNum - 1]);

        int i = 2;
        for (int n = 0; n < curvesNum - 1; n++) {
            equations(i, n * 2 + 1) = ks[n];
            equations(i, n * 2 + 2) = ks[n + 1];
            answer(i) = (ks[n] + ks[n + 1]) * points[n + 1];
            i++;
        }

        for (int n = 0; n < curvesNum - 1; n++) {
            equations(i, n * 2) = ks2[n];
            equations(i, n * 2 + 1) = -2 * ks2[n];
            equations(i, n * 2 + 2) = 2 * ks2[n + 1];
            equations(i, n * 2 + 3) = -ks2[n + 1];
            answer(i) = points[n + 1] * (ks2[n + 1] - ks2[n]);
            i++;
        }

        ColPivHouseholderQR<MatrixXd> solver(equations);
        VectorXd solution = solver.solve(answer);
        return solution;
    }
}

}  // namespace Planning
