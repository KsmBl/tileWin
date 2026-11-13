#ifndef _TW_VOLUME_H
#define _TW_VOLUME_H

/*
 * The output volume goes on past 100 % to this, as PipeWire and PulseAudio
 * amplify in software over 100 %. The sliders show a mark at 100 and the part
 * past it tinted (the theme key flyout.boost, as the brightness over 100 %).
 * The volume of single apps stays at 100 at most, as on Windows.
 */
#define TW_VOLUME_MAX 130

#endif
