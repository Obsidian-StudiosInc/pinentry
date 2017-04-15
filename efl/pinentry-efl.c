/* pinentry-efl.c
   Copyright (C) 1999 Robert Bihlmeyer <robbe@orcus.priv.at>
   Copyright (C) 2001, 2002, 2007, 2015 g10 Code GmbH
   Copyright (C) 2004 by Albrecht Dreß <albrecht.dress@arcor.de>
   Copyright (C) 2016 Mike Blumenkrantz <zmike@osg..samsung.com>

   pinentry-efl is a pinentry application written using EFL.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.  */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <Elementary.h>
#include <Ecore_X.h>
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 7)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#endif
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 7)
#pragma GCC diagnostic pop
#endif

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#else
#include "getopt.h"
#endif /* HAVE_GETOPT_H */

#include "pinentry.h"

#ifdef FALLBACK_CURSES
#include "pinentry-curses.h"
#endif

#define PGMNAME "pinentry-efl"

#ifndef VERSION
#define VERSION
#endif

static int pargc;
static char **pargv;

static pinentry_t pinentry;
static int grab_failed;
static int passphrase_ok;
typedef enum
{ CONFIRM_CANCEL, CONFIRM_OK, CONFIRM_NOTOK } confirm_value_t;
static confirm_value_t confirm_value;

static Evas_Object *entry;
static Evas_Object *repeat_entry;
static Evas_Object *error_label;
static Evas_Object *qualitybar;
static Evas_Object *win;

static Eina_Bool got_input;
static Ecore_Timer *timeout_timer;
static int confirm_mode;

/* utility macros for sizing widgets */
#define WEIGHT evas_object_size_hint_weight_set
#define ALIGN evas_object_size_hint_align_set
#define EXPAND(X) WEIGHT((X), EVAS_HINT_EXPAND, EVAS_HINT_EXPAND)
#define FILL(X) ALIGN((X), EVAS_HINT_FILL, EVAS_HINT_FILL)

/* The text shown in the quality bar when no text is shown.  This is not
 * the empty string, because with an empty string the height of
 * the quality bar is less than with a non-empty string.  This results
 * in ugly layout changes when the text changes from non-empty to empty
 * and vice versa.  */
#define QUALITYBAR_EMPTY_TEXT " "

/* Realize the window as transient if we grab the keyboard.  This
   makes the window a modal dialog to the root window, which helps the
   window manager.  See the following quote from:
   http://standards.freedesktop.org/wm-spec/wm-spec-1.4.html#id2512420

   Implementing enhanced support for application transient windows

   If the WM_TRANSIENT_FOR property is set to None or Root window, the
   window should be treated as a transient for all other windows in
   the same group. It has been noted that this is a slight ICCCM
   violation, but as this behavior is pretty standard for many
   toolkits and window managers, and is extremely unlikely to break
   anything, it seems reasonable to document it as standard.  */

static void
make_transient (void *data EINA_UNUSED, Evas * e EINA_UNUSED,
		Evas_Object * obj, void *event_info EINA_UNUSED)
{
  if (!pinentry->grab)
    return;

  /* Make window transient for the root window.  */
  ecore_x_icccm_transient_for_set (elm_win_window_id_get (obj),
				   ecore_x_window_root_first_get ());
}

/* Grab the keyboard for maximum security */
static void
grab_keyboard (void *data EINA_UNUSED, Evas_Object * obj EINA_UNUSED,
	       void *event_info EINA_UNUSED)
{
  if (!pinentry->grab)
    return;

  if (!ecore_x_keyboard_grab (elm_win_window_id_get (win)))
    {
      EINA_LOG_CRIT ("could not grab keyboard");
      grab_failed = 1;
      ecore_main_loop_quit ();
    }
}

/* Remove grab.  */
static void
ungrab_keyboard (void *data EINA_UNUSED, Evas_Object * obj EINA_UNUSED,
		 void *event_info EINA_UNUSED)
{
  ecore_x_keyboard_ungrab ();
  /* Unmake window transient for the root window.  */
  ecore_x_icccm_transient_for_unset (elm_win_window_id_get (win));
}

static void
delete_event (void *data EINA_UNUSED, Evas * e EINA_UNUSED,
	      Evas_Object * obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
  pinentry->close_button = 1;
}

/* A button was clicked.  DATA indicates which button was clicked
   (i.e., the appropriate action) and is either CONFIRM_CANCEL,
   CONFIRM_OK or CONFIRM_NOTOK.  */
