/*
 *  Copyright (C) 2002-2014  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/* $Id: sdl_mapper.cpp,v 1.60 2009-06-01 10:25:51 qbix79 Exp $ */

#include <vector>
#include <list>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <assert.h>


#include "SDL.h"
#include "SDL_thread.h"

#include "dosbox.h"
#include "video.h"
#include "keyboard.h"
#include "joystick.h"
#include "support.h"
#include "mapper.h"
#include "setup.h"

enum {
	CLR_BLACK=0,
	CLR_WHITE=1,
	CLR_RED=2,
	CLR_BLUE=3,
	CLR_GREEN=4
};

enum BB_Types {
	BB_Next,BB_Add,BB_Del,
	BB_Save,BB_Exit
};

enum BC_Types {
	BC_Mod1,BC_Mod2,BC_Mod3,
	BC_Hold
};

#define BMOD_Mod1 0x0001
#define BMOD_Mod2 0x0002
#define BMOD_Mod3 0x0004

#define BFLG_Hold 0x0001
#define BFLG_Repeat 0x0004


#define MAXSTICKS 8
#define MAXACTIVE 16
#define MAXBUTTON 32
#define MAXBUTTON_CAP 16

class CEvent;
class CHandlerEvent;
class CButton;
class CBind;
class CBindGroup;

static void SetActiveEvent(CEvent * event);
static void SetActiveBind(CBind * _bind);
extern Bit8u int10_font_14[256 * 14];

static std::vector<CEvent *> events;
static std::vector<CButton *> buttons;
static std::vector<CBindGroup *> bindgroups;
static std::vector<CHandlerEvent *> handlergroup;
typedef std::list<CBind *> CBindList;
typedef std::list<CEvent *>::iterator CEventList_it;
typedef std::list<CBind *>::iterator CBindList_it;
typedef std::vector<CButton *>::iterator CButton_it;
typedef std::vector<CEvent *>::iterator CEventVector_it;
typedef std::vector<CHandlerEvent *>::iterator CHandlerEventVector_it;
typedef std::vector<CBindGroup *>::iterator CBindGroup_it;

static CBindList holdlist;


class CEvent {
public:
	CEvent(char const * const _entry) {
		safe_strncpy(entry,_entry,16);
		events.push_back(this);
		bindlist.clear();
		activity=0;
		current_value=0;
	}
	void AddBind(CBind * bind);
	virtual ~CEvent() {}
	virtual void Active(bool yesno)=0;
	virtual void ActivateEvent(bool ev_trigger,bool skip_action)=0;
	virtual void DeActivateEvent(bool ev_trigger)=0;
	void DeActivateAll(void);
	void SetValue(Bits value){
		current_value=value;
	}
	Bits GetValue(void) {
		return current_value;
	}
	char * GetName(void) { return entry; }
	virtual bool IsTrigger(void)=0;
	CBindList bindlist;
protected:
	Bitu activity;
	char entry[16];
	Bits current_value;
};

/* class for events which can be ON/OFF only: key presses, joystick buttons, joystick hat */
class CTriggeredEvent : public CEvent {
public:
	CTriggeredEvent(char const * const _entry) : CEvent(_entry) {}
	virtual bool IsTrigger(void) {
		return true;
	}
	void ActivateEvent(bool ev_trigger,bool skip_action) {
		if (current_value>25000) {
			/* value exceeds boundary, trigger event if not active */
			if (!activity && !skip_action) Active(true);
			if (activity<32767) activity++;
		} else {
			if (activity>0) {
				/* untrigger event if it is fully inactive */
				DeActivateEvent(ev_trigger);
				activity=0;
			}
		}
	}
	void DeActivateEvent(bool /*ev_trigger*/) {
		activity--;
		if (!activity) Active(false);
	}
};

/* class for events which have a non-boolean state: joystick axis movement */
class CContinuousEvent : public CEvent {
public:
	CContinuousEvent(char const * const _entry) : CEvent(_entry) {}
	virtual bool IsTrigger(void) {
		return false;
	}
	void ActivateEvent(bool ev_trigger,bool skip_action) {
		if (ev_trigger) {
			activity++;
			if (!skip_action) Active(true);
		} else {
			/* test if no trigger-activity is present, this cares especially
			   about activity of the opposite-direction joystick axis for example */
			if (!GetActivityCount()) Active(true);
		}
	}
	void DeActivateEvent(bool ev_trigger) {
		if (ev_trigger) {
			if (activity>0) activity--;
			if (activity==0) {
				/* test if still some trigger-activity is present,
				   adjust the state in this case accordingly */
				if (GetActivityCount()) RepostActivity();
				else Active(false);
			}
		} else {
			if (!GetActivityCount()) Active(false);
		}
	}
	virtual Bitu GetActivityCount(void) {
		return activity;
	}
	virtual void RepostActivity(void) {}
};

class CBind {
public:
	virtual ~CBind () {
		list->remove(this);
//		event->bindlist.remove(this);
	}
	CBind(CBindList * _list) {
		list=_list;
		_list->push_back(this);
		mods=flags=0;
		event=0;
		active=holding=false;
	}
	void AddFlags(char * buf) {
		if (mods & BMOD_Mod1) strcat(buf," mod1");
		if (mods & BMOD_Mod2) strcat(buf," mod2");
		if (mods & BMOD_Mod3) strcat(buf," mod3");
		if (flags & BFLG_Hold) strcat(buf," hold");
	}
	void SetFlags(char * buf) {
		char * word;
		while (*(word=StripWord(buf))) {
			if (!strcasecmp(word,"mod1")) mods|=BMOD_Mod1;
			if (!strcasecmp(word,"mod2")) mods|=BMOD_Mod2;
			if (!strcasecmp(word,"mod3")) mods|=BMOD_Mod3;
			if (!strcasecmp(word,"hold")) flags|=BFLG_Hold;
		}
	}
	void ActivateBind(Bits _value,bool ev_trigger,bool skip_action=false) {
		if (event->IsTrigger()) {
			/* use value-boundary for on/off events */
			if (_value>25000) {
				event->SetValue(_value);
				if (active) return;
				event->ActivateEvent(ev_trigger,skip_action);
				active=true;
			} else {
				if (active) {
					event->DeActivateEvent(ev_trigger);
					active=false;
				}
			}
		} else {
			/* store value for possible later use in the activated event */
			event->SetValue(_value);
			event->ActivateEvent(ev_trigger,false);
		}
	}
	void DeActivateBind(bool ev_trigger) {
		if (event->IsTrigger()) {
			if (!active) return;
			active=false;
			if (flags & BFLG_Hold) {
				if (!holding) {
					holdlist.push_back(this);
					holding=true;
					return;
				} else {
					holdlist.remove(this);
					holding=false;
				}
			}
			event->DeActivateEvent(ev_trigger);
		} else {
			/* store value for possible later use in the activated event */
			event->SetValue(0);
			event->DeActivateEvent(ev_trigger);
		}
	}
	virtual void ConfigName(char * buf)=0;
	virtual void BindName(char * buf)=0;
   
	Bitu mods,flags;
	Bit16s value;
	CEvent * event;
	CBindList * list;
	bool active,holding;
};


void CEvent::AddBind(CBind * bind) {
	bindlist.push_front(bind);
	bind->event=this;
}
void CEvent::DeActivateAll(void) {
	for (CBindList_it bit=bindlist.begin();bit!=bindlist.end();bit++) {
		(*bit)->DeActivateBind(true);
	}
}



class CBindGroup {
public:
	CBindGroup() {
		bindgroups.push_back(this);
	}
	void ActivateBindList(CBindList * list,Bits value,bool ev_trigger);
	void DeactivateBindList(CBindList * list,bool ev_trigger);
	virtual CBind * CreateConfigBind(char *&buf)=0;
	virtual CBind * CreateEventBind(SDL_Event * event)=0;

	virtual bool CheckEvent(SDL_Event * event)=0;
	virtual const char * ConfigStart(void)=0;
	virtual const char * BindStart(void)=0;
	virtual ~CBindGroup (void) { }

protected:

};


#define MAX_SDLKEYS SDL_NUM_SCANCODES

static bool usescancodes;
static Bit8u scancode_map[MAX_SDLKEYS];

#define Z SDL_SCANCODE_UNKNOWN

#if defined (MACOSX)
static SDL_Scancode sdlkey_map[]={
	/* Main block printables */
	/*00-05*/ SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F, SDL_SCANCODE_H, SDL_SCANCODE_G,
	/*06-0B*/ SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_WORLD_0, SDL_SCANCODE_B,
	/*0C-11*/ SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_Y, SDL_SCANCODE_T, 
	/*12-17*/ SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_6, SDL_SCANCODE_5, 
	/*18-1D*/ SDL_SCANCODE_EQUALS, SDL_SCANCODE_9, SDL_SCANCODE_7, SDL_SCANCODE_MINUS, SDL_SCANCODE_8, SDL_SCANCODE_0, 
	/*1E-21*/ SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_O, SDL_SCANCODE_U, SDL_SCANCODE_LEFTBRACKET, 
	/*22-23*/ SDL_SCANCODE_I, SDL_SCANCODE_P,
	/*24-29*/ SDL_SCANCODE_RETURN, SDL_SCANCODE_L, SDL_SCANCODE_J, SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_K, SDL_SCANCODE_SEMICOLON, 
	/*2A-29*/ SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_COMMA, SDL_SCANCODE_SLASH, SDL_SCANCODE_N, SDL_SCANCODE_M, 
	/*2F-2F*/ SDL_SCANCODE_PERIOD,

	/* Spaces, controls, modifiers (dosbox uses LMETA only for
	 * hotkeys, it's not really mapped to an emulated key) */
	/*30-33*/ SDL_SCANCODE_TAB, SDL_SCANCODE_SPACE, SDL_SCANCODE_GRAVE, SDL_SCANCODE_BACKSPACE,
	/*34-37*/ Z, SDL_SCANCODE_ESCAPE, Z, SDL_SCANCODE_LMETA,
	/*38-3B*/ SDL_SCANCODE_LSHIFT, SDL_SCANCODE_CAPSLOCK, SDL_SCANCODE_LALT, SDL_SCANCODE_LCTRL,

	/*3C-40*/ Z, Z, Z, Z, Z,

	/* Keypad (KP_EQUALS not supported, NUMLOCK used on what is CLEAR
	 * in Mac OS X) */
	/*41-46*/ SDL_SCANCODE_KP_PERIOD, Z, SDL_SCANCODE_KP_MULTIPLY, Z, SDL_SCANCODE_KP_PLUS, Z,
	/*47-4A*/ SDL_SCANCODE_NUMLOCKCLEAR /*==SDL_SCANCODE_CLEAR*/, Z, Z, Z,
	/*4B-4D*/ SDL_SCANCODE_KP_DIVIDE, SDL_SCANCODE_KP_ENTER, Z,
	/*4E-51*/ SDL_SCANCODE_KP_MINUS, Z, Z, SDL_SCANCODE_KP_EQUALS,
	/*52-57*/ SDL_SCANCODE_KP0, SDL_SCANCODE_KP1, SDL_SCANCODE_KP2, SDL_SCANCODE_KP3, SDL_SCANCODE_KP4, SDL_SCANCODE_KP5, 
	/*58-5C*/ SDL_SCANCODE_KP6, SDL_SCANCODE_KP7, Z, SDL_SCANCODE_KP8, SDL_SCANCODE_KP9, 

	/*5D-5F*/ Z, Z, Z,
	
	/* Function keys and cursor blocks (F13 not supported, F14 =>
	 * PRINT[SCREEN], F15 => SCROLLOCK, F16 => PAUSE, HELP => INSERT) */
	/*60-64*/ SDL_SCANCODE_F5, SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F3, SDL_SCANCODE_F8,
	/*65-6A*/ SDL_SCANCODE_F9, Z, SDL_SCANCODE_F11, Z, SDL_SCANCODE_F13, SDL_SCANCODE_PAUSE /*==SDL_SCANCODE_F16*/,
	/*6B-70*/ SDL_SCANCODE_PRINTSCREEN /*==SDL_SCANCODE_F14*/, Z, SDL_SCANCODE_F10, Z, SDL_SCANCODE_F12, Z,
	/*71-72*/ SDL_SCANCODE_SCROLLLOCK /*==SDL_SCANCODE_F15*/, SDL_SCANCODE_INSERT /*==SDL_SCANCODE_HELP*/, 
	/*73-77*/ SDL_SCANCODE_HOME, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_DELETE, SDL_SCANCODE_F4, SDL_SCANCODE_END,
	/*78-7C*/ SDL_SCANCODE_F2, SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_F1, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
	/*7D-7E*/ SDL_SCANCODE_DOWN, SDL_SCANCODE_UP,

	/*7F-7F*/ Z,

	/* 4 extra keys that don't really exist, but are needed for
	 * round-trip mapping (dosbox uses RMETA only for hotkeys, it's
	 * not really mapped to an emulated key) */
	SDL_SCANCODE_RMETA, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RALT, SDL_SCANCODE_RCTRL,
};
#define MAX_SCANCODES (0x80+4)
/* Make sure that the table above has the expected size.  This
   expression will raise a compiler error if the condition is false.  */
