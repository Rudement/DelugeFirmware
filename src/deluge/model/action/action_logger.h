/*
 * Copyright © 2017-2023 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "definitions_cxx.hpp"
#include "gui/views/performance_view.h"
#include "model/action/action.h"
#include <cstdint>

class ParamCollection;
class Sound;
class ModelStack;

enum class ActionAddition {
	NOT_ALLOWED,
	ALLOWED,
	ALLOWED_ONLY_IF_NO_TIME_PASSED,
};

class ActionLogger {
public:
	ActionLogger();

	/// True while a sound-editor command that edits NOTES is running.
	///
	/// getNewAction() refuses anything raised while a UI sits over the root view, and rightly: almost
	/// everything a menu does is a param change with its own undo path, and a stray Action from a menu
	/// would be un-revertable because the view it was stamped with is not one you can navigate back to.
	/// The note editor and the note row editor are already carved out of that rule, because they edit
	/// notes on the grid while technically being the sound editor.
	///
	/// A one-shot menu command that writes notes - MIDI FX > Generate - is the same case without being
	/// a whole editor, so it gets the same carve-out, held open only for the length of the one call
	/// that asked for it. Use the scope guard below rather than setting this by hand, and stamp the
	/// resulting Action with the ROOT view: the menu is the button that was pressed, not the place the
	/// edit belongs to, and undo has to behave exactly as it does for any other note edit.
	bool allowNoteEditFromMenu{false};

	/// RAII for the flag above.
	class MenuNoteEditScope {
	public:
		MenuNoteEditScope();
		~MenuNoteEditScope();
		MenuNoteEditScope(MenuNoteEditScope const&) = delete;
		MenuNoteEditScope& operator=(MenuNoteEditScope const&) = delete;
	};

	// warning - super not thread safe
	Action* getNewAction(ActionType newActionType,
	                     ActionAddition addToExistingIfPossible = ActionAddition::NOT_ALLOWED);
	void recordUnautomatedParamChange(ModelStackWithAutoParam const* modelStack,
	                                  ActionType actionType = ActionType::PARAM_UNAUTOMATED_VALUE_CHANGE);
	void recordSwingChange(int8_t swingBefore, int8_t swingAfter);
	void recordTempoChange(uint64_t timePerBigBefore, uint64_t timePerBigAfter);
	Action* recordPerformanceViewPress(FXColumnPress fxPressBefore[kDisplayWidth],
	                                   FXColumnPress fxPressAfter[kDisplayWidth], int32_t xDisplay);
	void closeAction(ActionType actionType);
	void closeActionUnlessCreatedJustNow(ActionType actionType);
	void deleteAllLogs();
	void deleteLog(int32_t time);
	bool revert(TimeType time, bool updateVisually = true, bool doNavigation = true);
	void updateAction(Action* newAction);
	void undo();
	void redo();
	bool undoJustOneConsequencePerNoteRow(ModelStack* modelStack);
	bool allowedToDoReversion();
	void notifyClipRecordingAborted(Clip* clip);

	Action* firstAction[2];

private:
	ActionType getParameterChangeActionType() const;
	void revertAction(Action* action, bool updateVisually, bool doNavigation, TimeType time);
	void deleteLastActionIfEmpty();
	void deleteLastAction();
};

extern ActionLogger actionLogger;