static void
button_clicked (void *data, Evas_Object * obj EINA_UNUSED,
		void *event_info EINA_UNUSED)
{
  if (confirm_mode)
    {
      confirm_value = (confirm_value_t) data;
      ecore_main_loop_quit ();

      return;
    }

  if (data)
    {
      const char *s, *s2;

      /* Okay button or enter used in text field.  */
      s = elm_entry_entry_get (entry);
      if (!s)
	s = "";

      if (pinentry->repeat_passphrase && repeat_entry)
	{
	  s2 = elm_entry_entry_get (repeat_entry);
	  if (!s2)
	    s2 = "";
	  if (strcmp (s, s2))
	    {
	      elm_object_text_set (error_label,
				   pinentry->repeat_error_string ?
				   pinentry->repeat_error_string :
				   "not correctly repeated");
	      elm_object_focus_set (entry, 1);
	      return;		/* again */
	    }
	  pinentry->repeat_okay = 1;
	}

      passphrase_ok = 1;
      pinentry_setbufferlen (pinentry, strlen (s) + 1);
      if (pinentry->pin)
	strcpy (pinentry->pin, s);
    }
  ecore_main_loop_quit ();
}

static void
enter_callback (void *data, Evas_Object * obj, void *event_info EINA_UNUSED)
{
  if (data)
    elm_object_focus_set (data, 1);
  else
    button_clicked ((void *) CONFIRM_OK, obj, NULL);
}

static void
cancel_callback (void *data EINA_UNUSED, Evas * e EINA_UNUSED,
		 Evas_Object * obj EINA_UNUSED, void *event_info)
{
  Evas_Event_Key_Down *ev = event_info;
  if (eina_streq (ev->key, "Escape"))
    button_clicked ((void *) CONFIRM_CANCEL, NULL, NULL);
}

/* Handler called for "changed".   We use it to update the quality
   indicator.  */
static void
changed_text_handler (void *data EINA_UNUSED, Evas_Object * widget,
		      void *event_info EINA_UNUSED)
{
  const char *s;
  int length;
  int percent;

  got_input = EINA_TRUE;

  if (pinentry->repeat_passphrase && repeat_entry)
    {
      elm_entry_entry_set (repeat_entry, NULL);
      elm_object_text_set (error_label, NULL);
    }

  if (!qualitybar || !pinentry->quality_bar)
    return;

  s = elm_entry_entry_get (widget);
  if (!s)
    s = "";
  length = strlen (s);
  percent = length ? pinentry_inq_quality (pinentry, s, length) : 0;

  elm_progressbar_value_set (qualitybar, (double) percent / 100.0);
}

#ifdef HAVE_LIBSECRET
static void
may_save_passphrase_toggled (void *data, Evas_Object * obj,
			     void *event_info EINA_UNUSED)
{
  pinentry_t ctx = (pinentry_t) data;

  ctx->may_cache_password = elm_check_state_get (obj);
}

#endif

static Eina_Bool
timeout_cb (void *data EINA_UNUSED)
{
  if (!got_input)
    ecore_main_loop_quit ();

  /* Don't run again.  */
  timeout_timer = NULL;
  return EINA_FALSE;
}