typedef char assert_right_size [MAX_SCANCODES == (sizeof(sdlkey_map)/sizeof(sdlkey_map[0]))	? 1 : -1];

#else // !MACOSX

#define MAX_SCANCODES 212
static SDL_Scancode sdlkey_map[MAX_SCANCODES]={SDL_SCANCODE_UNKNOWN,SDL_SCANCODE_ESCAPE,
	SDL_SCANCODE_1,SDL_SCANCODE_2,SDL_SCANCODE_3,SDL_SCANCODE_4,SDL_SCANCODE_5,SDL_SCANCODE_6,SDL_SCANCODE_7,SDL_SCANCODE_8,SDL_SCANCODE_9,SDL_SCANCODE_0,
	/* 0x0c: */
	SDL_SCANCODE_MINUS,SDL_SCANCODE_EQUALS,SDL_SCANCODE_BACKSPACE,SDL_SCANCODE_TAB,
	SDL_SCANCODE_Q,SDL_SCANCODE_W,SDL_SCANCODE_E,SDL_SCANCODE_R,SDL_SCANCODE_T,SDL_SCANCODE_Y,SDL_SCANCODE_U,SDL_SCANCODE_I,SDL_SCANCODE_O,SDL_SCANCODE_P,
	SDL_SCANCODE_LEFTBRACKET,SDL_SCANCODE_RIGHTBRACKET,SDL_SCANCODE_RETURN,SDL_SCANCODE_LCTRL,
	SDL_SCANCODE_A,SDL_SCANCODE_S,SDL_SCANCODE_D,SDL_SCANCODE_F,SDL_SCANCODE_G,SDL_SCANCODE_H,SDL_SCANCODE_J,SDL_SCANCODE_K,SDL_SCANCODE_L,
	SDL_SCANCODE_SEMICOLON,SDL_SCANCODE_APOSTROPHE,SDL_SCANCODE_GRAVE,SDL_SCANCODE_LSHIFT,SDL_SCANCODE_BACKSLASH,
	SDL_SCANCODE_Z,SDL_SCANCODE_X,SDL_SCANCODE_C,SDL_SCANCODE_V,SDL_SCANCODE_B,SDL_SCANCODE_N,SDL_SCANCODE_M,
	/* 0x33: */
	SDL_SCANCODE_COMMA,SDL_SCANCODE_PERIOD,SDL_SCANCODE_SLASH,SDL_SCANCODE_RSHIFT,SDL_SCANCODE_KP_MULTIPLY,
	SDL_SCANCODE_LALT,SDL_SCANCODE_SPACE,SDL_SCANCODE_CAPSLOCK,
	SDL_SCANCODE_F1,SDL_SCANCODE_F2,SDL_SCANCODE_F3,SDL_SCANCODE_F4,SDL_SCANCODE_F5,SDL_SCANCODE_F6,SDL_SCANCODE_F7,SDL_SCANCODE_F8,SDL_SCANCODE_F9,SDL_SCANCODE_F10,
	/* 0x45: */
	SDL_SCANCODE_NUMLOCKCLEAR,SDL_SCANCODE_SCROLLLOCK,
	SDL_SCANCODE_KP_7,SDL_SCANCODE_KP_8,SDL_SCANCODE_KP_9,SDL_SCANCODE_KP_MINUS,SDL_SCANCODE_KP_4,SDL_SCANCODE_KP_5,SDL_SCANCODE_KP_6,SDL_SCANCODE_KP_PLUS,
	SDL_SCANCODE_KP_1,SDL_SCANCODE_KP_2,SDL_SCANCODE_KP_3,SDL_SCANCODE_KP_0,SDL_SCANCODE_KP_PERIOD,
	SDL_SCANCODE_UNKNOWN,SDL_SCANCODE_UNKNOWN,
	SDL_SCANCODE_COMMA,SDL_SCANCODE_F11,SDL_SCANCODE_F12,
	Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,
	Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,
	Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,
	/* 0xb7: */
	Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z
	/* 0xd4: ... */
};
#endif

#undef Z


SDL_Scancode MapSDLCode(Bitu skey) {
	if (usescancodes) {
		if (skey<MAX_SCANCODES) return sdlkey_map[skey];
		else return SDL_SCANCODE_UNKNOWN;
	} else return (SDL_Scancode)skey;
}

Bitu GetKeyCode(SDL_Keysym keysym) {
    return (Bitu)keysym.scancode;
}


class CKeyBind;
class CKeyBindGroup;

class CKeyBind : public CBind {
public:
	CKeyBind(CBindList * _list,SDL_Scancode _key) : CBind(_list) {
		key = _key;
	}
	void BindName(char * buf) {
		sprintf(buf,"Key %s",SDL_GetKeyName(MapSDLCode((Bitu)key)));
	}
	void ConfigName(char * buf) {
		sprintf(buf,"key %d",MapSDLCode((Bitu)key));
	}
public:
	SDL_Scancode key;
};

class CKeyBindGroup : public  CBindGroup {
public:
	CKeyBindGroup(Bitu _keys) : CBindGroup (){
		lists=new CBindList[_keys];
		for (Bitu i=0;i<_keys;i++) lists[i].clear();
		keys=_keys;
		configname="key";
	}
	~CKeyBindGroup() { delete[] lists; }
	CBind * CreateConfigBind(char *& buf) {
		if (strncasecmp(buf,configname,strlen(configname))) return 0;
		StripWord(buf);char * num=StripWord(buf);
		Bitu code=ConvDecWord(num);
		if (usescancodes) {
			if (code<MAX_SDLKEYS) code=scancode_map[code];
			else code=0;
		}
		CBind * bind=CreateKeyBind((SDL_Scancode)code);
		return bind;
	}
	CBind * CreateEventBind(SDL_Event * event) {
		if (event->type!=SDL_KEYDOWN) return 0;
		return CreateKeyBind((SDL_Scancode)GetKeyCode(event->key.keysym));
	};
	bool CheckEvent(SDL_Event * event) {
		if (event->type!=SDL_KEYDOWN && event->type!=SDL_KEYUP) return false;
		Bitu key=GetKeyCode(event->key.keysym);
//		LOG_MSG("key type %i is %x [%x %x]",event->type,key,event->key.keysym.sym,event->key.keysym.scancode);
		assert(Bitu(event->key.keysym.scancode)<keys);
		if (event->type==SDL_KEYDOWN) ActivateBindList(&lists[key],0x7fff,true);
		else DeactivateBindList(&lists[key],true);
		return 0;
	}
	CBind * CreateKeyBind(SDL_Scancode _key) {
		if (!usescancodes) assert((Bitu)_key<keys);
		return new CKeyBind(&lists[(Bitu)_key],_key);
	}
private:
	const char * ConfigStart(void) {
		return configname;
	}
	const char * BindStart(void) {
		return "Key";
	}
protected:
	const char * configname;
	CBindList * lists;
	Bitu keys;
};

