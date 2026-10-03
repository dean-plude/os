/*
 * NovaOS: the part of the Windows SDK's <icu.h> (ICU's C API, served by
 * System32\icu.dll) that the STL's time-zone database (tzdb.cpp) calls.
 * The values are ICU's own.
 */
#pragma once
#include <float.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef char16_t UChar;
typedef int8_t UBool;
typedef double UDate;
typedef enum UErrorCode { U_ZERO_ERROR = 0, U_BUFFER_OVERFLOW_ERROR = 15 } UErrorCode;
#define U_FAILURE(x) ((x) > U_ZERO_ERROR)
#define U_SUCCESS(x) ((x) <= U_ZERO_ERROR)
#define U_DATE_MAX DBL_MAX
#define U_DATE_MIN (-DBL_MAX)

typedef void *UCalendar;
typedef struct UEnumeration UEnumeration;
typedef enum UCalendarType { UCAL_TRADITIONAL, UCAL_DEFAULT = UCAL_TRADITIONAL, UCAL_GREGORIAN } UCalendarType;
typedef enum UCalendarDisplayNameType { UCAL_STANDARD, UCAL_SHORT_STANDARD, UCAL_DST, UCAL_SHORT_DST } UCalendarDisplayNameType;
typedef enum UCalendarDateFields { UCAL_ZONE_OFFSET = 15, UCAL_DST_OFFSET = 16 } UCalendarDateFields;
typedef enum USystemTimeZoneType { UCAL_ZONE_TYPE_ANY, UCAL_ZONE_TYPE_CANONICAL, UCAL_ZONE_TYPE_CANONICAL_LOCATION } USystemTimeZoneType;
typedef enum UTimeZoneTransitionType {
    UCAL_TZ_TRANSITION_NEXT, UCAL_TZ_TRANSITION_NEXT_INCLUSIVE,
    UCAL_TZ_TRANSITION_PREVIOUS, UCAL_TZ_TRANSITION_PREVIOUS_INCLUSIVE
} UTimeZoneTransitionType;

void __cdecl ucal_close(UCalendar *cal);
int32_t __cdecl ucal_get(const UCalendar *cal, UCalendarDateFields field, UErrorCode *status);
int32_t __cdecl ucal_getCanonicalTimeZoneID(const UChar *id, int32_t len, UChar *result, int32_t resultCapacity,
    UBool *isSystemID, UErrorCode *status);
int32_t __cdecl ucal_getDefaultTimeZone(UChar *result, int32_t resultCapacity, UErrorCode *ec);
int32_t __cdecl ucal_getTimeZoneDisplayName(const UCalendar *cal, UCalendarDisplayNameType type, const char *locale,
    UChar *result, int32_t resultLength, UErrorCode *status);
UBool __cdecl ucal_getTimeZoneTransitionDate(const UCalendar *cal, UTimeZoneTransitionType type, UDate *transition,
    UErrorCode *status);
const char *__cdecl ucal_getTZDataVersion(UErrorCode *status);
UBool __cdecl ucal_inDaylightTime(const UCalendar *cal, UErrorCode *status);
UCalendar *__cdecl ucal_open(const UChar *zoneID, int32_t len, const char *locale, UCalendarType type, UErrorCode *status);
UEnumeration *__cdecl ucal_openTimeZoneIDEnumeration(USystemTimeZoneType zoneType, const char *region,
    const int32_t *rawOffset, UErrorCode *ec);
void __cdecl ucal_setMillis(UCalendar *cal, UDate dateTime, UErrorCode *status);
void __cdecl uenum_close(UEnumeration *en);
int32_t __cdecl uenum_count(UEnumeration *en, UErrorCode *status);
const UChar *__cdecl uenum_unext(UEnumeration *en, int32_t *resultLength, UErrorCode *status);
#ifdef __cplusplus
}
#endif
