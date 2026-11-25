//  SuperTuxKart - a fun racing game with go-kart
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

#ifndef HEADER_MUSIC_WAV64_HPP
#define HEADER_MUSIC_WAV64_HPP

#if defined(ENABLE_SOUND) && defined(N64)

#include "audio/music.hpp"

#include <AL/al.h>
#include <string>

/**
 * \brief Music on the N64: the cartridge's .wav64 version of an .ogg,
 *  streamed and decoded by libdragon's mixer through the N64's OpenAL
 *  (n64/compat/src/al_n64.c), which also loops it between STK's loop
 *  points. MusicOggStream decodes Vorbis on the CPU, which the console
 *  cannot spare.
 * \ingroup audio
 */
class MusicWav64 : public Music
{
public:
    MusicWav64(float loop_start, float loop_end);
    virtual ~MusicWav64();

    virtual void update() {}
    virtual bool load(const std::string& filename);
    virtual bool playMusic();
    virtual bool stopMusic();
    virtual bool pauseMusic();
    virtual bool resumeMusic();
    virtual void setVolume(float volume);
    virtual bool isPlaying();

private:
    float  m_loop_start;
    float  m_loop_end;
    ALuint m_buffer;
    ALuint m_source;
    bool   m_playing;
};

#endif
#endif