bool autofire = false;
/*
//TODO: Add joystick support back in.
#define MAX_VJOY_BUTTONS 8

static struct {
	bool button_pressed[MAX_VJOY_BUTTONS];
	Bit16s axis_pos[8];
	bool hat_pressed[16];
} virtual_joysticks[2];


class CJAxisBind;
class CJButtonBind;
class CJHatBind;

class CJAxisBind : public CBind {
public:
	CJAxisBind(CBindList * _list,CBindGroup * _group,Bitu _axis,bool _positive) : CBind(_list){
		group = _group;
		axis = _axis;
		positive = _positive;
	}
	void ConfigName(char * buf) {
		sprintf(buf,"%s axis %d %d",group->ConfigStart(),axis,positive ? 1 : 0);
	}
	void BindName(char * buf) {
		sprintf(buf,"%s Axis %d%s",group->BindStart(),axis,positive ? "+" : "-");
	}
protected:
	CBindGroup * group;
	Bitu axis;
	bool positive;
};

class CJButtonBind : public CBind {
public:
	CJButtonBind(CBindList * _list,CBindGroup * _group,Bitu _button) : CBind(_list) {
		group = _group;
		button=_button;
	}
	void ConfigName(char * buf) {
		sprintf(buf,"%s button %d",group->ConfigStart(),button);
	}
	void BindName(char * buf) {
		sprintf(buf,"%s Button %d",group->BindStart(),button);
	}
protected:
	CBindGroup * group;
	Bitu button;
};

class CJHatBind : public CBind {
public:
	CJHatBind(CBindList * _list,CBindGroup * _group,Bitu _hat,Bit8u _dir) : CBind(_list) {
		group = _group;
		hat   = _hat;
		dir   = _dir;
		//allow only one hat position
		if (dir&SDL_HAT_UP) dir=SDL_HAT_UP;
		else if (dir&SDL_HAT_RIGHT) dir=SDL_HAT_RIGHT;
		else if (dir&SDL_HAT_DOWN) dir=SDL_HAT_DOWN;
		else if (dir&SDL_HAT_LEFT) dir=SDL_HAT_LEFT;
		else E_Exit("MAPPER:JOYSTICK:Invalid hat position");
	}
	void ConfigName(char * buf) {
		sprintf(buf,"%s hat %d %d",group->ConfigStart(),hat,dir);
	}
	void BindName(char * buf) {
		sprintf(buf,"%s Hat %d %s",group->BindStart(),hat,(dir==SDL_HAT_UP)?"up":
														((dir==SDL_HAT_RIGHT)?"right":
														((dir==SDL_HAT_DOWN)?"down":"left")));
	}
protected:
	CBindGroup * group;
	Bitu hat;
	Bit8u dir;
};

bool autofire = false;

class CStickBindGroup : public  CBindGroup {
public:
	CStickBindGroup(Bitu _stick,Bitu _emustick,bool _dummy=false) : CBindGroup (){
		stick=_stick;		// the number of the physical device (SDL numbering|)
		emustick=_emustick;	// the number of the emulated device
		sprintf(configname,"stick_%d",emustick);

		sdl_joystick=NULL;
		axes=0;	buttons=0; hats=0;
		button_wrap=0;
		button_cap=0; axes_cap=0; hats_cap=0;
		emulated_buttons=0; emulated_axes=0; emulated_hats=0;

		is_dummy=_dummy;
		if (_dummy) return;

		// initialize binding lists and position data
		pos_axis_lists=new CBindList[4];
		neg_axis_lists=new CBindList[4];
		button_lists=new CBindList[MAXBUTTON];
		hat_lists=new CBindList[4];
		Bitu i;
		for (i=0; i<MAXBUTTON; i++) {
			button_autofire[i]=0;
			old_button_state[i]=0;
		}
		for(i=0;i<16;i++) old_hat_state[i]=0;
		for (i=0; i<4; i++) {
			old_pos_axis_state[i]=false;
			old_neg_axis_state[i]=false;
		}

		// initialize emulated joystick state
		emulated_axes=2;
		emulated_buttons=2;
		emulated_hats=0;
		JOYSTICK_Enable(emustick,true);

		sdl_joystick=SDL_JoystickOpen(_stick);
		if (sdl_joystick==NULL) {
			button_wrap=emulated_buttons;
			return;
		}

		axes=SDL_JoystickNumAxes(sdl_joystick);
		buttons=SDL_JoystickNumButtons(sdl_joystick);
		hats=SDL_JoystickNumHats(sdl_joystick);
		button_wrap=buttons;
		button_cap=buttons;
		if (button_wrapping_enabled) {
			button_wrap=emulated_buttons;
			if (buttons>MAXBUTTON_CAP) button_cap = MAXBUTTON_CAP;
		}
		if (button_wrap > MAXBUTTON) button_wrap = MAXBUTTON;
		axes_cap=emulated_axes;
		if (axes_cap>axes) axes_cap=axes;
		hats_cap=emulated_hats;
		if (hats_cap>hats) hats_cap=hats;
		LOG_MSG("Using joystick %s with %d axes, %d buttons and %d hat(s)",SDL_JoystickName(stick),axes,buttons,hats);
	}
	~CStickBindGroup() {
		SDL_JoystickClose(sdl_joystick);
		delete[] pos_axis_lists;
		delete[] neg_axis_lists;
		delete[] button_lists;
		delete[] hat_lists;
	}

	CBind * CreateConfigBind(char *& buf) {
		if (strncasecmp(configname,buf,strlen(configname))) return 0;
		StripWord(buf);char * type=StripWord(buf);
		CBind * bind=0;
		if (!strcasecmp(type,"axis")) {
			Bitu ax=ConvDecWord(StripWord(buf));
			bool pos=ConvDecWord(StripWord(buf)) > 0;
			bind=CreateAxisBind(ax,pos);
		} else if (!strcasecmp(type,"button")) {
			Bitu but=ConvDecWord(StripWord(buf));			
			bind=CreateButtonBind(but);
		} else if (!strcasecmp(type,"hat")) {
			Bitu hat=ConvDecWord(StripWord(buf));			
			Bit8u dir=(Bit8u)ConvDecWord(StripWord(buf));			
			bind=CreateHatBind(hat,dir);
		}
		return bind;
	}
	CBind * CreateEventBind(SDL_Event * event) {
		if (event->type==SDL_JOYAXISMOTION) {
			if (event->jaxis.which!=stick) return 0;
#if defined (REDUCE_JOYSTICK_POLLING)
			if (event->jaxis.axis>=emulated_axes) return 0;
#endif
			if (abs(event->jaxis.value)<25000) return 0;
			return CreateAxisBind(event->jaxis.axis,event->jaxis.value>0);
		} else if (event->type==SDL_JOYBUTTONDOWN) {
			if (event->jbutton.which!=stick) return 0;
#if defined (REDUCE_JOYSTICK_POLLING)
			return CreateButtonBind(event->jbutton.button%button_wrap);
#else
			return CreateButtonBind(event->jbutton.button);
#endif
		} else if (event->type==SDL_JOYHATMOTION) {
			if (event->jhat.which!=stick) return 0;
			if (event->jhat.value==0) return 0;
			if (event->jhat.value>(SDL_HAT_UP|SDL_HAT_RIGHT|SDL_HAT_DOWN|SDL_HAT_LEFT)) return 0;
			return CreateHatBind(event->jhat.hat,event->jhat.value);
		} else return 0;
	}

	virtual bool CheckEvent(SDL_Event * event) {
		SDL_JoyAxisEvent * jaxis = NULL;
		SDL_JoyButtonEvent * jbutton = NULL;
		Bitu but = 0;

		switch(event->type) {
			case SDL_JOYAXISMOTION:
				jaxis = &event->jaxis;
				if(jaxis->which == stick) {
					if(jaxis->axis == 0)
						JOYSTICK_Move_X(emustick,(float)(jaxis->value/32768.0));
					else if(jaxis->axis == 1)
						JOYSTICK_Move_Y(emustick,(float)(jaxis->value/32768.0));
				}
				break;
			case SDL_JOYBUTTONDOWN:
			case SDL_JOYBUTTONUP:
				jbutton = &event->jbutton;
				bool state;
				state=jbutton->type==SDL_JOYBUTTONDOWN;
				but = jbutton->button % emulated_buttons;
				if (jbutton->which == stick) {
					JOYSTICK_Button(emustick,but,state);
				}
				break;
		}
		return false;
	}

	virtual void UpdateJoystick() {
		if (is_dummy) return;
		//query SDL joystick and activate bindings
		ActivateJoystickBoundEvents();

		bool button_pressed[MAXBUTTON];
		Bitu i;
		for (i=0; i<MAXBUTTON; i++) button_pressed[i]=false;
		for (i=0; i<MAX_VJOY_BUTTONS; i++) {
			if (virtual_joysticks[emustick].button_pressed[i])
				button_pressed[i % button_wrap]=true;
		}
		for (i=0; i<emulated_buttons; i++) {
			if (autofire && (button_pressed[i]))
				JOYSTICK_Button(emustick,i,(++button_autofire[i])&1);
			else
				JOYSTICK_Button(emustick,i,button_pressed[i]);
		}

		JOYSTICK_Move_X(emustick,((float)virtual_joysticks[emustick].axis_pos[0])/32768.0f);
		JOYSTICK_Move_Y(emustick,((float)virtual_joysticks[emustick].axis_pos[1])/32768.0f);
	}

	void ActivateJoystickBoundEvents() {
		if (GCC_UNLIKELY(sdl_joystick==NULL)) return;

		Bitu i;

		bool button_pressed[MAXBUTTON];
		for (i=0; i<MAXBUTTON; i++) button_pressed[i]=false;
		//read button states
		for (i=0; i<button_cap; i++) {
			if (SDL_JoystickGetButton(sdl_joystick,i))
				button_pressed[i % button_wrap]=true;
		}
		for (i=0; i<button_wrap; i++) {
			//activate binding if button state has changed
			if (button_pressed[i]!=old_button_state[i]) {
				if (button_pressed[i]) ActivateBindList(&button_lists[i],32767,true);
				else DeactivateBindList(&button_lists[i],true);
				old_button_state[i]=button_pressed[i];
			}
		}

		for (i=0; i<axes_cap; i++) {
			Sint16 caxis_pos=SDL_JoystickGetAxis(sdl_joystick,i);
			// activate bindings for joystick position
			if (caxis_pos>1) {
				if (old_neg_axis_state[i]) {
					DeactivateBindList(&neg_axis_lists[i],false);
					old_neg_axis_state[i] = false;
				}
				ActivateBindList(&pos_axis_lists[i],caxis_pos,false);
				old_pos_axis_state[i] = true;
			} else if (caxis_pos<-1) {
				if (old_pos_axis_state[i]) {
					DeactivateBindList(&pos_axis_lists[i],false);
					old_pos_axis_state[i] = false;
				}
				if (caxis_pos!=-32768) caxis_pos=(Sint16)abs(caxis_pos);
				else caxis_pos=32767;
				ActivateBindList(&neg_axis_lists[i],caxis_pos,false);
				old_neg_axis_state[i] = true;
			} else {
				// center
				if (old_pos_axis_state[i]) {
					DeactivateBindList(&pos_axis_lists[i],false);
					old_pos_axis_state[i] = false;
				}
				if (old_neg_axis_state[i]) {
					DeactivateBindList(&neg_axis_lists[i],false);
					old_neg_axis_state[i] = false;
				}
			}
		}

		for (i=0; i<hats_cap; i++) {
			Uint8 chat_state=SDL_JoystickGetHat(sdl_joystick,i);

			// activate binding if hat state has changed
			if ((chat_state & SDL_HAT_UP) != (old_hat_state[i] & SDL_HAT_UP)) {
				if (chat_state & SDL_HAT_UP) ActivateBindList(&hat_lists[(i<<2)+0],32767,true);
				else DeactivateBindList(&hat_lists[(i<<2)+0],true);
			}
			if ((chat_state & SDL_HAT_RIGHT) != (old_hat_state[i] & SDL_HAT_RIGHT)) {
				if (chat_state & SDL_HAT_RIGHT) ActivateBindList(&hat_lists[(i<<2)+1],32767,true);
				else DeactivateBindList(&hat_lists[(i<<2)+1],true);
			}
			if ((chat_state & SDL_HAT_DOWN) != (old_hat_state[i] & SDL_HAT_DOWN)) {
				if (chat_state & SDL_HAT_DOWN) ActivateBindList(&hat_lists[(i<<2)+2],32767,true);
				else DeactivateBindList(&hat_lists[(i<<2)+2],true);
			}
			if ((chat_state & SDL_HAT_LEFT) != (old_hat_state[i] & SDL_HAT_LEFT)) {
				if (chat_state & SDL_HAT_LEFT) ActivateBindList(&hat_lists[(i<<2)+3],32767,true);
				else DeactivateBindList(&hat_lists[(i<<2)+3],true);
			}
			old_hat_state[i]=chat_state;
		}
	}

private:
	CBind * CreateAxisBind(Bitu axis,bool positive) {
		if (axis<emulated_axes) {
			if (positive) return new CJAxisBind(&pos_axis_lists[axis],this,axis,positive);
			else return new CJAxisBind(&neg_axis_lists[axis],this,axis,positive);
		}
		return NULL;
	}
	CBind * CreateButtonBind(Bitu button) {
		if (button<button_wrap) 
			return new CJButtonBind(&button_lists[button],this,button);
		return NULL;
	}
	CBind * CreateHatBind(Bitu hat,Bit8u value) {
		Bitu hat_dir;
		if (value&SDL_HAT_UP) hat_dir=0;
		else if (value&SDL_HAT_RIGHT) hat_dir=1;
		else if (value&SDL_HAT_DOWN) hat_dir=2;
		else if (value&SDL_HAT_LEFT) hat_dir=3;
		else return NULL;
		return new CJHatBind(&hat_lists[(hat<<2)+hat_dir],this,hat,value);
	}
	const char * ConfigStart(void) {
		return configname;
	}
	const char * BindStart(void) {
		if (sdl_joystick!=NULL) return SDL_JoystickName(stick);
		else return "[missing joystick]";
	}

protected:
	CBindList * pos_axis_lists;
	CBindList * neg_axis_lists;
	CBindList * button_lists;
	CBindList * hat_lists;
	Bitu stick,emustick,axes,buttons,hats,emulated_axes,emulated_buttons,emulated_hats;
	Bitu button_wrap,button_cap,axes_cap,hats_cap;
	SDL_Joystick * sdl_joystick;
	char configname[10];
	Bitu button_autofire[MAXBUTTON];
	bool old_button_state[MAXBUTTON];
	bool old_pos_axis_state[16];
	bool old_neg_axis_state[16];
	Uint8 old_hat_state[16];
	bool is_dummy;
};

class C4AxisBindGroup : public  CStickBindGroup {
public:
	C4AxisBindGroup(Bitu _stick,Bitu _emustick) : CStickBindGroup (_stick,_emustick){
		emulated_axes=4;
		emulated_buttons=4;
		if (button_wrapping_enabled) button_wrap=emulated_buttons;

		axes_cap=emulated_axes;
		if (axes_cap>axes) axes_cap=axes;
		hats_cap=emulated_hats;
		if (hats_cap>hats) hats_cap=hats;

		JOYSTICK_Enable(1,true);
	}

	bool CheckEvent(SDL_Event * event) {
		SDL_JoyAxisEvent * jaxis = NULL;
		SDL_JoyButtonEvent * jbutton = NULL;
		Bitu but = 0;

		switch(event->type) {
			case SDL_JOYAXISMOTION:
				jaxis = &event->jaxis;
				if(jaxis->which == stick && jaxis->axis < 4) {
					if(jaxis->axis & 1)
						JOYSTICK_Move_Y(jaxis->axis>>1 & 1,(float)(jaxis->value/32768.0));
					else
						JOYSTICK_Move_X(jaxis->axis>>1 & 1,(float)(jaxis->value/32768.0));
				}
				break;
			case SDL_JOYBUTTONDOWN:
			case SDL_JOYBUTTONUP:
				jbutton = &event->jbutton;
				bool state;
				state=jbutton->type==SDL_JOYBUTTONDOWN;
				but = jbutton->button % emulated_buttons;
				if (jbutton->which == stick) {
					JOYSTICK_Button((but >> 1),(but & 1),state);
				}
				break;
		}
		return false;
	}

	virtual void UpdateJoystick() {
		// query SDL joystick and activate bindings
		ActivateJoystickBoundEvents();

		bool button_pressed[MAXBUTTON];
		Bitu i;
		for (i=0; i<MAXBUTTON; i++) button_pressed[i]=false;
		for (i=0; i<MAX_VJOY_BUTTONS; i++) {
			if (virtual_joysticks[0].button_pressed[i])
				button_pressed[i % button_wrap]=true;
			
		}
		for (i=0; i<emulated_buttons; i++) {
			if (autofire && (button_pressed[i]))
				JOYSTICK_Button(i>>1,i&1,(++button_autofire[i])&1);
			else
				JOYSTICK_Button(i>>1,i&1,button_pressed[i]);
		}

		JOYSTICK_Move_X(0,((float)virtual_joysticks[0].axis_pos[0])/32768.0f);
		JOYSTICK_Move_Y(0,((float)virtual_joysticks[0].axis_pos[1])/32768.0f);
		JOYSTICK_Move_X(1,((float)virtual_joysticks[0].axis_pos[2])/32768.0f);
		JOYSTICK_Move_Y(1,((float)virtual_joysticks[0].axis_pos[3])/32768.0f);
	}
};

class CFCSBindGroup : public  CStickBindGroup {
public:
	CFCSBindGroup(Bitu _stick,Bitu _emustick) : CStickBindGroup (_stick,_emustick){
		emulated_axes=4;
		emulated_buttons=4;
		old_hat_position=0;
		emulated_hats=1;
		if (button_wrapping_enabled) button_wrap=emulated_buttons;

		axes_cap=emulated_axes;
		if (axes_cap>axes) axes_cap=axes;
		hats_cap=emulated_hats;
		if (hats_cap>hats) hats_cap=hats;

		JOYSTICK_Enable(1,true);
		JOYSTICK_Move_Y(1,1.0);
	}

	bool CheckEvent(SDL_Event * event) {
		SDL_JoyAxisEvent * jaxis = NULL;
		SDL_JoyButtonEvent * jbutton = NULL;
		SDL_JoyHatEvent * jhat = NULL;
		Bitu but = 0;

		switch(event->type) {
			case SDL_JOYAXISMOTION:
				jaxis = &event->jaxis;
				if(jaxis->which == stick) {
					if(jaxis->axis == 0)
						JOYSTICK_Move_X(0,(float)(jaxis->value/32768.0));
					else if(jaxis->axis == 1)
						JOYSTICK_Move_Y(0,(float)(jaxis->value/32768.0));
					else if(jaxis->axis == 2)
						JOYSTICK_Move_X(1,(float)(jaxis->value/32768.0));
				}
				break;
			case SDL_JOYHATMOTION:
				jhat = &event->jhat;
				if(jhat->which == stick) DecodeHatPosition(jhat->value);
				break;
			case SDL_JOYBUTTONDOWN:
			case SDL_JOYBUTTONUP:
				jbutton = &event->jbutton;
				bool state;
				state=jbutton->type==SDL_JOYBUTTONDOWN;
				but = jbutton->button % emulated_buttons;
				if (jbutton->which == stick) {
						JOYSTICK_Button((but >> 1),(but & 1),state);
				}
				break;
		}
		return false;
	}

	virtual void UpdateJoystick() {
		// query SDL joystick and activate bindings
		ActivateJoystickBoundEvents();

		bool button_pressed[MAXBUTTON];
		Bitu i;
		for (i=0; i<MAXBUTTON; i++) button_pressed[i]=false;
		for (i=0; i<MAX_VJOY_BUTTONS; i++) {
			if (virtual_joysticks[0].button_pressed[i])
				button_pressed[i % button_wrap]=true;
			
		}
		for (i=0; i<emulated_buttons; i++) {
			if (autofire && (button_pressed[i]))
				JOYSTICK_Button(i>>1,i&1,(++button_autofire[i])&1);
			else
				JOYSTICK_Button(i>>1,i&1,button_pressed[i]);
		}

		JOYSTICK_Move_X(0,((float)virtual_joysticks[0].axis_pos[0])/32768.0f);
		JOYSTICK_Move_Y(0,((float)virtual_joysticks[0].axis_pos[1])/32768.0f);
		JOYSTICK_Move_X(1,((float)virtual_joysticks[0].axis_pos[2])/32768.0f);

		Uint8 hat_pos=0;
		if (virtual_joysticks[0].hat_pressed[0]) hat_pos|=SDL_HAT_UP;
		else if (virtual_joysticks[0].hat_pressed[2]) hat_pos|=SDL_HAT_DOWN;
		if (virtual_joysticks[0].hat_pressed[3]) hat_pos|=SDL_HAT_LEFT;
		else if (virtual_joysticks[0].hat_pressed[1]) hat_pos|=SDL_HAT_RIGHT;

		if (hat_pos!=old_hat_position) {
			DecodeHatPosition(hat_pos);
			old_hat_position=hat_pos;
		}
	}

private:
	Uint8 old_hat_position;

	void DecodeHatPosition(Uint8 hat_pos) {
		switch(hat_pos) {
			case SDL_HAT_CENTERED:
				JOYSTICK_Move_Y(1,1.0);
				break;
			case SDL_HAT_UP:
				JOYSTICK_Move_Y(1,-1.0);
				break;
			case SDL_HAT_RIGHT:
				JOYSTICK_Move_Y(1,-0.5);
				break;
			case SDL_HAT_DOWN:
				JOYSTICK_Move_Y(1,0.0);
				break;
			case SDL_HAT_LEFT:
				JOYSTICK_Move_Y(1,0.5);
				break;
			case SDL_HAT_LEFTUP:
				if(JOYSTICK_GetMove_Y(1) < 0)
					JOYSTICK_Move_Y(1,0.5);
				else
					JOYSTICK_Move_Y(1,-1.0);
				break;
			case SDL_HAT_RIGHTUP:
				if(JOYSTICK_GetMove_Y(1) < -0.7)
					JOYSTICK_Move_Y(1,-0.5);
				else
					JOYSTICK_Move_Y(1,-1.0);
				break;
			case SDL_HAT_RIGHTDOWN:
				if(JOYSTICK_GetMove_Y(1) < -0.2)
					JOYSTICK_Move_Y(1,0.0);
				else
					JOYSTICK_Move_Y(1,-0.5);
				break;
			case SDL_HAT_LEFTDOWN:
				if(JOYSTICK_GetMove_Y(1) > 0.2)
					JOYSTICK_Move_Y(1,0.0);
				else
					JOYSTICK_Move_Y(1,0.5);
				break;
		}
	}
};

class CCHBindGroup : public  CStickBindGroup {
public:
	CCHBindGroup(Bitu _stick,Bitu _emustick) : CStickBindGroup (_stick,_emustick){
		emulated_axes=4;
		emulated_buttons=6;
		emulated_hats=1;
		if (button_wrapping_enabled) button_wrap=emulated_buttons;

		axes_cap=emulated_axes;
		if (axes_cap>axes) axes_cap=axes;
		hats_cap=emulated_hats;
		if (hats_cap>hats) hats_cap=hats;

		JOYSTICK_Enable(1,true);
		button_state=0;
	}

	bool CheckEvent(SDL_Event * event) {
		SDL_JoyAxisEvent * jaxis = NULL;
		SDL_JoyButtonEvent * jbutton = NULL;
		SDL_JoyHatEvent * jhat = NULL;
		Bitu but = 0;
		static unsigned const button_magic[6]={0x02,0x04,0x10,0x100,0x20,0x200};
		static unsigned const hat_magic[2][5]={{0x8888,0x8000,0x800,0x80,0x08},
							   {0x5440,0x4000,0x400,0x40,0x1000}};
		switch(event->type) {
			case SDL_JOYAXISMOTION:
				jaxis = &event->jaxis;
				if(jaxis->which == stick && jaxis->axis < 4) {
					if(jaxis->axis & 1)
						JOYSTICK_Move_Y(jaxis->axis>>1 & 1,(float)(jaxis->value/32768.0));
					else
						JOYSTICK_Move_X(jaxis->axis>>1 & 1,(float)(jaxis->value/32768.0));
				}
				break;
			case SDL_JOYHATMOTION:
				jhat = &event->jhat;
				if(jhat->which == stick && jhat->hat < 2) {
					if(jhat->value == SDL_HAT_CENTERED)
						button_state&=~hat_magic[jhat->hat][0];
					if(jhat->value & SDL_HAT_UP)
						button_state|=hat_magic[jhat->hat][1];
					if(jhat->value & SDL_HAT_RIGHT)
						button_state|=hat_magic[jhat->hat][2];
					if(jhat->value & SDL_HAT_DOWN)
						button_state|=hat_magic[jhat->hat][3];
					if(jhat->value & SDL_HAT_LEFT)
						button_state|=hat_magic[jhat->hat][4];
				}
				break;
			case SDL_JOYBUTTONDOWN:
				jbutton = &event->jbutton;
				but = jbutton->button % emulated_buttons;
				if (jbutton->which == stick)
					button_state|=button_magic[but];
				break;
			case SDL_JOYBUTTONUP:
				jbutton = &event->jbutton;
				but = jbutton->button % emulated_buttons;
				if (jbutton->which == stick)
					button_state&=~button_magic[but];
				break;
		}

		unsigned i;
		Bit16u j;
		j=button_state;
		for(i=0;i<16;i++) if (j & 1) break; else j>>=1;
		JOYSTICK_Button(0,0,i&1);
		JOYSTICK_Button(0,1,(i>>1)&1);
		JOYSTICK_Button(1,0,(i>>2)&1);
		JOYSTICK_Button(1,1,(i>>3)&1);
		return false;
	}

	void UpdateJoystick() {
		static unsigned const button_priority[6]={7,11,13,14,5,6};
		static unsigned const hat_priority[2][4]={{0,1,2,3},{8,9,10,12}};

		// query SDL joystick and activate bindings
		ActivateJoystickBoundEvents();

		JOYSTICK_Move_X(0,((float)virtual_joysticks[0].axis_pos[0])/32768.0f);
		JOYSTICK_Move_Y(0,((float)virtual_joysticks[0].axis_pos[1])/32768.0f);
		JOYSTICK_Move_X(1,((float)virtual_joysticks[0].axis_pos[2])/32768.0f);
		JOYSTICK_Move_Y(1,((float)virtual_joysticks[0].axis_pos[3])/32768.0f);

		Bitu bt_state=15;

		Bitu i;
		for (i=0; i<(hats<2?hats:2); i++) {
			Uint8 hat_pos=0;
			if (virtual_joysticks[0].hat_pressed[(i<<2)+0]) hat_pos|=SDL_HAT_UP;
			else if (virtual_joysticks[0].hat_pressed[(i<<2)+2]) hat_pos|=SDL_HAT_DOWN;
			if (virtual_joysticks[0].hat_pressed[(i<<2)+3]) hat_pos|=SDL_HAT_LEFT;
			else if (virtual_joysticks[0].hat_pressed[(i<<2)+1]) hat_pos|=SDL_HAT_RIGHT;

			if (hat_pos & SDL_HAT_UP)
				if (bt_state>hat_priority[i][0]) bt_state=hat_priority[i][0];
			if (hat_pos & SDL_HAT_DOWN)
				if (bt_state>hat_priority[i][1]) bt_state=hat_priority[i][1];
			if (hat_pos & SDL_HAT_RIGHT)
				if (bt_state>hat_priority[i][2]) bt_state=hat_priority[i][2];
			if (hat_pos & SDL_HAT_LEFT)
				if (bt_state>hat_priority[i][3]) bt_state=hat_priority[i][3];
		}

		bool button_pressed[MAXBUTTON];
		for (i=0; i<MAXBUTTON; i++) button_pressed[i]=false;
		for (i=0; i<MAX_VJOY_BUTTONS; i++) {
			if (virtual_joysticks[0].button_pressed[i])
				button_pressed[i % button_wrap]=true;
			
		}
		for (i=0; i<6; i++) {
			if ((button_pressed[i]) && (bt_state>button_priority[i]))
				bt_state=button_priority[i];
		}

		if (bt_state>15) bt_state=15;
		JOYSTICK_Button(0,0,(bt_state&8)==0);
		JOYSTICK_Button(0,1,(bt_state&4)==0);
		JOYSTICK_Button(1,0,(bt_state&2)==0);
		JOYSTICK_Button(1,1,(bt_state&1)==0);
	}

protected:
	Bit16u button_state;
};
*/

