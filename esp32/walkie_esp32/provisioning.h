#pragma once
#include "settings.h"

// Called before I2S/audio/network tasks start. Returns immutable operational settings.
// Opens setup if nothing valid is saved or the PTT boot gesture was requested.
bool configureWalkie(walkie::Settings &settings, bool forceSetup);
