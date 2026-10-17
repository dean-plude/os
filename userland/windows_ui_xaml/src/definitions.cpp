// NovaOS: definitions MSVC makes from declarations in XAML's sources, which
// clang (as the C++ standard says) keeps as declarations.
//
// An explicit specialization of a static data member without an
// initializer, "template <> T C<X>::member;", declares it; MSVC defines it
// (default-initialized).  CDependencyObject's associative storage keeps its
// lookup table so (components/DependencyObject/dependencyobject.cpp); the
// definition with an initializer here is the one MSVC would make.
#include <CDependencyObject.h>
#include <ModifiedValue.h>
#include <ThemeResource.h>
#include <DOCollection.h>
#include <Framework.h>
#include "CDOAssociativeImpl.h"

namespace AssociativeStorage
{
    template <> Detail::VariableStorageBlock<Detail::Bitfield<CDOFields>>::OffsetLookupTable
        Detail::VariableStorageBlock<Detail::Bitfield<CDOFields>>::s_lookupTable{};
}