static struct CMapper {
	SDL_Surface * surface;
	SDL_Surface * draw_surface;
	bool exit;
	CEvent * aevent;				//Active Event
	CBind * abind;					//Active Bind
	CBindList_it abindit;			//Location of active bind in list
	bool redraw;
	bool addbind;
	Bitu mods;
	struct {
		Bitu num_groups,num;
		//CStickBindGroup * stick[MAXSTICKS];
	} sticks;
	std::string filename;
} mapper;

void CBindGroup::ActivateBindList(CBindList * list,Bits value,bool ev_trigger) {
	Bitu validmod=0;
	CBindList_it it;
	for (it=list->begin();it!=list->end();it++) {
		if (((*it)->mods & mapper.mods) == (*it)->mods) {
			if (validmod<(*it)->mods) validmod=(*it)->mods;
		}
	}
	for (it=list->begin();it!=list->end();it++) {
	/*BUG:CRASH if keymapper key is removed*/
		if (validmod==(*it)->mods) (*it)->ActivateBind(value,ev_trigger);
	}
}

void CBindGroup::DeactivateBindList(CBindList * list,bool ev_trigger) {
	CBindList_it it;
	for (it=list->begin();it!=list->end();it++) {
		(*it)->DeActivateBind(ev_trigger);
	}
}

