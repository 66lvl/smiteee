#pragma once
#include <cstdint>
namespace NoxQuickExitLogic
{
    enum class Event { None, ClickDue, Stopped, Expired };
    struct Gate
    {
        bool requireRelease=true,running=false;
        uint64_t next=0,deadline=0;
        unsigned clicks=0;
        void Reset(){running=false;requireRelease=true;next=deadline=0;clicks=0;}
        Event Step(uint64_t now,bool allowed,bool hold,bool triggerEdge,bool keysReleased,unsigned intervalMs)
        {
            if(!allowed){bool active=running;Reset();return active?Event::Stopped:Event::None;}
            if(requireRelease){if(keysReleased)requireRelease=false;return Event::None;}
            if(running){
                if(!hold){Reset();return Event::Stopped;}
                if(now>=deadline||clicks>=80){Reset();return Event::Expired;}
                if(now<next)return Event::None;
            }else{
                if(!hold||!triggerEdge)return Event::None;
                running=true;deadline=now+2000;clicks=0;
            }
            ++clicks;next=now+(intervalMs<25?25:intervalMs);return Event::ClickDue;
        }
    };
}
