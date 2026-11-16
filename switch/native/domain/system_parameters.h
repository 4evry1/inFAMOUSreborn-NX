#ifndef NATIVE_SYSTEM_PARAMETERS_H
#define NATIVE_SYSTEM_PARAMETERS_H
#include "system_language.h"

enum class SystemParameterProfile { Unavailable, NativePortConventions };
enum class SystemLanguageCompatibility { Unconfigured, NativePortLegacyFamilies };
enum class SystemConfirmButton : uint32_t { Circle=0, Cross=1 };
enum class SystemDateFormat : uint32_t { YearMonthDay=0, DayMonthYear=1, MonthDayYear=2 };
enum class SystemTimeFormat : uint32_t { TwelveHour=0, TwentyFourHour=1 };

// Native profile conventions are independent of measured Horizon language.
// parental_level describes this port's PS3 profile, never Switch parental settings.
struct SystemParameters {
    SystemLanguageSnapshot language{};
    SystemLanguageCompatibility language_compatibility=SystemLanguageCompatibility::Unconfigured;
    SystemParameterProfile profile=SystemParameterProfile::Unavailable;
    SystemConfirmButton confirm_button=SystemConfirmButton::Cross;
    SystemDateFormat date_format=SystemDateFormat::YearMonthDay;
    SystemTimeFormat time_format=SystemTimeFormat::TwentyFourHour;
    uint32_t parental_level=0;
};
#endif