static void DrawText(Bitu x,Bitu y,const char * text,Bit8u color) {
	Bit8u * draw=((Bit8u *)mapper.surface->pixels)+(y*mapper.surface->pitch)+x;
	while (*text) {
		Bit8u * font=&int10_font_14[(*text)*14];
		Bitu i,j;Bit8u * draw_line=draw;
		for (i=0;i<14;i++) {
			Bit8u map=*font++;
			for (j=0;j<8;j++) {
				if (map & 0x80) *(draw_line+j)=color;
				else *(draw_line+j)=CLR_BLACK;
				map<<=1;
			}
			draw_line+=mapper.surface->pitch;
		}
		text++;draw+=8;
	}
}

class CButton {
public:
	virtual ~CButton(){};
	CButton(Bitu _x,Bitu _y,Bitu _dx,Bitu _dy) {
		x=_x;y=_y;dx=_dx;dy=_dy;
		buttons.push_back(this);
		color=CLR_WHITE;
		enabled=true;
	}
	virtual void Draw(void) {
		if (!enabled) return;
		Bit8u * point=((Bit8u *)mapper.surface->pixels)+(y*mapper.surface->pitch)+x;
		for (Bitu lines=0;lines<dy;lines++)  {
			if (lines==0 || lines==(dy-1)) {
				for (Bitu cols=0;cols<dx;cols++) *(point+cols)=color;
			} else {
				*point=color;*(point+dx-1)=color;
			}
			point+=mapper.surface->pitch;
		}
	}
	virtual bool OnTop(Bitu _x,Bitu _y) {
		return ( enabled && (_x>=x) && (_x<x+dx) && (_y>=y) && (_y<y+dy));
	}
	virtual void Click(void) {}
	void Enable(bool yes) { 
		enabled=yes; 
		mapper.redraw=true;
	}
	void SetColor(Bit8u _col) { color=_col; }
protected:
	Bitu x,y,dx,dy;
	Bit8u color;
	bool enabled;
};

class CTextButton : public CButton {
public:
	CTextButton(Bitu _x,Bitu _y,Bitu _dx,Bitu _dy,const char * _text) : CButton(_x,_y,_dx,_dy) { text=_text;}
	void Draw(void) {
		if (!enabled) return;
		CButton::Draw();
		DrawText(x+2,y+2,text,color);
	}
protected:
	const char * text;
};

class CEventButton;
static CEventButton * last_clicked = NULL;

class CEventButton : public CTextButton {
public:
	CEventButton(Bitu _x,Bitu _y,Bitu _dx,Bitu _dy,const char * _text,CEvent * _event) 
	: CTextButton(_x,_y,_dx,_dy,_text) 	{ 
		event=_event;	
	}
	void Click(void) {
		if (last_clicked) last_clicked->SetColor(CLR_WHITE);
		this->SetColor(CLR_GREEN);
		SetActiveEvent(event);
		last_clicked=this;
	}
protected:
	CEvent * event;
};

class CCaptionButton : public CButton {
public:
	CCaptionButton(Bitu _x,Bitu _y,Bitu _dx,Bitu _dy) : CButton(_x,_y,_dx,_dy){
		caption[0]=0;
	}
	void Change(const char * format,...) GCC_ATTRIBUTE(__format__(__printf__,2,3));

	void Draw(void) {
		if (!enabled) return;
		DrawText(x+2,y+2,caption,color);
	}
protected:
	char caption[128];
};

void CCaptionButton::Change(const char * format,...) {
	va_list msg;
	va_start(msg,format);
	vsprintf(caption,format,msg);
	va_end(msg);
	mapper.redraw=true;
}		

static void change_action_text(const char* text,Bit8u col);

static void MAPPER_SaveBinds(void);
class CBindButton : public CTextButton {
public:	
	CBindButton(Bitu _x,Bitu _y,Bitu _dx,Bitu _dy,const char * _text,BB_Types _type) 
	: CTextButton(_x,_y,_dx,_dy,_text) 	{ 
		type=_type;
	}
	void Click(void) {
		switch (type) {
		case BB_Add: 
			mapper.addbind=true;
			SetActiveBind(0);
			change_action_text("Press a key/joystick button or move the joystick.",CLR_RED);
			break;
		case BB_Del:
			if (mapper.abindit!=mapper.aevent->bindlist.end())  {
				delete (*mapper.abindit);
				mapper.abindit=mapper.aevent->bindlist.erase(mapper.abindit);
				if (mapper.abindit==mapper.aevent->bindlist.end()) 
					mapper.abindit=mapper.aevent->bindlist.begin();
			}
			if (mapper.abindit!=mapper.aevent->bindlist.end()) SetActiveBind(*(mapper.abindit));
			else SetActiveBind(0);
			break;
		case BB_Next:
			if (mapper.abindit!=mapper.aevent->bindlist.end()) 
				mapper.abindit++;
			if (mapper.abindit==mapper.aevent->bindlist.end()) 
				mapper.abindit=mapper.aevent->bindlist.begin();
			SetActiveBind(*(mapper.abindit));
			break;
		case BB_Save:
			MAPPER_SaveBinds();
			break;
		case BB_Exit:   
			mapper.exit=true;
			break;
		}
	}
protected:
	BB_Types type;
};

class CCheckButton : public CTextButton {
public:	
	CCheckButton(Bitu _x,Bitu _y,Bitu _dx,Bitu _dy,const char * _text,BC_Types _type) 
	: CTextButton(_x,_y,_dx,_dy,_text) 	{ 
		type=_type;
	}
	void Draw(void) {
		if (!enabled) return;
		bool checked=false;
		switch (type) {
		case BC_Mod1:
			checked=(mapper.abind->mods&BMOD_Mod1)>0;
			break;
		case BC_Mod2:
			checked=(mapper.abind->mods&BMOD_Mod2)>0;
			break;
		case BC_Mod3:
			checked=(mapper.abind->mods&BMOD_Mod3)>0;
			break;
		case BC_Hold:
			checked=(mapper.abind->flags&BFLG_Hold)>0;
			break;
		}
		if (checked) {
			Bit8u * point=((Bit8u *)mapper.surface->pixels)+((y+2)*mapper.surface->pitch)+x+dx-dy+2;
			for (Bitu lines=0;lines<(dy-4);lines++)  {
				memset(point,color,dy-4);
				point+=mapper.surface->pitch;
			}
		}
		CTextButton::Draw();
	}
	void Click(void) {
		switch (type) {
		case BC_Mod1:
			mapper.abind->mods^=BMOD_Mod1;
			break;
		case BC_Mod2:
			mapper.abind->mods^=BMOD_Mod2;
			break;
		case BC_Mod3:
			mapper.abind->mods^=BMOD_Mod3;
			break;
		case BC_Hold:
			mapper.abind->flags^=BFLG_Hold;
			break;
		}
		mapper.redraw=true;
	}
protected:
	BC_Types type;
};

class CKeyEvent : public CTriggeredEvent {
public:
	CKeyEvent(char const * const _entry,KBD_KEYS _key) : CTriggeredEvent(_entry) {
		key=_key;
	}
	void Active(bool yesno) {
		KEYBOARD_AddKey(key,yesno);
	};
	KBD_KEYS key;
};

/*
//TODO: Add joystick support back in.
class CJAxisEvent : public CContinuousEvent {
public:
	CJAxisEvent(char const * const _entry,Bitu _stick,Bitu _axis,bool _positive,CJAxisEvent * _opposite_axis) : CContinuousEvent(_entry) {
		stick=_stick;
		axis=_axis;
		positive=_positive;
		opposite_axis=_opposite_axis;
		if (_opposite_axis) {
			_opposite_axis->SetOppositeAxis(this);
		}
	}
	void Active(bool) {
		virtual_joysticks[stick].axis_pos[axis]=(Bit16s)(GetValue()*(positive?1:-1));
	}
	virtual Bitu GetActivityCount(void) {
		return activity|opposite_axis->activity;
	}
	virtual void RepostActivity(void) {
		//caring for joystick movement into the opposite direction
		opposite_axis->Active(true);
	}
protected:
	void SetOppositeAxis(CJAxisEvent * _opposite_axis) {
		opposite_axis=_opposite_axis;
	}
	Bitu stick,axis;
	bool positive;
	CJAxisEvent * opposite_axis;
};

class CJButtonEvent : public CTriggeredEvent {
public:
	CJButtonEvent(char const * const _entry,Bitu _stick,Bitu _button) : CTriggeredEvent(_entry) {
		stick=_stick;
		button=_button;
	}
	void Active(bool pressed) {
		virtual_joysticks[stick].button_pressed[button]=pressed;
	}
protected:
	Bitu stick,button;
};

class CJHatEvent : public CTriggeredEvent {
public:
	CJHatEvent(char const * const _entry,Bitu _stick,Bitu _hat,Bitu _dir) : CTriggeredEvent(_entry) {
		stick=_stick;
		hat=_hat;
		dir=_dir;
	}
	void Active(bool pressed) {
		virtual_joysticks[stick].hat_pressed[(hat<<2)+dir]=pressed;
	}
protected:
	Bitu stick,hat,dir;
};
*/

class CModEvent : public CTriggeredEvent {
public:
	CModEvent(char const * const _entry,Bitu _wmod) : CTriggeredEvent(_entry) {
		wmod=_wmod;
	}
	void Active(bool yesno) {
		if (yesno) mapper.mods|=(1 << (wmod-1));
		else mapper.mods&=~(1 << (wmod-1));
	};
protected:
	Bitu wmod;
};

