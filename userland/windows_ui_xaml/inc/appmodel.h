/* NovaOS: MinGW-w64's appmodel.h, where PACKAGE_INFO_REFERENCE is the
 * handle's struct itself; the Windows SDK's is a pointer to it, the handle
 * type WIL's unique_package_info_reference (wil/resource.h) takes as a
 * template argument.  XAML's sources do not use that wrapper, so WIL is told
 * it is already defined. */
#pragma once
#include_next <appmodel.h>
#ifndef __WIL_APPMODEL_H_
#define __WIL_APPMODEL_H_
#define __WIL_APPMODEL_H_STL
#endif

/* (the SDK's longest application user model ID, which MinGW-w64's lacks;
   XAML's trace session sizes its buffer with it) */
#ifndef APPLICATION_USER_MODEL_ID_MAX_LENGTH
#define APPLICATION_USER_MODEL_ID_MAX_LENGTH 130
#endif