static Evas_Object *
create_window (pinentry_t ctx EINA_UNUSED)
{
  Evas_Object *w;
  Evas_Object *box;
  Evas_Object *wvbox, *chbox, *bbox;
  Eina_Bool noimage = EINA_FALSE;
  char *msg;

  repeat_entry = NULL;

  /* FIXME: check the grabbing code against the one we used with the
     old gpg-agent */
  win = elm_win_util_standard_add (PGMNAME, PGMNAME);
  elm_win_autodel_set (win, 1);
  evas_object_event_callback_add (win, EVAS_CALLBACK_DEL, delete_event, NULL);

  if (!confirm_mode)
    {
      if (pinentry->grab)
	evas_object_event_callback_add (win, EVAS_CALLBACK_SHOW,
					make_transient, NULL);

      /* We need to grab the keyboard when its visible! not when its
         mapped (there is a difference)  */
      if (pinentry->grab)
	{
	  /* this is a hack to ensure we get visibility callbacks on the window
	   * the toolkit doesn't hook them, so we can use them freely
	   */
	  Ecore_Evas *ee =
	    ecore_evas_ecore_evas_get (evas_object_evas_get (win));

	  ecore_evas_callback_show_set (ee, (void *) grab_keyboard);
	  ecore_evas_callback_hide_set (ee, (void *) ungrab_keyboard);

	  /* technically more correct, but not functional until 1.19 (or later)
	     evas_object_smart_callback_add(win, "normal", grab_keyboard, NULL);
	     evas_object_smart_callback_add(win, "withdrawn", ungrab_keyboard, NULL);
	     evas_object_smart_callback_add(win, "iconified", ungrab_keyboard, NULL);
	   */
	}
      else
	{
	  evas_object_smart_callback_add (win, "focused", grab_keyboard,
					  NULL);
	  evas_object_smart_callback_add (win, "unfocused", ungrab_keyboard,
					  NULL);
	}
    }

  wvbox = elm_box_add (win);
  elm_box_padding_set(wvbox, 0, ELM_SCALE_SIZE(6));
  elm_win_resize_object_add (win, wvbox);
  evas_object_show (wvbox);

  elm_box_pack_end(wvbox, elm_box_add(win));

  chbox = elm_box_add (win);
  elm_box_horizontal_set (chbox, 1);
  evas_object_show (chbox);
  elm_box_pack_end (wvbox, chbox);

  w = elm_icon_add (chbox);
  evas_object_size_hint_aspect_set (w, EVAS_ASPECT_CONTROL_BOTH, 1, 1);
  evas_object_show (w);
  /* FIXME: need some sort of key icon... */
  if (elm_icon_standard_set (w, "system-lock-screen"))
    {
       ALIGN (w, 0, EVAS_HINT_FILL);
       EXPAND (w);
       elm_box_pack_end (chbox, w);
    }
  else
    {
       evas_object_del(w);
       noimage = EINA_TRUE;
    }

  box = elm_box_add (chbox);
  evas_object_show (box);
  elm_box_pack_end (chbox, box);
  EXPAND (box);
  FILL (box);

  if (pinentry->title)
    {
      msg = pinentry_utf8_to_local (pinentry->lc_ctype, pinentry->title);
      elm_win_title_set (win, msg);
      free (msg);
    }
  if (pinentry->description)
    {
      char *esc;

      msg =
	pinentry_utf8_to_local (pinentry->lc_ctype, pinentry->description);
      esc = elm_entry_utf8_to_markup (msg);
      w = elm_label_add (win);
      evas_object_show (w);
      elm_object_text_set (w, esc);
      free (esc);
      free (msg);
      elm_object_style_set (w, "default/left");
      elm_label_line_wrap_set (w, ELM_WRAP_WORD);
      EXPAND (w);
      FILL (w);
      elm_box_pack_end (box, w);
    }
  if (!confirm_mode && (pinentry->error || pinentry->repeat_passphrase))
    {
      /* With the repeat passphrase option we need to create the label
         in any case so that it may later be updated by the error
         message.  */
      if (pinentry->error)
	msg = pinentry_utf8_to_local (pinentry->lc_ctype, pinentry->error);
      else
	msg = "";
      error_label = elm_label_add (box);
      evas_object_color_set(error_label, 255, 0, 0, 255);
      evas_object_show (error_label);
      elm_object_text_set (error_label, msg);
      if (pinentry->error)
	free (msg);
      elm_object_style_set (error_label, "marker/left");
      elm_label_line_wrap_set (error_label, ELM_WRAP_WORD);
      EXPAND (error_label);
      FILL (error_label);
      elm_box_pack_end (box, error_label);
    }

  qualitybar = NULL;

  if (!confirm_mode)
    {
      int nrow;
      Evas_Object *table;

      nrow = 1;
      if (pinentry->quality_bar)
	nrow++;
      if (pinentry->repeat_passphrase)
	nrow++;

      table = elm_table_add (box);
      elm_table_padding_set(table, ELM_SCALE_SIZE(5), ELM_SCALE_SIZE(5));
      evas_object_show (table);
      nrow = 0;
      elm_box_pack_end (box, table);

      if (pinentry->prompt)
	{
	  msg = pinentry_utf8_to_local (pinentry->lc_ctype, pinentry->prompt);
	  w = elm_label_add (table);
	  evas_object_show (w);
	  elm_object_text_set (w, msg);
	  free (msg);
	  elm_object_style_set (w, "default/right");
	  FILL (w);
	  elm_table_pack (table, w, 0, nrow, 1, 1);
	}

      entry = elm_entry_add (win);
      elm_entry_scrollable_set (entry, 1);
      elm_entry_password_set (entry, 1);
      elm_entry_single_line_set (entry, 1);
      /* FIXME: not supported yet
       * if (pinentry->invisible_char)
       */
      evas_object_smart_callback_add (entry, "changed,user",
				      changed_text_handler, NULL);

      EXPAND (entry);
      FILL (entry);
      elm_table_pack (table, entry, 1, nrow, 1, 1);
      evas_object_show (entry);
      elm_object_focus_set (entry, 1);
      elm_table_pack(table, elm_box_add(win), 2, nrow, 1, 1);
      nrow++;

      {
         Evas_Object *rect = evas_object_rectangle_add(evas_object_evas_get(win));
	 evas_object_size_hint_min_set(rect, ELM_SCALE_SIZE(200), 1);
	 elm_table_pack (table, rect, 1, nrow, 1, 1);
	 nrow++;
      }

      if (pinentry->quality_bar)
	{
	  msg =
	    pinentry_utf8_to_local (pinentry->lc_ctype,
				    pinentry->quality_bar);
	  w = elm_label_add (table);
	  evas_object_show (w);
	  elm_object_text_set (w, msg);
	  free (msg);
	  FILL (w);
	  elm_object_style_set (w, "default/right");
	  elm_table_pack (table, w, 0, nrow, 1, 1);

	  qualitybar = elm_progressbar_add (table);
	  evas_object_show (qualitybar);
	  elm_object_text_set (qualitybar, QUALITYBAR_EMPTY_TEXT);
	  elm_progressbar_unit_format_set (qualitybar, "%");
	  if (pinentry->quality_bar_tt)
	    elm_object_tooltip_text_set (qualitybar,
					 pinentry->quality_bar_tt);
	  EXPAND (qualitybar);
	  FILL (qualitybar);
	  elm_table_pack (table, qualitybar, 1 + noimage, nrow, 1, 1);
	  nrow++;
	}

      if (pinentry->repeat_passphrase)
	{
	  msg =
	    pinentry_utf8_to_local (pinentry->lc_ctype,
				    pinentry->repeat_passphrase);
	  w = elm_label_add (table);
	  evas_object_show (w);
	  elm_object_text_set (w, msg);
	  free (msg);
	  FILL (w);
	  elm_object_style_set (w, "default/right");
	  elm_table_pack (table, w, 0, nrow, 1, 1);

	  repeat_entry = elm_entry_add (win);
	  elm_entry_scrollable_set (repeat_entry, 1);
	  elm_entry_password_set (repeat_entry, 1);
	  elm_entry_single_line_set (repeat_entry, 1);
	  evas_object_smart_callback_add (repeat_entry, "activate",
					  enter_callback, NULL);

	  EXPAND (repeat_entry);
	  FILL (repeat_entry);
	  elm_table_pack (table, repeat_entry, 1, nrow, 1, 1);
	  evas_object_show (repeat_entry);
	  nrow++;
	}

      /* When the user presses enter in the entry widget, the widget
         is activated.  If we have a repeat entry, send the focus to
         it.  Otherwise, activate the "Ok" button.  */
      evas_object_smart_callback_add (entry, "activate", enter_callback,
				      repeat_entry);
    }

  bbox = elm_box_add (win);
  elm_box_horizontal_set (bbox, 1);
  evas_object_show (bbox);
  elm_box_padding_set (bbox, 6, 0);
  EXPAND (bbox);
  elm_box_pack_end (wvbox, bbox);

#ifdef HAVE_LIBSECRET
  if (ctx->allow_external_password_cache && ctx->keyinfo)
    {
      /* Only show this if we can cache passwords and we have a stable
         key identifier..  */
      w = elm_check_add (bbox);
      if (pinentry->default_pwmngr)
	{
	  msg =
	    pinentry_utf8_to_local (pinentry->lc_ctype,
				    pinentry->default_pwmngr);
	  elm_object_text_set (w, msg);
	  free (msg);
	}
      else
	elm_object_text_set (w, "Save passphrase using libsecret");

      EXPAND (w);
      elm_box_pack_end (box, w);
      evas_object_show (w);

      evas_object_smart_callback_add (w, "changed",
				      may_save_passphrase_toggled, ctx);
    }
#endif

  if (!pinentry->one_button)
    {
      w = elm_button_add (bbox);
      evas_object_show (w);
      if (pinentry->cancel)
	{
	  msg = pinentry_utf8_to_local (pinentry->lc_ctype, pinentry->cancel);
	  elm_object_text_set (w, msg);
	  free (msg);
	}
      else
	{
	  Evas_Object *image;

	  if (pinentry->default_cancel)
	    {
	      msg =
		pinentry_utf8_to_local (pinentry->lc_ctype,
					pinentry->default_cancel);
	      elm_object_text_set (w, msg);
	      free (msg);
	    }
	  else
	    elm_object_text_set (w, "Cancel");
	  image = elm_image_add (bbox);
	  if (elm_icon_standard_set (image, "dialog-cancel"))
	    elm_object_content_set (w, image);
	  else
	    evas_object_del(image);
	}
      elm_box_pack_end (bbox, w);
      evas_object_smart_callback_add (w, "clicked", button_clicked,
				      (void *) CONFIRM_CANCEL);
      if (evas_object_key_grab (w, "Escape", 0, 0, 1))
	{
	};
      evas_object_event_callback_add (w, EVAS_CALLBACK_KEY_DOWN,
				      cancel_callback, NULL);
    }

  if (confirm_mode && !pinentry->one_button && pinentry->notok)
    {
      msg = pinentry_utf8_to_local (pinentry->lc_ctype, pinentry->notok);
      w = elm_button_add (bbox);
      evas_object_show (w);
      elm_object_text_set (w, msg);
      free (msg);

      elm_box_pack_end (bbox, w);
      evas_object_smart_callback_add (w, "clicked", button_clicked,
				      (void *) CONFIRM_NOTOK);
    }

  w = elm_button_add (bbox);
  evas_object_show (w);
  if (pinentry->ok)
    {
      msg = pinentry_utf8_to_local (pinentry->lc_ctype, pinentry->ok);
      elm_object_text_set (w, msg);
      free (msg);
    }
  else
    {
      Evas_Object *image;

      if (pinentry->default_ok)
	{
	  msg =
	    pinentry_utf8_to_local (pinentry->lc_ctype, pinentry->default_ok);
	  elm_object_text_set (w, msg);
	  free (msg);
	}
      else
	elm_object_text_set (w, "Ok");
      image = elm_image_add (bbox);
      if (elm_icon_standard_set (image, "dialog-ok"))
        elm_object_content_set (w, image);
      else
        evas_object_del(image);
    }
  elm_box_pack_end (bbox, w);
  if (!confirm_mode)
    {
      if (evas_object_key_grab (w, "Return", 0, 0, 1))
	{
	};
      if (evas_object_key_grab (w, "Kp_Enter", 0, 0, 1))
	{
	};
    }

  evas_object_smart_callback_add (w, "clicked", button_clicked,
				  (void *) CONFIRM_OK);

  elm_box_pack_end(wvbox, elm_box_add(win));

  elm_win_center (win, 1, 1);
  elm_win_raise (win);
  evas_object_show (win);

  if (pinentry->timeout > 0)
    timeout_timer = ecore_timer_add (pinentry->timeout, timeout_cb, NULL);

  return win;
}