class CHandlerEvent : public CTriggeredEvent {
public:
	CHandlerEvent(char const * const _entry,MAPPER_Handler * _handler,MapKeys _key,Bitu _mod,char const * const _buttonname) : CTriggeredEvent(_entry) {
		handler=_handler;
		defmod=_mod;
		defkey=_key;
		buttonname=_buttonname;
		handlergroup.push_back(this);
	}
	void Active(bool yesno) {
		(*handler)(yesno);
	};
	const char * ButtonName(void) {
		return buttonname;
	}
	void MakeDefaultBind(char * buf) {
		Bitu key=0;
		switch (defkey) {
		case MK_f1:case MK_f2:case MK_f3:case MK_f4:
		case MK_f5:case MK_f6:case MK_f7:case MK_f8:
		case MK_f9:case MK_f10:case MK_f11:case MK_f12:	
			key=SDL_SCANCODE_F1+(defkey-MK_f1);
			break;
		case MK_return:
			key=SDL_SCANCODE_RETURN;
			break;
		case MK_kpminus:
			key=SDL_SCANCODE_KP_MINUS;
			break;
		case MK_scrolllock:
			key=SDL_SCANCODE_SCROLLLOCK;
			break;
		case MK_pause:
			key=SDL_SCANCODE_PAUSE;
			break;
		case MK_printscreen:
			key=SDL_SCANCODE_PRINTSCREEN;
			break;
		}
		sprintf(buf,"%s \"key %d%s%s%s\"",
			entry,
			key,
			defmod & 1 ? " mod1" : "",
			defmod & 2 ? " mod2" : "",
			defmod & 4 ? " mod3" : ""
		);
	}
protected:
	MapKeys defkey;
	Bitu defmod;
	MAPPER_Handler * handler;
public:
	const char * buttonname;
};


static struct {
	CCaptionButton *  event_title;
	CCaptionButton *  bind_title;
	CCaptionButton *  selected;
	CCaptionButton *  action;
	CBindButton * save;
	CBindButton * exit;   
	CBindButton * add;
	CBindButton * del;
	CBindButton * next;
	CCheckButton * mod1,* mod2,* mod3,* hold;
} bind_but;


static void change_action_text(const char* text,Bit8u col) {
	bind_but.action->Change(text,"");
	bind_but.action->SetColor(col);
}


static void SetActiveBind(CBind * _bind) {
	mapper.abind=_bind;
	if (_bind) {
		bind_but.bind_title->Enable(true);
		char buf[256];_bind->BindName(buf);
		bind_but.bind_title->Change("BIND:%s",buf);
		bind_but.del->Enable(true);
		bind_but.next->Enable(true);
		bind_but.mod1->Enable(true);
		bind_but.mod2->Enable(true);
		bind_but.mod3->Enable(true);
		bind_but.hold->Enable(true);
	} else {
		bind_but.bind_title->Enable(false);
		bind_but.del->Enable(false);
		bind_but.next->Enable(false);
		bind_but.mod1->Enable(false);
		bind_but.mod2->Enable(false);
		bind_but.mod3->Enable(false);
		bind_but.hold->Enable(false);
	}
}

static void SetActiveEvent(CEvent * event) {
	mapper.aevent=event;
	mapper.redraw=true;
	mapper.addbind=false;
	bind_but.event_title->Change("EVENT:%s",event ? event->GetName(): "none");
	if (!event) {
		change_action_text("Select an event to change.",CLR_WHITE);
		bind_but.add->Enable(false);
		SetActiveBind(0);
	} else {
		change_action_text("Select a different event or hit the Add/Del/Next buttons.",CLR_WHITE);
		mapper.abindit=event->bindlist.begin();
		if (mapper.abindit!=event->bindlist.end()) {
			SetActiveBind(*(mapper.abindit));
		} else SetActiveBind(0);
		bind_but.add->Enable(true);
	}
}

static CKeyEvent * AddKeyButtonEvent(Bitu x,Bitu y,Bitu dx,Bitu dy,char const * const title,char const * const entry,KBD_KEYS key) {
	char buf[64];
	strcpy(buf,"key_");
	strcat(buf,entry);
	CKeyEvent * event=new CKeyEvent(buf,key);
	new CEventButton(x,y,dx,dy,title,event);
	return event;
}

/*
static CJAxisEvent * AddJAxisButton(Bitu x,Bitu y,Bitu dx,Bitu dy,char const * const title,Bitu stick,Bitu axis,bool positive,CJAxisEvent * opposite_axis) {
	char buf[64];
	sprintf(buf,"jaxis_%d_%d%s",stick,axis,positive ? "+" : "-");
	CJAxisEvent	* event=new CJAxisEvent(buf,stick,axis,positive,opposite_axis);
	new CEventButton(x,y,dx,dy,title,event);
	return event;
}
static CJAxisEvent * AddJAxisButton_hidden(Bitu stick,Bitu axis,bool positive,CJAxisEvent * opposite_axis) {
	char buf[64];
	sprintf(buf,"jaxis_%d_%d%s",stick,axis,positive ? "+" : "-");
	return new CJAxisEvent(buf,stick,axis,positive,opposite_axis);
}

static void AddJButtonButton(Bitu x,Bitu y,Bitu dx,Bitu dy,char const * const title,Bitu stick,Bitu button) {
	char buf[64];
	sprintf(buf,"jbutton_%d_%d",stick,button);
	CJButtonEvent * event=new CJButtonEvent(buf,stick,button);
	new CEventButton(x,y,dx,dy,title,event);
}
static void AddJButtonButton_hidden(Bitu stick,Bitu button) {
	char buf[64];
	sprintf(buf,"jbutton_%d_%d",stick,button);
	new CJButtonEvent(buf,stick,button);
}

static void AddJHatButton(Bitu x,Bitu y,Bitu dx,Bitu dy,char const * const title,Bitu _stick,Bitu _hat,Bitu _dir) {
	char buf[64];
	sprintf(buf,"jhat_%d_%d_%d",_stick,_hat,_dir);
	CJHatEvent * event=new CJHatEvent(buf,_stick,_hat,_dir);
	new CEventButton(x,y,dx,dy,title,event);
}
*/

static void AddModButton(Bitu x,Bitu y,Bitu dx,Bitu dy,char const * const title,Bitu _mod) {
	char buf[64];
	sprintf(buf,"mod_%d",_mod);
	CModEvent * event=new CModEvent(buf,_mod);
	new CEventButton(x,y,dx,dy,title,event);
}

struct KeyBlock {
	const char * title;
	const char * entry;
	KBD_KEYS key;
};
static KeyBlock combo_f[12]={
	{"F1","f1",KBD_f1},		{"F2","f2",KBD_f2},		{"F3","f3",KBD_f3},		
	{"F4","f4",KBD_f4},		{"F5","f5",KBD_f5},		{"F6","f6",KBD_f6},
	{"F7","f7",KBD_f7},		{"F8","f8",KBD_f8},		{"F9","f9",KBD_f9},
	{"F10","f10",KBD_f10},	{"F11","f11",KBD_f11},	{"F12","f12",KBD_f12},
};

static KeyBlock combo_1[14]={
	{"`~","grave",KBD_grave},	{"1!","1",KBD_1},	{"2@","2",KBD_2},
	{"3#","3",KBD_3},			{"4$","4",KBD_4},	{"5%","5",KBD_5},
	{"6^","6",KBD_6},			{"7&","7",KBD_7},	{"8*","8",KBD_8},
	{"9(","9",KBD_9},			{"0)","0",KBD_0},	{"-_","minus",KBD_minus},	
	{"=+","equals",KBD_equals},	{"\x1B","bspace",KBD_backspace},
};

static KeyBlock combo_2[12]={
	{"q","q",KBD_q},			{"w","w",KBD_w},	{"e","e",KBD_e},
	{"r","r",KBD_r},			{"t","t",KBD_t},	{"y","y",KBD_y},
	{"u","u",KBD_u},			{"i","i",KBD_i},	{"o","o",KBD_o},	
	{"p","p",KBD_p},			{"[","lbracket",KBD_leftbracket},	
	{"]","rbracket",KBD_rightbracket},	
};

static KeyBlock combo_3[12]={
	{"a","a",KBD_a},			{"s","s",KBD_s},	{"d","d",KBD_d},
	{"f","f",KBD_f},			{"g","g",KBD_g},	{"h","h",KBD_h},
	{"j","j",KBD_j},			{"k","k",KBD_k},	{"l","l",KBD_l},
	{";","semicolon",KBD_semicolon},				{"'","quote",KBD_quote},
	{"\\","backslash",KBD_backslash},	
};

static KeyBlock combo_4[11]={
	{"<","lessthan",KBD_extra_lt_gt},
	{"z","z",KBD_z},			{"x","x",KBD_x},	{"c","c",KBD_c},
	{"v","v",KBD_v},			{"b","b",KBD_b},	{"n","n",KBD_n},
	{"m","m",KBD_m},			{",","comma",KBD_comma},
	{".","period",KBD_period},						{"/","slash",KBD_slash},		
};

static CKeyEvent * caps_lock_event=NULL;
static CKeyEvent * num_lock_event=NULL;

static SDL_Color map_pal[5]={
	{0x00,0x00,0x00,0x00},			//0=black
	{0xff,0xff,0xff,0x00},			//1=white
	{0xff,0x00,0x00,0x00},			//2=red
	{0x10,0x30,0xff,0x00},			//3=blue
	{0x00,0xff,0x20,0x00}			//4=green
};

static void CreateStringBind(char * line) {
	line=trim(line);
	char * eventname=StripWord(line);
	CEvent * event;
	for (CEventVector_it ev_it=events.begin();ev_it!=events.end();ev_it++) {
		if (!strcasecmp((*ev_it)->GetName(),eventname)) {
			event=*ev_it;
			goto foundevent;
		}
	}
	LOG_MSG("Can't find matching event for %s",eventname);
	return ;
foundevent:
	CBind * bind;
	for (char * bindline=StripWord(line);*bindline;bindline=StripWord(line)) {
		for (CBindGroup_it it=bindgroups.begin();it!=bindgroups.end();it++) {
			bind=(*it)->CreateConfigBind(bindline);
			if (bind) {
				event->AddBind(bind);
				bind->SetFlags(bindline);
				break;
			}
		}
	}
}

