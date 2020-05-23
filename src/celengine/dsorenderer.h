// dsorenderer.h
//
// Copyright (C) 2001-2020, the Celestia Development Team
// Original version by Chris Laurel <claurel@gmail.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <Eigen/Core>
#include <celmath/frustum.h>
#include "objectrenderer.h"

class DeepSkyObject;

class DSORenderer : public ObjectRenderer<DeepSkyObject*, double>
{
 public:
    DSORenderer();

    void process(DeepSkyObject* const &, double, float);

 public:
    Eigen::Vector3d     obsPos;
    Eigen::Matrix3f     orientationMatrix;
    celmath::Frustum    frustum         { 45.0_deg, 1.0f, 1.0f };
    DSODatabase*        dsoDB           { nullptr };

    int                 wWidth          { 0 };
    int                 wHeight         { 0 };
    double              avgAbsMag       { 0.0 };
    uint32_t            dsosProcessed   { 0 };
};
