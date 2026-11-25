 //  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2009-2015 Marianne Gagnon
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

#ifndef SERVER_ONLY // No GUI files in server builds

#include "states_screens/options/options_common.hpp"

#include "guiengine/screen.hpp"
#include "guiengine/widgets/ribbon_widget.hpp"

namespace OptionsCommon
{
    void switchTab(std::string selected_tab)
    {
        GUIEngine::Screen *screen = NULL;
        if (selected_tab == "tab_audio")
            screen = OptionsScreenAudio::getInstance();
        else if (selected_tab == "tab_display")
            screen = OptionsScreenDisplay::getInstance();
        else if (selected_tab == "tab_video")
            screen = OptionsScreenVideo::getInstance();
        else if (selected_tab == "tab_players")
            screen = TabbedUserScreen::getInstance();
        else if (selected_tab == "tab_controls")
            screen = OptionsScreenInput::getInstance();
        else if (selected_tab == "tab_ui")
            screen = OptionsScreenUI::getInstance();
        else if (selected_tab == "tab_general")
            screen = OptionsScreenGeneral::getInstance();
        else if (selected_tab == "tab_language")
            screen = OptionsScreenLanguage::getInstance();
        if(screen)
            StateManager::get()->replaceTopMostScreen(screen);
    }

    void updatePauseTooltip(GUIEngine::Widget* widget, bool is_pause)
    {
        if (is_pause)
            // TODO : display a different message in non-racing modes ??
            widget->setTooltip(_("This option cannot be changed during a race."));
        else
            widget->unsetTooltip();
    } // updatePauseTooltip

    void setTabStatus()
    {
        GUIEngine::Widget* players  = GUIEngine::getWidget("tab_players");
        GUIEngine::Widget* language = GUIEngine::getWidget("tab_language");
        bool is_pause = StateManager::get()->getGameState() == GUIEngine::INGAME_MENU;

        // (not there on the N64, see trimTabsForConsole)
        if (players)
        {
            players->setActive(!is_pause);
            updatePauseTooltip(players, is_pause);
        }
        if (language)
        {
            language->setActive(!is_pause);
            updatePauseTooltip(language, is_pause);
        }
    } // setTabStatus

    /** Called by each options screen when it is loaded. On the N64 the
     *  tabs for what the console does not have are left out: General (its
     *  settings are for the internet connection), Players (one default
     *  player) and Language (translations are not on the cartridge). */
    void trimTabsForConsole(GUIEngine::Screen* screen)
    {
#ifdef N64
        GUIEngine::RibbonWidget* tabs =
            screen->getWidget<GUIEngine::RibbonWidget>("options_choice");
        if (!tabs)
            return;
        tabs->removeChildNamed("tab_general");
        tabs->removeChildNamed("tab_players");
        tabs->removeChildNamed("tab_language");
#else
        (void)screen;
#endif
    } // trimTabsForConsole
}

#endif // ifndef SERVER_ONLY