static struct {
	const char * eventend;
	Bitu key;
} DefaultKeys[]={
	{"f1",SDL_SCANCODE_F1},		{"f2",SDL_SCANCODE_F2},		{"f3",SDL_SCANCODE_F3},		{"f4",SDL_SCANCODE_F4},
	{"f5",SDL_SCANCODE_F5},		{"f6",SDL_SCANCODE_F6},		{"f7",SDL_SCANCODE_F7},		{"f8",SDL_SCANCODE_F8},
	{"f9",SDL_SCANCODE_F9},		{"f10",SDL_SCANCODE_F10},	{"f11",SDL_SCANCODE_F11},	{"f12",SDL_SCANCODE_F12},

	{"1",SDL_SCANCODE_1},		{"2",SDL_SCANCODE_2},		{"3",SDL_SCANCODE_3},		{"4",SDL_SCANCODE_4},
	{"5",SDL_SCANCODE_5},		{"6",SDL_SCANCODE_6},		{"7",SDL_SCANCODE_7},		{"8",SDL_SCANCODE_8},
	{"9",SDL_SCANCODE_9},		{"0",SDL_SCANCODE_0},

	{"a",SDL_SCANCODE_A},		{"b",SDL_SCANCODE_B},		{"c",SDL_SCANCODE_C},		{"d",SDL_SCANCODE_D},
	{"e",SDL_SCANCODE_E},		{"f",SDL_SCANCODE_F},		{"g",SDL_SCANCODE_G},		{"h",SDL_SCANCODE_H},
	{"i",SDL_SCANCODE_I},		{"j",SDL_SCANCODE_J},		{"k",SDL_SCANCODE_K},		{"l",SDL_SCANCODE_L},
	{"m",SDL_SCANCODE_M},		{"n",SDL_SCANCODE_N},		{"o",SDL_SCANCODE_O},		{"p",SDL_SCANCODE_P},
	{"q",SDL_SCANCODE_Q},		{"r",SDL_SCANCODE_R},		{"s",SDL_SCANCODE_S},		{"t",SDL_SCANCODE_T},
	{"u",SDL_SCANCODE_U},		{"v",SDL_SCANCODE_V},		{"w",SDL_SCANCODE_W},		{"x",SDL_SCANCODE_X},
	{"y",SDL_SCANCODE_Y},		{"z",SDL_SCANCODE_Z},		{"space",SDL_SCANCODE_SPACE},
	{"esc",SDL_SCANCODE_ESCAPE},	{"equals",SDL_SCANCODE_EQUALS},		{"grave",SDL_SCANCODE_GRAVE},
	{"tab",SDL_SCANCODE_TAB},		{"enter",SDL_SCANCODE_RETURN},		{"bspace",SDL_SCANCODE_BACKSPACE},
	{"lbracket",SDL_SCANCODE_LEFTBRACKET},						{"rbracket",SDL_SCANCODE_RIGHTBRACKET},
	{"minus",SDL_SCANCODE_MINUS},	{"capslock",SDL_SCANCODE_CAPSLOCK},	{"semicolon",SDL_SCANCODE_SEMICOLON},
	{"quote", SDL_SCANCODE_APOSTROPHE},	{"backslash",SDL_SCANCODE_BACKSLASH},	{"lshift",SDL_SCANCODE_LSHIFT},
	{"rshift",SDL_SCANCODE_RSHIFT},	{"lalt",SDL_SCANCODE_LALT},			{"ralt",SDL_SCANCODE_RALT},
	{"lctrl",SDL_SCANCODE_LCTRL},	{"rctrl",SDL_SCANCODE_RCTRL},		{"comma",SDL_SCANCODE_COMMA},
	{"period",SDL_SCANCODE_PERIOD},	{"slash",SDL_SCANCODE_SLASH},		{"printscreen",SDL_SCANCODE_PRINTSCREEN},
	{"scrolllock",SDL_SCANCODE_SCROLLLOCK},	{"pause",SDL_SCANCODE_PAUSE},		{"pagedown",SDL_SCANCODE_PAGEDOWN},
	{"pageup",SDL_SCANCODE_PAGEUP},	{"insert",SDL_SCANCODE_INSERT},		{"home",SDL_SCANCODE_HOME},
	{"delete",SDL_SCANCODE_DELETE},	{"end",SDL_SCANCODE_END},			{"up",SDL_SCANCODE_UP},
	{"left",SDL_SCANCODE_LEFT},		{"down",SDL_SCANCODE_DOWN},			{"right",SDL_SCANCODE_RIGHT},
	{"kp_0",SDL_SCANCODE_KP_0},	{"kp_1",SDL_SCANCODE_KP_1},	{"kp_2",SDL_SCANCODE_KP_2},	{"kp_3",SDL_SCANCODE_KP_3},
	{"kp_4",SDL_SCANCODE_KP_4},	{"kp_5",SDL_SCANCODE_KP_5},	{"kp_6",SDL_SCANCODE_KP_6},	{"kp_7",SDL_SCANCODE_KP_7},
	{"kp_8",SDL_SCANCODE_KP_8},	{"kp_9",SDL_SCANCODE_KP_9},	{"numlock",SDL_SCANCODE_NUMLOCKCLEAR},
	{"kp_divide",SDL_SCANCODE_KP_DIVIDE},	{"kp_multiply",SDL_SCANCODE_KP_MULTIPLY},
	{"kp_minus",SDL_SCANCODE_KP_MINUS},		{"kp_plus",SDL_SCANCODE_KP_PLUS},
	{"kp_period",SDL_SCANCODE_KP_PERIOD},	{"kp_enter",SDL_SCANCODE_KP_ENTER},

#if defined (MACOSX)
	/* Intl Mac keyboards in US layout actually put U+00A7 SECTION SIGN here */
	{"lessthan",SDL_SCANCODE_WORLD_0},
#else
	{"lessthan",SDL_SCANCODE_COMMA},
#endif

	{0,0}
};

static void CreateDefaultBinds(void) {
	char buffer[512];
	Bitu i=0;
	while (DefaultKeys[i].eventend) {
		sprintf(buffer,"key_%s \"key %d\"",DefaultKeys[i].eventend,DefaultKeys[i].key);
		CreateStringBind(buffer);
		i++;
	}
	sprintf(buffer,"mod_1 \"key %d\"",SDL_SCANCODE_RCTRL);CreateStringBind(buffer);
	sprintf(buffer,"mod_1 \"key %d\"",SDL_SCANCODE_LCTRL);CreateStringBind(buffer);
	sprintf(buffer,"mod_2 \"key %d\"",SDL_SCANCODE_RALT);CreateStringBind(buffer);
	sprintf(buffer,"mod_2 \"key %d\"",SDL_SCANCODE_LALT);CreateStringBind(buffer);
	for (CHandlerEventVector_it hit=handlergroup.begin();hit!=handlergroup.end();hit++) {
		(*hit)->MakeDefaultBind(buffer);
		CreateStringBind(buffer);
	}

	/* joystick1, buttons 1-6 */
	sprintf(buffer,"jbutton_0_0 \"stick_0 button 0\" ");CreateStringBind(buffer);
	sprintf(buffer,"jbutton_0_1 \"stick_0 button 1\" ");CreateStringBind(buffer);
	sprintf(buffer,"jbutton_0_2 \"stick_0 button 2\" ");CreateStringBind(buffer);
	sprintf(buffer,"jbutton_0_3 \"stick_0 button 3\" ");CreateStringBind(buffer);
	sprintf(buffer,"jbutton_0_4 \"stick_0 button 4\" ");CreateStringBind(buffer);
	sprintf(buffer,"jbutton_0_5 \"stick_0 button 5\" ");CreateStringBind(buffer);
	/* joystick2, buttons 1-2 */
	sprintf(buffer,"jbutton_1_0 \"stick_1 button 0\" ");CreateStringBind(buffer);
	sprintf(buffer,"jbutton_1_1 \"stick_1 button 1\" ");CreateStringBind(buffer);

	/* joystick1, axes 1-4 */
	sprintf(buffer,"jaxis_0_0- \"stick_0 axis 0 0\" ");CreateStringBind(buffer);
	sprintf(buffer,"jaxis_0_0+ \"stick_0 axis 0 1\" ");CreateStringBind(buffer);
	sprintf(buffer,"jaxis_0_1- \"stick_0 axis 1 0\" ");CreateStringBind(buffer);
	sprintf(buffer,"jaxis_0_1+ \"stick_0 axis 1 1\" ");CreateStringBind(buffer);
	sprintf(buffer,"jaxis_0_2- \"stick_0 axis 2 0\" ");CreateStringBind(buffer);
	sprintf(buffer,"jaxis_0_2+ \"stick_0 axis 2 1\" ");CreateStringBind(buffer);
	sprintf(buffer,"jaxis_0_3- \"stick_0 axis 3 0\" ");CreateStringBind(buffer);
	sprintf(buffer,"jaxis_0_3+ \"stick_0 axis 3 1\" ");CreateStringBind(buffer);
	/* joystick2, axes 1-2 */
	sprintf(buffer,"jaxis_1_0- \"stick_1 axis 0 0\" ");CreateStringBind(buffer);
	sprintf(buffer,"jaxis_1_0+ \"stick_1 axis 0 1\" ");CreateStringBind(buffer);
	sprintf(buffer,"jaxis_1_1- \"stick_1 axis 1 0\" ");CreateStringBind(buffer);
	sprintf(buffer,"jaxis_1_1+ \"stick_1 axis 1 1\" ");CreateStringBind(buffer);

	/* joystick1, hat */
	sprintf(buffer,"jhat_0_0_0 \"stick_0 hat 0 1\" ");CreateStringBind(buffer);
	sprintf(buffer,"jhat_0_0_1 \"stick_0 hat 0 2\" ");CreateStringBind(buffer);
	sprintf(buffer,"jhat_0_0_2 \"stick_0 hat 0 4\" ");CreateStringBind(buffer);
	sprintf(buffer,"jhat_0_0_3 \"stick_0 hat 0 8\" ");CreateStringBind(buffer);
}

void MAPPER_AddHandler(MAPPER_Handler * handler,MapKeys key,Bitu mods,char const * const eventname,char const * const buttonname) {
	//Check if it allready exists=> if so return.
	for(CHandlerEventVector_it it=handlergroup.begin();it!=handlergroup.end();it++)
		if(strcmp((*it)->buttonname,buttonname) == 0) return;

	char tempname[17];
	strcpy(tempname,"hand_");
	strcat(tempname,eventname);
	new CHandlerEvent(tempname,handler,key,mods,buttonname);
	return ;
}

static void MAPPER_SaveBinds(void) {
	FILE * savefile=fopen(mapper.filename.c_str(),"wt+");
	if (!savefile) {
		LOG_MSG("Can't open %s for saving the mappings",mapper.filename.c_str());
		return;
	}
	char buf[128];
	for (CEventVector_it event_it=events.begin();event_it!=events.end();event_it++) {
		CEvent * event=*(event_it);
		fprintf(savefile,"%s ",event->GetName());
		for (CBindList_it bind_it=event->bindlist.begin();bind_it!=event->bindlist.end();bind_it++) {
			CBind * bind=*(bind_it);
			bind->ConfigName(buf);
			bind->AddFlags(buf);
			fprintf(savefile,"\"%s\" ",buf);
		}
		fprintf(savefile,"\n");
	}
	fclose(savefile);
	change_action_text("Mapper file saved.",CLR_WHITE);
}

static bool MAPPER_LoadBinds(void) {
	FILE * loadfile=fopen(mapper.filename.c_str(),"rt");
	if (!loadfile) return false;
	char linein[512];
	while (fgets(linein,512,loadfile)) {
		CreateStringBind(linein);
	}
	fclose(loadfile);
	LOG_MSG("MAPPER: Loading mapper settings from %s", mapper.filename.c_str());
	return true;
}

void MAPPER_CheckEvent(SDL_Event * event) {
	for (CBindGroup_it it=bindgroups.begin();it!=bindgroups.end();it++) {
		if ((*it)->CheckEvent(event)) return;
	}
}

void BIND_MappingEvents(void) {
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
		case SDL_MOUSEBUTTONUP:
			/* Check the press */
			for (CButton_it but_it = buttons.begin();but_it!=buttons.end();but_it++) {
				if ((*but_it)->OnTop(event.button.x,event.button.y)) {
					(*but_it)->Click();
				}
			}	
			break;
		case SDL_QUIT:
			mapper.exit=true;
			break;
		default:
			if (mapper.addbind) for (CBindGroup_it it=bindgroups.begin();it!=bindgroups.end();it++) {
				CBind * newbind=(*it)->CreateEventBind(&event);
				if (!newbind) continue;
				mapper.aevent->AddBind(newbind);
				SetActiveEvent(mapper.aevent);
				mapper.addbind=false;
				break;
			}
		}
	}
}

static void InitializeJoysticks(void) {
	mapper.sticks.num=0;
	mapper.sticks.num_groups=0;
	if (joytype != JOY_NONE) {
		mapper.sticks.num=SDL_NumJoysticks();
		if (joytype==JOY_AUTO) {
			// try to figure out what joystick type to select
			// depending on the number of physically attached joysticks
			if (mapper.sticks.num>1) {
				// more than one joystick present; if all are acceptable use 2axis
				// to allow emulation of two joysticks
				bool first_usable=false;
				SDL_Joystick* tmp_stick1=SDL_JoystickOpen(0);
				if (tmp_stick1) {
					if ((SDL_JoystickNumAxes(tmp_stick1)>1) || (SDL_JoystickNumButtons(tmp_stick1)>0)) {
						first_usable=true;
					}
					SDL_JoystickClose(tmp_stick1);
				}
				bool second_usable=false;
				SDL_Joystick* tmp_stick2=SDL_JoystickOpen(1);
				if (tmp_stick2) {
					if ((SDL_JoystickNumAxes(tmp_stick2)>1) || (SDL_JoystickNumButtons(tmp_stick2)>0)) {
						second_usable=true;
					}
					SDL_JoystickClose(tmp_stick2);
				}
				// choose joystick type now that we know which physical joysticks are usable
				if (first_usable) {
					if (second_usable) {
						joytype=JOY_2AXIS;
						LOG_MSG("Two or more joysticks reported, initializing with 2axis");
					} else {
						joytype=JOY_4AXIS;
						LOG_MSG("One joystick reported, initializing with 4axis");
					}
				} else if (second_usable) {
					joytype=JOY_4AXIS_2;
					LOG_MSG("One joystick reported, initializing with 4axis_2");
				}
			} else if (mapper.sticks.num) {
				// one joystick present; if it is acceptable use 4axis
				joytype=JOY_NONE;
				SDL_Joystick* tmp_stick1=SDL_JoystickOpen(0);
				if (tmp_stick1) {
					if ((SDL_JoystickNumAxes(tmp_stick1)>0) || (SDL_JoystickNumButtons(tmp_stick1)>0)) {
						joytype=JOY_4AXIS;
						LOG_MSG("One joystick reported, initializing with 4axis");
					}
				}
			} else {
				joytype=JOY_NONE;
			}
		}
	}
}

