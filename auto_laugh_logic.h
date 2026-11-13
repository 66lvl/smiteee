#pragma once
#include <cstdint>
#include <cmath>
#include <string>
#include <cctype>
namespace AutoLaugh::Logic
{
    inline float ClampInterval(float value) { return std::isfinite(value)?(value<.025f?.025f:value>10.f?10.f:value):.1f; }
    // Do not reserve chat / menu / primary combat mouse keys for this macro.
    inline bool AllowedKey(int key) { return key>=5 && key<=255 && key!=8 && key!=13 && key!=27 && key!=35 && key!=36 && key!=45 && key!=46; }
    inline bool IsLaughTag(std::string tag)
    {
        for(char& c:tag)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const auto dot=tag.find_last_of('.');
        return tag.substr(dot==std::string::npos?0:dot+1)=="laugh";
    }
    struct Gate
    {
        bool NeedRelease=true;
        uint64_t NextAllowed=0;
        bool Ready(uint64_t now,bool held,bool allowed,float interval)
        {
            if(!held){NeedRelease=false;return false;}
            if(!allowed){NeedRelease=true;return false;}
            if(NeedRelease||now<NextAllowed)return false;
            NextAllowed=now+static_cast<uint64_t>(ClampInterval(interval)*1000.f+.5f);
            return true;
        }
    };
}
