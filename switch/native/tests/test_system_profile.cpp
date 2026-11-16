#include "../domain/system_profile.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <type_traits>

static_assert(std::is_trivially_copyable<SystemProfile>::value);
static SystemProfile configured() {
    SystemProfile profile{}; profile.source=SystemProfileSource::NativeApplicationProfile;
    std::memcpy(profile.current_user_display_name,"Player",7);
    std::memcpy(profile.device_display_label,"Nintendo Switch",16);
    return profile;
}
static void sources_and_fields() {
    auto profile=configured();
    auto user=validate_system_profile_text(profile,SystemProfileField::CurrentUserDisplayName);
    auto device=validate_system_profile_text(profile,SystemProfileField::DeviceDisplayLabel);
    assert(user.status==SystemProfileStatus::Valid && user.length==6);
    assert(device.status==SystemProfileStatus::Valid && device.length==15);
    assert(validate_system_profile_text(SystemProfile{},SystemProfileField::CurrentUserDisplayName).status==SystemProfileStatus::Unconfigured);
    profile.source=static_cast<SystemProfileSource>(99);
    assert(validate_system_profile_text(profile,SystemProfileField::CurrentUserDisplayName).status==SystemProfileStatus::InvalidSource);
    profile=configured();
    assert(validate_system_profile_text(profile,static_cast<SystemProfileField>(99)).status==SystemProfileStatus::InvalidField);
    std::memset(profile.device_display_label,0xff,sizeof profile.device_display_label);
    assert(validate_system_profile_text(profile,SystemProfileField::CurrentUserDisplayName).status==SystemProfileStatus::Valid);
    assert(validate_system_profile_text(profile,SystemProfileField::DeviceDisplayLabel).status==SystemProfileStatus::Unterminated);
}
static void boundaries_and_immutability() {
    for(auto field:{SystemProfileField::CurrentUserDisplayName,SystemProfileField::DeviceDisplayLabel}) {
        auto profile=configured();
        char *text=field==SystemProfileField::CurrentUserDisplayName?profile.current_user_display_name:profile.device_display_label;
        const auto capacity=field==SystemProfileField::CurrentUserDisplayName?sizeof profile.current_user_display_name:sizeof profile.device_display_label;
        std::memset(text,'x',capacity); text[0]=0;
        auto result=validate_system_profile_text(profile,field);
        assert(result.status==SystemProfileStatus::Valid && result.length==0);
        std::memset(text,'x',capacity); text[capacity-1]=0;
        result=validate_system_profile_text(profile,field);
        assert(result.status==SystemProfileStatus::Valid && result.length==capacity-1);
        text[capacity-1]='x';
        assert(validate_system_profile_text(profile,field).status==SystemProfileStatus::Unterminated);
        text[0]='a'; text[1]=0; text[2]=static_cast<char>(0xff);
        const auto before=profile;
        result=validate_system_profile_text(profile,field);
        assert(result.status==SystemProfileStatus::Valid && result.length==1);
        assert(std::memcmp(&profile,&before,sizeof profile)==0);
        std::memset(text,'x',capacity);
        text[capacity-4]=static_cast<char>(0xe2); text[capacity-3]=static_cast<char>(0x82);
        text[capacity-2]=static_cast<char>(0xac); text[capacity-1]=0;
        result=validate_system_profile_text(profile,field);
        assert(result.status==SystemProfileStatus::Valid && result.length==capacity-1);
        text[capacity-2]=0;
        assert(validate_system_profile_text(profile,field).status==SystemProfileStatus::InvalidUtf8);
    }
}
static void unicode_scalars() {
    const char *valid[]={"", "ASCII", "\xc2\x80", "\xdf\xbf", "\xe0\xa0\x80", "\xed\x9f\xbf",
        "\xee\x80\x80", "\xef\xbf\xbf", "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf",
        "A\xc3\xa9\xe6\x97\xa5\xf0\x9f\x8e\xaeZ"};
    const char *invalid[]={"\x80", "\xbf", "\xc0\x80", "\xc1\xbf", "\xc2", "\xc2" "A",
        "\xe0\x9f\xbf", "\xed\xa0\x80", "\xed\xbf\xbf", "\xe1\x80", "\xe1" "A\x80",
        "\xf0\x8f\xbf\xbf", "\xf4\x90\x80\x80", "\xf1\x80\x80", "\xf5\x80\x80\x80",
        "\xf8\x80\x80\x80\x80", "\xfe", "\xff"};
    for(auto field:{SystemProfileField::CurrentUserDisplayName,SystemProfileField::DeviceDisplayLabel}) {
        for(const auto text:valid) {
            auto profile=configured(); char *out=field==SystemProfileField::CurrentUserDisplayName?profile.current_user_display_name:profile.device_display_label;
            std::strcpy(out,text); const auto before=profile;
            const auto result=validate_system_profile_text(profile,field);
            assert(result.status==SystemProfileStatus::Valid && result.length==std::strlen(text));
            assert(std::memcmp(&profile,&before,sizeof profile)==0);
        }
        for(const auto text:invalid) {
            auto profile=configured(); char *out=field==SystemProfileField::CurrentUserDisplayName?profile.current_user_display_name:profile.device_display_label;
            std::strcpy(out,text); const auto before=profile;
            const auto result=validate_system_profile_text(profile,field);
            assert(result.status==SystemProfileStatus::InvalidUtf8 && result.length==0);
            assert(std::memcmp(&profile,&before,sizeof profile)==0);
        }
    }
}
int main() {
    sources_and_fields(); boundaries_and_immutability(); unicode_scalars();
    std::puts("SYSTEM_PROFILE_PASS sources=1 perfield=1 empty=1 bounds=1 unicode_scalars=1 ignored_tail=1 immutable=1");
}
