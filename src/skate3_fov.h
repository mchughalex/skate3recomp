#pragma once

void Skate3InitializeFieldOfViewOverride();
void Skate3UpdateFieldOfViewOverride(double degrees);
float Skate3MaybeOverrideProjectionFovRadians(float native_radians);