static void CreateBindGroups(void) {
	bindgroups.clear();
	new CKeyBindGroup(SDL_NUM_SCANCODES);
    /*
    //TODO: Add joystick support back in.
	if (joytype != JOY_NONE) {
#if defined (REDUCE_JOYSTICK_POLLING)
		// direct access to the SDL joystick, thus removed from the event handling
		if (mapper.sticks.num) SDL_JoystickEventState(SDL_DISABLE);
#else
		// enable joystick event handling
		if (mapper.sticks.num) SDL_JoystickEventState(SDL_ENABLE);
		else return;
#endif
		Bit8u joyno=0;
		switch (joytype) {
		case JOY_NONE:
			break;
		case JOY_4AXIS:
			mapper.sticks.stick[mapper.sticks.num_groups++]=new C4AxisBindGroup(joyno,joyno);
			new CStickBindGroup(joyno+1U,joyno+1U,true);
			break;
		case JOY_4AXIS_2:
			mapper.sticks.stick[mapper.sticks.num_groups++]=new C4AxisBindGroup(joyno+1U,joyno);
			new CStickBindGroup(joyno,joyno+1U,true);
			break;
		case JOY_FCS:
			mapper.sticks.stick[mapper.sticks.num_groups++]=new CFCSBindGroup(joyno,joyno);
			new CStickBindGroup(joyno+1U,joyno+1U,true);
			break;
		case JOY_CH:
			mapper.sticks.stick[mapper.sticks.num_groups++]=new CCHBindGroup(joyno,joyno);
			new CStickBindGroup(joyno+1U,joyno+1U,true);
			break;
		case JOY_2AXIS:
		default:
			mapper.sticks.stick[mapper.sticks.num_groups++]=new CStickBindGroup(joyno,joyno);
			if((joyno+1U) < mapper.sticks.num) {
				mapper.sticks.stick[mapper.sticks.num_groups++]=new CStickBindGroup(joyno+1U,joyno+1U);
			} else {
				new CStickBindGroup(joyno+1U,joyno+1U,true);
			}
			break;
		}
	}
    */
}

#if defined (REDUCE_JOYSTICK_POLLING)
void MAPPER_UpdateJoysticks(void) {
    /*
    //TODO: Add joystick support back in.
	for (Bitu i=0; i<mapper.sticks.num_groups; i++) {
		mapper.sticks.stick[i]->UpdateJoystick();
	}
    */
}
#endif

void MAPPER_LosingFocus(void) {
	for (CEventVector_it evit=events.begin();evit!=events.end();evit++) {
		if(*evit != caps_lock_event && *evit != num_lock_event)
			(*evit)->DeActivateAll();
	}
}

void MAPPER_Run(bool pressed) {
	if (pressed)
		return;
	KEYBOARD_ClrBuffer();	//Clear buffer
	GFX_LosingFocus();		//Release any keys pressed (buffer gets filled again).
	//MAPPER_RunInternal();
}

void MAPPER_Init(void) {
	InitializeJoysticks();
	//CreateLayout();
	CreateBindGroups();
	if (!MAPPER_LoadBinds()) CreateDefaultBinds();
	if (SDL_GetModState()&KMOD_CAPS) {
		for (CBindList_it bit=caps_lock_event->bindlist.begin();bit!=caps_lock_event->bindlist.end();bit++) {
#if SDL_VERSION_ATLEAST(1, 2, 14)
			(*bit)->ActivateBind(32767,true,false);
			(*bit)->DeActivateBind(false);
#else
			(*bit)->ActivateBind(32767,true,true); //Skip the action itself as bios_keyboard.cpp handles the startup state.
#endif
		}
	}
	if (SDL_GetModState()&KMOD_NUM) {
		for (CBindList_it bit=num_lock_event->bindlist.begin();bit!=num_lock_event->bindlist.end();bit++) {
#if SDL_VERSION_ATLEAST(1, 2, 14)
			(*bit)->ActivateBind(32767,true,false);
			(*bit)->DeActivateBind(false);
#else
			(*bit)->ActivateBind(32767,true,true);
#endif
		}
	}
}
//Somehow including them at the top conflicts with something in setup.h
#ifdef C_X11_XKB
#include "SDL_syswm.h"
#include <X11/XKBlib.h>
#endif
void MAPPER_StartUp(Section * sec) {
	Section_prop * section=static_cast<Section_prop *>(sec);
	mapper.sticks.num=0;
	mapper.sticks.num_groups=0;

	//memset(&virtual_joysticks,0,sizeof(virtual_joysticks));

	usescancodes = false;

	if (section->Get_bool("usescancodes")) {
		usescancodes=true;

		/* Note: table has to be tested/updated for various OSs */
#if defined (MACOSX)
		/* nothing */
#elif defined(OS2)
		sdlkey_map[0x61]=SDL_SCANCODE_UP;
		sdlkey_map[0x66]=SDL_SCANCODE_DOWN;
		sdlkey_map[0x63]=SDL_SCANCODE_LEFT;
		sdlkey_map[0x64]=SDL_SCANCODE_RIGHT;
		sdlkey_map[0x60]=SDL_SCANCODE_HOME;
		sdlkey_map[0x65]=SDL_SCANCODE_END;
		sdlkey_map[0x62]=SDL_SCANCODE_PAGEUP;
		sdlkey_map[0x67]=SDL_SCANCODE_PAGEDOWN;
		sdlkey_map[0x68]=SDL_SCANCODE_INSERT;
		sdlkey_map[0x69]=SDL_SCANCODE_DELETE;
		sdlkey_map[0x5C]=SDL_SCANCODE_KP_DIVIDE;
		sdlkey_map[0x5A]=SDL_SCANCODE_KP_ENTER;
		sdlkey_map[0x5B]=SDL_SCANCODE_RCTRL;
		sdlkey_map[0x5F]=SDL_SCANCODE_PAUSE;
//		sdlkey_map[0x00]=SDL_SCANCODE_PRINTSCREEN;
		sdlkey_map[0x5E]=SDL_SCANCODE_RALT;
		sdlkey_map[0x40]=SDL_SCANCODE_KP_5;
		sdlkey_map[0x41]=SDL_SCANCODE_KP_6;
#elif !defined (WIN32) /* => Linux & BSDs */
		bool evdev_input = false;
#ifdef SDL_VIDEO_DRIVER_X11
//SDL needs to be compiled to use it, else the next makes no sense.
#ifdef C_X11_XKB
        //TODO: What is this for?
        /*
		SDL_SysWMinfo info;
		SDL_VERSION(&info.version);
		if (SDL_GetWMInfo(&info)) {
			XkbDescPtr desc = NULL;
			if((desc = XkbGetMap(info.info.x11.display,XkbAllComponentsMask,XkbUseCoreKbd))) {
				if(XkbGetNames(info.info.x11.display,XkbAllNamesMask,desc) == 0) {
					const char* keycodes = XGetAtomName(info.info.x11.display, desc->names->keycodes);
//					const char* geom = XGetAtomName(info.info.x11.display, desc->names->geometry);
					if(keycodes) {
						LOG(LOG_MISC,LOG_NORMAL)("keyboard type %s",keycodes);
						if (strncmp(keycodes,"evdev",5) == 0) evdev_input = true;
					}
					XkbFreeNames(desc,XkbAllNamesMask,True);
				}
			XkbFreeClientMap(desc,0,True);
			}
		}
        */
#endif
#endif
		if (evdev_input) {
			sdlkey_map[0x67]=SDL_SCANCODE_UP;
			sdlkey_map[0x6c]=SDL_SCANCODE_DOWN;
			sdlkey_map[0x69]=SDL_SCANCODE_LEFT;
			sdlkey_map[0x6a]=SDL_SCANCODE_RIGHT;
			sdlkey_map[0x66]=SDL_SCANCODE_HOME;
			sdlkey_map[0x6b]=SDL_SCANCODE_END;
			sdlkey_map[0x68]=SDL_SCANCODE_PAGEUP;
			sdlkey_map[0x6d]=SDL_SCANCODE_PAGEDOWN;
			sdlkey_map[0x6e]=SDL_SCANCODE_INSERT;
			sdlkey_map[0x6f]=SDL_SCANCODE_DELETE;
			sdlkey_map[0x62]=SDL_SCANCODE_KP_DIVIDE;
			sdlkey_map[0x60]=SDL_SCANCODE_KP_ENTER;
			sdlkey_map[0x61]=SDL_SCANCODE_RCTRL;
			sdlkey_map[0x77]=SDL_SCANCODE_PAUSE;
			sdlkey_map[0x63]=SDL_SCANCODE_PRINTSCREEN;
			sdlkey_map[0x64]=SDL_SCANCODE_RALT;
		} else {
			sdlkey_map[0x5a]=SDL_SCANCODE_UP;
			sdlkey_map[0x60]=SDL_SCANCODE_DOWN;
			sdlkey_map[0x5c]=SDL_SCANCODE_LEFT;
			sdlkey_map[0x5e]=SDL_SCANCODE_RIGHT;
			sdlkey_map[0x59]=SDL_SCANCODE_HOME;
			sdlkey_map[0x5f]=SDL_SCANCODE_END;
			sdlkey_map[0x5b]=SDL_SCANCODE_PAGEUP;
			sdlkey_map[0x61]=SDL_SCANCODE_PAGEDOWN;
			sdlkey_map[0x62]=SDL_SCANCODE_INSERT;
			sdlkey_map[0x63]=SDL_SCANCODE_DELETE;
			sdlkey_map[0x68]=SDL_SCANCODE_KP_DIVIDE;
			sdlkey_map[0x64]=SDL_SCANCODE_KP_ENTER;
			sdlkey_map[0x65]=SDL_SCANCODE_RCTRL;
			sdlkey_map[0x66]=SDL_SCANCODE_PAUSE;
			sdlkey_map[0x67]=SDL_SCANCODE_PRINTSCREEN;
			sdlkey_map[0x69]=SDL_SCANCODE_RALT;
		}
#else
		sdlkey_map[0xc8]=SDL_SCANCODE_UP;
		sdlkey_map[0xd0]=SDL_SCANCODE_DOWN;
		sdlkey_map[0xcb]=SDL_SCANCODE_LEFT;
		sdlkey_map[0xcd]=SDL_SCANCODE_RIGHT;
		sdlkey_map[0xc7]=SDL_SCANCODE_HOME;
		sdlkey_map[0xcf]=SDL_SCANCODE_END;
		sdlkey_map[0xc9]=SDL_SCANCODE_PAGEUP;
		sdlkey_map[0xd1]=SDL_SCANCODE_PAGEDOWN;
		sdlkey_map[0xd2]=SDL_SCANCODE_INSERT;
		sdlkey_map[0xd3]=SDL_SCANCODE_DELETE;
		sdlkey_map[0xb5]=SDL_SCANCODE_KP_DIVIDE;
		sdlkey_map[0x9c]=SDL_SCANCODE_KP_ENTER;
		sdlkey_map[0x9d]=SDL_SCANCODE_RCTRL;
		sdlkey_map[0xc5]=SDL_SCANCODE_PAUSE;
		sdlkey_map[0xb7]=SDL_SCANCODE_PRINTSCREEN;
		sdlkey_map[0xb8]=SDL_SCANCODE_RALT;
#endif

		Bitu i;
		for (i=0; i<MAX_SDLKEYS; i++) scancode_map[i]=0;
		for (i=0; i<MAX_SCANCODES; i++) {
			SDL_Scancode key=sdlkey_map[i];
			if (key<MAX_SDLKEYS) scancode_map[key]=(Bit8u)i;
		}
	}

	Prop_path* pp = section->Get_path("mapperfile");
	mapper.filename = pp->realpath;
	MAPPER_AddHandler(&MAPPER_Run,MK_f1,MMOD1,"mapper","Mapper");
}

