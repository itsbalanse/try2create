#pragma once
#include "metadata.h"

// Given raw fields already collected into meta.rawFields, detects which
// generator produced the image and fills meta.generator / meta.parsed /
// meta.hasMetadata. meta.rawText should already be populated for display.
void InterpretRawFields(ImageMetadata& meta);
