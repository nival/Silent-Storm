/*
 *  rtti_compat.cpp -- dynamic_cast from a pointer of unknown static type.
 *
 *  The engine keeps object pointers as void* (command contexts) and as
 *  pointers to forward-declared classes, and later does
 *      dynamic_cast<X*>( (CObjectBase*)p )
 *  On MSVC that works because its RTTI locates the complete object from
 *  whatever vptr sits at p.  Under the Itanium ABI __dynamic_cast wants p to
 *  really be a CObjectBase subobject; CObjectBase is a *virtual* base in this
 *  engine, so it sits at the end of the object, and the cast returns null.
 *
 *  This helper does what MSVC did: read the vptr at p, take the offset-to-top
 *  and the type_info the vtable carries (Itanium vtable layout: [-2] and
 *  [-1]) to reach the complete object and its exact type, then walk that
 *  type's base-class descriptors (Itanium C++ ABI 2.9.5: __si_class_type_info
 *  and __vmi_class_type_info) down to the requested destination, applying
 *  the recorded offsets -- non-virtual bases at a fixed offset, virtual bases
 *  through the vbase-offset slot of the current subobject's vtable.
 *
 *  libc++abi's own __dynamic_cast is not usable for the last step: it is only
 *  meant for casts whose static type differs from the dynamic type, and does
 *  not descend below a static type that equals the complete type.
 */
#include <typeinfo>
#include <cstddef>
#include <cstring>

namespace
{

/*  Layouts from the Itanium C++ ABI, section 2.9.5. */
struct SBaseClassInfo
{
    const std::type_info *pBaseType;
    long                  nOffsetFlags;      /* offset << 8 | flags */
};
enum { BASE_VIRTUAL = 0x1, BASE_PUBLIC = 0x2, OFFSET_SHIFT = 8 };

struct SSiClassTypeInfo                       /* single, public, non-virtual base at offset 0 */
{
    const void           *vptr;
    const char           *pszName;
    const std::type_info *pBaseType;
};
struct SVmiClassTypeInfo                      /* everything else with bases */
{
    const void    *vptr;
    const char    *pszName;
    unsigned       nFlags;
    unsigned       nBaseCount;
    SBaseClassInfo bases[ 1 ];
};

const void *Walk( const void *pObj, const std::type_info *pType, const std::type_info &dst )
{
    if ( *pType == dst )
        return pObj;
    /* which kind of class descriptor is this?  Its own dynamic type says. */
    const char *pszKind = typeid( *pType ).name();
    if ( strstr( pszKind, "__si_class_type_info" ) )
    {
        const SSiClassTypeInfo *pSi = reinterpret_cast< const SSiClassTypeInfo * >( pType );
        return Walk( pObj, pSi->pBaseType, dst );
    }
    if ( strstr( pszKind, "__vmi_class_type_info" ) )
    {
        const SVmiClassTypeInfo *pVmi = reinterpret_cast< const SVmiClassTypeInfo * >( pType );
        for ( unsigned i = 0; i < pVmi->nBaseCount; ++i )
        {
            const SBaseClassInfo &b = pVmi->bases[ i ];
            if ( !( b.nOffsetFlags & BASE_PUBLIC ) )
                continue;
            const long nOffset = b.nOffsetFlags >> OFFSET_SHIFT;
            const char *pBase;
            if ( b.nOffsetFlags & BASE_VIRTUAL )
            {
                /* nOffset indexes the vbase-offset slot in this subobject's vtable */
                const char *pVTable = *static_cast< const char *const * >( pObj );
                const std::ptrdiff_t nVBaseOffset = *reinterpret_cast< const std::ptrdiff_t * >( pVTable + nOffset );
                pBase = static_cast< const char * >( pObj ) + nVBaseOffset;
            }
            else
                pBase = static_cast< const char * >( pObj ) + nOffset;
            if ( const void *pFound = Walk( pBase, b.pBaseType, dst ) )
                return pFound;
        }
    }
    return 0;   /* __class_type_info: no bases */
}

}  // namespace

extern "C++" void *a5_dynamic_cast_from_opaque( const void *p, const std::type_info &dst )
{
    if ( !p )
        return 0;
    void *const *vtable = *static_cast< void *const *const * >( p );
    const std::ptrdiff_t nOffsetToTop = reinterpret_cast< std::ptrdiff_t >( vtable[ -2 ] );
    const std::type_info *pDynamic = static_cast< const std::type_info * >( vtable[ -1 ] );
    const void *pComplete = static_cast< const char * >( p ) + nOffsetToTop;
    return const_cast< void * >( Walk( pComplete, pDynamic, dst ) );
}
