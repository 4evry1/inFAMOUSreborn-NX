#include "system_profile.h"
#include <cstdint>

namespace {
bool is_utf8(const char *text,std::size_t length) {
    std::size_t offset=0;
    while(offset<length) {
        const auto first=static_cast<unsigned char>(text[offset++]);
        if(first<0x80) continue;
        unsigned remaining=0;
        uint32_t scalar=0,minimum=0;
        if(first>=0xc2 && first<=0xdf) { remaining=1; scalar=first&0x1f; minimum=0x80; }
        else if(first>=0xe0 && first<=0xef) { remaining=2; scalar=first&0x0f; minimum=0x800; }
        else if(first>=0xf0 && first<=0xf4) { remaining=3; scalar=first&7; minimum=0x10000; }
        else return false;
        if(remaining>length-offset) return false;
        for(unsigned i=0;i<remaining;++i) {
            const auto next=static_cast<unsigned char>(text[offset++]);
            if((next&0xc0)!=0x80) return false;
            scalar=(scalar<<6)|(next&0x3f);
        }
        if(scalar<minimum || scalar>0x10ffff || (scalar>=0xd800 && scalar<=0xdfff)) return false;
    }
    return true;
}
}

SystemProfileTextValidation validate_system_profile_text(const SystemProfile &profile,SystemProfileField field) {
    if(profile.source==SystemProfileSource::Unconfigured) return {};
    if(profile.source!=SystemProfileSource::NativeApplicationProfile) return {SystemProfileStatus::InvalidSource,0};
    const char *text=nullptr;
    std::size_t capacity=0;
    switch(field) {
    case SystemProfileField::CurrentUserDisplayName:
        text=profile.current_user_display_name; capacity=sizeof profile.current_user_display_name; break;
    case SystemProfileField::DeviceDisplayLabel:
        text=profile.device_display_label; capacity=sizeof profile.device_display_label; break;
    default: return {SystemProfileStatus::InvalidField,0};
    }
    std::size_t length=0;
    while(length<capacity && text[length]) ++length;
    if(length==capacity) return {SystemProfileStatus::Unterminated,0};
    if(!is_utf8(text,length)) return {SystemProfileStatus::InvalidUtf8,0};
    return {SystemProfileStatus::Valid,length};
}
