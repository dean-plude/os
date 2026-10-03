/*
 * xapo.h — XAPO effects between programs and FAudio
 *
 * XAudio2 takes effects as XAPO COM objects (IXAPO, IXAPOParameters);
 * FAudio takes them as FAPO structures of function pointers.  A program's
 * own XAPO is wrapped in an FAPO for FAudio, and FAudio's built-in effects
 * (reverb, volume meter, FAPOFX) are handed out wrapped in an XAPO.
 */

#pragma once

#include <windows.h>
#include <objbase.h>
#include "FAudio.h"
#include "FAPO.h"

/* An FAPO for the XAPO @u (a wrapped built-in comes back unwrapped); the
 * caller holds one reference to it and releases it with fapo->Release */
FAPO *xapo_to_fapo(IUnknown *u);
/* A new XAPO (one reference) wrapping @f, taking over the caller's reference */
HRESULT fapo_to_xapo(FAPO *f, IUnknown **out);