static int
efl_cmd_handler (pinentry_t pe)
{
  Evas_Object *w;
  int want_pass = ! !pe->pin;

  got_input = EINA_FALSE;
  pinentry = pe;
  confirm_value = CONFIRM_CANCEL;
  passphrase_ok = 0;
  confirm_mode = want_pass ? 0 : 1;
  /* init ecore-x explicitly using DISPLAY since this can launch
   * from console
   */
  if (pe->display)
    ecore_x_init (pe->display);
  elm_init (pargc, pargv);
  w = create_window (pe);
  ecore_main_loop_begin ();
  evas_object_del (w);

  if (timeout_timer)
    {
      /* There is a timer running.  Cancel it.  */
      ecore_timer_del (timeout_timer);
      timeout_timer = NULL;
    }

  if (confirm_value == CONFIRM_CANCEL || grab_failed)
    pe->canceled = 1;

  pinentry = NULL;
  if (want_pass)
    {
      if (passphrase_ok && pe->pin)
	return strlen (pe->pin);
      else
	return -1;
    }
  else
    return (confirm_value == CONFIRM_OK) ? 1 : 0;
}

pinentry_cmd_handler_t pinentry_cmd_handler = efl_cmd_handler;

int
main (int argc, char *argv[])
{
  pinentry_init (PGMNAME);

#ifdef FALLBACK_CURSES
  if (!pinentry_have_display (argc, argv))
    pinentry_cmd_handler = curses_cmd_handler;
#endif

  pinentry_parse_opts (argc, argv);
  pargc = argc;
  pargv = argv;

  if (pinentry_loop ())
    return 1;

  return 0;
}
