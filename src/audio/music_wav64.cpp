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

#include "audio/music_wav64.hpp"

#if defined(ENABLE_SOUND) && defined(N64)

#include "utils/log.hpp"
#include "utils/string_utils.hpp"

// n64/compat/src/al_n64.c
extern "C" float alBufferWav64N64(ALuint buffer, const char *path);
extern "C" void  alBufferLoopN64(ALuint buffer, float start, float end);

MusicWav64::MusicWav64(float loop_start, float loop_end)
           : m_loop_start(loop_start), m_loop_end(loop_end), m_buffer(0),
             m_source(0), m_playing(false)
{
}   // MusicWav64

// ----------------------------------------------------------------------------
MusicWav64::~MusicWav64()
{
    stopMusic();
    if (m_source)
        alDeleteSources(1, &m_source);
    if (m_buffer)
        alDeleteBuffers(1, &m_buffer);
}   // ~MusicWav64

// ----------------------------------------------------------------------------
bool MusicWav64::load(const std::string& filename)
{
    const std::string wav64 = StringUtils::removeExtension(filename) +
                              ".wav64";
    alGenBuffers(1, &m_buffer);
    if (alBufferWav64N64(m_buffer, wav64.c_str()) < 0.0f)
    {
        Log::warn("MusicWav64", "No %s on the cartridge.", wav64.c_str());
        return false;
    }
    // STK loops music from loop_start to loop_end (seconds; -1 = none)
    alBufferLoopN64(m_buffer, m_loop_start > 0.0f ? m_loop_start : 0.0f,
                    m_loop_end > 0.0f ? m_loop_end : -1.0f);
    alGenSources(1, &m_source);
    alSourcei(m_source, AL_BUFFER, (ALint)m_buffer);
    alSourcei(m_source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcef(m_source, AL_ROLLOFF_FACTOR, 0.0f);
    alSourcei(m_source, AL_LOOPING, AL_TRUE);
    return true;
}   // load

// ----------------------------------------------------------------------------
bool MusicWav64::playMusic()
{
    if (!m_source)
        return false;
    alSourcePlay(m_source);
    m_playing = true;
    return true;
}   // playMusic

// ----------------------------------------------------------------------------
bool MusicWav64::stopMusic()
{
    if (m_source)
        alSourceStop(m_source);
    m_playing = false;
    return true;
}   // stopMusic

// ----------------------------------------------------------------------------
bool MusicWav64::pauseMusic()
{
    if (m_source)
        alSourcePause(m_source);
    return true;
}   // pauseMusic

// ----------------------------------------------------------------------------
bool MusicWav64::resumeMusic()
{
    if (m_source)
        alSourcePlay(m_source);
    return true;
}   // resumeMusic

// ----------------------------------------------------------------------------
void MusicWav64::setVolume(float volume)
{
    if (m_source)
        alSourcef(m_source, AL_GAIN, volume);
}   // setVolume

// ----------------------------------------------------------------------------
bool MusicWav64::isPlaying()
{
    return m_playing;
}   // isPlaying

#endif
